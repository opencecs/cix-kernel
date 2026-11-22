// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2024 Houmo AI Inc.
 * Author: hongxing.ma<hongxing.ma@houmo.ai>
 */

#ifndef __XH2A_MEMORY_TRANSFER_SYSDMA_H__
#define __XH2A_MEMORY_TRANSFER_SYSDMA_H__

#include <linux/types.h>
#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/wait.h>

#define XH2A_MEMORY_TRANSFER_SYSDMA_CHANNEL_NUM	     4
#define XH2A_MEMORY_TRANSFER_SYSDMA_FAST_CHANNEL_NUM 4

struct xh2a_memory_transfer_sysdma_handle {
	struct device *dev;
	struct mutex sysdma_mutex;
	/* Serializes channel configuration/START with PM stop confirmation. */
	struct mutex start_stop_mutex;
	unsigned long chan_bitmap[BITS_TO_LONGS(
		XH2A_MEMORY_TRANSFER_SYSDMA_CHANNEL_NUM)];
	struct wait_queue_head chan_avail_wq;
	struct completion
		chan_completion[XH2A_MEMORY_TRANSFER_SYSDMA_CHANNEL_NUM];
	atomic_t chan_removed;
	atomic_t suspend_in_progress;
	/* Old waiters remain cancelled even after an early PM rollback. */
	atomic_t stop_generation;
	struct work_struct sysdma_work;
	void *private_data;
};

int xh2a_memory_transfer_sysdma_init(
	struct xh2a_memory_transfer_sysdma_handle *handle, struct device *dev,
	void *private_data);

int xh2a_memory_transfer_sysdma_deinit(
	struct xh2a_memory_transfer_sysdma_handle *handle);

int xh2a_memory_transfer_sysdma_memcpy(
	struct xh2a_memory_transfer_sysdma_handle *handle, uint64_t src_addr,
	uint64_t dst_addr, uint64_t size);

int xh2a_memory_transfer_sysdma_memset(
	struct xh2a_memory_transfer_sysdma_handle *handle, uint64_t paddr,
	uint64_t size, int c);

int xh2a_memory_transfer_sysdma_pm_prepare(
	struct xh2a_memory_transfer_sysdma_handle *handle);

int xh2a_memory_transfer_sysdma_pm_complete(
	struct xh2a_memory_transfer_sysdma_handle *handle);

int xh2a_memory_transfer_sysdma_stop(
	struct xh2a_memory_transfer_sysdma_handle *handle);

void xh2a_memory_transfer_sysdma_resume(
	struct xh2a_memory_transfer_sysdma_handle *handle);

#endif
