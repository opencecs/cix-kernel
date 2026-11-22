// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2024 Houmo AI Inc.
 * Author: hongxing.ma<hongxing.ma@houmo.ai>
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/of.h>
#include <linux/idr.h>
#include <linux/pm_runtime.h>
#include <linux/suspend.h>
#include <linux/delay.h>
#include <xh2a_host_drv.h>
#include <xh2a_rpmsg_lite_api.h>
#include <xh2a_pcie_api.h>
#include <version_compat.h>
#include "xh2a_pcie.h"

/* Linux dev_t minor encoding limit, not the CTL host-device capacity. */
#define XH2A_PCIE_MINOR_ID_MAX (1U << MINORBITS)
static DEFINE_IDR(xh2a_minor_idr);
static DEFINE_MUTEX(xh2a_minor_idr_mutex);

static bool hm_runtime_pm_enable = !IS_ENABLED(CONFIG_XH2A_KEEP_AWAKE_BY_DEFAULT);
module_param_named(runtime_pm_enable, hm_runtime_pm_enable, bool, 0644);
MODULE_PARM_DESC(runtime_pm_enable, "Enable runtime PM (overrides the configured idle policy)");

static pci_ers_result_t xh2a_pcie_err_error_detected(struct pci_dev *pdev,
						     pci_channel_state_t state)
{
	(void)state;
	return PCI_ERS_RESULT_NEED_RESET;
}

static pci_ers_result_t xh2a_pcie_err_slot_reset(struct pci_dev *pdev)
{
	return PCI_ERS_RESULT_NEED_RESET;
}

/* System PM is one notifier transaction, potentially containing both the
 * freeze/thaw and poweroff/restore pairs used by hibernation. */
int xh2a_pcie_pm_io_status(void *handle)
{
	struct xh2a_pcie_dev *xh2a = handle;
	unsigned long flags;
	int ret = 0;

	if (IS_ERR_OR_NULL(xh2a))
		return -ENODEV;
	spin_lock_irqsave(&xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);
	if (xh2a->reset_pending || atomic_read(&xh2a->shutting_down))
		ret = -ENODEV;
	else if (xh2a->pm_failed || xh2a->runtime_pm_failed)
		ret = -EIO;
	else if (xh2a->system_pm_active)
		ret = -EAGAIN;
	spin_unlock_irqrestore(&xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);
	return ret;
}
EXPORT_SYMBOL(xh2a_pcie_pm_io_status);

static void xh2a_pcie_pm_mask_irqs(struct xh2a_pcie_dev *xh2a)
{
	int i;

	/* Track only the disable depth owned by this driver's PM transaction. */
	mutex_lock(&xh2a->pm_irq_lock);
	for (i = 0; i < xh2a->irqs; i++) {
		if (xh2a->pm_irqs_disabled & BIT(i))
			continue;
		disable_irq_nosync(xh2a->msi_irq[i]);
		xh2a->pm_irqs_disabled |= BIT(i);
	}
	for (i = 0; i < xh2a->irqs; i++)
		synchronize_irq(xh2a->msi_irq[i]);
	mutex_unlock(&xh2a->pm_irq_lock);
}

static int xh2a_pcie_pm_enable_irqs(struct xh2a_pcie_dev *xh2a,
				     unsigned long requested)
{
	unsigned long flags;
	int i, ret = 0;

	mutex_lock(&xh2a->pm_irq_lock);
	spin_lock_irqsave(&xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);
	if (xh2a_pcie_mmio_blocked(xh2a) || atomic_read(&xh2a->shutting_down))
		ret = -ESHUTDOWN;
	spin_unlock_irqrestore(&xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);
	if (ret)
		goto out;
	/* HDMA vector 7 must run before a pending AOSS vector 3 can request DMA. */
	for (i = xh2a->irqs - 1; i >= 0; i--) {
		if (!(requested & xh2a->pm_irqs_disabled & BIT(i)))
			continue;
		enable_irq(xh2a->msi_irq[i]);
		xh2a->pm_irqs_disabled &= ~BIT(i);
	}
	/* A concurrent terminal error will mask the newly enabled bits under
	 * this same mutex. Do not report readiness after its MMIO gate closed. */
	spin_lock_irqsave(&xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);
	if (xh2a_pcie_mmio_blocked(xh2a) || atomic_read(&xh2a->shutting_down))
		ret = -ESHUTDOWN;
	spin_unlock_irqrestore(&xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);
out:
	mutex_unlock(&xh2a->pm_irq_lock);
	return ret;
}

static int xh2a_pcie_pm_unmask_irqs(struct xh2a_pcie_dev *xh2a)
{
	return xh2a_pcie_pm_enable_irqs(xh2a,
				       GENMASK(XH2A_PCIE_MSI_NVEC - 1, 0));
}

static int xh2a_pcie_pm_restore_transport(struct xh2a_pcie_dev *xh2a)
{
	int ret;

	/* State 4 asks SCP to restore its saved image through AOSS transport.
	 * That worker needs HDMA and its completion IRQ before firmware can
	 * reach state 6. User admission and all other client IRQs stay closed. */
	xh2a_pcie_hdma_resume(&xh2a->hdma);
	ret = xh2a_pcie_pm_enable_irqs(xh2a,
		BIT(XH2A_PCIE_MSI_ID_HDMA) | BIT(XH2A_PCIE_MSI_ID_AOSS));
	if (ret)
		return ret;
	dev_info(&xh2a->pdev->dev,
		 "system PM: image restore transport ready before HOST_READY\n");
	return 0;
}

static int xh2a_pcie_pm_check_dstate(struct xh2a_pcie_dev *xh2a,
				     pci_power_t expected)
{
	u16 pmcsr, vendor;
	int ret;

	if (!xh2a->pdev->pm_cap)
		return -EOPNOTSUPP;
	ret = pci_read_config_word(xh2a->pdev, PCI_VENDOR_ID, &vendor);
	if (ret || vendor == 0xffff || vendor != xh2a->pdev->vendor)
		return -ENODEV;
	ret = pci_read_config_word(xh2a->pdev,
				   xh2a->pdev->pm_cap + PCI_PM_CTRL, &pmcsr);
	if (ret || pmcsr == 0xffff)
		return -ENODEV;
	if ((pmcsr & PCI_PM_CTRL_STATE_MASK) != expected)
		return -EIO;
	return 0;
}

static int xh2a_pcie_pm_quiesce(struct xh2a_pcie_dev *xh2a)
{
	struct xh2a_pcie_client *client;
	int i, ret;

	xh2a_pcie_pm_mask_irqs(xh2a);
	/* No IRQ producer can queue another client callback after synchronize. */
	list_for_each_entry(client, &xh2a->client_list, node)
		for (i = 0; i < client->work_num; i++)
			if (client->work[i].msi_work)
				cancel_work_sync(client->work[i].msi_work);
	cancel_work_sync(&xh2a->hdma.hdma_work);
	ret = xh2a_pcie_hdma_stop(&xh2a->hdma);
	return ret;
}

