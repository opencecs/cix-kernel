// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2026 Houmo AI Inc. */

#include <linux/device.h>
#include <linux/jiffies.h>
#include <linux/kref.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/slab.h>
#include <linux/wait.h>

#include <xh2a_ctl_internal.h>
#include "xh2a_ctl.h"
#include "xh2a_ctl_dev.h"

static bool xh2a_ctl_mask_test(const uint64_t *words, uint32_t bit)
{
	return words[bit / 64] & (1ULL << (bit % 64));
}

static bool xh2a_ctl_mask_valid(const uint64_t *words, uint32_t nbits)
{
	uint32_t nwords;
	uint64_t tail_mask;

	if (!words || !nbits || nbits > XH2A_CTL_MAX_DEVICES)
		return false;
	nwords = DIV_ROUND_UP(nbits, 64);
	if (nbits % 64) {
		tail_mask = (1ULL << (nbits % 64)) - 1;
		if (words[nwords - 1] & ~tail_mask)
			return false;
	}
	while (nwords--)
		if (words[nwords])
			return true;
	return false;
}

static bool xh2a_ctl_owner_alive(struct pid *owner_pid)
{
	struct task_struct *task;

	if (!owner_pid)
		return false;
	task = get_pid_task(owner_pid, PIDTYPE_TGID);
	if (!task)
		return false;
	put_task_struct(task);
	return true;
}

static struct xh2a_ctl_dev_ctx *
xh2a_ctl_get_lock_ctx(struct xh2a_ctl_ctx *ctl, uint32_t dev_id)
{
	struct xh2a_ctl_dev_ctx *ctl_ctx;

	mutex_lock(&ctl->lock);
	list_for_each_entry(ctl_ctx, &ctl->ctl_list, node) {
		if (ctl_ctx->dev_id == dev_id) {
			xh2a_ctl_ctx_get(ctl_ctx);
			mutex_unlock(&ctl->lock);
			return ctl_ctx;
		}
	}
	mutex_unlock(&ctl->lock);
	return NULL;
}

static struct xh2a_ctl_dev_ctx *
xh2a_ctl_find_remove_lock_ctx(struct xh2a_ctl_ctx *ctl, uint32_t dev_id)
{
	struct xh2a_ctl_dev_ctx *ctl_ctx, *tmp;

	mutex_lock(&ctl->lock);
	list_for_each_entry_safe(ctl_ctx, tmp, &ctl->ctl_list, node) {
		if (ctl_ctx->dev_id == dev_id) {
			list_del_init(&ctl_ctx->node);
			mutex_unlock(&ctl->lock);
			return ctl_ctx;
		}
	}
	mutex_unlock(&ctl->lock);
	return NULL;
}

struct xh2a_ctl_dev_ctx *xh2a_ctl_create_lock(struct xh2a_ctl_ctx *ctl,
					      uint32_t dev_id, void *handle)
{
	struct xh2a_ctl_dev_ctx *ctl_ctx;

	ctl_ctx = kzalloc(sizeof(*ctl_ctx), GFP_KERNEL);
	if (!ctl_ctx)
		return NULL;
	kref_init(&ctl_ctx->kref);
	mutex_init(&ctl_ctx->lock);
	init_waitqueue_head(&ctl_ctx->wq);
	ctl_ctx->online = true;
	ctl_ctx->dev_id = dev_id;
	ctl_ctx->pcie_dev = handle;
	INIT_LIST_HEAD(&ctl_ctx->node);
	return ctl_ctx;
}

static void xh2a_ctl_clear_owner_locked(struct xh2a_ctl_dev_ctx *ctl_ctx)
{
	struct pid *owner_pid = ctl_ctx->owner_pid;

	ctl_ctx->owner_pid = NULL;
	ctl_ctx->refcnt = 0;
	if (owner_pid)
		put_pid(owner_pid);
}

