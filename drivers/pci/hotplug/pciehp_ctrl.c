// SPDX-License-Identifier: GPL-2.0+
/*
 * PCI Express Hot Plug Controller Driver
 *
 * Copyright (C) 1995,2001 Compaq Computer Corporation
 * Copyright (C) 2001 Greg Kroah-Hartman (greg@kroah.com)
 * Copyright (C) 2001 IBM Corp.
 * Copyright (C) 2003-2004 Intel Corporation
 *
 * All rights reserved.
 *
 * Send feedback to <greg@kroah.com>, <kristen.c.accardi@intel.com>
 *
 */

#define dev_fmt(fmt) "pciehp: " fmt

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/pm_runtime.h>
#include <linux/of.h>
#include <linux/pci.h>
#include "pciehp.h"

/* The following routines constitute the bulk of the
   hotplug controller logic
 */

#define SAFE_REMOVAL	 true
#define SURPRISE_REMOVAL false

/*
 * M50 firmware reboot drops the link while the card remains installed.  On
 * Y4, cycling the IX8024 slot power during that reboot drops an already
 * recovering link a second time.  Keep power only for a known M50 on the
 * fixed-256 fabric; still remove/re-enumerate it so reset configuration and
 * driver state are rebuilt before DMA is enabled.
 */
static bool pciehp_m50_slot_occupied(struct controller *ctrl)
{
	struct pci_dev *port = ctrl->pcie->port;
	u16 status;
	int ret;

	if (READ_ONCE(ctrl->power_fault_detected) ||
	    (atomic_read(&ctrl->pending_events) &
	     (PCI_EXP_SLTSTA_PDC | PCI_EXP_SLTSTA_PFD | DISABLE_SLOT)))
		return false;

	ret = pcie_capability_read_word(port, PCI_EXP_SLTSTA, &status);
	if (ret || PCI_POSSIBLE_ERROR(status))
		return false;

	return (status & PCI_EXP_SLTSTA_PDS) &&
		!(status & (PCI_EXP_SLTSTA_PDC | PCI_EXP_SLTSTA_PFD |
			    PCI_EXP_SLTSTA_MRLSS));
}

static bool pciehp_m50_keep_power(struct controller *ctrl, u32 events)
{
	struct pci_dev *port = ctrl->pcie->port;
	struct pci_dev *dev;
	bool m50 = false;

	if (events != PCI_EXP_SLTSTA_DLLSC || !POWER_CTRL(ctrl) ||
	    !of_machine_is_compatible("myt,y4") ||
	    pci_find_host_bridge(port->bus)->fixed_mps != 256 ||
	    port->vendor != 0x1ec8 || port->device != 0x8024 ||
	    pci_pcie_type(port) != PCI_EXP_TYPE_DOWNSTREAM ||
	    !port->subordinate || !pciehp_m50_slot_occupied(ctrl))
		return false;

	/* The reboot tool may concurrently remove/rescan the endpoint. */
	pci_lock_rescan_remove();
	dev = pci_get_slot(port->subordinate, PCI_DEVFN(0, 0));
	if (dev) {
		m50 = dev->vendor == 0x1f6b && dev->device == 0x0c00;
		pci_dev_put(dev);
	}
	pci_unlock_rescan_remove();

	return m50;
}

static void set_slot_off(struct controller *ctrl)
{
	/*
	 * Turn off slot, turn on attention indicator, turn off power
	 * indicator
	 */
	if (POWER_CTRL(ctrl)) {
		pciehp_power_off_slot(ctrl);

		/*
		 * After turning power off, we must wait for at least 1 second
		 * before taking any action that relies on power having been
		 * removed from the slot/adapter.
		 */
		msleep(1000);
	}

	pciehp_set_indicators(ctrl, PCI_EXP_SLTCTL_PWR_IND_OFF,
			      PCI_EXP_SLTCTL_ATTN_IND_ON);
}

/**
 * board_added - Called after a board has been added to the system.
 * @ctrl: PCIe hotplug controller where board is added
 *
 * Turns power on for the board.
 * Configures board.
 */
