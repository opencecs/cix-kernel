// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2025 Houmo AI Inc.
 * Author: mingdong.wang<mingdong.wang@houmo.ai>
 */
#include <linux/mm.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/sched.h>

#include <xh2a_pcie_api.h>
#include <xh2a_address.h>

#include "xh2a_ipu_interrupt.h"
#include "xh2a_ipu_device.h"
#include "xh2a_ipu_hw.h"
#include "xh2a_ipu_group.h"
#include "xh2a_ipu_mempool.h"
#include "xh2a_ipu_load.h"

static void xh2a_ipu_queue_done_group(struct xh2a_ipu_group *group,
				      struct list_head *done_list)
{
	if (!group || !done_list)
		return;

	xh2a_ipu_group_get(group);
	if (!list_empty(&group->tile_list_node)) {
		list_del_init(&group->tile_list_node);
		xh2a_ipu_group_put(group);
	}

	if (list_empty(&group->done_list_node))
		list_add_tail(&group->done_list_node, done_list);
	else
		xh2a_ipu_group_put(group);
}

void xh2a_ipu_complete_done_groups(struct list_head *done_list)
{
	struct xh2a_ipu_group *group, *group_n;

	if (!done_list)
		return;

	list_for_each_entry_safe(group, group_n, done_list, done_list_node) {
		list_del_init(&group->done_list_node);

		xh2a_ipu_group_wait_load_done(group);

		xh2a_ipu_group_disarm_timeout(group);

		xh2a_ipu_group_log_exec_time(group, "irq_done");
		xh2a_ipu_group_release_exec_resources(group);
		xh2a_ipu_group_clear_exec_tails(group);
		/* The coordinator owns timeout publication until every participant
		 * is claimed.  IRQ still converges queue state and resources, but it
		 * must not expose this timeout before that barrier. */
		if (atomic_read(&group->status) != GROUP_TIMEOUT ||
		    !xh2a_ipu_ctc_timeout_completion_deferred(group->ipu_dev,
							       group))
			complete_all(&group->sync_completion);
		xh2a_ipu_group_put(group);
	}
}

static void xh2a_ipuss_tile_done_work(struct work_struct *work)
{
	uint32_t core_id;
	unsigned long flags;
	struct xh2a_ipu_booter_queue *queue = NULL;
	struct xh2a_ipu_event *event = NULL;

	struct xh2a_ipu_msi_work *msi_work =
		container_of(work, struct xh2a_ipu_msi_work, work);

	struct xh2a_ipu_device *ipu_dev = container_of(
		msi_work, struct xh2a_ipu_device, msi_works[msi_work->msi_id]);

	core_id = msi_work->msi_id % XH2A_IPU_CORE_NUM;

	dev_dbg(ipu_dev->cdev_dev,
		"tile_irq: %d, core_id = %d tile done work\n", msi_work->msi_id,
		core_id);

	if (!xh2a_ipu_device_ready(ipu_dev)) {
		dev_dbg(ipu_dev->cdev_dev,
			 "%s: skip tile done work pid=%d core=%u dev_state=%d reset=%d sync_cnt=%d\n",
			 __func__, current->pid, core_id,
			 atomic_read(&ipu_dev->dev_state),
			 atomic_read(&ipu_dev->reset_flag),
			 atomic_read(&ipu_dev->sync_cnt));
		xh2a_ipu_drain_pending_events(ipu_dev);
		return;
	}

	queue = &ipu_dev->tile_queues[core_id][0];

	event = xh2a_ipu_event_alloc(&ipu_dev->event_pool);
	if (!event) {
		dev_err(ipu_dev->cdev_dev, "core %u fatal err, event NULL!\n",
			core_id);
		return;
	}

	event->queue = queue;

	spin_lock_irqsave(&ipu_dev->event_lock, flags);
	list_add_tail(&event->node, &ipu_dev->isr_event_list);
	spin_unlock_irqrestore(&ipu_dev->event_lock, flags);

	queue_work(ipu_dev->intr_wq, &ipu_dev->ipu_event_work);
}

void xh2a_ipu_group_done_handler(struct xh2a_ipu_device *ipu_dev,
				 struct xh2a_ipu_booter_queue *queue,
				 uint32_t current_rptr,
				 bool update_other_queues,
				 struct list_head *done_list)
{
	int old_status, core_id, target_core;
	struct xh2a_ipu_group *group, *group_n;
	bool done;