void xh2a_ctl_destroy_lock(struct xh2a_ctl_ctx *ctl, uint32_t dev_id)
{
	struct xh2a_ctl_dev_ctx *ctl_ctx;

	if (!ctl)
		return;
	ctl_ctx = xh2a_ctl_find_remove_lock_ctx(ctl, dev_id);
	if (!ctl_ctx)
		return;
	mutex_lock(&ctl_ctx->lock);
	ctl_ctx->online = false;
	ctl_ctx->pcie_dev = NULL;
	xh2a_ctl_clear_owner_locked(ctl_ctx);
	mutex_unlock(&ctl_ctx->lock);
	wake_up_all(&ctl_ctx->wq);
	xh2a_ctl_ctx_put(ctl_ctx);
}

static int xh2a_ctl_try_lock_one(struct xh2a_ctl_dev_ctx *ctl_ctx,
				  struct pid *owner_pid)
{
	bool reclaimed = false;
	int ret = 0;

	mutex_lock(&ctl_ctx->lock);
	if (!ctl_ctx->online) {
		ret = -ENODEV;
	} else if (ctl_ctx->owner_pid && ctl_ctx->owner_pid != owner_pid &&
		   !xh2a_ctl_owner_alive(ctl_ctx->owner_pid)) {
		xh2a_ctl_clear_owner_locked(ctl_ctx);
		reclaimed = true;
	}
	if (!ret) {
		if (!ctl_ctx->owner_pid) {
			ctl_ctx->owner_pid = get_pid(owner_pid);
			ctl_ctx->refcnt = 1;
		} else if (ctl_ctx->owner_pid == owner_pid) {
			ctl_ctx->refcnt++;
		} else {
			ret = -EBUSY;
		}
	}
	mutex_unlock(&ctl_ctx->lock);
	if (reclaimed)
		wake_up_all(&ctl_ctx->wq);
	return ret;
}

static int xh2a_ctl_unlock_one(struct xh2a_ctl_dev_ctx *ctl_ctx,
				 struct pid *owner_pid, bool *wake)
{
	int ret = 0;

	*wake = false;
	mutex_lock(&ctl_ctx->lock);
	if (!ctl_ctx->online)
		ret = -ENODEV;
	else if (!ctl_ctx->owner_pid || ctl_ctx->owner_pid != owner_pid)
		ret = -EPERM;
	else if (!ctl_ctx->refcnt)
		ret = -EINVAL;
	else if (--ctl_ctx->refcnt == 0) {
		xh2a_ctl_clear_owner_locked(ctl_ctx);
		*wake = true;
	}
	mutex_unlock(&ctl_ctx->lock);
	return ret;
}

static bool xh2a_ctl_lock_is_ready(struct xh2a_ctl_dev_ctx *ctl_ctx)
{
	return !READ_ONCE(ctl_ctx->online) || !READ_ONCE(ctl_ctx->owner_pid);
}

static int xh2a_ctl_get_mask_ctxs(struct xh2a_ctl_ctx *ctl,
					  const uint64_t *words, uint32_t nbits,
					  struct xh2a_ctl_dev_ctx **ctxs)
{
	uint32_t i;

	for (i = 0; i < nbits; i++) {
		ctxs[i] = NULL;
		if (!xh2a_ctl_mask_test(words, i))
			continue;
		ctxs[i] = xh2a_ctl_get_lock_ctx(ctl, i);
		if (!ctxs[i])
			goto err;
	}
	return 0;
err:
	while (i--)
		if (ctxs[i])
			xh2a_ctl_ctx_put(ctxs[i]);
	return -ENODEV;
}

static void xh2a_ctl_put_mask_ctxs(struct xh2a_ctl_dev_ctx **ctxs,
					   uint32_t nbits)
{
	uint32_t i;

	for (i = 0; i < nbits; i++)
		if (ctxs[i])
			xh2a_ctl_ctx_put(ctxs[i]);
}

static void xh2a_ctl_rollback_mask(struct xh2a_ctl_dev_ctx **ctxs,
				    const uint64_t *acquired, uint32_t nbits,
				    struct pid *owner_pid)
{
	uint32_t i = nbits;

	while (i--) {
		bool wake;

		if (!xh2a_ctl_mask_test(acquired, i))
			continue;
		if (!xh2a_ctl_unlock_one(ctxs[i], owner_pid, &wake) && wake)
			wake_up_all(&ctxs[i]->wq);
	}
}

