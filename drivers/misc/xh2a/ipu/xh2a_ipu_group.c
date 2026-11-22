// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2025 Houmo AI Inc.
 * Author: mingdong.wang<mingdong.wang@houmo.ai>
 */

#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/idr.h>
#include <linux/jiffies.h>
#include <linux/sched.h>

#include <xh2a_host_drv.h>
#include <xh2a_pcie_api.h>
#include <xh2a_spm_api.h>

#include "xh2a_ipu_file_handle.h"
#include "xh2a_ipu_device.h"
#include "xh2a_ipu_group.h"
#include "xh2a_ipu_kernel.h"
#include "xh2a_ipu_ioctl.h"
#include "xh2a_ipu_policy.h"

static void xh2a_ipu_group_sync_cnt_dec(struct xh2a_ipu_device *ipu_device)
{
	int sync_cnt = atomic_dec_return(&ipu_device->sync_cnt);

	dev_dbg(ipu_device->cdev_dev,
		 "%s: sync_cnt dec pid=%d sync_cnt=%d dev_state=%d reset=%d quiesce=%d\n",
		 __func__, current->pid, sync_cnt,
		 atomic_read(&ipu_device->dev_state),
		 atomic_read(&ipu_device->reset_flag),
		 atomic_read(&ipu_device->quiesce_flag));

	if (sync_cnt == 0)
		wake_up_all(&ipu_device->group_sync_wq);
}

static bool xh2a_ipu_group_terminal_status(int status)
{
	return status == GROUP_DONE || status == GROUP_TIMEOUT ||
	       status == GROUP_DESTROY || status == GROUP_CANCELED;
}

static bool xh2a_ipu_group_load_blocked(struct xh2a_ipu_device *ipu_device)
{
	return !xh2a_ipu_device_ready(ipu_device) ||
	       atomic_read(&ipu_device->reset_flag) ||
	       atomic_read(&ipu_device->quiesce_flag);
}

static bool xh2a_ipu_group_claim_timeout_owner(struct xh2a_ipu_device *ipu_device)
{
	return atomic_cmpxchg(&ipu_device->dev_state,
			      XH2A_IPU_DEV_READY,
			      XH2A_IPU_DEV_RESET_REQUIRED) ==
	       XH2A_IPU_DEV_READY;
}

static bool xh2a_ipu_group_reset_or_quiesce(struct xh2a_ipu_device *ipu_device)
{
	return atomic_read(&ipu_device->reset_flag) ||
	       atomic_read(&ipu_device->quiesce_flag) ||
	       atomic_read(&ipu_device->dev_state) == XH2A_IPU_DEV_CTC_RESETTING;
}

void xh2a_ipu_group_log_exec_time(struct xh2a_ipu_group *group,
				  const char *reason)
{
	struct xh2a_ipu_device *ipu_device;
	s64 now_us, total_us = -1, queue_wait_us = -1, exec_us = -1;
	s64 sync_start_us, load_start_us, exec_start_us;

	if (!group || !group->ipu_dev)
		return;

	ipu_device = group->ipu_dev;
	now_us = ktime_to_us(ktime_get());
	sync_start_us = ktime_to_us(group->sync_start);
	load_start_us = (s64)group->load_start;
	exec_start_us = ktime_to_us(group->exec_start);

	if (sync_start_us > 0)
		total_us = now_us - sync_start_us;

	if (load_start_us > 0 && exec_start_us > 0)
		queue_wait_us = exec_start_us - load_start_us;

	if (exec_start_us > 0)
		exec_us = now_us - exec_start_us;

	dev_dbg(ipu_device->cdev_dev,
		 "%s: group %u %p reason=%s status=%d total_us=%lld queue_wait_us=%lld exec_us=%lld kernels=%u cores=%u tiles=%u timeout_us=%llu sync_cnt=%d dev_state=%d reset=%d\n",
		 __func__, group->id, group, reason ? reason : "unknown",
		 atomic_read(&group->status), total_us, queue_wait_us, exec_us,
		 group->kernel_num, group->core_num, group->tile_num,
		 group->kernels_timeout, atomic_read(&ipu_device->sync_cnt),
		 atomic_read(&ipu_device->dev_state),
		 atomic_read(&ipu_device->reset_flag));
}

static unsigned long xh2a_ipu_group_timeout_jiffies(
						struct xh2a_ipu_group *group)
{
	return usecs_to_jiffies(group->kernels_timeout);
}

void xh2a_ipu_group_arm_timeout(struct xh2a_ipu_group *group,
						 ktime_t exec_start)
{
	bool put = false;

	if (!group)
		return;

	mutex_lock(&group->mutex);
	if (atomic_read(&group->status) == GROUP_RUNNING &&
	    !group->timeout_armed) {
		group->exec_start = exec_start;
		group->timeout_armed = true;
		xh2a_ipu_group_get(group);
		if (!schedule_delayed_work(&group->timeout_work,
					   xh2a_ipu_group_timeout_jiffies(group))) {
			group->timeout_armed = false;
			put = true;
		}
	}
	mutex_unlock(&group->mutex);

	if (put)
		xh2a_ipu_group_put(group);
}

void xh2a_ipu_group_disarm_timeout(struct xh2a_ipu_group *group)
{
	bool put = false;

	if (!group)
		return;

	mutex_lock(&group->mutex);
	if (group->timeout_armed) {
		group->timeout_armed = false;
		put = true;
	}
	mutex_unlock(&group->mutex);

	/*
	 * CTC recovery abort can cancel the group whose timeout work is currently
	 * establishing the recovery epoch.  Waiting synchronously for that same
	 * work would wait for the current worker to return and deadlock.
	 */
	if (current_work() == &group->timeout_work.work)
		cancel_delayed_work(&group->timeout_work);
	else
		cancel_delayed_work_sync(&group->timeout_work);

	if (put)
		xh2a_ipu_group_put(group);
}

