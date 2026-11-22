// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2025 Houmo AI Inc.
 * Author: mingdong.wang<mingdong.wang@houmo.ai>
 */
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/ioctl.h>
#include <linux/file.h>
#include <linux/mm.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/jiffies.h>
#include <linux/vmalloc.h>
#include <linux/suspend.h>
#include <linux/delay.h>
#include <linux/pm_runtime.h>
#include <xh2a_host_drv.h>
#include <xh2a_pcie_api.h>
#include <xh2a_spm_api.h>
#include <xh2a_address.h>
#include <version_compat.h>
#include <xh2a_chrdev.h>

#include "xh2a_ipu_device.h"
#include "xh2a_ipu_file_handle.h"
#include "xh2a_ipu_policy.h"
#include "xh2a_ipu_group.h"
#include "xh2a_ipu_ioctl.h"
#include "xh2a_ipu_interrupt.h"
#include "xh2a_ipu_mempool.h"
#include "xh2a_ipu_ctc.h"

#define XH2A_IPU_PM_QUIESCE_TIMEOUT_MS		 3000
#define XH2A_IPU_GROUP_SCHED_RETRY_MS		 1
#define XH2A_IPU_GROUP_SCHED_BUSY_WARN_THRESHOLD 1000
#define XH2A_IPU_RESET_QUIESCE_TIMEOUT_MS	 2000
#define XH2A_IPU_RESET_POST_SYNC_DELAY_IDLE_MS	 100
#define XH2A_IPU_RESET_POST_SYNC_DELAY_PER_GROUP_MS 500
#define XH2A_IPU_AUTO_RESET_DELAY_MS		 1000
#define XH2A_IPU_CTC_RECOVERY_RETRY_MS		 100

static void xh2a_ipu_drain_booter_queue_groups(struct xh2a_ipu_device *ipu_dev);
static void xh2a_ipu_quiesce_exec_groups(struct xh2a_ipu_device *ipu_dev);

struct xh2a_ipu_reset_waiter {
	struct list_head node;
	struct completion done;
	int result;
};

static void xh2a_device_schedule_work(struct work_struct *work)
{
	struct xh2a_ipu_group *group = NULL;
	enum xh2a_ipu_policy_result result = XH2A_POLICY_NO_RESOURCE;
	struct xh2a_ipu_device *ipu_dev =
		container_of(work, struct xh2a_ipu_device, group_sche_work);

	dev_dbg(ipu_dev->cdev_dev, "ipu schedule group work\n");

	mutex_lock(&ipu_dev->dev_mutex);

	if (!ipu_dev->policy) {
		mutex_unlock(&ipu_dev->dev_mutex);
		return;
	}

	if (!xh2a_ipu_device_ready(ipu_dev)) {
		mutex_unlock(&ipu_dev->dev_mutex);
		return;
	}

	group = ipu_dev->policy->ops->pick_runnable_group(ipu_dev->policy,
							  &result);

	while (group) {
		mutex_unlock(&ipu_dev->dev_mutex);
		xh2a_ipu_group_load_kds(group);
		xh2a_ipu_group_put(group);
		mutex_lock(&ipu_dev->dev_mutex);

		if (!ipu_dev->policy)
			break;

		if (!xh2a_ipu_device_ready(ipu_dev))
			break;

		group = ipu_dev->policy->ops->pick_runnable_group(
			ipu_dev->policy, &result);
	}

	if (result == XH2A_POLICY_BUSY_RETRY) {
		ipu_dev->group_sche_busy_retry_count++;
		if (ipu_dev->group_sche_busy_retry_count >=
		    XH2A_IPU_GROUP_SCHED_BUSY_WARN_THRESHOLD) {
			dev_warn_ratelimited(
				ipu_dev->cdev_dev,
				"%s: tile queue busy retry keeps blocking "
				"schedule, count=%u\n",
				__func__, ipu_dev->group_sche_busy_retry_count);
		}
		queue_delayed_work(
			ipu_dev->sched_wq, &ipu_dev->group_sche_retry_work,
			msecs_to_jiffies(XH2A_IPU_GROUP_SCHED_RETRY_MS));
	} else {
		ipu_dev->group_sche_busy_retry_count = 0;
	}

	mutex_unlock(&ipu_dev->dev_mutex);
}

static void xh2a_device_schedule_retry_work(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct xh2a_ipu_device *ipu_dev = container_of(
		dwork, struct xh2a_ipu_device, group_sche_retry_work);

	queue_work(ipu_dev->sched_wq, &ipu_dev->group_sche_work);
}

static struct workqueue_struct *
xh2a_ipu_create_singlethread_wq(struct xh2a_ipu_device *ipu_dev,
				const char *suffix)
{
	char wq_name[XH2A_IPU_WQ_NAME_LEN];

	snprintf(wq_name, sizeof(wq_name), "%s_%s", ipu_dev->name, suffix);

	return create_singlethread_workqueue(wq_name);
}

void xh2a_ipu_cancel_async_works(struct xh2a_ipu_device *ipu_dev)
{
	int i;
	uint32_t mask;
	struct xh2a_pcie_client *client;

	if (!ipu_dev)
		return;

	cancel_delayed_work_sync(&ipu_dev->group_sche_retry_work);
	cancel_work_sync(&ipu_dev->group_sche_work);
	cancel_work_sync(&ipu_dev->ipu_event_work);
	cancel_work_sync(&ipu_dev->ipu_load_work);

	client = &ipu_dev->client;
	if (ipu_dev->msi_work_inited) {
		for (i = 0; i < client->work_num; i++)
			cancel_work_sync(client->work[i].msi_work);
	}

	for (i = 0; i < XH2A_IPU_CORE_NUM; i++) {
		mask = (0xF << (i * 4));
		if ((ipu_dev->available_tiles_mask & mask) != mask)
			continue;

		cancel_work_sync(&ipu_dev->core_works[i].work);
	}
}

void xh2a_ipu_cancel_auto_reset(struct xh2a_ipu_device *ipu_dev)
{
	if (!ipu_dev || !ipu_dev->reset_wq)
		return;

	cancel_delayed_work_sync(&ipu_dev->auto_reset_work);
	cancel_delayed_work_sync(&ipu_dev->ctc_recovery_retry_work);
}

void xh2a_ipu_drain_pending_events(struct xh2a_ipu_device *ipu_dev)
{
	unsigned long flags;
	struct xh2a_ipu_event *event, *event_n;
	LIST_HEAD(event_list);

	spin_lock_irqsave(&ipu_dev->event_lock, flags);
	list_splice_init(&ipu_dev->isr_event_list, &event_list);
	spin_unlock_irqrestore(&ipu_dev->event_lock, flags);

	list_for_each_entry_safe(event, event_n, &event_list, node) {
		list_del_init(&event->node);
		xh2a_ipu_event_free(&ipu_dev->event_pool, event);
	}
}

static int xh2a_ipu_begin_reset(struct xh2a_ipu_device *ipu_dev,
				uint32_t mode)
{
	int old_state;

	do {
		old_state = atomic_read(&ipu_dev->dev_state);

		if (old_state == XH2A_IPU_DEV_FATAL)
			return -EIO;
		if (old_state == XH2A_IPU_DEV_RESETTING)
			return -EBUSY;
	} while (atomic_cmpxchg(&ipu_dev->dev_state, old_state,
				XH2A_IPU_DEV_RESETTING) != old_state);

	atomic_set(&ipu_dev->active_reset_mode, mode);
	return 0;
}

static void xh2a_ipu_resume_after_reset(struct xh2a_ipu_device *ipu_dev)
{
	atomic_set(&ipu_dev->reset_flag, 0);
	atomic_set(&ipu_dev->active_reset_mode, XH2A_IPU_RESET_MODE_DISCARD);
	/* A recovery abort may race a late reset completion.  Do not allow that
	 * completion to resurrect a device which has already become FATAL. */
	atomic_cmpxchg(&ipu_dev->dev_state, XH2A_IPU_DEV_RESETTING,
		       XH2A_IPU_DEV_READY);
	queue_work(ipu_dev->sched_wq, &ipu_dev->group_sche_work);
}

/* Caller holds reset_mutex. Each waiter receives this reset epoch's result
 * before a later reset can reuse the device-level owner/result fields. */
static void xh2a_ipu_complete_reset_waiters(struct xh2a_ipu_device *ipu_dev,
					    int result)
{
	struct xh2a_ipu_reset_waiter *waiter, *next;

	list_for_each_entry_safe(waiter, next, &ipu_dev->reset_waiter_list, node) {
		list_del_init(&waiter->node);
		waiter->result = result;
		complete(&waiter->done);
	}
}

static void
xh2a_ipu_complete_ctc_recovery_local_reset(struct xh2a_ipu_device *ipu_dev)
{
	int old_state;

	atomic_set(&ipu_dev->reset_flag, 0);
	atomic_set(&ipu_dev->active_reset_mode, XH2A_IPU_RESET_MODE_DISCARD);
	old_state = atomic_cmpxchg(&ipu_dev->dev_state,
				   XH2A_IPU_DEV_RESETTING,
				   XH2A_IPU_DEV_CTC_RESETTING);
	if (old_state == XH2A_IPU_DEV_RESET_REQUIRED)
		atomic_cmpxchg(&ipu_dev->dev_state,
			       XH2A_IPU_DEV_RESET_REQUIRED,
			       XH2A_IPU_DEV_CTC_RESETTING);
	atomic_set(&ipu_dev->ctc_recovery_local_reset_done, 1);
	wake_up_all(&ipu_dev->group_sync_wq);
}

