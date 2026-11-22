// SPDX-License-Identifier: GPL-2.0+
/*
 * PCI Express PCI Hot Plug Driver
 *
 * Copyright (C) 1995,2001 Compaq Computer Corporation
 * Copyright (C) 2001 Greg Kroah-Hartman (greg@kroah.com)
 * Copyright (C) 2001 IBM Corp.
 * Copyright (C) 2003-2004 Intel Corporation
 *
 * All rights reserved.
 *
 * Send feedback to <greg@kroah.com>,<kristen.c.accardi@intel.com>
 */

#define dev_fmt(fmt) "pciehp: " fmt

#include <linux/dmi.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/jiffies.h>
#include <linux/kthread.h>
#include <linux/pci.h>
#include <linux/of.h>
#include <linux/pm_runtime.h>
#include <linux/interrupt.h>
#include <linux/slab.h>

#include "../pci.h"
#include "pciehp.h"

static const struct dmi_system_id inband_presence_disabled_dmi_table[] = {
	/*
	 * Match all Dell systems, as some Dell systems have inband
	 * presence disabled on NVMe slots (but don't support the bit to
	 * report it). Setting inband presence disabled should have no
	 * negative effect, except on broken hotplug slots that never
	 * assert presence detect--and those will still work, they will
	 * just have a bit of extra delay before being probed.
	 */
	{
		.ident = "Dell System",
		.matches = {
			DMI_MATCH(DMI_OEM_STRING, "Dell System"),
		},
	},
	{}
};

static inline struct pci_dev *ctrl_dev(struct controller *ctrl)
{
	return ctrl->pcie->port;
}

static irqreturn_t pciehp_isr(int irq, void *dev_id);
static irqreturn_t pciehp_ist(int irq, void *dev_id);
static int pciehp_poll(void *data);

static inline int pciehp_request_irq(struct controller *ctrl)
{
	int retval, irq = ctrl->pcie->irq;

	if (pciehp_poll_mode) {
		ctrl->poll_thread = kthread_run(&pciehp_poll, ctrl,
						"pciehp_poll-%s",
						slot_name(ctrl));
		return PTR_ERR_OR_ZERO(ctrl->poll_thread);
	}

	/* Installs the interrupt handler */
	retval = request_threaded_irq(irq, pciehp_isr, pciehp_ist,
				      IRQF_SHARED, "pciehp", ctrl);
	if (retval)
		ctrl_err(ctrl, "Cannot get irq %d for the hotplug controller\n",
			 irq);
	return retval;
}

static inline void pciehp_free_irq(struct controller *ctrl)
{
	if (pciehp_poll_mode)
		kthread_stop(ctrl->poll_thread);
	else
		free_irq(ctrl->pcie->irq, ctrl);
}

static int pcie_poll_cmd(struct controller *ctrl, int timeout)
{
	struct pci_dev *pdev = ctrl_dev(ctrl);
	u16 slot_status;

	do {
		pcie_capability_read_word(pdev, PCI_EXP_SLTSTA, &slot_status);
		if (PCI_POSSIBLE_ERROR(slot_status)) {
			ctrl_info(ctrl, "%s: no response from device\n",
				  __func__);
			return 0;
		}

		if (slot_status & PCI_EXP_SLTSTA_CC) {
			pcie_capability_write_word(pdev, PCI_EXP_SLTSTA,
						   PCI_EXP_SLTSTA_CC);
			ctrl->cmd_busy = 0;
			smp_mb();
			return 1;
		}
		msleep(10);
		timeout -= 10;
	} while (timeout >= 0);
	return 0;	/* timeout */
}

static bool ix8024_downstream(struct controller *ctrl)
{
	struct pci_dev *pdev = ctrl_dev(ctrl);

	return pdev->vendor == 0x1ec8 && pdev->device == 0x8024 &&
	       pci_pcie_type(pdev) == PCI_EXP_TYPE_DOWNSTREAM;
}


/* Fixed, read-only mirror evidence.  Never called from the IRQ path.
 * Internal config ranges are from IX8024 User Manual table42, and the
 * internal-to-BAR0 translation was verified on this Y4 (40 anchors).
 * Compare config-before/BAR/config-after; changing fields are not failures.
 */
static int ix_config_words(struct pci_dev *pdev, const u16 *offset,
			   unsigned int count, u32 *value)
{
	unsigned int i;
	int ret;

	for (i = 0; i < count; i++) {
		ret = pci_read_config_dword(pdev, offset[i], &value[i]);
		if (ret)
			return ret;
	}
	return 0;
}

void pciehp_ix_snapshot(struct controller *ctrl, const char *phase, int pm_rc, bool pm_rc_known)
{
	struct pci_dev *pdev = ctrl_dev(ctrl), *usp;
	void __iomem *mapping;
	u32 before[8] = {0}, mirror[8] = {0}, after[8] = {0};
	u16 c, offset[8];
	u32 cfg_offset, port, stable = 0, equal = 0;
	unsigned int count = 7, i;
	u64 start;
	int a, b;

	if (!ix8024_downstream(ctrl))
		return;
	if (pm_rc_known && pm_rc < 0) {
		ctrl_dbg(ctrl, "IX MIRROR %s resume-failed pm_get_known=%u pmrc=%d\n",
			  phase, pm_rc_known, pm_rc);
		return;
	}
	if (WARN_ON_ONCE(in_interrupt()))
		return;
	usp = pdev->bus->self;
	c = pci_pcie_cap(pdev);
	if (!usp || usp->vendor != 0x1ec8 || usp->device != 0x8024 ||
	    pci_pcie_type(usp) != PCI_EXP_TYPE_UPSTREAM || c < 0x40 ||
	    c > 0xe4 || (c & 3) ||
	    !(pci_resource_flags(usp, 0) & IORESOURCE_MEM) ||
	    pci_resource_len(usp, 0) != 0x200000) {
		ctrl_dbg(ctrl, "IX MIRROR %s rejected identity/resource pm_get_known=%u pmrc=%d\n",
			  phase, pm_rc_known, pm_rc);
		return;
	}
	offset[0] = PCI_VENDOR_ID;
	offset[1] = PCI_CLASS_REVISION;
	offset[2] = PCI_PRIMARY_BUS;
	offset[3] = c + PCI_EXP_LNKCAP;
	offset[4] = c + PCI_EXP_LNKCTL;
	offset[5] = c + PCI_EXP_SLTCAP;
	offset[6] = c + PCI_EXP_SLTCTL;
	if (pdev->pm_cap >= 0x40 && pdev->pm_cap <= 0xf8 &&
	    !(pdev->pm_cap & 3)) {
		offset[7] = pdev->pm_cap + PCI_PM_CTRL;
		count++;
	}
	start = ktime_get_ns();
	a = ix_config_words(pdev, offset, count, before);
	if (a || before[0] != 0x80241ec8 ||
	    (before[1] >> 16) != PCI_CLASS_BRIDGE_PCI) {
		ctrl_dbg(ctrl, "IX MIRROR %s config-rejected rc=%d pm_get_known=%u pmrc=%d\n",
			  phase, a, pm_rc_known, pm_rc);
		return;
	}
	port = before[3] >> 24;
	switch (port) {
	case 0:
		cfg_offset = 0x20000;
		break;
	case 3:
		cfg_offset = 0x23000;
		break;
	case 6:
		cfg_offset = 0x38000;
		break;
	case 9:
		cfg_offset = 0x3b000;
		break;
	default:
		ctrl_dbg(ctrl, "IX MIRROR %s rejected port=%u\n", phase, port);
		return;
	}
	mapping = pci_iomap_range(usp, 0, cfg_offset, 0x1000);
	if (!mapping) {
		ctrl_dbg(ctrl, "IX MIRROR %s map-failed port=%u\n", phase, port);
		return;
	}
	for (i = 0; i < count; i++)
		mirror[i] = readl(mapping + offset[i]);
	pci_iounmap(usp, mapping);
	b = ix_config_words(pdev, offset, count, after);
	if (!b) {
		for (i = 0; i < count; i++) {
			if (before[i] == after[i]) {
				stable |= BIT(i);
				if (before[i] == mirror[i])
					equal |= BIT(i);
			}
		}
	}
	ctrl_dbg(ctrl,
		  "IX MIRROR %s port=%u pm_get_known=%u pmrc=%d active=%u runtime=%d usage=%d ns=%llu readerr=%d/%d stable=%#x equal=%#x\n",
		  phase, port, pm_rc_known, pm_rc, pm_runtime_active(&pdev->dev),
		  READ_ONCE(pdev->dev.power.runtime_status),
		  atomic_read(&pdev->dev.power.usage_count),
		  (unsigned long long)(ktime_get_ns() - start), a, b,
		  stable, equal);
	/* Values are evidence, never used to change or accept slot state. */
	for (i = 0; i < count; i++)
		ctrl_dbg(ctrl,
			  "IX MIRROR %s reg=%#x before=%#010x bar=%#010x after=%#010x\n",
			  phase, offset[i], before[i], mirror[i], after[i]);
}

