// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2025 Houmo AI Inc.
 * Author: mingdong.wang<mingdong.wang@houmo.ai>
 */

#ifndef _XH2A_IPU_DEVICE_H_
#define _XH2A_IPU_DEVICE_H_

#include <linux/mutex.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/workqueue.h>
#include <linux/atomic.h>
#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/kref.h>
#include <xh2a_pcie_api.h>
#include <xh2a_ipu_internal.h>

#include "xh2a_ipu_hw.h"
#include "xh2a_ipu_config.h"
#include "xh2a_ipu_policy.h"
#include "xh2a_ipu_mempool.h"
#include "xh2a_ipu_load.h"
#include "xh2a_ipu_ctc.h"

struct xh2a_ipu_group;

/**
 * struct xh2a_ipu_msi_work - Structure for representing an IPU MSI work item.
 * @work: Work structure of pcie.
 * @msi_id: Identifier for the specific MSI associated with the work item.
 */
struct xh2a_ipu_msi_work {
	struct work_struct work;
	int msi_id;
};

enum xh2a_ipu_device_state {
	XH2A_IPU_DEV_READY = 0,
	XH2A_IPU_DEV_RESET_REQUIRED,
	XH2A_IPU_DEV_RESETTING,
	XH2A_IPU_DEV_CTC_RESETTING,
	XH2A_IPU_DEV_FATAL,
};

enum xh2a_ipu_reset_owner {
	XH2A_IPU_RESET_OWNER_NONE = 0,
	XH2A_IPU_RESET_OWNER_AUTO,
	XH2A_IPU_RESET_OWNER_IOCTL,
};

/**
 * struct xh2a_ipu_crg_regs - Structure for representing ipu crg regs.
 * @pll_int: PLL integer reg, offset 0x8.
 * @pll_frac: PLL frac reg, offset 0xC.
 * @clk_select: Clock select reg, offset 0x20.
 * @clk_div: Clocl divider reg, offset 0x90.
 */
struct xh2a_ipu_crg_regs {
	uint32_t pll_int;
	uint32_t pll_frac;
	uint32_t clk_select;
	uint32_t clk_div;
};

/**
 * struct xh2a_ipu_device - Represents an IPU device.
 * @name: Name of the IPU device.
 * @cdev_dev: Sysfs device (/sys/class/xh2a_ipu_device/).
 * @parent_dev: Parent PCI device.
 * @dev_mutex: Mutex for synchronizing access to the device.
 * @fh_mutex: Mutex for synchronizing access to the file_handle.
 * @load_mutex: Mutex for synchronizing access to the ipu load.
 * @event_lock: Spinlock for ipu event list.
 * @ops: booter queue operations for ipu device.
 * @file_handle_list: List of file handles associated with the device.
 * @isr_event_list: List of isr events associated with the device.
 * @client: client for pcie handle.
 *     usage and preventing premature release.
 * @msi_works: The work struct array of msi.
 * @core_works: The work struct array of msi, for post irq work.
 * @private_data: Handle for pcie.
 * @event_pool: Memory pool for events.
 * @tile_queues: Tile queues for group execution across cores and tiles.
 * Organized as a 2D array where:
 *  - First dimension corresponds to cores (XH2A_IPU_CORE_NUM).
 *  - Second dimension corresponds to tiles per core (XH2A_TILE_NUM_PER_CORE).
 * @policy: The policy for tile queue allocation.
 * @group_sche_work: The work struct for group scheduling.
 * @group_sche_retry_work: Delayed retry work for transient scheduler busy.
 * @ipu_event_work: The work struct for ipu event.
 * @ipu_load_work: The work struct for ipu load.
 * @sched_wq: Single-thread workqueue for launch and scheduling.
 * @intr_wq: Single-thread workqueue for interrupt recovery workers.
 * @load_wq: Single-thread workqueue for periodic load accounting.
 * @reset_wq: Single-thread workqueue for timeout recovery reset.
 * @load_dev: IPU device load statistics info.
 * @load_timer: Kernel timer used to trigger periodic load computation.
 * @lock: Spinlock for synchronizing access to shared fields
 *        across timer and ISR contexts.
 * @block_ioctl_flag: flag for block ioctl
 * @block_ioctl_wq: wait queue for block ioctl
 * @dev_initialized: flag for device initialized
 * @dev_removed: flag for device removed
 * @dev_refcnt: reference count for device
 */
struct xh2a_ipu_device {
	char name[XH2A_IPU_NAME_LEN];
	struct device *cdev_dev;
	struct device *parent_dev;
	struct mutex dev_mutex;
	struct mutex fh_mutex;
	struct mutex dump_mutex;
	/* Serializes this device's CTC state and PHY register sequences. */
	struct mutex ctc_mutex;
	int ctc_chip_id;
	int ctc_group_id;
	int ctc_is_single;
	int ctc_group_num;
	uint32_t ctc_fail_info;
	bool ctc_needs_phy_prepare;
	atomic_t ctc_reinit_inhibit;
	spinlock_t event_lock;

	const struct xh2a_booter_ops *ops;

	struct list_head file_handle_list;
	struct list_head isr_event_list;

	struct xh2a_pcie_client client;
	bool msi_work_inited;
	struct xh2a_ipu_msi_work msi_works[XH2A_IPU_MSI_WORK_NUM];
	struct xh2a_ipu_msi_work core_works[XH2A_IPU_CORE_NUM];
	void *private_data;

