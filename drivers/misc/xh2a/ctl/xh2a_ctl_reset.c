// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2026 Houmo AI Inc.
 * Author: mingdong.wang<mingdong.wang@houmo.ai>
 */

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>

#include <xh2a_host_drv.h>
#include <xh2a_ctl_internal.h>
#include <xh2a_pcie_api.h>
#include "xh2a_ctl.h"
#include "xh2a_ctl_dev.h"
#include "../ipu/include/xh2a_ipu_device.h"

static int xh2a_ctl_get_ipu_dev(struct xh2a_ctl_dev_ctx *ctl_ctx,
				struct xh2a_ipu_device **ipu_dev)
{
	struct xh2a_pcie_client *client = NULL;
	int ret;

	if (!ctl_ctx || !ctl_ctx->pcie_dev || !ipu_dev)
		return -EINVAL;

	ret = xh2a_pcie_get_client(ctl_ctx->pcie_dev, &client,
				   XH2A_IPU_DEVICE_NAME);
	if (ret || !client || !client->client_data)
		return -ENODEV;

	*ipu_dev = client->client_data;
	return 0;
}

/*
 * Iterate every device in @devmask and invoke @fn for it.
 *
 * When @continue_on_error is false the walk stops at the first unusable device
 * or the first callback error and returns that error. This is used by the
 * "enter recovery" and "poll local reset done" paths, where a missing device
 * must abort the whole operation.
 *
 * When @continue_on_error is true an unusable device or a device that cannot be
 * resolved is skipped, the callback is invoked for every remaining device, and
 * the first error seen is returned only after the full walk completes. This is
 * required by the recovery finish path: a single offline peer must never stop
 * the loop, otherwise later devices are left with ctc_recovery_pending set and
 * are never resumed or aborted.
 */
static int xh2a_ctl_reset_for_each_ipu_in_mask(
	struct xh2a_ctl_ctx *ctl, const unsigned long *devmask,
	int (*fn)(struct xh2a_ipu_device *ipu_dev, uint32_t dev_id, void *data),
	void *data, bool continue_on_error)
{
	struct xh2a_ctl_dev_ctx *ctl_ctx;
	int ret = 0;

	mutex_lock(&ctl->lock);
	list_for_each_entry(ctl_ctx, &ctl->ctl_list, node) {
		struct xh2a_ipu_device *ipu_dev = NULL;
		bool usable;
		int fn_ret;

		if (!test_bit(ctl_ctx->dev_id, devmask))
			continue;

		mutex_lock(&ctl_ctx->lock);
		usable = ctl_ctx->online && !ctl_ctx->removing &&
			 ctl_ctx->ipu_ready;
		mutex_unlock(&ctl_ctx->lock);
		if (!usable) {
			if (!ret)
				ret = -ENODEV;
			if (continue_on_error)
				continue;
			break;
		}

		fn_ret = xh2a_ctl_get_ipu_dev(ctl_ctx, &ipu_dev);
		if (fn_ret) {
			if (!ret)
				ret = fn_ret;
			if (continue_on_error)
				continue;
			break;
		}

		fn_ret = fn(ipu_dev, ctl_ctx->dev_id, data);
		if (fn_ret) {
			if (!ret)
				ret = fn_ret;
			if (continue_on_error)
				continue;
			break;
		}
	}
	mutex_unlock(&ctl->lock);

	return ret;
}

/*
 * Callback context for the setup walk. Once prepare succeeds, the device has
 * ctc_recovery_pending set and must be explicitly resolved on every later
 * setup failure; leaving it in READY would make a later ordinary reset take
 * the CTC completion path without a coordinator.
 */
struct xh2a_ctl_reset_prepare_ctx {
	DECLARE_BITMAP(prepared_mask, XH2A_CTL_MAX_DEVICES);
};

struct xh2a_ctl_reset_claim_ctx {
	uint32_t root_dev_id;
};

static int xh2a_ctl_reset_prepare_ipu_recovery(struct xh2a_ipu_device *ipu_dev,
						uint32_t dev_id, void *data)
{
	struct xh2a_ctl_reset_prepare_ctx *ctx = data;
	int ret;

	ret = xh2a_ipu_ctc_prepare_recovery(ipu_dev);
	if (!ret)
		set_bit(dev_id, ctx->prepared_mask);

	return ret;
}

static int xh2a_ctl_reset_claim_peer_timeout(struct xh2a_ipu_device *ipu_dev,
					      uint32_t dev_id, void *data)
{
	struct xh2a_ctl_reset_claim_ctx *ctx = data;

	if (dev_id == ctx->root_dev_id)
		return 0;

	return xh2a_ipu_ctc_claim_peer_timeout(ipu_dev);
}

static int xh2a_ctl_reset_enter_ipu_recovery(struct xh2a_ipu_device *ipu_dev,
					     uint32_t dev_id, void *data)
{
	(void)dev_id;
	(void)data;
	return xh2a_ipu_enter_ctc_recovery_reset(ipu_dev);
}

