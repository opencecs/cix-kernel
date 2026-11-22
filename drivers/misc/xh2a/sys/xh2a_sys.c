// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2024 Houmo AI Inc.
 * Author: hongxing.ma<hongxing.ma@houmo.ai>
 */

/*
 * xh2a_sys is a simple, single user/process device driver, which
 * is used to demonstrate how to access xh2a's soc address.
 */

#include <linux/module.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/kref.h>
#include <linux/mutex.h>
#include <linux/pm_runtime.h>
#include <xh2a_host_drv.h>
#include <xh2a_pcie_api.h>
#include <version_compat.h>
#include "xh2a_ipu_device.h"
#include <xh2a_chrdev.h>
#include "xh2a_sys.h"

#define ADDRESS_2GB		 0x80000000ULL
#define XH2A_DEFAULT_ACCESS_ADDR 0x60000000ULL
#define XH2A_FULLCHIP_RESET_ADDR 0x7000106CULL

static void xh2a_sys_safe_release(struct kref *kref)
{
	struct xh2a_sys_dev *sys_dev =
		container_of(kref, struct xh2a_sys_dev, refcount);
	kfree(sys_dev);
}

static int xh2a_sys_get_drvdata(void *drvdata)
{
	struct xh2a_sys_dev *sys_dev = drvdata;

	if (atomic_read(&sys_dev->removed)) {
		pr_err("%s: device mark removed\n", __func__);
		return -ENODEV;
	}

	if (kref_get_unless_zero(&sys_dev->refcount) == 0) {
		pr_err("%s: device is removing\n", __func__);
		return -ENODEV;
	}

	return 0;
}

static int xh2a_sys_open(struct inode *inode, struct file *filp)
{
	struct xh2a_sys_dev *sys_dev = NULL;
	int ret;

	ret = xh2a_chrdev_get_drvdata(inode, xh2a_sys_get_drvdata,
				      (void **)&sys_dev);
	if (ret)
		return ret;

	mutex_lock(&sys_dev->sys_access_mutex);
	if (atomic_read(&sys_dev->removed)) {
		mutex_unlock(&sys_dev->sys_access_mutex);
		kref_put(&sys_dev->refcount, xh2a_sys_safe_release);
		return -ENODEV;
	}

	dev_dbg(sys_dev->cdev_dev, "%s: major=%d, minor=%d\n", __func__,
		MAJOR(inode->i_rdev), MINOR(inode->i_rdev));

	filp->private_data = sys_dev;
	mutex_unlock(&sys_dev->sys_access_mutex);

	return 0;
}

static int xh2a_sys_release(struct inode *inode, struct file *filp)
{
	struct xh2a_sys_dev *sys_dev = filp->private_data;

	if (!sys_dev) {
		filp->private_data = NULL;
		return 0;
	}

	mutex_lock(&sys_dev->sys_access_mutex);
	if (!atomic_read(&sys_dev->removed))
		dev_dbg(sys_dev->cdev_dev, "%s: major=%d, minor=%d\n", __func__,
			MAJOR(inode->i_rdev), MINOR(inode->i_rdev));

	filp->private_data = NULL;
	mutex_unlock(&sys_dev->sys_access_mutex);
	kref_put(&sys_dev->refcount, xh2a_sys_safe_release);

	return 0;
}