static int xh2a_pcie_pm_check_config(struct xh2a_pcie_dev *xh2a)
{
	struct pci_dev *pdev = xh2a->pdev;
	u16 command;
	u32 low, high;
	u64 address;
	int bar, ret;

	ret = pci_read_config_word(pdev, PCI_COMMAND, &command);
	if (ret)
		return pcibios_err_to_errno(ret);
	if (command == 0xffff)
		return -ENODEV;
	if ((command & (PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER)) !=
	    (PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER))
		return -EIO;
	/* pci_restore_state() has no error return. Check all three mapped BARs
	 * in PCI bus coordinates before permitting any access through iomap. */
	for (bar = XH2A_PCIE_BAR0; bar <= XH2A_PCIE_BAR4; bar += 2) {
		ret = pci_read_config_dword(pdev, PCI_BASE_ADDRESS_0 + bar * 4, &low);
		if (ret)
			return pcibios_err_to_errno(ret);
		if (low == 0xffffffff)
			return -ENODEV;
		if (!(pci_resource_flags(pdev, bar) & IORESOURCE_MEM) ||
		    (low & PCI_BASE_ADDRESS_SPACE_IO) ||
		    (low & PCI_BASE_ADDRESS_MEM_TYPE_MASK) != PCI_BASE_ADDRESS_MEM_TYPE_64)
			return -EIO;
		ret = pci_read_config_dword(pdev, PCI_BASE_ADDRESS_0 + (bar + 1) * 4,
					    &high);
		if (ret)
			return pcibios_err_to_errno(ret);
		address = ((u64)high << 32) | (low & PCI_BASE_ADDRESS_MEM_MASK);
		if (!pci_resource_len(pdev, bar) || address != pci_bus_address(pdev, bar))
			return -EIO;
	}
	return 0;
}

static int xh2a_pcie_pm_enter_d3hot(struct xh2a_pcie_dev *xh2a)
{
	unsigned long flags;
	int ret, rollback;

	ret = pci_save_state(xh2a->pdev);
	if (ret)
		return ret;
	/* normal hibernate firmware waits for PCI D3/PERST, not ELBI. Commit
	 * this endpoint's D3hot here so a pre-noirq abort can return it to D0.
	 * This neither powers off a bridge nor changes other endpoints. */
	spin_lock_irqsave(&xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);
	xh2a->pm_mmio_suspended = true;
	spin_unlock_irqrestore(&xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);
	ret = pci_set_power_state(xh2a->pdev, PCI_D3hot);
	if (!ret)
		ret = xh2a_pcie_pm_check_dstate(xh2a, PCI_D3hot);
	if (!ret) {
		dev_info(&xh2a->pdev->dev, "system PM: endpoint D3hot confirmed\n");
		return 0;
	}
	/* The PM core does not resume a device whose suspend callback failed. */
	rollback = pci_set_power_state(xh2a->pdev, PCI_D0);
	if (!rollback) {
		pci_restore_state(xh2a->pdev);
		rollback = xh2a_pcie_pm_check_dstate(xh2a, PCI_D0);
		if (!rollback)
			rollback = xh2a_pcie_pm_check_config(xh2a);
	}
	if (!rollback) {
		spin_lock_irqsave(&xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);
		xh2a->pm_mmio_suspended = false;
		spin_unlock_irqrestore(&xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);
	}
	dev_err(&xh2a->pdev->dev, "D3hot transition failed %d, D0 rollback %d\n",
		ret, rollback);
	return ret;
}

static int xh2a_pcie_pm_resume_access(struct xh2a_pcie_dev *xh2a)
{
	unsigned long flags;
	int ret;

	/* The PCI core restores D0/config even for pre-noirq aborts when the
	 * driver saved config. Its return value is not propagated: verify it. */
	ret = xh2a_pcie_pm_check_dstate(xh2a, PCI_D0);
	if (!ret)
		ret = xh2a_pcie_pm_check_config(xh2a);
	if (ret)
		return ret;
	spin_lock_irqsave(&xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);
	if (xh2a->pm_failed || xh2a->reset_pending)
		ret = -ESHUTDOWN;
	else
		xh2a->pm_mmio_suspended = false;
	spin_unlock_irqrestore(&xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);
	return ret;
}

static int xh2a_pcie_pm_error(struct xh2a_pcie_dev *xh2a, int error,
			    const char *phase, bool drain_work)
{
	struct xh2a_pcie_client *client;
	unsigned long flags;
	int i, stop_error = 0;

	/* Stop IRQ producers before waking a transport worker waiting on DMA. */
	xh2a_pcie_pm_mask_irqs(xh2a);
	if (error >= 0 || error == -EBUSY || error == -EAGAIN)
		error = -EIO;
	/* An inaccessible BAR must not be used for best-effort cleanup. */
	if (error != -ENODEV && error != -ESHUTDOWN &&
	    !READ_ONCE(xh2a->pm_failed))
		stop_error = xh2a_pcie_hdma_stop(&xh2a->hdma);
	spin_lock_irqsave(&xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);
	xh2a->pm_failed = true;
	xh2a->runtime_pm_failed = true;
	xh2a->system_pm_phase = XH2A_SYSTEM_PM_FAILED;
	xh2a->system_pm_error = error;
	atomic_set(&xh2a->hdma.suspend_in_progress, 1);
	xh2a_pcie_hdma_wake_waiters_locked(&xh2a->hdma);
	spin_unlock_irqrestore(&xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);
	/* An enable racing the first mask can no longer pass the failed gate.
	 * Re-mask before draining, without taking another disable depth. */
	xh2a_pcie_pm_mask_irqs(xh2a);
	/* Only system PM callers request draining. A runtime PM error may
	 * return through an ioctl holding a lock needed by a client worker.
	 * Stop/wake above must precede cancel of the image-restore worker. */
	if (drain_work) {
		list_for_each_entry(client, &xh2a->client_list, node)
			for (i = 0; i < client->work_num; i++)
				if (client->work[i].msi_work)
					cancel_work_sync(client->work[i].msi_work);
		cancel_work_sync(&xh2a->hdma.hdma_work);
	}
	/* This prevents new requests; it is not proof that DMA has drained. */
	pci_clear_master(xh2a->pdev);
	dev_err(&xh2a->pdev->dev,
		"%s failed (%d), HDMA stop %d; I/O blocked until reinitialization\n",
		phase, error, stop_error);
	return error;
}

static int xh2a_pcie_wait_power_states(struct xh2a_pcie_dev *xh2a,
				     u32 first, u32 second,
				     unsigned int timeout_ms, u32 *observed)
{
	unsigned long timeout = jiffies + msecs_to_jiffies(timeout_ms);
	u32 state = ~0U;
	int ret;

	do {
		ret = xh2a_pcie_check_pwrsts(xh2a, &state);
		if (ret)
			return ret;
		if (state == first || state == second) {
			if (observed)
				*observed = state;
			return 0;
		}
		usleep_range(10, 100);
	} while (time_before(jiffies, timeout));
	dev_err(&xh2a->pdev->dev,
		"system PM state timeout: got %#x expected %#x/%#x after %u ms\n",
		state, first, second, timeout_ms);
	return -ETIMEDOUT;
}

static void xh2a_pcie_pm_put(struct xh2a_pcie_dev *xh2a, bool idle)
{
	if (!xh2a->system_pm_ref)
		return;
	xh2a->system_pm_ref = false;
	if (idle) {
		pm_runtime_mark_last_busy(&xh2a->pdev->dev);
		pm_runtime_put_autosuspend(&xh2a->pdev->dev);
	} else {
		pm_runtime_put_noidle(&xh2a->pdev->dev);
	}
}

static int xh2a_pcie_pm_rollback_notifiers(struct xh2a_pcie_dev *xh2a)
{
	struct xh2a_pcie_client *client;
	int ret, error = 0;

	list_for_each_entry_reverse(client, &xh2a->client_list, node) {
		if (!client->system_pm_notified)
			continue;
		ret = client->notifier_prepare_cb(xh2a, true);
		if (ret && !error)
			error = ret;
		if (!ret)
			client->system_pm_notified = false;
	}
	return error;
}