static int board_added(struct controller *ctrl)
{
	int retval = 0;
	struct pci_bus *parent = ctrl->pcie->port->subordinate;

	if (POWER_CTRL(ctrl)) {
		/* Power on slot */
		retval = pciehp_power_on_slot(ctrl);
		if (retval)
			return retval;
	}

	pciehp_set_indicators(ctrl, PCI_EXP_SLTCTL_PWR_IND_BLINK,
			      INDICATOR_NOOP);

	/* Check link training status */
	retval = pciehp_check_link_status(ctrl);
	if (retval)
		goto err_exit;

	/* Check for a power fault */
	if (ctrl->power_fault_detected || pciehp_query_power_fault(ctrl)) {
		ctrl_err(ctrl, "Slot(%s): Power fault\n", slot_name(ctrl));
		retval = -EIO;
		goto err_exit;
	}

	retval = pciehp_configure_device(ctrl);
	if (retval) {
		if (retval != -EEXIST) {
			ctrl_err(ctrl, "Cannot add device at %04x:%02x:00\n",
				 pci_domain_nr(parent), parent->number);
			goto err_exit;
		}
	}

	pciehp_set_indicators(ctrl, PCI_EXP_SLTCTL_PWR_IND_ON,
			      PCI_EXP_SLTCTL_ATTN_IND_OFF);
	return 0;

err_exit:
	set_slot_off(ctrl);
	return retval;
}

/**
 * remove_board - Turn off slot and Power Indicator
 * @ctrl: PCIe hotplug controller where board is being removed
 * @safe_removal: whether the board is safely removed (versus surprise removed)
 * @keep_power: retain slot power across a known M50 link-only reset
 */
static void remove_board(struct controller *ctrl, bool safe_removal,
			 bool keep_power)
{
	pciehp_unconfigure_device(ctrl, safe_removal);

	/* A real removal or fault during driver teardown cancels retention. */
	keep_power = keep_power && !safe_removal &&
		pciehp_m50_slot_occupied(ctrl);
	if (keep_power)
		ctrl_info(ctrl, "Slot(%s): keeping power for M50 link recovery\n",
			  slot_name(ctrl));

	if (POWER_CTRL(ctrl) && !keep_power) {
		pciehp_power_off_slot(ctrl);

		/*
		 * After turning power off, we must wait for at least 1 second
		 * before taking any action that relies on power having been
		 * removed from the slot/adapter.
		 */
		msleep(1000);

		/* Ignore link or presence changes caused by power off */
		atomic_and(~(PCI_EXP_SLTSTA_DLLSC | PCI_EXP_SLTSTA_PDC),
			   &ctrl->pending_events);
	}

	pciehp_set_indicators(ctrl, PCI_EXP_SLTCTL_PWR_IND_OFF,
			      INDICATOR_NOOP);
}

static int pciehp_enable_slot(struct controller *ctrl, bool kept_power);
static int pciehp_disable_slot(struct controller *ctrl, bool safe_removal,
			       bool keep_power);

void pciehp_request(struct controller *ctrl, int action)
{
	atomic_or(action, &ctrl->pending_events);
	if (!pciehp_poll_mode)
		irq_wake_thread(ctrl->pcie->irq, ctrl);
}

void pciehp_queue_pushbutton_work(struct work_struct *work)
{
	struct controller *ctrl = container_of(work, struct controller,
					       button_work.work);

	mutex_lock(&ctrl->state_lock);
	switch (ctrl->state) {
	case BLINKINGOFF_STATE:
		pciehp_request(ctrl, DISABLE_SLOT);
		break;
	case BLINKINGON_STATE:
		pciehp_request(ctrl, PCI_EXP_SLTSTA_PDC);
		break;
	default:
		break;
	}
	mutex_unlock(&ctrl->state_lock);
}