int xh2a_ipu_enter_ctc_recovery_reset(struct xh2a_ipu_device *ipu_dev)
{
	int old_state;

	if (!ipu_dev)
		return -EINVAL;
	if (atomic_read(&ipu_dev->dev_removed) ||
	    atomic_read(&ipu_dev->quiesce_flag))
		return -ENODEV;

	for (;;) {
		old_state = atomic_read(&ipu_dev->dev_state);
		switch (old_state) {
		case XH2A_IPU_DEV_READY:
			if (atomic_cmpxchg(&ipu_dev->dev_state, old_state,
					   XH2A_IPU_DEV_RESET_REQUIRED) !=
			    old_state)
				continue;
			xh2a_ipu_schedule_auto_reset(ipu_dev);
			return 0;
		case XH2A_IPU_DEV_RESET_REQUIRED:
			xh2a_ipu_schedule_auto_reset(ipu_dev);
			return 0;
		case XH2A_IPU_DEV_RESETTING:
		case XH2A_IPU_DEV_CTC_RESETTING:
			/* A reset ioctl may have completed the local reset after its
			 * timeout completion was published.  The coordinated recovery
			 * still owns the device, but there is no further reset to admit. */
			return 0;
		case XH2A_IPU_DEV_FATAL:
			return -EIO;
		default:
			return -EINVAL;
		}
	}
}

int xh2a_ipu_ctc_prepare_recovery(struct xh2a_ipu_device *ipu_dev)
{
	if (!ipu_dev)
		return -EINVAL;

	/* The root timeout worker reserves its claim after this coordinator
	 * returns.  Claims are cleared only when the preceding epoch finishes. */
	mutex_lock(&ipu_dev->reset_mutex);
	if (atomic_read(&ipu_dev->dev_removed) ||
	    atomic_read(&ipu_dev->quiesce_flag) ||
	    atomic_read(&ipu_dev->dev_state) == XH2A_IPU_DEV_FATAL) {
		mutex_unlock(&ipu_dev->reset_mutex);
		return -ENODEV;
	}
	/* Publish the pending bit before releasing reset_mutex.  An auto-reset
	 * worker may already be queued and must observe CTC ownership before it
	 * starts the hardware reset.  Serialize publication with group admission:
	 * either the group is enqueued first and belongs to the old epoch, or the
	 * pending bit is visible first and the submitter waits for recovery. */
	mutex_lock(&ipu_dev->dev_mutex);
	atomic_set(&ipu_dev->ctc_recovery_pending, 1);
	atomic_set(&ipu_dev->ctc_recovery_local_reset_done, 0);
	mutex_unlock(&ipu_dev->dev_mutex);
	mutex_unlock(&ipu_dev->reset_mutex);
	return 0;
}

int xh2a_ipu_ctc_claim_timeout_group(struct xh2a_ipu_device *ipu_dev,
				      struct xh2a_ipu_group *group,
				      int previous_status)
{
	int old_status;

	if (!ipu_dev || !group)
		return -EINVAL;
	if (previous_status != GROUP_RUNNING && previous_status != GROUP_PENDING)
		return -EINVAL;

	/* ctc_claim_mutex is the shared linearization point with reset admission.
	 * Do not take reset_mutex here: reset cancels timeout work while holding it.
	 */
	mutex_lock(&ipu_dev->ctc_claim_mutex);
	if (atomic_read(&ipu_dev->ctc_timeout_claimed)) {
		mutex_unlock(&ipu_dev->ctc_claim_mutex);
		return -EALREADY;
	}
	/* A normal reset has already claimed this device. Its cancellation path
	 * will complete the group, so do not start a second CTC recovery epoch. */
	if (atomic_read(&ipu_dev->dev_state) == XH2A_IPU_DEV_RESETTING &&
	    !atomic_read(&ipu_dev->ctc_recovery_pending)) {
		mutex_unlock(&ipu_dev->ctc_claim_mutex);
		return -EBUSY;
	}
	if (atomic_read(&ipu_dev->dev_removed) ||
	    atomic_read(&ipu_dev->quiesce_flag) ||
	    atomic_read(&ipu_dev->dev_state) == XH2A_IPU_DEV_FATAL) {
		mutex_unlock(&ipu_dev->ctc_claim_mutex);
		return -ENODEV;
	}

	old_status = atomic_cmpxchg(&group->status, previous_status,
				   GROUP_TIMEOUT);
	if (old_status != previous_status) {
		mutex_unlock(&ipu_dev->ctc_claim_mutex);
		return -EAGAIN;
	}

	mutex_lock(&group->mutex);
	group->exec_result[0] = XH2A_GROUP_KERNELS_TIMEOUT;
	mutex_unlock(&group->mutex);
	xh2a_ipu_group_get(group);
	ipu_dev->ctc_timeout_group = group;
	ipu_dev->ctc_timeout_previous_status = previous_status;
	atomic_set(&ipu_dev->ctc_timeout_claimed, 1);
	mutex_unlock(&ipu_dev->ctc_claim_mutex);

	return 0;
}

void xh2a_ipu_ctc_publish_timeout(struct xh2a_ipu_device *ipu_dev)
{
	struct xh2a_ipu_group *group;
	int previous_status;

	if (!ipu_dev)
		return;

	mutex_lock(&ipu_dev->ctc_claim_mutex);
	group = ipu_dev->ctc_timeout_group;
	if (!group || ipu_dev->ctc_timeout_publishing) {
		mutex_unlock(&ipu_dev->ctc_claim_mutex);
		return;
	}
	previous_status = ipu_dev->ctc_timeout_previous_status;
	ipu_dev->ctc_timeout_publishing = true;
	mutex_unlock(&ipu_dev->ctc_claim_mutex);

	xh2a_ipu_group_complete_ctc_timeout(group, previous_status);

	mutex_lock(&ipu_dev->ctc_claim_mutex);
	/* Keep the claim visible until completion has been fully published. */
	if (ipu_dev->ctc_timeout_group == group) {
		ipu_dev->ctc_timeout_group = NULL;
		ipu_dev->ctc_timeout_publishing = false;
	}
	mutex_unlock(&ipu_dev->ctc_claim_mutex);

	xh2a_ipu_group_put(group);
}

void xh2a_ipu_ctc_release_standalone_timeout_claim(
	struct xh2a_ipu_device *ipu_dev)
{
	if (!ipu_dev)
		return;

	/* A non-CTC timeout has already published its result. It must not keep
	 * reset admission blocked, while a newer claim must remain intact. */
	mutex_lock(&ipu_dev->ctc_claim_mutex);
	if (!ipu_dev->ctc_timeout_group)
		atomic_set(&ipu_dev->ctc_timeout_claimed, 0);
	mutex_unlock(&ipu_dev->ctc_claim_mutex);
}

bool xh2a_ipu_ctc_timeout_completion_deferred(
	struct xh2a_ipu_device *ipu_dev, struct xh2a_ipu_group *group)
{
	bool deferred;

	if (!ipu_dev || !group)
		return false;

	mutex_lock(&ipu_dev->ctc_claim_mutex);
	deferred = ipu_dev->ctc_timeout_group == group;
	mutex_unlock(&ipu_dev->ctc_claim_mutex);

	return deferred;
}

int xh2a_ipu_ctc_claim_peer_timeout(struct xh2a_ipu_device *ipu_dev)
{
	struct xh2a_ipu_group *group = NULL, *candidate;
	struct xh2a_ipu_booter_queue *queue;
	int core_id, old_status, ret;

	if (!ipu_dev)
		return -EINVAL;

	mutex_lock(&ipu_dev->ctc_claim_mutex);
	if (atomic_read(&ipu_dev->ctc_timeout_claimed)) {
		mutex_unlock(&ipu_dev->ctc_claim_mutex);
		return 0;
	}
	if (atomic_read(&ipu_dev->dev_removed) ||
	    atomic_read(&ipu_dev->quiesce_flag) ||
	    atomic_read(&ipu_dev->dev_state) == XH2A_IPU_DEV_FATAL) {
		mutex_unlock(&ipu_dev->ctc_claim_mutex);
		return -ENODEV;
	}
	mutex_unlock(&ipu_dev->ctc_claim_mutex);

	for (;;) {
		/* Snapshot a candidate under queue/policy locks, never nested under
		 * ctc_claim_mutex (completion work takes these locks first). */
		for (core_id = 0; core_id < XH2A_IPU_CORE_NUM && !group;
		     core_id++) {
			queue = &ipu_dev->tile_queues[core_id][0];
			mutex_lock(&queue->tile_mutex);
			list_for_each_entry(candidate, &queue->group_list,
					    tile_list_node) {
				if (atomic_read(&candidate->status) == GROUP_RUNNING) {
					xh2a_ipu_group_get(candidate);
					group = candidate;
					break;
				}
			}
			mutex_unlock(&queue->tile_mutex);
		}
		if (!group) {
			/* The policy is replaced/destroyed during PM and remove under
			 * dev_mutex. Keep that lock across the lookup so the returned
			 * group reference never comes from a detached policy. */
			mutex_lock(&ipu_dev->dev_mutex);
			group = xh2a_ipu_policy_find_pending_group(ipu_dev->policy);
			mutex_unlock(&ipu_dev->dev_mutex);
		}
		/* An idle peer still participates in the coordinated hardware reset;
		 * it simply has no in-flight group to claim as a timeout victim. */
		if (!group) {
			dev_warn(ipu_dev->cdev_dev,
				 "%s: idle peer joins CTC recovery: dev_state=%d pending=%d claimed=%d sync_cnt=%d\n",
				 __func__, atomic_read(&ipu_dev->dev_state),
				 atomic_read(&ipu_dev->ctc_recovery_pending),
				 atomic_read(&ipu_dev->ctc_timeout_claimed),
				 atomic_read(&ipu_dev->sync_cnt));
			return 0;
		}

		old_status = atomic_read(&group->status);
		ret = xh2a_ipu_ctc_claim_timeout_group(ipu_dev, group,
						       old_status);
		if (!ret) {
			xh2a_ipu_group_put(group);
			return 0;
		}
		if (ret == -EALREADY) {
			xh2a_ipu_group_put(group);
			return 0;
		}
		if (ret == -ENODEV) {
			xh2a_ipu_group_put(group);
			return ret;
		}

		xh2a_ipu_group_put(group);
		group = NULL;
		cond_resched();
	}
}

