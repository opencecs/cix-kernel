/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_OPENCECS_LIFECYCLE_H
#define _UAPI_LINUX_OPENCECS_LIFECYCLE_H
#include <linux/ioctl.h>
#include <linux/types.h>
#define OPENCECS_EMERGENCY_VERSION 1U
#define OPENCECS_EMERGENCY_RESTART 1U
#define OPENCECS_EMERGENCY_FASTBOOT 2U
#define OPENCECS_EMERGENCY_ACK_UNCLEAN 1U
/* Fixed native/compat ABI. No pointer, arbitrary reboot string, physical
 * address, timeout or hardware register operand is accepted. */
struct opencecs_emergency_restart {
 __u32 version;
 __u32 action;
 __u32 flags;
 __u32 reserved;
};
#define OPENCECS_IOC_EMERGENCY_RESTART \
 _IOW(0xCE, 0x01, struct opencecs_emergency_restart)
#endif