void xh2a_ipu_group_exec_ready_for_trigger(struct xh2a_ipu_group *group,
						    ktime_t exec_start)
{
	bool ready;

	if (!group)
		return;

	mutex_lock(&group->mutex);
	ready = group->exec_deps_ready && group->exec_triggered &&
		atomic_read(&group->pending_exec_deps) == 0;
	mutex_unlock(&group->mutex);

	if (ready)
		xh2a_ipu_group_arm_timeout(group, exec_start);
}

void xh2a_ipu_group_add_exec_dependency(struct xh2a_ipu_group *group,
						 int core_id)
{
	struct xh2a_ipu_exec_dep *dep;
	struct xh2a_ipu_group *predecessor;
	struct xh2a_ipu_device *ipu_device;

	if (!group || core_id < 0 || core_id >= XH2A_IPU_CORE_NUM)
		return;

	ipu_device = group->ipu_dev;
	mutex_lock(&ipu_device->core_exec_mutex[core_id]);
	dep = &group->exec_deps[core_id];
	predecessor = ipu_device->core_tail_group[core_id];
	if (predecessor) {
		atomic_inc(&group->pending_exec_deps);
		mutex_lock(&predecessor->mutex);
		if (!predecessor->exec_waiters_released) {
			spin_lock(&dep->lock);
			dep->predecessor = predecessor;
			dep->linked = true;
			xh2a_ipu_group_get(group);
			xh2a_ipu_group_get(predecessor);
			list_add_tail(&dep->node, &predecessor->exec_waiters);
			spin_unlock(&dep->lock);
		} else {
			atomic_dec(&group->pending_exec_deps);
		}
		mutex_unlock(&predecessor->mutex);
	}

	xh2a_ipu_group_get(group);
	ipu_device->core_tail_group[core_id] = group;
	if (predecessor)
		xh2a_ipu_group_put(predecessor);
	mutex_unlock(&ipu_device->core_exec_mutex[core_id]);
}

void xh2a_ipu_group_release_exec_waiters(struct xh2a_ipu_group *group,
						  ktime_t exec_end)
{
	struct xh2a_ipu_exec_dep *dep, *next;
	LIST_HEAD(waiters);

	if (!group)
		return;

	mutex_lock(&group->mutex);
	if (group->exec_waiters_released) {
		mutex_unlock(&group->mutex);
		return;
	}
	group->exec_waiters_released = true;
	while (!list_empty(&group->exec_waiters)) {
		dep = list_first_entry(&group->exec_waiters,
					       struct xh2a_ipu_exec_dep, node);
		spin_lock(&dep->lock);
		list_del_init(&dep->node);
		dep->linked = false;
		dep->predecessor = NULL;
		spin_unlock(&dep->lock);
		list_add_tail(&dep->node, &waiters);
	}
	mutex_unlock(&group->mutex);

	list_for_each_entry_safe(dep, next, &waiters, node) {
		struct xh2a_ipu_group *waiter = dep->waiter;

		list_del_init(&dep->node);
		if (atomic_dec_and_test(&waiter->pending_exec_deps))
			xh2a_ipu_group_exec_ready_for_trigger(waiter, exec_end);
		xh2a_ipu_group_put(group);
		xh2a_ipu_group_put(waiter);
	}
}

void xh2a_ipu_group_drop_exec_deps(struct xh2a_ipu_group *group)
{
	int core_id;

	if (!group)
		return;

	for (core_id = 0; core_id < XH2A_IPU_CORE_NUM; core_id++) {
		struct xh2a_ipu_exec_dep *dep = &group->exec_deps[core_id];
		struct xh2a_ipu_group *predecessor;
		bool dropped = false;

		spin_lock(&dep->lock);
		predecessor = dep->predecessor;
		if (predecessor)
			xh2a_ipu_group_get(predecessor);
		spin_unlock(&dep->lock);
		if (!predecessor)
			continue;

		mutex_lock(&predecessor->mutex);
		spin_lock(&dep->lock);
		if (dep->linked && dep->predecessor == predecessor) {
			list_del_init(&dep->node);
			dep->linked = false;
			dep->predecessor = NULL;
			dropped = true;
		}
		spin_unlock(&dep->lock);
		mutex_unlock(&predecessor->mutex);

		if (dropped) {
			atomic_dec(&group->pending_exec_deps);
			xh2a_ipu_group_put(predecessor);
			xh2a_ipu_group_put(group);
		}
		xh2a_ipu_group_put(predecessor);
	}
}

void xh2a_ipu_group_clear_exec_tails(struct xh2a_ipu_group *group)
{
	int i, core_id;

	if (!group)
		return;

	for (i = 0; i < group->core_num; i++) {
		core_id = group->target[i].core_id;
		mutex_lock(&group->ipu_dev->core_exec_mutex[core_id]);
		if (group->ipu_dev->core_tail_group[core_id] == group) {
			group->ipu_dev->core_tail_group[core_id] = NULL;
			xh2a_ipu_group_put(group);
		}
		mutex_unlock(&group->ipu_dev->core_exec_mutex[core_id]);
	}
}

static bool xh2a_ipu_group_consume_timeout(struct xh2a_ipu_group *group)
{
	bool consumed = false;

	mutex_lock(&group->mutex);
	if (group->timeout_armed) {
		group->timeout_armed = false;
		consumed = true;
	}
	mutex_unlock(&group->mutex);

	return consumed;
}

static bool xh2a_ipu_group_hw_queue_pending(struct xh2a_ipu_group *group)
{
	struct xh2a_ipu_booter_queue *queue;
	bool pending;

	queue = group->queue;
	if (!queue)
		return false;

	mutex_lock(&queue->tile_mutex);
	pending = !list_empty(&group->tile_list_node);
	mutex_unlock(&queue->tile_mutex);

	return pending;
}

void xh2a_ipu_group_wait_load_done(struct xh2a_ipu_group *group)
{
	bool loading;

	if (!group)
		return;

	mutex_lock(&group->mutex);
	loading = group->loading;
	mutex_unlock(&group->mutex);

	if (loading)
		wait_for_completion(&group->load_completion);
}