	struct xh2a_event_pool event_pool;
	struct xh2a_ipu_booter_queue tile_queues[XH2A_IPU_CORE_NUM]
						[XH2A_TILE_NUM_PER_CORE];
	/* One execution-order tail per core; all groups occupy all four tiles. */
	struct xh2a_ipu_group *core_tail_group[XH2A_IPU_CORE_NUM];
	struct mutex core_exec_mutex[XH2A_IPU_CORE_NUM];

	struct xh2a_ipu_policy *policy;
	bool group_work_inited;
	int last_core;
	unsigned int group_sche_busy_retry_count;
	struct work_struct group_sche_work;
	struct delayed_work group_sche_retry_work;
	struct delayed_work auto_reset_work;
	struct delayed_work ctc_recovery_retry_work;
	struct work_struct ipu_event_work;
	struct work_struct ipu_load_work;
	struct workqueue_struct *sched_wq;
	struct workqueue_struct *intr_wq;
	struct workqueue_struct *load_wq;
	struct workqueue_struct *reset_wq;

	struct xh2a_ipu_load_core load_core[XH2A_IPU_CORE_NUM];
	struct timer_list load_timer;
	spinlock_t load_lock;
	struct mutex load_mutex;
	atomic_t load_running;
	atomic_t is_reboot; /* reboot or shutdown */
	struct notifier_block load_shutdown_nb;

	atomic_t block_ioctl_flag;
	atomic_t quiesce_flag;
	struct wait_queue_head block_ioctl_wq;

	uint32_t available_cores_num;
	uint32_t available_tiles_mask;
	void *spm_snapshot_buf[XH2A_IPU_CORE_NUM];

	atomic_t dev_initialized;
	atomic_t dev_removed;
	struct kref dev_refcnt;

	atomic_t reset_flag;
	atomic_t dev_state;
	atomic_t active_reset_mode;
	atomic_t sync_cnt;
	atomic_t ctc_recovery_pending;
	atomic_t ctc_recovery_local_reset_done;
	/* Serializes terminal group claims from timeout, IRQ and recovery work. */
	struct mutex ctc_claim_mutex;
	atomic_t ctc_timeout_claimed;
	/* Protected by ctc_claim_mutex and held until timeout completion is
	 * published. */
	struct xh2a_ipu_group *ctc_timeout_group;
	int ctc_timeout_previous_status;
	/* Protected by ctc_claim_mutex; blocks cancellation and duplicate publish
	 * while the claimed group's completion is being delivered. */
	bool ctc_timeout_publishing;
	struct mutex reset_mutex;
	/* Protected by reset_mutex; one hardware reset owner per recovery epoch. */
	enum xh2a_ipu_reset_owner reset_owner;
	bool reset_local_done;
	int reset_result;
	/* Waiters take a per-call result snapshot before another epoch can start. */
	struct list_head reset_waiter_list;
	wait_queue_head_t group_sync_wq;
};

struct xh2a_ipu_device *xh2a_ipu_device_create(void *handle);
void xh2a_ipu_device_destroy(struct xh2a_ipu_device *ipu_dev);
int xh2a_ipu_prepare_fullchip_reset(void *handle);
void xh2a_ipu_abort_fullchip_reset(void *handle);
void xh2a_ipu_cancel_async_works(struct xh2a_ipu_device *ipu_dev);
void xh2a_ipu_drain_pending_events(struct xh2a_ipu_device *ipu_dev);
int xh2a_ipu_device_reset_discard(struct xh2a_ipu_device *ipu_dev);
int xh2a_ipu_device_reset_discard_owner(struct xh2a_ipu_device *ipu_dev,
					enum xh2a_ipu_reset_owner owner);
void xh2a_ipu_schedule_auto_reset(struct xh2a_ipu_device *ipu_dev);
void xh2a_ipu_cancel_auto_reset(struct xh2a_ipu_device *ipu_dev);
void xh2a_ipu_defer_ctc_timeout_recovery(struct xh2a_ipu_device *ipu_dev);
int xh2a_ipu_enter_ctc_recovery_reset(struct xh2a_ipu_device *ipu_dev);
int xh2a_ipu_ctc_prepare_recovery(struct xh2a_ipu_device *ipu_dev);
bool xh2a_ipu_ctc_recovery_local_reset_done(struct xh2a_ipu_device *ipu_dev);
void xh2a_ipu_resume_after_ctc_recovery(struct xh2a_ipu_device *ipu_dev);
void xh2a_ipu_abort_ctc_recovery_fatal(struct xh2a_ipu_device *ipu_dev);
int xh2a_ipu_ctc_claim_peer_timeout(struct xh2a_ipu_device *ipu_dev);
int xh2a_ipu_ctc_claim_timeout_group(struct xh2a_ipu_device *ipu_dev,
				      struct xh2a_ipu_group *group,
				      int previous_status);
void xh2a_ipu_ctc_publish_timeout(struct xh2a_ipu_device *ipu_dev);
void xh2a_ipu_ctc_release_standalone_timeout_claim(
	struct xh2a_ipu_device *ipu_dev);
bool xh2a_ipu_ctc_timeout_completion_deferred(
	struct xh2a_ipu_device *ipu_dev, struct xh2a_ipu_group *group);

static inline bool xh2a_ipu_device_ready(struct xh2a_ipu_device *ipu_dev)
{
	return atomic_read(&ipu_dev->dev_state) == XH2A_IPU_DEV_READY;
}

#endif /* _XH2A_IPU_DEVICE_H_ */