static long xh2a_ctl_try_lock_mask(struct xh2a_ctl_dev_ctx **ctxs,
				    const uint64_t *words, uint32_t nbits,
				    struct pid *owner_pid, uint32_t *conflict_dev)
{
	uint32_t nwords = DIV_ROUND_UP(nbits, 64);
	uint64_t *acquired;
	uint32_t i;
	long ret = 0;

	acquired = kcalloc(nwords, sizeof(*acquired), GFP_KERNEL);
	if (!acquired)
		return -ENOMEM;
	for (i = 0; i < nbits; i++) {
		if (!xh2a_ctl_mask_test(words, i))
			continue;
		ret = xh2a_ctl_try_lock_one(ctxs[i], owner_pid);
		if (ret) {
			xh2a_ctl_rollback_mask(ctxs, acquired, nbits, owner_pid);
			*conflict_dev = i;
			goto out;
		}
		acquired[i / 64] |= 1ULL << (i % 64);
	}
out:
	kfree(acquired);
	return ret;
}

static bool xh2a_ctl_current_is_owner(
	struct xh2a_ctl_process_lock_ctx *process_lock_ctx)
{
	struct pid *current_pid = get_task_pid(current, PIDTYPE_TGID);
	bool match = current_pid == process_lock_ctx->owner_pid;

	if (current_pid)
		put_pid(current_pid);
	return match;
}

/* Returns with @process_lock_ctx->op_lock held when a LOCK turn is claimed. */
static long xh2a_ctl_claim_lock(
	struct xh2a_ctl_process_lock_ctx *process_lock_ctx, uint32_t flags,
	bool has_timeout, unsigned long deadline)
{
	long ret;

	for (;;) {
		mutex_lock(&process_lock_ctx->op_lock);
		if (!READ_ONCE(process_lock_ctx->lock_pending)) {
			WRITE_ONCE(process_lock_ctx->lock_pending, true);
			return 0;
		}
		mutex_unlock(&process_lock_ctx->op_lock);
		if (flags & XH2A_LOCK_F_NONBLOCK)
			return -EBUSY;
		if (has_timeout) {
			long left;

			if (time_after_eq(jiffies, deadline))
				return -ETIMEDOUT;
			left = wait_event_interruptible_timeout(
				process_lock_ctx->lock_waitq,
				!READ_ONCE(process_lock_ctx->lock_pending),
				deadline - jiffies);
			if (left == 0)
				return -ETIMEDOUT;
			if (left < 0)
				return left;
		} else {
			ret = wait_event_interruptible(
				process_lock_ctx->lock_waitq,
				!READ_ONCE(process_lock_ctx->lock_pending));
			if (ret)
				return ret;
		}
	}
}

static void xh2a_ctl_complete_lock(
	struct xh2a_ctl_process_lock_ctx *process_lock_ctx)
{
	WRITE_ONCE(process_lock_ctx->lock_pending, false);
	wake_up_all(&process_lock_ctx->lock_waitq);
}

static long xh2a_ctl_wait_topology_ready(
	struct xh2a_ctl_ctx *ctl,
	struct xh2a_ctl_process_lock_ctx *process_lock_ctx, uint32_t flags,
	bool has_timeout, unsigned long deadline)
{
	long ret;

	if (flags & XH2A_LOCK_F_NONBLOCK)
		return -EAGAIN;

	/* Do not stall same-process unlock or final-FD cleanup while waiting. */
	mutex_unlock(&process_lock_ctx->op_lock);
	if (has_timeout) {
		if (time_after_eq(jiffies, deadline))
			ret = -ETIMEDOUT;
		else
			ret = xh2a_ctl_ctc_wait_ready_timeout(ctl,
						       deadline - jiffies);
	} else {
		ret = xh2a_ctl_ctc_wait_ready(ctl, 0);
	}
	mutex_lock(&process_lock_ctx->op_lock);

	return ret;
}

