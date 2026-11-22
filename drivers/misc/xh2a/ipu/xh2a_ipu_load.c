// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2025 Houmo AI Inc.
 * Author: mingdong.wang<mingdong.wang@houmo.ai>
 */

#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>
#include <linux/reboot.h>
#include <linux/bug.h>
#include <xh2a_address.h>

#include "xh2a_ipu_device.h"
#include "xh2a_ipu_group.h"
#include "xh2a_ipu_load.h"
#include "xh2a_ipu_config.h"
#include "version_compat.h"

static inline uint64_t time_delta_us(uint64_t new_time, uint64_t old_time)
{
	return new_time - old_time;
}

void xh2a_ipu_load_on_enqueue(struct xh2a_ipu_device *ipu_dev, int core_id,
			      uint64_t start_time_us)
{
	unsigned long flags;
	struct xh2a_ipu_load_core *ld_core;

	if (!ipu_dev || core_id < 0 || core_id >= XH2A_IPU_CORE_NUM)
		return;

	ld_core = &ipu_dev->load_core[core_id];

	spin_lock_irqsave(&ipu_dev->load_lock, flags);

	if (!ld_core->is_working) {
		ld_core->is_working = true;
		ld_core->start_ts = start_time_us;
	}

	ld_core->group_count++;

	spin_unlock_irqrestore(&ipu_dev->load_lock, flags);
}

void xh2a_ipu_load_on_done(struct xh2a_ipu_device *ipu_dev, int core_id,
			   uint64_t end_time_us)
{
	unsigned long flags;
	struct xh2a_ipu_load_core *ld_core;

	if (!ipu_dev || core_id < 0 || core_id >= XH2A_IPU_CORE_NUM)
		return;

	ld_core = &ipu_dev->load_core[core_id];
	spin_lock_irqsave(&ipu_dev->load_lock, flags);

	if (ld_core->group_count == 0)
		goto out;

	ld_core->group_count--;

	if (ld_core->group_count == 0) {
		if (ld_core->is_working) {
			ld_core->curr_busy_time +=
				time_delta_us(end_time_us, ld_core->start_ts);
			ld_core->is_working = false;
		}
	}

out:
	spin_unlock_irqrestore(&ipu_dev->load_lock, flags);
}

void xh2a_ipu_get_core_load(struct xh2a_ipu_device *ipu_dev, uint32_t core_id,
			    uint32_t *ipu_load)
{
	uint64_t current_ts, delta_ts, busy_time;
	uint64_t sample_period_us = IPU_SAMPLE_PERIOD * 1000ULL;
	struct xh2a_ipu_load_core *ld_core;

	if (core_id < 0 || core_id >= XH2A_IPU_CORE_NUM) {
		dev_err(ipu_dev->cdev_dev, "%s: core_id:%u err\n", __func__,
			core_id);
		return;
	}

	ld_core = &ipu_dev->load_core[core_id];

	current_ts = ktime_to_us(ktime_get());
	delta_ts = time_delta_us(current_ts, ld_core->last_ts);

	if (delta_ts >= sample_period_us)
		busy_time = 0;
	else
		busy_time = ld_core->last_busy_time *
			    (sample_period_us - delta_ts) / sample_period_us;

	busy_time += ld_core->curr_busy_time;
	if (ld_core->is_working)
		busy_time += (current_ts - ld_core->start_ts);

	*ipu_load = busy_time * 10000 / sample_period_us;

	if (*ipu_load > 10000)
		*ipu_load = 10000;
}

void xh2a_ipu_load_work(struct work_struct *work)
{
	uint32_t load_value = 0, core_id, core_load[XH2A_IPU_CORE_NUM];
	uint64_t now_us;
	unsigned long flags;
	struct xh2a_ipu_load_core *ld_core;
	struct xh2a_ipu_device *ipu_dev =
		container_of(work, struct xh2a_ipu_device, ipu_load_work);

	if (unlikely(!atomic_read(&ipu_dev->load_running)))
		return;

	spin_lock_irqsave(&ipu_dev->load_lock, flags);

	now_us = ktime_to_us(ktime_get());

	/* Do the load work here */
	for (core_id = 0; core_id < XH2A_IPU_CORE_NUM; core_id++) {
		ld_core = &ipu_dev->load_core[core_id];

		if (ld_core->is_working)
			ld_core->curr_busy_time += now_us - ld_core->start_ts;

		ld_core->last_busy_time = ld_core->curr_busy_time;
		ld_core->curr_busy_time = 0;
		ld_core->start_ts = now_us;
		ld_core->last_ts = now_us;
	}

	for (core_id = 0; core_id < XH2A_IPU_CORE_NUM; core_id++)
		xh2a_ipu_get_core_load(ipu_dev, core_id, &core_load[core_id]);

	load_value = core_load[0] | (core_load[1] << 16);
	spin_unlock_irqrestore(&ipu_dev->load_lock, flags);

	if (likely(atomic_read(&ipu_dev->load_running))) {
		xh2a_ipu_update_load(ipu_dev, load_value);
		mod_timer(&ipu_dev->load_timer,
			  jiffies + msecs_to_jiffies(IPU_SAMPLE_PERIOD));
	}
}

