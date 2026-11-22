// SPDX-License-Identifier: GPL-2.0
/*++

Copyright(c) 2023 Houmo AI Inc. All rights reserved.

Module Name:

	xh2a_memory_transfer_internal.h

Abstract:

	This module contains the common declarations shared by driver
	and HAL for Memory Transfer module internal used.

	Since this file is shared with the HAL layer,
	any modifications to its content must be very carefully
	synchronized with corresponding changes in the HAL layer.

Environment:

	user and kernel

--*/
#ifndef _XH2A_MEMORY_TRANSFER_INTERNAL_H_
#define _XH2A_MEMORY_TRANSFER_INTERNAL_H_

/*
 * XH2A_MEMORY_TRANSFER_DEVICE_NAME - unique name of xh2a memory transfer
 */
#define XH2A_MEMORY_TRANSFER_DEVICE_NAME "xh2a_memory_transfer_device"

/*
 * driver_memory_transfer_type - memory transfer type, public to user
 * DRIVER_MEMORY_TRANSFER_TYPE_HOST_TO_DEVICE - host to device
 * DRIVER_MEMORY_TRANSFER_TYPE_DEVICE_TO_HOST - device to host
 * DRIVER_MEMORY_TRANSFER_TYPE_INNER_DEVICE - inner device
 * DRIVER_MEMORY_TRANSFER_TYPE_PEER_TO_PEER - peer to peer, MRd method
 * DRIVER_MEMORY_TRANSFER_TYPE_PEER_TO_PEER_ALT - peer to peer, MWr method
 */
enum driver_memory_transfer_type {
	DRIVER_MEMORY_TRANSFER_TYPE_HOST_TO_DEVICE = 0,
	DRIVER_MEMORY_TRANSFER_TYPE_DEVICE_TO_HOST = 1,
	DRIVER_MEMORY_TRANSFER_TYPE_INNER_DEVICE = 2,
};

/*
 * xh2a_memory_transfer_ops - memory transfer operations
 */
enum xh2a_memory_transfer_ops {
	XH2A_MEMORY_TRANSFER_COPY_BUFFER = 0,
	XH2A_MEMORY_TRANSFER_MEMBAR = 1,
	XH2A_MEMORY_TRANSFER_MEMSET = 2,
};

enum driver_memory_transfer_type_internal {
	DRIVER_MEMORY_TRANSFER_TYPE_PEER_TO_PEER = 3,
	DRIVER_MEMORY_TRANSFER_TYPE_PEER_TO_PEER_ALT = 4,
};

/*
 * struct xh2a_memory_transfer_request_arg - ioctl arg for transfer
 * @src_addr: start address of source
 * @dst_addr: start address of destination
 * @size: size of destination
 * @type: transfer type/transfer type internal, d2h/h2d/d2d
 */
struct xh2a_memory_transfer_request_arg {
	uint64_t src_addr;
	uint64_t dst_addr;
	uint64_t size;
	int32_t type;
};

/*
 * struct xh2a_memory_transfer_memset_arg - ioctl arg for transfer memset
 * @paddr: start address to memset
 * @c: value to set
 * @size: size to memset
 */
struct xh2a_memory_transfer_memset_arg {
	uint64_t paddr;
	uint64_t size;
	int c;
};

/*
 * xh2a_memory_transfer_membar_request_type - request for membar access
 * XH2A_MEMORY_TRANSFER_MEMBAR_GETINFO - get bar information
 * XH2A_MEMORY_TRANSFER_MEMBAR_MAP - set membar mapping
 * XH2A_MEMORY_TRANSFER_MEMBAR_UNMAP - unset membar mapping
 * XH2A_MEMORY_TRANSFER_MEMBAR_LOCK_DEV - lock device
 * XH2A_MEMORY_TRANSFER_MEMBAR_UNLOCK_DEV - unlock device
 */
enum xh2a_memory_transfer_membar_request_type {
	XH2A_MEMORY_TRANSFER_MEMBAR_GETINFO,
	XH2A_MEMORY_TRANSFER_MEMBAR_MAP,
	XH2A_MEMORY_TRANSFER_MEMBAR_UNMAP,
	XH2A_MEMORY_TRANSFER_MEMBAR_LOCK_DEV,
	XH2A_MEMORY_TRANSFER_MEMBAR_UNLOCK_DEV,
};

/*
 * struct xh2a_memory_transfer_membar_arg - ioctl arg for membar lock
 * @paddr: start address in xh2a's address space
 * @bar_addr: bar address in host's address space
 * @bar_size: bar size
 * @type: request type, getinfo / lock / unlock
 */
struct xh2a_memory_transfer_membar_arg {
	uint64_t paddr;
	uint64_t bar_addr;
	uint64_t bar_size;
	enum xh2a_memory_transfer_membar_request_type type;
};

#define XH2A_MEMORY_TRANSFER_IOC_MAGIC 'B'

#define IOCTL_XH2A_MEMORY_TRANSFER_COPY_BUFFER  \
	_IOWR(XH2A_MEMORY_TRANSFER_IOC_MAGIC,   \
	      XH2A_MEMORY_TRANSFER_COPY_BUFFER, \
	      struct xh2a_memory_transfer_request_arg)

#define IOCTL_XH2A_MEMORY_TRANSFER_MEMBAR                                  \
	_IOWR(XH2A_MEMORY_TRANSFER_IOC_MAGIC, XH2A_MEMORY_TRANSFER_MEMBAR, \
	      struct xh2a_memory_transfer_membar_arg)

#define IOCTL_XH2A_MEMORY_TRANSFER_MEMSET                                  \
	_IOWR(XH2A_MEMORY_TRANSFER_IOC_MAGIC, XH2A_MEMORY_TRANSFER_MEMSET, \
	      struct xh2a_memory_transfer_memset_arg)

/*
 * p2p_status - P2P capability status for a device pair
 * P2P_UNKNOWN            - not yet probed, try P2P path
 * P2P_SUPPORTED          - P2P path confirmed working
 * P2P_UNSUPPORTED_DISTANCE - P2P unavailable due to PCIe topology/distance
 * P2P_UNSUPPORTED_IOMMU   - P2P unavailable due to IOMMU restriction
 */
enum driver_memory_transfer_p2p_status {
	DRIVER_MEMORY_TRANSFER_P2P_UNKNOWN = 0,
	DRIVER_MEMORY_TRANSFER_P2P_SUPPORTED = 1,
	DRIVER_MEMORY_TRANSFER_P2P_UNSUPPORTED_DISTANCE = 2,
	DRIVER_MEMORY_TRANSFER_P2P_UNSUPPORTED_IOMMU = 3,
};

/*
 * struct xh2a_memory_transfer_p2p_status_arg - ioctl arg for P2P status
 * @src_devid: source device id (minor number)
 * @dst_devid: destination device id (minor number)
 * @status: P2P status, see enum xh2a_memory_transfer_p2p_status
 */
struct xh2a_memory_transfer_p2p_status_arg {
	uint32_t src_devid;
	uint32_t dst_devid;
	uint32_t status;
};

#define IOCTL_XH2A_MEMORY_TRANSFER_P2P_STATUS    \
	_IOWR(XH2A_MEMORY_TRANSFER_IOC_MAGIC, 3, \
	      struct xh2a_memory_transfer_p2p_status_arg)

#endif /*  !_XH2A_MEMORY_TRANSFER_INTERNAL_H_ */