static long xh2a_ctl_lock_bitmap(struct file *filp, const uint64_t *words,
				  uint32_t nbits, uint32_t flags, uint32_t timeout_ms)
{
	struct xh2a_ctl_file_ctx *file_ctx = filp->private_data;
	struct xh2a_ctl_ctx *ctl = xh2a_ctl_file_ctx_get(filp);
	struct xh2a_ctl_process_lock_ctx *process_lock_ctx;
	struct xh2a_ctl_dev_ctx **ctxs;
	unsigned long deadline = 0;
	long ret;
	uint32_t conflict_dev = 0;

	if (!ctl || !file_ctx || !xh2a_ctl_mask_valid(words, nbits) ||
	    flags & ~XH2A_LOCK_F_NONBLOCK)
		return -EINVAL;
	process_lock_ctx = file_ctx->process_lock_ctx;
	if (!process_lock_ctx || !xh2a_ctl_current_is_owner(process_lock_ctx))
		return -EPERM;
	ctxs = kcalloc(nbits, sizeof(*ctxs), GFP_KERNEL);
	if (!ctxs)
		return -ENOMEM;

	if (timeout_ms)
		deadline = jiffies + msecs_to_jiffies(timeout_ms);
	ret = xh2a_ctl_claim_lock(process_lock_ctx, flags, !!timeout_ms,
				  deadline);
	if (ret)
		goto out_free_ctxs;
	ret = xh2a_ctl_get_mask_ctxs(ctl, words, nbits, ctxs);
	if (ret)
		goto out_complete_turn;
	for (;;) {
		/*
		 * Serialize topology validation and ownership changes with PCIe
		 * lifecycle operations.  Waiting always happens after this mutex is
		 * released, so remove and topology rebuild can make progress.
		 */
		mutex_lock(&ctl->ctc_mutex);
		ret = xh2a_ctl_ctc_check_ready(ctl);
		if (!ret)
			ret = xh2a_ctl_try_lock_mask(ctxs, words, nbits,
						     process_lock_ctx->owner_pid,
						     &conflict_dev);
		mutex_unlock(&ctl->ctc_mutex);

		if (ret == -EAGAIN) {
			ret = xh2a_ctl_wait_topology_ready(ctl, process_lock_ctx,
							    flags, !!timeout_ms,
							    deadline);
			if (!ret)
				continue;
			break;
		}
		if (ret != -EBUSY || (flags & XH2A_LOCK_F_NONBLOCK))
			break;
		if (timeout_ms) {
			long left;

			if (time_after_eq(jiffies, deadline)) {
				ret = -ETIMEDOUT;
				break;
			}
			mutex_unlock(&process_lock_ctx->op_lock);
			left = wait_event_interruptible_timeout(ctxs[conflict_dev]->wq,
				xh2a_ctl_lock_is_ready(ctxs[conflict_dev]),
				deadline - jiffies);
			mutex_lock(&process_lock_ctx->op_lock);
			if (left == 0) {
				ret = -ETIMEDOUT;
				break;
			}
			if (left < 0) {
				ret = left;
				break;
			}
		} else {
			mutex_unlock(&process_lock_ctx->op_lock);
			ret = wait_event_interruptible(ctxs[conflict_dev]->wq,
				xh2a_ctl_lock_is_ready(ctxs[conflict_dev]));
			mutex_lock(&process_lock_ctx->op_lock);
			if (ret)
				break;
		}
	}
	xh2a_ctl_put_mask_ctxs(ctxs, nbits);
out_complete_turn:
	xh2a_ctl_complete_lock(process_lock_ctx);
	mutex_unlock(&process_lock_ctx->op_lock);
out_free_ctxs:
	kfree(ctxs);
	return ret;
}

