/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _R8127_Y4_LED_H
#define _R8127_Y4_LED_H

#include <linux/ethtool.h>
#include <linux/workqueue.h>

struct rtl8127_private;

/* Included after the driver's queue limits and workqueue definitions. */
struct rtl8127_y4_led {
        struct delayed_work work;
        u32 rx[R8127_MAX_RX_QUEUES];
        u32 tx[R8127_MAX_TX_QUEUES];
        unsigned long sampled;
        u16 output;
        bool supported;
        bool allowed;
        bool configured;
        bool output_valid;
        bool locate;
        bool phase;
};

void rtl8127_y4_led_init(struct rtl8127_private *tp);
bool rtl8127_y4_led_owned(struct rtl8127_private *tp);
int rtl8127_y4_led_prepare(struct rtl8127_private *tp);
void rtl8127_y4_led_start(struct rtl8127_private *tp);
void rtl8127_y4_led_stop(struct rtl8127_private *tp);
void rtl8127_y4_led_reset(struct rtl8127_private *tp);
int rtl8127_y4_led_phys_id(struct rtl8127_private *tp,
                           enum ethtool_phys_id_state state);

#endif