static int xh2a_pcie_pm_notifier(struct notifier_block *nb,
				 unsigned long action, void *data)
{
	struct xh2a_pcie_dev *xh2a =
		container_of(nb, struct xh2a_pcie_dev, pm_notifier);
	struct xh2a_pcie_client *client;
	struct device *dev = &xh2a->pdev->dev;
	unsigned long flags;
	int ret = 0, rollback;

	switch (action) {
	case PM_HIBERNATION_PREPARE:
		/* A poweroff callback failure can fall back to kernel_power_off().
		 * Reject unvalidated disk hibernation before the transaction starts. */
		dev_err(dev, "disk hibernation is not supported\n");
		return notifier_from_errno(-EOPNOTSUPP);
	case PM_SUSPEND_PREPARE:
		ret = xh2a_pcie_pm_io_status(xh2a);
		if (ret)
			return notifier_from_errno(ret);
		spin_lock_irqsave(&xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);
		xh2a->system_pm_active = true;
		xh2a->system_pm_phase = XH2A_SYSTEM_PM_PREPARING;
		spin_unlock_irqrestore(&xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);
		/* No driver lock is held: runtime resume can call clients. */
		ret = pm_runtime_resume_and_get(dev);
		if (ret < 0) {
			/* PCI/runtime resume did not establish BAR accessibility. */
			WRITE_ONCE(xh2a->pm_mmio_suspended, true);
			goto prepare_failed;
		}
		xh2a->system_pm_ref = true;
		list_for_each_entry(client, &xh2a->client_list, node) {
			client->system_pm_notified = false;
			client->system_pm_prepared = false;
		}
		ret = xh2a_pcie_hdma_stop(&xh2a->hdma);
		if (ret)
			goto prepare_failed;
		list_for_each_entry(client, &xh2a->client_list, node) {
			if (!client->notifier_prepare_cb)
				continue;
			ret = client->notifier_prepare_cb(xh2a, false);
			if (ret) {
				/* Only an early busy check promises no partial hardware
				 * stop. Other failures retain all user/DMA gates. */
				if (ret != -EBUSY)
					goto prepare_failed;
				rollback = xh2a_pcie_pm_rollback_notifiers(xh2a);
				if (rollback)
					goto prepare_failed;
				xh2a_pcie_hdma_resume(&xh2a->hdma);
				spin_lock_irqsave(&xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);
				xh2a->system_pm_active = false;
				xh2a->system_pm_phase = XH2A_SYSTEM_PM_IDLE;
				spin_unlock_irqrestore(&xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);
				xh2a_pcie_pm_put(xh2a, true);
				return notifier_from_errno(ret);
			}
			client->system_pm_notified = true;
		}
		xh2a->system_pm_phase = XH2A_SYSTEM_PM_PREPARED;
		return NOTIFY_OK;

	case PM_POST_HIBERNATION:
	case PM_POST_SUSPEND:
		if (!xh2a->system_pm_active)
			return NOTIFY_DONE;
		if (xh2a->pm_failed) {
			xh2a_pcie_pm_put(xh2a, false);
			return notifier_from_errno(xh2a->system_pm_error);
		}
		if (xh2a->system_pm_phase == XH2A_SYSTEM_PM_PREPARED) {
			/* Another device aborted before this device entered sleep. */
			ret = xh2a_pcie_pm_rollback_notifiers(xh2a);
			if (ret)
				goto post_failed;
		} else if (xh2a->system_pm_phase == XH2A_SYSTEM_PM_RESUMED) {
			ret = xh2a_pcie_write_bar_msgbit(xh2a,
				XH2A_LPCTRL_EXIT_HIBERNATE_CODE, XH2A_PCIE_MSG_TO_E2);
			if (ret)
				goto post_failed;
			list_for_each_entry(client, &xh2a->client_list, node) {
				if (!client->system_pm_notified || !client->notifier_complete_cb)
					continue;
				ret = client->notifier_complete_cb(xh2a);
				if (ret)
					goto post_failed;
				client->system_pm_notified = false;
			}
		} else {
			ret = -EIO;
			goto post_failed;
		}
		xh2a_pcie_hdma_resume(&xh2a->hdma);
		spin_lock_irqsave(&xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);
		xh2a->system_pm_phase = XH2A_SYSTEM_PM_IDLE;
		xh2a->system_pm_active = false;
		spin_unlock_irqrestore(&xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);
		xh2a_pcie_pm_put(xh2a, true);
		return NOTIFY_OK;
	default:
		return NOTIFY_DONE;
	}
prepare_failed:
	ret = xh2a_pcie_pm_error(xh2a, ret, "system PM prepare", true);
	xh2a_pcie_pm_put(xh2a, false);
	return notifier_from_errno(ret);
post_failed:
	ret = xh2a_pcie_pm_error(xh2a, ret, "system PM completion", true);
	xh2a_pcie_pm_put(xh2a, false);
	return notifier_from_errno(ret);
}

/* Capture the negotiated link before hibernate can change endpoint power. */
static int xh2a_pcie_pm_save_link(struct xh2a_pcie_dev *xh2a)
{
	struct pci_dev *pdev = xh2a->pdev, *port = pci_upstream_bridge(pdev);
	u16 ep, dsp;
	int ret;

	xh2a->system_pm_link_status = 0;
	if (!of_machine_is_compatible("myt,y4") || !port ||
	    pdev->vendor != 0x1f6b || pdev->device != 0x0c00 ||
	    port->vendor != 0x1ec8 || port->device != 0x8024 ||
	    pci_pcie_type(port) != PCI_EXP_TYPE_DOWNSTREAM ||
	    pci_find_host_bridge(port->bus)->fixed_mps != 256)
		return 0;
	ret = pcie_capability_read_word(pdev, PCI_EXP_LNKSTA, &ep);
	if (!ret)
		ret = pcie_capability_read_word(port, PCI_EXP_LNKSTA, &dsp);
	if (ret)
		return pcibios_err_to_errno(ret);
	if (PCI_POSSIBLE_ERROR(ep) || PCI_POSSIBLE_ERROR(dsp))
		return -ENODEV;
	if ((ep | dsp) & PCI_EXP_LNKSTA_LT || !(dsp & PCI_EXP_LNKSTA_DLLLA) ||
	    !(ep & PCI_EXP_LNKSTA_NLW) || !(ep & PCI_EXP_LNKSTA_CLS) ||
	    (ep & PCI_EXP_LNKSTA_CLS) > 4 ||
	    ((ep ^ dsp) & (PCI_EXP_LNKSTA_CLS | PCI_EXP_LNKSTA_NLW)))
		return -EIO;
	xh2a->system_pm_link_status = ep &
		(PCI_EXP_LNKSTA_CLS | PCI_EXP_LNKSTA_NLW);
	return 0;
}

static int xh2a_pcie_pm_enter(struct device *dev,
			      enum xh2a_lpctrl_start_cmd cmd)
{
	struct xh2a_pcie_dev *xh2a = pci_get_drvdata(to_pci_dev(dev));
	struct xh2a_pcie_client *client;
	u32 mode;
	unsigned long flags;
	int ret;

