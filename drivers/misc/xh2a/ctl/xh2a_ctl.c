// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2026 Houmo AI Inc.
 * Author: mingdong.wang<mingdong.wang@houmo.ai>
 */

#include <linux/fs.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/kref.h>
#include <linux/types.h>
#include <linux/sched.h>
#include <linux/jiffies.h>

#include <xh2a_host_drv.h>
#include <xh2a_chrdev.h>
#include <xh2a_ctl_internal.h>
#include <xh2a_pcie_api.h>
#include <xh2a_ipu_internal.h>
#include "xh2a_ctl.h"
#include "xh2a_ctl_dev.h"

static void xh2a_ctl_free_dev_ctx(struct kref *kref)
{
	struct xh2a_ctl_dev_ctx *ctl_ctx =
		container_of(kref, struct xh2a_ctl_dev_ctx, kref);

	kfree(ctl_ctx);
}

void xh2a_ctl_ctx_get(struct xh2a_ctl_dev_ctx *ctl_ctx)
{
	kref_get(&ctl_ctx->kref);
}

void xh2a_ctl_ctx_put(struct xh2a_ctl_dev_ctx *ctl_ctx)
{
	kref_put(&ctl_ctx->kref, xh2a_ctl_free_dev_ctx);
}

void xh2a_ctl_ctc_lifecycle_lock(struct xh2a_ctl_ctx *ctl)
{
	if (ctl)
		mutex_lock(&ctl->ctc_mutex);
}

void xh2a_ctl_ctc_lifecycle_unlock(struct xh2a_ctl_ctx *ctl)
{
	if (ctl)
		mutex_unlock(&ctl->ctc_mutex);
}

static void xh2a_ctl_ctc_requeue_auto_init_locked(struct xh2a_ctl_ctx *ctl);

static void xh2a_ctl_ctc_auto_work(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct xh2a_ctl_ctx *ctl =
		container_of(dwork, struct xh2a_ctl_ctx, ctc_auto_work);
	u64 generation;
	int ret;

	mutex_lock(&ctl->lock);
	if (ctl->ctc_auto_state == XH2A_CTL_CTC_AUTO_READY) {
		mutex_unlock(&ctl->lock);
		return;
	}
	ctl->ctc_auto_state = XH2A_CTL_CTC_AUTO_BUILDING;
	ctl->ctc_auto_errno = 0;
	generation = ctl->ctc_auto_generation;
	mutex_unlock(&ctl->lock);
	wake_up_all(&ctl->ctc_wq);

	xh2a_ctl_ctc_lifecycle_lock(ctl);
	ret = xh2a_ctl_ctc_build_topology_auto_locked(ctl);

	mutex_lock(&ctl->lock);
	if (generation != ctl->ctc_auto_generation) {
		ctl->ctc_topology_dirty = true;
		xh2a_ctl_ctc_requeue_auto_init_locked(ctl);
	} else if (ret == -EAGAIN) {
		xh2a_ctl_ctc_requeue_auto_init_locked(ctl);
	} else if (ret) {
		ctl->ctc_auto_state = XH2A_CTL_CTC_AUTO_FAILED;
		ctl->ctc_auto_errno = ret;
	} else {
		ctl->ctc_auto_state = XH2A_CTL_CTC_AUTO_READY;
		ctl->ctc_auto_errno = 0;
	}
	mutex_unlock(&ctl->lock);
	xh2a_ctl_ctc_lifecycle_unlock(ctl);
	wake_up_all(&ctl->ctc_wq);
}

static void xh2a_ctl_ctc_requeue_auto_init_locked(struct xh2a_ctl_ctx *ctl)
{
	ctl->ctc_auto_state = XH2A_CTL_CTC_AUTO_PENDING;
	ctl->ctc_auto_errno = 0;
	mod_delayed_work(system_wq, &ctl->ctc_auto_work,
			 msecs_to_jiffies(XH2A_CTL_CTC_AUTO_INIT_DELAY_MS));
}

static void xh2a_ctl_ctc_schedule_auto_init_locked(struct xh2a_ctl_ctx *ctl)
{
	ctl->ctc_topology_dirty = true;
	ctl->ctc_auto_generation++;
	xh2a_ctl_ctc_requeue_auto_init_locked(ctl);
}

void xh2a_ctl_ctc_schedule_auto_init(struct xh2a_ctl_ctx *ctl)
{
	if (!ctl)
		return;

	mutex_lock(&ctl->lock);
	xh2a_ctl_ctc_schedule_auto_init_locked(ctl);
	mutex_unlock(&ctl->lock);
	wake_up_all(&ctl->ctc_wq);
}