static long xh2a_ctl_unlock_bitmap(struct file *filp,
				    const uint64_t *words, uint32_t nbits)
{
	struct xh2a_ctl_file_ctx *file_ctx = filp->private_data;
	struct xh2a_ctl_ctx *ctl = xh2a_ctl_file_ctx_get(filp);
	struct xh2a_ctl_process_lock_ctx *process_lock_ctx;
	struct xh2a_ctl_dev_ctx **ctxs;
	long ret = 0;
	uint32_t i;

	if (!ctl || !file_ctx || !xh2a_ctl_mask_valid(words, nbits))
		return -EINVAL;
	process_lock_ctx = file_ctx->process_lock_ctx;
	if (!process_lock_ctx || !xh2a_ctl_current_is_owner(process_lock_ctx))
		return -EPERM;
	ctxs = kcalloc(nbits, sizeof(*ctxs), GFP_KERNEL);
	if (!ctxs)
		return -ENOMEM;

	mutex_lock(&process_lock_ctx->op_lock);
	ret = xh2a_ctl_get_mask_ctxs(ctl, words, nbits, ctxs);
	if (ret)
		goto out_unlock;
	/* Validate the whole set first so an invalid mask cannot partly unlock. */
	for (i = 0; i < nbits; i++) {
		if (!xh2a_ctl_mask_test(words, i))
			continue;
		mutex_lock(&ctxs[i]->lock);
		if (!ctxs[i]->online)
			ret = -ENODEV;
		else if (!ctxs[i]->owner_pid ||
			 ctxs[i]->owner_pid != process_lock_ctx->owner_pid)
			ret = -EPERM;
		else if (!ctxs[i]->refcnt)
			ret = -EINVAL;
		mutex_unlock(&ctxs[i]->lock);
		if (ret)
			goto out_ctxs;
	}
	for (i = 0; i < nbits; i++) {
		bool wake;
		long unlock_ret;

		if (!xh2a_ctl_mask_test(words, i))
			continue;
		unlock_ret = xh2a_ctl_unlock_one(ctxs[i],
						process_lock_ctx->owner_pid, &wake);
		if (unlock_ret && !ret)
			ret = unlock_ret;
		if (wake)
			wake_up_all(&ctxs[i]->wq);
	}
out_ctxs:
	xh2a_ctl_put_mask_ctxs(ctxs, nbits);
out_unlock:
	mutex_unlock(&process_lock_ctx->op_lock);
	kfree(ctxs);
	return ret;
}

long xh2a_ctl_lock_dev(struct file *filp, const struct xh2a_ctl_lock_req *req)
{
	if (!req || req->reserved)
		return -EINVAL;
	return xh2a_ctl_lock_bitmap(filp, req->words, req->nbits, req->flags,
				    req->timeout_ms);
}

long xh2a_ctl_unlock_dev(struct file *filp,
			 const struct xh2a_ctl_lock_req *req)
{
	if (!req || req->flags || req->timeout_ms || req->reserved)
		return -EINVAL;
	return xh2a_ctl_unlock_bitmap(filp, req->words, req->nbits);
}

void xh2a_ctl_release_locks_locked(
	struct xh2a_ctl_ctx *ctl,
	struct xh2a_ctl_process_lock_ctx *process_lock_ctx)
{
	struct xh2a_ctl_dev_ctx *ctl_ctx;

	if (!ctl || !process_lock_ctx)
		return;
	list_for_each_entry(ctl_ctx, &ctl->ctl_list, node) {
		bool wake = false;

		mutex_lock(&ctl_ctx->lock);
		if (ctl_ctx->owner_pid == process_lock_ctx->owner_pid) {
			xh2a_ctl_clear_owner_locked(ctl_ctx);
			wake = true;
		}
		mutex_unlock(&ctl_ctx->lock);
		if (wake)
			wake_up_all(&ctl_ctx->wq);
	}
}