/* Observes standard PCIe registers only; never clears event bits. */
static void ix8024_cmd_diagnostic(struct controller *ctrl, const char *phase)
{
	struct pci_dev *pdev = ctrl_dev(ctrl);
	u16 command = 0xffff, status = 0xffff, link = 0xffff, link_ctl = 0xffff;
	u32 cap = 0xffffffff;
	int a, b, c, d, e;

	if (!ix8024_downstream(ctrl))
		return;
	a = pcie_capability_read_word(pdev, PCI_EXP_SLTCTL, &command);
	b = pcie_capability_read_word(pdev, PCI_EXP_SLTSTA, &status);
	c = pcie_capability_read_word(pdev, PCI_EXP_LNKSTA, &link);
	d = pcie_capability_read_dword(pdev, PCI_EXP_SLTCAP, &cap);
	e = pcie_capability_read_word(pdev, PCI_EXP_LNKCTL, &link_ctl);
	ctrl_dbg(ctrl,
		  "IX CC %s: ctl=%#06x sta=%#06x link=%#06x linkctl=%#06x cap=%#010x busy=%u fault=%u irq=%d msi=%u age=%u readerr=%d/%d/%d/%d/%d\n",
		  phase, command, status, link, link_ctl, cap,
		  !!ctrl->cmd_busy,
		  !!READ_ONCE(ctrl->power_fault_detected),
		  ctrl->pcie->irq, !!pci_dev_msi_enabled(pdev),
		  jiffies_to_msecs(jiffies - ctrl->cmd_started), a,b,c,d,e);
}

static void pcie_wait_cmd(struct controller *ctrl)
{
	unsigned int msecs = pciehp_poll_mode ? 2500 : 1000;
	unsigned long duration = msecs_to_jiffies(msecs);
	unsigned long cmd_timeout = ctrl->cmd_started + duration;
	unsigned long now, timeout;
	int rc;

	/* The IX8024 downstream ports never signal command completion: the
	 * completion bit stays clear and the busy flag set at boot time is
	 * still set minutes later, even though the power commands do take
	 * effect. Waiting for a completion that never comes only burns
	 * seconds on every write, so treat those ports like the
	 * NO_CMD_CMPL case below and proceed at once. */
	if (ix8024_downstream(ctrl))
		return;

	/*
	 * If the controller does not generate notifications for command
	 * completions, we never need to wait between writes.
	 */
	if (NO_CMD_CMPL(ctrl))
		return;

	if (!ctrl->cmd_busy)
		return;

	/*
	 * Even if the command has already timed out, we want to call
	 * pcie_poll_cmd() so it can clear PCI_EXP_SLTSTA_CC.
	 */
	now = jiffies;
	if (time_before_eq(cmd_timeout, now))
		timeout = 1;
	else
		timeout = cmd_timeout - now;

	if (ctrl->slot_ctrl & PCI_EXP_SLTCTL_HPIE &&
	    ctrl->slot_ctrl & PCI_EXP_SLTCTL_CCIE)
		rc = wait_event_timeout(ctrl->queue, !ctrl->cmd_busy, timeout);
	else
		rc = pcie_poll_cmd(ctrl, jiffies_to_msecs(timeout));

	if (!rc && ix8024_downstream(ctrl)) {
		ix8024_cmd_diagnostic(ctrl, "timeout");
		/* A lost CC interrupt may leave a real sticky CC bit set.
		 * Consume only that existing CC via the standard helper.
		 * No NCCS override, no fabricated completion, no PCC retry.
		 */
		rc = pcie_poll_cmd(ctrl, 0);
		if (rc)
			ix8024_cmd_diagnostic(ctrl, "timeout-cc-polled");
	}
	if (!rc)
		ctrl_info(ctrl, "Timeout on hotplug command %#06x (issued %u msec ago)\n",
			  ctrl->slot_ctrl,
			  jiffies_to_msecs(jiffies - ctrl->cmd_started));
}

#define CC_ERRATUM_MASK		(PCI_EXP_SLTCTL_PCC |	\
				 PCI_EXP_SLTCTL_PIC |	\
				 PCI_EXP_SLTCTL_AIC |	\
				 PCI_EXP_SLTCTL_EIC)

static void pcie_do_write_cmd(struct controller *ctrl, u16 cmd,
			      u16 mask, bool wait)
{
	struct pci_dev *pdev = ctrl_dev(ctrl);
	u16 slot_ctrl_orig, slot_ctrl;

	mutex_lock(&ctrl->ctrl_lock);

	/*
	 * Always wait for any previous command that might still be in progress
	 */
	pcie_wait_cmd(ctrl);

	pcie_capability_read_word(pdev, PCI_EXP_SLTCTL, &slot_ctrl);
	if (PCI_POSSIBLE_ERROR(slot_ctrl)) {
		ctrl_info(ctrl, "%s: no response from device\n", __func__);
		goto out;
	}

	slot_ctrl_orig = slot_ctrl;
	ix8024_cmd_diagnostic(ctrl, "before-write");
	slot_ctrl &= ~mask;
	slot_ctrl |= (cmd & mask);

	if (ix8024_downstream(ctrl))
		ctrl_dbg(ctrl,
			  "IX WRITE mask=%#x req=%#x old=%#x new=%#x delta=%#x kind=%s\n",
			  mask, cmd, slot_ctrl_orig, slot_ctrl,
			  slot_ctrl_orig ^ slot_ctrl,
			  ((slot_ctrl_orig ^ slot_ctrl) & PCI_EXP_SLTCTL_PCC) ?
			  "PCC" : ((slot_ctrl_orig ^ slot_ctrl) &
			  (PCI_EXP_SLTCTL_PIC | PCI_EXP_SLTCTL_AIC)) ?
			  "INDICATOR" : slot_ctrl_orig == slot_ctrl ?
			  "UNCHANGED" : "OTHER");
	ctrl->cmd_busy = 1;
	smp_mb();
	ctrl->slot_ctrl = slot_ctrl;
	pcie_capability_write_word(pdev, PCI_EXP_SLTCTL, slot_ctrl);
	ctrl->cmd_started = jiffies;
	ix8024_cmd_diagnostic(ctrl, "after-write");

	/*
	 * Controllers with the Intel CF118 and similar errata advertise
	 * Command Completed support, but they only set Command Completed
	 * if we change the "Control" bits for power, power indicator,
	 * attention indicator, or interlock.  If we only change the
	 * "Enable" bits, they never set the Command Completed bit.
	 */
	if (pdev->broken_cmd_compl &&
	    (slot_ctrl_orig & CC_ERRATUM_MASK) == (slot_ctrl & CC_ERRATUM_MASK))
		ctrl->cmd_busy = 0;

	/*
	 * Optionally wait for the hardware to be ready for a new command,
	 * indicating completion of the above issued command.
	 */
	if (wait)
		pcie_wait_cmd(ctrl);

out:
	mutex_unlock(&ctrl->ctrl_lock);
}

