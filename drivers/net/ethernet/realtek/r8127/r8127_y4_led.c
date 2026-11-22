// SPDX-License-Identifier: GPL-2.0-only
/* Y4 onboard ATF ports: GPIO8 green link, GPIO5 yellow descriptor activity.
 * All configuration transactions run under RTNL. The work uses trylock so
 * an RTNL holder can cancel it before reset, suspend or freeing the rings.
 */
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/netdevice.h>
#include <linux/rtnetlink.h>
#include <linux/ethtool.h>
#include <linux/of.h>
#include "r8127.h"
#include "r8127_fiber.h"

#define Y4_LED_GREEN 0x0100
#define Y4_LED_YELLOW 0x0020
#define Y4_LED_MASK (Y4_LED_GREEN | Y4_LED_YELLOW)
#define Y4_LED_PERIOD msecs_to_jiffies(100)

static bool rtl8127_y4_led_matches(struct rtl8127_private *tp)
{
        struct pci_dev *pdev = tp->pci_dev;
        struct pci_bus *bus = pdev->bus;
        struct device_node *node;
        u32 id;

        if ((!of_machine_is_compatible("myt,y4") &&
             !of_machine_is_compatible("myt,y12")) ||
            tp->HwFiberModeVer != FIBER_MODE_RTL8127ATF ||
            !bus->self || !pci_is_pcie(bus->self) ||
            pci_pcie_type(bus->self) != PCI_EXP_TYPE_ROOT_PORT)
                return false;
        while (bus->parent)
                bus = bus->parent;
        node = pci_bus_to_OF_node(bus);
        return node && of_device_is_compatible(node, "cix,sky1-pcie-host") &&
               !of_property_read_u32(node, "sky1,pcie-ctrl-id", &id) &&
               (id == 2 || id == 4);
}

bool rtl8127_y4_led_owned(struct rtl8127_private *tp)
{
        return tp->y4_led.supported;
}

static int rtl8127_y4_led_update(struct rtl8127_private *tp, u16 reg, u16 bits)
{
        unsigned long flags;
        u16 old, value;
        int ret = 0;

        ASSERT_RTNL();
        if (!READ_ONCE(tp->hw_powered) || pci_channel_offline(tp->pci_dev))
                return -ENODEV;
        /* RTNL serializes MAC configuration; phy_lock also excludes the
         * existing PHY accessors sharing the MAC OCP selector. Never hold
         * this lock across work cancellation or user copies.
         */
        r8127_spin_lock(&tp->phy_lock, flags);
        old = rtl8127_mac_ocp_read(tp, reg);
        if (old == 0xffff) {
                ret = -ENODEV;
                goto out;
        }
        value = (old & ~Y4_LED_MASK) | (bits & Y4_LED_MASK);
        if (value != old)
                rtl8127_mac_ocp_write(tp, reg, value);
out:
        r8127_spin_unlock(&tp->phy_lock, flags);
        return ret;
}

static int rtl8127_y4_led_output(struct rtl8127_private *tp, u16 bits)
{
        struct rtl8127_y4_led *led = &tp->y4_led;
        int ret;

        if (led->output_valid && led->output == bits)
                return 0;
        ret = rtl8127_y4_led_update(tp, 0xDC52, bits);
        if (!ret) {
                led->output = bits;
                led->output_valid = true;
        }
        return ret;
}

static void rtl8127_y4_led_baseline(struct rtl8127_private *tp,
                                   unsigned long now)
{
        struct rtl8127_y4_led *led = &tp->y4_led;
        unsigned int i;

        for (i = 0; i < tp->num_rx_rings; i++)
                led->rx[i] = READ_ONCE(tp->rx_ring[i].cur_rx);
        for (i = 0; i < tp->num_tx_rings; i++)
                led->tx[i] = READ_ONCE(tp->tx_ring[i].dirty_tx);
        led->sampled = now;
        led->phase = false;
}

