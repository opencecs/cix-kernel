// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2024 Houmo AI Inc.
 * Author: hongxing.ma<hongxing.ma@houmo.ai>
 */

#ifndef _XH2A_PCIE_H_
#define _XH2A_PCIE_H_

#include <linux/kref.h>

#include <linux/pci.h>
#include <linux/msi.h>
#include <linux/irq.h>
#include <linux/kref.h>
#include <linux/mutex.h>
#include <xh2a_pcie_msi.h>
#include "xh2a_pcie_hdma.h"

#define XH2A_PCIE_EP_VID     0x1f6b
#define XH2A_PCIE_EP_DID     0x0c00
#define XH2A_PCIE_EP_ALT_VID 0x1ec8
#define XH2A_PCIE_EP_ALT_DID 0x0c00

#define XH2A_PCIE_BAR0	 (0)
#define XH2A_PCIE_BAR2	 (2)
#define XH2A_PCIE_BAR4	 (4)
#define XH2A_PCIE_TRGT0	 XH2A_PCIE_BAR4
#define XH2A_PCIE_REGBAR XH2A_PCIE_BAR0
#define XH2A_PCIE_MEMBAR XH2A_PCIE_BAR2

#define XH2A_PCIE_ATU_NCHAN (8)

/* iATU */
#define PCIE_ATU_OFFSET		   0x3000
#define PCIE_ATU_UNR_REGION_CTRL1  0x00
#define PCIE_ATU_UNR_REGION_CTRL2  0x04
#define PCIE_ATU_UNR_LOWER_BASE	   0x08
#define PCIE_ATU_UNR_UPPER_BASE	   0x0C
#define PCIE_ATU_UNR_LOWER_LIMIT   0x10
#define PCIE_ATU_UNR_LOWER_TARGET  0x14
#define PCIE_ATU_UNR_UPPER_TARGET  0x18
#define PCIE_ATU_UNR_REGION_CTRL3  0x1C
#define PCIE_ATU_UNR_UPPER_LIMIT   0x20
#define PCIE_ATU_ENABLE		   BIT(31)
#define PCIE_ATU_BAR_MODE_ENABLE   BIT(30)
#define PCIE_ATU_FUNC_NUM_MATCH_EN BIT(19)
#define PCIE_ATU_FUNC_NUM(pf)	   ((pf) << 20)

/* Register address builder */
#define PCIE_GET_ATU_OUTB_UNR_REG_OFFSET(region) ((region) << 9)

#define PCIE_GET_ATU_INB_UNR_REG_OFFSET(region) (((region) << 9) | BIT(8))

#define XH2A_PCIE_IATU_IB_REG(region, reg)     \
	(p_xh2a->trgt0_mem + PCIE_ATU_OFFSET + \
	 PCIE_GET_ATU_INB_UNR_REG_OFFSET(region) + (reg))

/* ELBI */
#define PCIE_ELBI_PWRSTS      0x0fcc
#define PCIE_ELBI_MSG_SET     0x0fd0
#define PCIE_ELBI_MSG_CLR     0x0fd4
#define PCIE_ELBI_MSG_MASK_U7 0x0fd8
#define PCIE_ELBI_MSG_MASK_E2 0x0fdc

/* XH2A Lowpower Status */
#define XH2A_LPSTS_COLD_BOOT	0
#define XH2A_LPSTS_U7_ENTER_LP	1
#define XH2A_LPSTS_E2_ENTER_LP	2
#define XH2A_LPSTS_E2_WAIT_HOST 3
#define XH2A_LPSTS_HOST_READY	4
#define XH2A_LPSTS_E2_EXIT_LP	5
#define XH2A_LPSTS_U7_EXIT_LP	6
#define XH2A_LPSTS_DEVICE_READY XH2A_LPSTS_U7_EXIT_LP

#define XH2A_LPCTRL_START_HIBERNATE_STR "hibernate"
#define XH2A_LPCTRL_START_HIBERNATE_LEN 10
#define XH2A_LPCTRL_START_HIBERNATE_ID	0xED

#define XH2A_LPCTRL_START_SLEEP_STR "sleep"
#define XH2A_LPCTRL_START_SLEEP_LEN 6
#define XH2A_LPCTRL_START_SLEEP_ID  0xED