/**
 * pcie_write_cmd - Issue controller command
 * @ctrl: controller to which the command is issued
 * @cmd:  command value written to slot control register
 * @mask: bitmask of slot control register to be modified
 */
static void pcie_write_cmd(struct controller *ctrl, u16 cmd, u16 mask)
{
	pcie_do_write_cmd(ctrl, cmd, mask, true);
}

/* Same as above without waiting for the hardware to latch */
static void pcie_write_cmd_nowait(struct controller *ctrl, u16 cmd, u16 mask)
{
	pcie_do_write_cmd(ctrl, cmd, mask, false);
}

/**
 * pciehp_check_link_active() - Is the link active
 * @ctrl: PCIe hotplug controller
 *
 * Check whether the downstream link is currently active. Note it is
 * possible that the card is removed immediately after this so the
 * caller may need to take it into account.
 *
 * If the hotplug controller itself is not available anymore returns
 * %-ENODEV.
 */
int pciehp_check_link_active(struct controller *ctrl)
{
	struct pci_dev *pdev = ctrl_dev(ctrl);
	u16 lnk_status;
	int ret;

	ret = pcie_capability_read_word(pdev, PCI_EXP_LNKSTA, &lnk_status);
	if (ret == PCIBIOS_DEVICE_NOT_FOUND || PCI_POSSIBLE_ERROR(lnk_status))
		return -ENODEV;

	ret = !!(lnk_status & PCI_EXP_LNKSTA_DLLLA);
	ctrl_dbg(ctrl, "%s: lnk_status = %x\n", __func__, lnk_status);

	return ret;
}

static bool pci_bus_check_dev(struct pci_bus *bus, int devfn)
{
	u32 l;
	int count = 0;
	int delay = 1000, step = 20;
	bool found = false;

	do {
		found = pci_bus_read_dev_vendor_id(bus, devfn, &l, 0);
		count++;

		if (found)
			break;

		msleep(step);
		delay -= step;
	} while (delay > 0);

	if (count > 1)
		pr_debug("pci %04x:%02x:%02x.%d id reading try %d times with interval %d ms to get %08x\n",
			pci_domain_nr(bus), bus->number, PCI_SLOT(devfn),
			PCI_FUNC(devfn), count, step, l);

	return found;
}

static void pcie_wait_for_presence(struct pci_dev *pdev)
{
	int timeout = 1250;
	u16 slot_status;

	do {
		pcie_capability_read_word(pdev, PCI_EXP_SLTSTA, &slot_status);
		if (slot_status & PCI_EXP_SLTSTA_PDS)
			return;
		msleep(10);
		timeout -= 10;
	} while (timeout > 0);
}

/*
 * Y4 diagnostic experiment: one bounded read-only wait for IX8024 DSPs.
 * Does not issue PCC, link retrain, reset, or any other register write.
 */
static int ix8024_wait_link_ready(struct controller *ctrl)
{
	struct pci_dev *pdev = ctrl_dev(ctrl);
	unsigned long start = jiffies;
	unsigned long deadline = start + msecs_to_jiffies(5000);
	u16 link = 0, slot = 0;
	int ret;

	for (;;) {
		ret = pcie_capability_read_word(pdev, PCI_EXP_SLTSTA, &slot);
		if (ret || PCI_POSSIBLE_ERROR(slot))
			return -ENODEV;
		if (READ_ONCE(ctrl->power_fault_detected) ||
		    (slot & PCI_EXP_SLTSTA_PFD)) {
			ctrl_info(ctrl, "IX8024 link wait: power fault, slot=%#06x\n",
				  slot);
			return -EIO;
		}
		if (!(slot & PCI_EXP_SLTSTA_PDS))
			return -ENODEV;
		ret = pcie_capability_read_word(pdev, PCI_EXP_LNKSTA, &link);
		if (ret || PCI_POSSIBLE_ERROR(link))
			return -ENODEV;
		if ((link & PCI_EXP_LNKSTA_DLLLA) &&
		    !(link & PCI_EXP_LNKSTA_LT) &&
		    (link & PCI_EXP_LNKSTA_NLW)) {
			ctrl_info(ctrl,
				  "IX8024 link ready after %u ms: link=%#06x slot=%#06x\n",
				  jiffies_to_msecs(jiffies - start), link, slot);
			/* Preserve the standard delay before configuration access. */
			msleep(100);
			return 0;
		}
		if (time_after_eq(jiffies, deadline)) {
			ctrl_info(ctrl,
				  "IX8024 link wait 5000 ms expired: link=%#06x slot=%#06x\n",
				  link, slot);
			return -ETIMEDOUT;
		}
		msleep(20);
	}
}

int pciehp_check_link_status(struct controller *ctrl)
{
	struct pci_dev *pdev = ctrl_dev(ctrl);
	bool found;
	u16 lnk_status;

	if (pdev->vendor == 0x1ec8 && pdev->device == 0x8024 &&
	    pci_pcie_type(pdev) == PCI_EXP_TYPE_DOWNSTREAM) {
		int ret = ix8024_wait_link_ready(ctrl);

		if (ret)
			return ret;
	} else if (!pcie_wait_for_link(pdev, true)) {
		ctrl_info(ctrl, "Slot(%s): No link\n", slot_name(ctrl));
		return -1;
	}

	if (ctrl->inband_presence_disabled)
		pcie_wait_for_presence(pdev);

	found = pci_bus_check_dev(ctrl->pcie->port->subordinate,
					PCI_DEVFN(0, 0));

	/* ignore link or presence changes up to this point */
	if (found)
		atomic_and(~(PCI_EXP_SLTSTA_DLLSC | PCI_EXP_SLTSTA_PDC),
			   &ctrl->pending_events);

	pcie_capability_read_word(pdev, PCI_EXP_LNKSTA, &lnk_status);
	ctrl_dbg(ctrl, "%s: lnk_status = %x\n", __func__, lnk_status);
	if ((lnk_status & PCI_EXP_LNKSTA_LT) ||
	    !(lnk_status & PCI_EXP_LNKSTA_NLW)) {
		ctrl_info(ctrl, "Slot(%s): Cannot train link: status %#06x\n",
			  slot_name(ctrl), lnk_status);
		return -1;
	}

	pcie_update_link_speed(ctrl->pcie->port->subordinate, lnk_status);

	if (!found) {
		ctrl_info(ctrl, "Slot(%s): No device found\n",
			  slot_name(ctrl));
		return -1;
	}

	return 0;
}

static int __pciehp_link_set(struct controller *ctrl, bool enable)
{
	struct pci_dev *pdev = ctrl_dev(ctrl);

	pcie_capability_clear_and_set_word(pdev, PCI_EXP_LNKCTL,
					   PCI_EXP_LNKCTL_LD,
					   enable ? 0 : PCI_EXP_LNKCTL_LD);

	return 0;
}

static int pciehp_link_enable(struct controller *ctrl)
{
	return __pciehp_link_set(ctrl, true);
}

int pciehp_get_raw_indicator_status(struct hotplug_slot *hotplug_slot,
				    u8 *status)
{
	struct controller *ctrl = to_ctrl(hotplug_slot);
	struct pci_dev *pdev = ctrl_dev(ctrl);
	u16 slot_ctrl;

	pci_config_pm_runtime_get(pdev);
	pcie_capability_read_word(pdev, PCI_EXP_SLTCTL, &slot_ctrl);
	pci_config_pm_runtime_put(pdev);
	*status = (slot_ctrl & (PCI_EXP_SLTCTL_AIC | PCI_EXP_SLTCTL_PIC)) >> 6;
	return 0;
}