	list_for_each_entry_safe(group, group_n, &queue->group_list,
				 tile_list_node) {
		old_status = atomic_read(&group->status);
		done = xh2a_ipu_is_group_done(group, current_rptr,
					      queue->last_rptr);

		if (!done) {
			dev_dbg(ipu_dev->cdev_dev, "group %u %p not done\n",
				group->id, group);
			break; /* No need to check further groups */
		}

		xh2a_ipu_group_release_exec_waiters(group, ktime_get());

		if (old_status == GROUP_DONE || old_status == GROUP_TIMEOUT ||
		    old_status == GROUP_DESTROY ||
		    old_status == GROUP_CANCELED) {
			if (update_other_queues)
				xh2a_booter_update_other_queues_rptr(
					ipu_dev, group, done_list);
			xh2a_ipu_queue_done_group(group, done_list);
			continue;
		}

		mutex_lock(&ipu_dev->ctc_claim_mutex);
		old_status = atomic_cmpxchg(&group->status, GROUP_RUNNING,
					    GROUP_DONE);
		if (old_status != GROUP_RUNNING) {
			mutex_unlock(&ipu_dev->ctc_claim_mutex);
			if (old_status == GROUP_DONE ||
			    old_status == GROUP_TIMEOUT ||
			    old_status == GROUP_DESTROY ||
			    old_status == GROUP_CANCELED) {
				if (update_other_queues)
					xh2a_booter_update_other_queues_rptr(
						ipu_dev, group, done_list);
				xh2a_ipu_queue_done_group(group, done_list);
			}
			continue;
		}

		dev_dbg(ipu_dev->cdev_dev, "group %u %p is done\n", group->id,
			group);

		group->load_end = ktime_to_us(ktime_get());

		/* record ipu load from group */
		for (core_id = 0; core_id < group->core_num; core_id++) {
			target_core = group->target[core_id].core_id;
			xh2a_ipu_load_on_done(ipu_dev, target_core,
					      group->load_end);
		}

		/* gets the execution result of the group */
		xh2a_ipu_booter_get_hw_context(ipu_dev, group->exec_result);
		mutex_unlock(&ipu_dev->ctc_claim_mutex);

		/* update the read pointer for other tile queues */
		if (update_other_queues)
			xh2a_booter_update_other_queues_rptr(ipu_dev, group,
							     done_list);

		xh2a_ipu_queue_done_group(group, done_list);
	}

}

void xh2a_ipuss_core_bh_work(struct work_struct *work)
{
	uint32_t core_id;
	unsigned long flags;
	struct xh2a_ipu_booter_queue *queue = NULL;
	struct xh2a_ipu_event *event = NULL;

	struct xh2a_ipu_msi_work *msi_work =
		container_of(work, struct xh2a_ipu_msi_work, work);

	struct xh2a_ipu_device *ipu_dev = container_of(
		msi_work, struct xh2a_ipu_device, core_works[msi_work->msi_id]);

	core_id = msi_work->msi_id % XH2A_IPU_CORE_NUM;

	queue = &ipu_dev->tile_queues[core_id][0];

	if (!xh2a_ipu_device_ready(ipu_dev)) {
		dev_dbg(ipu_dev->cdev_dev,
			 "%s: skip core bh before intr flag read pid=%d core=%u dev_state=%d reset=%d sync_cnt=%d\n",
			 __func__, current->pid, core_id,
			 atomic_read(&ipu_dev->dev_state),
			 atomic_read(&ipu_dev->reset_flag),
			 atomic_read(&ipu_dev->sync_cnt));
		xh2a_ipu_drain_pending_events(ipu_dev);
		return;
	}

	if (!xh2a_booter_queue_get_intr_flag(ipu_dev, queue))
		return;

	dev_dbg(ipu_dev->cdev_dev, "tile_irq: %d, core_id = %d bh work\n",
		msi_work->msi_id, core_id);

	event = xh2a_ipu_event_alloc(&ipu_dev->event_pool);
	if (!event) {
		dev_err(ipu_dev->cdev_dev, "fatal err, event "
					   "NULL!\n");
		return;
	}

	event->queue = queue;

	spin_lock_irqsave(&ipu_dev->event_lock, flags);
	list_add_tail(&event->node, &ipu_dev->isr_event_list);
	spin_unlock_irqrestore(&ipu_dev->event_lock, flags);

	queue_work(ipu_dev->intr_wq, &ipu_dev->ipu_event_work);
}

