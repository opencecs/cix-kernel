// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2025 Houmo AI Inc. */
#include <linux/module.h>
#include <linux/pci.h>
#include <xh2a_pcie_api.h>
#include "xh2a_pcie.h"

#define XH2A_PCIE_MSI_MAPPING_ADDR 0x60500008ULL

/* Install before request_irq(): standard PCI per-vector mask semantics must
 * not replace the vendor all-zero mask quirk for the 32-source/8-vector map.
 * ITS masks host delivery independently; no other IRQ topology is accepted.
 */
int xh2a_pcie_msi_init_quirk(struct xh2a_pcie_dev *xh2a)
{
#ifdef CONFIG_IRQ_DOMAIN_HIERARCHY
	struct pci_dev *pdev = xh2a->pdev;
	struct msi_desc *desc;
	struct irq_data *data, *parent;
	unsigned long flags;
	int i;

	if ((pdev->vendor != XH2A_PCIE_EP_VID &&
	     pdev->vendor != XH2A_PCIE_EP_ALT_VID) ||
	    pdev->device != XH2A_PCIE_EP_DID || !pdev->msi_enabled ||
	    xh2a->irqs != XH2A_PCIE_MSI_NVEC)
		return -EOPNOTSUPP;
	desc = irq_get_msi_desc(pdev->irq);
	if (!desc || desc->dev != &pdev->dev ||
	    desc->nvec_used != XH2A_PCIE_MSI_NVEC ||
	    desc->pci.msi_attrib.is_msix || !desc->pci.msi_attrib.is_64 ||
	    !desc->pci.msi_attrib.can_mask || desc->pci.msi_attrib.multi_cap != 5)
		return -EOPNOTSUPP;
	for (i = 0; i < xh2a->irqs; i++) {
		data = irq_get_irq_data(xh2a->msi_irq[i]);
		if (!data || irq_data_get_msi_desc(data) != desc ||
		    !data->chip || !data->chip->name ||
		    strcmp(data->chip->name, "ITS-MSI") ||
		    !data->chip->irq_mask || !data->chip->irq_unmask ||
		    irqd_is_forwarded_to_vcpu(data))
			return -EOPNOTSUPP;
		parent = data->parent_data;
		if (!parent || !parent->chip || !parent->chip->name ||
		    strcmp(parent->chip->name, "ITS") ||
		    !parent->chip->irq_mask || !parent->chip->irq_unmask ||
		    irqd_is_forwarded_to_vcpu(parent))
			return -EOPNOTSUPP;
	}
	/* No handlers have been requested yet. Disable only this descriptor's
	 * generic mask writes, including PCI restore and IRQ enable/disable.
	 * Leave the hardware's allocation-time mask until probe_post is ready.
	 */
	raw_spin_lock_irqsave(&pdev->msi_lock, flags);
	desc->pci.msi_attrib.can_mask = false;
	desc->pci.msi_mask = 0;
	xh2a->msi_mask_quirk = true;
	raw_spin_unlock_irqrestore(&pdev->msi_lock, flags);
	dev_info(&pdev->dev, "M50 MSI mask quirk: using ITS parent IRQ masking\n");
	return 0;
#else
	return -EOPNOTSUPP;
#endif
}

int xh2a_pcie_probe_post(struct xh2a_pcie_dev *xh2a)
{
	static const u32 mapping[] = {
		XH2A_PCIE_MSI_MAPPING_3_0, XH2A_PCIE_MSI_MAPPING_7_4,
		XH2A_PCIE_MSI_MAPPING_11_8, XH2A_PCIE_MSI_MAPPING_15_12,
		XH2A_PCIE_MSI_MAPPING_19_16, XH2A_PCIE_MSI_MAPPING_23_20,
		XH2A_PCIE_MSI_MAPPING_27_24, XH2A_PCIE_MSI_MAPPING_31_28,
	};
	struct msi_desc *desc;
	unsigned long flags;
	u32 mask;
	u16 control;
	int cap, ret, i;

	if (READ_ONCE(xh2a->pm_failed) || READ_ONCE(xh2a->reset_pending) ||
	    READ_ONCE(xh2a->pm_mmio_suspended))
		return -ESHUTDOWN;
	cap = pci_find_capability(xh2a->pdev, PCI_CAP_ID_MSI);
	if (!cap)
		return -ENODEV;
	ret = pci_read_config_word(xh2a->pdev, cap + PCI_MSI_FLAGS, &control);
	if (ret)
		return pcibios_err_to_errno(ret);
	if (control == 0xffff)
		return -ENODEV;
	desc = irq_get_msi_desc(xh2a->pdev->irq);
	if (!xh2a->pdev->msi_enabled || !desc || desc->pci.msi_attrib.is_msix)
		return -ENODEV;
	if (!xh2a->msi_mask_quirk || desc->pci.msi_attrib.can_mask ||
	    !(control & PCI_MSI_FLAGS_MASKBIT) || desc->pci.mask_pos !=
	    cap + ((control & PCI_MSI_FLAGS_64BIT) ? PCI_MSI_MASK_64 : PCI_MSI_MASK_32))
		return -EIO;
	/* Preserve the vendor raw-zero quirk. The descriptor cannot write this
	 * register again; disabled Linux IRQs remain masked in the ITS parent.
	 */
	raw_spin_lock_irqsave(&xh2a->pdev->msi_lock, flags);
	ret = pci_write_config_dword(xh2a->pdev, desc->pci.mask_pos, 0);
	if (!ret)
		ret = pci_read_config_dword(xh2a->pdev, desc->pci.mask_pos, &mask);
	if (ret)
		ret = pcibios_err_to_errno(ret);
	else if (mask)
		ret = -EIO;
	raw_spin_unlock_irqrestore(&xh2a->pdev->msi_lock, flags);
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(mapping); i++) {
		ret = xh2a_pcie_pio_writel(xh2a, XH2A_PCIE_MSI_MAPPING_ADDR + i * 4,
					mapping[i]);
		if (ret)
			return ret;
	}
	return 0;
}