int pciehp_get_attention_status(struct hotplug_slot *hotplug_slot, u8 *status)
{
	struct controller *ctrl = to_ctrl(hotplug_slot);
	struct pci_dev *pdev = ctrl_dev(ctrl);
	u16 slot_ctrl;

	pci_config_pm_runtime_get(pdev);
	pcie_capability_read_word(pdev, PCI_EXP_SLTCTL, &slot_ctrl);
	pci_config_pm_runtime_put(pdev);
	ctrl_dbg(ctrl, "%s: SLOTCTRL %x, value read %x\n", __func__,
		 pci_pcie_cap(ctrl->pcie->port) + PCI_EXP_SLTCTL, slot_ctrl);

	switch (slot_ctrl & PCI_EXP_SLTCTL_AIC) {
	case PCI_EXP_SLTCTL_ATTN_IND_ON:
		*status = 1;	/* On */
		break;
	case PCI_EXP_SLTCTL_ATTN_IND_BLINK:
		*status = 2;	/* Blink */
		break;
	case PCI_EXP_SLTCTL_ATTN_IND_OFF:
		*status = 0;	/* Off */
		break;
	default:
		*status = 0xFF;
		break;
	}

	return 0;
}

void pciehp_get_power_status(struct controller *ctrl, u8 *status)
{
	struct pci_dev *pdev = ctrl_dev(ctrl);
	u16 slot_ctrl;

	pcie_capability_read_word(pdev, PCI_EXP_SLTCTL, &slot_ctrl);
	ctrl_dbg(ctrl, "%s: SLOTCTRL %x value read %x\n", __func__,
		 pci_pcie_cap(ctrl->pcie->port) + PCI_EXP_SLTCTL, slot_ctrl);

	switch (slot_ctrl & PCI_EXP_SLTCTL_PCC) {
	case PCI_EXP_SLTCTL_PWR_ON:
		*status = 1;	/* On */
		break;
	case PCI_EXP_SLTCTL_PWR_OFF:
		*status = 0;	/* Off */
		break;
	default:
		*status = 0xFF;
		break;
	}
}

void pciehp_get_latch_status(struct controller *ctrl, u8 *status)
{
	struct pci_dev *pdev = ctrl_dev(ctrl);
	u16 slot_status;

	pcie_capability_read_word(pdev, PCI_EXP_SLTSTA, &slot_status);
	*status = !!(slot_status & PCI_EXP_SLTSTA_MRLSS);
}

/**
 * pciehp_card_present() - Is the card present
 * @ctrl: PCIe hotplug controller
 *
 * Function checks whether the card is currently present in the slot and
 * in that case returns true. Note it is possible that the card is
 * removed immediately after the check so the caller may need to take
 * this into account.
 *
 * It the hotplug controller itself is not available anymore returns
 * %-ENODEV.
 */
int pciehp_card_present(struct controller *ctrl)
{
	struct pci_dev *pdev = ctrl_dev(ctrl);
	u16 slot_status;
	int ret;

	ret = pcie_capability_read_word(pdev, PCI_EXP_SLTSTA, &slot_status);
	if (ret == PCIBIOS_DEVICE_NOT_FOUND || PCI_POSSIBLE_ERROR(slot_status))
		return -ENODEV;

	return !!(slot_status & PCI_EXP_SLTSTA_PDS);
}

/**
 * pciehp_card_present_or_link_active() - whether given slot is occupied
 * @ctrl: PCIe hotplug controller
 *
 * Unlike pciehp_card_present(), which determines presence solely from the
 * Presence Detect State bit, this helper also returns true if the Link Active
 * bit is set.  This is a concession to broken hotplug ports which hardwire
 * Presence Detect State to zero, such as Wilocity's [1ae9:0200].
 *
 * Returns: %1 if the slot is occupied and %0 if it is not. If the hotplug
 *	    port is not present anymore returns %-ENODEV.
 */
int pciehp_card_present_or_link_active(struct controller *ctrl)
{
	int ret;

	ret = pciehp_card_present(ctrl);
	if (ret)
		return ret;

	return pciehp_check_link_active(ctrl);
}

int pciehp_query_power_fault(struct controller *ctrl)
{
	struct pci_dev *pdev = ctrl_dev(ctrl);
	u16 slot_status;

	pcie_capability_read_word(pdev, PCI_EXP_SLTSTA, &slot_status);
	return !!(slot_status & PCI_EXP_SLTSTA_PFD);
}

int pciehp_set_raw_indicator_status(struct hotplug_slot *hotplug_slot,
				    u8 status)
{
	struct controller *ctrl = to_ctrl(hotplug_slot);
	struct pci_dev *pdev = ctrl_dev(ctrl);

	pci_config_pm_runtime_get(pdev);
	pcie_write_cmd_nowait(ctrl, status << 6,
			      PCI_EXP_SLTCTL_AIC | PCI_EXP_SLTCTL_PIC);
	pci_config_pm_runtime_put(pdev);
	return 0;
}

/**
 * pciehp_set_indicators() - set attention indicator, power indicator, or both
 * @ctrl: PCIe hotplug controller
 * @pwr: one of:
 *	PCI_EXP_SLTCTL_PWR_IND_ON
 *	PCI_EXP_SLTCTL_PWR_IND_BLINK
 *	PCI_EXP_SLTCTL_PWR_IND_OFF
 * @attn: one of:
 *	PCI_EXP_SLTCTL_ATTN_IND_ON
 *	PCI_EXP_SLTCTL_ATTN_IND_BLINK
 *	PCI_EXP_SLTCTL_ATTN_IND_OFF
 *
 * Either @pwr or @attn can also be INDICATOR_NOOP to leave that indicator
 * unchanged.
 */
void pciehp_set_indicators(struct controller *ctrl, int pwr, int attn)
{
	u16 cmd = 0, mask = 0;

	if (PWR_LED(ctrl) && pwr != INDICATOR_NOOP) {
		cmd |= (pwr & PCI_EXP_SLTCTL_PIC);
		mask |= PCI_EXP_SLTCTL_PIC;
	}

	if (ATTN_LED(ctrl) && attn != INDICATOR_NOOP) {
		cmd |= (attn & PCI_EXP_SLTCTL_AIC);
		mask |= PCI_EXP_SLTCTL_AIC;
	}

	if (cmd) {
		pcie_write_cmd_nowait(ctrl, cmd, mask);
		ctrl_dbg(ctrl, "%s: SLOTCTRL %x write cmd %x\n", __func__,
			 pci_pcie_cap(ctrl->pcie->port) + PCI_EXP_SLTCTL, cmd);
	}
}

int pciehp_power_on_slot(struct controller *ctrl)
{
	struct pci_dev *pdev = ctrl_dev(ctrl);
	u16 slot_status;
	int retval;

	ix8024_cmd_diagnostic(ctrl, "power-on-entry");
	pciehp_ix_snapshot(ctrl, "power-on-entry", 0, false);
	/* Clear power-fault bit from previous power failures */
	pcie_capability_read_word(pdev, PCI_EXP_SLTSTA, &slot_status);
	if (slot_status & PCI_EXP_SLTSTA_PFD)
		pcie_capability_write_word(pdev, PCI_EXP_SLTSTA,
					   PCI_EXP_SLTSTA_PFD);
	ctrl->power_fault_detected = 0;

	pcie_write_cmd(ctrl, PCI_EXP_SLTCTL_PWR_ON, PCI_EXP_SLTCTL_PCC);
	ix8024_cmd_diagnostic(ctrl, "power-on-after-pcc");
	pciehp_ix_snapshot(ctrl, "power-on-after-pcc", 0, false);
	ctrl_dbg(ctrl, "%s: SLOTCTRL %x write cmd %x\n", __func__,
		 pci_pcie_cap(ctrl->pcie->port) + PCI_EXP_SLTCTL,
		 PCI_EXP_SLTCTL_PWR_ON);

	retval = pciehp_link_enable(ctrl);
	ix8024_cmd_diagnostic(ctrl, "power-on-after-link-enable");
	if (retval)
		ctrl_err(ctrl, "%s: Can not enable the link!\n", __func__);

	return retval;
}

