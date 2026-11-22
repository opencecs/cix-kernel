/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Y4 FAN1 extension of pwm-fan; no userspace owner or extra hardware service. */
#include "pwm-fan-y4-policy.h"

struct y4_fan1 {
	struct pwm_fan_ctx *ctx;
	struct y4_fan1_policy policy;
	struct delayed_work temperature_work;
	struct delayed_work guard_work;
	const char *zone_names[4];
	unsigned long last_tach;
	u64 last_down_ms;
	unsigned int applied_percent;
	unsigned int number;
	bool tach_available;
	bool stopping;
	bool control_fault;
	int temperature_error;
};

/* CONFIG_SENSORS_PWM_FAN=y is a required production input. These bits survive
 * consumer rebind and every userspace restart, and reset only at kernel boot.
 */
static unsigned long y4_fan1_boot_logs[2];
enum y4_fan1_log {
	Y4_FAN1_LOG_MISSING,
	Y4_FAN1_LOG_TEMPERATURE,
	Y4_FAN1_LOG_TEMP_STALE,
	Y4_FAN1_LOG_TACH_UNAVAILABLE,
	Y4_FAN1_LOG_TACH_STALE,
	Y4_FAN1_LOG_CONTROL,
	Y4_FAN1_LOG_TACH_RUNNING,
};

static u64 y4_fan1_now(void)
{
	return div_u64(ktime_get_boottime_ns(), NSEC_PER_MSEC);
}

/* ctx->lock serializes both the PWM core state and hardware readback. The
 * 6.1 PWM core has only a cached public getter; this board-specific consumer
 * uses its checked Y4 provider's get_state op for actual register feedback.
 */
static int y4_fan1_drive(struct pwm_fan_ctx *ctx, unsigned int percent, bool force)
{
	struct pwm_state state = {
		.period = Y4_FAN1_PERIOD_NS,
		.duty_cycle = y4_fan1_duty_ns(percent),
		.polarity = PWM_POLARITY_NORMAL,
		.enabled = true,
	};
	struct pwm_state actual = {};
	int ret;

	if (force) {
		/* Y4 provider maps DISABLED to HIGH, so both writes are full speed.
		 * This also invalidates a matching cached state after hardware drift.
		 */
		state.enabled = false;
		ret = pwm_apply_might_sleep(ctx->pwm, &state);
		if (ret)
			return ret;
		state.enabled = true;
	}
	ret = pwm_apply_might_sleep(ctx->pwm, &state);
	if (ret)
		return ret;
	ret = ctx->pwm->chip->ops->get_state(ctx->pwm->chip, ctx->pwm, &actual);
	if (ret)
		return ret;
	if (!actual.enabled || actual.period != state.period ||
	    actual.duty_cycle != state.duty_cycle || actual.polarity != state.polarity)
		return -EIO;
	ctx->pwm_state = state;
	ctx->pwm_value = DIV_ROUND_CLOSEST(percent * MAX_PWM, 100);
	ctx->enabled = true;
	return 0;
}

static void y4_fan1_control(struct pwm_fan_ctx *ctx, unsigned int percent)
{
	int ret, safe_ret;

	ret = y4_fan1_drive(ctx, ctx->y4->control_fault ? 100 : percent, false);
	if (!ret) {
		ctx->y4->applied_percent = ctx->y4->control_fault ? 100 : percent;
		return;
	}
	ctx->y4->control_fault = true;
	safe_ret = y4_fan1_drive(ctx, 100, true);
	if (!safe_ret)
		ctx->y4->applied_percent = 100;
	if (!test_and_set_bit(Y4_FAN1_LOG_CONTROL, &y4_fan1_boot_logs[ctx->y4->number - 1]))
		printk_noconsole(KERN_ERR "SoC cooling fan FAN%u: PWM control failure (%d); full-speed fallback status %d.\n",
				 ctx->y4->number, ret, safe_ret);
}