void pciehp_handle_button_press(struct controller *ctrl)
{
	mutex_lock(&ctrl->state_lock);
	switch (ctrl->state) {
	case OFF_STATE:
	case ON_STATE:
		if (ctrl->state == ON_STATE) {
			ctrl->state = BLINKINGOFF_STATE;
			ctrl_info(ctrl, "Slot(%s): Button press: will power off in 5 sec\n",
				  slot_name(ctrl));
		} else {
			ctrl->state = BLINKINGON_STATE;
			ctrl_info(ctrl, "Slot(%s): Button press: will power on in 5 sec\n",
				  slot_name(ctrl));
		}
		/* blink power indicator and turn off attention */
		pciehp_set_indicators(ctrl, PCI_EXP_SLTCTL_PWR_IND_BLINK,
				      PCI_EXP_SLTCTL_ATTN_IND_OFF);
		schedule_delayed_work(&ctrl->button_work, 5 * HZ);
		break;
	case BLINKINGOFF_STATE:
	case BLINKINGON_STATE:
		/*
		 * Cancel if we are still blinking; this means that we
		 * press the attention again before the 5 sec. limit
		 * expires to cancel hot-add or hot-remove
		 */
		cancel_delayed_work(&ctrl->button_work);
		if (ctrl->state == BLINKINGOFF_STATE) {
			ctrl->state = ON_STATE;
			pciehp_set_indicators(ctrl, PCI_EXP_SLTCTL_PWR_IND_ON,
					      PCI_EXP_SLTCTL_ATTN_IND_OFF);
			ctrl_info(ctrl, "Slot(%s): Button press: canceling request to power off\n",
				  slot_name(ctrl));
		} else {
			ctrl->state = OFF_STATE;
			pciehp_set_indicators(ctrl, PCI_EXP_SLTCTL_PWR_IND_OFF,
					      PCI_EXP_SLTCTL_ATTN_IND_OFF);
			ctrl_info(ctrl, "Slot(%s): Button press: canceling request to power on\n",
				  slot_name(ctrl));
		}
		break;
	default:
		ctrl_err(ctrl, "Slot(%s): Button press: ignoring invalid state %#x\n",
			 slot_name(ctrl), ctrl->state);
		break;
	}
	mutex_unlock(&ctrl->state_lock);
}

void pciehp_handle_disable_request(struct controller *ctrl)
{
	mutex_lock(&ctrl->state_lock);
	switch (ctrl->state) {
	case BLINKINGON_STATE:
	case BLINKINGOFF_STATE:
		cancel_delayed_work(&ctrl->button_work);
		break;
	}
	ctrl->state = POWEROFF_STATE;
	mutex_unlock(&ctrl->state_lock);

	ctrl->request_result = pciehp_disable_slot(ctrl, SAFE_REMOVAL, false);
}

void pciehp_handle_presence_or_link_change(struct controller *ctrl, u32 events)
{
	bool keep_power = false;
	int present, link_active;

	/*
	 * If the slot is on and presence or link has changed, turn it off.
	 * Even if it's occupied again, we cannot assume the card is the same.
	 */
	mutex_lock(&ctrl->state_lock);
	switch (ctrl->state) {
	case BLINKINGOFF_STATE:
		cancel_delayed_work(&ctrl->button_work);
		fallthrough;
	case ON_STATE:
		ctrl->state = POWEROFF_STATE;
		mutex_unlock(&ctrl->state_lock);
		if (events & PCI_EXP_SLTSTA_DLLSC)
			ctrl_info(ctrl, "Slot(%s): Link Down\n",
				  slot_name(ctrl));
		if (events & PCI_EXP_SLTSTA_PDC)
			ctrl_info(ctrl, "Slot(%s): Card not present\n",
				  slot_name(ctrl));
		keep_power = pciehp_m50_keep_power(ctrl, events);
		pciehp_disable_slot(ctrl, SURPRISE_REMOVAL, keep_power);
		break;
	default:
		mutex_unlock(&ctrl->state_lock);
		break;
	}

	/* Turn the slot on if it's occupied or link is up */
	mutex_lock(&ctrl->state_lock);
	present = pciehp_card_present(ctrl);
	link_active = pciehp_check_link_active(ctrl);
	if (present <= 0 && link_active <= 0) {
		if (ctrl->state == BLINKINGON_STATE) {
			ctrl->state = OFF_STATE;
			cancel_delayed_work(&ctrl->button_work);
			pciehp_set_indicators(ctrl, PCI_EXP_SLTCTL_PWR_IND_OFF,
					      INDICATOR_NOOP);
			ctrl_info(ctrl, "Slot(%s): Card not present\n",
				  slot_name(ctrl));
		}
		mutex_unlock(&ctrl->state_lock);
		if (keep_power)
			set_slot_off(ctrl);
		return;
	}

	switch (ctrl->state) {
	case BLINKINGON_STATE:
		cancel_delayed_work(&ctrl->button_work);
		fallthrough;
	case OFF_STATE:
		ctrl->state = POWERON_STATE;
		mutex_unlock(&ctrl->state_lock);
		if (present)
			ctrl_info(ctrl, "Slot(%s): Card present\n",
				  slot_name(ctrl));
		if (link_active)
			ctrl_info(ctrl, "Slot(%s): Link Up\n",
				  slot_name(ctrl));
		ctrl->request_result = pciehp_enable_slot(ctrl, keep_power);
		break;
	default:
		mutex_unlock(&ctrl->state_lock);
		break;
	}
}