static void xh2a_ipu_group_finish_load(struct xh2a_ipu_group *group)
{
	if (!group)
		return;

	mutex_lock(&group->mutex);
	if (group->loading) {
		group->loading = false;
		complete_all(&group->load_completion);
	}
	mutex_unlock(&group->mutex);
}

static bool xh2a_ipu_group_queue_space_available(struct xh2a_ipu_group *group)
{
	int i, j, core_id, queue_id;
	uint32_t space;
	struct xh2a_ipu_booter_queue *queue;
	struct xh2a_ipu_device *ipu_device;

	if (!group)
		return false;

	ipu_device = group->ipu_dev;

	for (i = 0; i < group->core_num; i++) {
		core_id = group->target[i].core_id;
		for (j = 0; j < group->tile_num; j++) {
			queue_id = group->target[i].queue_id + j;
			queue = &ipu_device->tile_queues[core_id][queue_id];

			mutex_lock(&queue->tile_mutex);
			space = xh2a_booter_queue_calc_space(
				queue->entry_capacity, queue->wptr, queue->rptr);
			WRITE_ONCE(queue->cached_wptr, queue->wptr);
			WRITE_ONCE(queue->cached_rptr, queue->rptr);
			mutex_unlock(&queue->tile_mutex);

			if (space < group->kernel_num) {
				dev_dbg(ipu_device->cdev_dev,
					"%s: group %u %p C%dT%d space=%u needed=%u\n",
					__func__, group->id, group, core_id,
					queue_id, space, group->kernel_num);
				return false;
			}
		}
	}

	return true;
}

static bool xh2a_ipu_group_requeue_pending(struct xh2a_ipu_group *group)
{
	bool requeued = false;
	struct xh2a_ipu_device *ipu_device;

	if (!group)
		return false;

	ipu_device = group->ipu_dev;

	mutex_lock(&ipu_device->dev_mutex);
	if (ipu_device->policy && xh2a_ipu_device_ready(ipu_device) &&
	    ipu_device->policy->ops->requeue_group_head &&
	    atomic_read(&group->status) == GROUP_PENDING) {
		ipu_device->policy->ops->requeue_group_head(ipu_device->policy,
							    group);
		requeued = true;
	}
	mutex_unlock(&ipu_device->dev_mutex);

	return requeued;
}

static void xh2a_ipu_group_release_sync_cnt(struct xh2a_ipu_group *group)
{
	struct xh2a_ipu_device *ipu_device;
	bool wake_sync = false;

	if (!group)
		return;

	ipu_device = group->ipu_dev;
	mutex_lock(&group->mutex);
	if (group->sync_cnt_acquired) {
		group->sync_cnt_acquired = false;
		wake_sync = true;
	}
	mutex_unlock(&group->mutex);

	if (wake_sync)
		xh2a_ipu_group_sync_cnt_dec(ipu_device);
}

void xh2a_ipu_group_complete_ctc_timeout(struct xh2a_ipu_group *group,
					 int previous_status)
{
	if (previous_status == GROUP_PENDING) {
		xh2a_ipu_policy_delete_group(group);
		xh2a_ipu_group_release_exec_resources(group);
	} else {
		/* Keep the running queue node and backing resources until reset
		 * drains the hardware queue, but release the synchronous waiter. */
		xh2a_ipu_group_release_sync_cnt(group);
	}
	complete_all(&group->sync_completion);
}

static void xh2a_ipu_group_abort_load(struct xh2a_ipu_group *group)
{
	int old_status;

	if (!group)
		return;

	do {
		old_status = atomic_read(&group->status);
		if (xh2a_ipu_group_terminal_status(old_status))
			break;
	} while (atomic_cmpxchg(&group->status, old_status, GROUP_CANCELED) !=
		 old_status);

	xh2a_ipu_group_release_exec_resources(group);
	complete_all(&group->sync_completion);
	xh2a_ipu_group_finish_load(group);
}

void xh2a_ipu_group_release_exec_resources(struct xh2a_ipu_group *group)
{
	struct xh2a_ipu_device *ipu_device;
	bool wake_sync = false;

	if (!group)
		return;

	ipu_device = group->ipu_dev;
	mutex_lock(&group->mutex);

	if (!group->resources_released) {
		xh2a_group_host_param_free(ipu_device, group);

		if (group->param_type == XH2A_GROUP_PARAM_SPM)
			xh2a_ipu_group_free_spm(group);

		group->resources_released = true;
	}

	if (group->sync_cnt_acquired) {
		group->sync_cnt_acquired = false;
		wake_sync = true;
	}

	mutex_unlock(&group->mutex);

	if (wake_sync)
		xh2a_ipu_group_sync_cnt_dec(ipu_device);
}

static void xh2a_ipu_group_unlink_tile_queue(struct xh2a_ipu_group *group)
{
	struct xh2a_ipu_booter_queue *queue;

	queue = group->queue;
	if (!queue)
		return;

	mutex_lock(&queue->tile_mutex);
	if (!list_empty(&group->tile_list_node)) {
		list_del_init(&group->tile_list_node);
		xh2a_ipu_group_put(group);
	}
	mutex_unlock(&queue->tile_mutex);
}