void xh2a_ctl_ctc_cancel_auto_init(struct xh2a_ctl_ctx *ctl)
{
	if (!ctl)
		return;

	cancel_delayed_work_sync(&ctl->ctc_auto_work);
	wake_up_all(&ctl->ctc_wq);
}

int xh2a_ctl_ctc_mark_ipu_ready(struct xh2a_ctl_ctx *ctl, uint32_t dev_id)
{
	struct xh2a_ctl_dev_ctx *ctl_ctx;
	struct xh2a_pcie_client *client = NULL;
	int ret = -ENODEV;

	if (!ctl)
		return -EINVAL;

	mutex_lock(&ctl->lock);
	list_for_each_entry(ctl_ctx, &ctl->ctl_list, node) {
		if (ctl_ctx->dev_id != dev_id)
			continue;

		mutex_lock(&ctl_ctx->lock);
		if (ctl_ctx->online && !ctl_ctx->removing &&
		    !xh2a_pcie_get_client(ctl_ctx->pcie_dev, &client,
					 XH2A_IPU_DEVICE_NAME) &&
		    client && client->client_data) {
			ctl_ctx->ipu_ready = true;
			ret = 0;
		}
		mutex_unlock(&ctl_ctx->lock);
		break;
	}
	mutex_unlock(&ctl->lock);

	if (!ret)
		xh2a_ctl_ctc_schedule_auto_init(ctl);

	return ret;
}

void xh2a_ctl_ctc_mark_dev_removing(struct xh2a_ctl_ctx *ctl,
				    uint32_t dev_id)
{
	struct xh2a_ctl_dev_ctx *ctl_ctx;

	if (!ctl)
		return;

	mutex_lock(&ctl->lock);
	list_for_each_entry(ctl_ctx, &ctl->ctl_list, node) {
		if (ctl_ctx->dev_id != dev_id)
			continue;

		mutex_lock(&ctl_ctx->lock);
		ctl_ctx->removing = true;
		ctl_ctx->ipu_ready = false;
		ctl_ctx->online = false;
		mutex_unlock(&ctl_ctx->lock);
		/* Waiters must re-evaluate !online before PCIe teardown starts. */
		wake_up_all(&ctl_ctx->wq);
		/* Removing one chip releases this owner's locks across its CTC group. */
		xh2a_ctl_release_group_locks_locked(ctl, dev_id);
		break;
	}
	xh2a_ctl_ctc_schedule_auto_init_locked(ctl);
	mutex_unlock(&ctl->lock);
	wake_up_all(&ctl->ctc_wq);
}

static int xh2a_ctl_open(struct inode *inode, struct file *filp)
{
	struct xh2a_ctl_ctx *ctl = NULL;
	struct xh2a_ctl_file_ctx *file_ctx;
	struct xh2a_ctl_process_lock_ctx *process_lock_ctx;
	struct pid *owner_pid;
	int ret;

	ret = xh2a_chrdev_get_drvdata(inode, NULL, (void **)&ctl);
	if (ret)
		return ret;

	file_ctx = kzalloc(sizeof(*file_ctx), GFP_KERNEL);
	if (!file_ctx)
		return -ENOMEM;

	owner_pid = get_task_pid(current, PIDTYPE_TGID);
	if (!owner_pid) { kfree(file_ctx); return -ESRCH; }
	file_ctx->ctl = ctl;
	INIT_LIST_HEAD(&file_ctx->node);

	mutex_lock(&ctl->lock);
	list_for_each_entry(process_lock_ctx, &ctl->process_lock_ctx_list, node)
		if (process_lock_ctx->owner_pid == owner_pid)
			goto found_process;
	process_lock_ctx = kzalloc(sizeof(*process_lock_ctx), GFP_KERNEL);
	if (!process_lock_ctx) {
		mutex_unlock(&ctl->lock); put_pid(owner_pid); kfree(file_ctx); return -ENOMEM;
	}
	process_lock_ctx->owner_pid = owner_pid;
	mutex_init(&process_lock_ctx->op_lock);
	init_waitqueue_head(&process_lock_ctx->lock_waitq);
	INIT_LIST_HEAD(&process_lock_ctx->node);
	list_add_tail(&process_lock_ctx->node, &ctl->process_lock_ctx_list);
	goto add_file;
found_process:
	put_pid(owner_pid);
add_file:
	process_lock_ctx->fd_count++;
	file_ctx->process_lock_ctx = process_lock_ctx;
	list_add_tail(&file_ctx->node, &ctl->ctl_file_list);
	mutex_unlock(&ctl->lock);

	dev_dbg(ctl->cdev_dev, "%s: open\n", __func__);

	filp->private_data = file_ctx;

	return 0;
}