static void xh2a_ipu_load_timer_handler(struct timer_list *t)
{
	struct xh2a_ipu_device *ipu_dev;

	ipu_dev = compat_timer_container_of(ipu_dev, t, load_timer);

	if (unlikely(!atomic_read(&ipu_dev->load_running)))
		return;

	queue_work(ipu_dev->load_wq, &ipu_dev->ipu_load_work);
}

void xh2a_ipu_load_start(struct xh2a_ipu_device *ipu_dev)
{
	unsigned long flags;
	struct xh2a_ipu_load_core *load_core;
	u64 now = ktime_to_us(ktime_get());
	uint32_t core_id;

	if (!ipu_dev)
		return;

	mutex_lock(&ipu_dev->load_mutex);
	if (atomic_read(&ipu_dev->is_reboot))
		goto unlock;

	/* do nothing if load monitor is already running */
	if (atomic_xchg(&ipu_dev->load_running, 1))
		goto unlock;

	xh2a_ipu_update_load(ipu_dev, 0);

	spin_lock_irqsave(&ipu_dev->load_lock, flags);
	for (core_id = 0; core_id < XH2A_IPU_CORE_NUM; core_id++) {
		load_core = &ipu_dev->load_core[core_id];
		load_core->curr_busy_time = 0;
		load_core->last_busy_time = 0;
		load_core->start_ts = now;
		load_core->last_ts = now;
		load_core->group_count = 0;
		load_core->is_working = false;
	}
	spin_unlock_irqrestore(&ipu_dev->load_lock, flags);

	mod_timer(&ipu_dev->load_timer,
		  jiffies + msecs_to_jiffies(IPU_SAMPLE_PERIOD));
unlock:
	mutex_unlock(&ipu_dev->load_mutex);
}

void xh2a_ipu_load_stop(struct xh2a_ipu_device *ipu_dev)
{
	bool running;

	if (!ipu_dev)
		return;

	mutex_lock(&ipu_dev->load_mutex);
	running = atomic_xchg(&ipu_dev->load_running, 0);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 16, 0)
	timer_delete_sync(&ipu_dev->load_timer);
#else
	del_timer_sync(&ipu_dev->load_timer);
#endif
	cancel_work_sync(&ipu_dev->ipu_load_work);
	/* A worker past its running check may have rearmed while we drained it. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 16, 0)
	timer_delete_sync(&ipu_dev->load_timer);
#else
	del_timer_sync(&ipu_dev->load_timer);
#endif
	if (running)
		xh2a_ipu_update_load(ipu_dev, 0);
	mutex_unlock(&ipu_dev->load_mutex);
}

static int xh2a_ipu_load_shutdown_cb(struct notifier_block *nb,
				     unsigned long action, void *data)
{
	struct xh2a_ipu_device *ipu_dev =
		container_of(nb, struct xh2a_ipu_device, load_shutdown_nb);

	atomic_set(&ipu_dev->is_reboot, 1);
	xh2a_ipu_load_stop(ipu_dev);

	return 0;
}

void xh2a_ipu_load_init(struct xh2a_ipu_device *ipu_dev)
{
	if (!ipu_dev)
		return;

	spin_lock_init(&ipu_dev->load_lock);
	mutex_init(&ipu_dev->load_mutex);
	atomic_set(&ipu_dev->is_reboot, 0);
	atomic_set(&ipu_dev->load_running, 0);
	ipu_dev->load_shutdown_nb.notifier_call = xh2a_ipu_load_shutdown_cb;
	register_reboot_notifier(&ipu_dev->load_shutdown_nb);
	timer_setup(&ipu_dev->load_timer, xh2a_ipu_load_timer_handler, 0);
	xh2a_ipu_load_start(ipu_dev);
}

void xh2a_ipu_load_exit(struct xh2a_ipu_device *ipu_dev)
{
	unregister_reboot_notifier(&ipu_dev->load_shutdown_nb);
	xh2a_ipu_load_stop(ipu_dev);
	atomic_set(&ipu_dev->is_reboot, 0);
}