void pciehp_power_off_slot(struct controller *ctrl)
{
	ix8024_cmd_diagnostic(ctrl, "power-off-entry");
	pciehp_ix_snapshot(ctrl, "power-off-entry", 0, false);
	pcie_write_cmd(ctrl, PCI_EXP_SLTCTL_PWR_OFF, PCI_EXP_SLTCTL_PCC);
	ix8024_cmd_diagnostic(ctrl, "power-off-after-pcc");
	pciehp_ix_snapshot(ctrl, "power-off-after-pcc", 0, false);
	ctrl_dbg(ctrl, "%s: SLOTCTRL %x write cmd %x\n", __func__,
		 pci_pcie_cap(ctrl->pcie->port) + PCI_EXP_SLTCTL,
		 PCI_EXP_SLTCTL_PWR_OFF);
}

static void pciehp_ignore_dpc_link_change(struct controller *ctrl,
					  struct pci_dev *pdev, int irq)
{
	/*
	 * Ignore link changes which occurred while waiting for DPC recovery.
	 * Could be several if DPC triggered multiple times consecutively.
	 */
	synchronize_hardirq(irq);
	atomic_and(~PCI_EXP_SLTSTA_DLLSC, &ctrl->pending_events);
	if (pciehp_poll_mode)
		pcie_capability_write_word(pdev, PCI_EXP_SLTSTA,
					   PCI_EXP_SLTSTA_DLLSC);
	ctrl_info(ctrl, "Slot(%s): Link Down/Up ignored (recovered by DPC)\n",
		  slot_name(ctrl));

	/*
	 * If the link is unexpectedly down after successful recovery,
	 * the corresponding link change may have been ignored above.
	 * Synthesize it to ensure that it is acted on.
	 */
	down_read_nested(&ctrl->reset_lock, ctrl->depth);
	if (!pciehp_check_link_active(ctrl))
		pciehp_request(ctrl, PCI_EXP_SLTSTA_DLLSC);
	up_read(&ctrl->reset_lock);
}

static irqreturn_t pciehp_isr(int irq, void *dev_id)
{
	struct controller *ctrl = (struct controller *)dev_id;
	struct pci_dev *pdev = ctrl_dev(ctrl);
	struct device *parent = pdev->dev.parent;
	u16 status, events = 0;

	/*
	 * Interrupts only occur in D3hot or shallower and only if enabled
	 * in the Slot Control register (PCIe r4.0, sec 6.7.3.4).
	 */
	if (pdev->current_state == PCI_D3cold ||
	    (!(ctrl->slot_ctrl & PCI_EXP_SLTCTL_HPIE) && !pciehp_poll_mode))
		return IRQ_NONE;

	/*
	 * Keep the port accessible by holding a runtime PM ref on its parent.
	 * Defer resume of the parent to the IRQ thread if it's suspended.
	 * Mask the interrupt until then.
	 */
	if (parent) {
		pm_runtime_get_noresume(parent);
		if (!pm_runtime_active(parent)) {
			pm_runtime_put(parent);
			disable_irq_nosync(irq);
			atomic_or(RERUN_ISR, &ctrl->pending_events);
			return IRQ_WAKE_THREAD;
		}
	}

read_status:
	pcie_capability_read_word(pdev, PCI_EXP_SLTSTA, &status);
	if (PCI_POSSIBLE_ERROR(status)) {
		ctrl_info(ctrl, "%s: no response from device\n", __func__);
		if (parent)
			pm_runtime_put(parent);
		return IRQ_NONE;
	}

	/*
	 * Slot Status contains plain status bits as well as event
	 * notification bits; right now we only want the event bits.
	 */
	status &= PCI_EXP_SLTSTA_ABP | PCI_EXP_SLTSTA_PFD |
		  PCI_EXP_SLTSTA_PDC | PCI_EXP_SLTSTA_CC |
		  PCI_EXP_SLTSTA_DLLSC;

	/*
	 * If we've already reported a power fault, don't report it again
	 * until we've done something to handle it.
	 */
	if (ctrl->power_fault_detected)
		status &= ~PCI_EXP_SLTSTA_PFD;
	else if (status & PCI_EXP_SLTSTA_PFD)
		ctrl->power_fault_detected = true;

	events |= status;
	if (!events) {
		if (parent)
			pm_runtime_put(parent);
		return IRQ_NONE;
	}

	if (status) {
		pcie_capability_write_word(pdev, PCI_EXP_SLTSTA, status);

		/*
		 * In MSI mode, all event bits must be zero before the port
		 * will send a new interrupt (PCIe Base Spec r5.0 sec 6.7.3.4).
		 * So re-read the Slot Status register in case a bit was set
		 * between read and write.
		 */
		if (pci_dev_msi_enabled(pdev) && !pciehp_poll_mode)
			goto read_status;
	}

	ctrl_dbg(ctrl, "pending interrupts %#06x from Slot Status\n", events);
	if (parent)
		pm_runtime_put(parent);

	/*
	 * Command Completed notifications are not deferred to the
	 * IRQ thread because it may be waiting for their arrival.
	 */
	if (events & PCI_EXP_SLTSTA_CC) {
		ix8024_cmd_diagnostic(ctrl, "irq-cc-ack");
		ctrl->cmd_busy = 0;
		smp_mb();
		wake_up(&ctrl->queue);

		if (events == PCI_EXP_SLTSTA_CC)
			return IRQ_HANDLED;

		events &= ~PCI_EXP_SLTSTA_CC;
	}

	if (pdev->ignore_hotplug) {
		ctrl_dbg(ctrl, "ignoring hotplug event %#06x\n", events);
		return IRQ_HANDLED;
	}

	/* Save pending events for consumption by IRQ thread. */
	atomic_or(events, &ctrl->pending_events);
	return IRQ_WAKE_THREAD;
}

static irqreturn_t pciehp_ist(int irq, void *dev_id)
{
	struct controller *ctrl = (struct controller *)dev_id;
	struct pci_dev *pdev = ctrl_dev(ctrl);
	irqreturn_t ret;
	u32 events;

	ctrl->ist_running = true;
	pci_config_pm_runtime_get(pdev);

	/* rerun pciehp_isr() if the port was inaccessible on interrupt */
	if (atomic_fetch_and(~RERUN_ISR, &ctrl->pending_events) & RERUN_ISR) {
		ret = pciehp_isr(irq, dev_id);
		enable_irq(irq);
		if (ret != IRQ_WAKE_THREAD)
			goto out;
	}

	synchronize_hardirq(irq);
	events = atomic_xchg(&ctrl->pending_events, 0);
	if (!events) {
		ret = IRQ_NONE;
		goto out;
	}

	/* Check Attention Button Pressed */
	if (events & PCI_EXP_SLTSTA_ABP)
		pciehp_handle_button_press(ctrl);

	/* Check Power Fault Detected */
	if (events & PCI_EXP_SLTSTA_PFD) {
		ctrl_err(ctrl, "Slot(%s): Power fault\n", slot_name(ctrl));
		pciehp_set_indicators(ctrl, PCI_EXP_SLTCTL_PWR_IND_OFF,
				      PCI_EXP_SLTCTL_ATTN_IND_ON);
	}

	/*
	 * Ignore Link Down/Up events caused by Downstream Port Containment
	 * if recovery from the error succeeded.
	 */
	if ((events & PCI_EXP_SLTSTA_DLLSC) && pci_dpc_recovered(pdev) &&
	    ctrl->state == ON_STATE) {
		events &= ~PCI_EXP_SLTSTA_DLLSC;
		pciehp_ignore_dpc_link_change(ctrl, pdev, irq);
	}

	/*
	 * Disable requests have higher priority than Presence Detect Changed
	 * or Data Link Layer State Changed events.
	 */
	down_read_nested(&ctrl->reset_lock, ctrl->depth);
	if (events & DISABLE_SLOT)
		pciehp_handle_disable_request(ctrl);
	else if (events & (PCI_EXP_SLTSTA_PDC | PCI_EXP_SLTSTA_DLLSC))
		pciehp_handle_presence_or_link_change(ctrl, events);
	up_read(&ctrl->reset_lock);

	ret = IRQ_HANDLED;
out:
	pci_config_pm_runtime_put(pdev);
	ctrl->ist_running = false;
	wake_up(&ctrl->requester);
	return ret;
}