bool xh2a_ipu_ctc_recovery_local_reset_done(struct xh2a_ipu_device *ipu_dev)
{
	return ipu_dev && atomic_read(&ipu_dev->ctc_recovery_local_reset_done);
}

void xh2a_ipu_resume_after_ctc_recovery(struct xh2a_ipu_device *ipu_dev)
{
	if (!ipu_dev)
		return;

	/* Clear CTC admission flags before publishing READY.  A new timeout may
	 * arrive immediately after the state transition and must be able to claim
	 * its own recovery epoch. */
	atomic_set(&ipu_dev->ctc_recovery_pending, 0);
	atomic_set(&ipu_dev->ctc_recovery_local_reset_done, 0);
	mutex_lock(&ipu_dev->ctc_claim_mutex);
	atomic_set(&ipu_dev->ctc_timeout_claimed, 0);
	mutex_unlock(&ipu_dev->ctc_claim_mutex);
	atomic_set(&ipu_dev->reset_flag, 0);
	atomic_set(&ipu_dev->active_reset_mode, XH2A_IPU_RESET_MODE_DISCARD);

	/* The local reset that produced CTC_RESETTING has completed.  Release its
	 * epoch owner before allowing a later ordinary reset to claim the device. */
	mutex_lock(&ipu_dev->reset_mutex);
	if (atomic_read(&ipu_dev->dev_state) == XH2A_IPU_DEV_CTC_RESETTING) {
		ipu_dev->reset_owner = XH2A_IPU_RESET_OWNER_NONE;
		ipu_dev->reset_local_done = false;
		ipu_dev->reset_result = 0;
		atomic_set(&ipu_dev->dev_state, XH2A_IPU_DEV_READY);
	}
	mutex_unlock(&ipu_dev->reset_mutex);
	wake_up_all(&ipu_dev->group_sync_wq);
	queue_work(ipu_dev->sched_wq, &ipu_dev->group_sche_work);
}

void xh2a_ipu_abort_ctc_recovery_fatal(struct xh2a_ipu_device *ipu_dev)
{
	struct xh2a_ipu_policy *policy = NULL;

	if (!ipu_dev)
		return;

	/*
	 * Set ctc_recovery_local_reset_done so the recovery work's wait loop is
	 * released instead of spinning until its deadline; that loop checks the
	 * FATAL state first, so this device is still accounted as failed rather
	 * than as a clean local reset.
	 */
	/* Complete the claimed timeout before terminal cancellation.  This drops
	 * the claim record's kref and guarantees that only still-PENDING groups
	 * are candidates for the fatal cancel pass below. */
	xh2a_ipu_ctc_publish_timeout(ipu_dev);

	mutex_lock(&ipu_dev->ctc_claim_mutex);
	mutex_lock(&ipu_dev->dev_mutex);
	/* Publish FATAL before quiescing. group_execute() checks dev_state and
	 * otherwise could enqueue a new group during the quiesce window. */
	atomic_set(&ipu_dev->dev_state, XH2A_IPU_DEV_FATAL);
	policy = ipu_dev->policy;
	if (policy)
		xh2a_ipu_policy_cancel_all(policy);
	mutex_unlock(&ipu_dev->dev_mutex);
	mutex_unlock(&ipu_dev->ctc_claim_mutex);

	/* Setup/enter failure can happen before any hardware reset was started.
	 * Quiesce both policy and hardware-queue groups here so their waiters are
	 * completed and no work can continue against a terminal device. */
	xh2a_ipu_quiesce_exec_groups(ipu_dev);

	mutex_lock(&ipu_dev->reset_mutex);
	ipu_dev->reset_owner = XH2A_IPU_RESET_OWNER_NONE;
	ipu_dev->reset_local_done = false;
	ipu_dev->reset_result = -EIO;
	xh2a_ipu_complete_reset_waiters(ipu_dev, -EIO);
	mutex_unlock(&ipu_dev->reset_mutex);

	atomic_set(&ipu_dev->ctc_recovery_pending, 0);
	atomic_set(&ipu_dev->ctc_recovery_local_reset_done, 1);
	atomic_set(&ipu_dev->reset_flag, 0);
	atomic_set(&ipu_dev->active_reset_mode, XH2A_IPU_RESET_MODE_DISCARD);
	wake_up_all(&ipu_dev->group_sync_wq);
}

static void xh2a_ipu_converge_done_groups(struct xh2a_ipu_device *ipu_dev)
{
	int core_id;
	uint32_t current_rptr;
	struct xh2a_ipu_booter_queue *queue;

	for (core_id = 0; core_id < XH2A_IPU_CORE_NUM; core_id++) {
		LIST_HEAD(done_list);

		queue = &ipu_dev->tile_queues[core_id][0];

		mutex_lock(&queue->tile_mutex);
		current_rptr = xh2a_booter_queue_get_rptr(ipu_dev, queue);
		queue->last_rptr = queue->rptr;
		xh2a_ipu_group_done_handler(ipu_dev, queue, current_rptr, true,
					    &done_list);
		queue->rptr = current_rptr;
		WRITE_ONCE(queue->cached_rptr, queue->rptr);
		mutex_unlock(&queue->tile_mutex);

		xh2a_ipu_complete_done_groups(&done_list);
	}
}

static int __xh2a_ipu_device_reset_discard(struct xh2a_ipu_device *ipu_dev,
						 bool state_claimed)
{
	int ret;
	int core_id, tile_id;
	int initial_sync_cnt;
	unsigned int post_delay_ms;
	struct xh2a_ipu_booter_queue *queue;

	dev_dbg(ipu_dev->cdev_dev,
		 "%s: reset request enter pid=%d dev_state=%d reset=%d quiesce=%d sync_cnt=%d\n",
		 __func__, current->pid, atomic_read(&ipu_dev->dev_state),
		 atomic_read(&ipu_dev->reset_flag),
		 atomic_read(&ipu_dev->quiesce_flag),
		 atomic_read(&ipu_dev->sync_cnt));

	if (!state_claimed) {
		ret = xh2a_ipu_begin_reset(ipu_dev,
					   XH2A_IPU_RESET_MODE_DISCARD);
		if (ret) {
			dev_warn(ipu_dev->cdev_dev,
				 "%s: begin reset rejected pid=%d ret=%d dev_state=%d reset=%d sync_cnt=%d\n",
				 __func__, current->pid, ret,
				 atomic_read(&ipu_dev->dev_state),
				 atomic_read(&ipu_dev->reset_flag),
				 atomic_read(&ipu_dev->sync_cnt));
			return ret;
		}
	}

	mutex_lock(&ipu_dev->reset_mutex);

	atomic_set(&ipu_dev->reset_flag, 1);
	initial_sync_cnt = max(atomic_read(&ipu_dev->sync_cnt), 0);
	dev_warn(ipu_dev->cdev_dev,
		 "%s: reset_flag set pid=%d dev_state=%d sync_cnt=%d initial_sync_cnt=%d\n",
		 __func__, current->pid, atomic_read(&ipu_dev->dev_state),
		 atomic_read(&ipu_dev->sync_cnt), initial_sync_cnt);

	if (ipu_dev->group_work_inited)
		xh2a_ipu_cancel_async_works(ipu_dev);

	dev_warn(ipu_dev->cdev_dev,
		 "%s: async works canceled pid=%d sync_cnt=%d\n", __func__,
		 current->pid, atomic_read(&ipu_dev->sync_cnt));

	xh2a_ipu_drain_pending_events(ipu_dev);
	xh2a_ipu_converge_done_groups(ipu_dev);
	dev_warn(ipu_dev->cdev_dev,
		 "%s: done groups converged pid=%d sync_cnt=%d\n", __func__,
		 current->pid, atomic_read(&ipu_dev->sync_cnt));

	mutex_lock(&ipu_dev->dev_mutex);
	xh2a_ipu_policy_cancel_all(ipu_dev->policy);
	mutex_unlock(&ipu_dev->dev_mutex);
	xh2a_ipu_drain_booter_queue_groups(ipu_dev);
	dev_warn(ipu_dev->cdev_dev,
		 "%s: policy/booter groups discarded pid=%d sync_cnt=%d\n",
		 __func__, current->pid, atomic_read(&ipu_dev->sync_cnt));

	ret = wait_event_timeout(ipu_dev->group_sync_wq,
				 atomic_read(&ipu_dev->sync_cnt) == 0,
				 msecs_to_jiffies(
					 XH2A_IPU_RESET_QUIESCE_TIMEOUT_MS));
	if (ret == 0 && atomic_read(&ipu_dev->sync_cnt) != 0)
		dev_warn(ipu_dev->cdev_dev,
			 "%s: wait sync_cnt timeout before reset, sync_cnt=%d\n",
			 __func__, atomic_read(&ipu_dev->sync_cnt));
	else
		dev_warn(ipu_dev->cdev_dev,
			 "%s: sync_cnt drained before reset pid=%d wait_ret=%d\n",
			 __func__, current->pid, ret);

	if (atomic_read(&ipu_dev->sync_cnt) == 0) {
		if (initial_sync_cnt == 0)
			post_delay_ms = XH2A_IPU_RESET_POST_SYNC_DELAY_IDLE_MS;
		else
			post_delay_ms = initial_sync_cnt *
					XH2A_IPU_RESET_POST_SYNC_DELAY_PER_GROUP_MS;

		dev_warn(ipu_dev->cdev_dev,
			 "%s: sync_cnt is zero, delay %u ms before hw reset pid=%d initial_sync_cnt=%d\n",
			 __func__, post_delay_ms, current->pid,
			 initial_sync_cnt);
		msleep(post_delay_ms);
		dev_warn(ipu_dev->cdev_dev,
			 "%s: post sync delay done pid=%d sync_cnt=%d initial_sync_cnt=%d post_delay_ms=%u\n",
			 __func__, current->pid,
			 atomic_read(&ipu_dev->sync_cnt), initial_sync_cnt,
			 post_delay_ms);
	}

	dev_warn(ipu_dev->cdev_dev,
		 "%s: stopping load and shutting down hw pid=%d\n", __func__,
		 current->pid);
	xh2a_ipu_load_stop(ipu_dev);
	xh2a_ipu_hw_shutdown(ipu_dev, true);
	dev_warn(ipu_dev->cdev_dev,
		 "%s: hw shutdown done, starting hw pid=%d\n", __func__,
		 current->pid);
	xh2a_ipu_hw_startup(ipu_dev, true);
	dev_warn(ipu_dev->cdev_dev,
		 "%s: hw startup done, restarting load pid=%d\n", __func__,
		 current->pid);
	xh2a_ipu_load_start(ipu_dev);

	for (core_id = 0; core_id < XH2A_IPU_CORE_NUM; core_id++) {
		for (tile_id = 0; tile_id < XH2A_TILE_NUM_PER_CORE; tile_id++) {
			queue = &ipu_dev->tile_queues[core_id][tile_id];
			mutex_lock(&queue->tile_mutex);
			queue->rptr = 0;
			queue->wptr = 0;
			WRITE_ONCE(queue->cached_rptr, queue->rptr);
			WRITE_ONCE(queue->cached_wptr, queue->wptr);
			queue->last_rptr = 0;
			queue->flag &= XH2A_IPU_TILE_QUEUE_FLAG_BAD;
			mutex_unlock(&queue->tile_mutex);
		}
	}

	atomic_set(&ipu_dev->sync_cnt, 0);
	if (atomic_read(&ipu_dev->ctc_recovery_pending))
		xh2a_ipu_complete_ctc_recovery_local_reset(ipu_dev);
	else
		xh2a_ipu_resume_after_reset(ipu_dev);
	dev_warn(ipu_dev->cdev_dev,
		 "%s: reset finish pid=%d dev_state=%d reset=%d sync_cnt=%d\n",
		 __func__, current->pid, atomic_read(&ipu_dev->dev_state),
		 atomic_read(&ipu_dev->reset_flag),
		 atomic_read(&ipu_dev->sync_cnt));
	mutex_unlock(&ipu_dev->reset_mutex);

	return 0;
}