	if (!xh2a)
		return -ENODEV;
	if (xh2a->pm_failed)
		return -EIO;
	if (!xh2a->system_pm_active ||
	    (xh2a->system_pm_phase != XH2A_SYSTEM_PM_PREPARED &&
	     xh2a->system_pm_phase != XH2A_SYSTEM_PM_RESUMED))
		return -EINVAL;
	switch (cmd) {
	case XH2A_LPCTRL_START_SUSPEND:
		mode = XH2A_LPCTRL_ALT_MODE_NORMAL;
		break;
	case XH2A_LPCTRL_START_FREEZE:
		mode = XH2A_LPCTRL_ALT_MODE_FREEZE;
		break;
	case XH2A_LPCTRL_START_POWEROFF:
		/* Hibernation-image restore needs a separately validated firmware
		 * wake sequence. Reject before touching firmware; POST can unwind
		 * the completed freeze/thaw transaction without losing the device. */
		return -EOPNOTSUPP;
	default:
		return -EINVAL;
	}
	ret = xh2a_pcie_pm_save_link(xh2a);
	if (ret)
		goto failed;
	xh2a->system_pm_phase = XH2A_SYSTEM_PM_ENTERING;
	xh2a->pdev->d3cold_allowed = xh2a->d3cold_allowed_saved;
	/* Firmware image-save traffic needs HDMA after users have been gated. */
	xh2a_pcie_hdma_resume(&xh2a->hdma);
	ret = xh2a_pcie_pio_writel(xh2a, XH2A_LPCTRL_ALT_MODE_REG, mode);
	if (ret)
		goto failed;
	ret = xh2a_rpmsg_lite_ext_send(xh2a,
		XH2A_LPCTRL_START_HIBERNATE_ID,
		XH2A_LPCTRL_START_HIBERNATE_STR,
		XH2A_LPCTRL_START_HIBERNATE_LEN);
	if (ret)
		goto failed;
	ret = xh2a_pcie_wait_power_states(xh2a, XH2A_LPSTS_U7_ENTER_LP,
			XH2A_LPSTS_E2_ENTER_LP, 5000, NULL);
	if (ret)
		goto failed;
	/* An observed protocol acknowledgment, never a timeout-based fallback. */
	xh2a->system_pm_compatible = true;
	list_for_each_entry(client, &xh2a->client_list, node) {
		client->system_pm_prepared = false;
		if (client->prepare_cb) {
			ret = client->prepare_cb(xh2a, xh2a->system_pm_compatible);
			if (ret)
				goto failed;
		}
		client->system_pm_prepared = true;
	}
	ret = xh2a_pcie_wait_power_states(xh2a, XH2A_LPSTS_E2_ENTER_LP,
			XH2A_LPSTS_E2_ENTER_LP, 1000, NULL);
	if (ret)
		goto failed;
	ret = xh2a_pcie_pm_quiesce(xh2a);
	if (ret)
		goto failed;
	spin_lock_irqsave(&xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);
	xh2a->pm_mmio_suspended = true;
	spin_unlock_irqrestore(&xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);
	/* Sky1 also stops the link and asserts PERST for s2idle. */
	if (cmd == XH2A_LPCTRL_START_SUSPEND) {
		ret = xh2a_pcie_pm_enter_d3hot(xh2a);
		if (ret)
			goto failed;
	}
	xh2a->system_pm_phase = XH2A_SYSTEM_PM_ENTERED;
	dev_info(dev, "system PM entered: firmware acknowledged state 2\n");
	return 0;
failed:
	return xh2a_pcie_pm_error(xh2a, ret, "system PM enter", true);
}

static int xh2a_pcie_pm_exit(struct device *dev, enum xh2a_lpctrl_start_cmd cmd)
{
	struct xh2a_pcie_dev *xh2a = pci_get_drvdata(to_pci_dev(dev));
	struct xh2a_pcie_client *client;
	u32 state;
	unsigned long flags;
	int ret;

	if (!xh2a)
		return -ENODEV;
	if (xh2a->pm_failed)
		return -EIO;
	if (!xh2a->system_pm_active ||
	    xh2a->system_pm_phase != XH2A_SYSTEM_PM_ENTERED)
		return -EINVAL;
	ret = xh2a_pcie_pm_resume_access(xh2a);
	if (ret)
		goto failed;
	if (cmd == XH2A_LPCTRL_START_RESUME || cmd == XH2A_LPCTRL_START_THAW) {
		ret = xh2a_pcie_write_bar_msgbit(xh2a,
			XH2A_LPCTRL_EXIT_IDLE_OR_L1_CODE, XH2A_PCIE_MSG_TO_E2);
		if (ret)
			goto failed;
	} else if (cmd != XH2A_LPCTRL_START_RESTORE) {
		return -EINVAL;
	}
	ret = xh2a_pcie_wait_power_states(xh2a, XH2A_LPSTS_E2_WAIT_HOST,
			XH2A_LPSTS_U7_EXIT_LP, 5000, &state);
	if (ret)
		goto failed;
	/* No client/HDMA IRQ has been opened yet; retrain before image restore. */
	if (xh2a->system_pm_link_status) {
		spin_lock_irqsave(&xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);
		xh2a->pm_mmio_suspended = true;
		spin_unlock_irqrestore(&xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);
		ret = pci_y4_m50_restore_link(xh2a->pdev,
					    xh2a->system_pm_link_status);
		if (!ret)
			ret = xh2a_pcie_pm_resume_access(xh2a);
		if (ret)
			goto failed;
	}
	ret = xh2a_pcie_probe_post(xh2a);
	if (ret)
		goto failed;
	if (state == XH2A_LPSTS_E2_WAIT_HOST) {
		ret = xh2a_pcie_pm_restore_transport(xh2a);
		if (ret)
			goto failed;
		ret = xh2a_pcie_update_pwrsts(xh2a, XH2A_LPSTS_HOST_READY);
		if (ret)
			goto failed;
	}
	ret = xh2a_pcie_wait_power_states(xh2a, XH2A_LPSTS_U7_EXIT_LP,
			XH2A_LPSTS_U7_EXIT_LP, 5000, NULL);
	if (ret)
		goto failed;
	xh2a_pcie_hdma_resume(&xh2a->hdma);
	ret = xh2a_pcie_pm_unmask_irqs(xh2a);
	if (ret)
		goto failed;
	list_for_each_entry(client, &xh2a->client_list, node) {
		if (!client->system_pm_prepared)
			continue;
		if (client->complete_cb) {
			ret = client->complete_cb(xh2a, xh2a->system_pm_compatible);
			if (ret)
				goto failed;
		}
		client->system_pm_prepared = false;
	}
	xh2a->pdev->d3cold_allowed = 0;
	xh2a->system_pm_phase = XH2A_SYSTEM_PM_RESUMED;
	dev_info(dev, "system PM resumed: firmware and clients ready\n");
	return 0;
failed:
	return xh2a_pcie_pm_error(xh2a, ret, "system PM resume", true);
}

static int xh2a_pcie_pm_suspend(struct device *dev)
{
	/* suspend callback: notify device to enter suspend state;
	 * firmware enter "wait" state for pcie link state transition.
	 * firmware should add pcie link state transition and ELBI state
	 * transition to its sensitivity list.
	 */
	return xh2a_pcie_pm_enter(dev, XH2A_LPCTRL_START_SUSPEND);
}

static int xh2a_pcie_pm_resume(struct device *dev)
{
	/* resume callback: notify device to exit suspend state;
	 * firmware should wake up for pcie link state transition or ELBI state
	 * transition.
	 */
	return xh2a_pcie_pm_exit(dev, XH2A_LPCTRL_START_RESUME);
}