static int pciehp_poll(void *data)
{
	struct controller *ctrl = data;

	schedule_timeout_idle(10 * HZ); /* start with 10 sec delay */

	while (!kthread_should_stop()) {
		/* poll for interrupt events or user requests */
		while (pciehp_isr(IRQ_NOTCONNECTED, ctrl) == IRQ_WAKE_THREAD ||
		       atomic_read(&ctrl->pending_events))
			pciehp_ist(IRQ_NOTCONNECTED, ctrl);

		if (pciehp_poll_time <= 0 || pciehp_poll_time > 60)
			pciehp_poll_time = 2; /* clamp to sane value */

		schedule_timeout_idle(pciehp_poll_time * HZ);
	}

	return 0;
}

static void pcie_enable_notification(struct controller *ctrl)
{
	u16 cmd, mask;

	/*
	 * TBD: Power fault detected software notification support.
	 *
	 * Power fault detected software notification is not enabled
	 * now, because it caused power fault detected interrupt storm
	 * on some machines. On those machines, power fault detected
	 * bit in the slot status register was set again immediately
	 * when it is cleared in the interrupt service routine, and
	 * next power fault detected interrupt was notified again.
	 */

	/*
	 * Always enable link events: thus link-up and link-down shall
	 * always be treated as hotplug and unplug respectively. Enable
	 * presence detect only if Attention Button is not present.
	 */
	cmd = PCI_EXP_SLTCTL_DLLSCE;
	if (ATTN_BUTTN(ctrl))
		cmd |= PCI_EXP_SLTCTL_ABPE;
	else
		cmd |= PCI_EXP_SLTCTL_PDCE;
	if (!pciehp_poll_mode)
		cmd |= PCI_EXP_SLTCTL_HPIE;
	if (!pciehp_poll_mode && !NO_CMD_CMPL(ctrl))
		cmd |= PCI_EXP_SLTCTL_CCIE;

	mask = (PCI_EXP_SLTCTL_PDCE | PCI_EXP_SLTCTL_ABPE |
		PCI_EXP_SLTCTL_PFDE |
		PCI_EXP_SLTCTL_HPIE | PCI_EXP_SLTCTL_CCIE |
		PCI_EXP_SLTCTL_DLLSCE);

	pcie_write_cmd_nowait(ctrl, cmd, mask);
	ctrl_dbg(ctrl, "%s: SLOTCTRL %x write cmd %x\n", __func__,
		 pci_pcie_cap(ctrl->pcie->port) + PCI_EXP_SLTCTL, cmd);
}

static void pcie_disable_notification(struct controller *ctrl)
{
	u16 mask;

	mask = (PCI_EXP_SLTCTL_PDCE | PCI_EXP_SLTCTL_ABPE |
		PCI_EXP_SLTCTL_MRLSCE | PCI_EXP_SLTCTL_PFDE |
		PCI_EXP_SLTCTL_HPIE | PCI_EXP_SLTCTL_CCIE |
		PCI_EXP_SLTCTL_DLLSCE);
	pcie_write_cmd(ctrl, 0, mask);
	ctrl_dbg(ctrl, "%s: SLOTCTRL %x write cmd %x\n", __func__,
		 pci_pcie_cap(ctrl->pcie->port) + PCI_EXP_SLTCTL, 0);
}

void pcie_clear_hotplug_events(struct controller *ctrl)
{
	pcie_capability_write_word(ctrl_dev(ctrl), PCI_EXP_SLTSTA,
				   PCI_EXP_SLTSTA_PDC | PCI_EXP_SLTSTA_DLLSC);
}

void pcie_enable_interrupt(struct controller *ctrl)
{
	u16 mask;

	mask = PCI_EXP_SLTCTL_DLLSCE;
	if (!pciehp_poll_mode)
		mask |= PCI_EXP_SLTCTL_HPIE;
	pcie_write_cmd(ctrl, mask, mask);
}

void pcie_disable_interrupt(struct controller *ctrl)
{
	u16 mask;

	/*
	 * Mask hot-plug interrupt to prevent it triggering immediately
	 * when the link goes inactive (we still get PME when any of the
	 * enabled events is detected). Same goes with Link Layer State
	 * changed event which generates PME immediately when the link goes
	 * inactive so mask it as well.
	 */
	mask = PCI_EXP_SLTCTL_HPIE | PCI_EXP_SLTCTL_DLLSCE;
	pcie_write_cmd(ctrl, 0, mask);
}

/**
 * pciehp_slot_reset() - ignore link event caused by error-induced hot reset
 * @dev: PCI Express port service device
 *
 * Called from pcie_portdrv_slot_reset() after AER or DPC initiated a reset
 * further up in the hierarchy to recover from an error.  The reset was
 * propagated down to this hotplug port.  Ignore the resulting link flap.
 * If the link failed to retrain successfully, synthesize the ignored event.
 * Surprise removal during reset is detected through Presence Detect Changed.
 */
int pciehp_slot_reset(struct pcie_device *dev)
{
	struct controller *ctrl = get_service_data(dev);

	if (ctrl->state != ON_STATE)
		return 0;

	pcie_capability_write_word(dev->port, PCI_EXP_SLTSTA,
				   PCI_EXP_SLTSTA_DLLSC);

	if (!pciehp_check_link_active(ctrl))
		pciehp_request(ctrl, PCI_EXP_SLTSTA_DLLSC);

	return 0;
}

/*
 * pciehp has a 1:1 bus:slot relationship so we ultimately want a secondary
 * bus reset of the bridge, but at the same time we want to ensure that it is
 * not seen as a hot-unplug, followed by the hot-plug of the device. Thus,
 * disable link state notification and presence detection change notification
 * momentarily, if we see that they could interfere. Also, clear any spurious
 * events after.
 */
int pciehp_reset_slot(struct hotplug_slot *hotplug_slot, bool probe)
{
	struct controller *ctrl = to_ctrl(hotplug_slot);
	struct pci_dev *pdev = ctrl_dev(ctrl);
	u16 stat_mask = 0, ctrl_mask = 0;
	int rc;

	if (probe)
		return 0;

	down_write_nested(&ctrl->reset_lock, ctrl->depth);

	if (!ATTN_BUTTN(ctrl)) {
		ctrl_mask |= PCI_EXP_SLTCTL_PDCE;
		stat_mask |= PCI_EXP_SLTSTA_PDC;
	}
	ctrl_mask |= PCI_EXP_SLTCTL_DLLSCE;
	stat_mask |= PCI_EXP_SLTSTA_DLLSC;

	pcie_write_cmd(ctrl, 0, ctrl_mask);
	ctrl_dbg(ctrl, "%s: SLOTCTRL %x write cmd %x\n", __func__,
		 pci_pcie_cap(ctrl->pcie->port) + PCI_EXP_SLTCTL, 0);

	rc = pci_bridge_secondary_bus_reset(ctrl->pcie->port);

	pcie_capability_write_word(pdev, PCI_EXP_SLTSTA, stat_mask);
	pcie_write_cmd_nowait(ctrl, ctrl_mask, ctrl_mask);
	ctrl_dbg(ctrl, "%s: SLOTCTRL %x write cmd %x\n", __func__,
		 pci_pcie_cap(ctrl->pcie->port) + PCI_EXP_SLTCTL, ctrl_mask);

	up_write(&ctrl->reset_lock);
	return rc;
}

static int pciehp_y4_match_service(struct device *dev, void *unused)
{
	return dev->bus == &pcie_port_bus_type &&
	       to_pcie_device(dev)->service == PCIE_PORT_SERVICE_HP;
}

