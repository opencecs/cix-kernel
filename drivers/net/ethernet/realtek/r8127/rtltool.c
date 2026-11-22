// SPDX-License-Identifier: GPL-2.0-only
/*
################################################################################
#
# r8127 is the Linux device driver released for Realtek 10 Gigabit Ethernet
# controllers with PCI-Express interface.
#
# Copyright(c) 2026 Realtek Semiconductor Corp. All rights reserved.
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by the Free
# Software Foundation; either version 2 of the License, or (at your option)
# any later version.
#
# This program is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
# FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
# more details.
#
# You should have received a copy of the GNU General Public License along with
# this program; if not, see <http://www.gnu.org/licenses/>.
#
# Author:
# Realtek NIC software team <nicfae@realtek.com>
# No. 2, Innovation Road II, Hsinchu Science Park, Hsinchu 300, Taiwan
#
################################################################################
*/

/************************************************************************************
 *  This product is covered by one or more of the following patents:
 *  US6,570,884, US6,115,776, and US6,327,625.
 ***********************************************************************************/

#include <linux/module.h>
#include <linux/version.h>
#include <linux/pci.h>
#include <linux/of.h>
#include <linux/netdevice.h>
#include <linux/delay.h>
#include <linux/in.h>
#include <linux/ethtool.h>
#include <asm/uaccess.h>
#include "r8127.h"
#include "r8127_fiber.h"
#include "rtl_eeprom.h"
#include "rtltool.h"