static int xh2a_sys_ioctl_rw(struct xh2a_sys_dev *sys_dev, unsigned long arg)
{
	int ret;
	struct xh2a_sys_rw_request_arg ioc_request;

	ret = copy_from_user(&ioc_request, (void __user *)arg,
			     sizeof(ioc_request));

	if (ret != 0) {
		dev_err(sys_dev->cdev_dev, "%s: copy_from_user failed\n",
			__func__);
		return -EFAULT;
	}

	switch (ioc_request.type) {
	case XH2A_SYSTEM_RW_TYPE_READB:
		ret = xh2a_pcie_pio_readb(sys_dev->private_data,
					  ioc_request.addr,
					  (uint8_t *)&ioc_request.data);
		break;
	case XH2A_SYSTEM_RW_TYPE_READW:
		ret = xh2a_pcie_pio_readw(sys_dev->private_data,
					  ioc_request.addr & ~0x1ULL,
					  (uint16_t *)&ioc_request.data);
		break;
	case XH2A_SYSTEM_RW_TYPE_READL:
		ret = xh2a_pcie_pio_readl(sys_dev->private_data,
					  ioc_request.addr & ~0x3ULL,
					  (uint32_t *)&ioc_request.data);
		break;
	case XH2A_SYSTEM_RW_TYPE_READQ:
		ret = xh2a_pcie_pio_readq(sys_dev->private_data,
					  ioc_request.addr & ~0x7ULL,
					  &ioc_request.data);
		break;
	case XH2A_SYSTEM_RW_TYPE_WRITEB:
		ret = xh2a_pcie_pio_writeb(sys_dev->private_data,
					   ioc_request.addr, ioc_request.data);
		break;
	case XH2A_SYSTEM_RW_TYPE_WRITEW:
		ret = xh2a_pcie_pio_writew(sys_dev->private_data,
					   ioc_request.addr & ~0x1ULL,
					   ioc_request.data);
		break;
	case XH2A_SYSTEM_RW_TYPE_WRITEL:
		if (((ioc_request.addr & ~0x3ULL) == XH2A_FULLCHIP_RESET_ADDR) &&
		    (ioc_request.data & 0x3)) {
			ret = xh2a_ipu_prepare_fullchip_reset(sys_dev->private_data);
			if (ret)
				return ret;
			xh2a_pcie_stop_runtime_pm(sys_dev->private_data);
			ret = xh2a_pcie_fullchip_reset(sys_dev->private_data,
						     false, ioc_request.data);
			if (ret && ret != -ESHUTDOWN)
				xh2a_ipu_abort_fullchip_reset(sys_dev->private_data);
		} else {
			ret = xh2a_pcie_pio_writel(sys_dev->private_data,
						   ioc_request.addr & ~0x3ULL,
						   ioc_request.data);
		}
		break;
	case XH2A_SYSTEM_RW_TYPE_WRITEQ:
		ret = xh2a_pcie_pio_writeq(sys_dev->private_data,
					   ioc_request.addr & ~0x7ULL,
					   ioc_request.data);
		break;
	default:
		ret = -EINVAL;
		dev_err(sys_dev->cdev_dev, "%s: unknown type 0x%x\n", __func__,
			ioc_request.type);
		break;
	}

	if (ret) {
		dev_err(sys_dev->cdev_dev,
			"%s: rw failed. addr 0x%llx, type %x\n", __func__,
			ioc_request.addr, ioc_request.type);
		return ret;
	}

	ret = copy_to_user((void __user *)arg, &ioc_request,
			   sizeof(ioc_request));
	if (ret) {
		dev_err(sys_dev->cdev_dev, "%s: copy_to_user failed\n",
			__func__);
		return -EFAULT;
	}

	return 0;
}

static int xh2a_sys_ioctl_dumpload(struct xh2a_sys_dev *sys_dev,
				   unsigned long arg)
{
	int ret;
	struct xh2a_sys_dumpload_request_arg ioc_request;

	ret = copy_from_user(&ioc_request, (void __user *)arg,
			     sizeof(ioc_request));

	if (ret != 0) {
		dev_err(sys_dev->cdev_dev, "%s: copy_from_user failed\n",
			__func__);
		return -EFAULT;
	}

	if (ioc_request.size >= ADDRESS_2GB) {
		dev_err(sys_dev->cdev_dev, "%s: size is too big\n", __func__);
		return -EINVAL;
	}

	switch (ioc_request.type) {
	case XH2A_SYSTEM_DUMPLOAD_TYPE_DUMP:
		ret = xh2a_pcie_dma_read_mem_userspace(
			sys_dev->private_data, ioc_request.device_addr,
			(void __user *)ioc_request.host_addr, ioc_request.size);
		break;
	case XH2A_SYSTEM_DUMPLOAD_TYPE_LOAD:
		ret = xh2a_pcie_dma_write_mem_userspace(
			sys_dev->private_data, ioc_request.device_addr,
			(void __user *)ioc_request.host_addr, ioc_request.size);
		break;
	default:
		dev_err(sys_dev->cdev_dev, "%s: unknown type 0x%x\n", __func__,
			ioc_request.type);
		break;
	}

	if (ret)
		dev_err(sys_dev->cdev_dev,
			"%s: dump/load failed. device addr 0x%llx, type %x\n",
			__func__, ioc_request.device_addr, ioc_request.type);

	ret = copy_to_user((void __user *)arg, &ioc_request,
			   sizeof(ioc_request));
	if (ret)
		dev_err(sys_dev->cdev_dev, "%s: copy_to_user failed\n",
			__func__);

	return ret;
}