static int xh2a_ctl_release(struct inode *inode, struct file *filp)
{
	struct xh2a_ctl_file_ctx *file_ctx = filp->private_data;
	struct xh2a_ctl_ctx *ctl;
	struct xh2a_ctl_process_lock_ctx *process_lock_ctx;
	bool free_process_lock_ctx = false;

	if (!file_ctx) {
		filp->private_data = NULL;
		return 0;
	}

	ctl = file_ctx->ctl;
	if (!ctl) {
		filp->private_data = NULL;
		kfree(file_ctx);
		return 0;
	}

	process_lock_ctx = file_ctx->process_lock_ctx;
	if (!process_lock_ctx) { filp->private_data = NULL; kfree(file_ctx); return 0; }
	mutex_lock(&process_lock_ctx->op_lock);
	mutex_lock(&ctl->lock);
	list_del_init(&file_ctx->node);
	if (--process_lock_ctx->fd_count == 0) {
		xh2a_ctl_release_locks_locked(ctl, process_lock_ctx);
		list_del_init(&process_lock_ctx->node);
		free_process_lock_ctx = true;
	}
	mutex_unlock(&ctl->lock);
	mutex_unlock(&process_lock_ctx->op_lock);
	if (free_process_lock_ctx) { put_pid(process_lock_ctx->owner_pid); kfree(process_lock_ctx); }

	filp->private_data = NULL;
	kfree(file_ctx);

	return 0;
}

int xh2a_ctl_register_dev(struct xh2a_ctl_ctx *ctl, uint32_t dev_id,
			  void *handle)
{
	struct xh2a_ctl_dev_ctx *ctl_ctx, *tmp;
	struct xh2a_pcie_dev *pcie_dev = (struct xh2a_pcie_dev *)handle;
	struct device *ctldev;

	if (!ctl || dev_id >= XH2A_CTL_MAX_DEVICES || !handle)
		return -EINVAL;

	ctldev = ctl->cdev_dev;

	dev_dbg(ctldev, "%s: register dev_id=%u pcie_dev=%p\n", __func__,
		dev_id, pcie_dev);

	/* prevent duplicate device registration - keep mutex locked during
	 * entire operation */
	mutex_lock(&ctl->lock);
	list_for_each_entry(tmp, &ctl->ctl_list, node) {
		if (tmp->dev_id == dev_id) {
			mutex_unlock(&ctl->lock);
			return -EEXIST;
		}
	}

	ctl_ctx = xh2a_ctl_create_lock(ctl, dev_id, handle);
	if (!ctl_ctx) {
		mutex_unlock(&ctl->lock);
		return -ENOMEM;
	}

	list_add_tail(&ctl_ctx->node, &ctl->ctl_list);

	/*
	 * Wake waiters while still holding ctl->lock so a concurrent
	 * xh2a_ctl_destroy_lock() cannot drop the last reference to
	 * ctl_ctx and free its wait_queue_head out from under us.
	 */
	wake_up_all(&ctl_ctx->wq);
	mutex_unlock(&ctl->lock);

	dev_dbg(ctldev, "%s: dev_id=%u registered, ctx=%p\n", __func__, dev_id,
		ctl_ctx);

	xh2a_ctl_ctc_schedule_auto_init(ctl);

	return 0;
}

static long xh2a_ctl_ioctl(struct file *filp, unsigned int cmd,
			   unsigned long arg)
{
	struct xh2a_ctl_lock_req req;
	struct xh2a_ctl_ctc_get_req ctc_get_req;
	struct xh2a_ctl_ctx *ctl = xh2a_ctl_file_ctx_get(filp);
	struct device *ctldev;
	size_t req_size;
	uint32_t nwords;
	long ret;

	if (!ctl)
		return -EINVAL;

	ctldev = ctl->cdev_dev;

	switch (cmd) {
	case IOCTL_XH2A_CTL_LOCK_DEV:
	case IOCTL_XH2A_CTL_UNLOCK_DEV:
		if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
			return -EFAULT;
		if (req.reserved || !req.nbits || req.nbits > XH2A_CTL_MAX_DEVICES)
			return -EINVAL;
		nwords = DIV_ROUND_UP(req.nbits, 64);
		req_size = sizeof(req) + nwords * sizeof(*req.words);
		{
			struct xh2a_ctl_lock_req *full_req = kzalloc(req_size, GFP_KERNEL);
			if (!full_req) return -ENOMEM;
			*full_req = req;
			if (copy_from_user(full_req->words, (void __user *)(arg + sizeof(req)), req_size - sizeof(req))) {
				kfree(full_req); return -EFAULT;
			}
			ret = (cmd == IOCTL_XH2A_CTL_LOCK_DEV) ?
				xh2a_ctl_lock_dev(filp, full_req) : xh2a_ctl_unlock_dev(filp, full_req);
			kfree(full_req);
			return ret;
		}
	case IOCTL_XH2A_CTL_CTC_INIT:
		return 0;
	case IOCTL_XH2A_CTL_CTC_GET:
		if (copy_from_user(&ctc_get_req, (void __user *)arg,
				   sizeof(ctc_get_req)))
			return -EFAULT;
		ret = xh2a_ctl_ctc_get(filp, &ctc_get_req);
		if (ret)
			return ret;
		if (copy_to_user((void __user *)arg, &ctc_get_req,
				 sizeof(ctc_get_req)))
			return -EFAULT;
		return 0;
	default:
		if (ctldev)
			dev_dbg(ctldev, "%s: unsupported ioctl cmd=0x%x\n",
				__func__, cmd);
		return -ENOTTY;
	}
}