void xh2a_ipu_event_work(struct work_struct *work)
{
	unsigned long flags;
	uint32_t current_rptr, core_id, mask;
	struct xh2a_ipu_event *event = NULL;
	struct xh2a_ipu_booter_queue *queue = NULL;
	struct xh2a_ipu_device *ipu_dev =
		container_of(work, struct xh2a_ipu_device, ipu_event_work);

	if (!xh2a_ipu_device_ready(ipu_dev)) {
		xh2a_ipu_drain_pending_events(ipu_dev);
		return;
	}

	spin_lock_irqsave(&ipu_dev->event_lock, flags);
	while (!list_empty(&ipu_dev->isr_event_list)) {
		LIST_HEAD(done_list);

		event = list_first_entry_or_null(&ipu_dev->isr_event_list,
						 struct xh2a_ipu_event, node);

		list_del_init(&event->node);
		spin_unlock_irqrestore(&ipu_dev->event_lock, flags);

		queue = event->queue;

		mutex_lock(&queue->tile_mutex);
		/* clear kernel done interrupt */
		xh2a_booter_queue_clear_intr(ipu_dev, queue);

		current_rptr = xh2a_booter_queue_get_rptr(ipu_dev, queue);

		queue->last_rptr = queue->rptr;

		xh2a_ipu_group_done_handler(ipu_dev, queue, current_rptr, true,
					    &done_list);

		/* update queue rptr */
		queue->rptr = current_rptr;
		WRITE_ONCE(queue->cached_rptr, queue->rptr);
		mutex_unlock(&queue->tile_mutex);

		xh2a_ipu_complete_done_groups(&done_list);

		/* remain space update, wakeup wq to execute group */
		queue_work(ipu_dev->sched_wq, &ipu_dev->group_sche_work);

		xh2a_ipu_event_free(&ipu_dev->event_pool, event);
		spin_lock_irqsave(&ipu_dev->event_lock, flags);
	}
	spin_unlock_irqrestore(&ipu_dev->event_lock, flags);

	for (core_id = 0; core_id < XH2A_IPU_CORE_NUM; core_id++) {
		mask = (0xF << (core_id * 4));
		if ((ipu_dev->available_tiles_mask & mask) != mask)
			continue;

		queue_work(ipu_dev->intr_wq,
			   &ipu_dev->core_works[core_id].work);
	}
}

static void xh2a_ipuss_exception_work(struct work_struct *work)
{
	struct xh2a_ipu_msi_work *msi_work =
		container_of(work, struct xh2a_ipu_msi_work, work);

	struct xh2a_ipu_device *ipu_dev = container_of(
		msi_work, struct xh2a_ipu_device, msi_works[msi_work->msi_id]);

	dev_info(ipu_dev->cdev_dev, "%s:\n", __func__);
}

static void xh2a_ctcss_phy_work(struct work_struct *work)
{
	struct xh2a_ipu_msi_work *msi_work =
		container_of(work, struct xh2a_ipu_msi_work, work);

	struct xh2a_ipu_device *ipu_dev = container_of(
		msi_work, struct xh2a_ipu_device, msi_works[msi_work->msi_id]);

	dev_info(ipu_dev->cdev_dev, "%s:\n", __func__);
}

static work_func_t xh2a_msi_work_table[] = {
	[XH2A_PCIE_MSI_ID_IPUSS_C0T0] = xh2a_ipuss_tile_done_work,
	[XH2A_PCIE_MSI_ID_IPUSS_C1T0] = xh2a_ipuss_tile_done_work,
	[XH2A_PCIE_MSI_ID_IPUSS_C0C1] = xh2a_ipuss_exception_work,
	[XH2A_PCIE_MSI_ID_CTC] = xh2a_ctcss_phy_work,
};

void xh2a_ipu_msi_work_register(struct xh2a_ipu_device *ipu_dev)
{
	int work_count, i;
	struct xh2a_pcie_client *client = NULL;

	work_count = 0;
	client = &ipu_dev->client;

	for (i = 0; i < ARRAY_SIZE(xh2a_msi_work_table); i++) {
		if (xh2a_msi_work_table[i] == NULL)
			continue;

		INIT_WORK(&ipu_dev->msi_works[i].work, xh2a_msi_work_table[i]);
		ipu_dev->msi_works[i].msi_id = i;
		client->work[work_count].msi_id = i;
		client->work[work_count++].msi_work =
			&ipu_dev->msi_works[i].work;
	}

	client->work_num = work_count;

	ipu_dev->msi_work_inited = true;
}

void xh2a_ipu_msi_work_unregister(struct xh2a_ipu_device *ipu_dev)
{
	int i;
	struct xh2a_pcie_client *client = &ipu_dev->client;

	if (!ipu_dev->msi_work_inited)
		return;

	for (i = 0; i < client->work_num; i++)
		cancel_work_sync(client->work[i].msi_work);

	ipu_dev->msi_work_inited = false;
}