static int pciehp_y4_read_link(struct pci_dev *ep, struct pci_dev *port,
			       u16 expected, bool *restored)
{
	u16 a, b;
	int ret;

	ret = pcie_capability_read_word(port, PCI_EXP_LNKSTA, &a);
	if (!ret)
		ret = pcie_capability_read_word(ep, PCI_EXP_LNKSTA, &b);
	if (ret)
		return pcibios_err_to_errno(ret);
	if (PCI_POSSIBLE_ERROR(a) || PCI_POSSIBLE_ERROR(b))
		return -ENODEV;
	if ((a | b) & PCI_EXP_LNKSTA_LT || !(a & PCI_EXP_LNKSTA_DLLLA) ||
	    ((a ^ b) & (PCI_EXP_LNKSTA_CLS | PCI_EXP_LNKSTA_NLW)) ||
	    !(a & PCI_EXP_LNKSTA_NLW) || !(a & PCI_EXP_LNKSTA_CLS))
		return -EIO;
	*restored = (a & PCI_EXP_LNKSTA_CLS) >=
		    (expected & PCI_EXP_LNKSTA_CLS) &&
		    (a & PCI_EXP_LNKSTA_NLW) == (expected & PCI_EXP_LNKSTA_NLW);
	return 0;
}

/* System-PM-only recovery of the previously negotiated Y4 M50 link.
 * The caller keeps endpoint clients, DMA and IRQs quiesced throughout.
 * Own the hotplug protocol so a controlled retrain is not surprise removal.
 */
int pci_y4_m50_restore_link(struct pci_dev *ep, u16 saved_lnksta)
{
	struct pci_dev *port = pci_upstream_bridge(ep);
	struct device *service;
	struct controller *ctrl;
	u16 mask = PCI_EXP_SLTCTL_HPIE | PCI_EXP_SLTCTL_DLLSCE;
	u16 slot, old_ctrl, value, target;
	bool restored;
	int ret, restore_ret;

	if (!of_machine_is_compatible("myt,y4") || !port ||
	    ep->vendor != 0x1f6b || ep->device != 0x0c00 ||
	    port->vendor != 0x1ec8 || port->device != 0x8024 ||
	    pci_pcie_type(port) != PCI_EXP_TYPE_DOWNSTREAM ||
	    pci_find_host_bridge(port->bus)->fixed_mps != 256 ||
	    !(saved_lnksta & PCI_EXP_LNKSTA_NLW) ||
	    !(saved_lnksta & PCI_EXP_LNKSTA_CLS) ||
	    (saved_lnksta & PCI_EXP_LNKSTA_CLS) > 4)
		return -EINVAL;
	ret = pciehp_y4_read_link(ep, port, saved_lnksta, &restored);
	if (ret || restored)
		return ret;
	/* Polling can consume slot events even with HPIE disabled. */
	if (pciehp_poll_mode)
		return -EOPNOTSUPP;
	service = device_find_child(&port->dev, NULL, pciehp_y4_match_service);
	if (!service)
		return -ENODEV;
	if (!device_trylock(service)) {
		ret = -EBUSY;
		goto put_service;
	}
	ctrl = get_service_data(to_pcie_device(service));
	if (!service->driver || !ctrl) {
		ret = -ENODEV;
		goto unlock_service;
	}
	/* EP resume owns dev->mutex; never wait for a hotplug removal which
	 * can hold reset_lock while waiting for that same endpoint mutex. */
	if (!down_write_trylock(&ctrl->reset_lock)) {
		ret = -EBUSY;
		goto unlock_service;
	}
	if (READ_ONCE(ctrl->state) != ON_STATE ||
	    READ_ONCE(ctrl->power_fault_detected)) {
		ret = -EIO;
		goto unlock_reset;
	}
	ret = pcie_capability_read_word(port, PCI_EXP_SLTCTL, &old_ctrl);
	if (ret || PCI_POSSIBLE_ERROR(old_ctrl)) {
		ret = -ENODEV;
		goto unlock_reset;
	}
	pcie_write_cmd(ctrl, 0, mask);
	ret = pcie_capability_read_word(port, PCI_EXP_SLTCTL, &value);
	if (ret || PCI_POSSIBLE_ERROR(value) || (value & mask)) {
		ret = -EIO;
		goto restore_notification;
	}
	/* The ISR checks cached HPIE before reading/acknowledging events.
	 * Do not wait for an IRQ thread that might be blocked on reset_lock. */
	if (!synchronize_hardirq(ctrl->pcie->irq) ||
	    READ_ONCE(ctrl->ist_running) || atomic_read(&ctrl->pending_events)) {
		ret = -EBUSY;
		goto restore_notification;
	}
	ret = pcie_capability_read_word(port, PCI_EXP_SLTSTA, &slot);
	if (ret || PCI_POSSIBLE_ERROR(slot) || !(slot & PCI_EXP_SLTSTA_PDS) ||
	    (slot & (PCI_EXP_SLTSTA_PDC | PCI_EXP_SLTSTA_PFD |
		     PCI_EXP_SLTSTA_DLLSC | PCI_EXP_SLTSTA_MRLSS))) {
		ret = -EIO;
		goto restore_notification;
	}
	ret = pcie_capability_read_word(port, PCI_EXP_LNKCTL2, &target);
	if (ret || PCI_POSSIBLE_ERROR(target)) {
		ret = -ENODEV;
		goto restore_notification;
	}
	target = (target & ~PCI_EXP_LNKCTL2_TLS) |
		 (saved_lnksta & PCI_EXP_LNKSTA_CLS);
	ret = pcie_capability_write_word(port, PCI_EXP_LNKCTL2, target);
	if (!ret)
		ret = pcie_capability_read_word(port, PCI_EXP_LNKCTL2, &value);
	if (ret || value != target) {
		ret = -EIO;
		goto restore_notification;
	}
	ret = pcie_retrain_link(port, true);
	if (ret)
		goto restore_notification;
	ret = pciehp_y4_read_link(ep, port, saved_lnksta, &restored);
	if (ret || !restored) {
		ret = ret ?: -EIO;
		goto restore_notification;
	}
	ret = pcie_capability_read_word(port, PCI_EXP_SLTSTA, &slot);
	if (ret || PCI_POSSIBLE_ERROR(slot) || !(slot & PCI_EXP_SLTSTA_PDS) ||
	    (slot & (PCI_EXP_SLTSTA_PDC | PCI_EXP_SLTSTA_PFD |
		     PCI_EXP_SLTSTA_MRLSS)) ||
	    READ_ONCE(ctrl->power_fault_detected) ||
	    atomic_read(&ctrl->pending_events)) {
		ret = -EIO;
		goto restore_notification;
	}
	/* Only this retrain's DLLSC is discardable. Never clear PDC or PFD. */
	ret = pcie_capability_write_word(port, PCI_EXP_SLTSTA, PCI_EXP_SLTSTA_DLLSC);
	if (ret) {
		ret = pcibios_err_to_errno(ret);
		goto restore_notification;
	}
	ctrl_info(ctrl, "system PM restored M50 link to at least Gen%u x%u\n",
		  saved_lnksta & PCI_EXP_LNKSTA_CLS,
		  (saved_lnksta & PCI_EXP_LNKSTA_NLW) >> PCI_EXP_LNKSTA_NLW_SHIFT);
restore_notification:
	pcie_write_cmd_nowait(ctrl, old_ctrl & mask, mask);
	restore_ret = pcie_capability_read_word(port, PCI_EXP_SLTCTL, &value);
	if (restore_ret || PCI_POSSIBLE_ERROR(value) ||
	    (value & mask) != (old_ctrl & mask))
		ret = ret ?: -EIO;
unlock_reset:
	up_write(&ctrl->reset_lock);
unlock_service:
	device_unlock(service);
put_service:
	put_device(service);
	return ret;
}
EXPORT_SYMBOL_GPL(pci_y4_m50_restore_link);