int xh2a_ipu_device_reset_discard(struct xh2a_ipu_device *ipu_dev)
{
	return __xh2a_ipu_device_reset_discard(ipu_dev, false);
}

int xh2a_ipu_device_reset_discard_owner(struct xh2a_ipu_device *ipu_dev,
					 enum xh2a_ipu_reset_owner owner)
{
	int ret;
	struct xh2a_ipu_reset_waiter waiter;

	if (!ipu_dev || owner == XH2A_IPU_RESET_OWNER_NONE)
		return -EINVAL;

	/* Serialize reset admission with timeout attribution. A claimed timeout
	 * must first establish or reject its CTC epoch; otherwise a normal reset
	 * can clear its claim while the timeout worker still owns the waiter.
	 *
	 * Global order is reset_mutex -> ctc_claim_mutex. Reset execution holds
	 * reset_mutex while completion convergence takes ctc_claim_mutex. */
	mutex_lock(&ipu_dev->reset_mutex);
	mutex_lock(&ipu_dev->ctc_claim_mutex);
	if (atomic_read(&ipu_dev->ctc_timeout_claimed) &&
	    !atomic_read(&ipu_dev->ctc_recovery_pending)) {
		ret = -EBUSY;
		goto unlock_admission;
	}
	/* A completed non-CTC reset does not reserve the device indefinitely.
	 * CTC_RESETTING is intentionally excluded: its local reset result belongs
	 * to the still-active recovery epoch until CTC completion. */
	if (ipu_dev->reset_owner != XH2A_IPU_RESET_OWNER_NONE &&
	    ipu_dev->reset_local_done &&
	    atomic_read(&ipu_dev->dev_state) != XH2A_IPU_DEV_CTC_RESETTING)
		ipu_dev->reset_owner = XH2A_IPU_RESET_OWNER_NONE;
	if (ipu_dev->reset_owner != XH2A_IPU_RESET_OWNER_NONE) {
		if (owner == XH2A_IPU_RESET_OWNER_IOCTL &&
		    ipu_dev->reset_owner == XH2A_IPU_RESET_OWNER_AUTO) {
			if (ipu_dev->reset_local_done) {
				ret = ipu_dev->reset_result;
				goto unlock_admission;
			}
			INIT_LIST_HEAD(&waiter.node);
			init_completion(&waiter.done);
			list_add_tail(&waiter.node, &ipu_dev->reset_waiter_list);
			mutex_unlock(&ipu_dev->ctc_claim_mutex);
			mutex_unlock(&ipu_dev->reset_mutex);
			wait_for_completion(&waiter.done);
			return waiter.result;
		}
		if (ipu_dev->reset_owner == owner) {
			ret = ipu_dev->reset_local_done ? ipu_dev->reset_result :
				  -EBUSY;
			goto unlock_admission;
		}
		ret = owner == XH2A_IPU_RESET_OWNER_IOCTL ? -EBUSY : 0;
		goto unlock_admission;
	}
	if (atomic_read(&ipu_dev->dev_state) == XH2A_IPU_DEV_FATAL) {
		ret = -EIO;
		goto unlock_admission;
	}
	if (atomic_read(&ipu_dev->dev_state) == XH2A_IPU_DEV_CTC_RESETTING) {
		ipu_dev->reset_owner = XH2A_IPU_RESET_OWNER_AUTO;
		ipu_dev->reset_local_done = true;
		ipu_dev->reset_result = 0;
		ret = 0;
		goto unlock_admission;
	}
	/* Claim the owner and transition the device state together.  No other
	 * reset path can observe an owner without RESETTING being established. */
	ret = xh2a_ipu_begin_reset(ipu_dev, XH2A_IPU_RESET_MODE_DISCARD);
	if (ret)
		goto unlock_admission;
	ipu_dev->reset_owner = owner;
	ipu_dev->reset_local_done = false;
	ipu_dev->reset_result = -EBUSY;
	mutex_unlock(&ipu_dev->ctc_claim_mutex);
	mutex_unlock(&ipu_dev->reset_mutex);

	ret = __xh2a_ipu_device_reset_discard(ipu_dev, true);
	mutex_lock(&ipu_dev->reset_mutex);
	ipu_dev->reset_result = ret;
	ipu_dev->reset_local_done = true;
	xh2a_ipu_complete_reset_waiters(ipu_dev, ret);
	mutex_unlock(&ipu_dev->reset_mutex);
	return ret;

unlock_admission:
	mutex_unlock(&ipu_dev->ctc_claim_mutex);
	mutex_unlock(&ipu_dev->reset_mutex);
	return ret;
}

static void xh2a_ipu_auto_reset_work(struct work_struct *work)
{
	int ret, pm_ret;
	struct delayed_work *dwork = to_delayed_work(work);
	struct xh2a_ipu_device *ipu_dev =
		container_of(dwork, struct xh2a_ipu_device, auto_reset_work);

	if (atomic_read(&ipu_dev->dev_removed) ||
	    atomic_read(&ipu_dev->is_reboot) ||
	    atomic_read(&ipu_dev->quiesce_flag) ||
	    atomic_read(&ipu_dev->dev_state) != XH2A_IPU_DEV_RESET_REQUIRED)
		return;

	pm_ret = pm_runtime_get_sync(ipu_dev->parent_dev);
	if (pm_ret < 0) {
		pm_runtime_put_noidle(ipu_dev->parent_dev);
		dev_warn(ipu_dev->cdev_dev,
			 "%s: pm_runtime_get_sync failed ret=%d dev_state=%d\n",
			 __func__, pm_ret, atomic_read(&ipu_dev->dev_state));
		return;
	}

	if (atomic_read(&ipu_dev->dev_removed) ||
	    atomic_read(&ipu_dev->is_reboot) ||
	    atomic_read(&ipu_dev->quiesce_flag) ||
	    atomic_read(&ipu_dev->dev_state) != XH2A_IPU_DEV_RESET_REQUIRED)
		goto out_pm_put;

	dev_warn(ipu_dev->cdev_dev,
		 "%s: reset source=auto_work start dev_state=%d reset=%d sync_cnt=%d\n",
		 __func__, atomic_read(&ipu_dev->dev_state),
		 atomic_read(&ipu_dev->reset_flag),
		 atomic_read(&ipu_dev->sync_cnt));

	ret = xh2a_ipu_device_reset_discard_owner(
		ipu_dev, XH2A_IPU_RESET_OWNER_AUTO);
	if (ret) {
		dev_warn(ipu_dev->cdev_dev,
			 "%s: auto reset failed ret=%d dev_state=%d reset=%d sync_cnt=%d\n",
			 __func__, ret, atomic_read(&ipu_dev->dev_state),
			 atomic_read(&ipu_dev->reset_flag),
			 atomic_read(&ipu_dev->sync_cnt));
		if (atomic_read(&ipu_dev->ctc_recovery_pending))
			xh2a_ipu_abort_ctc_recovery_fatal(ipu_dev);
	}

out_pm_put:
	pm_runtime_mark_last_busy(ipu_dev->parent_dev);
	pm_runtime_put_autosuspend(ipu_dev->parent_dev);
}