void xh2a_ipu_group_cancel(struct xh2a_ipu_group *group)
{
	struct xh2a_ipu_device *ipu_device;
	int old_status;

	if (!group)
		return;

	ipu_device = group->ipu_dev;

	do {
		old_status = atomic_read(&group->status);

		if (old_status == GROUP_DONE || old_status == GROUP_DESTROY)
			return;

		if (old_status == GROUP_CANCELED || old_status == GROUP_TIMEOUT)
			break;
	} while (atomic_cmpxchg(&group->status, old_status, GROUP_CANCELED) !=
		 old_status);

	/* A timeout claimed by the coordinator retains its timeout result until
	 * the all-participant publication barrier.  File close/remove may drop
	 * their own reference, but must not wake this waiter prematurely. */
	if (old_status == GROUP_TIMEOUT &&
	    xh2a_ipu_ctc_timeout_completion_deferred(ipu_device, group))
		return;

	if (old_status == GROUP_PENDING)
		xh2a_ipu_policy_delete_group(group);

	xh2a_ipu_group_wait_load_done(group);
	xh2a_ipu_group_log_exec_time(group, "cancel");

	xh2a_ipu_group_disarm_timeout(group);
	if (xh2a_ipu_group_reset_or_quiesce(ipu_device))
		xh2a_ipu_group_drop_exec_deps(group);

	if (xh2a_ipu_group_hw_queue_pending(group) &&
	    !xh2a_ipu_group_reset_or_quiesce(ipu_device)) {
		xh2a_ipu_group_release_sync_cnt(group);
		complete_all(&group->sync_completion);
		return;
	}

	xh2a_ipu_group_unlink_tile_queue(group);
	xh2a_ipu_group_release_exec_resources(group);
	complete_all(&group->sync_completion);
}

static void xh2a_ipu_group_timeout_work(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct xh2a_ipu_group *group =
		container_of(dwork, struct xh2a_ipu_group, timeout_work);
	struct xh2a_ipu_device *ipu_device = group->ipu_dev;
	struct device *cdev_dev = ipu_device->cdev_dev;

	int old_status;
	bool timeout_claimed;

	if (!xh2a_ipu_group_consume_timeout(group))
		return;

	dev_err(cdev_dev, "group %u %p exec timeout!\n", group->id, group);

	xh2a_ipu_group_wait_load_done(group);
	xh2a_ipu_group_log_exec_time(group, "timeout_work");

	old_status = atomic_read(&group->status);
	if (xh2a_ipu_group_terminal_status(old_status)) {
		xh2a_ipu_group_put(group);
		return;
	}

	if (xh2a_ipu_group_reset_or_quiesce(ipu_device)) {
		mutex_lock(&ipu_device->ctc_claim_mutex);
		if (atomic_cmpxchg(&group->status, old_status,
				   GROUP_CANCELED) != old_status) {
			mutex_unlock(&ipu_device->ctc_claim_mutex);
			xh2a_ipu_group_put(group);
			return;
		}
		mutex_unlock(&ipu_device->ctc_claim_mutex);

		xh2a_ipu_group_unlink_tile_queue(group);
		xh2a_ipu_group_release_exec_resources(group);
		complete_all(&group->sync_completion);
		xh2a_ipu_group_put(group);
		return;
	}

	/* Claim records retain the group reference and defer completion until the
	 * coordinator has claimed every participant. */
	timeout_claimed = xh2a_ipu_ctc_claim_timeout_group(ipu_device, group,
							    old_status) == 0;
	if (!timeout_claimed) {
		mutex_lock(&ipu_device->ctc_claim_mutex);
		if (atomic_cmpxchg(&group->status, old_status, GROUP_CANCELED) !=
		    old_status) {
			mutex_unlock(&ipu_device->ctc_claim_mutex);
			xh2a_ipu_group_put(group);
			return;
		}
		mutex_unlock(&ipu_device->ctc_claim_mutex);
	}

	if (timeout_claimed) {
		/*
		 * The SDK's group handle is per IPU file. A process can own
		 * unrelated cards, so its ownership mask cannot identify this
		 * job's peers. Recover only the IPU on which this group timed out.
		 */
		xh2a_ipu_group_claim_timeout_owner(ipu_device);
		xh2a_ipu_schedule_auto_reset(ipu_device);
		xh2a_ipu_ctc_publish_timeout(ipu_device);
		xh2a_ipu_ctc_release_standalone_timeout_claim(ipu_device);
	}

	if (!timeout_claimed)
		xh2a_ipu_group_complete_ctc_timeout(group, old_status);
	xh2a_ipu_group_put(group);
}

struct xh2a_ipu_group *
xh2a_ipu_group_create(struct xh2a_ipu_file_handle *file_handle)
{
	int ret, core_id;
	struct xh2a_ipu_group *group = NULL;

	struct xh2a_ipu_device *ipu_dev = file_handle->ipu_dev;
	struct device *cdev_dev = ipu_dev->cdev_dev;

	group = kzalloc(sizeof(struct xh2a_ipu_group), GFP_KERNEL);

	if (!group) {
		dev_err(cdev_dev, "%s: alloc group failed\n", __func__);
		return NULL;
	}

	ret = mutex_lock_interruptible(&file_handle->file_mutex);
	if (ret != 0) {
		dev_err(cdev_dev, "%s: mutex_lock_interruptible failed\n",
			__func__);
		kfree(group);
		return NULL;
	}

	group->id = idr_alloc_cyclic(&file_handle->group_idr, group, 0, 0,
				     GFP_KERNEL);

	if (group->id < 0) {
		mutex_unlock(&file_handle->file_mutex);
		dev_err(cdev_dev, "%s: idr_alloc group id failed\n", __func__);
		kfree(group);
		group = NULL;
		return NULL;
	}

	/* update file_handle min_id */
	if (group->id < file_handle->min_id)
		file_handle->min_id = group->id;

	mutex_unlock(&file_handle->file_mutex);

	mutex_init(&group->mutex);
	INIT_LIST_HEAD(&group->kernel_list);
	INIT_LIST_HEAD(&group->policy_list_node);
	group->policy_level = -1;
	INIT_LIST_HEAD(&group->tile_list_node);
	INIT_LIST_HEAD(&group->done_list_node);
	INIT_LIST_HEAD(&group->exec_waiters);
	for (core_id = 0; core_id < XH2A_IPU_CORE_NUM; core_id++) {
		struct xh2a_ipu_exec_dep *dep = &group->exec_deps[core_id];

		INIT_LIST_HEAD(&dep->node);
		dep->waiter = group;
		spin_lock_init(&dep->lock);
	}
	atomic_set(&group->pending_exec_deps, 0);
	init_completion(&group->sync_completion);
	init_completion(&group->load_completion);
	INIT_DELAYED_WORK(&group->timeout_work, xh2a_ipu_group_timeout_work);

	group->kernel_num = 0;
	group->file_handle = file_handle;
	group->ipu_dev = ipu_dev;
	atomic_set(&group->status, GROUP_CREATED);
	kref_init(&group->ref);

	dev_dbg(cdev_dev, "group %u %p created\n", group->id, group);

	return group;
}