static int xh2a_sys_ioctl_fullchip_reset(struct xh2a_sys_dev *sys_dev,
					 unsigned long arg)
{
	int ret;
	struct xh2a_sys_fullchip_reset_request_arg ioc_request;

	ret = copy_from_user(&ioc_request, (void __user *)arg,
			     sizeof(ioc_request));

	if (ret != 0) {
		dev_err(sys_dev->cdev_dev, "%s: copy_from_user failed\n",
			__func__);
		return -EFAULT;
	}

	if (ioc_request.reserved != 0xDEADBEEFDEADBEEFULL) {
		dev_err(sys_dev->cdev_dev, "%s: invalid arg\n", __func__);
		return -EINVAL;
	}

	ret = xh2a_ipu_prepare_fullchip_reset(sys_dev->private_data);
	if (ret != 0) {
		dev_err(sys_dev->cdev_dev,
			"%s: prepare ipu fullchip reset failed %d\n", __func__,
			ret);
		return ret;
	}

	xh2a_pcie_stop_runtime_pm(sys_dev->private_data);

	ret = xh2a_pcie_fullchip_reset(sys_dev->private_data, true, 0);

	if (ret != 0) {
		if (ret != -ESHUTDOWN)
			xh2a_ipu_abort_fullchip_reset(sys_dev->private_data);
		pr_err("%s: write msgbit fail\n", __func__);
		return ret;
	}

	return ret;
}

static long xh2a_sys_ioctl(struct file *filp, unsigned int cmd,
			   unsigned long arg)
{
	int ret = -EINVAL;
	int pm_ret;
	struct xh2a_sys_dev *sys_dev = filp->private_data;

	if (!sys_dev) {
		pr_err("%s: sys_dev is NULL\n", __func__);
		return -EINVAL;
	}

	mutex_lock(&sys_dev->sys_access_mutex);

	if (atomic_read(&sys_dev->removed)) {
		pr_err("%s: device mark removed\n", __func__);
		ret = -ENODEV;
		goto out_access_unlock;
	}

	ret = xh2a_pcie_pm_io_status(sys_dev->private_data);
	if (ret)
		goto out_access_unlock;
	if (atomic_read(&sys_dev->block_ioctl_flag)) {
		ret = -EAGAIN;
		goto out_access_unlock;
	}

	if (atomic_read(&sys_dev->removed)) {
		ret = -ENODEV;
		goto out_access_unlock;
	}

	pm_ret = pm_runtime_resume_and_get(sys_dev->parent_dev);
	if (pm_ret < 0) {
		ret = pm_ret;
		goto out_access_unlock;
	}

	ret = xh2a_pcie_pm_io_status(sys_dev->private_data);
	if (ret) {
		pm_runtime_put_noidle(sys_dev->parent_dev);
		goto out_access_unlock;
	}

	mutex_lock(&sys_dev->sys_mutex);

	switch (cmd) {
	case IOCTL_XH2A_SYS_RW:
		ret = xh2a_sys_ioctl_rw(sys_dev, arg);
		if (ret)
			dev_err(sys_dev->cdev_dev, "%s: rw failed\n", __func__);
		break;
	case IOCTL_XH2A_SYS_DUMPLOAD:
		ret = xh2a_sys_ioctl_dumpload(sys_dev, arg);
		if (ret)
			dev_err(sys_dev->cdev_dev, "%s: dumpload failed\n",
				__func__);
		break;
	case IOCTL_XH2A_SYS_FULLCHIP_RESET:
		ret = xh2a_sys_ioctl_fullchip_reset(sys_dev, arg);
		if (ret)
			dev_err(sys_dev->cdev_dev,
				"%s: fullchip reset failed\n", __func__);
		break;
	case IOCTL_XH2A_SYS_CTC_GET:
	case IOCTL_XH2A_SYS_CTC_SET:
		ret = -EOPNOTSUPP;
		dev_dbg(sys_dev->cdev_dev,
			"%s: deprecated CTC sys ioctl cmd=0x%x\n", __func__,
			cmd);
		break;
	default:
		ret = -EINVAL;
		dev_err(sys_dev->cdev_dev, "%s: unknown cmd 0x%x\n", __func__,
			cmd);
		break;
	}

	mutex_unlock(&sys_dev->sys_mutex);

	if ((ret != 0) &&
	    xh2a_pcie_pm_io_status(sys_dev->private_data) == -EAGAIN) {
		dev_err(sys_dev->cdev_dev,
			"%s: ioctl failed in sleep process. need retry\n",
			__func__);
		ret = -EINTR;
	}

	pm_runtime_mark_last_busy(sys_dev->parent_dev);
	pm_runtime_put_sync(sys_dev->parent_dev);

out_access_unlock:
	mutex_unlock(&sys_dev->sys_access_mutex);
	return ret;
}

