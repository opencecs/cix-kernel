// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2025 Houmo AI Inc.
 * Author: mingdong.wang<mingdong.wang@houmo.ai>
 */

#ifndef _XH2A_IPU_POLICY_H_
#define _XH2A_IPU_POLICY_H_

struct xh2a_ipu_device;
struct xh2a_ipu_policy;
struct xh2a_ipu_group;

enum xh2a_ipu_policy_result {
	XH2A_POLICY_AVAILABLE = 0,
	XH2A_POLICY_NO_RESOURCE,
	XH2A_POLICY_BUSY_RETRY,
};

struct xh2a_ipu_policy_ops {
	void (*enqueue_group)(struct xh2a_ipu_policy *,
			      struct xh2a_ipu_group *);
	void (*requeue_group_head)(struct xh2a_ipu_policy *,
				   struct xh2a_ipu_group *);
	struct xh2a_ipu_group *(*pick_runnable_group)(
		struct xh2a_ipu_policy *, enum xh2a_ipu_policy_result *result);
	enum xh2a_ipu_policy_result (*resource_enough)(
		struct xh2a_ipu_group *);
	void (*remove_group)(struct xh2a_ipu_policy *, struct xh2a_ipu_group *);
	enum xh2a_ipu_policy_result (*can_allocate_entries)(
		struct xh2a_ipu_policy *, int core_id, int queue_id,
		int entry_num_needed, int *remain_queue_entries);
	void (*cancel_all)(struct xh2a_ipu_policy *);
	struct xh2a_ipu_group *(*find_pending_group)(struct xh2a_ipu_policy *);
};

struct xh2a_ipu_policy {
	struct xh2a_ipu_device *ipu_dev;
	const struct xh2a_ipu_policy_ops *ops;
};

/* policy api */
struct xh2a_ipu_policy *xh2a_ipu_policy_create(struct xh2a_ipu_device *ipu_dev);
void xh2a_ipu_policy_destroy(struct xh2a_ipu_policy *policy);
void xh2a_ipu_policy_delete_group(struct xh2a_ipu_group *group);
void xh2a_ipu_policy_cancel_all(struct xh2a_ipu_policy *policy);
struct xh2a_ipu_group *xh2a_ipu_policy_find_pending_group(
	struct xh2a_ipu_policy *policy);

#endif /* _XH2A_IPU_POLICY_H_ */
