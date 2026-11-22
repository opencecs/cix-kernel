// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2024 Houmo AI Inc.
 * Author: hongxing.ma<hongxing.ma@houmo.ai>
 */
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <xh2a_host_drv.h>
#include <xh2a_chrdev.h>
#include <version_compat.h>

static struct xh2a_ctl_ctx *ctl_ctx;
static BLOCKING_NOTIFIER_HEAD(xh2a_host_notifier_list);

/*
 * Global chrdev state — initialized in xh2a_host_init().
 * Definitions (not extern) so sub-modules can reference them.
 */

/*
 * Dummy file_operations for the raw major device (/dev/xh2a).
 * Users should open sub-device nodes (e.g. /dev/xh2a_memory_transfer_device0).
 * This fops only rejects unexpected open attempts cleanly.
 */
static int xh2a_chrdev_dummy_open(struct inode *inode, struct file *filp)
{
	return -ENXIO; /* no raw access — use sub-device nodes */
}

static char *xh2a_devnode(DEVNODE_ATTR_CONST struct device *dev, umode_t *mode)
{
	if (mode)
		*mode = 0666;
	return NULL;
}

static const struct file_operations xh2a_chrdev_dummy_fops = {
	.owner = THIS_MODULE,
	.open = xh2a_chrdev_dummy_open,
};

int xh2a_global_chrdev_major;
struct class *xh2a_ctl_class;
struct class *xh2a_memory_allocator_class;
struct class *xh2a_memory_transfer_class;
struct class *xh2a_fast_memory_class;
struct class *xh2a_ipu_class;
struct class *xh2a_sys_class;
struct class *xh2a_qspi_class;
struct class *xh2a_i2c_class;
struct class *xh2a_rpmsg_lite_class;
struct class *xh2a_transport_class;

int xh2a_host_register_notifier_chain(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&xh2a_host_notifier_list, nb);
}
EXPORT_SYMBOL(xh2a_host_register_notifier_chain);

void xh2a_host_unregister_notifier_chain(struct notifier_block *nb)
{
	blocking_notifier_chain_unregister(&xh2a_host_notifier_list, nb);
}
EXPORT_SYMBOL(xh2a_host_unregister_notifier_chain);

int xh2a_host_call_notifier_chain(int val, void *v)
{
	return blocking_notifier_call_chain(&xh2a_host_notifier_list, val, v);
}
EXPORT_SYMBOL(xh2a_host_call_notifier_chain);

int xh2a_ctl_get_ctx(struct xh2a_ctl_ctx **ctl_out)
{
	if (!ctl_ctx)
		return -ENODEV;

	*ctl_out = ctl_ctx;

	return 0;
}
EXPORT_SYMBOL(xh2a_ctl_get_ctx);

