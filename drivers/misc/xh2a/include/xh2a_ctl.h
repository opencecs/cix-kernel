// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2026 Houmo AI Inc.
 * Author: mingdong.wang<mingdong.wang@houmo.ai>
 */

#ifndef _XH2A_CTL_H_
#define _XH2A_CTL_H_

#include <linux/cdev.h>
#include <linux/bitmap.h>
#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include <xh2a_ctl_internal.h>

#define XH2A_CTL_CTC_CHIP_MAX 64
#define XH2A_CTL_CTC_GROUP_MAX 64
#define XH2A_CTL_CTC_MAX_DEVICES XH2A_CTL_MAX_DEVICES
#define XH2A_CTL_CTC_AUTO_INIT_DELAY_MS 500
#define XH2A_CTL_CTC_RECOVERY_TIMEOUT_MS 10000

enum xh2a_ctl_ctc_auto_state {
	XH2A_CTL_CTC_AUTO_PENDING = 0,
	XH2A_CTL_CTC_AUTO_BUILDING,
	XH2A_CTL_CTC_AUTO_READY,
	XH2A_CTL_CTC_AUTO_FAILED,
};

enum xh2a_ctl_ctc_recovery_role {
	XH2A_CTL_CTC_RECOVERY_NONE = 0,
	XH2A_CTL_CTC_RECOVERY_ROOT,
	XH2A_CTL_CTC_RECOVERY_PEER,
};

struct xh2a_ctl_ctc_group_entry {
	struct list_head node;
	uint32_t group_id;
	uint32_t chip_num;
	uint32_t live_dev_id[XH2A_CTL_CTC_CHIP_MAX];
};

struct xh2a_ctl_ctx;

struct xh2a_ctl_process_lock_ctx {
	struct pid *owner_pid;
	struct mutex op_lock;
	bool lock_pending;
	wait_queue_head_t lock_waitq;
	unsigned int fd_count;
	struct list_head node;
};

/**
 * struct xh2a_ctl_file_ctx - Per-open CTL file context.
 * @ctl:        Global CTL context backing this file.
 * @process_lock_ctx: Process-scoped lock and FD lease context.
 * @node:       Links this open file into ctl->ctl_file_list.
 *
 * Device locks remain owned by TGID, but are released only after the last
 * CTL file opened by that TGID is closed.
 */
struct xh2a_ctl_file_ctx {
	struct xh2a_ctl_ctx *ctl;
	struct xh2a_ctl_process_lock_ctx *process_lock_ctx;
	struct list_head node;
};

/**
 * struct xh2a_ctl_ctx - Global ctl context used by the ctl chrdev.
 * @cdev_dev:   Sysfs device for /sys/class/xh2a_ctl/...
 * @lock:       Protects ctl_list and other ctl-wide shared state.
 * @ctl_list:   List head of all per-device lock contexts
 *              (struct xh2a_ctl_dev_ctx).
 */
struct xh2a_ctl_ctx {
	struct device *cdev_dev;

	/* Serializes CTC ioctl with PCIe probe/remove and BAR lifetime. */
	struct mutex ctc_mutex;
	struct mutex lock;

	struct list_head ctl_list;
	struct list_head ctl_file_list;
	struct list_head process_lock_ctx_list;
	struct list_head ctc_group_list;
	bool ctc_topology_dirty;

	struct delayed_work ctc_auto_work;
	struct work_struct ctc_recovery_work;
	wait_queue_head_t ctc_wq;
	enum xh2a_ctl_ctc_auto_state ctc_auto_state;
	int ctc_auto_errno;
	u64 ctc_auto_generation;

	bool ctc_recovery_active;
	/* Numeric PID snapshot for diagnostics; not used for ownership checks. */
	pid_t ctc_recovery_owner_pid_snap;
	uint32_t ctc_recovery_root_dev_id;
	DECLARE_BITMAP(ctc_recovery_devmask, XH2A_CTL_MAX_DEVICES);
	int ctc_recovery_errno;
};

#endif /*_XH2A_CTL_H_*/
