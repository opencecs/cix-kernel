// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2025 Houmo AI Inc.
 * Author: mingdong.wang<mingdong.wang@houmo.ai>
 */

#ifndef _XH2A_IPU_IOCTL_H_
#define _XH2A_IPU_IOCTL_H_

#include <xh2a_ipu_internal.h>
#include "xh2a_ipu_kernel.h"
#include "xh2a_ipu_group.h"
#include "xh2a_ipu_config.h"

struct xh2a_ipu_group;
struct xh2a_group_result {
	uint32_t group_num;
	uint32_t cur;
	struct {
		int group_id;
		struct xh2a_ipu_group *group; /* just for debug and test */
		uint32_t buf[XH2A_GROUP_RESULT_BUF_SZ];
	} result[XH2A_GROUP_RESULT_NUM];
};

enum xh2a_ipu_reset_mode {
	XH2A_IPU_RESET_MODE_REPLAY = 0,
	XH2A_IPU_RESET_MODE_DISCARD = 1,
};

struct xh2a_ipu_reset_req {
	uint32_t mode;
	uint32_t flags;
	uint32_t reserved[2];
};

/* ioctl api */
long xh2a_ipu_ioctl_dispatch(struct file *filp, unsigned int cmd,
			     unsigned long arg);

#endif /* _XH2A_IPU_IOCTL_H_ */