static int xh2a_pcie_pm_freeze(struct device *dev)
{
	/* freeze callback: notify device to enter freeze state;
	 * firmware enter "wait" state for ELBI state transition.
	 * pcie link state remains unchanged during freeze & thaw.
	 */
	return xh2a_pcie_pm_enter(dev, XH2A_LPCTRL_START_FREEZE);
}

static int xh2a_pcie_pm_thaw(struct device *dev)
{
	/* thaw callback: notify device to exit freeze state, back to D0 state.
	 * firmware should wake up for ELBI state transition.
	 */
	return xh2a_pcie_pm_exit(dev, XH2A_LPCTRL_START_THAW);
}

static int xh2a_pcie_pm_poweroff(struct device *dev)
{
	/* poweroff callback: notify device to enter poweroff state.
	 * firmware enter "wait" state for pcie link state transition.
	 */
	return xh2a_pcie_pm_enter(dev, XH2A_LPCTRL_START_POWEROFF);
}

static int xh2a_pcie_pm_restore(struct device *dev)
{
	/* restore callback: notfiy device to exit hibernation.
	 */
	return xh2a_pcie_pm_exit(dev, XH2A_LPCTRL_START_RESTORE);
}

/*
 * A failed handshake leaves firmware/client state uncertain.  Do not let the
 * PM core report success or retry I/O against a partially resumed device.
 */
static int xh2a_pcie_runtime_pm_error(struct xh2a_pcie_dev *xh2a, int error)
{
	return xh2a_pcie_pm_error(xh2a, error, "runtime PM", false);
}

static int xh2a_pcie_wait_power_state(struct xh2a_pcie_dev *xh2a,
				    u32 expected, unsigned int timeout_ms)
{
	unsigned long timeout = jiffies + msecs_to_jiffies(timeout_ms);
	u32 state;
	int ret;

	do {
		ret = xh2a_pcie_check_pwrsts(xh2a, &state);
		if (ret)
			return ret;
		if (state == expected)
			return 0;
		usleep_range(10, 100);
	} while (time_before(jiffies, timeout));

	dev_err(&xh2a->pdev->dev, "power state timeout: got %#x, expected %#x\n",
		state, expected);
	return -ETIMEDOUT;
}

static int xh2a_pcie_pm_runtime_suspend(struct device *dev)
{
	struct xh2a_pcie_dev *xh2a = pci_get_drvdata(to_pci_dev(dev));
	struct xh2a_pcie_client *pos;
	int ret;

	if (!xh2a)
		return -ENODEV;
	if (xh2a->runtime_pm_failed)
		return -EIO;

	list_for_each_entry(pos, &xh2a->client_list, node) {
		if (!pos->runtime_suspend_cb)
			continue;
		ret = pos->runtime_suspend_cb(xh2a);
		if (ret)
			goto failed;
	}

	ret = xh2a_pcie_pio_writel(xh2a, XH2A_LPCTRL_ALT_MODE_REG,
				 XH2A_LPCTRL_ALT_MODE_NORMAL);
	if (ret)
		goto failed;
	ret = xh2a_rpmsg_lite_ext_send(xh2a, XH2A_LPCTRL_START_SLEEP_ID,
				     XH2A_LPCTRL_START_SLEEP_STR,
				     XH2A_LPCTRL_START_SLEEP_LEN);
	if (ret)
		goto failed;

	ret = xh2a_pcie_wait_power_state(xh2a, XH2A_LPSTS_E2_ENTER_LP, 1000);
	if (ret)
		goto failed;

	dev_info(dev, "runtime suspended: firmware sleep acknowledged\n");
	return 0;

failed:
	/* Already suspended clients remain stopped until device reinitialization. */
	return xh2a_pcie_runtime_pm_error(xh2a, ret);
}

static int xh2a_pcie_pm_runtime_resume(struct device *dev)
{
	struct xh2a_pcie_dev *xh2a = pci_get_drvdata(to_pci_dev(dev));
	struct xh2a_pcie_client *pos;
	u32 state;
	int ret;

	if (!xh2a)
		return -ENODEV;
	if (xh2a->runtime_pm_failed)
		return -EIO;

	ret = xh2a_pcie_check_pwrsts(xh2a, &state);
	if (ret)
		goto failed;
	if (state == XH2A_LPSTS_E2_ENTER_LP) {
		ret = xh2a_pcie_write_bar_msgbit(xh2a,
					       XH2A_LPCTRL_EXIT_IDLE_OR_L1_CODE,
					       XH2A_PCIE_MSG_TO_E2);
		if (ret)
			goto failed;
	}

	ret = xh2a_pcie_wait_power_state(xh2a, XH2A_LPSTS_U7_EXIT_LP, 2000);
	if (ret)
		goto failed;

	/* Firmware must acknowledge wake before clients can access its NoC. */
	ret = xh2a_pcie_probe_post(xh2a);
	if (ret)
		goto failed;
	list_for_each_entry(pos, &xh2a->client_list, node) {
		if (!pos->runtime_resume_cb)
			continue;
		ret = pos->runtime_resume_cb(xh2a);
		if (ret)
			goto failed;
	}

	dev_info(dev, "runtime resumed: firmware and clients ready\n");
	return 0;

failed:
	return xh2a_pcie_runtime_pm_error(xh2a, ret);
}

static irqreturn_t xh2a_pcie_msi_handler_hdma(int irq, void *arg)
{
	struct xh2a_pcie_dev *p_xh2a = (struct xh2a_pcie_dev *)arg;

	unsigned long flags;

	spin_lock_irqsave(&p_xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);
	/* Paired with probe publishing the initialized HDMA work item. */
	if (smp_load_acquire(&p_xh2a->hdma_ready) &&
	    !xh2a_pcie_mmio_blocked(p_xh2a) && !p_xh2a->hdma.teardown)
		schedule_work(&p_xh2a->hdma.hdma_work);
	spin_unlock_irqrestore(&p_xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);
	return IRQ_HANDLED;
}

static irqreturn_t xh2a_pcie_msi_handler(int irq, void *arg)
{
	struct xh2a_pcie_client *pos, *n;
	struct xh2a_pcie_dev *p_xh2a = (struct xh2a_pcie_dev *)arg;
	int i, msi_id = -1, pending_nr = 0;
	struct work_struct *pending[XH2A_PCIE_CLIENT_MAX_WORK_NUM];

	for (i = 0; i < p_xh2a->irqs; i++) {
		if (irq == p_xh2a->msi_irq[i]) {
			msi_id = i;
			break;
		}
	}

	if (msi_id == -1) {
		dev_err(&p_xh2a->pdev->dev, "%s: unknown IRQ %d\n", __func__,
			irq);
		return IRQ_NONE;
	}

	dev_dbg(&p_xh2a->pdev->dev, "%s: got IRQ%d MSI vector%d\n", __func__,
		irq, msi_id);

	spin_lock(&p_xh2a->client_list_lock);
	list_for_each_entry_safe(pos, n, &p_xh2a->client_list, node) {
		for (i = 0; i < pos->work_num; i++) {
			if (pos->work[i].msi_id != msi_id)
				continue;
			if (!pos->work[i].msi_work) {
				dev_err(&p_xh2a->pdev->dev,
					"%s: client %s has null work, skip\n",
					__func__, pos->name);
				continue;
			}
			if (pending_nr < XH2A_PCIE_CLIENT_MAX_WORK_NUM) {
				pending[pending_nr] = pos->work[i].msi_work;
				pending_nr++;
			} else {
				dev_err(&p_xh2a->pdev->dev,
					"%s: MSI %d binded too many works\n",
					__func__, msi_id);
			}
		}
	}
	spin_unlock(&p_xh2a->client_list_lock);

	for (i = 0; i < pending_nr; i++)
		schedule_work(pending[i]);

	return IRQ_HANDLED;
}

