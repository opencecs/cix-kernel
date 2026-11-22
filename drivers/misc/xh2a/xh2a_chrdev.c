// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2026 Houmo AI Inc.
 * Author: hongxing.ma<hongxing.ma@houmo.ai>
 */

#include <linux/cdev.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include "xh2a_chrdev.h"

#define XH2A_CHRDEV_NAME_MAX 64

struct xh2a_chrdev_slot {
	struct cdev cdev;
	struct class *cls;
	struct device *dev;
	void *drvdata;
	dev_t devt;
	struct mutex lock;
	struct list_head node;
};

static LIST_HEAD(xh2a_chrdev_slot_list);
static DEFINE_MUTEX(xh2a_chrdev_slot_list_lock);

static struct xh2a_chrdev_slot *xh2a_chrdev_slot_find_locked(dev_t devt)
{
	struct xh2a_chrdev_slot *slot;

	list_for_each_entry(slot, &xh2a_chrdev_slot_list, node) {
		if (slot->devt == devt)
			return slot;
	}

	return NULL;
}

static struct xh2a_chrdev_slot *xh2a_chrdev_slot_find(dev_t devt)
{
	struct xh2a_chrdev_slot *slot;

	mutex_lock(&xh2a_chrdev_slot_list_lock);
	slot = xh2a_chrdev_slot_find_locked(devt);
	mutex_unlock(&xh2a_chrdev_slot_list_lock);

	return slot;
}

struct device *xh2a_chrdev_create(struct class *cls, struct device *parent,
				  dev_t devt, void *drvdata,
				  const struct file_operations *fops,
				  const struct attribute_group **groups,
				  const char *fmt, ...)
{
	struct xh2a_chrdev_slot *slot;
	struct device *dev;
	va_list args;
	char name[XH2A_CHRDEV_NAME_MAX];
	bool new_slot = false;
	int ret;

	if (!cls || !drvdata || !fops)
		return ERR_PTR(-EINVAL);

	va_start(args, fmt);
	ret = vsnprintf(name, sizeof(name), fmt, args);
	va_end(args);
	if (ret < 0 || ret >= sizeof(name))
		return ERR_PTR(-EINVAL);

	mutex_lock(&xh2a_chrdev_slot_list_lock);
	slot = xh2a_chrdev_slot_find_locked(devt);
	if (!slot) {
		slot = kzalloc(sizeof(*slot), GFP_KERNEL);
		if (!slot) {
			mutex_unlock(&xh2a_chrdev_slot_list_lock);
			return ERR_PTR(-ENOMEM);
		}

		slot->devt = devt;
		slot->cls = cls;
		mutex_init(&slot->lock);
		cdev_init(&slot->cdev, fops);
		ret = cdev_add(&slot->cdev, devt, 1);
		if (ret) {
			mutex_unlock(&xh2a_chrdev_slot_list_lock);
			kfree(slot);
			return ERR_PTR(ret);
		}

		list_add_tail(&slot->node, &xh2a_chrdev_slot_list);
		new_slot = true;
	}
	mutex_unlock(&xh2a_chrdev_slot_list_lock);

	mutex_lock(&slot->lock);
	if (slot->drvdata) {
		mutex_unlock(&slot->lock);
		return ERR_PTR(-EBUSY);
	}
	slot->cls = cls;

	if (groups)
		dev = device_create_with_groups(cls, parent, devt, drvdata,
						groups, "%s", name);
	else
		dev = device_create(cls, parent, devt, drvdata, "%s", name);

	if (IS_ERR(dev)) {
		mutex_unlock(&slot->lock);

		if (new_slot) {
			mutex_lock(&xh2a_chrdev_slot_list_lock);
			list_del(&slot->node);
			mutex_unlock(&xh2a_chrdev_slot_list_lock);
			cdev_del(&slot->cdev);
			kfree(slot);
		}

		return dev;
	}

	slot->dev = dev;
	slot->drvdata = drvdata;
	mutex_unlock(&slot->lock);

	return dev;
}
EXPORT_SYMBOL_GPL(xh2a_chrdev_create);

void xh2a_chrdev_destroy(struct class *cls, dev_t devt, void *drvdata)
{
	struct xh2a_chrdev_slot *slot;

	if (!cls)
		return;

	slot = xh2a_chrdev_slot_find(devt);
	if (!slot)
		return;

	/*
	 * Hold slot->lock across device_destroy() so it cannot race with a
	 * concurrent xh2a_chrdev_create() (which calls device_create() under
	 * the same lock) on the same dev_t during hotplug remove/probe.
	 *
	 * drvdata is the per-probe device object and acts as the owner token:
	 * a stale destroy() for an old generation will not clobber a device
	 * already re-published by a newer probe.
	 */
	mutex_lock(&slot->lock);
	if (drvdata && slot->drvdata != drvdata)
		goto out_unlock;

	slot->drvdata = NULL;
	if (slot->dev) {
		device_destroy(cls, devt);
		slot->dev = NULL;
	}
out_unlock:
	mutex_unlock(&slot->lock);
}
EXPORT_SYMBOL_GPL(xh2a_chrdev_destroy);

int xh2a_chrdev_get_drvdata(struct inode *inode, xh2a_chrdev_get_cb get,
			    void **drvdata)
{
	struct xh2a_chrdev_slot *slot;
	void *data;
	int ret = 0;

	if (!inode || !inode->i_cdev || !drvdata)
		return -ENODEV;

	slot = container_of(inode->i_cdev, struct xh2a_chrdev_slot, cdev);

	mutex_lock(&slot->lock);
	data = slot->drvdata;
	if (!data) {
		ret = -ENODEV;
		goto out_unlock;
	}

	if (get) {
		ret = get(data);
		if (ret)
			goto out_unlock;
	}

	*drvdata = data;

out_unlock:
	mutex_unlock(&slot->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(xh2a_chrdev_get_drvdata);

void xh2a_chrdev_registry_destroy(void)
{
	struct xh2a_chrdev_slot *slot, *tmp;

	mutex_lock(&xh2a_chrdev_slot_list_lock);
	list_for_each_entry_safe(slot, tmp, &xh2a_chrdev_slot_list, node) {
		list_del(&slot->node);

		mutex_lock(&slot->lock);
		if (slot->dev)
			device_destroy(slot->cls, slot->devt);
		slot->drvdata = NULL;
		slot->dev = NULL;
		mutex_unlock(&slot->lock);

		cdev_del(&slot->cdev);
		kfree(slot);
	}
	mutex_unlock(&xh2a_chrdev_slot_list_lock);
}
EXPORT_SYMBOL_GPL(xh2a_chrdev_registry_destroy);