static void y4_fan1_temperature_work(struct work_struct *work)
{
	struct y4_fan1 *fan = container_of(to_delayed_work(work),
					 struct y4_fan1, temperature_work);
	struct pwm_fan_ctx *ctx = fan->ctx;
	u64 started = y4_fan1_now();
	int hottest = -40000, temp, ret = 0, i;

	if (READ_ONCE(fan->stopping))
		return;
	for (i = 0; i < ARRAY_SIZE(fan->zone_names); i++) {
		struct thermal_zone_device *zone;

		zone = thermal_zone_get_zone_by_name(fan->zone_names[i]);
		if (IS_ERR(zone)) {
			ret = PTR_ERR(zone);
			break;
		}
#ifdef CONFIG_THERMAL_EMULATION
		/* An emulated value must never reduce a physical cooling request. */
		if (READ_ONCE(zone->emul_temperature)) {
			ret = -EINVAL;
			break;
		}
#endif
		ret = thermal_zone_get_temp(zone, &temp);
		if (ret)
			break;
#ifdef CONFIG_THERMAL_EMULATION
		if (READ_ONCE(zone->emul_temperature)) {
			ret = -EINVAL;
			break;
		}
#endif
		if (!y4_fan1_temp_valid(temp)) {
			ret = -ERANGE;
			break;
		}
		hottest = max(hottest, temp);
	}
	mutex_lock(&ctx->lock);
	if (!fan->stopping) {
		fan->policy.temp_valid = !ret;
		fan->temperature_error = ret;
		if (!ret) {
			fan->policy.temp = hottest;
			/* Oldest read, not completion time: slow reads cannot refresh age. */
			fan->policy.temp_ms = started;
		}
		mod_delayed_work(system_highpri_wq, &fan->guard_work, 0);
		queue_delayed_work(system_unbound_wq, &fan->temperature_work, HZ);
	}
	mutex_unlock(&ctx->lock);
}

static void y4_fan1_guard_work(struct work_struct *work)
{
	struct y4_fan1 *fan = container_of(to_delayed_work(work),
					 struct y4_fan1, guard_work);
	struct pwm_fan_ctx *ctx = fan->ctx;
	struct y4_fan1_policy *p = &fan->policy;
	unsigned long tach_at;
	u64 now;

	mutex_lock(&ctx->lock);
	if (fan->stopping)
		goto out;
	now = y4_fan1_now();
	tach_at = READ_ONCE(ctx->tach_updated);
	if (fan->tach_available && tach_at && tach_at != fan->last_tach) {
		u64 sample_ms = now - jiffies_to_msecs(jiffies - tach_at);
		unsigned int rpm = READ_ONCE(ctx->tachs[0].rpm);

		/* A delayed timer is an observation gap, not sustained zero RPM. */
		if (p->tach_valid && sample_ms - p->tach_ms >= Y4_FAN1_STALE_MS)
			y4_fan1_tach_sample(p, now, false, 0);
		y4_fan1_tach_sample(p, sample_ms, true, rpm);
		fan->last_tach = tach_at;
		if (rpm && now - sample_ms < Y4_FAN1_STALE_MS &&
		    sample_ms - p->start_ms >= Y4_FAN1_TACH_GRACE_MS + Y4_FAN1_ZERO_MS &&
		    !test_and_set_bit(Y4_FAN1_LOG_TACH_RUNNING, &y4_fan1_boot_logs[ctx->y4->number - 1]))
			printk_noconsole(KERN_INFO "SoC cooling fan FAN%u: tachometer reports %u RPM.\n", fan->number, rpm);
	}
	if (p->tach_valid && now - p->tach_ms >= Y4_FAN1_STALE_MS) {
		y4_fan1_tach_sample(p, now, false, 0);
		if (!test_and_set_bit(Y4_FAN1_LOG_TACH_STALE, &y4_fan1_boot_logs[ctx->y4->number - 1]))
			printk_noconsole(KERN_WARNING "SoC cooling fan FAN%u: tachometer samples expired; using full speed.\n", fan->number);
	}
	if (fan->tach_available && !tach_at &&
	    now - p->start_ms >= Y4_FAN1_STALE_MS &&
	    !test_and_set_bit(Y4_FAN1_LOG_TACH_STALE, &y4_fan1_boot_logs[ctx->y4->number - 1]))
		printk_noconsole(KERN_WARNING "SoC cooling fan FAN%u: tachometer samples unavailable; using full speed.\n", fan->number);
	if (now - p->start_ms >= Y4_FAN1_START_MS) {
		if (!p->temp_valid &&
		    !test_and_set_bit(Y4_FAN1_LOG_TEMPERATURE, &y4_fan1_boot_logs[ctx->y4->number - 1]))
			printk_noconsole(KERN_WARNING "SoC cooling fan FAN%u: SoC temperature unavailable (%d); using full speed.\n",
					 fan->number, fan->temperature_error);
		if (p->temp_valid && now - p->temp_ms >= Y4_FAN1_STALE_MS &&
		    !test_and_set_bit(Y4_FAN1_LOG_TEMP_STALE, &y4_fan1_boot_logs[ctx->y4->number - 1]))
			printk_noconsole(KERN_WARNING "SoC cooling fan FAN%u: SoC temperature samples expired; using full speed.\n", fan->number);
	}
	if (p->tach_missing &&
	    !test_and_set_bit(Y4_FAN1_LOG_MISSING, &y4_fan1_boot_logs[ctx->y4->number - 1]))
		printk_noconsole(KERN_WARNING "SoC cooling fan FAN%u: no tachometer feedback detected.\n", fan->number);
	{
		unsigned int target = y4_fan1_percent(p, now);

		if (target < fan->applied_percent) {
			if (now - fan->last_down_ms < 2000)
				target = fan->applied_percent;
			else
				target = max(target, fan->applied_percent - 5);
		}
		if (target != fan->applied_percent)
			fan->last_down_ms = now;
		y4_fan1_control(ctx, target);
	}
	queue_delayed_work(system_highpri_wq, &fan->guard_work, msecs_to_jiffies(500));
out:
	mutex_unlock(&ctx->lock);
}