static void xh2a_ipu_group_release(struct kref *kref)
{
	struct xh2a_ipu_kernel *kernel, *n;
	struct xh2a_ipu_group *group =
		container_of(kref, struct xh2a_ipu_group, ref);
	struct xh2a_ipu_device *ipu_dev = group->ipu_dev;
	struct device *cdev_dev = ipu_dev->cdev_dev;

	dev_dbg(cdev_dev, "group %u %p release\n", group->id, group);
	xh2a_ipu_group_drop_exec_deps(group);

	if (group->param_type == XH2A_GROUP_PARAM_SPM)
		xh2a_ipu_group_free_spm(group);

	list_for_each_entry_safe(kernel, n, &group->kernel_list, node) {
		list_del_init(&kernel->node);
		xh2a_ipu_kernel_destroy(kernel);
	}

	kfree(group);
}

void xh2a_ipu_group_get(struct xh2a_ipu_group *group)
{
	if (group)
		kref_get(&group->ref);
}

void xh2a_ipu_group_put(struct xh2a_ipu_group *group)
{
	if (group)
		kref_put(&group->ref, xh2a_ipu_group_release);
}

void xh2a_ipu_group_free_spm(struct xh2a_ipu_group *group)
{
	int idx, core_id;
	struct xh2a_ipu_device *ipu_dev = NULL;

	if (!group || !group->ipu_dev) {
		pr_err("%s: group is NULL\n", __func__);
		return;
	}

	ipu_dev = group->ipu_dev;

	for (idx = 0; idx < group->core_num; idx++) {
		core_id = group->target[idx].core_id;
		dev_dbg(group->ipu_dev->cdev_dev,
			"group %u %p free SPM%d: 0x%llx %u(0x%x)\n", group->id,
			group, core_id, group->param_addr[core_id],
			group->param_size, group->param_size);
		if (group->param_addr[core_id] && group->param_size) {
			xh2a_memory_allocator_kernel_free_spm(
				ipu_dev->private_data, core_id,
				group->param_size, group->param_addr[core_id]);
			group->param_addr[core_id] = 0;
		}
	}

	group->param_type = 0;
}

int xh2a_ipu_group_destroy(struct xh2a_ipu_group *group)
{
	int old_status;
	struct xh2a_ipu_device *ipu_dev = group->ipu_dev;
	struct device *cdev_dev = ipu_dev->cdev_dev;

	dev_dbg(cdev_dev, "group %u %p destroy\n", group->id, group);

	old_status = atomic_read(&group->status);
	if (old_status == GROUP_DESTROY) {
		dev_err(cdev_dev, "group id %d %p already destroyed\n",
			group->id, group);
		return -EAGAIN;
	}

	if (old_status == GROUP_PENDING || old_status == GROUP_RUNNING ||
	    old_status == GROUP_CANCELED || old_status == GROUP_TIMEOUT) {
		xh2a_ipu_group_cancel(group);
		xh2a_ipu_group_put(group);
		return 0;
	}

	atomic_set(&group->status, GROUP_DESTROY);
	xh2a_ipu_group_put(group);

	return 0;
}

int xh2a_ipu_group_add_kernel(struct xh2a_ipu_group *group,
			      struct ipu_kernel_launch_data *kld)
{
	struct xh2a_ipu_kernel *kernel = NULL;
	struct xh2a_ipu_file_handle *file_handle = group->file_handle;
	struct xh2a_ipu_device *ipu_dev = file_handle->ipu_dev;
	struct device *cdev_dev = ipu_dev->cdev_dev;
	uint32_t index;

	if (!kld)
		return -EINVAL;

	if (kld->core_num == 0 || kld->core_num > XH2A_IPU_CORE_NUM)
		return -EINVAL;

	if (kld->tile_num != XH2A_TILE_NUM_PER_CORE)
		return -EINVAL;

	if (kld->param_type != XH2A_GROUP_PARAM_DDR &&
	    kld->param_type != XH2A_GROUP_PARAM_SPM)
		return -EINVAL;

	if (group->kernel_num >= XH2A_GROUP_MAX_KERNEL_NUM) {
		dev_err(ipu_dev->cdev_dev,
			"%s: group %u %p unsupport kernel num: %u\n", __func__,
			group->id, group, group->kernel_num);

		return -ENOMEM;
	}

	if ((atomic_read(&group->status) != GROUP_CREATED) &&
	    (atomic_read(&group->status) != GROUP_PACKING)) {
		dev_err(ipu_dev->cdev_dev, "%s: group status error %d\n",
			__func__, group->id);

		return -EACCES;
	}

	kernel = xh2a_ipu_kernel_create(group, kld);
	if (!kernel) {
		dev_err(cdev_dev, "%s: xh2a_ipu_kernel_create failed\n",
			__func__);
		return -ENOMEM;
	}

	if (kld->param_size > XH2A_SPM_TOTAL_SIZE ||
	    group->param_size > XH2A_SPM_TOTAL_SIZE - kld->param_size) {
		dev_err(cdev_dev, "%s: param size exceed, group_id %d\n",
			__func__, group->id);
		xh2a_ipu_kernel_destroy(kernel);

		return -EINVAL;
	}

	index = group->kernel_num;
	group->kernel_num++;
	group->kernels_timeout += kld->timeout_us;
	group->param_size += kld->param_size;
	list_add_tail(&kernel->node, &group->kernel_list);

	atomic_set(&group->status, GROUP_PACKING);

