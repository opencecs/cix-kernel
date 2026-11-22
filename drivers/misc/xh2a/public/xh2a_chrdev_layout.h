// SPDX-License-Identifier: GPL-2.0
/*
 * Character-device minor layout shared by the Linux driver and HAL.
 * Changing these values changes the device-node ABI.
 */
#ifndef XH2A_CHRDEV_LAYOUT_H_
#define XH2A_CHRDEV_LAYOUT_H_

#define XH2A_CHRDEV_SUB_MINOR_MAX 16

#define XH2A_CHRDEV_SUB_CTL              0
#define XH2A_CHRDEV_SUB_MEMORY_ALLOCATOR 1
#define XH2A_CHRDEV_SUB_MEMORY_TRANSFER  4
#define XH2A_CHRDEV_SUB_FAST_MEMORY      5
#define XH2A_CHRDEV_SUB_IPU              6
#define XH2A_CHRDEV_SUB_SYS              7
#define XH2A_CHRDEV_SUB_QSPI             8
#define XH2A_CHRDEV_SUB_I2C              9
#define XH2A_CHRDEV_SUB_RPMSG_LITE       10
#define XH2A_CHRDEV_SUB_TRANSPORT        11

#endif /* XH2A_CHRDEV_LAYOUT_H_ */