static void msi_free(struct xh2a_pcie_dev *p_xh2a, int nirqs)
{
	struct pci_dev *pdev = p_xh2a->pdev;
	struct device *dev = &pdev->dev;
	int i, minirqs;

	minirqs = (nirqs <= p_xh2a->irqs) ? nirqs : p_xh2a->irqs;

	for (i = 0; i < minirqs; i++)
		devm_free_irq(dev, (uint32_t)p_xh2a->msi_irq[i],
			      (void *)p_xh2a);

	pci_free_irq_vectors(pdev);
}

static int parse_interrupt_resource(struct xh2a_pcie_dev *p_xh2a)
{
	struct pci_dev *pdev = p_xh2a->pdev;
	struct device *dev = &pdev->dev;
	int ret, i;

	p_xh2a->irqs = pci_alloc_irq_vectors(pdev, XH2A_PCIE_MSI_NVEC,
					     XH2A_PCIE_MSI_NVEC, PCI_IRQ_MSI);

	if (p_xh2a->irqs < 0) {
		dev_err(dev, "pci_alloc_irq_vectors failed %d\n", p_xh2a->irqs);
		return -1;
	}

	for (i = 0; i < p_xh2a->irqs; i++) {
		p_xh2a->msi_irq[i] = pci_irq_vector(pdev, (uint32_t)i);

		if (p_xh2a->msi_irq[i] < 0) {
			dev_err(dev, "pci_irq_vector failed: %d, %d\n", i,
				p_xh2a->msi_irq[i]);
			pci_free_irq_vectors(pdev);
			return -1;
		}
	}

	ret = xh2a_pcie_msi_init_quirk(p_xh2a);
	if (ret) {
		dev_err(dev, "unsupported M50 MSI mask topology: %d\n", ret);
		pci_free_irq_vectors(pdev);
		return ret;
	}

	/* Force ITS parent masking on disable_irq(), without lazy disabling. */
	for (i = 0; i < p_xh2a->irqs; i++)
		irq_set_status_flags(p_xh2a->msi_irq[i], IRQ_DISABLE_UNLAZY);

	for (i = 0; i < p_xh2a->irqs - 1; i++) {
		ret = devm_request_irq(dev, (uint32_t)p_xh2a->msi_irq[i],
				       xh2a_pcie_msi_handler, 0x0, "xh2a_pcie",
				       (void *)p_xh2a);

		if (ret != 0) {
			dev_err(dev, "devm_request_irq failed: %d, %d\n",
				p_xh2a->msi_irq[i], ret);
			msi_free(p_xh2a, i);
			return -1;
		}
	}

	ret = devm_request_irq(dev,
			       (uint32_t)p_xh2a->msi_irq[XH2A_PCIE_MSI_ID_HDMA],
			       xh2a_pcie_msi_handler_hdma, 0x0, "xh2a_pcie",
			       (void *)p_xh2a);

	if (ret != 0) {
		dev_err(dev, "devm_request_irq failed: %d, %d\n",
			p_xh2a->msi_irq[XH2A_PCIE_MSI_ID_HDMA], ret);
		msi_free(p_xh2a, p_xh2a->irqs - 1);
		return -1;
	}

	if (irq_get_msi_desc(p_xh2a->msi_irq[XH2A_PCIE_MSI_ID_HDMA])) {
		get_cached_msi_msg(p_xh2a->msi_irq[XH2A_PCIE_MSI_ID_HDMA],
				   &p_xh2a->hdma_msi);
		p_xh2a->hdma_msi.data |= XH2A_PCIE_MSI_ID_HDMA;
	}

	return 0;
}

static void mem_unmap(struct xh2a_pcie_dev *p_xh2a)
{
	struct pci_dev *pdev = p_xh2a->pdev;

	pcim_iounmap(pdev, p_xh2a->bar0_mem);
	pcim_iounmap(pdev, p_xh2a->bar2_mem);
	pcim_iounmap(pdev, p_xh2a->trgt0_mem);
}

static int parse_mem_resource(struct xh2a_pcie_dev *p_xh2a)
{
	struct pci_dev *pdev = p_xh2a->pdev;
	struct device *dev = &pdev->dev;

	if (!(pci_resource_flags(pdev, XH2A_PCIE_BAR0) & IORESOURCE_MEM) ||
	    !(pci_resource_flags(pdev, XH2A_PCIE_BAR2) & IORESOURCE_MEM) ||
	    !(pci_resource_flags(pdev, XH2A_PCIE_TRGT0) & IORESOURCE_MEM) ||
	    pci_resource_len(pdev, XH2A_PCIE_TRGT0) <
	    PCIE_HDMA_REGS_OFFSET + (XH2A_PCIE_HDMA_NWRCHAN + XH2A_PCIE_HDMA_NRDCHAN) *
				   PCIE_HDMA_CHAN_BLK_SIZE)
		return -ENODEV;

	p_xh2a->bar0_base = pci_resource_start(pdev, XH2A_PCIE_BAR0);
	p_xh2a->bar0_len = pci_resource_len(pdev, XH2A_PCIE_BAR0);
	p_xh2a->bar0_mem = pcim_iomap(pdev, XH2A_PCIE_BAR0, p_xh2a->bar0_len);

	if (!p_xh2a->bar0_mem) {
		dev_err(dev, "map BAR%d failed\n", XH2A_PCIE_BAR0);
		return -1;
	}

	p_xh2a->bar2_base = pci_resource_start(pdev, XH2A_PCIE_BAR2);
	p_xh2a->bar2_len = pci_resource_len(pdev, XH2A_PCIE_BAR2);
	p_xh2a->bar2_mem = pcim_iomap(pdev, XH2A_PCIE_BAR2, p_xh2a->bar2_len);

	if (!p_xh2a->bar2_mem) {
		dev_err(dev, "map BAR%d failed\n", XH2A_PCIE_BAR2);
		return -1;
	}

	p_xh2a->trgt0_base = pci_resource_start(pdev, XH2A_PCIE_TRGT0);
	p_xh2a->trgt0_len = pci_resource_len(pdev, XH2A_PCIE_TRGT0);
	p_xh2a->trgt0_mem =
		pcim_iomap(pdev, XH2A_PCIE_TRGT0, p_xh2a->trgt0_len);

	if (!p_xh2a->trgt0_mem) {
		dev_err(dev, "map BAR%d failed\n", XH2A_PCIE_TRGT0);
		return -1;
	}

	p_xh2a->membar_lockmap = 0;

	return 0;
}

static inline int xh2a_pcie_get_minor(struct xh2a_pcie_dev *p_xh2a)
{
	int ret;

	mutex_lock(&xh2a_minor_idr_mutex);
	ret = idr_alloc(&xh2a_minor_idr, p_xh2a, 0, XH2A_PCIE_MINOR_ID_MAX,
			GFP_KERNEL);

	if (ret >= 0) {
		p_xh2a->minor = ret;
		ret = 0;
	} else if (ret == -ENOSPC) {
		dev_err(&p_xh2a->pdev->dev, "too many xh2a devices\n");
		ret = -EINVAL;
	}

	mutex_unlock(&xh2a_minor_idr_mutex);
	return ret;
}

static inline void xh2a_pcie_put_minor(struct xh2a_pcie_dev *p_xh2a)
{
	mutex_lock(&xh2a_minor_idr_mutex);
	idr_remove(&xh2a_minor_idr, p_xh2a->minor);
	mutex_unlock(&xh2a_minor_idr_mutex);
}

