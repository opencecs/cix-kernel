// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2024 Houmo AI Inc.
 * Author: hongxing.ma<hongxing.ma@houmo.ai>
 */

#ifndef _XH2A_MEMORY_TRANSFER_H_
#define _XH2A_MEMORY_TRANSFER_H_

#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/list.h>
#include <linux/kref.h>
#include <linux/hashtable.h>
#include <xh2a_pcie_api.h>
#include <xh2a_memory_transfer_internal.h>
#include "xh2a_memory_transfer_sysdma.h"

#define XH2A_MEMORY_TRANSFER_DEVICE_NAME_LEN 64

#define P2P_STATUS_HASH_BITS 6 /* 64 buckets */
/*
 * struct p2p_status_entry - per-device P2P status entry
 * @src_devid: source device id
 * @dst_devid: destination device id
 * @status: P2P capability status
 * @node: hash list node, indexed by (src_devid << 32 | dst_devid)
 */
struct p2p_status_entry {
	uint32_t src_devid;
	uint32_t dst_devid;
	uint32_t status; /* enum driver_memory_transfer_p2p_status */
	struct hlist_node node;
};

/*
 * struct xh2a_memory_transfer_dev - xh2a memory_transfer device
 * @cdev_dev: sysfs device (/sys/class/xh2a_memory_transfer_device*)
 * @parent_dev: parent pci device, used for P2P pci_dev lookup
 * @name_buf: name buffer
 * @transfer_mutex: mutex for memory transfer
 * @sysdma: structure of sysdma
 * @file_handle_list: list for file handle
 * @lock_dev: lock device status
 * @lock_dev_owner: which file handle lock this device
 * @lock_dev_wq: wait queue for lock device
 * @block_ioctl_flag: flag for block ioctl
 * @block_ioctl_wq: wait queue for block ioctl
 * @dev_initialized: flag for device initialized
 * @dev_removed: flag for device removed
 * @dev_refcnt: reference count for device
 * @private_data: handle for pcie
 */
struct xh2a_memory_transfer_dev {
	struct device *cdev_dev;
	struct device *parent_dev;
	char name_buf[XH2A_MEMORY_TRANSFER_DEVICE_NAME_LEN];
	struct mutex transfer_mutex;
	struct xh2a_memory_transfer_sysdma_handle sysdma;
	struct list_head file_handle_list;
	uint64_t lock_dev;
	void *lock_dev_owner;
	struct wait_queue_head lock_dev_wq;
	atomic_t block_ioctl_flag;
	struct wait_queue_head block_ioctl_wq;
	atomic_t dev_initialized;
	atomic_t dev_removed;
	struct kref dev_refcnt;
	void *private_data;
	struct xh2a_pcie_client client;
	/* Per-device P2P status tracking */
	DECLARE_HASHTABLE(p2p_status_ht, P2P_STATUS_HASH_BITS);
	spinlock_t p2p_status_lock;
	/*
	 * Cross-device registry node:
	 * each transfer_dev registers itself into a module-global list so
	 * that a lookup by devid can resolve to its underlying pci_dev,
	 * which is required by P2P capability detection.
	 */
	struct list_head registry_node;
	int devid;
};

/*
 * struct xh2a_memory_transfer_file_handle - file handle
 * @node: list_head structure
 * @transfer: memory transfer device
 * @file_handle_mutex: mutex for file handle
 * @file_handle_lockmap: lockmap status from this file handle
 */
struct xh2a_memory_transfer_file_handle {
	struct list_head node;
	struct xh2a_memory_transfer_dev *transfer;
	struct mutex file_handle_mutex;
	uint64_t file_handle_lockmap;
};

#include <xh2a_address.h>
/*
 * XH2A_MEMORY_TRANSFER_DDR_TOTAL_SIZE - total size for compiler
 * XH2A_MEMORY_TRANSFER_DDR_START_ADDR - start address for compiler
 */
#define XH2A_MEMORY_TRANSFER_DDR_START_ADDR XH2A_DEVICE_DDR_START
#define XH2A_MEMORY_TRANSFER_DDR_TOTAL_SIZE XH2A_DEVICE_DDR_SIZE

/*
 * XH2A_MEMORY_TRANSFER_SPM0_START_ADDR - start address of SPM0
 * XH2A_MEMORY_TRANSFER_SPM0_TOTAL_SIZE - size of SPM0
 */
#define XH2A_MEMORY_TRANSFER_SPM0_START_ADDR XH2A_DEVICE_SPM0_START
#define XH2A_MEMORY_TRANSFER_SPM0_TOTAL_SIZE XH2A_DEVICE_SPM0_SIZE

/*
 * XH2A_MEMORY_TRANSFER_SPM1_START_ADDR - start address of SPM1
 * XH2A_MEMORY_TRANSFER_SPM1_TOTAL_SIZE - size of SPM1
 */
#define XH2A_MEMORY_TRANSFER_SPM1_START_ADDR XH2A_DEVICE_SPM1_START
#define XH2A_MEMORY_TRANSFER_SPM1_TOTAL_SIZE XH2A_DEVICE_SPM1_SIZE

/*
 * XH2A_MEMORY_TRANSFER_TIM1_START_ADDR - start address of TIM1
 * XH2A_MEMORY_TRANSFER_TIM1_TOTAL_SIZE - size of TIM1
 */
#define XH2A_MEMORY_TRANSFER_TIM1_START_ADDR XH2A_DEVICE_TIM1_START
#define XH2A_MEMORY_TRANSFER_TIM1_TOTAL_SIZE XH2A_DEVICE_TIM1_SIZE

#endif