static void xh2a_ipu_ctc_recovery_retry_work(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct xh2a_ipu_device *ipu_dev = container_of(
		dwork, struct xh2a_ipu_device, ctc_recovery_retry_work);
	int ret;

	if (atomic_read(&ipu_dev->dev_removed) ||
	    atomic_read(&ipu_dev->is_reboot) ||
	    atomic_read(&ipu_dev->quiesce_flag) ||
	    atomic_read(&ipu_dev->dev_state) == XH2A_IPU_DEV_FATAL ||
	    !atomic_read(&ipu_dev->ctc_timeout_claimed))
		return;

	/*
	 * Retry stays local: a process-wide ownership mask cannot identify
	 * the participants in a single job.
	 */
	xh2a_ipu_ctc_release_standalone_timeout_claim(ipu_dev);
	ret = xh2a_ipu_enter_ctc_recovery_reset(ipu_dev);
	if (ret)
		dev_warn(ipu_dev->cdev_dev,
			 "%s: timeout reset fallback failed ret=%d dev_state=%d\n",
			 __func__, ret, atomic_read(&ipu_dev->dev_state));
}

void xh2a_ipu_schedule_auto_reset(struct xh2a_ipu_device *ipu_dev)
{
	if (!ipu_dev || !ipu_dev->reset_wq)
		return;

	if (atomic_read(&ipu_dev->dev_removed) ||
	    atomic_read(&ipu_dev->is_reboot) ||
	    atomic_read(&ipu_dev->quiesce_flag) ||
	    atomic_read(&ipu_dev->dev_state) != XH2A_IPU_DEV_RESET_REQUIRED)
		return;

	if (queue_delayed_work(ipu_dev->reset_wq, &ipu_dev->auto_reset_work,
			       msecs_to_jiffies(XH2A_IPU_AUTO_RESET_DELAY_MS)))
		dev_warn(ipu_dev->cdev_dev,
			 "%s: auto reset scheduled after %u ms\n", __func__,
			 XH2A_IPU_AUTO_RESET_DELAY_MS);
}

void xh2a_ipu_defer_ctc_timeout_recovery(struct xh2a_ipu_device *ipu_dev)
{
	if (!ipu_dev || !ipu_dev->reset_wq)
		return;

	if (atomic_read(&ipu_dev->dev_removed) ||
	    atomic_read(&ipu_dev->is_reboot) ||
	    atomic_read(&ipu_dev->quiesce_flag) ||
	    atomic_read(&ipu_dev->dev_state) == XH2A_IPU_DEV_FATAL)
		return;

	mod_delayed_work(ipu_dev->reset_wq, &ipu_dev->ctc_recovery_retry_work,
			 msecs_to_jiffies(XH2A_IPU_CTC_RECOVERY_RETRY_MS));
}

static void xh2a_ipu_cancel_file_handle_groups(struct xh2a_ipu_device *ipu_dev)
{
	struct xh2a_ipu_file_handle *fh_pos, *fh_n;

	mutex_lock(&ipu_dev->fh_mutex);
	list_for_each_entry_safe(fh_pos, fh_n, &ipu_dev->file_handle_list, node)
		xh2a_ipu_file_handle_stop_group(fh_pos);
	mutex_unlock(&ipu_dev->fh_mutex);
}

static void xh2a_ipu_drain_booter_queue_groups(struct xh2a_ipu_device *ipu_dev)
{
	int core_id, tile_id;
	struct xh2a_ipu_booter_queue *queue;
	struct xh2a_ipu_group *group;

	for (core_id = 0; core_id < XH2A_IPU_CORE_NUM; core_id++) {
		for (tile_id = 0; tile_id < XH2A_TILE_NUM_PER_CORE; tile_id++) {
			queue = &ipu_dev->tile_queues[core_id][tile_id];

			mutex_lock(&queue->tile_mutex);
			while (!list_empty(&queue->group_list)) {
				group = list_first_entry(&queue->group_list,
							 struct xh2a_ipu_group,
							 tile_list_node);
				xh2a_ipu_group_get(group);
				list_del_init(&group->tile_list_node);
				xh2a_ipu_group_put(group);
				mutex_unlock(&queue->tile_mutex);

				xh2a_ipu_group_cancel(group);
				xh2a_ipu_group_put(group);

				mutex_lock(&queue->tile_mutex);
			}
			mutex_unlock(&queue->tile_mutex);
		}
		mutex_lock(&ipu_dev->core_exec_mutex[core_id]);
		group = ipu_dev->core_tail_group[core_id];
		ipu_dev->core_tail_group[core_id] = NULL;
		mutex_unlock(&ipu_dev->core_exec_mutex[core_id]);
		if (group)
			xh2a_ipu_group_put(group);
	}
}

static void xh2a_ipu_quiesce_exec_groups(struct xh2a_ipu_device *ipu_dev)
{
	int ret;

	atomic_set(&ipu_dev->quiesce_flag, 1);
	wake_up_all(&ipu_dev->group_sync_wq);

	if (ipu_dev->group_work_inited)
		xh2a_ipu_cancel_async_works(ipu_dev);

	xh2a_ipu_drain_pending_events(ipu_dev);
	xh2a_ipu_cancel_file_handle_groups(ipu_dev);
	xh2a_ipu_drain_booter_queue_groups(ipu_dev);

	ret = wait_event_timeout(
		ipu_dev->group_sync_wq, atomic_read(&ipu_dev->sync_cnt) == 0,
		msecs_to_jiffies(XH2A_IPU_PM_QUIESCE_TIMEOUT_MS));
	if (ret == 0 && atomic_read(&ipu_dev->sync_cnt) != 0)
		dev_warn(ipu_dev->cdev_dev,
			 "%s: wait sync_cnt timeout, sync_cnt=%d\n", __func__,
			 atomic_read(&ipu_dev->sync_cnt));
}

static void xh2a_ipu_destroy_workqueue(struct workqueue_struct **wq)
{
	if (!wq || !*wq)
		return;

	destroy_workqueue(*wq);
	*wq = NULL;
}

static void xh2a_ipu_destroy_workqueues(struct xh2a_ipu_device *ipu_dev)
{
	xh2a_ipu_destroy_workqueue(&ipu_dev->reset_wq);
	xh2a_ipu_destroy_workqueue(&ipu_dev->load_wq);
	xh2a_ipu_destroy_workqueue(&ipu_dev->intr_wq);
	xh2a_ipu_destroy_workqueue(&ipu_dev->sched_wq);
}

static void xh2a_ipu_device_safe_release(struct kref *kref)
{
	struct xh2a_ipu_device *ipu_dev =
		container_of(kref, struct xh2a_ipu_device, dev_refcnt);

	kfree(ipu_dev);
}

static int xh2a_ipu_get_drvdata(void *drvdata)
{
	struct xh2a_ipu_device *ipu_dev = drvdata;

	if (atomic_read(&ipu_dev->dev_initialized) == 0) {
		pr_err("%s: device not initialized\n", __func__);
		return -EAGAIN;
	}

	if (atomic_read(&ipu_dev->dev_removed)) {
		pr_err("%s: device mark removed\n", __func__);
		return -ENODEV;
	}

	if (kref_get_unless_zero(&ipu_dev->dev_refcnt) == 0) {
		pr_err("%s: device is removing\n", __func__);
		return -ENODEV;
	}

	return 0;
}

int xh2a_ipu_prepare_fullchip_reset(void *handle)
{
	struct xh2a_pcie_client *client = NULL;
	struct xh2a_ipu_device *ipu_dev;

	xh2a_pcie_get_client(handle, &client, XH2A_IPU_DEVICE_NAME);

	if (!client)
		return 0;

	ipu_dev = client->client_data;
	if (!ipu_dev)
		return -EINVAL;

	if (atomic_read(&ipu_dev->dev_removed))
		return 0;

	atomic_set(&ipu_dev->is_reboot, 1);
	xh2a_ipu_cancel_auto_reset(ipu_dev);
	xh2a_ipu_load_stop(ipu_dev);

	return 0;
}

void xh2a_ipu_abort_fullchip_reset(void *handle)
{
	struct xh2a_pcie_client *client = NULL;
	struct xh2a_ipu_device *ipu_dev;

	xh2a_pcie_get_client(handle, &client, XH2A_IPU_DEVICE_NAME);

	if (!client)
		return;

	ipu_dev = client->client_data;
	if (!ipu_dev || atomic_read(&ipu_dev->dev_removed))
		return;

	atomic_set(&ipu_dev->is_reboot, 0);
	xh2a_ipu_load_start(ipu_dev);
}

static int xh2a_ipu_open(struct inode *inode, struct file *filp)
{
	struct xh2a_ipu_device *ipu_dev = NULL;
	struct xh2a_ipu_file_handle *file_handle = NULL;
	int ret;

	ret = xh2a_chrdev_get_drvdata(inode, xh2a_ipu_get_drvdata,
				      (void **)&ipu_dev);
	if (ret)
		return ret;

	file_handle = xh2a_ipu_file_handle_create(ipu_dev);
	if (IS_ERR_OR_NULL(file_handle)) {
		dev_err(ipu_dev->cdev_dev,
			"%s: xh2a_ipu_file_handle_create failed\n", __func__);
		kref_put(&ipu_dev->dev_refcnt, xh2a_ipu_device_safe_release);
		return -ENOMEM;
	}

	filp->private_data = file_handle;

	return 0;
}

static int xh2a_ipu_release(struct inode *inode, struct file *filp)
{
	struct xh2a_ipu_file_handle *file_handle = filp->private_data;
	struct xh2a_ipu_device *ipu_dev = NULL;

	filp->private_data = NULL;

	if (!file_handle)
		return 0;

	ipu_dev = file_handle->ipu_dev;

	xh2a_ipu_file_handle_destroy(file_handle);

	if (ipu_dev)
		kref_put(&ipu_dev->dev_refcnt, xh2a_ipu_device_safe_release);

	return 0;
}

