// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2026 Houmo AI Inc.
 * Author: mingdong.wang<mingdong.wang@houmo.ai>
 */

#ifndef _XH2A_CTL_CTX_H_
#define _XH2A_CTL_CTX_H_

#include <linux/fs.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/kref.h>
#include <linux/pid.h>
#include <linux/uaccess.h>
#include <linux/sched.h>

#include "../pcie/xh2a_pcie.h"

#define XH2A_LOCK_F_NONBLOCK (1u << 0)

static inline struct xh2a_ctl_ctx *
xh2a_ctl_file_ctx_get(struct file *filp)
{
	struct xh2a_ctl_file_ctx *file_ctx;

	if (!filp)
		return NULL;

	file_ctx = filp->private_data;
	return file_ctx ? file_ctx->ctl : NULL;
}

/**
 * struct xh2a_ctl_dev_ctx - Per-device lock context managed by the ctl.
 * @dev_id:     Logical device identifier, typically the PCIe device minor.
 * @pcie_dev:   Pointer to the corresponding PCIe device driver context.
 * @lock:       Protects owner_pid, refcnt and online fields for this lock.
 * @wq:         Wait queue used by threads blocking on this device lock.
 * @refcnt:     Recursive lock hold count for the current owner process.
 * @owner_pid: Referenced PID object of the lock owner; NULL when free.
 * @kref:       Reference counter for safe lifetime management of this
 *              lock context.
 * @online:     True if the device is present and usable, false if removed
 *              or otherwise unavailable.
 * @ipu_ready:  True after IPU probe has registered the IPU client.
 * @removing:   True after PCIe remove starts.
 * @node:       List node linking this context into ctl->ctl_list.
 */
struct xh2a_ctl_dev_ctx {
	uint32_t dev_id;
	struct xh2a_pcie_dev *pcie_dev;

	struct mutex lock;
	wait_queue_head_t wq;

	uint32_t refcnt;
	struct pid *owner_pid;
	struct kref kref;

	bool online;
	bool ipu_ready;
	bool removing;

	struct list_head node;
};

/* Lock-side helpers implemented in xh2a_ctl_lock.c */
long xh2a_ctl_lock_dev(struct file *filp, const struct xh2a_ctl_lock_req *req);
long xh2a_ctl_unlock_dev(struct file *filp,
			 const struct xh2a_ctl_lock_req *req);
int xh2a_ctl_ctc_build_topology_auto_locked(struct xh2a_ctl_ctx *ctl);
int xh2a_ctl_ctc_check_ready(struct xh2a_ctl_ctx *ctl);
int xh2a_ctl_ctc_wait_ready(struct xh2a_ctl_ctx *ctl, uint32_t timeout_ms);
int xh2a_ctl_ctc_wait_ready_timeout(struct xh2a_ctl_ctx *ctl,
				     unsigned long timeout_jiffies);
long xh2a_ctl_ctc_get(struct file *filp, struct xh2a_ctl_ctc_get_req *req);
void xh2a_ctl_ctc_group_list_clear(struct xh2a_ctl_ctx *ctl);
void xh2a_ctl_ctc_lifecycle_lock(struct xh2a_ctl_ctx *ctl);
void xh2a_ctl_ctc_lifecycle_unlock(struct xh2a_ctl_ctx *ctl);
int xh2a_ctl_ctc_mark_ipu_ready(struct xh2a_ctl_ctx *ctl, uint32_t dev_id);
void xh2a_ctl_ctc_mark_dev_removing(struct xh2a_ctl_ctx *ctl,
				    uint32_t dev_id);
void xh2a_ctl_ctc_schedule_auto_init(struct xh2a_ctl_ctx *ctl);
void xh2a_ctl_ctc_cancel_auto_init(struct xh2a_ctl_ctx *ctl);

/* Reset/recovery coordinator implemented in xh2a_ctl_reset.c */
void xh2a_ctl_ctc_recovery_work(struct work_struct *work);
int xh2a_ctl_ctc_recover_devmask_locked(struct xh2a_ctl_ctx *ctl,
					const unsigned long *devmask);

struct xh2a_ctl_dev_ctx *xh2a_ctl_create_lock(struct xh2a_ctl_ctx *ctl,
					      uint32_t dev_id, void *handle);
void xh2a_ctl_destroy_lock(struct xh2a_ctl_ctx *ctl, uint32_t dev_id);
void xh2a_ctl_ctx_get(struct xh2a_ctl_dev_ctx *ctl_ctx);
void xh2a_ctl_ctx_put(struct xh2a_ctl_dev_ctx *ctl_ctx);

void xh2a_ctl_cleanup_all_locks(struct xh2a_ctl_ctx *ctl);
void xh2a_ctl_release_locks_locked(struct xh2a_ctl_ctx *ctl,
					struct xh2a_ctl_process_lock_ctx *process_lock_ctx);
/* Caller holds ctl->lock. Clear the removing device owner's locks in its CTC group. */
void xh2a_ctl_release_group_locks_locked(struct xh2a_ctl_ctx *ctl,
					     uint32_t dev_id);
int xh2a_ctl_get_owner_devmask(struct xh2a_ctl_ctx *ctl,
			       uint32_t timeout_dev_id, pid_t *owner_pid_snap,
			       unsigned long *devmask);
#endif /* _XH2A_CTL_CTX_H_ */
