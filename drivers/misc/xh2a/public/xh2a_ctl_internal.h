// SPDX-License-Identifier: GPL-2.0
/*++

Copyright(c) 2026 Houmo AI Inc. All rights reserved.

Module Name:

	xh2a_ctl_internal.h

Abstract:

	This module contains the declarations shared by driver
	and HAL for IPU module for internal used.

	Since this file is shared with the HAL layer,
	any modifications to its content must be very carefully
	synchronized with corresponding changes in the HAL layer.

Environment:

	user and kernel

--*/

#ifndef XH2A_CTL_INTERNAL_H_
#define XH2A_CTL_INTERNAL_H_

#define XH2A_CTL_DEVICE_NAME "xh2a_ctl"

#if defined(__KERNEL__)
#include <linux/ioctl.h>
#include <linux/types.h>
#else
#include <stdint.h>
#include <sys/ioctl.h>
#endif

/* Maximum device IDs accepted by a CTL lock request. */
#define XH2A_CTL_MAX_DEVICES 256U

/**
 * struct xh2a_ctl_lock_req - Variable-length lock/unlock request for xh2a ctl.
 * @nbits:       Number of valid bits in @words, in the range 1..256.
 * @flags:       Flags for lock operation (e.g., non-blocking).
 * @timeout_ms:  Timeout in milliseconds; 0 means infinite wait in
 *               blocking mode.
 * @reserved:    Reserved for future extensions; must be zero.
 * @words:       Little-endian 64-bit host-device bitmap words.
 */
struct xh2a_ctl_lock_req {
	uint32_t nbits;
	uint32_t flags;
	uint32_t timeout_ms;
	uint32_t reserved;
	uint64_t words[];
};

struct xh2a_ctl_ctc_info {
	int chip_id;
	int group_id;
	int group_size;
	int group_num;
	int is_single;
};

struct xh2a_ctl_ctc_get_req {
	uint32_t dev_id;
	uint32_t reserved;
	struct xh2a_ctl_ctc_info info;
};

#define XH2A_CTL_IOCTL_MAGIC 'c'

#define IOCTL_XH2A_CTL_LOCK_DEV \
	_IOW(XH2A_CTL_IOCTL_MAGIC, 0xA0, struct xh2a_ctl_lock_req)
#define IOCTL_XH2A_CTL_UNLOCK_DEV \
	_IOW(XH2A_CTL_IOCTL_MAGIC, 0xA1, struct xh2a_ctl_lock_req)
#define IOCTL_XH2A_CTL_CTC_INIT _IO(XH2A_CTL_IOCTL_MAGIC, 0xB0)
#define IOCTL_XH2A_CTL_CTC_GET \
	_IOWR(XH2A_CTL_IOCTL_MAGIC, 0xB1, struct xh2a_ctl_ctc_get_req)

#endif /*  !XH2A_CTL_INTERNAL_H_ */