static int __pciehp_enable_slot(struct controller *ctrl)
{
	u8 getstatus = 0;

	if (MRL_SENS(ctrl)) {
		pciehp_get_latch_status(ctrl, &getstatus);
		if (getstatus) {
			ctrl_info(ctrl, "Slot(%s): Latch open\n",
				  slot_name(ctrl));
			return -ENODEV;
		}
	}

	if (POWER_CTRL(ctrl)) {
		pciehp_get_power_status(ctrl, &getstatus);
		if (getstatus) {
			struct pci_dev *port = ctrl->pcie->port;

			/* IX8024 can report presence and slot power before its
			 * downstream link finishes training.  Treating that as
			 * ON_STATE without enumeration turns the subsequent initial
			 * DLLSC event into surprise removal and powers off the card.
			 * Validate the live link and enumerate through the normal
			 * hotplug path before accepting this pre-powered slot.
			 */
			if (port->vendor == 0x1ec8 && port->device == 0x8024) {
				int ret = pciehp_check_link_status(ctrl);

				if (ret)
					return ret;
				if (ctrl->power_fault_detected ||
				    pciehp_query_power_fault(ctrl)) {
					ctrl_err(ctrl, "Slot(%s): Power fault\n",
						 slot_name(ctrl));
					set_slot_off(ctrl);
					return -EIO;
				}
				ret = pciehp_configure_device(ctrl);
				if (ret && ret != -EEXIST)
					return ret;
				pciehp_set_indicators(ctrl, PCI_EXP_SLTCTL_PWR_IND_ON,
						      PCI_EXP_SLTCTL_ATTN_IND_OFF);
			}
			ctrl_info(ctrl, "Slot(%s): Already enabled\n",
				  slot_name(ctrl));
			return 0;
		}
	}

	return board_added(ctrl);
}

static int pciehp_enable_slot(struct controller *ctrl, bool kept_power)
{
	int ret, pm_rc;

	pm_rc = pm_runtime_get_sync(&ctrl->pcie->port->dev);
	pciehp_ix_snapshot(ctrl, "enable-after-pm-get", pm_rc, true);
	/* Do not re-enumerate after a removal, fault or queued disable request. */
	if (kept_power && !pciehp_m50_slot_occupied(ctrl))
		ret = -ENODEV;
	else
		ret = __pciehp_enable_slot(ctrl);
	/* Retention is one bounded recovery attempt, never an OFF/powered slot. */
	if (ret && kept_power)
		set_slot_off(ctrl);
	pciehp_ix_snapshot(ctrl, "enable-result", pm_rc, true);
	if (ret && ATTN_BUTTN(ctrl))
		/* may be blinking */
		pciehp_set_indicators(ctrl, PCI_EXP_SLTCTL_PWR_IND_OFF,
				      INDICATOR_NOOP);
	pm_runtime_put(&ctrl->pcie->port->dev);

	mutex_lock(&ctrl->state_lock);
	ctrl->state = ret ? OFF_STATE : ON_STATE;
	mutex_unlock(&ctrl->state_lock);

	return ret;
}