static long xh2a_ipu_ioctl(struct file *filp, unsigned int cmd,
			   unsigned long arg)
{
	return xh2a_ipu_ioctl_dispatch(filp, cmd, arg);
}

static const struct file_operations xh2a_ipu_fops = {
	.owner = THIS_MODULE,
	.open = xh2a_ipu_open,
	.release = xh2a_ipu_release,
	.unlocked_ioctl = xh2a_ipu_ioctl,
};

static ssize_t device_id_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	struct xh2a_ipu_device *pipu = dev_get_drvdata(dev);
	return XH2A_SYSFS_EMIT(buf, "%d\n",
			       xh2a_pcie_device_index(pipu->private_data));
}
static DEVICE_ATTR_RO(device_id);

static ssize_t device_dbdf_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	uint32_t dbdf = 0;
	struct xh2a_ipu_device *pipu = dev_get_drvdata(dev);

	xh2a_pcie_device_dbdf(pipu->private_data, &dbdf);
	return XH2A_SYSFS_EMIT(buf, "%04x:%02x:%02x.%02x\n",
			       (dbdf >> 16) & 0xffff, (dbdf >> 8) & 0xff,
			       (dbdf >> 3) & 0x1f, dbdf & 0x7);
}
static DEVICE_ATTR_RO(device_dbdf);

static struct attribute *xh2a_ipu_info_node_attrs[] = {
	&dev_attr_device_id.attr,
	&dev_attr_device_dbdf.attr,
	NULL,
};
ATTRIBUTE_GROUPS(xh2a_ipu_info_node);

static int xh2a_ipu_spm_save_snapshot(struct xh2a_ipu_device *ipu_dev)
{
	int ret;

	/*save spm. TODO. add efuse detect which core is good*/
	if (ipu_dev->available_tiles_mask & 0xf) {
		ret = xh2a_pcie_dma_read_mem(ipu_dev->private_data,
					     XH2A_DEVICE_SPM0_START,
					     ipu_dev->spm_snapshot_buf[0],
					     XH2A_DEVICE_SPM0_SIZE);

		if (ret != 0) {
			dev_err(ipu_dev->cdev_dev, "%s: dma read fail\n",
				__func__);
			return ret;
		}
	}

	if (ipu_dev->available_tiles_mask & 0xf0) {
		ret = xh2a_pcie_dma_read_mem(ipu_dev->private_data,
					     XH2A_DEVICE_SPM1_START,
					     ipu_dev->spm_snapshot_buf[1],
					     XH2A_DEVICE_SPM1_SIZE);

		if (ret != 0) {
			dev_err(ipu_dev->cdev_dev, "%s: dma read fail\n",
				__func__);
			return ret;
		}
	}
	return 0;
}

static int xh2a_ipu_spm_restore_snapshot(struct xh2a_ipu_device *ipu_dev)
{
	int ret;

	/*save spm. TODO. add efuse detect which core is good*/
	if (ipu_dev->available_tiles_mask & 0xf) {
		ret = xh2a_pcie_dma_write_mem(ipu_dev->private_data,
					      XH2A_DEVICE_SPM0_START,
					      ipu_dev->spm_snapshot_buf[0],
					      XH2A_DEVICE_SPM0_SIZE);

		if (ret != 0) {
			dev_err(ipu_dev->cdev_dev, "%s: dma write fail\n",
				__func__);
			return ret;
		}
	}

	if (ipu_dev->available_tiles_mask & 0xf0) {
		ret = xh2a_pcie_dma_write_mem(ipu_dev->private_data,
					      XH2A_DEVICE_SPM1_START,
					      ipu_dev->spm_snapshot_buf[1],
					      XH2A_DEVICE_SPM1_SIZE);

		if (ret != 0) {
			dev_err(ipu_dev->cdev_dev, "%s: dma write fail\n",
				__func__);
			return ret;
		}
	}
	return 0;
}

/*
 * xh2a_ipu_pm_notifier_prepare()
 * 	- before pm prepare the ipu device
 * @handle: pcie handle
 * Return: 0 on success, negative error code on failure
 */
static int xh2a_ipu_pm_notifier_prepare(void *handle, bool rollback)
{
	struct xh2a_pcie_client *client = NULL;
	struct xh2a_ipu_device *ipu_dev;

	xh2a_pcie_get_client(handle, &client, XH2A_IPU_DEVICE_NAME);

	if (client == NULL) {
		pr_debug("%s: client not found\n", __func__);
		return 0;
	}

	ipu_dev = client->client_data;

	if (rollback) {
		atomic_set(&ipu_dev->quiesce_flag, 0);
		xh2a_ipu_load_start(ipu_dev);

		if (atomic_dec_and_test(&ipu_dev->block_ioctl_flag))
			wake_up_interruptible(&ipu_dev->block_ioctl_wq);
	} else {
		atomic_inc(&ipu_dev->block_ioctl_flag);
		xh2a_ipu_cancel_auto_reset(ipu_dev);
		xh2a_ipu_load_stop(ipu_dev);
		xh2a_ipu_quiesce_exec_groups(ipu_dev);
	}

	return 0;
}

/*
 * xh2a_ipu_pm_notifier_complete()
 * 	- after pm complete the ipu device
 * @handle: pcie handle
 * Return: 0 on success, negative error code on failure
 */
static int xh2a_ipu_pm_notifier_complete(void *handle)
{
	struct xh2a_pcie_client *client = NULL;
	struct xh2a_ipu_device *ipu_dev;

	xh2a_pcie_get_client(handle, &client, XH2A_IPU_DEVICE_NAME);

	if (client == NULL) {
		pr_debug("%s: client not found\n", __func__);
		return 0;
	}

	ipu_dev = client->client_data;

	atomic_set(&ipu_dev->is_reboot, 0);
	atomic_set(&ipu_dev->quiesce_flag, 0);
	xh2a_ipu_load_start(ipu_dev);
	if (atomic_dec_and_test(&ipu_dev->block_ioctl_flag))
		wake_up_interruptible(&ipu_dev->block_ioctl_wq);

	return 0;
}

/*
 * xh2a_ipu_pm_prepare()
 * 	- pm prepare the ipu device
 * @handle: pcie handle
 * Return: 0 on success, negative error code on failure
 */
static int xh2a_ipu_pm_prepare(void *handle, bool is_compatible)
{
	struct xh2a_pcie_client *client = NULL;
	struct xh2a_ipu_device *ipu_dev;
	struct xh2a_ipu_policy *old = NULL;
	int ret;
	struct xh2a_ipu_file_handle *fh_pos, *fh_n;

	(void)is_compatible;
	xh2a_pcie_get_client(handle, &client, XH2A_IPU_DEVICE_NAME);

	if (client == NULL) {
		pr_err("%s: Failed to get client for name '%s'\n", __func__,
		       XH2A_IPU_DEVICE_NAME);
		return -EINVAL;
	}

	ipu_dev = client->client_data;

	if (ipu_dev == NULL) {
		pr_err("%s: Client data is NULL for '%s'\n", __func__,
		       XH2A_IPU_DEVICE_NAME);
		return -EINVAL;
	}

	if (ipu_dev->group_work_inited)
		xh2a_ipu_cancel_async_works(ipu_dev);
	xh2a_ipu_cancel_auto_reset(ipu_dev);

	ret = xh2a_ipu_spm_save_snapshot(ipu_dev);
	if (ret)
		return ret;
	xh2a_ipu_hw_shutdown(ipu_dev, false);
	xh2a_ipu_ctc_pm_shutdown(ipu_dev);

	mutex_lock(&ipu_dev->dev_mutex);
	old = ipu_dev->policy;
	ipu_dev->policy = NULL;
	mutex_unlock(&ipu_dev->dev_mutex);

	if (old) {
		xh2a_ipu_policy_cancel_all(old);
		xh2a_ipu_policy_destroy(old);
	}

	/* if not s2idle, mark all fd to invald */
	mutex_lock(&ipu_dev->fh_mutex);

	if (pm_suspend_target_state != PM_SUSPEND_TO_IDLE)
		list_for_each_entry_safe(fh_pos, fh_n,
					 &ipu_dev->file_handle_list, node) {
			atomic_set(&fh_pos->drop_flag, 1);
		}

	mutex_unlock(&ipu_dev->fh_mutex);
	return 0;
}

/*
 * xh2a_ipu_pm_complete()
 * 	- pm complete the ipu device
 * @handle: pcie handle
 * Return: 0 on success, negative error code on failure
 */
static int xh2a_ipu_pm_complete(void *handle, bool is_compatible)
{
	struct xh2a_pcie_client *client = NULL;
	struct xh2a_ipu_device *ipu_dev;
	int ret;

	(void)is_compatible;
	xh2a_pcie_get_client(handle, &client, XH2A_IPU_DEVICE_NAME);

	if (client == NULL) {
		pr_err("%s: Failed to get client for name '%s'\n", __func__,
		       XH2A_IPU_DEVICE_NAME);
		return -EINVAL;
	}

	ipu_dev = client->client_data;

	if (ipu_dev == NULL) {
		pr_err("%s: Client data is NULL for '%s'\n", __func__,
		       XH2A_IPU_DEVICE_NAME);
		return -EINVAL;
	}

	mutex_lock(&ipu_dev->dev_mutex);
	ipu_dev->policy = xh2a_ipu_policy_create(ipu_dev);
	ret = ipu_dev->policy ? 0 : -ENOMEM;
	mutex_unlock(&ipu_dev->dev_mutex);
	if (ret)
		return ret;

	xh2a_ipu_hw_startup(ipu_dev, false);
	ret = xh2a_ipu_spm_restore_snapshot(ipu_dev);
	if (ret)
		return ret;
	ret = xh2a_ipu_ctc_pm_startup(ipu_dev);
	if (ret)
		return ret;

	/* The notifier commits user admission after every client recovered. */

	return 0;
}