static void y4_fan1_stop(struct pwm_fan_ctx *ctx, bool sync)
{
	struct y4_fan1 *fan = ctx->y4;

	WRITE_ONCE(fan->stopping, true);
	if (sync)
		cancel_delayed_work_sync(&fan->temperature_work);
	else
		cancel_delayed_work(&fan->temperature_work);
	cancel_delayed_work_sync(&fan->guard_work);
	del_timer_sync(&ctx->rpm_timer);
	mutex_lock(&ctx->lock);
	y4_fan1_control(ctx, 100);
	mutex_unlock(&ctx->lock);
}

static void y4_fan1_cleanup(void *data)
{
	y4_fan1_stop(data, true);
}

static void y4_fan1_start(struct pwm_fan_ctx *ctx)
{
	struct y4_fan1 *fan = ctx->y4;

	fan->policy = (struct y4_fan1_policy) { .start_ms = y4_fan1_now() };
	fan->stopping = false;
	fan->last_tach = 0;
	WRITE_ONCE(ctx->tach_updated, 0);
	ctx->sample_start = ktime_get();
	if (fan->tach_available) {
		atomic_set(&ctx->tachs[0].pulses, 0);
		mod_timer(&ctx->rpm_timer, jiffies + HZ);
	}
	queue_delayed_work(system_unbound_wq, &fan->temperature_work, 0);
	queue_delayed_work(system_highpri_wq, &fan->guard_work, 0);
}

static const struct hwmon_channel_info *y4_fan1_channels[] = {
	HWMON_CHANNEL_INFO(pwm, HWMON_PWM_INPUT | HWMON_PWM_ENABLE),
	HWMON_CHANNEL_INFO(fan, HWMON_F_INPUT), NULL,
};