static int xh2a_ctl_reset_publish_ipu_timeout(struct xh2a_ipu_device *ipu_dev,
					       uint32_t dev_id, void *data)
{
	(void)dev_id;
	(void)data;

	xh2a_ipu_ctc_publish_timeout(ipu_dev);
	return 0;
}

static int xh2a_ctl_reset_finish_ipu_recovery(struct xh2a_ipu_device *ipu_dev,
					      uint32_t dev_id, void *data)
{
	bool success = *(bool *)data;

	(void)dev_id;

	if (success)
		xh2a_ipu_resume_after_ctc_recovery(ipu_dev);
	else
		xh2a_ipu_abort_ctc_recovery_fatal(ipu_dev);

	return 0;
}

static int xh2a_ctl_reset_ipu_local_done(struct xh2a_ipu_device *ipu_dev,
					 uint32_t dev_id, void *data)
{
	bool *all_done = data;

	(void)dev_id;

	/*
	 * A device that already went FATAL (local reset failed, post-reset
	 * sync timed out, or it was aborted) will never publish a clean local
	 * reset done. Fail the wait immediately instead of spinning until the
	 * 60s deadline. abort_ctc_recovery_fatal() also sets
	 * ctc_recovery_local_reset_done, so check FATAL first to avoid
	 * mistaking an aborted device for a successfully reset one.
	 */
	if (atomic_read(&ipu_dev->dev_state) == XH2A_IPU_DEV_FATAL)
		return -EIO;

	if (!xh2a_ipu_ctc_recovery_local_reset_done(ipu_dev))
		*all_done = false;

	return 0;
}

static int xh2a_ctl_reset_wait_ipu_recovery_done(struct xh2a_ctl_ctx *ctl,
						 const unsigned long *devmask)
{
	unsigned long deadline =
		jiffies + msecs_to_jiffies(XH2A_CTL_CTC_RECOVERY_TIMEOUT_MS);

	for (;;) {
		bool all_done = true;
		int ret;

		ret = xh2a_ctl_reset_for_each_ipu_in_mask(
			ctl, devmask, xh2a_ctl_reset_ipu_local_done,
			&all_done, false);
		if (ret)
			return ret;
		if (all_done)
			return 0;
		if (time_after_eq(jiffies, deadline))
			return -ETIMEDOUT;

		msleep(20);
	}
}

void xh2a_ctl_ctc_recovery_work(struct work_struct *work)
{
	struct xh2a_ctl_ctx *ctl =
		container_of(work, struct xh2a_ctl_ctx, ctc_recovery_work);
	DECLARE_BITMAP(devmask, XH2A_CTL_MAX_DEVICES);
	u64 generation;
	bool requeue = false;
	bool success;
	int finish_ret;
	int ret;

	mutex_lock(&ctl->lock);
	bitmap_copy(devmask, ctl->ctc_recovery_devmask, XH2A_CTL_MAX_DEVICES);
	generation = ctl->ctc_auto_generation;
	mutex_unlock(&ctl->lock);

	ret = xh2a_ctl_reset_wait_ipu_recovery_done(ctl, devmask);
	mutex_lock(&ctl->ctc_mutex);
	if (!ret)
		ret = xh2a_ctl_ctc_recover_devmask_locked(ctl, devmask);

	success = ret == 0;
	/*
	 * Resume or abort every device in the mask. Use continue_on_error so an
	 * offline/removing peer in the middle of the mask cannot stop the walk
	 * and leave later devices stuck with ctc_recovery_pending set forever.
	 */
	finish_ret = xh2a_ctl_reset_for_each_ipu_in_mask(
		ctl, devmask, xh2a_ctl_reset_finish_ipu_recovery, &success,
		true);
	if (!ret && finish_ret)
		ret = finish_ret;

	if (ret)
		pr_err("[CTC] recovery failed ret=%d\n", ret);

	mutex_lock(&ctl->lock);
	ctl->ctc_recovery_errno = ret;
	ctl->ctc_recovery_active = false;
	ctl->ctc_recovery_owner_pid_snap = 0;
	ctl->ctc_recovery_root_dev_id = XH2A_CTL_MAX_DEVICES;
	bitmap_zero(ctl->ctc_recovery_devmask, XH2A_CTL_MAX_DEVICES);
	if (generation != ctl->ctc_auto_generation) {
		ctl->ctc_topology_dirty = true;
		ctl->ctc_auto_state = XH2A_CTL_CTC_AUTO_PENDING;
		ctl->ctc_auto_errno = 0;
		requeue = true;
	} else if (ret) {
		ctl->ctc_auto_state = XH2A_CTL_CTC_AUTO_FAILED;
		ctl->ctc_auto_errno = ret;
	} else {
		ctl->ctc_auto_state = XH2A_CTL_CTC_AUTO_READY;
		ctl->ctc_auto_errno = 0;
	}
	mutex_unlock(&ctl->lock);
	if (requeue)
		mod_delayed_work(system_wq, &ctl->ctc_auto_work,
				 msecs_to_jiffies(
					 XH2A_CTL_CTC_AUTO_INIT_DELAY_MS));
	mutex_unlock(&ctl->ctc_mutex);
	wake_up_all(&ctl->ctc_wq);
}