#define XH2A_LPCTRL_START_IDLE_STR "idle"
#define XH2A_LPCTRL_START_IDLE_LEN 5
#define XH2A_LPCTRL_START_IDLE_ID  0xED

#define XH2A_LPCTRL_EXIT_IDLE_OR_L1_CODE 2
#define XH2A_LPCTRL_EXIT_HIBERNATE_CODE	 3

#define XH2A_LPCTRL_ALT_MODE_REG      0x7000029C
#define XH2A_LPCTRL_ALT_MODE_NORMAL   0x0
#define XH2A_LPCTRL_ALT_MODE_SMBUS    0x1
#define XH2A_LPCTRL_ALT_MODE_FREEZE   0x2
#define XH2A_LPCTRL_ALT_MODE_POWEROFF 0x3

enum xh2a_lpctrl_start_cmd {
	XH2A_LPCTRL_START_SUSPEND,
	XH2A_LPCTRL_START_RESUME,
	XH2A_LPCTRL_START_FREEZE,
	XH2A_LPCTRL_START_THAW,
	XH2A_LPCTRL_START_POWEROFF,
	XH2A_LPCTRL_START_RESTORE,
};

enum xh2a_system_pm_phase {
	XH2A_SYSTEM_PM_IDLE,
	XH2A_SYSTEM_PM_PREPARING,
	XH2A_SYSTEM_PM_PREPARED,
	XH2A_SYSTEM_PM_ENTERING,
	XH2A_SYSTEM_PM_ENTERED,
	XH2A_SYSTEM_PM_RESUMED,
	XH2A_SYSTEM_PM_FAILED,
};

struct xh2a_pcie_dev {
	bool runtime_pm_failed;
	/* pm_failed is an irreversible MMIO gate protected by REGBAR iatu_lock. */
	bool pm_failed;
	bool pm_mmio_suspended;
	/* PM-owned IRQ disable depths; serialized by pm_irq_lock. */
	unsigned long pm_irqs_disabled;
	struct mutex pm_irq_lock;
	bool system_pm_active;
	bool system_pm_ref;
	bool system_pm_compatible;
	enum xh2a_system_pm_phase system_pm_phase;
	int system_pm_error;

	struct pci_dev *pdev;

	/* for trgt0, aka bar4 */
	phys_addr_t trgt0_base;
	resource_size_t trgt0_len;
	void __iomem *trgt0_mem;

	/* for bar0/2 */
	phys_addr_t bar0_base;
	phys_addr_t bar2_base;
	resource_size_t bar0_len;
	resource_size_t bar2_len;
	void __iomem *bar0_mem;
	void __iomem *bar2_mem;

	uint64_t membar_lockmap;

	int irqs;
	int msi_irq[XH2A_PCIE_MSI_NVEC];
	/* M50 vendor mask quirk: only validated ITS supplies IRQ masking. */
	bool msi_mask_quirk;

	spinlock_t iatu_lock[XH2A_PCIE_ATU_NCHAN];

	struct kref lifetime;
	bool hdma_ready;
	struct xh2a_pcie_hdma hdma;
	struct msi_msg hdma_msi;

	spinlock_t client_list_lock;
	struct list_head client_list;

	struct notifier_block pm_notifier;
	int d3cold_allowed_saved;
	u16 system_pm_link_status;

	int minor;

	/* REGBAR iatu_lock is also the endpoint MMIO gate. Once reset is
	 * issued, only a newly probed device may access the endpoint again.
	 */
	bool reset_pending;
	atomic_t shutting_down;
};

/* Caller holds the REGBAR MMIO gate. PM internal traffic is allowed until
 * a terminal failure; users additionally check xh2a_pcie_pm_io_status(). */
static inline bool xh2a_pcie_mmio_blocked(struct xh2a_pcie_dev *xh2a)
{
	return xh2a->reset_pending || xh2a->pm_failed || xh2a->pm_mmio_suspended;
}

void xh2a_pcie_put_lifetime(struct xh2a_pcie_dev *xh2a);
int xh2a_pcie_msi_init_quirk(struct xh2a_pcie_dev *xh2a);
int xh2a_pcie_probe_post(struct xh2a_pcie_dev *p_xh2a);

#endif
