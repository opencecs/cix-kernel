// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2025 Houmo AI Inc.
 * Author: mingdong.wang<mingdong.wang@houmo.ai>
 */

#ifndef _XH2A_IPU_INTERRUPT_H_
#define _XH2A_IPU_INTERRUPT_H_

#include <linux/list.h>
#include <linux/types.h>
#include <linux/workqueue.h>

/* interrupt api */
struct xh2a_ipu_device;
struct xh2a_ipu_booter_queue;
void xh2a_ipu_event_work(struct work_struct *work);
void xh2a_ipuss_core_bh_work(struct work_struct *work);
void xh2a_ipu_msi_work_register(struct xh2a_ipu_device *ipu_dev);
void xh2a_ipu_msi_work_unregister(struct xh2a_ipu_device *ipu_dev);
void xh2a_ipu_group_done_handler(struct xh2a_ipu_device *ipu_dev,
					struct xh2a_ipu_booter_queue *queue,
					uint32_t current_rptr,
					bool update_other_queues,
					struct list_head *done_list);
void xh2a_ipu_complete_done_groups(struct list_head *done_list);

#endif /* _XH2A_IPU_INTERRUPT_H_ */