static const struct file_operations xh2a_ctl_fops = {
	.owner = THIS_MODULE,
	.open = xh2a_ctl_open,
	.release = xh2a_ctl_release,
	.unlocked_ioctl = xh2a_ctl_ioctl,
};

int xh2a_ctl_init(struct xh2a_ctl_ctx **ctl_out)
{
	int ret;
	struct xh2a_ctl_ctx *ctl;

	if (!ctl_out)
		return -EINVAL;

	ctl = kzalloc(sizeof(*ctl), GFP_KERNEL);
	if (!ctl)
		return -ENOMEM;

	mutex_init(&ctl->ctc_mutex);
	mutex_init(&ctl->lock);
	INIT_LIST_HEAD(&ctl->ctl_list);
	INIT_LIST_HEAD(&ctl->ctl_file_list);
	INIT_LIST_HEAD(&ctl->process_lock_ctx_list);
	INIT_LIST_HEAD(&ctl->ctc_group_list);
	INIT_DELAYED_WORK(&ctl->ctc_auto_work, xh2a_ctl_ctc_auto_work);
	INIT_WORK(&ctl->ctc_recovery_work, xh2a_ctl_ctc_recovery_work);
	init_waitqueue_head(&ctl->ctc_wq);
	ctl->ctc_topology_dirty = false;
	ctl->ctc_auto_state = XH2A_CTL_CTC_AUTO_PENDING;
	ctl->ctc_auto_errno = 0;
	ctl->ctc_auto_generation = 0;
	ctl->ctc_recovery_active = false;
	ctl->ctc_recovery_owner_pid_snap = 0;
	ctl->ctc_recovery_root_dev_id = XH2A_CTL_MAX_DEVICES;
	bitmap_zero(ctl->ctc_recovery_devmask, XH2A_CTL_MAX_DEVICES);
	ctl->ctc_recovery_errno = 0;

	ctl->cdev_dev = xh2a_chrdev_create(
		xh2a_ctl_class, NULL,
		MKDEV(xh2a_global_chrdev_major, XH2A_CHRDEV_SUB_CTL), ctl,
		&xh2a_ctl_fops, NULL, "xh2a_ctl%d", 0);
	if (IS_ERR(ctl->cdev_dev)) {
		ret = PTR_ERR(ctl->cdev_dev);
		kfree(ctl);
		return ret;
	}

	dev_dbg(ctl->cdev_dev, "%s: ctl created dev=%s\n", __func__,
		dev_name(ctl->cdev_dev));

	*ctl_out = ctl;

	return 0;
}

void xh2a_ctl_exit(struct xh2a_ctl_ctx *ctl)
{
	if (!ctl)
		return;

	dev_dbg(ctl->cdev_dev, "%s: ctl exit\n", __func__);

	xh2a_ctl_ctc_cancel_auto_init(ctl);
	cancel_work_sync(&ctl->ctc_recovery_work);

	xh2a_chrdev_destroy(
		xh2a_ctl_class,
		MKDEV(xh2a_global_chrdev_major, XH2A_CHRDEV_SUB_CTL), ctl);

	xh2a_ctl_ctc_group_list_clear(ctl);
	xh2a_ctl_cleanup_all_locks(ctl);
	{
		struct xh2a_ctl_process_lock_ctx *p, *tmp;
		list_for_each_entry_safe(p, tmp, &ctl->process_lock_ctx_list, node) {
			list_del_init(&p->node); put_pid(p->owner_pid); kfree(p);
		}
	}

	kfree(ctl);
}

MODULE_LICENSE("GPL");