int pcie_init_notification(struct controller *ctrl)
{
	if (pciehp_request_irq(ctrl))
		return -1;
	pcie_enable_notification(ctrl);
	ctrl->notification_enabled = 1;
	return 0;
}

void pcie_shutdown_notification(struct controller *ctrl)
{
	if (ctrl->notification_enabled) {
		pcie_disable_notification(ctrl);
		pciehp_free_irq(ctrl);
		ctrl->notification_enabled = 0;
	}
}

static inline void dbg_ctrl(struct controller *ctrl)
{
	struct pci_dev *pdev = ctrl->pcie->port;
	u16 reg16;

	ctrl_dbg(ctrl, "Slot Capabilities      : 0x%08x\n", ctrl->slot_cap);
	pcie_capability_read_word(pdev, PCI_EXP_SLTSTA, &reg16);
	ctrl_dbg(ctrl, "Slot Status            : 0x%04x\n", reg16);
	pcie_capability_read_word(pdev, PCI_EXP_SLTCTL, &reg16);
	ctrl_dbg(ctrl, "Slot Control           : 0x%04x\n", reg16);
}

#define FLAG(x, y)	(((x) & (y)) ? '+' : '-')

static inline int pcie_hotplug_depth(struct pci_dev *dev)
{
	struct pci_bus *bus = dev->bus;
	int depth = 0;

	while (bus->parent) {
		bus = bus->parent;
		if (bus->self && bus->self->is_hotplug_bridge)
			depth++;
	}

	return depth;
}

struct controller *pcie_init(struct pcie_device *dev)
{
	struct controller *ctrl;
	u32 slot_cap, slot_cap2;
	u8 poweron;
	struct pci_dev *pdev = dev->port;
	struct pci_bus *subordinate = pdev->subordinate;

	ctrl = kzalloc(sizeof(*ctrl), GFP_KERNEL);
	if (!ctrl)
		return NULL;

	ctrl->pcie = dev;
	ctrl->depth = pcie_hotplug_depth(dev->port);
	pcie_capability_read_dword(pdev, PCI_EXP_SLTCAP, &slot_cap);

	if (pdev->hotplug_user_indicators)
		slot_cap &= ~(PCI_EXP_SLTCAP_AIP | PCI_EXP_SLTCAP_PIP);

	/*
	 * We assume no Thunderbolt controllers support Command Complete events,
	 * but some controllers falsely claim they do.
	 */
	if (pdev->is_thunderbolt)
		slot_cap |= PCI_EXP_SLTCAP_NCCS;

	ctrl->slot_cap = slot_cap;
	mutex_init(&ctrl->ctrl_lock);
	mutex_init(&ctrl->state_lock);
	init_rwsem(&ctrl->reset_lock);
	init_waitqueue_head(&ctrl->requester);
	init_waitqueue_head(&ctrl->queue);
	INIT_DELAYED_WORK(&ctrl->button_work, pciehp_queue_pushbutton_work);
	dbg_ctrl(ctrl);

	down_read(&pci_bus_sem);
	ctrl->state = list_empty(&subordinate->devices) ? OFF_STATE : ON_STATE;
	up_read(&pci_bus_sem);
	if (ctrl->state == ON_STATE) {
		/* Firmware prepowered the slot and the PCI core already
		 * enumerated the device, so the state machine starts out ON
		 * and pciehp_check_presence() will never synthesize an enable
		 * event; board_added() therefore never runs and the power
		 * indicator stays off forever. Light it here so an occupied,
		 * pre-enabled slot does not look dead to the operator. The
		 * later notification write does not touch the indicator
		 * fields. */
		pciehp_set_indicators(ctrl, PCI_EXP_SLTCTL_PWR_IND_ON,
				      INDICATOR_NOOP);
	}

	pcie_capability_read_dword(pdev, PCI_EXP_SLTCAP2, &slot_cap2);
	if (slot_cap2 & PCI_EXP_SLTCAP2_IBPD) {
		pcie_write_cmd_nowait(ctrl, PCI_EXP_SLTCTL_IBPD_DISABLE,
				      PCI_EXP_SLTCTL_IBPD_DISABLE);
		ctrl->inband_presence_disabled = 1;
	}

	if (dmi_first_match(inband_presence_disabled_dmi_table))
		ctrl->inband_presence_disabled = 1;

	/* Clear all remaining event bits in Slot Status register. */
	pcie_capability_write_word(pdev, PCI_EXP_SLTSTA,
		PCI_EXP_SLTSTA_ABP | PCI_EXP_SLTSTA_PFD |
		PCI_EXP_SLTSTA_MRLSC | PCI_EXP_SLTSTA_CC |
		PCI_EXP_SLTSTA_DLLSC | PCI_EXP_SLTSTA_PDC);

	ctrl_info(ctrl, "Slot #%d AttnBtn%c PwrCtrl%c MRL%c AttnInd%c PwrInd%c HotPlug%c Surprise%c Interlock%c NoCompl%c IbPresDis%c LLActRep%c%s\n",
		(slot_cap & PCI_EXP_SLTCAP_PSN) >> 19,
		FLAG(slot_cap, PCI_EXP_SLTCAP_ABP),
		FLAG(slot_cap, PCI_EXP_SLTCAP_PCP),
		FLAG(slot_cap, PCI_EXP_SLTCAP_MRLSP),
		FLAG(slot_cap, PCI_EXP_SLTCAP_AIP),
		FLAG(slot_cap, PCI_EXP_SLTCAP_PIP),
		FLAG(slot_cap, PCI_EXP_SLTCAP_HPC),
		FLAG(slot_cap, PCI_EXP_SLTCAP_HPS),
		FLAG(slot_cap, PCI_EXP_SLTCAP_EIP),
		FLAG(slot_cap, PCI_EXP_SLTCAP_NCCS),
		FLAG(slot_cap2, PCI_EXP_SLTCAP2_IBPD),
		FLAG(pdev->link_active_reporting, true),
		pdev->broken_cmd_compl ? " (with Cmd Compl erratum)" : "");

	/*
	 * If empty slot's power status is on, turn power off.  The IRQ isn't
	 * requested yet, so avoid triggering a notification with this command.
	 */
	if (POWER_CTRL(ctrl)) {
		pciehp_get_power_status(ctrl, &poweron);
		if (!pciehp_card_present_or_link_active(ctrl) && poweron) {
			pcie_disable_notification(ctrl);
			pciehp_power_off_slot(ctrl);
		}
	}

	return ctrl;
}

void pciehp_release_ctrl(struct controller *ctrl)
{
	cancel_delayed_work_sync(&ctrl->button_work);
	kfree(ctrl);
}

static void quirk_cmd_compl(struct pci_dev *pdev)
{
	u32 slot_cap;

	if (pci_is_pcie(pdev)) {
		pcie_capability_read_dword(pdev, PCI_EXP_SLTCAP, &slot_cap);
		if (slot_cap & PCI_EXP_SLTCAP_HPC &&
		    !(slot_cap & PCI_EXP_SLTCAP_NCCS))
			pdev->broken_cmd_compl = 1;
	}
}
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_INTEL, PCI_ANY_ID,
			      PCI_CLASS_BRIDGE_PCI, 8, quirk_cmd_compl);
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_QCOM, 0x010e,
			      PCI_CLASS_BRIDGE_PCI, 8, quirk_cmd_compl);
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_QCOM, 0x0110,
			      PCI_CLASS_BRIDGE_PCI, 8, quirk_cmd_compl);
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_QCOM, 0x0400,
			      PCI_CLASS_BRIDGE_PCI, 8, quirk_cmd_compl);
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_QCOM, 0x0401,
			      PCI_CLASS_BRIDGE_PCI, 8, quirk_cmd_compl);
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_HXT, 0x0401,
			      PCI_CLASS_BRIDGE_PCI, 8, quirk_cmd_compl);