static const struct file_operations xh2a_sys_fops = {
	.owner = THIS_MODULE,
	.open = xh2a_sys_open,
	.release = xh2a_sys_release,
	.unlocked_ioctl = xh2a_sys_ioctl,
};

/*
 * xh2a_sys_pm_notifier_prepare()
 * 	- before pm prepare the sys device
 * @handle: pcie handle
 * @rollback: rollback flag
 * Return: 0 on success, negative error code on failure
 */
static int xh2a_sys_pm_notifier_prepare(void *handle, bool rollback)
{
	struct xh2a_pcie_client *client = NULL;
	struct xh2a_sys_dev *sys_dev;

	xh2a_pcie_get_client(handle, &client, XH2A_SYSTEM_DEVICE_NAME);

	if (client == NULL) {
		pr_debug("%s: Failed to get client for name '%s'\n", __func__,
			 XH2A_SYSTEM_DEVICE_NAME);
		return 0;
	}

	sys_dev = client->client_data;

	if (rollback) {
		if (atomic_dec_and_test(&sys_dev->block_ioctl_flag))
			wake_up_interruptible(&sys_dev->block_ioctl_wq);
	} else {
		atomic_inc(&sys_dev->block_ioctl_flag);
	}

	return 0;
}

/*
 * xh2a_sys_pm_notifier_complete()
 * 	- after pm complete the sys device
 * @handle: pcie handle
 * Return: 0 on success, negative error code on failure
 */
static int xh2a_sys_pm_notifier_complete(void *handle)
{
	struct xh2a_pcie_client *client = NULL;
	struct xh2a_sys_dev *sys_dev;

	xh2a_pcie_get_client(handle, &client, XH2A_SYSTEM_DEVICE_NAME);

	if (client == NULL) {
		pr_debug("%s: Failed to get client for name '%s'\n", __func__,
			 XH2A_SYSTEM_DEVICE_NAME);
		return 0;
	}

	sys_dev = client->client_data;

	if (atomic_dec_and_test(&sys_dev->block_ioctl_flag))
		wake_up_interruptible(&sys_dev->block_ioctl_wq);

	return 0;
}

/*
 * xh2a_sys_pm_prepare()
 * 	- pm prepare the sys device
 * @handle: pcie handle
 * Return: 0 on success, negative error code on failure
 */
static int xh2a_sys_pm_prepare(void *handle, bool is_compatible)
{
	struct xh2a_pcie_client *client = NULL;
	struct xh2a_sys_dev *sys_dev;

	(void)is_compatible;
	xh2a_pcie_get_client(handle, &client, XH2A_SYSTEM_DEVICE_NAME);

	if (client == NULL) {
		pr_err("%s: Failed to get client for name '%s'\n", __func__,
		       XH2A_SYSTEM_DEVICE_NAME);
		return -EINVAL;
	}

	sys_dev = client->client_data;

	if (sys_dev == NULL) {
		pr_err("%s: Client data is NULL for '%s'\n", __func__,
		       XH2A_SYSTEM_DEVICE_NAME);
		return -EINVAL;
	}

	return 0;
}

/*
 * xh2a_sys_pm_complete()
 * 	- pm complete the sys device
 * @handle: pcie handle
 * Return: 0 on success, negative error code on failure
 */
static int xh2a_sys_pm_complete(void *handle, bool is_compatible)
{
	struct xh2a_pcie_client *client = NULL;
	struct xh2a_sys_dev *sys_dev;

	(void)is_compatible;
	xh2a_pcie_get_client(handle, &client, XH2A_SYSTEM_DEVICE_NAME);

	if (client == NULL) {
		pr_err("%s: Failed to get client for name '%s'\n", __func__,
		       XH2A_SYSTEM_DEVICE_NAME);
		return -EINVAL;
	}

	sys_dev = client->client_data;

	if (sys_dev == NULL) {
		pr_err("%s: Client data is NULL for '%s'\n", __func__,
		       XH2A_SYSTEM_DEVICE_NAME);
		return -EINVAL;
	}

	(void)sys_dev;
	/* The notifier commits user admission after every client recovered. */

	return 0;
}

