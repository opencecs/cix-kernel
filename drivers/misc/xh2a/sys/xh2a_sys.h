// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2024 Houmo AI Inc.
 * Author: hongxing.ma<hongxing.ma@houmo.ai>
 */

#ifndef _XH2A_SYS_H_
#define _XH2A_SYS_H_

#include <xh2a_pcie_api.h>
#include <xh2a_system_internal.h>

#define CTC_UNINIT_VAL 0xAAAA

/*
 * struct xh2a_sys_dev - xh2a sys device
 * @cdev_dev: sysfs device
 * @parent_dev: parent pci device
 * @name_buf: name buffer
 * @sys_access_mutex: serializes sys operations with device unpublish/remove
 * @sys_mutex: protects sys runtime state and register operations
 * @device_addr: device addr to read or write
 * @private_data: handle for pcie
 * @ctc_chip_id: local chip index within the discovered CTC group
 * @ctc_group_id: discovered CTC group id
 * @ctc_is_single: 1 when the device is treated as a single chip
 * @ctc_group_num: total discovered CTC groups
 * @ctc_needs_phy_prepare: CTC PHY was shut down and must be prepared before use
 * @block_ioctl_flag: flag for block ioctl
 * @block_ioctl_wq: wait queue for block ioctl
 * @removed: flag for device removed
 * @refcount: reference count
 */
struct xh2a_sys_dev {
	struct device *cdev_dev;
	struct device *parent_dev;
	char name_buf[XH2A_SYSTEM_DEVICE_NAME_LEN];
	struct mutex sys_access_mutex;
	struct mutex sys_mutex;
	uint64_t device_addr;
	void *private_data;
	struct xh2a_pcie_client client;
	atomic_t block_ioctl_flag;
	struct wait_queue_head block_ioctl_wq;
	atomic_t removed;
	struct kref refcount;
};

#endif /*_XH2A_SYS_H_*/