int rtl8127_tool_ioctl(struct rtl8127_private *tp, struct ifreq *ifr)
{
        struct rtltool_sds_cmd *sds_cmd;
        struct rtltool_cmd my_cmd;
        unsigned long flags;
        int ret;

        /* ndo ioctl dispatch holds RTNL, as do recovery and PM. */
        if (!rtl8127_hw_accessible(tp))
                return -ENODEV;

        if (copy_from_user(&my_cmd, ifr->ifr_data, sizeof(my_cmd)))
                return -EFAULT;

        /* Raw configuration can bypass the driver's DMA/power ownership.
         * Keep diagnostics readable; permit only PHY/EEPROM maintenance on
         * an already quiesced, closed device. GPIO is handled below using
         * a restricted Y4 mask for the existing boardd during migration.
         */
        switch (my_cmd.cmd) {
        case RTLTOOL_WRITE_MAC:
        case RTLTOOL_WRITE_ERI:
        case RTLTOOL_WRITE_PCI:
        case RTL_WRITE_OOB_MAC:
        case RTL_ENABLE_PCI_DIAG:
                return -EOPNOTSUPP;
        case RTLTOOL_WRITE_PHY:
        case RTLTOOL_WRITE_EPHY:
        case RTLTOOL_WRITE_EEPROM:
        case RTL_DIRECT_WRITE_PHY_OCP:
        case RTL_WRITE_FIBER_PHY:
        case RTL_WRITE_SDS_PHY:
                if (tp->lifecycle_state != RTL8127_STATE_CLOSED ||
                    !tp->dma_quiesced)
                        return -EBUSY;
                break;
        default:
                break;
        }

        sds_cmd = (struct rtltool_sds_cmd*)&my_cmd;
        ret = 0;
        switch (my_cmd.cmd) {
        case RTLTOOL_READ_MAC:
                if (my_cmd.offset >= pci_resource_len(tp->pci_dev, 2) ||
                    my_cmd.len > pci_resource_len(tp->pci_dev, 2) - my_cmd.offset) {
                        ret = -EINVAL;
                        break;
                }

                if (my_cmd.len==1)
                        my_cmd.data = readb(tp->mmio_addr+my_cmd.offset);
                else if (my_cmd.len==2)
                        my_cmd.data = readw(tp->mmio_addr+(my_cmd.offset&~1));
                else if (my_cmd.len==4)
                        my_cmd.data = readl(tp->mmio_addr+(my_cmd.offset&~3));
                else {
                        ret = -EOPNOTSUPP;
                        break;
                }

                if (copy_to_user(ifr->ifr_data, &my_cmd, sizeof(my_cmd))) {
                        ret = -EFAULT;
                        break;
                }
                break;

        case RTLTOOL_WRITE_MAC:
                if (my_cmd.offset >= pci_resource_len(tp->pci_dev, 2) ||
                    my_cmd.len > pci_resource_len(tp->pci_dev, 2) - my_cmd.offset) {
                        ret = -EINVAL;
                        break;
                }

                if (my_cmd.len==1)
                        writeb(my_cmd.data, tp->mmio_addr+my_cmd.offset);
                else if (my_cmd.len==2)
                        writew(my_cmd.data, tp->mmio_addr+(my_cmd.offset&~1));
                else if (my_cmd.len==4)
                        writel(my_cmd.data, tp->mmio_addr+(my_cmd.offset&~3));
                else {
                        ret = -EOPNOTSUPP;
                        break;
                }

                break;

        case RTLTOOL_READ_PHY:
                r8127_spin_lock(&tp->phy_lock, flags);
                my_cmd.data = rtl8127_mdio_prot_read(tp, my_cmd.offset);
                r8127_spin_unlock(&tp->phy_lock, flags);
                if (copy_to_user(ifr->ifr_data, &my_cmd, sizeof(my_cmd))) {
                        ret = -EFAULT;
                        break;
                }

                break;

        case RTLTOOL_WRITE_PHY:
                r8127_spin_lock(&tp->phy_lock, flags);
                rtl8127_mdio_prot_write(tp, my_cmd.offset, my_cmd.data);
                r8127_spin_unlock(&tp->phy_lock, flags);
                break;

        case RTLTOOL_READ_EPHY:
                my_cmd.data = rtl8127_ephy_read(tp, my_cmd.offset);
                if (copy_to_user(ifr->ifr_data, &my_cmd, sizeof(my_cmd))) {
                        ret = -EFAULT;
                        break;
                }

                break;

        case RTLTOOL_WRITE_EPHY:
                rtl8127_ephy_write(tp, my_cmd.offset, my_cmd.data);
                break;

        case RTLTOOL_READ_ERI:
                my_cmd.data = 0;
                if (my_cmd.len==1 || my_cmd.len==2 || my_cmd.len==4) {
                        my_cmd.data = rtl8127_eri_read(tp, my_cmd.offset, my_cmd.len, ERIAR_ExGMAC);
                } else {
                        ret = -EOPNOTSUPP;
                        break;
                }

                if (copy_to_user(ifr->ifr_data, &my_cmd, sizeof(my_cmd))) {
                        ret = -EFAULT;
                        break;
                }

                break;

        case RTLTOOL_WRITE_ERI:
                if (my_cmd.len==1 || my_cmd.len==2 || my_cmd.len==4) {
                        rtl8127_eri_write(tp, my_cmd.offset, my_cmd.len, my_cmd.data, ERIAR_ExGMAC);
                } else {
                        ret = -EOPNOTSUPP;
                        break;
                }
                break;

        case RTLTOOL_READ_PCI:
                my_cmd.data = 0;
                if (my_cmd.len==1)
                        pci_read_config_byte(tp->pci_dev, my_cmd.offset,
                                             (u8 *)&my_cmd.data);
                else if (my_cmd.len==2)
                        pci_read_config_word(tp->pci_dev, my_cmd.offset,
                                             (u16 *)&my_cmd.data);
                else if (my_cmd.len==4)
                        pci_read_config_dword(tp->pci_dev, my_cmd.offset,
                                              &my_cmd.data);
                else {
                        ret = -EOPNOTSUPP;
                        break;
                }

                if (copy_to_user(ifr->ifr_data, &my_cmd, sizeof(my_cmd))) {
                        ret = -EFAULT;
                        break;
                }
                break;

        case RTLTOOL_WRITE_PCI:
                if (my_cmd.len==1)
                        pci_write_config_byte(tp->pci_dev, my_cmd.offset,
                                              my_cmd.data);
                else if (my_cmd.len==2)
                        pci_write_config_word(tp->pci_dev, my_cmd.offset,
                                              my_cmd.data);
                else if (my_cmd.len==4)
                        pci_write_config_dword(tp->pci_dev, my_cmd.offset,
                                               my_cmd.data);
                else {
                        ret = -EOPNOTSUPP;
                        break;
                }

                break;

        case RTLTOOL_READ_EEPROM:
                my_cmd.data = rtl8127_eeprom_read_sc(tp, my_cmd.offset);
                if (copy_to_user(ifr->ifr_data, &my_cmd, sizeof(my_cmd))) {
                        ret = -EFAULT;
                        break;
                }

                break;

        case RTLTOOL_WRITE_EEPROM:
                rtl8127_eeprom_write_sc(tp, my_cmd.offset, my_cmd.data);
                break;

        case RTL_READ_OOB_MAC:
                rtl8127_oob_mutex_lock(tp);
                my_cmd.data = rtl8127_ocp_read(tp, my_cmd.offset, 4);
                rtl8127_oob_mutex_unlock(tp);
                if (copy_to_user(ifr->ifr_data, &my_cmd, sizeof(my_cmd))) {
                        ret = -EFAULT;
                        break;
                }
                break;

        case RTL_WRITE_OOB_MAC:
                if (my_cmd.len == 0 || my_cmd.len > 4)
                        return -EOPNOTSUPP;

                rtl8127_oob_mutex_lock(tp);
                rtl8127_ocp_write(tp, my_cmd.offset, my_cmd.len, my_cmd.data);
                rtl8127_oob_mutex_unlock(tp);
                break;

        case RTL_ENABLE_PCI_DIAG:
                r8127_spin_lock(&tp->phy_lock, flags);
                tp->rtk_enable_diag = 1;
                r8127_spin_unlock(&tp->phy_lock, flags);

                dprintk("enable rtk diag\n");
                break;

        case RTL_DISABLE_PCI_DIAG:
                r8127_spin_lock(&tp->phy_lock, flags);
                tp->rtk_enable_diag = 0;
                r8127_spin_unlock(&tp->phy_lock, flags);

                dprintk("disable rtk diag\n");
                break;

        case RTL_READ_MAC_OCP:
                if (my_cmd.offset % 2)
                        return -EOPNOTSUPP;

                my_cmd.data = rtl8127_mac_ocp_read(tp, my_cmd.offset);
                if (copy_to_user(ifr->ifr_data, &my_cmd, sizeof(my_cmd))) {
                        ret = -EFAULT;
                        break;
                }
                break;

        case RTL_WRITE_MAC_OCP: {
                const u16 gpio_mask = 0x0120;
                u16 value;

                if (rtl8127_y4_led_owned(tp))
                        return -EBUSY;

                /* Legacy GPIO access is only for ports without driver LEDs.
                 * Never expose arbitrary MAC OCP writes to a running DMA
                 * engine, and preserve every unrelated GPIO pin.
                 */
                if ((!of_machine_is_compatible("myt,y4") &&
                     !of_machine_is_compatible("myt,y12")) ||
                    tp->HwFiberModeVer != FIBER_MODE_RTL8127ATF ||
                    tp->lifecycle_state != RTL8127_STATE_RUNNING ||
                    my_cmd.len != 2 || my_cmd.data > 0xffff ||
                    (my_cmd.offset != 0xDC46 && my_cmd.offset != 0xDC4C &&
                     my_cmd.offset != 0xDC52 && my_cmd.offset != 0xDC5E))
                        return -EOPNOTSUPP;

                r8127_spin_lock(&tp->phy_lock, flags);
                value = rtl8127_mac_ocp_read(tp, my_cmd.offset);
                if (value == 0xffff) {
                        ret = -ENODEV;
                } else {
                        value = (value & ~gpio_mask) |
                                (my_cmd.data & gpio_mask);
                        rtl8127_mac_ocp_write(tp, my_cmd.offset, value);
                }
                r8127_spin_unlock(&tp->phy_lock, flags);
                break;
        }

        case RTL_DIRECT_READ_PHY_OCP:
                r8127_spin_lock(&tp->phy_lock, flags);
                my_cmd.data = rtl8127_mdio_prot_direct_read_phy_ocp(tp, my_cmd.offset);
                r8127_spin_unlock(&tp->phy_lock, flags);
                if (copy_to_user(ifr->ifr_data, &my_cmd, sizeof(my_cmd))) {
                        ret = -EFAULT;
                        break;
                }

                break;

        case RTL_DIRECT_WRITE_PHY_OCP:
                r8127_spin_lock(&tp->phy_lock, flags);
                rtl8127_mdio_prot_direct_write_phy_ocp(tp, my_cmd.offset, my_cmd.data);
                r8127_spin_unlock(&tp->phy_lock, flags);
                break;

        case RTL_READ_SDS_PHY:
                if (!HW_FIBER_MODE_ENABLED(tp)) {
                        ret = -EOPNOTSUPP;
                        break;
                }

                r8127_spin_lock(&tp->phy_lock, flags);
                sds_cmd->data = rtl8127_sds_phy_read_8127(tp, sds_cmd->index,
                                sds_cmd->page, sds_cmd->addr);
                r8127_spin_unlock(&tp->phy_lock, flags);
                if (copy_to_user(ifr->ifr_data, &my_cmd, sizeof(my_cmd))) {
                        ret = -EFAULT;
                        break;
                }

                break;

        case RTL_WRITE_SDS_PHY:
                if (!HW_FIBER_MODE_ENABLED(tp)) {
                        ret = -EOPNOTSUPP;
                        break;
                }

                r8127_spin_lock(&tp->phy_lock, flags);
                rtl8127_sds_phy_write_8127(tp, sds_cmd->index, sds_cmd->page,
                                           sds_cmd->addr, sds_cmd->data);
                r8127_spin_unlock(&tp->phy_lock, flags);
                break;

        default:
                ret = -EOPNOTSUPP;
                break;
        }

        return ret;
}