static int xh2a_pcie_ensure_device_ready(struct xh2a_pcie_dev *p_xh2a)
{
	int ret;
	uint32_t pwrsts;
	unsigned long timeout;
	struct device *dev = &(p_xh2a->pdev->dev);

	ret = xh2a_pcie_check_pwrsts(p_xh2a, &pwrsts);

	if ((ret != 0) || (pwrsts == 0xFFFFFFFF)) {
		dev_err(dev, "%s: pcie read failed\n", __func__);
		return ret;
	}

	if ((pwrsts == XH2A_LPSTS_COLD_BOOT) ||
	    (pwrsts == XH2A_LPSTS_DEVICE_READY))
		return 0;

	if (pwrsts == XH2A_LPSTS_E2_WAIT_HOST) {
		dev_info(dev, "%s: host is hibernated but reboot directly.\n",
			 __func__);

		ret = xh2a_pcie_update_pwrsts(p_xh2a, XH2A_LPSTS_COLD_BOOT);
		if (ret)
			return ret;

		timeout = jiffies + msecs_to_jiffies(5000);
		while (time_before(jiffies, timeout)) {
			ret = xh2a_pcie_check_pwrsts(p_xh2a, &pwrsts);

			if ((ret != 0) || (pwrsts == 0xFFFFFFFF)) {
				dev_err(dev, "%s: pcie read failed\n",
					__func__);
				return ret;
			}

			if (pwrsts == XH2A_LPSTS_DEVICE_READY)
				return 0;

			usleep_range(1000, 1000);
		}
		return -ETIMEDOUT;
	}

	return -EHOSTDOWN;
}

static void xh2a_pcie_release_lifetime(struct kref *ref)
{
	struct xh2a_pcie_dev *xh2a = container_of(ref, struct xh2a_pcie_dev, lifetime);

	pci_dev_put(xh2a->pdev);
	kfree(xh2a);
}

void xh2a_pcie_put_lifetime(struct xh2a_pcie_dev *xh2a)
{
	kref_put(&xh2a->lifetime, xh2a_pcie_release_lifetime);
}

static int xh2a_pcie_probe(struct pci_dev *pdev,
			   const struct pci_device_id *ent)
{
	struct xh2a_pcie_dev *p_xh2a = NULL;
	struct xh2a_ctl_ctx *ctl_ctx;
	struct device *dev = &pdev->dev;
	int ret, i;

	ret = xh2a_ctl_get_ctx(&ctl_ctx);
	if (ret) {
		dev_err(dev, "xh2a_ctl_get_ctx failed %d\n", ret);
		return ret;
	}

	p_xh2a = kzalloc(sizeof(*p_xh2a), GFP_KERNEL);

	if (!p_xh2a) {
		dev_err(dev, "cannot allocate XH2A device\n");
		return -ENOMEM;
	}

	kref_init(&p_xh2a->lifetime);
	p_xh2a->pdev = pci_dev_get(pdev);
	pci_set_drvdata(pdev, p_xh2a);

	ret = xh2a_pcie_get_minor(p_xh2a);

	if (ret) {
		dev_err(dev, "xh2a_pcie_get_minor failed %d\n", ret);
		goto get_minor_err;
	}

	/*
	 * Managed enable: devm registers pci_disable_device on unbind / probe
	 * rollback. Do not call pci_disable_device() manually in remove() or on
	 * error paths, or the core will WARN ("disabling already-disabled
	 * device").
	 */
	ret = pcim_enable_device(pdev);

	if (ret) {
		dev_err(dev, "pcim_enable_device failed %d\n", ret);
		goto enable_device_err;
	}

	ret = pci_request_regions(pdev, "xh2a_pci_region");

	if (ret) {
		dev_err(dev, "pci_request_regions failed %d\n", ret);
		goto request_regions_err;
	}

	ret = parse_mem_resource(p_xh2a);

	if (ret) {
		dev_err(dev, "parse_mem_resource failed %d\n", ret);
		goto mem_parse_err;
	}

	mutex_init(&p_xh2a->pm_irq_lock);

	/* IRQ work uses the endpoint MMIO gate from its first invocation. */
	for (i = 0; i < XH2A_PCIE_ATU_NCHAN; i++)
		spin_lock_init(&p_xh2a->iatu_lock[i]);

	INIT_LIST_HEAD(&p_xh2a->client_list);
	spin_lock_init(&p_xh2a->client_list_lock);

	ret = parse_interrupt_resource(p_xh2a);

	if (ret) {
		dev_err(dev, "parse_interrupt_resource failed %d\n", ret);
		goto interrupt_parse_err;
	}

	ret = hdma_init(&p_xh2a->hdma, p_xh2a->trgt0_mem, &p_xh2a->hdma_msi,
			&p_xh2a->pdev->dev);

	if (ret) {
		dev_err(dev, "hdma_init failed %d\n", ret);
		goto hdma_init_err;
	}

	/* The HDMA interrupt can only schedule work after this publication. */
	smp_store_release(&p_xh2a->hdma_ready, true);
	pci_set_master(pdev);

	ret = xh2a_pcie_ensure_device_ready(p_xh2a);
	if (ret) {
		dev_err(dev, "xh2a_pcie_ensure_device_ready failed %d\n", ret);
		goto notifier_err;
	}

	ret = xh2a_pcie_probe_post(p_xh2a);
	if (ret)
		goto notifier_err;

	dev_info(dev, "%s pcie init done. notify each sub-module\n", __func__);

	xh2a_ctl_ctc_lifecycle_lock(ctl_ctx);
	ret = xh2a_ctl_register_dev(ctl_ctx, p_xh2a->minor, p_xh2a);
	xh2a_ctl_ctc_lifecycle_unlock(ctl_ctx);
	if (ret) {
		dev_err(dev, "xh2a_ctl_register_dev failed %d\n", ret);
		goto notifier_err;
	}

	ret = xh2a_host_call_notifier_chain(XH2A_HOST_NOTIFY_PCIE_PROBE,
					    p_xh2a);

	if (ret == NOTIFY_STOP) {
		dev_err(dev, "%s notify stop\n", __func__);
		xh2a_ctl_ctc_lifecycle_lock(ctl_ctx);
		xh2a_host_call_notifier_chain(XH2A_HOST_NOTIFY_PCIE_REMOVE,
					      p_xh2a);
		xh2a_ctl_destroy_lock(ctl_ctx, p_xh2a->minor);
		xh2a_ctl_ctc_lifecycle_unlock(ctl_ctx);
		goto notifier_err;
	}

	xh2a_ctl_ctc_lifecycle_lock(ctl_ctx);
	ret = xh2a_ctl_ctc_mark_ipu_ready(ctl_ctx, p_xh2a->minor);
	if (ret) {
		dev_err(dev, "mark IPU device ready failed %d\n", ret);
		xh2a_host_call_notifier_chain(XH2A_HOST_NOTIFY_PCIE_REMOVE,
					      p_xh2a);
		xh2a_ctl_destroy_lock(ctl_ctx, p_xh2a->minor);
		xh2a_ctl_ctc_lifecycle_unlock(ctl_ctx);
		goto notifier_err;
	}
	xh2a_ctl_ctc_lifecycle_unlock(ctl_ctx);

	p_xh2a->pm_notifier.notifier_call = xh2a_pcie_pm_notifier;
	register_pm_notifier(&p_xh2a->pm_notifier);

	pm_runtime_use_autosuspend(dev);
	pm_runtime_set_autosuspend_delay(dev, 5000);

	/* force disable d3cold */
	p_xh2a->d3cold_allowed_saved = pdev->d3cold_allowed;
	pdev->d3cold_allowed = 0;

	if (hm_runtime_pm_enable) {
		pm_runtime_allow(dev);
	} else {
		pm_runtime_forbid(dev);
	}

	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_sync(dev);

	return 0;

notifier_err:
	xh2a_pcie_pm_mask_irqs(p_xh2a);
	hdma_deinit(&p_xh2a->hdma);
hdma_init_err:
	msi_free(p_xh2a, XH2A_PCIE_MSI_NVEC);
interrupt_parse_err:
	mem_unmap(p_xh2a);
mem_parse_err:
	pci_clear_master(pdev);
	pci_release_regions(pdev);
request_regions_err:
	/* pci_disable_device: devm cleanup from pcim_enable_device above */
enable_device_err:
	xh2a_pcie_put_minor(p_xh2a);
get_minor_err:
	pci_set_drvdata(pdev, NULL);
	xh2a_pcie_put_lifetime(p_xh2a);
	return ret;
}