int xh2a_ctl_ctc_begin_ipu_timeout_recovery(void *timeout_handle)
{
	struct xh2a_ctl_ctx *ctl = NULL;
	struct xh2a_ctl_reset_prepare_ctx prepare_ctx = { { 0 } };
	struct xh2a_ctl_reset_claim_ctx claim_ctx;
	uint32_t timeout_dev_id;
	DECLARE_BITMAP(devmask, XH2A_CTL_MAX_DEVICES);
	pid_t owner_pid_snap;
	bool already_active = false;
	bool is_root = false;
	int ret;

	if (!timeout_handle)
		return -EINVAL;

	ret = xh2a_ctl_get_ctx(&ctl);
	if (ret)
		return ret;

	timeout_dev_id = xh2a_pcie_device_index(timeout_handle);
	ret = xh2a_ctl_get_owner_devmask(ctl, timeout_dev_id, &owner_pid_snap,
					 devmask);
	if (ret)
		return ret;

	mutex_lock(&ctl->ctc_mutex);
	mutex_lock(&ctl->lock);
	if (ctl->ctc_recovery_active) {
		already_active = test_bit(timeout_dev_id,
					ctl->ctc_recovery_devmask);
		is_root = ctl->ctc_recovery_root_dev_id == timeout_dev_id;
		mutex_unlock(&ctl->lock);
		mutex_unlock(&ctl->ctc_mutex);
		if (!already_active)
			return -EBUSY;
		return is_root ? XH2A_CTL_CTC_RECOVERY_ROOT :
				 XH2A_CTL_CTC_RECOVERY_PEER;
	}

	ctl->ctc_recovery_active = true;
	ctl->ctc_recovery_owner_pid_snap = owner_pid_snap;
	ctl->ctc_recovery_root_dev_id = timeout_dev_id;
	bitmap_copy(ctl->ctc_recovery_devmask, devmask,
		    XH2A_CTL_MAX_DEVICES);
	ctl->ctc_recovery_errno = 0;
	mutex_unlock(&ctl->lock);

	claim_ctx.root_dev_id = timeout_dev_id;
	ret = xh2a_ctl_reset_for_each_ipu_in_mask(
		ctl, devmask, xh2a_ctl_reset_prepare_ipu_recovery, &prepare_ctx,
		false);
	if (ret)
		goto abort_prepared;
	ret = xh2a_ctl_reset_for_each_ipu_in_mask(
		ctl, devmask, xh2a_ctl_reset_claim_peer_timeout, &claim_ctx, false);
	if (ret)
		goto abort_prepared;

	ret = xh2a_ctl_reset_for_each_ipu_in_mask(
		ctl, devmask, xh2a_ctl_reset_enter_ipu_recovery, NULL, false);
	if (ret)
		goto abort_prepared;

	/* A participant becomes visible to userspace only after every peer has
	 * been claimed and every device accepted the local-reset admission. */
	ret = xh2a_ctl_reset_for_each_ipu_in_mask(
		ctl, devmask, xh2a_ctl_reset_publish_ipu_timeout, NULL, false);
	if (ret)
		goto abort_prepared;

	queue_work(system_wq, &ctl->ctc_recovery_work);
	mutex_unlock(&ctl->ctc_mutex);

	pr_warn("[CTC] recovery started owner=%d timeout_dev=%u\n",
		owner_pid_snap, timeout_dev_id);

	return XH2A_CTL_CTC_RECOVERY_ROOT;

abort_prepared:
	{
		bool success = false;

		/* A timeout result may already have been published. Do not leave a
		 * prepared participant apparently usable when the coordinated reset
		 * could not be established. Continue across offline peers so every
		 * device still reachable by CTL is terminally resolved. */
		xh2a_ctl_reset_for_each_ipu_in_mask(
			ctl, prepare_ctx.prepared_mask,
			xh2a_ctl_reset_finish_ipu_recovery, &success, true);
	}
	mutex_lock(&ctl->lock);
	ctl->ctc_recovery_active = false;
	ctl->ctc_recovery_owner_pid_snap = 0;
	ctl->ctc_recovery_root_dev_id = XH2A_CTL_MAX_DEVICES;
	bitmap_zero(ctl->ctc_recovery_devmask, XH2A_CTL_MAX_DEVICES);
	ctl->ctc_recovery_errno = ret;
	mutex_unlock(&ctl->lock);
	mutex_unlock(&ctl->ctc_mutex);
	return ret;
}

MODULE_LICENSE("GPL");