	dev_dbg(cdev_dev,
		"group %u %p add kernel%u: 0x%llx(0x%x) param: %u "
		"0x%llx(0x%x)\n",
		group->id, group, index, kernel->kld.kernel_addr,
		kernel->kld.kernel_size, kernel->kld.param_type,
		kernel->kld.param_phy_addr, kernel->kld.param_size);

	return 0;
}

int xh2a_ipu_group_execute(struct xh2a_ipu_group *group)
{
	int ret;
	int status;
	bool done_claimed = false;
	bool sync_interrupted = false;
	struct xh2a_ipu_file_handle *file_handle = group->file_handle;
	struct xh2a_ipu_device *ipu_dev = file_handle->ipu_dev;

	for (;;) {
		mutex_lock(&ipu_dev->dev_mutex);
		status = atomic_read(&group->status);
		if (xh2a_ipu_group_terminal_status(status)) {
			mutex_unlock(&ipu_dev->dev_mutex);
			if (status == GROUP_TIMEOUT)
				return -ETIMEDOUT;
			if (status == GROUP_DONE)
				return 0;
			return -ECANCELED;
		}
		if (atomic_read(&ipu_dev->quiesce_flag)) {
			mutex_unlock(&ipu_dev->dev_mutex);
			atomic_cmpxchg(&group->status, GROUP_PENDING, GROUP_PACKING);
			return -EBUSY;
		}

		/* A coordinated CTC reset owns the device. Keep this synchronous
		 * submitter asleep until the coordinator publishes the outcome, so the
		 * group is not exposed to policy_cancel_all() during reset. */
		if ((atomic_read(&ipu_dev->ctc_recovery_pending) ||
		     atomic_read(&ipu_dev->dev_state) == XH2A_IPU_DEV_CTC_RESETTING) &&
		    !atomic_read(&ipu_dev->dev_removed) &&
		    atomic_read(&ipu_dev->dev_state) != XH2A_IPU_DEV_FATAL) {
			mutex_unlock(&ipu_dev->dev_mutex);
			ret = wait_event_interruptible(
				ipu_dev->group_sync_wq,
				(!atomic_read(&ipu_dev->ctc_recovery_pending) &&
				 atomic_read(&ipu_dev->dev_state) == XH2A_IPU_DEV_READY) ||
				atomic_read(&ipu_dev->quiesce_flag) ||
				atomic_read(&ipu_dev->dev_removed) ||
				atomic_read(&ipu_dev->dev_state) == XH2A_IPU_DEV_FATAL ||
				xh2a_ipu_group_terminal_status(
					atomic_read(&group->status)));
			if (ret < 0) {
				atomic_cmpxchg(&group->status, GROUP_PENDING,
					       GROUP_PACKING);
				return ret;
			}
			continue;
		}

		status = atomic_read(&ipu_dev->dev_state);
		if (status != XH2A_IPU_DEV_READY) {
			int state_ret = atomic_read(&ipu_dev->dev_removed) ? -ENODEV :
				(status == XH2A_IPU_DEV_FATAL ? -EIO : -EBUSY);

			mutex_unlock(&ipu_dev->dev_mutex);
			dev_err(ipu_dev->cdev_dev, "%s: ipu device state is %d\n",
				__func__, atomic_read(&ipu_dev->dev_state));
			atomic_cmpxchg(&group->status, GROUP_PENDING, GROUP_PACKING);
			return state_ret;
		}

		if (!ipu_dev->policy) {
			mutex_unlock(&ipu_dev->dev_mutex);
			dev_err(ipu_dev->cdev_dev, "%s: ipu policy is NULL\n",
				__func__);
			atomic_cmpxchg(&group->status, GROUP_PENDING, GROUP_PACKING);
			return -ENODEV;
		}

		group->sync_start = ktime_get();
		dev_dbg(ipu_dev->cdev_dev,
			"group %u %p, %u kernels, %u cores, start = %llu\n", group->id,
			group, group->kernel_num, group->core_num, group->sync_start);

		reinit_completion(&group->sync_completion);

		ipu_dev->policy->ops->enqueue_group(ipu_dev->policy, group);
		mutex_unlock(&ipu_dev->dev_mutex);
		dev_dbg(ipu_dev->cdev_dev, "group %u %p enqueue to policy done\n",
			group->id, group);

		/* wakeup wq to execute group */
		queue_work(ipu_dev->sched_wq, &ipu_dev->group_sche_work);
		break;
	}

	ret = wait_for_completion_interruptible(&group->sync_completion);

	if (atomic_read(&ipu_dev->dev_removed)) {
		pr_err("%s: device mark removed\n", __func__);
		ret = -ENODEV;
		goto exec_err;
	}
	if (ret) {
		dev_err(ipu_dev->cdev_dev, "%s: group sync interrupted\n",
			__func__);

		status = atomic_read(&group->status);
		if (status == GROUP_RUNNING) {
			wait_for_completion(&group->sync_completion);

			if (atomic_read(&ipu_dev->dev_removed)) {
				pr_err("%s: device mark removed\n", __func__);
				ret = -ENODEV;
				goto exec_err;
			}

			sync_interrupted = true;
		} else if (!xh2a_ipu_group_terminal_status(status)) {
			xh2a_ipu_group_cancel(group);
			ret = -EINTR;
			goto exec_err;
		} else {
			sync_interrupted = true;
		}
	}

	if (atomic_read(&group->status) == GROUP_CANCELED) {
		dev_err(ipu_dev->cdev_dev,
			"%s: group %u sync canceled when ipu reset\n", __func__,
			group->id);

		ret = -ECANCELED;
		goto exec_err;
	}

	if (atomic_read(&group->status) == GROUP_TIMEOUT ||
	    group->exec_result[0] == XH2A_GROUP_KERNELS_TIMEOUT) {
		xh2a_ipu_read_device_temp(ipu_dev);
		dev_err(ipu_dev->cdev_dev, "%s: group sync timeout\n",
			__func__);

		ret = -ETIMEDOUT;
		goto exec_err;
	}

	if (group->exec_result[0] == XH2A_GROUP_ALLOC_SPM_FAIL) {
		dev_err(ipu_dev->cdev_dev, "%s: group alloc spm failed\n",
			__func__);

		ret = -ENOMEM;
		goto exec_err;
	}

	if (group->exec_result[0] == XH2A_GROUP_TRANSFER_SPM_FAIL) {
		dev_err(ipu_dev->cdev_dev, "%s: group transfer spm failed\n",
			__func__);

		ret = -EFAULT;
		goto exec_err;
	}

	if (sync_interrupted) {
		ret = -EINTR;
		goto exec_err;
	}

	dev_dbg(ipu_dev->cdev_dev, "group %u %p sync done\n", group->id,
		group);
	xh2a_ipu_group_log_exec_time(group, "execute_done");

	return 0;

exec_err:
	xh2a_ipu_group_log_exec_time(group, "execute_error");

	if (ret == -ETIMEDOUT)
		xh2a_dump_debug_regs(ipu_dev, group);

	/* Serialize the terminal-state claim with timeout and IRQ completion.
	 * Only a running group whose failure path wins this claim may be
	 * converted to DONE and removed from the hardware queue. */
	if (ret != -EINTR) {
		mutex_lock(&ipu_dev->ctc_claim_mutex);
		if (atomic_read(&group->status) == GROUP_RUNNING)
			done_claimed = atomic_cmpxchg(&group->status, GROUP_RUNNING,
						      GROUP_DONE) == GROUP_RUNNING;
		mutex_unlock(&ipu_dev->ctc_claim_mutex);
	}

	if (done_claimed) {
		if (group->queue) {
			mutex_lock(&group->queue->tile_mutex);
			if (!list_empty(&group->tile_list_node)) {
				list_del_init(&group->tile_list_node);
				xh2a_ipu_group_put(group);
			}
			mutex_unlock(&group->queue->tile_mutex);
		}
	}

	return ret;
}