void xh2a_ctl_release_group_locks_locked(struct xh2a_ctl_ctx *ctl,
					     uint32_t dev_id)
{
	struct xh2a_ctl_dev_ctx *target;
	struct xh2a_ctl_dev_ctx *ctl_ctx;
	struct xh2a_ctl_ctc_group_entry *group;
	struct pid *owner = NULL;
	bool in_group = false;

	if (!ctl)
		return;

	/* ctl->lock serializes the list and the CTC group snapshot. */
	target = NULL;
	list_for_each_entry(ctl_ctx, &ctl->ctl_list, node) {
		if (ctl_ctx->dev_id == dev_id) {
			target = ctl_ctx;
			break;
		}
	}
	if (!target)
		return;

	mutex_lock(&target->lock);
	if (target->owner_pid)
		owner = get_pid(target->owner_pid);
	mutex_unlock(&target->lock);
	if (!owner)
		return;

	list_for_each_entry(group, &ctl->ctc_group_list, node) {
		uint32_t i;

		for (i = 0; i < group->chip_num; i++) {
			if (group->live_dev_id[i] == dev_id) {
				in_group = true;
				break;
			}
		}
		if (in_group)
			break;
	}

	list_for_each_entry(ctl_ctx, &ctl->ctl_list, node) {
		bool member = ctl_ctx->dev_id == dev_id;
		bool wake = false;
		uint32_t i;

		if (!member && in_group) {
			for (i = 0; i < group->chip_num; i++) {
				if (group->live_dev_id[i] == ctl_ctx->dev_id) {
					member = true;
					break;
				}
			}
		}
		if (!member)
			continue;

		mutex_lock(&ctl_ctx->lock);
		if (ctl_ctx->owner_pid == owner) {
			xh2a_ctl_clear_owner_locked(ctl_ctx);
			wake = true;
		}
		mutex_unlock(&ctl_ctx->lock);
		if (wake)
			wake_up_all(&ctl_ctx->wq);
	}
	put_pid(owner);
}

void xh2a_ctl_cleanup_all_locks(struct xh2a_ctl_ctx *ctl)
{
	struct xh2a_ctl_dev_ctx *cur, *tmp;

	if (!ctl)
		return;
	mutex_lock(&ctl->lock);
	list_for_each_entry_safe(cur, tmp, &ctl->ctl_list, node) {
		list_del_init(&cur->node);
		mutex_lock(&cur->lock);
		cur->online = false;
		cur->pcie_dev = NULL;
		xh2a_ctl_clear_owner_locked(cur);
		mutex_unlock(&cur->lock);
		wake_up_all(&cur->wq);
		xh2a_ctl_ctx_put(cur);
	}
	mutex_unlock(&ctl->lock);
}

int xh2a_ctl_get_owner_devmask(struct xh2a_ctl_ctx *ctl,
			       uint32_t timeout_dev_id, pid_t *owner_pid_snap,
			       unsigned long *devmask)
{
	struct xh2a_ctl_dev_ctx *ctx;
	struct pid *owner = NULL;
	bool found = false;
	bool usable = false;

	if (!ctl || !owner_pid_snap || !devmask ||
	    timeout_dev_id >= XH2A_CTL_MAX_DEVICES)
		return -EINVAL;
	bitmap_zero(devmask, XH2A_CTL_MAX_DEVICES);

	/* First find the timed-out device's owner, independent of list order. */
	mutex_lock(&ctl->lock);
	list_for_each_entry(ctx, &ctl->ctl_list, node) {
		mutex_lock(&ctx->lock);
		if (ctx->dev_id == timeout_dev_id) {
			found = true;
			usable = ctx->online && !ctx->removing && ctx->ipu_ready;
			if (usable && ctx->refcnt && ctx->owner_pid)
				owner = get_pid(ctx->owner_pid);
		}
		mutex_unlock(&ctx->lock);
		if (found)
			break;
	}
	if (!found || !usable) {
		mutex_unlock(&ctl->lock);
		return -ENODEV;
	}
	if (!owner) {
		mutex_unlock(&ctl->lock);
		return -ENOENT;
	}
	if (!xh2a_ctl_owner_alive(owner)) {
		put_pid(owner);
		mutex_unlock(&ctl->lock);
		return -ESRCH;
	}

	/* Re-scan every context while the owner snapshot is protected. */
	list_for_each_entry(ctx, &ctl->ctl_list, node) {
		mutex_lock(&ctx->lock);
		if (ctx->dev_id < XH2A_CTL_MAX_DEVICES && ctx->online &&
		    !ctx->removing && ctx->ipu_ready && ctx->refcnt &&
		    ctx->owner_pid == owner)
			set_bit(ctx->dev_id, devmask);
		mutex_unlock(&ctx->lock);
	}
	mutex_unlock(&ctl->lock);
	if (bitmap_empty(devmask, XH2A_CTL_MAX_DEVICES)) {
		put_pid(owner);
		return -ENOENT;
	}
	*owner_pid_snap = pid_nr(owner);
	put_pid(owner);
	return 0;
}

MODULE_LICENSE("GPL");
