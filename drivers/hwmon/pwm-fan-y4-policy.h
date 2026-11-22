/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PWM_FAN_Y4_POLICY_H
#define PWM_FAN_Y4_POLICY_H

#define Y4_FAN1_PERIOD_NS 40000
#define Y4_FAN1_START_MS 2000
#define Y4_FAN1_STALE_MS 3000
#define Y4_FAN1_TACH_GRACE_MS 10000
#define Y4_FAN1_ZERO_MS 5000

/* This state is protected by pwm_fan_ctx.lock in the kernel consumer. */
struct y4_fan1_policy {
	unsigned long long start_ms;
	unsigned long long temp_ms;
	unsigned long long tach_ms;
	unsigned long long zero_ms;
	int temp;
	bool temp_valid;
	bool tach_valid;
	bool zero_pending;
	bool tach_missing;
};

static inline bool y4_fan1_temp_valid(int temp)
{
	return temp >= -40000 && temp <= 125000;
}

static inline unsigned int y4_fan1_temperature_percent(int temp)
{
	if (temp < 45000)
		return 30;
	if (temp >= 70000)
		return 100;
	return 50;
}

/* Called once per NEW timer sample. Unavailable feedback is never zero RPM. */
static inline void y4_fan1_tach_sample(struct y4_fan1_policy *p,
		unsigned long long now, bool valid, unsigned int rpm)
{
	p->tach_valid = valid;
	if (!valid) {
		p->zero_pending = false;
		p->tach_missing = false;
		return;
	}
	p->tach_ms = now;
	if (rpm || now - p->start_ms < Y4_FAN1_TACH_GRACE_MS) {
		p->zero_pending = false;
		p->tach_missing = false;
		return;
	}
	if (!p->zero_pending) {
		p->zero_pending = true;
		p->zero_ms = now;
	}
	p->tach_missing = now - p->zero_ms >= Y4_FAN1_ZERO_MS;
}

static inline unsigned int y4_fan1_percent(const struct y4_fan1_policy *p,
					 unsigned long long now)
{
	if (now - p->start_ms < Y4_FAN1_START_MS || !p->temp_valid ||
	    now - p->temp_ms >= Y4_FAN1_STALE_MS || !p->tach_valid ||
	    now - p->tach_ms >= Y4_FAN1_STALE_MS || p->tach_missing)
		return 100;
	return y4_fan1_temperature_percent(p->temp);
}

static inline unsigned long long y4_fan1_duty_ns(unsigned int percent)
{
	return (unsigned long long)Y4_FAN1_PERIOD_NS * percent / 100;
}
#endif
