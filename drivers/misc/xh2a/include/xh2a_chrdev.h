/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright(c) 2026 Houmo AI Inc.
 */

#ifndef _XH2A_CHRDEV_H_
#define _XH2A_CHRDEV_H_

#include <linux/device.h>
#include <linux/fs.h>

typedef int (*xh2a_chrdev_get_cb)(void *drvdata);

struct device *xh2a_chrdev_create(struct class *cls, struct device *parent,
				  dev_t devt, void *drvdata,
				  const struct file_operations *fops,
				  const struct attribute_group **groups,
				  const char *fmt, ...);

void xh2a_chrdev_destroy(struct class *cls, dev_t devt, void *drvdata);

int xh2a_chrdev_get_drvdata(struct inode *inode, xh2a_chrdev_get_cb get,
			    void **drvdata);

void xh2a_chrdev_registry_destroy(void);

#endif /* _XH2A_CHRDEV_H_ */