static void xh2a_pcie_remove(struct pci_dev *pdev)
{
	int ret;
	struct xh2a_ctl_ctx *ctl_ctx;
	struct xh2a_pcie_dev *p_xh2a = pci_get_drvdata(pdev);
	struct device *dev = &pdev->dev;

	if (!p_xh2a) {
		dev_err(dev, "%s cannot find xh2a device\n", __func__);
		return;
	}

	ret = xh2a_ctl_get_ctx(&ctl_ctx);
	if (ret) {
		dev_err(dev, "xh2a_ctl_get_ctx failed %d\n", ret);
		return;
	}

	unregister_pm_notifier(&p_xh2a->pm_notifier);
	pm_runtime_dont_use_autosuspend(dev);
	xh2a_pcie_pm_put(p_xh2a, false);
	ret = pm_runtime_get_sync(dev);
	if (ret < 0) {
		dev_warn(dev, "runtime resume before remove failed %d\n", ret);
		pm_runtime_put_noidle(dev);
	}

	/* Runtime resume may enter SYS CTC code and take ctc_mutex. */
	xh2a_ctl_ctc_lifecycle_lock(ctl_ctx);
	xh2a_ctl_ctc_mark_dev_removing(ctl_ctx, p_xh2a->minor);
	xh2a_pcie_pm_mask_irqs(p_xh2a);
	ret = xh2a_pcie_hdma_begin_remove(&p_xh2a->hdma);
	if (ret)
		dev_warn(dev, "HDMA stop before client removal failed: %d\n", ret);
	pci_clear_master(pdev);

	pdev->d3cold_allowed = p_xh2a->d3cold_allowed_saved;

	dev_dbg(dev, "%s notify\n", __func__);
	xh2a_host_call_notifier_chain(XH2A_HOST_NOTIFY_PCIE_REMOVE, p_xh2a);

	hdma_deinit(&p_xh2a->hdma);
	msi_free(p_xh2a, XH2A_PCIE_MSI_NVEC);
	mem_unmap(p_xh2a);
	pci_clear_master(pdev);
	pci_release_regions(pdev);
	/* pci_disable_device: devm cleanup from pcim_enable_device (probe) */
	xh2a_pcie_put_minor(p_xh2a);

	pci_set_drvdata(pdev, NULL);
	xh2a_ctl_destroy_lock(ctl_ctx, p_xh2a->minor);
	xh2a_pcie_put_lifetime(p_xh2a);
	xh2a_ctl_ctc_lifecycle_unlock(ctl_ctx);
	dev_info(dev, "%s module remove done.\n", __func__);
}

static void xh2a_pcie_shutdown(struct pci_dev *pdev)
{
	struct xh2a_pcie_dev *p_xh2a = pci_get_drvdata(pdev);
	struct device *dev = &pdev->dev;
	int i;

	if (!p_xh2a) {
		dev_err(dev, "%s cannot find xh2a device\n", __func__);
		return;
	}

	/* runtime resume is called by device_shutdown */

	dev_dbg(dev, "%s notify\n", __func__);
	xh2a_host_call_notifier_chain(XH2A_HOST_NOTIFY_PCIE_SHUTDOWN, p_xh2a);

	/* quiesce the device
	 * step1. disable interrupt;
	 * step2. stop hdma;
	 * step3. enable static key
	 * step4. drain for inflight memcpy
	 */
	for (i = 0; i < XH2A_PCIE_MSI_NVEC; i++)
		disable_irq(p_xh2a->msi_irq[i]);

	xh2a_pcie_hdma_stop(&p_xh2a->hdma);

	atomic_set(&p_xh2a->shutting_down, 1);

	usleep_range(100, 100);
}

static const struct pci_error_handlers xh2a_pcie_err_handler = {
	.error_detected = xh2a_pcie_err_error_detected,
	.slot_reset = xh2a_pcie_err_slot_reset,
	.resume = NULL,
};

static const struct dev_pm_ops xh2a_pcie_pm_ops = {
	.suspend = xh2a_pcie_pm_suspend,
	.resume = xh2a_pcie_pm_resume,
	.freeze = xh2a_pcie_pm_freeze,
	.thaw = xh2a_pcie_pm_thaw,
	.poweroff = xh2a_pcie_pm_poweroff,
	.restore = xh2a_pcie_pm_restore,
	SET_RUNTIME_PM_OPS(xh2a_pcie_pm_runtime_suspend,
			   xh2a_pcie_pm_runtime_resume, NULL)
};

static ssize_t driver_version_show(struct device_driver *drv, char *buf)
{
	return sprintf(buf, "xh2a driver version %s: %s\n",
		       XH2A_HOST_DRIVER_RELEASE, XH2A_HOST_DRIVER_BUILDTIME);
}
static DRIVER_ATTR_RO(driver_version);

static struct attribute *xh2a_pcie_drv_attrs[] = {
	&driver_attr_driver_version.attr,
	NULL,
};
ATTRIBUTE_GROUPS(xh2a_pcie_drv);

static const struct pci_device_id xh2a_pcie_pci_tbl[] = {
	{ PCI_DEVICE(XH2A_PCIE_EP_VID, XH2A_PCIE_EP_DID) },
	{ PCI_DEVICE(XH2A_PCIE_EP_ALT_VID, XH2A_PCIE_EP_ALT_DID) },
	{ 0 },
};
MODULE_DEVICE_TABLE(pci, xh2a_pcie_pci_tbl);

static struct pci_driver xh2a_pcie_driver = {
	.name = "houmo,xh2a",
	.probe = xh2a_pcie_probe,
	.remove = xh2a_pcie_remove,
	.id_table = xh2a_pcie_pci_tbl,
	.driver = {
		.pm = &xh2a_pcie_pm_ops,
	},
	.shutdown = xh2a_pcie_shutdown,
	.err_handler = &xh2a_pcie_err_handler,
	.groups = xh2a_pcie_drv_groups,
};

int __init xh2a_pcie_register_driver(void)
{
	pr_debug("%s\n", __func__);

	return pci_register_driver(&xh2a_pcie_driver);
}

void __exit xh2a_pcie_unregister_driver(void)
{
	pr_debug("%s\n", __func__);

	idr_destroy(&xh2a_minor_idr);

	return pci_unregister_driver(&xh2a_pcie_driver);
}

MODULE_LICENSE("GPL");