bool xh2a_ipu_is_group_done(struct xh2a_ipu_group *group, uint32_t current_rptr,
			    uint32_t last_rptr)
{
	uint32_t group_wptr = group->target[0].end_wptr[0];
	uint32_t done_off = xh2a_ipu_calc_queue_delta(last_rptr, group_wptr);
	uint32_t curr_off = xh2a_ipu_calc_queue_delta(last_rptr, current_rptr);

	return done_off <= curr_off;
}

static int xh2a_group_transfer_param2spm(struct xh2a_ipu_device *ipu_dev,
					 struct xh2a_ipu_group *group,
					 uint32_t core_id)
{
	int ret;
	struct xh2a_ipu_kernel *kernel, *kernel_n;

	list_for_each_entry_safe(kernel, kernel_n, &group->kernel_list, node) {
		dev_dbg(ipu_dev->cdev_dev,
			"group %u %p kernel->host_param 0x%llx, size=0x%x\n",
			group->id, group, (uint64_t)kernel->host_param,
			kernel->kld.param_size);

		ret = xh2a_kernel_transfer_param2spm(kernel, core_id);
		if (ret) {
			dev_err(ipu_dev->cdev_dev,
				"%s: group %u %p write kernel param failed\n",
				__func__, group->id, group);
			return ret;
		}
	}

	return 0;
}

void xh2a_group_host_param_free(struct xh2a_ipu_device *ipu_dev,
				struct xh2a_ipu_group *group)
{
	struct xh2a_ipu_kernel *kernel, *kernel_n;

	list_for_each_entry_safe(kernel, kernel_n, &group->kernel_list, node) {
		dev_dbg(ipu_dev->cdev_dev,
			"group %u %p free host_param 0x%llx, size=0x%x\n",
			group->id, group, (uint64_t)kernel->host_param,
			kernel->kld.param_size);

		xh2a_kernel_host_param_free(kernel);
	}
}

static int
xh2a_ipu_group_transfer_param_to_spm(struct xh2a_ipu_device *ipu_device,
				     struct xh2a_ipu_group *group)
{
	int i, ret, core_id;

	mutex_lock(&group->mutex);
	for (i = 0; i < group->core_num; i++) {
		core_id = group->target[i].core_id;
		ret = xh2a_group_transfer_param2spm(ipu_device, group, core_id);
		if (ret) {
			dev_err(ipu_device->cdev_dev,
				"%s: %d: group %u %p transfer param2spm "
				"coreid%d failed\n",
				__func__, i, group->id, group, core_id);

			goto free_spm;
		}
	}
	mutex_unlock(&group->mutex);

	return 0;

free_spm:
	group->exec_result[0] = XH2A_GROUP_TRANSFER_SPM_FAIL;
	xh2a_group_host_param_free(ipu_device, group);
	if (group->param_type == XH2A_GROUP_PARAM_SPM)
		xh2a_ipu_group_free_spm(group);
	group->resources_released = true;
	if (group->sync_cnt_acquired) {
		group->sync_cnt_acquired = false;
		xh2a_ipu_group_sync_cnt_dec(ipu_device);
	}
	mutex_unlock(&group->mutex);
	complete_all(&group->sync_completion);

	return ret;
}