static bool rtl8127_y4_led_activity(struct rtl8127_private *tp,
                                   unsigned long now)
{
        struct rtl8127_y4_led *led = &tp->y4_led;
        bool active = false;
        unsigned int i;
        u32 value;

        for (i = 0; i < tp->num_rx_rings; i++) {
                value = READ_ONCE(tp->rx_ring[i].cur_rx);
                active |= value != led->rx[i];
                led->rx[i] = value;
        }
        for (i = 0; i < tp->num_tx_rings; i++) {
                value = READ_ONCE(tp->tx_ring[i].dirty_tx);
                active |= value != led->tx[i];
                led->tx[i] = value;
        }
        /* Inequality handles ordinary u32 wrap. After a long scheduling
         * gap discard old history, including possible full-wrap aliases.
         * Reset/ring changes always establish a fresh baseline separately.
         */
        if (time_after(now, led->sampled + HZ))
                active = false;
        led->sampled = now;
        return active;
}

void rtl8127_y4_led_reset(struct rtl8127_private *tp)
{
        tp->y4_led.configured = false;
        tp->y4_led.output_valid = false;
}

static int rtl8127_y4_led_verify(struct rtl8127_private *tp)
{
        const u16 regs[] = { 0xDC4C, 0xDC5E, 0xDC46, 0xDC52 };
        unsigned long flags;
        unsigned int i;
        u16 value;
        int ret = 0;

        /* Read back mode and the initial off state only at configuration
         * boundaries. Ordinary samples retain a single output RMW.
         */
        r8127_spin_lock(&tp->phy_lock, flags);
        for (i = 0; i < ARRAY_SIZE(regs); i++) {
                value = rtl8127_mac_ocp_read(tp, regs[i]);
                if (value == 0xffff) {
                        ret = -ENODEV;
                        break;
                }
                if ((value & Y4_LED_MASK) != (i == 3 ? 0 : Y4_LED_MASK)) {
                        ret = -EIO;
                        break;
                }
        }
        r8127_spin_unlock(&tp->phy_lock, flags);
        return ret;
}

int rtl8127_y4_led_prepare(struct rtl8127_private *tp)
{
        struct rtl8127_y4_led *led = &tp->y4_led;
        int ret;

        if (!led->supported)
                return 0;
        ASSERT_RTNL();
        rtl8127_y4_led_reset(tp);
        /* Preserve the vendor GPIO-enable order, starting with both lamps
         * off. No LEDSEL table or unverified PHY event bits are involved.
         */
        ret = rtl8127_y4_led_update(tp, 0xDC4C, Y4_LED_MASK);
        if (ret)
                return ret;
        ret = rtl8127_y4_led_output(tp, 0);
        if (ret)
                return ret;
        ret = rtl8127_y4_led_update(tp, 0xDC5E, Y4_LED_MASK);
        if (ret)
                return ret;
        ret = rtl8127_y4_led_update(tp, 0xDC46, Y4_LED_MASK);
        if (ret)
                return ret;
        ret = rtl8127_y4_led_verify(tp);
        if (ret)
                return ret;
        led->configured = true;
        rtl8127_y4_led_baseline(tp, jiffies);
        return 0;
}

static void rtl8127_y4_led_queue(struct rtl8127_private *tp,
                                unsigned long delay)
{
        unsigned long flags;

        /* Same admission lock as stop_io and control workers: stop cannot
         * race a check-then-enqueue after cancel_delayed_work_sync returns.
         */
        spin_lock_irqsave(&tp->hw_access_lock, flags);
        if (tp->y4_led.supported && READ_ONCE(tp->y4_led.allowed) &&
            !READ_ONCE(tp->y4_led.locate) && rtl8127_is_running(tp))
                schedule_delayed_work(&tp->y4_led.work, delay);
        spin_unlock_irqrestore(&tp->hw_access_lock, flags);
}

static void rtl8127_y4_led_gate(struct rtl8127_private *tp, bool allowed,
                               bool locate)
{
        unsigned long flags;

        spin_lock_irqsave(&tp->hw_access_lock, flags);
        WRITE_ONCE(tp->y4_led.allowed, allowed);
        WRITE_ONCE(tp->y4_led.locate, locate);
        spin_unlock_irqrestore(&tp->hw_access_lock, flags);
}

