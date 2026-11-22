// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2024 Houmo AI Inc.
 * Author: hongxing.ma<hongxing.ma@houmo.ai>
 */

#ifndef _XH2A_HOST_DRV_H_
#define _XH2A_HOST_DRV_H_

#include <xh2a_ctl.h>
#include <xh2a_chrdev_layout.h>
#include <linux/types.h>

/*
 * chrdev global state.
 * Initialized in xh2a_host_init() (insmod), shared by all sub-modules.
 */
extern int xh2a_global_chrdev_major;
extern struct class *xh2a_ctl_class;
extern struct class *xh2a_memory_allocator_class;
extern struct class *xh2a_memory_transfer_class;
extern struct class *xh2a_fast_memory_class;
extern struct class *xh2a_ipu_class;
extern struct class *xh2a_sys_class;
extern struct class *xh2a_qspi_class;
extern struct class *xh2a_i2c_class;
extern struct class *xh2a_rpmsg_lite_class;
extern struct class *xh2a_transport_class;

/*
 * XH2A_CHRDEV_SUB_MINOR_MAX - minor count per PCIe card (must be power of 2
 * or idr-safe; set to 16 to allow future expansion without renumbering)
 */
#ifndef XH2A_CHRDEV_SUB_MINOR_MAX
#define XH2A_CHRDEV_SUB_MINOR_MAX 16
#endif

/*
 * Sub-minor offsets per sub-module.
 * dev_t = MKDEV(xh2a_global_chrdev_major,
 *               pcie_card_minor * XH2A_CHRDEV_SUB_MINOR_MAX + sub_offset)
 */
struct xh2a_sys_dev;

/*
 * xh2a_host_register_notifier_chain() - register notifier chain
 * @nb: notifier block to register
 * Return: 0 on success, negative error code on failure
 */
int xh2a_host_register_notifier_chain(struct notifier_block *nb);

/*
 * xh2a_host_unregister_notifier_chain() - unregister notifier chain
 * @nb: notifier block to unregister
 */
void xh2a_host_unregister_notifier_chain(struct notifier_block *nb);

/*
 * xh2a_host_call_notifier_chain() - call notifier chain
 * @val: event value
 * @v: notifier chain
 * Return: 0 on success, negative error code on failure
 */
int xh2a_host_call_notifier_chain(int val, void *v);

/*
 * XH2A_HOST_NOTIFY_PCIE_PROBE - notify when PCIe device is probed
 * XH2A_HOST_NOTIFY_PCIE_REMOVE - notify when PCIe device is removed
 * XH2A_HOST_NOTIFY_PCIE_SHUTDOWN - notify when PCIe device is shutdown
 */
#define XH2A_HOST_NOTIFY_PCIE_PROBE    100
#define XH2A_HOST_NOTIFY_PCIE_REMOVE   200
#define XH2A_HOST_NOTIFY_PCIE_SHUTDOWN 300

int xh2a_pcie_register_driver(void);
void xh2a_pcie_unregister_driver(void);

int xh2a_sys_register_driver(void);
void xh2a_sys_unregister_driver(void);

int xh2a_memory_allocator_register_driver(void);
void xh2a_memory_allocator_unregister_driver(void);

int xh2a_memory_transfer_register_driver(void);
void xh2a_memory_transfer_unregister_driver(void);

int xh2a_transport_register_driver(void);
void xh2a_transport_unregister_driver(void);

int xh2a_qspi_register_driver(void);
void xh2a_qspi_unregister_driver(void);

int xh2a_i2c_register_driver(void);
void xh2a_i2c_unregister_driver(void);

int xh2a_ipu_register_driver(void);
void xh2a_ipu_unregister_driver(void);

int xh2a_rpmsg_register_driver(void);
void xh2a_rpmsg_unregister_driver(void);

int xh2a_fast_memory_register_driver(void);
void xh2a_fast_memory_unregister_driver(void);

int xh2a_ctl_init(struct xh2a_ctl_ctx **ctl_out);
void xh2a_ctl_exit(struct xh2a_ctl_ctx *ctl);
int xh2a_ctl_register_dev(struct xh2a_ctl_ctx *ctl, uint32_t dev_id,
			  void *handle);
int xh2a_ctl_get_ctx(struct xh2a_ctl_ctx **ctl_out);
void xh2a_ctl_ctc_group_list_clear(struct xh2a_ctl_ctx *ctl);
void xh2a_ctl_ctc_lifecycle_lock(struct xh2a_ctl_ctx *ctl);
void xh2a_ctl_ctc_lifecycle_unlock(struct xh2a_ctl_ctx *ctl);
int xh2a_ctl_ctc_mark_ipu_ready(struct xh2a_ctl_ctx *ctl, uint32_t dev_id);
void xh2a_ctl_ctc_mark_dev_removing(struct xh2a_ctl_ctx *ctl,
				    uint32_t dev_id);
void xh2a_ctl_destroy_lock(struct xh2a_ctl_ctx *ctl, uint32_t dev_id);
int xh2a_ctl_ctc_begin_ipu_timeout_recovery(void *timeout_handle);

#endif