void xh2a_ipu_group_load_kds(struct xh2a_ipu_group *group)
{
	int i, j, ret, core_id, queue_id;
	int old_status;

	struct xh2a_ipu_booter_queue *queue = NULL;
	struct xh2a_ipu_device *ipu_device = group->ipu_dev;

	if (xh2a_ipu_group_load_blocked(ipu_device)) {
		xh2a_ipu_group_cancel(group);
		return;
	}

	if (!xh2a_ipu_group_queue_space_available(group)) {
		if (!xh2a_ipu_group_requeue_pending(group))
			xh2a_ipu_group_cancel(group);
		return;
	}

	xh2a_ipu_group_disarm_timeout(group);

	mutex_lock(&group->mutex);
	if (xh2a_ipu_group_load_blocked(ipu_device)) {
		mutex_unlock(&group->mutex);
		xh2a_ipu_group_cancel(group);
		return;
	}

	old_status =
		atomic_cmpxchg(&group->status, GROUP_PENDING, GROUP_RUNNING);
	if (old_status != GROUP_PENDING) {
		mutex_unlock(&group->mutex);
		complete_all(&group->sync_completion);
		return;
	}

	atomic_inc(&ipu_device->sync_cnt);
	group->sync_cnt_acquired = true;
	group->exec_deps_ready = false;
	group->exec_triggered = false;
	group->exec_waiters_released = false;
	atomic_set(&group->pending_exec_deps, 0);
	reinit_completion(&group->load_completion);
	group->loading = true;
	mutex_unlock(&group->mutex);

	dev_dbg(ipu_device->cdev_dev, "ipu sync_cnt after load: %u\n",
		atomic_read(&ipu_device->sync_cnt));

	if (xh2a_ipu_group_load_blocked(ipu_device)) {
		xh2a_ipu_group_abort_load(group);
		return;
	}

	if (group->param_type & ~XH2A_GROUP_PARAM_DDR) {
		ret = xh2a_ipu_group_spm_alloc(group);
		if (ret) {
			dev_err(ipu_device->cdev_dev,
				"%s: group %u %p alloc SPM failed\n", __func__,
				group->id, group);
			mutex_lock(&group->mutex);
			group->exec_result[0] = XH2A_GROUP_ALLOC_SPM_FAIL;
			mutex_unlock(&group->mutex);
			xh2a_ipu_group_release_exec_resources(group);
			complete_all(&group->sync_completion);
			xh2a_ipu_group_finish_load(group);
			return;
		}
	}

	if (group->param_type & XH2A_GROUP_PARAM_SPM) {
		ret = xh2a_ipu_group_transfer_param_to_spm(ipu_device, group);
		if (ret) {
			dev_err(ipu_device->cdev_dev,
				"%s: group %u %p write param to spm failed\n",
				__func__, group->id, group);
			xh2a_ipu_group_finish_load(group);
			return;
		}
	}
	/* enqueue KDs, set first queue interrupt flag */
	for (i = 0; i < group->core_num; i++) {
		core_id = group->target[i].core_id;
		xh2a_ipu_group_add_exec_dependency(group, core_id);
		for (j = 0; j < group->tile_num; j++) {
			queue_id = group->target[i].queue_id + j;
			queue = &ipu_device->tile_queues[core_id][queue_id];

			mutex_lock(&queue->tile_mutex);
			/* set interrupt flag to the first launched queue */
			if (i == 0 && j == 0)
				queue->flag |=
					XH2A_IPU_TILE_QUEUE_FLAG_INTERRUPT;

			xh2a_booter_queue_enqueue_group(
				ipu_device,
				&ipu_device->tile_queues[core_id][queue_id],
				group);
			group->target[i].end_wptr[j] = queue->wptr;

			mutex_unlock(&queue->tile_mutex);
		}
	}

	mutex_lock(&group->mutex);
	group->exec_deps_ready = true;
	mutex_unlock(&group->mutex);

	group->load_start = ktime_to_us(ktime_get());
	/* Record load before trigger so a fast done IRQ cannot beat accounting. */
	for (i = 0; i < group->core_num; i++) {
		core_id = group->target[i].core_id;
		xh2a_ipu_load_on_enqueue(ipu_device, core_id,
					 group->load_start);
	}

	/* trigger booter execute kernels */
	for (i = 0; i < group->core_num; i++) {
		core_id = group->target[i].core_id;
		for (j = 0; j < group->tile_num; j++) {
			queue_id = group->target[i].queue_id + j;
			queue = &ipu_device->tile_queues[core_id][queue_id];

			mutex_lock(&queue->tile_mutex);
			if (i == 0 && j == 0) {
				mutex_lock(&group->mutex);
				group->exec_triggered = true;
				mutex_unlock(&group->mutex);
				xh2a_ipu_group_exec_ready_for_trigger(group,
							       ktime_get());
			}
			xh2a_booter_queue_trigger(ipu_device, queue, group);
			mutex_unlock(&queue->tile_mutex);
		}
	}

	xh2a_ipu_group_finish_load(group);
}

int xh2a_ipu_group_spm_alloc(struct xh2a_ipu_group *group)
{
	int ret, i, j, core_id, free_core_id;
	uint64_t param_addr;
	struct xh2a_ipu_device *ipu_device = group->ipu_dev;

	mutex_lock(&group->mutex);

	for (i = 0; i < group->core_num; i++) {
		core_id = group->target[i].core_id;

		ret = xh2a_memory_allocator_kernel_alloc_spm(
			ipu_device->private_data, core_id, group->param_size,
			&param_addr);
		if (ret) {
			dev_err(ipu_device->cdev_dev,
				"%s: group %u %p alloc SPM%d failed\n",
				__func__, group->id, group, core_id);
			mutex_unlock(&group->mutex);
			goto err_spm_alloc;
		}

		group->param_addr[core_id] = param_addr;
		dev_dbg(group->ipu_dev->cdev_dev,
			"group %u %p alloc SPM%d: 0x%llx %u(0x%x)\n", group->id,
			group, core_id, param_addr, group->param_size,
			group->param_size);
	}
	mutex_unlock(&group->mutex);

	return 0;

err_spm_alloc:
	/* free SPM for all cores */
	for (j = 0; j < i; j++) {
		free_core_id = group->target[j].core_id;

		xh2a_memory_allocator_kernel_free_spm(
			ipu_device->private_data, free_core_id,
			group->param_size, group->param_addr[free_core_id]);

		group->param_addr[free_core_id] = 0;
	}

	return ret;
}

void xh2a_ipu_group_get_result(struct xh2a_ipu_group *group,
			       struct xh2a_group_result *result,
			       uint32_t *group_num)
{
	if (atomic_read(&group->status) == GROUP_DONE) {
		result->result[*group_num].group_id = group->id;
		memcpy(result->result[*group_num].buf, group->exec_result,
		       sizeof(group->exec_result));

		(*group_num)++;
	}
}