static int y4_fan1_probe(struct pwm_fan_ctx *ctx)
{
	static const u32 sensor_ids[] = { 10, 8, 11, 9 };
	struct device *dev = ctx->dev;
	struct gpio_desc *tach_gpio;
	struct device *hwmon;
	int ret, i, irq;
	bool fan2 = of_device_is_compatible(dev->of_node, "myt,y4-soc-fan2");

	/* A module reload cannot preserve once-per-kernel-boot log state. */
	if (IS_MODULE(CONFIG_SENSORS_PWM_FAN) || !IS_ENABLED(CONFIG_THERMAL) ||
	    !of_device_is_compatible(ctx->pwm->chip->dev->of_node,
				 fan2 ? "myt,y4-fan2-pwm" : "myt,y4-fan1-pwm") ||
	    !ctx->pwm->chip->ops->get_state)
		return -EINVAL;
	ctx->y4 = devm_kzalloc(dev, sizeof(*ctx->y4), GFP_KERNEL);
	ctx->tachs = devm_kzalloc(dev, sizeof(*ctx->tachs), GFP_KERNEL);
	if (!ctx->y4 || !ctx->tachs)
		return -ENOMEM;
	ctx->y4->ctx = ctx;
	ctx->y4->number = fan2 ? 2 : 1;
	ctx->y4->temperature_error = -EAGAIN;
	INIT_DELAYED_WORK(&ctx->y4->temperature_work, y4_fan1_temperature_work);
	INIT_DELAYED_WORK(&ctx->y4->guard_work, y4_fan1_guard_work);
	timer_setup(&ctx->rpm_timer, sample_timer, 0);
	/* V11 fan rail is enabled by the MCU. */
	if (ctx->reg_en || ctx->pwm_state.period != Y4_FAN1_PERIOD_NS)
		return -EINVAL;
	ctx->enable_mode = pwm_enable_reg_enable;
	mutex_lock(&ctx->lock);
	y4_fan1_control(ctx, 100);
	mutex_unlock(&ctx->lock);
	if (of_count_phandle_with_args(dev->of_node, "myt,thermal-zones", NULL) != 4)
		return -EINVAL;
	for (i = 0; i < ARRAY_SIZE(sensor_ids); i++) {
		struct of_phandle_args sensor;
		struct device_node *zone;

		zone = of_parse_phandle(dev->of_node, "myt,thermal-zones", i);
		if (!zone)
			return -EINVAL;
		ret = of_parse_phandle_with_args(zone, "thermal-sensors",
						"#thermal-sensor-cells", 0, &sensor);
		if (!ret) {
			if (sensor.args_count != 1 || sensor.args[0] != sensor_ids[i])
				ret = -EINVAL;
			of_node_put(sensor.np);
		}
		ctx->y4->zone_names[i] = devm_kstrdup(dev, zone->name, GFP_KERNEL);
		of_node_put(zone);
		if (ret || !ctx->y4->zone_names[i])
			return ret ?: -ENOMEM;
	}
	tach_gpio = devm_gpiod_get(dev, "tach", GPIOD_IN);
	ret = IS_ERR(tach_gpio) ? PTR_ERR(tach_gpio) : 0;
	if (ret == -EPROBE_DEFER)
		return ret;
	if (!ret) {
		irq = gpiod_to_irq(tach_gpio);
		ret = irq < 0 ? irq : devm_request_irq(dev, irq, pulse_handler,
					 IRQF_TRIGGER_FALLING, "y4-fan1-tach", ctx->tachs);
	}
	if (ret == -EPROBE_DEFER)
		return ret;
	if (!ret) {
		ctx->y4->tach_available = true;
		ctx->tach_count = 1;
		ctx->tachs[0].pulses_per_revolution = 2;
	} else if (!test_and_set_bit(Y4_FAN1_LOG_TACH_UNAVAILABLE, &y4_fan1_boot_logs[ctx->y4->number - 1])) {
		printk_noconsole(KERN_WARNING "SoC cooling fan FAN%u: tachometer interface unavailable (%d); using full speed.\n", ctx->y4->number, ret);
	}
	ctx->info.ops = &pwm_fan_hwmon_ops;
	ctx->info.info = y4_fan1_channels;
	hwmon = devm_hwmon_device_register_with_info(dev,
		ctx->y4->number == 2 ? "y4_fan2" : "pwmfan",
		ctx, &ctx->info, NULL);
	if (IS_ERR(hwmon))
		return PTR_ERR(hwmon);
	ret = devm_add_action_or_reset(dev, y4_fan1_cleanup, ctx);
	if (ret)
		return ret;
	y4_fan1_start(ctx);
	return 0;
}