void rtl8127_y4_led_start(struct rtl8127_private *tp)
{
        ASSERT_RTNL();
        if (!tp->y4_led.supported || !tp->y4_led.configured)
                return;
        rtl8127_y4_led_gate(tp, true, false);
        rtl8127_y4_led_queue(tp, 0);
}

void rtl8127_y4_led_stop(struct rtl8127_private *tp)
{
        struct rtl8127_y4_led *led = &tp->y4_led;

        if (!led->supported)
                return;
        ASSERT_RTNL();
        rtl8127_y4_led_gate(tp, false, false);
        cancel_delayed_work_sync(&led->work);
        if (led->configured)
                rtl8127_y4_led_output(tp, 0);
        rtl8127_y4_led_reset(tp);
        led->phase = false;
}

static void rtl8127_y4_led_fault(struct rtl8127_private *tp)
{
        rtl8127_y4_led_gate(tp, false, false);
        rtl8127_y4_led_reset(tp);
        /* No retry loop against an inaccessible endpoint. A subsequent
         * successful lifecycle recovery will configure and restart LEDs.
         */
        netdev_warn(tp->dev, "Y4 LED access failed; sampling stopped until recovery\n");
}

static void rtl8127_y4_led_work(struct work_struct *work)
{
        struct rtl8127_private *tp = container_of(work, struct rtl8127_private,
                                                 y4_led.work.work);
        struct rtl8127_y4_led *led = &tp->y4_led;
        u32 status;
        bool link, active;
        u16 output;

        if (!rtnl_trylock()) {
                rtl8127_y4_led_queue(tp, Y4_LED_PERIOD);
                return;
        }
        if (!led->allowed || led->locate || !rtl8127_is_running(tp))
                goto out;
        if (pci_channel_offline(tp->pci_dev))
                goto fault;
        status = RTL_R32(tp, PHYstatus);
        if (status == ~0U)
                goto fault;
        link = !!(status & LinkStatus);
        active = rtl8127_y4_led_activity(tp, jiffies);
        led->phase = link && active ? !led->phase : false;
        output = (link ? Y4_LED_GREEN : 0) | (led->phase ? Y4_LED_YELLOW : 0);
        if (rtl8127_y4_led_output(tp, output))
                goto fault;
out:
        rtnl_unlock();
        rtl8127_y4_led_queue(tp, Y4_LED_PERIOD);
        return;
fault:
        rtl8127_y4_led_fault(tp);
        rtnl_unlock();
}

void rtl8127_y4_led_init(struct rtl8127_private *tp)
{
        INIT_DELAYED_WORK(&tp->y4_led.work, rtl8127_y4_led_work);
        tp->y4_led.supported = rtl8127_y4_led_matches(tp);
}

int rtl8127_y4_led_phys_id(struct rtl8127_private *tp,
                           enum ethtool_phys_id_state state)
{
        struct rtl8127_y4_led *led = &tp->y4_led;
        int ret;

        ASSERT_RTNL();
        if (state == ETHTOOL_ID_INACTIVE && !led->locate)
                return 0; /* A lifecycle stop already ended this session. */
        if (!rtl8127_is_running(tp))
                return -ENETDOWN;
        switch (state) {
        case ETHTOOL_ID_ACTIVE:
                rtl8127_y4_led_stop(tp);
                ret = rtl8127_y4_led_prepare(tp);
                if (ret)
                        return ret;
                rtl8127_y4_led_gate(tp, false, true);
                return 2;
        case ETHTOOL_ID_ON:
        case ETHTOOL_ID_OFF:
                if (!led->locate)
                        return -ECANCELED;
                ret = rtl8127_y4_led_output(tp,
                        state == ETHTOOL_ID_ON ? Y4_LED_MASK : 0);
                if (ret)
                        rtl8127_y4_led_fault(tp);
                return ret;
        case ETHTOOL_ID_INACTIVE:
                rtl8127_y4_led_gate(tp, false, false);
                ret = rtl8127_y4_led_prepare(tp);
                if (ret) {
                        /* ethtool core ignores this callback's return value. */
                        rtl8127_y4_led_fault(tp);
                        return ret;
                }
                rtl8127_y4_led_start(tp);
                return 0;
        default:
                return -EOPNOTSUPP;
        }
}