/*
 * xh2a_sys_pm_runtime_suspend()
 * 	- pm runtime suspend the sys device
 * @handle: pcie handle
 * Return: 0 on success, negative error code on failure
 */
static int xh2a_sys_pm_runtime_suspend(void *handle)
{
	struct xh2a_pcie_client *client = NULL;
	struct xh2a_sys_dev *sys_dev;

	xh2a_pcie_get_client(handle, &client, XH2A_SYSTEM_DEVICE_NAME);

	if (client == NULL) {
		pr_err("%s: Failed to get client for name '%s'\n", __func__,
		       XH2A_SYSTEM_DEVICE_NAME);
		return -EINVAL;
	}

	sys_dev = client->client_data;

	if (sys_dev == NULL) {
		pr_err("%s: Client data is NULL for '%s'\n", __func__,
		       XH2A_SYSTEM_DEVICE_NAME);
		return -EINVAL;
	}

	return 0;
}

/*
 * xh2a_sys_pm_runtime_resume()
 * 	- pm runtime resume the sys device
 * @handle: pcie handle
 * Return: 0 on success, negative error code on failure
 */
static int xh2a_sys_pm_runtime_resume(void *handle)
{
	struct xh2a_pcie_client *client = NULL;
	struct xh2a_sys_dev *sys_dev;

	xh2a_pcie_get_client(handle, &client, XH2A_SYSTEM_DEVICE_NAME);

	if (client == NULL) {
		pr_err("%s: Failed to get client for name '%s'\n", __func__,
		       XH2A_SYSTEM_DEVICE_NAME);
		return -EINVAL;
	}

	sys_dev = client->client_data;

	if (sys_dev == NULL) {
		pr_err("%s: Client data is NULL for '%s'\n", __func__,
		       XH2A_SYSTEM_DEVICE_NAME);
		return -EINVAL;
	}

	return 0;
}

/*
 * xh2a_sys_probe() - probe function for sys device
 * @handle: handle of xh2a_pcie_dev
 * Return: 0 on success, negative error code on failure
 */
static int xh2a_sys_probe(void *handle)
{
	bool client_registered = false;
	int ret;
	struct xh2a_pcie_client *client = NULL;
	struct xh2a_sys_dev *sys_dev = NULL;

	pr_debug("%s\n", __func__);
	pr_debug("0x%x, 0x%llx\n", ~0x3, ~0x3ULL);
	sys_dev = kzalloc(sizeof(struct xh2a_sys_dev), GFP_KERNEL);

	if (sys_dev == NULL) {
		pr_err("%s: alloc sys_dev failed\n", __func__);
		return -ENOMEM;
	}

	client = &sys_dev->client;

	sys_dev->private_data = handle;

	mutex_init(&sys_dev->sys_access_mutex);
	mutex_init(&sys_dev->sys_mutex);
	atomic_set(&sys_dev->removed, 0);
	kref_init(&sys_dev->refcount);

	atomic_set(&sys_dev->block_ioctl_flag, 0);
	init_waitqueue_head(&sys_dev->block_ioctl_wq);

	sys_dev->device_addr = XH2A_DEFAULT_ACCESS_ADDR;
	memset(sys_dev->name_buf, 0, XH2A_SYSTEM_DEVICE_NAME_LEN);
	snprintf(sys_dev->name_buf, XH2A_SYSTEM_DEVICE_NAME_LEN,
		 XH2A_SYSTEM_DEVICE_NAME "%d", xh2a_pcie_device_index(handle));

	sys_dev->parent_dev = xh2a_pcie_device_ptr(handle);
	sys_dev->cdev_dev = xh2a_chrdev_create(
		xh2a_sys_class, sys_dev->parent_dev,
		xh2a_chrdev_devt(handle, XH2A_CHRDEV_SUB_SYS), sys_dev,
		&xh2a_sys_fops, NULL, "%s%d", XH2A_SYSTEM_DEVICE_NAME,
		xh2a_pcie_device_index(handle));
	if (IS_ERR(sys_dev->cdev_dev)) {
		ret = PTR_ERR(sys_dev->cdev_dev);
		pr_err("%s: xh2a_chrdev_create failed %d\n", __func__, ret);
		goto err_chrdev_create;
	}

	snprintf(client->name, XH2A_PCIE_CLIENT_NAME_LEN,
		 XH2A_SYSTEM_DEVICE_NAME);

	client->work[0].msi_id = -1;
	client->work_num = 0;

	client->client_data = sys_dev;
	client->private_data = handle;
	client->prepare_cb = xh2a_sys_pm_prepare;
	client->complete_cb = xh2a_sys_pm_complete;
	client->notifier_prepare_cb = xh2a_sys_pm_notifier_prepare;
	client->notifier_complete_cb = xh2a_sys_pm_notifier_complete;
	client->runtime_suspend_cb = xh2a_sys_pm_runtime_suspend;
	client->runtime_resume_cb = xh2a_sys_pm_runtime_resume;

	ret = xh2a_pcie_register_client(handle, client);

	if (ret != 0) {
		pr_err("%s: register client failed %d\n", __func__, ret);
		goto err_unpublish;
	}
	client_registered = true;

	pr_info("%s: sys device probed successfully %d\n", __func__,
		xh2a_pcie_device_index(handle));

	return 0;

err_unpublish:
	atomic_set(&sys_dev->removed, 1);
	wake_up_interruptible(&sys_dev->block_ioctl_wq);
	mutex_lock(&sys_dev->sys_access_mutex);
	xh2a_chrdev_destroy(xh2a_sys_class,
			    xh2a_chrdev_devt(handle, XH2A_CHRDEV_SUB_SYS),
			    sys_dev);
	if (client_registered)
		xh2a_pcie_unregister_client(handle, client);
	mutex_unlock(&sys_dev->sys_access_mutex);
	kref_put(&sys_dev->refcount, xh2a_sys_safe_release);
	return ret;
err_chrdev_create:
	kfree(sys_dev);
	return ret;
}