static int __init xh2a_host_init(void)
{
	int rc;

	pr_info("%s: installing xh2a driver version %s: %s\n", __func__,
		XH2A_HOST_DRIVER_RELEASE, XH2A_HOST_DRIVER_BUILDTIME);

	/* Register one global chrdev major for all sub-modules. */
	xh2a_global_chrdev_major =
		register_chrdev(0, "xh2a", &xh2a_chrdev_dummy_fops);
	if (xh2a_global_chrdev_major < 0) {
		rc = xh2a_global_chrdev_major;
		pr_err("%s: register_chrdev failed %d\n", __func__, rc);
		goto module_exit;
	}

	/* Create one class per sub-module for /sys/class/ entries. */
	xh2a_ctl_class = compat_class_create("xh2a_ctl");
	xh2a_memory_allocator_class = compat_class_create("xh2a_memory_"
							  "allocator_device");
	xh2a_memory_transfer_class = compat_class_create("xh2a_memory_transfer_"
							 "device");
	xh2a_fast_memory_class = compat_class_create("xh2a_fast_memory_device");
	xh2a_ipu_class = compat_class_create("xh2a_ipu");
	xh2a_sys_class = compat_class_create("xh2a_sys_device");
	xh2a_qspi_class = compat_class_create("xh2a_qspi_device");
	xh2a_i2c_class = compat_class_create("xh2a_i2c_device");
	xh2a_rpmsg_lite_class = compat_class_create("xh2a_rpmsg_device");
	xh2a_transport_class = compat_class_create("xh2a_transport_device");

	xh2a_ctl_class->devnode = xh2a_devnode;
	xh2a_memory_allocator_class->devnode = xh2a_devnode;
	xh2a_memory_transfer_class->devnode = xh2a_devnode;
	xh2a_fast_memory_class->devnode = xh2a_devnode;
	xh2a_ipu_class->devnode = xh2a_devnode;
	xh2a_sys_class->devnode = xh2a_devnode;
	xh2a_qspi_class->devnode = xh2a_devnode;
	xh2a_i2c_class->devnode = xh2a_devnode;
	xh2a_rpmsg_lite_class->devnode = xh2a_devnode;
	xh2a_transport_class->devnode = xh2a_devnode;

	/* Initialize ctl after chrdev/class are ready. */
	rc = xh2a_ctl_init(&ctl_ctx);
	if (rc < 0) {
		pr_err("%s: xh2a_ctl_init failed\n", __func__);
		goto err_ctl_init;
	}
	xh2a_memory_allocator_register_driver();

	xh2a_memory_transfer_register_driver();

	xh2a_fast_memory_register_driver();

	xh2a_ipu_register_driver();

	xh2a_sys_register_driver();

	xh2a_qspi_register_driver();

	xh2a_i2c_register_driver();

	xh2a_rpmsg_register_driver();

	xh2a_transport_register_driver();

	rc = xh2a_pcie_register_driver();

	if (rc < 0) {
		pr_info("%s: NO xh2a PCI driver found.\n", __func__);
		rc = 0; /* not an error — module loaded, devices ready for
			   hotplug */
	}

	return 0;

err_ctl_init:
	class_destroy(xh2a_transport_class);
	class_destroy(xh2a_rpmsg_lite_class);
	class_destroy(xh2a_i2c_class);
	class_destroy(xh2a_qspi_class);
	class_destroy(xh2a_sys_class);
	class_destroy(xh2a_ipu_class);
	class_destroy(xh2a_fast_memory_class);
	class_destroy(xh2a_memory_transfer_class);
	class_destroy(xh2a_memory_allocator_class);
	class_destroy(xh2a_ctl_class);
	unregister_chrdev(xh2a_global_chrdev_major, "xh2a");

module_exit:
	return rc;
}

static void __exit xh2a_host_exit(void)
{
	xh2a_pcie_unregister_driver();

	xh2a_transport_unregister_driver();

	xh2a_rpmsg_unregister_driver();

	xh2a_i2c_unregister_driver();

	xh2a_qspi_unregister_driver();

	xh2a_sys_unregister_driver();

	xh2a_ipu_unregister_driver();

	xh2a_fast_memory_unregister_driver();

	xh2a_memory_transfer_unregister_driver();

	xh2a_memory_allocator_unregister_driver();

	xh2a_ctl_exit(ctl_ctx);
	ctl_ctx = NULL;

	xh2a_chrdev_registry_destroy();

	class_destroy(xh2a_transport_class);
	class_destroy(xh2a_rpmsg_lite_class);
	class_destroy(xh2a_i2c_class);
	class_destroy(xh2a_qspi_class);
	class_destroy(xh2a_sys_class);
	class_destroy(xh2a_ipu_class);
	class_destroy(xh2a_fast_memory_class);
	class_destroy(xh2a_memory_transfer_class);
	class_destroy(xh2a_memory_allocator_class);
	class_destroy(xh2a_ctl_class);
	unregister_chrdev(xh2a_global_chrdev_major, "xh2a");

	pr_info("%s: removed xh2a driver version %s: %s\n", __func__,
		XH2A_HOST_DRIVER_RELEASE, XH2A_HOST_DRIVER_BUILDTIME);
}

module_init(xh2a_host_init);
module_exit(xh2a_host_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("houmo.ai");
MODULE_DESCRIPTION("xh2a host driver");
MODULE_VERSION(XH2A_HOST_DRIVER_RELEASE);