static int __pciehp_disable_slot(struct controller *ctrl, bool safe_removal,
				 bool keep_power)
{
	u8 getstatus = 0;

	if (POWER_CTRL(ctrl)) {
		pciehp_get_power_status(ctrl, &getstatus);
		if (!getstatus) {
			ctrl_info(ctrl, "Slot(%s): Already disabled\n",
				  slot_name(ctrl));
			return -EINVAL;
		}
	}

	remove_board(ctrl, safe_removal, keep_power);
	return 0;
}

static int pciehp_disable_slot(struct controller *ctrl, bool safe_removal,
			       bool keep_power)
{
	int ret;

	pm_runtime_get_sync(&ctrl->pcie->port->dev);
	ret = __pciehp_disable_slot(ctrl, safe_removal, keep_power);
	pm_runtime_put(&ctrl->pcie->port->dev);

	mutex_lock(&ctrl->state_lock);
	ctrl->state = OFF_STATE;
	mutex_unlock(&ctrl->state_lock);

	return ret;
}

int pciehp_sysfs_enable_slot(struct hotplug_slot *hotplug_slot)
{
	struct controller *ctrl = to_ctrl(hotplug_slot);

	mutex_lock(&ctrl->state_lock);
	switch (ctrl->state) {
	case BLINKINGON_STATE:
	case OFF_STATE:
		mutex_unlock(&ctrl->state_lock);
		/*
		 * The IRQ thread becomes a no-op if the user pulls out the
		 * card before the thread wakes up, so initialize to -ENODEV.
		 */
		ctrl->request_result = -ENODEV;
		pciehp_request(ctrl, PCI_EXP_SLTSTA_PDC);
		wait_event(ctrl->requester,
			   !atomic_read(&ctrl->pending_events) &&
			   !ctrl->ist_running);
		return ctrl->request_result;
	case POWERON_STATE:
		ctrl_info(ctrl, "Slot(%s): Already in powering on state\n",
			  slot_name(ctrl));
		break;
	case BLINKINGOFF_STATE:
	case ON_STATE:
	case POWEROFF_STATE:
		ctrl_info(ctrl, "Slot(%s): Already enabled\n",
			  slot_name(ctrl));
		break;
	default:
		ctrl_err(ctrl, "Slot(%s): Invalid state %#x\n",
			 slot_name(ctrl), ctrl->state);
		break;
	}
	mutex_unlock(&ctrl->state_lock);

	return -ENODEV;
}

int pciehp_sysfs_disable_slot(struct hotplug_slot *hotplug_slot)
{
	struct controller *ctrl = to_ctrl(hotplug_slot);

	mutex_lock(&ctrl->state_lock);
	switch (ctrl->state) {
	case BLINKINGOFF_STATE:
	case ON_STATE:
		mutex_unlock(&ctrl->state_lock);
		pciehp_request(ctrl, DISABLE_SLOT);
		wait_event(ctrl->requester,
			   !atomic_read(&ctrl->pending_events) &&
			   !ctrl->ist_running);
		return ctrl->request_result;
	case POWEROFF_STATE:
		ctrl_info(ctrl, "Slot(%s): Already in powering off state\n",
			  slot_name(ctrl));
		break;
	case BLINKINGON_STATE:
	case OFF_STATE:
	case POWERON_STATE:
		ctrl_info(ctrl, "Slot(%s): Already disabled\n",
			  slot_name(ctrl));
		break;
	default:
		ctrl_err(ctrl, "Slot(%s): Invalid state %#x\n",
			 slot_name(ctrl), ctrl->state);
		break;
	}
	mutex_unlock(&ctrl->state_lock);

	return -ENODEV;
}