/*
 * xh2a_ipu_pm_runtime_suspend()
 * 	- pm runtime suspend the ipu device
 * @handle: pcie handle
 * Return: 0 on success, negative error code on failure
 */
static int xh2a_ipu_pm_runtime_suspend(void *handle)
{
	struct xh2a_pcie_client *client = NULL;
	struct xh2a_ipu_device *ipu_dev;
	int ret;

	xh2a_pcie_get_client(handle, &client, XH2A_IPU_DEVICE_NAME);

	if (client == NULL) {
		pr_err("%s: Failed to get client for name '%s'\n", __func__,
		       XH2A_IPU_DEVICE_NAME);
		return -EINVAL;
	}

	ipu_dev = client->client_data;

	if (ipu_dev == NULL) {
		pr_err("%s: Client data is NULL for '%s'\n", __func__,
		       XH2A_IPU_DEVICE_NAME);
		return -EINVAL;
	}

	xh2a_ipu_load_stop(ipu_dev);
	if (ipu_dev->group_work_inited)
		xh2a_ipu_cancel_async_works(ipu_dev);
	xh2a_ipu_cancel_auto_reset(ipu_dev);

	ret = xh2a_ipu_spm_save_snapshot(ipu_dev);
	if (ret)
		return ret;
	xh2a_ipu_hw_shutdown(ipu_dev, false);
	xh2a_ipu_ctc_pm_shutdown(ipu_dev);

	return 0;
}

/*
 * xh2a_ipu_pm_runtime_resume()
 * 	- pm runtime resume the ipu device
 * @handle: pcie handle
 * Return: 0 on success, negative error code on failure
 */
static int xh2a_ipu_pm_runtime_resume(void *handle)
{
	struct xh2a_pcie_client *client = NULL;
	struct xh2a_ipu_device *ipu_dev;
	int ret;

	xh2a_pcie_get_client(handle, &client, XH2A_IPU_DEVICE_NAME);

	if (client == NULL) {
		pr_err("%s: Failed to get client for name '%s'\n", __func__,
		       XH2A_IPU_DEVICE_NAME);
		return -EINVAL;
	}

	ipu_dev = client->client_data;

	if (ipu_dev == NULL) {
		pr_err("%s: Client data is NULL for '%s'\n", __func__,
		       XH2A_IPU_DEVICE_NAME);
		return -EINVAL;
	}

	xh2a_ipu_hw_startup(ipu_dev, false);
	ret = xh2a_ipu_spm_restore_snapshot(ipu_dev);
	if (ret)
		return ret;
	ret = xh2a_ipu_ctc_pm_startup(ipu_dev);
	if (ret) {
		dev_err(ipu_dev->cdev_dev, "%s: CTC runtime resume failed %d\n",
			__func__, ret);
		xh2a_ipu_hw_shutdown(ipu_dev, false);
		return ret;
	}

	xh2a_ipu_load_start(ipu_dev);

	return 0;
}

struct xh2a_ipu_device *xh2a_ipu_device_create(void *handle)
{
	int ret, i;
	uint32_t mask;
	struct xh2a_pcie_client *client = NULL;
	struct xh2a_ipu_device *ipu_dev = NULL;

	ipu_dev = kzalloc(sizeof(struct xh2a_ipu_device), GFP_KERNEL);

	if (!ipu_dev) {
		pr_err("%s: alloc ipu_dev failed\n", __func__);
		return NULL;
	}

	ipu_dev->private_data = handle;

	atomic_set(&ipu_dev->dev_initialized, 0);
	atomic_set(&ipu_dev->dev_removed, 0);
	kref_init(&ipu_dev->dev_refcnt);
	mutex_init(&ipu_dev->ctc_mutex);
	ipu_dev->ctc_chip_id = XH2A_CTC_UNINIT_VAL;
	ipu_dev->ctc_group_id = XH2A_CTC_UNINIT_VAL;
	ipu_dev->ctc_group_num = XH2A_CTC_UNINIT_VAL;
	ipu_dev->ctc_is_single = XH2A_CTC_UNINIT_VAL;
	ipu_dev->ctc_fail_info = 0;
	ipu_dev->ctc_needs_phy_prepare = false;
	atomic_set(&ipu_dev->ctc_reinit_inhibit, 0);

	memset(ipu_dev->name, 0, XH2A_IPU_NAME_LEN);
	snprintf(ipu_dev->name, XH2A_IPU_NAME_LEN, XH2A_IPU_DEVICE_NAME "%d",
		 xh2a_pcie_device_index(handle));

	ipu_dev->parent_dev = xh2a_pcie_device_ptr(handle);
	ipu_dev->cdev_dev = xh2a_chrdev_create(
		xh2a_ipu_class, ipu_dev->parent_dev,
		xh2a_chrdev_devt(handle, XH2A_CHRDEV_SUB_IPU), ipu_dev,
		&xh2a_ipu_fops, xh2a_ipu_info_node_groups, "xh2a_ipu%d",
		xh2a_pcie_device_index(handle));
	if (IS_ERR(ipu_dev->cdev_dev)) {
		ret = PTR_ERR(ipu_dev->cdev_dev);
		pr_err("%s: xh2a_chrdev_create failed\n", __func__);
		goto err_chrdev_create;
	}

	client = &ipu_dev->client;
	snprintf(client->name, XH2A_PCIE_CLIENT_NAME_LEN, XH2A_IPU_DEVICE_NAME);

	xh2a_ipu_msi_work_register(ipu_dev);

	client->client_data = ipu_dev;
	client->private_data = handle;

	client->prepare_cb = xh2a_ipu_pm_prepare;
	client->complete_cb = xh2a_ipu_pm_complete;
	client->notifier_prepare_cb = xh2a_ipu_pm_notifier_prepare;
	client->notifier_complete_cb = xh2a_ipu_pm_notifier_complete;
	atomic_set(&ipu_dev->block_ioctl_flag, 0);
	atomic_set(&ipu_dev->quiesce_flag, 0);
	init_waitqueue_head(&ipu_dev->block_ioctl_wq);
	client->runtime_suspend_cb = xh2a_ipu_pm_runtime_suspend;
	client->runtime_resume_cb = xh2a_ipu_pm_runtime_resume;

	ipu_dev->spm_snapshot_buf[0] = vmalloc(XH2A_DEVICE_SPM0_SIZE);
	ipu_dev->spm_snapshot_buf[1] = vmalloc(XH2A_DEVICE_SPM1_SIZE);
	if (ipu_dev->spm_snapshot_buf[0] == NULL ||
	    ipu_dev->spm_snapshot_buf[1] == NULL) {
		dev_err(ipu_dev->cdev_dev, "%s: Failed to allocate memory\n",
			__func__);
		goto err_spm;
	}

	atomic_set(&ipu_dev->reset_flag, 0);
	atomic_set(&ipu_dev->dev_state, XH2A_IPU_DEV_READY);
	atomic_set(&ipu_dev->active_reset_mode, XH2A_IPU_RESET_MODE_DISCARD);
	atomic_set(&ipu_dev->sync_cnt, 0);
	atomic_set(&ipu_dev->ctc_recovery_pending, 0);
	atomic_set(&ipu_dev->ctc_recovery_local_reset_done, 0);
	mutex_init(&ipu_dev->ctc_claim_mutex);
	atomic_set(&ipu_dev->ctc_timeout_claimed, 0);
	ipu_dev->ctc_timeout_group = NULL;
	ipu_dev->ctc_timeout_previous_status = GROUP_DONE;
	ipu_dev->ctc_timeout_publishing = false;
	init_waitqueue_head(&ipu_dev->group_sync_wq);

	ipu_dev->sched_wq = xh2a_ipu_create_singlethread_wq(ipu_dev, "sched");
	if (!ipu_dev->sched_wq) {
		dev_err(ipu_dev->cdev_dev, "create sched workqueue failed\n");
		goto err_spm;
	}

	ipu_dev->intr_wq = xh2a_ipu_create_singlethread_wq(ipu_dev, "intr");
	if (!ipu_dev->intr_wq) {
		dev_err(ipu_dev->cdev_dev, "create intr workqueue failed\n");
		goto err_workqueue;
	}

	ipu_dev->load_wq = xh2a_ipu_create_singlethread_wq(ipu_dev, "load");
	if (!ipu_dev->load_wq) {
		dev_err(ipu_dev->cdev_dev, "create load workqueue failed\n");
		goto err_workqueue;
	}

	ipu_dev->reset_wq = xh2a_ipu_create_singlethread_wq(ipu_dev, "reset");
	if (!ipu_dev->reset_wq) {
		dev_err(ipu_dev->cdev_dev, "create reset workqueue failed\n");
		goto err_workqueue;
	}

	ipu_dev->last_core = -1;
	ipu_dev->policy = xh2a_ipu_policy_create(ipu_dev);
	if (!ipu_dev->policy)
		goto err_workqueue;

	ret = xh2a_ipu_get_efuse_info(ipu_dev);
	if (ret != 0 || ipu_dev->available_tiles_mask == 0) {
		/* To do : Adapt chip without burned efuse */
		ipu_dev->available_tiles_mask = 0xFF;
		ipu_dev->available_cores_num = 0x2;
	}