/*
 * xh2a_sys_remove() - remove the sys device
 * @handle: pcie handle
 * Return: 0 on success, negative error code on failure
 */
static int xh2a_sys_remove(void *handle)
{
	struct xh2a_pcie_client *client = NULL;
	struct xh2a_sys_dev *sys_dev;

	pr_debug("%s\n", __func__);

	xh2a_pcie_get_client(handle, &client, XH2A_SYSTEM_DEVICE_NAME);

	if (client == NULL) {
		pr_err("%s: Failed to get client for name '%s'\n", __func__,
		       XH2A_SYSTEM_DEVICE_NAME);
		return 0;
	}

	sys_dev = client->client_data;

	atomic_set(&sys_dev->removed, 1);
	wake_up_interruptible(&sys_dev->block_ioctl_wq);

	mutex_lock(&sys_dev->sys_access_mutex);
	xh2a_chrdev_destroy(xh2a_sys_class,
			    xh2a_chrdev_devt(handle, XH2A_CHRDEV_SUB_SYS),
			    sys_dev);

	xh2a_pcie_unregister_client(handle, client);
	mutex_unlock(&sys_dev->sys_access_mutex);

	/*
	 * Keep private_data unchanged for the remaining open-file lifetime.
	 * New operations observe removed while holding sys_access_mutex and
	 * cannot use the stale PCIe handle after this function returns.
	 */
	kref_put(&sys_dev->refcount, xh2a_sys_safe_release);

	return 0;
}

/*
 * xh2a_sys_notifier_call() - called when the pcie notifies this driver
 * @nb: notifier block
 * @event: event type
 * @data: data from notifier
 * Return:
 *  = NOTIFY_OK - Success
 */
static int xh2a_sys_notifier_call(struct notifier_block *nb,
				  unsigned long event, void *data)
{
	int ret = 0;

	pr_debug("%s: event = %ld, data = %p\n", __func__, event, data);

	if (event == XH2A_HOST_NOTIFY_PCIE_PROBE)
		ret = xh2a_sys_probe(data);
	else if (event == XH2A_HOST_NOTIFY_PCIE_REMOVE)
		ret = xh2a_sys_remove(data);

	if (ret)
		return NOTIFY_STOP;

	return NOTIFY_OK;
}

/*
 * @notifier_call: called when the pcie notifies this driver
 */
static struct notifier_block xh2a_sys_notifier_block = {
	.notifier_call = xh2a_sys_notifier_call,
};

/*
 * xh2a_sys_register_driver() - register sys driver
 */
int __init xh2a_sys_register_driver(void)
{
	pr_debug("%s\n", __func__);
	xh2a_host_register_notifier_chain(&xh2a_sys_notifier_block);
	return 0;
}

/*
 * xh2a_sys_unregister_driver() - unregister sys driver
 */
void __exit xh2a_sys_unregister_driver(void)
{
	pr_debug("%s\n", __func__);
	xh2a_host_unregister_notifier_chain(&xh2a_sys_notifier_block);
}

MODULE_LICENSE("GPL");