	INIT_WORK(&ipu_dev->group_sche_work, xh2a_device_schedule_work);
	INIT_DELAYED_WORK(&ipu_dev->group_sche_retry_work,
			  xh2a_device_schedule_retry_work);
	INIT_DELAYED_WORK(&ipu_dev->auto_reset_work, xh2a_ipu_auto_reset_work);
	INIT_DELAYED_WORK(&ipu_dev->ctc_recovery_retry_work,
			  xh2a_ipu_ctc_recovery_retry_work);
	INIT_WORK(&ipu_dev->ipu_event_work, xh2a_ipu_event_work);
	INIT_WORK(&ipu_dev->ipu_load_work, xh2a_ipu_load_work);
	for (i = 0; i < XH2A_IPU_CORE_NUM; i++) {
		mask = (0xF << (i * 4));
		if ((ipu_dev->available_tiles_mask & mask) != mask)
			continue;

		ipu_dev->core_works[i].msi_id = i;
		INIT_WORK(&ipu_dev->core_works[i].work,
			  xh2a_ipuss_core_bh_work);
	}

	ipu_dev->group_work_inited = true;

	mutex_init(&ipu_dev->dev_mutex);
	mutex_init(&ipu_dev->fh_mutex);
	mutex_init(&ipu_dev->reset_mutex);
	ipu_dev->reset_owner = XH2A_IPU_RESET_OWNER_NONE;
	ipu_dev->reset_local_done = false;
	ipu_dev->reset_result = 0;
	INIT_LIST_HEAD(&ipu_dev->reset_waiter_list);
	mutex_init(&ipu_dev->dump_mutex);
	for (i = 0; i < XH2A_IPU_CORE_NUM; i++)
		mutex_init(&ipu_dev->core_exec_mutex[i]);

	INIT_LIST_HEAD(&ipu_dev->file_handle_list);
	INIT_LIST_HEAD(&ipu_dev->isr_event_list);
	spin_lock_init(&ipu_dev->event_lock);

	ret = xh2a_pcie_register_client(handle, client);
	if (ret != 0) {
		pr_err("%s: register client failed %d\n", __func__, ret);
		goto err_register;
	}

	xh2a_ipu_booter_pre_startup(ipu_dev);

	xh2a_ipu_hw_startup(ipu_dev, false);

	xh2a_ipu_event_pool_create(&ipu_dev->event_pool);

	xh2a_ipu_load_init(ipu_dev);

	atomic_set(&ipu_dev->dev_initialized, 1);

	return ipu_dev;

err_register:
	xh2a_ipu_cancel_async_works(ipu_dev);
	xh2a_ipu_cancel_auto_reset(ipu_dev);
	ipu_dev->group_work_inited = false;
	xh2a_ipu_policy_destroy(ipu_dev->policy);

err_workqueue:
	xh2a_ipu_destroy_workqueues(ipu_dev);

err_spm:
	for (i = 0; i < XH2A_IPU_CORE_NUM; i++) {
		if (ipu_dev->spm_snapshot_buf[i])
			vfree(ipu_dev->spm_snapshot_buf[i]);
	}
	xh2a_ipu_msi_work_unregister(ipu_dev);
	xh2a_chrdev_destroy(xh2a_ipu_class,
			    xh2a_chrdev_devt(handle, XH2A_CHRDEV_SUB_IPU),
			    ipu_dev);

err_chrdev_create:
	kfree(ipu_dev);
	return NULL;
}

void xh2a_ipu_device_destroy(struct xh2a_ipu_device *ipu_dev)
{
	int i;
	struct xh2a_pcie_client *client = &ipu_dev->client;
	struct xh2a_ipu_file_handle *fh_pos, *fh_n;
	struct xh2a_ipu_event *event, *event_n;
	LIST_HEAD(event_list);
	unsigned long flags;
	struct xh2a_ipu_policy *old = NULL;

	if (!ipu_dev)
		return;

	atomic_set(&ipu_dev->dev_removed, 1);
	xh2a_ipu_cancel_auto_reset(ipu_dev);

	/*
	 * Cancel and drain all groups while IPU-local allocators and hardware
	 * state are still valid. This also makes later file close safe.
	 */
	xh2a_ipu_quiesce_exec_groups(ipu_dev);

	/*
	 * Destroy the chrdev (no new opens, dev_removed already set) and
	 * unregister from PCIe *before* tearing down internal state that
	 * in-flight ioctls may still be touching. The kref keeps the struct
	 * itself alive for any open fd until its release().
	 */
	xh2a_chrdev_destroy(xh2a_ipu_class,
			    xh2a_chrdev_devt(client->private_data,
					     XH2A_CHRDEV_SUB_IPU),
			    ipu_dev);

	xh2a_pcie_unregister_client(client->private_data, client);

	xh2a_ipu_msi_work_unregister(ipu_dev);

	mutex_lock(&ipu_dev->fh_mutex);
	list_for_each_entry_safe(fh_pos, fh_n, &ipu_dev->file_handle_list,
				 node) {
		list_del_init(&fh_pos->node);
		xh2a_ipu_file_handle_stop_group(fh_pos);
	}
	mutex_unlock(&ipu_dev->fh_mutex);

	if (ipu_dev->group_work_inited) {
		xh2a_ipu_cancel_async_works(ipu_dev);
		ipu_dev->group_work_inited = false;
	}

	spin_lock_irqsave(&ipu_dev->event_lock, flags);
	list_splice_init(&ipu_dev->isr_event_list, &event_list);
	spin_unlock_irqrestore(&ipu_dev->event_lock, flags);

	list_for_each_entry_safe(event, event_n, &event_list, node) {
		list_del_init(&event->node);
		xh2a_ipu_event_free(&ipu_dev->event_pool, event);
	}

	xh2a_ipu_load_exit(ipu_dev);

	xh2a_ipu_hw_shutdown(ipu_dev, false);

	mutex_lock(&ipu_dev->dev_mutex);
	old = ipu_dev->policy;
	ipu_dev->policy = NULL;
	mutex_unlock(&ipu_dev->dev_mutex);

	if (old)
		xh2a_ipu_policy_destroy(old);

	xh2a_ipu_destroy_workqueues(ipu_dev);

	xh2a_ipu_event_pool_destroy(&ipu_dev->event_pool);

	for (i = 0; i < XH2A_IPU_CORE_NUM; i++) {
		if (ipu_dev->spm_snapshot_buf[i])
			vfree(ipu_dev->spm_snapshot_buf[i]);
	}

	kref_put(&ipu_dev->dev_refcnt, xh2a_ipu_device_safe_release);
}

/*
 * xh2a_ipu_probe() - init function for ipu device
 * @handle: handle of xh2a_pcie_dev
 * Return: 0 on success, negative error code on failure
 */
static int xh2a_ipu_probe(void *handle)
{
	struct xh2a_ipu_device *ipu_dev = NULL;

	pr_debug("%s:\n", __func__);

	ipu_dev = xh2a_ipu_device_create(handle);

	if (!ipu_dev) {
		pr_err("%s: xh2a_ipu device init failed\n", __func__);
		return -ENOMEM;
	}

	return 0;
}

/*
 * xh2a_ipu_remove() - remove the ipu device
 * @handle: pcie handle
 * Return: 0 on success, negative error code on failure
 */
static int xh2a_ipu_remove(void *handle)
{
	struct xh2a_pcie_client *client = NULL;
	struct xh2a_ipu_device *ipu_dev;

	pr_debug("%s:\n", __func__);

	xh2a_pcie_get_client(handle, &client, XH2A_IPU_DEVICE_NAME);

	if (client == NULL) {
		pr_err("%s: Failed to get client for name '%s'\n", __func__,
		       XH2A_IPU_DEVICE_NAME);
		return 0;
	}

	ipu_dev = client->client_data;

	xh2a_ipu_device_destroy(ipu_dev);

	return 0;
}

/*
 * xh2a_ipu_shutdown() - shutdown the ipu device
 * @handle: pcie handle
 * Return: 0 on success, negative error code on failure
 */
static int xh2a_ipu_shutdown(void *handle)
{
	struct xh2a_pcie_client *client = NULL;
	struct xh2a_ipu_device *ipu_dev;

	pr_debug("%s:\n", __func__);

	xh2a_pcie_get_client(handle, &client, XH2A_IPU_DEVICE_NAME);

	if (client == NULL) {
		pr_err("%s: Failed to get client for name '%s'\n", __func__,
		       XH2A_IPU_DEVICE_NAME);
		return 0;
	}

	ipu_dev = client->client_data;

	xh2a_ipu_load_exit(ipu_dev);

	return 0;
}

/*
 * xh2a_ipu_notifier_call() - called when the pcie notifies this driver
 * @nb: notifier block
 * @event: event type
 * @data: data from notifier
 * Return:
 *  = NOTIFY_OK - Success
 */
static int xh2a_ipu_notifier_call(struct notifier_block *nb,
				  unsigned long event, void *data)
{
	int ret = 0;

	pr_debug("%s: event = %ld, data = %p\n", __func__, event, data);

	if (event == XH2A_HOST_NOTIFY_PCIE_PROBE)
		ret = xh2a_ipu_probe(data);
	else if (event == XH2A_HOST_NOTIFY_PCIE_REMOVE)
		ret = xh2a_ipu_remove(data);
	else if (event == XH2A_HOST_NOTIFY_PCIE_SHUTDOWN)
		ret = xh2a_ipu_shutdown(data);

	if (ret)
		return NOTIFY_STOP;

	return NOTIFY_OK;
}

/*
 * @notifier_call: called when the pcie notifies this driver
 */
static struct notifier_block xh2a_ipu_notifier_block = {
	.notifier_call = xh2a_ipu_notifier_call,
};

/*
 * xh2a_ipu_register_driver() - register ipu driver
 */
int __init xh2a_ipu_register_driver(void)
{
	pr_debug("%s:\n", __func__);
	xh2a_host_register_notifier_chain(&xh2a_ipu_notifier_block);
	return 0;
}

/*
 * xh2a_ipu_unregister_driver() - unregister ipu driver
 */
void __exit xh2a_ipu_unregister_driver(void)
{
	pr_debug("%s:\n", __func__);
	xh2a_host_unregister_notifier_chain(&xh2a_ipu_notifier_block);
}

MODULE_LICENSE("GPL");
