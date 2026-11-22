// SPDX-License-Identifier: GPL-2.0
/*
 * pci-sky1 - PCIe controller driver for CIX's sky1 SoCs
 *
 * Author: Peter Chen <peter.chen@cixtech.com>
 * Author: Shuyu Li <shuyu.li@cixtech.com>
 */
#include <linux/acpi.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_irq.h>
#include <linux/pci.h>
#include <linux/pci-acpi.h>
#include <linux/pci-ecam.h>
#include <linux/pm_runtime.h>
#include <linux/delay.h>
#include <linux/reset.h>
#include <linux/syscore_ops.h>

#include "../../pci.h"
#include "pcie-cadence.h"
#include "pci-sky1.h"
#include "pci-sky1-debugfs.h"

#define APP_SUBOFFSET_STRAP_REG_X2 0x40
#define APP_SUBOFFSET_STRAP_REG_X1A 0x60
#define APP_SUBOFFSET_STRAP_REG_X1B 0x80

#define APP_SUBOFFSET_STATUS_REG_X2 0x40
#define APP_SUBOFFSET_STATUS_REG_X1A 0x60
#define APP_SUBOFFSET_STATUS_REG_X1B 0x80

#define I_CFG_5 (CDNS_PCIE_IP_REG_BANK_BASE + 0x414)
#define I_CFG_7 (CDNS_PCIE_IP_REG_BANK_BASE + 0x41c)
#define I_CFG_9 (CDNS_PCIE_IP_REG_BANK_BASE + 0x724)
#define IP_REG_I_DBG_STS_0 (CDNS_PCIE_IP_REG_BANK_BASE + 0x420)

#define I_HAL_CTRL_DEC_TLP_FILTER (CDNS_PCIE_IP_REG_BANK_BASE + 0x100c)

/* local interrupt */
#define I_LOCAL_ERR_STS_REG0 (CDNS_PCIE_IP_REG_BANK_BASE + 0x1414)
#define I_LOCAL_ERR_MASK_REG0 (CDNS_PCIE_IP_REG_BANK_BASE + 0x1418)
#define I_LOCAL_ERR_STS_REG1 (CDNS_PCIE_IP_REG_BANK_BASE + 0x1448)
#define I_LOCAL_ERR_MASK_REG1 (CDNS_PCIE_IP_REG_BANK_BASE + 0x144c)

#define POWER_STATE_CHANGE_CTRL (CDNS_PCIE_IP_REG_BANK_BASE + 0x1430)

/* indication bit */
#define LTSSM_TRANS_DEBUG_CTRL_REG0 (CDNS_PCIE_IP_REG_BANK_BASE + 0x6cc)
#define LTSSM_TRANS_DEBUG_CTRL_REG1 (CDNS_PCIE_IP_REG_BANK_BASE + 0x6d0)
#define LTSSM_TRANS_DEBUG_CTRL_REG2 (CDNS_PCIE_IP_REG_BANK_BASE + 0x6d4)
#define LTSSM_TRANS_DEBUG_CTRL_REG3 (CDNS_PCIE_IP_REG_BANK_BASE + 0x6d8)
#define LTSSM_TRANS_DEBUG_CTRL_STATUS (CDNS_PCIE_IP_REG_BANK_BASE + 0x6dc)
#define AXI_SLAVE_CTRL (CDNS_PCIE_IP_AXI_SLAVE_BASE + 0x4)
#define AXI_SLAVE_RESP_CTRL_MEM (CDNS_PCIE_IP_AXI_SLAVE_BASE + 0x10c)

#define BF_MAX_LANE_NUM_MASK GENMASK(17, 15)
#define MAX_EVAL_ITER_SHIFT 18
#define BF_MAX_LANE_NUM_SHIFT 15
#define PCIE_LINK_SPEED_SHIFT 12

#define EXT_REFCLK_DETECTED_CFG BIT(26)
#define PMA_CMN_RESCAL_INSEL BIT(15)
#define REFCLK0_TERM_EN BIT(14)
#define PMA_REFCLK_DIG_MASK GENMASK(13, 12)
#define PMA_REFCLK_DIG_DIV 12

#define LINK_TRAINING_ENABLE BIT(0)
#define LINK_COMPLETE BIT(0)

#define SKY1_MAX_LANES 8

#define PERST_DELAY_US 1000

/* local interrupt (aer error) */
#define LOCAL_ERR_PFOVFL BIT(8)
#define LOCAL_ERR_NPFOVFL BIT(9)
#define LOCAL_ERR_CPFOVFL BIT(10)
#define LOCAL_ERR_RETTO BIT(11)
#define LOCAL_ERR_RETRL BIT(12)
#define LOCAL_ERR_PHYER BIT(13)
#define LOCAL_ERR_MLTLRX BIT(14)
#define LOCAL_ERR_UNEXPCPL BIT(15)
#define LOCAL_ERR_FCERR BIT(16)
#define LOCAL_ERR_CPLTO BIT(17)

#define LOCAL_INT_AER (LOCAL_ERR_PFOVFL | LOCAL_ERR_NPFOVFL | LOCAL_ERR_CPFOVFL \
		       | LOCAL_ERR_RETTO | LOCAL_ERR_RETRL | LOCAL_ERR_PHYER | LOCAL_ERR_MLTLRX \
		       | LOCAL_ERR_UNEXPCPL | LOCAL_ERR_FCERR | LOCAL_ERR_CPLTO)
#define LOCAL_ERR_LTSSTTRAN (31)
#define LOCAL_ERR_PME (5)
#define LOCAL_ERR_NFTTO BIT(26)


#define PCIE_LTSSM_DISABLED 32
#define SKY1_PHY_DELAY_US_MIN (200)
#define SKY1_PHY_DELAY_US_MAX (SKY1_PHY_DELAY_US_MIN + 1)
#define SKY1_PHY_TIMEOUT_CNT (50)

#define PCIE_RESET_CONFIG_WAIT_MS	100

/* Pre-EP-power delay on x8 when a display-class GPU is downstream (resume). */
#define SKY1_PCIE_X8_GPU_RESUME_MS	500

static DEFINE_MUTEX(sky1_init_mutex);
static DEFINE_MUTEX(sky1_x211_mutex);
static atomic_t phy_rst_deassert_cnt = ATOMIC_INIT(0);
static atomic_t phy_rst_assert_cnt = ATOMIC_INIT(0);
static atomic_t pwr_en_cnt = ATOMIC_INIT(0);
static atomic_t x211_phy_rst_finish_cnt = ATOMIC_INIT(0);

static LIST_HEAD(sky1_pcie_list);
static unsigned long linkup_delay;

static const struct sky1_pcie_ctrl_desc sky1_pcie_desc[] = {
	{
		.id = PCIE_ID_x8,
		.name = "sky1-pcie-x8",
		.dualmode = true,
		.link_speed = 4,
		.max_lanes = 8,
		.sbsa = 0x1,
	},
	{
		.id = PCIE_ID_x4,
		.name = "sky1-pcie-x4",
		.dualmode = false,
		.link_speed = 4,
		.max_lanes = 4,
		.sbsa = 0x1,
	},
	{
		.id = PCIE_ID_x2,
		.name = "sky1-pcie-x2",
		.dualmode = false,
		.link_speed = 4,
		.max_lanes = 2,
		.sbsa = 0x1,
	},
	{
		.id = PCIE_ID_x1_1,
		.name = "sky1-pcie-x1_1",
		.dualmode = false,
		.link_speed = 4,
		.max_lanes = 1,
		.sbsa = 0x1,
	},
	{
		.id = PCIE_ID_x1_0,
		.name = "sky1-pcie-x1_0",
		.dualmode = false,
		.link_speed = 4,
		.max_lanes = 1,
		.sbsa = 0x1,
	},
	{}
};

u32 sky1_pcie_get_bits32(void __iomem *addr, u32 bits_msk)
{
	u32 value = 0;

	value = readl(addr);
	value &= bits_msk;
	return value;
}

static void sky1_pcie_set_bits32(void __iomem *addr, u32 bits_msk)
{
	u32 value;

	value = readl(addr);
	value |= bits_msk;
	writel(value, addr);
}

static void sky1_pcie_clear_bits32(void __iomem *addr, u32 bits_msk)
{
	u32 value;

	value = readl(addr);
	value &= ~bits_msk;
	writel(value, addr);
}

static void sky1_pcie_update_bits32(void __iomem *addr, u32 bits_msk, u32 vals)
{
	u32 value;

	value = readl(addr);
	value &= ~bits_msk;
	value |= vals;
	writel(value, addr);
}

static bool sky1_pcie_is_x211(struct sky1_pcie *pcie)
{
	return ((pcie->desc->id == PCIE_ID_x2) ||
		(pcie->desc->id == PCIE_ID_x1_1) ||
		(pcie->desc->id == PCIE_ID_x1_0)) ?
		       true :
		       false;
}

static void sky1_pcie_clear_x211_cnt(void)
{
	atomic_set(&phy_rst_deassert_cnt, 0);
	atomic_set(&phy_rst_assert_cnt, 0);
	atomic_set(&pwr_en_cnt, 0);
}

static void sky1_pcie_enable_local_irq(struct sky1_pcie *pcie, bool en)
{
	void __iomem *reg_base;
	u32 enanle_err_0 = 0;
	u32 enanle_err_1 = 0;

	reg_base = pcie->reg_base + CDNS_PCIE_RP_BASE;
	if (en) {
		/* mask aer error local interrupt */
		enanle_err_0 |= LOCAL_INT_AER;
		enanle_err_0 |= LOCAL_ERR_NFTTO;
		writel(enanle_err_0, reg_base + I_LOCAL_ERR_MASK_REG0);
		writel(enanle_err_1, reg_base + I_LOCAL_ERR_MASK_REG1);
	} else {
		writel(~enanle_err_0, reg_base + I_LOCAL_ERR_MASK_REG0);
		writel(~enanle_err_1, reg_base + I_LOCAL_ERR_MASK_REG1);
	}
}

static u32 sky1_pcie_ctrl_readl_reg(struct sky1_pcie *pcie, u32 reg)
{
	return readl(pcie->reg_base + reg);
}

static void sky1_pcie_ctrl_writel_reg(struct sky1_pcie *pcie, u32 reg, u32 val)
{
	writel(val, pcie->reg_base + reg);
}

void sky1_pcie_enable_indicatbit_int(struct sky1_pcie *pcie, bool en)
{
	u32 reg = 0;

	reg |= 57 << 24; /* HOT_RESET_1 */
	reg |= 70 << 16; /* TX_ELEC_IDLE_ST */
	reg |= 50 << 8; /* DISABLE_LINK_2 */
	reg |= 51; /* DISABLE_LINK_3 */
	sky1_pcie_ctrl_writel_reg(pcie, LTSSM_TRANS_DEBUG_CTRL_REG0, reg);

	reg = 0;
	reg |= 72 << 24; /* TX_ELEC_IDLE_2 */
	reg |= 1 << 16; /* DETECT_QUIET_ENTRY */
	reg |= 54 << 8; /* DISABLE_LINK_6 */
	reg |= 1; /* DETECT_QUIET_ENTRY */
	sky1_pcie_ctrl_writel_reg(pcie, LTSSM_TRANS_DEBUG_CTRL_REG1, reg);

	reg = 0;
	reg |= 110 << 24; /* l2_IDLE */
	reg |= 1 << 16; /* DETECT_QUIET_ENTRY */
	reg |= 73 << 8; /* TX_ELEC_IDLE_3 */
	reg |= 1; /* DETECT_QUIET_ENTRY */
	sky1_pcie_ctrl_writel_reg(pcie, LTSSM_TRANS_DEBUG_CTRL_REG2, reg);

	reg = 0;
	reg |= 56 << 8; /* HOT_RESET */
	reg |= 57; /* HOT_RESET_1 */
	sky1_pcie_ctrl_writel_reg(pcie, LTSSM_TRANS_DEBUG_CTRL_REG3, reg);

	/* enable/disable channel 0 ~ 7 */
	if (en) {
		reg = sky1_pcie_ctrl_readl_reg(pcie,
					       LTSSM_TRANS_DEBUG_CTRL_STATUS);
		reg |= 0xff << 24;
		sky1_pcie_ctrl_writel_reg(pcie, LTSSM_TRANS_DEBUG_CTRL_STATUS,
					  reg);
	} else {
		reg = sky1_pcie_ctrl_readl_reg(pcie,
					       LTSSM_TRANS_DEBUG_CTRL_STATUS);
		reg &= ~(0xff << 24);
		sky1_pcie_ctrl_writel_reg(pcie, LTSSM_TRANS_DEBUG_CTRL_STATUS,
					  reg);
	}
}

/* Display controller / GPU (VGA, 3D, etc.); not only PCI_CLASS_DISPLAY_VGA. */
static bool sky1_pcie_pci_class_is_display(unsigned int class_rev)
{
	return ((class_rev >> 16) & 0xff) == PCI_BASE_CLASS_DISPLAY;
}

static bool sky1_pcie_bus_subtree_has_display(struct pci_bus *bus)
{
	struct pci_dev *pdev;
	struct pci_bus *child;

	if (!bus)
		return false;

	list_for_each_entry(pdev, &bus->devices, bus_list) {
		if (sky1_pcie_pci_class_is_display(pdev->class))
			return true;
		if (pdev->subordinate &&
		    sky1_pcie_bus_subtree_has_display(pdev->subordinate))
			return true;
	}

	list_for_each_entry(child, &bus->children, node) {
		if (sky1_pcie_bus_subtree_has_display(child))
			return true;
	}

	return false;
}

/*
 * True if any display-class device exists under this host bridge root bus
 * (full subtree: root port subordinate, switches, any slot).
 */
static bool sky1_pcie_downstream_has_display(struct sky1_pcie *pcie)
{
	struct pci_host_bridge *bridge;

	bridge = pci_host_bridge_from_priv(pcie->cdns_pcie_rc);
	if (!bridge || !bridge->bus)
		return false;

	return sky1_pcie_bus_subtree_has_display(bridge->bus);
}

/*
 * Arm x8_vga_resume_delay for PCIE_ID_x8 when a display device exists under
 * this RC. Call from suspend_noirq before link teardown.
 */
static void sky1_pcie_refresh_x8_gpu_resume_delay(struct sky1_pcie *pcie)
{
	pcie->x8_vga_resume_delay = false;

	if (pcie->desc->id != PCIE_ID_x8)
		return;

	pcie->x8_vga_resume_delay = sky1_pcie_downstream_has_display(pcie);
}

static struct pci_dev *sky1_pcie_get_ep_pci_dev(struct sky1_pcie *pcie)
{
	struct pci_host_bridge *bridge;
	struct pci_bus *child, *root_bus = NULL;
	struct pci_dev *pdev;
	struct pci_dev *ep_pdev = NULL;
	struct device *dev = pcie->dev;
	u16 vid = 0, pid = 0;

	bridge = pci_host_bridge_from_priv(pcie->cdns_pcie_rc);
	if (!bridge->bus)
		return NULL;

	list_for_each_entry(child, &bridge->bus->children, node) {
		if (child->parent == bridge->bus) {
			root_bus = child;
			break;
		}
	}

	if (!root_bus)
		return NULL;

	list_for_each_entry(pdev, &root_bus->devices, bus_list) {
		if (PCI_SLOT(pdev->devfn) == 0) {
			pci_read_config_word(pdev, PCI_VENDOR_ID, &vid);
			pci_read_config_word(pdev, PCI_DEVICE_ID, &pid);
			dev_info(dev, "ep vid = 0x%04x, pid = 0x%04x\n", vid, pid);
			ep_pdev = pdev;
		}
	}

	return ep_pdev;
}

static void sky1_pcie_enable_pmpme_ep(struct sky1_pcie *pcie, bool en)
{
	struct pci_dev *ep_pdev;
	struct device *dev = pcie->dev;
	int pmcsr_pos;
	u16 pmcsr;

	ep_pdev = sky1_pcie_get_ep_pci_dev(pcie);
	if (!ep_pdev) {
		dev_err(dev, "can't find ep pci_dev\n");
		return;
	}

	if (!ep_pdev->pm_cap)
		return;

	pmcsr_pos = ep_pdev->pm_cap + PCI_PM_CTRL;
	pci_read_config_word(ep_pdev, pmcsr_pos, &pmcsr);

	/* Clear PME status. */
	pmcsr |= PCI_PM_CTRL_PME_STATUS;
	if (en)
		pmcsr |= PCI_PM_CTRL_PME_ENABLE;
	else
		pmcsr &= ~PCI_PM_CTRL_PME_ENABLE;

	pci_write_config_word(ep_pdev, pmcsr_pos, pmcsr);
}

static void sky1_pcie_enable_pmpme_rc(struct sky1_pcie *pcie, bool en)
{
	struct device *dev = pcie->dev;
	u32 reg;
	u8 offset;

	offset = cdns_pcie_find_capability(pcie->reg_base, PCI_CAP_ID_PM);
	dev_info(dev, "PCI_CAP_ID_PM offset = 0x%x\n", offset);
	reg = sky1_pcie_ctrl_readl_reg(pcie, offset + PCI_PM_CTRL);
	/* Clear PME status. */
	reg |= PCI_PM_CTRL_PME_STATUS;
	if (en)
		reg |= PCI_PM_CTRL_PME_ENABLE;
	else
		reg &= ~PCI_PM_CTRL_PME_ENABLE;
	sky1_pcie_ctrl_writel_reg(pcie, offset + PCI_PM_CTRL, reg);
}

static void sky1_pcie_pme_work_fn(struct work_struct *work)
{
	struct sky1_pcie *pcie =
			container_of(work, struct sky1_pcie, pme_work);
	struct device *dev = pcie->dev;
	u32 reg;
	u8 offset;

	mutex_lock(&pcie->pme_mutex);
	offset = cdns_pcie_find_capability(pcie->reg_base, PCI_CAP_ID_EXP);
	dev_info(dev, "PCI_CAP_ID_EXP offset = 0x%x\n", offset);

	/* disable pme interrupt */
	reg = sky1_pcie_ctrl_readl_reg(pcie, offset + PCI_EXP_RTCTL);
	reg &= ~PCI_EXP_RTCTL_PMEIE;
	sky1_pcie_ctrl_writel_reg(pcie, offset + PCI_EXP_RTCTL, reg);

	reg = sky1_pcie_ctrl_readl_reg(pcie, offset + PCI_EXP_RTSTA);
	if (PCI_POSSIBLE_ERROR(reg))
		goto pme_out;

	if (reg & PCI_EXP_RTSTA_PME) {
		u8 busnr, devfn;

		/* Clear PME status of the port. */
		reg &= ~PCI_EXP_RTSTA_PME;
		sky1_pcie_ctrl_writel_reg(pcie, offset + PCI_EXP_RTSTA, reg);

		sky1_pcie_enable_pmpme_ep(pcie, false);
		sky1_pcie_enable_pmpme_rc(pcie, false);
		reg &= 0xffff;
		busnr = reg >> 8;
		devfn = reg & 0xff;
		dev_info(dev, "PME request id: %02x:%02x.%d\n", busnr,
			 PCI_SLOT(devfn), PCI_FUNC(devfn));
	}

	/* enable pme interrupt */
	reg = sky1_pcie_ctrl_readl_reg(pcie, offset + PCI_EXP_RTCTL);
	reg |= PCI_EXP_RTCTL_PMEIE;
	sky1_pcie_ctrl_writel_reg(pcie, offset + PCI_EXP_RTCTL, reg);

	/* enable pm pme interrupt */
	sky1_pcie_enable_pmpme_rc(pcie, true);
	sky1_pcie_enable_pmpme_ep(pcie, true);

pme_out:
	mutex_unlock(&pcie->pme_mutex);
}

static void sky1_pcie_clear_indication_bit(struct sky1_pcie *pcie)
{
	struct device *dev = pcie->dev;
	u32 val;

	val = cdns_pcie_readl(pcie->cdns_pcie, AXI_SLAVE_CTRL);
	dev_info(dev, "AXI_SLAVE_CTRL = 0x%08x\n", val);
	val &= ~BIT(0);
	cdns_pcie_writel(pcie->cdns_pcie, AXI_SLAVE_CTRL, val);
	val = cdns_pcie_readl(pcie->cdns_pcie, AXI_SLAVE_CTRL);
	dev_info(dev, "AXI_SLAVE_CTRL = 0x%08x\n", val);
}

static void sky1_pcie_ltssttran_clear(struct sky1_pcie *pcie)
{
	u32 val, ltssttran;
	u8 i;

	sky1_pcie_clear_indication_bit(pcie);
	/* clear ltssm changed status */
	val = sky1_pcie_ctrl_readl_reg(pcie, LTSSM_TRANS_DEBUG_CTRL_STATUS);
	dev_info(pcie->dev, "LTSSM_TRANS_DEBUG_CTRL_STATUS = 0x%08x\n", val);
	ltssttran = val;
	val |= 0xff;
	sky1_pcie_ctrl_writel_reg(pcie, LTSSM_TRANS_DEBUG_CTRL_STATUS, val);

	ltssttran &= 0xff;
	for (i = 0; i < LTSSTTRAN_MSG_LEN; i++) {
		if (ltssttran & BIT(i))
			dev_err(pcie->dev, "ltssttran changed: %s\n",
				sky1_ltssttran_msg(i));
	}
}

static irqreturn_t sky1_pcie_local_irq_handler(int irq, void *arg)
{
	struct sky1_pcie *pcie = arg;
	void __iomem *reg_base;
	int i = 0;
	u32 sta;

	reg_base = pcie->reg_base + CDNS_PCIE_RP_BASE;
	sta = readl(reg_base + I_LOCAL_ERR_STS_REG0);
	dev_info(pcie->dev, "line = %d sta = 0x%08x \n", __LINE__, sta);
	for (i = 0; i < 32; i++) {
		if (sta & BIT(i)) {
			dev_err(pcie->dev, "local err0 int: %s happend\n",
				sky1_local_err_msg(PCIE_LOCAL_ERR0, i));
			/* ltssm changed */
			if (i == LOCAL_ERR_LTSSTTRAN)
				sky1_pcie_ltssttran_clear(pcie);
		}
	}
	// clear interrupt
	writel(sta, reg_base + I_LOCAL_ERR_STS_REG0);

	sta = readl(reg_base + I_LOCAL_ERR_STS_REG1);
	dev_info(pcie->dev, "line = %d sta = 0x%08x \n", __LINE__, sta);
	for (i = 0; i < 32; i++) {
		if (sta & BIT(i)) {
			dev_err(pcie->dev, "local err1 int: %s happend\n",
				sky1_local_err_msg(PCIE_LOCAL_ERR1, i));
			/* received PME msg */
			if (i == LOCAL_ERR_PME)
				schedule_work(&pcie->pme_work);
		}
	}
	// clear interrupt
	writel(sta, reg_base + I_LOCAL_ERR_STS_REG1);

	return IRQ_HANDLED;
}

static void sky1_pcie_set_link_disable(struct sky1_pcie *pcie);

struct sky1_aer_event {
	struct sky1_pcie *pcie;
	bool uncorrectable;
	bool log_correctable;
};

static bool sky1_aer_log_event(struct sky1_pcie *pcie, u32 status)
{
	const u32 ce_bits = PCI_ERR_ROOT_COR_RCV | PCI_ERR_ROOT_MULTI_COR_RCV;
	unsigned long now = jiffies;

	/* Events with an unknown or uncorrectable status remain visible. */
	if (!pcie->aer_log_ce_ratelimited || !(status & PCI_ERR_ROOT_COR_RCV) ||
	    (status & ~ce_bits))
		return true;

	if (pcie->aer_ce_last_log &&
	    time_before(now, pcie->aer_ce_last_log + 5 * HZ)) {
		pcie->aer_ce_suppressed++;
		return false;
	}

	if (pcie->aer_ce_suppressed) {
		dev_info(pcie->dev, "AER: %u repeated correctable interrupts not printed\n",
			 pcie->aer_ce_suppressed);
		pcie->aer_ce_suppressed = 0;
	}
	pcie->aer_ce_last_log = now ?: 1;
	return true;
}

static void sky1_pcie_aer_root_command(struct sky1_pcie *pcie, bool enable)
{
	void __iomem *base = pcie->reg_base + CDNS_PCIE_RP_BASE;
	u16 aer = cdns_pcie_find_ext_capability(base, PCI_EXT_CAP_ID_ERR);
	u32 command;

	if (!aer)
		return;
	command = readl(base + aer + PCI_ERR_ROOT_COMMAND);
	command &= ~(PCI_ERR_ROOT_CMD_COR_EN | PCI_ERR_ROOT_CMD_NONFATAL_EN |
		     PCI_ERR_ROOT_CMD_FATAL_EN);
	if (enable)
		command |= PCI_ERR_ROOT_CMD_COR_EN | PCI_ERR_ROOT_CMD_NONFATAL_EN |
			   PCI_ERR_ROOT_CMD_FATAL_EN;
	writel(command, base + aer + PCI_ERR_ROOT_COMMAND);
}

static void sky1_pcie_handle_rp_aer_irq(struct sky1_aer_event *event)
{
	struct sky1_pcie *pcie = event->pcie;
	struct sky1_aer_stats *stats = &pcie->aer_stats;
	void __iomem *base = pcie->reg_base + CDNS_PCIE_RP_BASE;
	u16 aer = cdns_pcie_find_ext_capability(base, PCI_EXT_CAP_ID_ERR);
	u16 cap = cdns_pcie_find_capability(base, PCI_CAP_ID_EXP);
	u32 ce, ue, mask, severity;

	if (!aer || !cap)
		return;
	ue = readl(base + aer + PCI_ERR_UNCOR_STATUS);
	mask = readl(base + aer + PCI_ERR_UNCOR_MASK);
	ue &= ~mask;
	ce = readl(base + aer + PCI_ERR_COR_STATUS);
	ce &= ~readl(base + aer + PCI_ERR_COR_MASK);
	severity = readl(base + aer + PCI_ERR_UNCOR_SEVER);
	if (ue) {
		dev_err(pcie->dev,
			"RC AER UESta=%08x mask=%08x severity=%08x header=%08x %08x %08x %08x\n",
			ue, mask, severity,
			readl(base + aer + PCI_ERR_HEADER_LOG),
			readl(base + aer + PCI_ERR_HEADER_LOG + 4),
			readl(base + aer + PCI_ERR_HEADER_LOG + 8),
			readl(base + aer + PCI_ERR_HEADER_LOG + 12));
		event->uncorrectable = true;
		pcie->aer_uncor_dump = true;
		if (ue & severity)
			stats->rp_total_fatal_errs++;
		if (ue & ~severity)
			stats->rp_total_nonfatal_errs++;
		/* Preserve UE evidence until panic or explicit isolation. */
	}
	if (ce) {
		if (event->log_correctable)
			dev_info(pcie->dev, "RC AER CESta=%08x\n", ce);
		stats->rp_total_cor_errs++;
		writel(ce, base + aer + PCI_ERR_COR_STATUS);
		/* DevSta is W1C: never rewrite DevCtl's MPS/MRRS here. */
		writew(PCI_EXP_DEVSTA_CED, base + cap + PCI_EXP_DEVSTA);
	}
}

static int sky1_pcie_scan_aer(struct pci_dev *pdev, void *data)
{
	struct sky1_aer_event *event = data;
	struct sky1_aer_stats *stats = &event->pcie->aer_stats;
	u16 aer = pdev->aer_cap;
	u32 ce, ue, cmask, umask, severity, header[4];
	int i;

	/* The root's own status is read through the controller aperture. */
	if (pci_pcie_type(pdev) == PCI_EXP_TYPE_ROOT_PORT || !aer)
		return 0;
	if (pci_read_config_dword(pdev, aer + PCI_ERR_UNCOR_STATUS, &ue) ||
	    pci_read_config_dword(pdev, aer + PCI_ERR_COR_STATUS, &ce) ||
	    pci_read_config_dword(pdev, aer + PCI_ERR_UNCOR_MASK, &umask) ||
	    pci_read_config_dword(pdev, aer + PCI_ERR_COR_MASK, &cmask) ||
	    ue == ~0U || ce == ~0U) {
		pci_warn(pdev, "cannot read AER status during subtree scan\n");
		return 0;
	}
	ue &= ~umask;
	ce &= ~cmask;
	if (!ue && !ce)
		return 0;

	if (ue || event->log_correctable)
		pci_info(pdev, "Sky1 AER UESta=%08x CESta=%08x UEMsk=%08x CEMsk=%08x\n",
			 ue, ce, umask, cmask);
	if (ue) {
		severity = 0;
		pci_read_config_dword(pdev, aer + PCI_ERR_UNCOR_SEVER, &severity);
		for (i = 0; i < ARRAY_SIZE(header); i++) {
			header[i] = ~0U;
			pci_read_config_dword(pdev, aer + PCI_ERR_HEADER_LOG + i * 4,
					      &header[i]);
		}
		pci_err(pdev, "AER severity=%08x header=%08x %08x %08x %08x\n",
			severity, header[0], header[1], header[2], header[3]);
		event->uncorrectable = true;
		if (ue & severity)
			stats->ep_total_fatal_errs++;
		if (ue & ~severity)
			stats->ep_total_nonfatal_errs++;
	}
	if (ce) {
		stats->ep_total_cor_errs++;
		pci_write_config_dword(pdev, aer + PCI_ERR_COR_STATUS, ce);
		pcie_capability_write_word(pdev, PCI_EXP_DEVSTA, PCI_EXP_DEVSTA_CED);
	}
	return 0;
}

static int sky1_pcie_isolate_device(struct pci_dev *pdev, void *unused)
{
	/* Disable DMA before marking config space inaccessible. */
	pci_clear_master(pdev);
	pci_dev_set_disconnected(pdev, NULL);
	return 0;
}

static irqreturn_t sky1_pcie_aer_irq_handler(int irq, void *arg)
{
	struct sky1_pcie *pcie = arg;
	struct pci_host_bridge *bridge =
		pci_host_bridge_from_priv(pcie->cdns_pcie_rc);
	struct sky1_aer_event event = { .pcie = pcie };
	void __iomem *base = pcie->reg_base + CDNS_PCIE_RP_BASE;
	u16 aer;
	u32 status, source;

	/* All AER lines share this owner, including multi-source events. */
	mutex_lock(&pcie->aer_lock);
	if (pcie->aer_failed) {
		mutex_unlock(&pcie->aer_lock);
		return IRQ_HANDLED;
	}
	aer = cdns_pcie_find_ext_capability(base, PCI_EXT_CAP_ID_ERR);
	if (!aer) {
		mutex_unlock(&pcie->aer_lock);
		return IRQ_NONE;
	}
	status = readl(base + aer + PCI_ERR_ROOT_STATUS);
	source = readl(base + aer + PCI_ERR_ROOT_ERR_SRC);
	/*
	 * Snapshot and acknowledge the root first so a new arrival can latch.
	 * Do not clear endpoint UE evidence; scan every device, even for a bad
	 * source ID, MULTI bits, or an inaccessible reporting device.
	 */
	writel(status, base + aer + PCI_ERR_ROOT_STATUS);
	event.uncorrectable = status &
		(PCI_ERR_ROOT_UNCOR_RCV | PCI_ERR_ROOT_MULTI_UNCOR_RCV);
	event.log_correctable = sky1_aer_log_event(pcie, status);
	if (event.log_correctable)
		dev_info(pcie->dev, "root err status=%08x source=%08x\n", status, source);
	sky1_pcie_handle_rp_aer_irq(&event);
	if (bridge->bus)
		pci_walk_bus(bridge->bus, sky1_pcie_scan_aer, &event);

	if (event.uncorrectable) {
		if (!event.log_correctable)
			dev_err(pcie->dev, "root err status=%08x source=%08x\n", status, source);
		sky1_pcie_aer_root_command(pcie, false);
		if (pcie->is_aer_uncor_panic)
			panic("sky1: PCIe uncorrectable error on %s, status=%08x source=%08x\n",
			      dev_name(pcie->dev), status, source);
		/*
		 * M50 has no complete reset/recovery callback.  Fail closed;
		 * clearing status and continuing would falsely imply recovery.
		 */
		if (bridge->bus)
			pci_walk_bus(bridge->bus, sky1_pcie_isolate_device, NULL);
		pcie->aer_failed = true;
		/* Preserve UE latches without letting dedicated lines storm. */
		disable_irq_nosync(pcie->aer_c_irq);
		disable_irq_nosync(pcie->aer_f_irq);
		disable_irq_nosync(pcie->aer_nf_irq);
		sky1_pcie_set_link_disable(pcie);
		dev_crit(pcie->dev,
			 "PCIe fabric isolated after uncorrectable error; reboot required\n");
	}
	mutex_unlock(&pcie->aer_lock);
	return IRQ_HANDLED;
}

static void sky1_pcie_free_irq(struct sky1_pcie *pcie)
{
	if (!pcie->irqs_requested)
		return;
	sky1_pcie_aer_root_command(pcie, false);
	devm_free_irq(pcie->dev, pcie->aer_nf_irq, pcie);
	devm_free_irq(pcie->dev, pcie->aer_f_irq, pcie);
	devm_free_irq(pcie->dev, pcie->aer_c_irq, pcie);
	devm_free_irq(pcie->dev, pcie->local_irq, pcie);
	cancel_work_sync(&pcie->pme_work);
	pcie->irqs_requested = false;
}

static int sky1_pcie_request_irq(struct sky1_pcie *pcie)
{
	int ret;

	sky1_pcie_aer_root_command(pcie, false);
	ret = devm_request_irq(pcie->dev, pcie->local_irq,
			       sky1_pcie_local_irq_handler, IRQF_SHARED,
			       "pcie-local", pcie);
	if (ret)
		return ret;
	ret = devm_request_threaded_irq(pcie->dev, pcie->aer_c_irq, NULL,
					sky1_pcie_aer_irq_handler, IRQF_ONESHOT,
					"pcie-aer-c", pcie);
	if (ret)
		goto free_local;
	ret = devm_request_threaded_irq(pcie->dev, pcie->aer_f_irq, NULL,
					sky1_pcie_aer_irq_handler, IRQF_ONESHOT,
					"pcie-aer-f", pcie);
	if (ret)
		goto free_cor;
	ret = devm_request_threaded_irq(pcie->dev, pcie->aer_nf_irq, NULL,
					sky1_pcie_aer_irq_handler, IRQF_ONESHOT,
					"pcie-aer-nf", pcie);
	if (ret)
		goto free_fatal;
	pcie->irqs_requested = true;
	return 0;

free_fatal:
	devm_free_irq(pcie->dev, pcie->aer_f_irq, pcie);
free_cor:
	devm_free_irq(pcie->dev, pcie->aer_c_irq, pcie);
free_local:
	devm_free_irq(pcie->dev, pcie->local_irq, pcie);
	cancel_work_sync(&pcie->pme_work);
	return ret;
}

static void sky1_pcie_disable_irq(struct sky1_pcie *pcie)
{
	disable_irq(pcie->local_irq);
	disable_irq(pcie->aer_c_irq);
	disable_irq(pcie->aer_f_irq);
	disable_irq(pcie->aer_nf_irq);
}

static void sky1_pcie_enable_irq(struct sky1_pcie *pcie, bool en)
{
	/* local (some error status) */
	sky1_pcie_enable_local_irq(pcie, en);
	sky1_pcie_enable_indicatbit_int(pcie, en);
}

static int sky1_pcie_get_max_lane_count(struct sky1_pcie *pcie)
{
	int ret = 0;

	switch (pcie->num_lanes) {
	case 8:
		ret = 3;
		break;
	case 4:
		ret = 2;
		break;
	case 2:
		ret = 1;
		break;
	case 1:
		ret = 0;
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static void sky1_pcie_ctrl_set_work_mode(struct sky1_pcie *pcie)
{
	struct device *dev = pcie->dev;
	int mode = 0;

	if ((pcie->desc->dualmode == false) || (pcie->desc->id != PCIE_ID_x8))
		return;

	mode = (pcie->mode == PCI_MODE_RC) ? 1 : 0;
	/* Set as RC or EP */
	sky1_pcie_update_bits32(pcie->strap_base + STRAP_REG(4), 0x2,
				mode << 1);

	dev_dbg(dev, "%s Set work mode as: %s\n", __func__,
		mode ? "RC" : "EP");
}

static int sky1_pcie_ctrl_set_axi_clk_en(struct sky1_pcie *pcie, bool en)
{
	struct device *dev = pcie->dev;
	int ret = 0;

	mutex_lock(&sky1_init_mutex);
	if (en) {
		ret = clk_prepare_enable(pcie->pcie_axi_clk);
		if (ret) {
			mutex_unlock(&sky1_init_mutex);
			dev_err(dev, "unable to enable pcie axi clock\n");
			return ret;
		}
	} else {
		clk_disable_unprepare(pcie->pcie_axi_clk);
		ret = 0;
	}
	mutex_unlock(&sky1_init_mutex);

	dev_dbg(dev, "%s Set axl clk %s\n", __func__,
		((en) ? "enable" : "disable"));

	return ret;
}

static int sky1_pcie_ctrl_set_apb_clk_en(struct sky1_pcie *pcie, bool en)
{
	struct device *dev = pcie->dev;
	int ret = 0;

	mutex_lock(&sky1_init_mutex);
	if (en) {
		ret = clk_prepare_enable(pcie->pcie_apb_clk);
		if (ret) {
			mutex_unlock(&sky1_init_mutex);
			dev_err(dev, "unable to enable pcie axi clock\n");
			return ret;
		}
	} else {
		clk_disable_unprepare(pcie->pcie_apb_clk);
		ret = 0;
	}
	mutex_unlock(&sky1_init_mutex);

	dev_dbg(dev, "%s Set axl clk %s\n", __func__,
		((en) ? "enable" : "disable"));
	dev_dbg(dev, "Set STRAP_REG(6) as :0x%08x\n",
		readl(pcie->strap_base + STRAP_REG(6)));

	return ret;
}

static int sky1_pcie_ctrl_set_refclk_b_en(struct sky1_pcie *pcie, bool en)
{
	struct device *dev = pcie->dev;
	int ret = 0;

	mutex_lock(&sky1_init_mutex);
	if (en) {
		ret = clk_prepare_enable(pcie->pcie_refclk_b);
		if (ret) {
			mutex_unlock(&sky1_init_mutex);
			dev_err(dev, "unable to enable pcie refclk_b\n");
			return ret;
		}
	} else {
		clk_disable_unprepare(pcie->pcie_refclk_b);
	}
	mutex_unlock(&sky1_init_mutex);

	return ret;
}

static void sky1_pcie_clear_macro_pwr_en(struct sky1_pcie *pcie)
{
	static atomic_t x211_macro_pwr_en_cnt = ATOMIC_INIT(0);
	void __iomem *strap_addr;

	switch (pcie->desc->id) {
	case PCIE_ID_x8:
	case PCIE_ID_x4:
		strap_addr = pcie->strap_base + STRAP_REG(8);
		sky1_pcie_clear_bits32(strap_addr, BIT(8));
		break;
	case PCIE_ID_x2:
	case PCIE_ID_x1_0:
	case PCIE_ID_x1_1:
		strap_addr = pcie->rcsu_base + APP_OFFSET_STRAP_REG + STRAP_REG(40);
		mutex_lock(&sky1_init_mutex);
		if (atomic_read(&x211_macro_pwr_en_cnt) == 0) {
			sky1_pcie_clear_bits32(strap_addr, BIT(8));
			atomic_inc(&x211_macro_pwr_en_cnt);
		}
		mutex_unlock(&sky1_init_mutex);
		break;
	default:
		break;
	}
}

static int sky1_pcie_set_pwr_en(struct sky1_pcie *pcie, bool en)
{
	struct device *dev = pcie->dev;
	void __iomem *strap_addr, *status_addr;
	u32 read, msk, mskc;
	int ret = 0;

	switch (pcie->desc->id) {
	case PCIE_ID_x8:
	case PCIE_ID_x4:
		strap_addr = pcie->strap_base + STRAP_REG(8);
		status_addr = pcie->status_base + STATUS_REG(8);
		msk = (pcie->desc->id == PCIE_ID_x8) ? (0xff) : (0xf);
		mskc = (pcie->desc->id == PCIE_ID_x8) ? (0x1ff) : (0x10f);
		if (en) {
			sky1_pcie_set_bits32(strap_addr, 0x100);
			ret = readl_poll_timeout(status_addr, read, !!(read&0x200), 5, 100);
			if (ret) {
				dev_err(dev, "Set pwr en err, 0x%x\n", read);
				return -ETIMEDOUT;
			}

			sky1_pcie_set_bits32(strap_addr, msk);
		} else
			sky1_pcie_clear_bits32(strap_addr, mskc);
		break;
	case PCIE_ID_x2:
	case PCIE_ID_x1_0:
	case PCIE_ID_x1_1:
		strap_addr = pcie->rcsu_base + APP_OFFSET_STRAP_REG + STRAP_REG(40);
		status_addr = pcie->rcsu_base + APP_OFFSET_STATUS_REG + STATUS_REG(40);
		msk = 0xf;
		mskc = 0x10f;
		mutex_lock(&sky1_init_mutex);
		if (en) {
			if (atomic_read(&pwr_en_cnt) == 0) {
				sky1_pcie_set_bits32(strap_addr, 0x100);
				ret = readl_poll_timeout(status_addr, read, !!(read&0x200), 5, 100);
				if (ret) {
					dev_err(dev, "Set pwr en err, 0x%x\n", read);
					return -ETIMEDOUT;
				}

				sky1_pcie_set_bits32(strap_addr, msk);
			}
			atomic_inc(&pwr_en_cnt);
		} else {
			atomic_dec(&pwr_en_cnt);
			if (atomic_read(&pwr_en_cnt) == 0)
				sky1_pcie_clear_bits32(strap_addr, mskc);
		}
		mutex_unlock(&sky1_init_mutex);
		break;
	default:
		return -EINVAL;
	}

	dev_dbg(dev, "Set STRAP_REG(0x%px) as :0x%x\n", strap_addr, readl(strap_addr));

	return ret;
}

static int sky1_pcie_set_phy_rst_n(struct sky1_pcie *pcie, int en)
{
	struct device *dev = pcie->dev;
	void __iomem *addr;
	int ret = 0;

	switch (pcie->desc->id) {
	case PCIE_ID_x8:
	case PCIE_ID_x4:
		addr = pcie->rcsu_base + APP_OFFSET_STRAP_REG + STRAP_REG(9);
		sky1_pcie_update_bits32(addr, 0x1, (en ? 0x1 : 0x0));
		break;
	case PCIE_ID_x2:
	case PCIE_ID_x1_0:
	case PCIE_ID_x1_1:
		addr = pcie->rcsu_base + APP_OFFSET_STRAP_REG + STRAP_REG(41);
		mutex_lock(&sky1_init_mutex);
		if (!en) {
			if (atomic_read(&phy_rst_assert_cnt) == 0) {
				sky1_pcie_update_bits32(addr, 0x1, 0x0);
				atomic_inc(&phy_rst_assert_cnt);
			}
		} else {
			if (atomic_read(&phy_rst_deassert_cnt) == 0) {
				sky1_pcie_update_bits32(addr, 0x1, 0x1);
				atomic_inc(&phy_rst_deassert_cnt);
			}
			atomic_inc(&x211_phy_rst_finish_cnt);
		}
		mutex_unlock(&sky1_init_mutex);
		break;
	default:
		ret = -EINVAL;
		break;
	}
	dev_dbg(dev, "Set STRAP_REG(9) as :0x%x\n",
		readl(pcie->strap_base + STRAP_REG(9)));

	return ret;
}

static void sky1_pcie_set_x211_phy_rst_assert(struct sky1_pcie *pcie)
{
	void __iomem *addr;

	if (sky1_pcie_is_x211(pcie)) {
		addr = pcie->rcsu_base + APP_OFFSET_STRAP_REG + STRAP_REG(41);
		mutex_lock(&sky1_init_mutex);
		if (atomic_read(&x211_phy_rst_finish_cnt) == 0)
			sky1_pcie_update_bits32(addr, 0x1, 0x0);
		mutex_unlock(&sky1_init_mutex);
	}
}

static int sky1_pcie_set_phy_pnn_rst_n(struct sky1_pcie *pcie, int en)
{
	struct device *dev = pcie->dev;
	void __iomem *addr;
	int ret = 0;
	u32 en_msk;

	switch (pcie->desc->id) {
	case PCIE_ID_x8:
		addr = pcie->rcsu_base + APP_OFFSET_STRAP_REG + STRAP_REG(10);
		en_msk = 0xff;
		break;
	case PCIE_ID_x4:
		addr = pcie->rcsu_base + APP_OFFSET_STRAP_REG + STRAP_REG(10);
		en_msk = 0xf;
		break;
	case PCIE_ID_x2:
		addr = pcie->rcsu_base + APP_OFFSET_STRAP_REG + STRAP_REG(42);
		sky1_pcie_update_bits32(addr, 0xf, 0xf);
		en_msk = (pcie->plat != PCIE_PLAT_EMU) ? 0x4 : 0x1;
		break;
	case PCIE_ID_x1_0:
		addr = pcie->rcsu_base + APP_OFFSET_STRAP_REG + STRAP_REG(42);
		sky1_pcie_update_bits32(addr, 0xf, 0xf);
		en_msk = (pcie->plat != PCIE_PLAT_EMU) ? 0x1 : 0x2;
		break;
	case PCIE_ID_x1_1:
		addr = pcie->rcsu_base + APP_OFFSET_STRAP_REG + STRAP_REG(42);
		sky1_pcie_update_bits32(addr, 0xf, 0xf);
		en_msk = (pcie->plat != PCIE_PLAT_EMU) ? 0x2 : 0x4;
		break;
	default:
		ret = -EINVAL;
		break;
	}

	if (!ret)
		sky1_pcie_update_bits32(addr, en_msk, (en ? en_msk : 0x0));

	dev_dbg(dev, "Set STRAP_REG(10)/STRAP_REG(42) as :0x%x\n", readl(addr));

	return ret;
}

static int __maybe_unused sky1_pcie_set_link_training_en(struct sky1_pcie *pcie, int en)
{
	struct device *dev = pcie->dev;
	void __iomem *addr;
	int ret = 0;

	switch (pcie->desc->id) {
	case PCIE_ID_x8:
	case PCIE_ID_x4:
	case PCIE_ID_x2:
	case PCIE_ID_x1_0:
	case PCIE_ID_x1_1:
		addr = pcie->strap_base + STRAP_REG(1);
		sky1_pcie_update_bits32(addr, 0x1, (en ? 0x1 : 0x0));
		break;
	default:
		ret = -EINVAL;
		break;
	}
	dev_dbg(dev, "Set STRAP_REG(1) as :0x%x\n", readl(addr));

	return ret;
}

static const struct sky1_pcie_ctrl_desc *
	sky1_pcie_find_desc_by_id(struct sky1_pcie *pcie, u32 id)
{
	const struct sky1_pcie_ctrl_desc *p = pcie->data->desc;
	const struct sky1_pcie_ctrl_desc *desc = NULL;

	while (p) {
		if (p->id != id) {
			p++;
			continue;
		} else {
			desc = p;
			break;
		}
	}
	if (desc == NULL)
		dev_err(pcie->dev, "%s err\n", __func__);
	else
		dev_dbg(pcie->dev, "%s, name:%s\n", __func__, desc->name);

	return desc;
}

static void sky1_pcie_init_bases(struct sky1_pcie *pcie)
{
	struct device *dev = pcie->dev;

	switch (pcie->desc->id) {
	case PCIE_ID_x1_1:
		pcie->strap_base = pcie->rcsu_base + APP_OFFSET_STRAP_REG +
				   APP_SUBOFFSET_STRAP_REG_X1B;
		pcie->status_base = pcie->rcsu_base + APP_OFFSET_STATUS_REG +
				    APP_SUBOFFSET_STRAP_REG_X1B;
		break;
	case PCIE_ID_x1_0:
		pcie->strap_base = pcie->rcsu_base + APP_OFFSET_STRAP_REG +
				   APP_SUBOFFSET_STRAP_REG_X1A;
		pcie->status_base = pcie->rcsu_base + APP_OFFSET_STATUS_REG +
				    APP_SUBOFFSET_STATUS_REG_X1A;
		break;
	case PCIE_ID_x2:
		pcie->strap_base = pcie->rcsu_base + APP_OFFSET_STRAP_REG +
				   APP_SUBOFFSET_STRAP_REG_X2;
		pcie->status_base = pcie->rcsu_base + APP_OFFSET_STATUS_REG +
				    APP_SUBOFFSET_STATUS_REG_X2;
		break;
	case PCIE_ID_x8:
	case PCIE_ID_x4:
	default:
		pcie->strap_base = pcie->rcsu_base + APP_OFFSET_STRAP_REG;
		pcie->status_base = pcie->rcsu_base + APP_OFFSET_STATUS_REG;
		break;
	}
	dev_dbg(dev, "pcie->strap_base vaddr is: 0x%px\n", pcie->strap_base);
	dev_dbg(dev, "pcie->status_base vaddr is: 0x%px\n", pcie->status_base);
}

static int sky1_pcie_parse_mem(struct sky1_pcie *pcie)
{
	struct device *dev = pcie->dev;
	struct platform_device *pdev = to_platform_device(dev);
	struct resource *res;
	void __iomem *base;
	int ret = 0;

	/*rcsu*/
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "rcsu");
	if (!res) {
		dev_err(dev, "Parse \"rcsu\" resource err\n");
		return (-ENXIO);
	}
	pcie->rcsu_base = devm_ioremap(dev, res->start, resource_size(res));
	if (!pcie->rcsu_base) {
		dev_err(dev, "ioremap failed for resource %pR\n", res);
		ret = -ENOMEM;
	}
	dev_info(dev, "ioremap %s, paddr:%pR, vaddr:%px\n", "rcsu", res,
		 pcie->rcsu_base);

	/*reg*/
	base = devm_platform_ioremap_resource_byname(pdev, "reg");
	if (IS_ERR(base)) {
		dev_err(dev, "Parse \"reg\" resource err\n");
		return PTR_ERR(base);
	}
	pcie->reg_base = base;

	/*msg*/
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "msg");
	if (!res) {
		dev_err(dev, "Parse \"msg\" resource err\n");
		return (-ENXIO);
	}
	pcie->msg_res = res;
	pcie->msg_base = devm_ioremap(dev, res->start, resource_size(res));
	if (!pcie->msg_base) {
		dev_err(dev, "ioremap failed for resource %pR\n", res);
		ret = -ENOMEM;
	}
	dev_info(dev, "ioremap %s, paddr:%pR, vaddr:%px\n", "msg", res,
		 pcie->msg_base);

	/*cfg*/
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "cfg");
	if (!res) {
		dev_err(dev, "Parse \"cfg\" resource err\n");
		return (-ENXIO);
	}
	pcie->cfg_res = res;

	return ret;
}

static int sky1_pcie_parse_ctrl_id(struct sky1_pcie *pcie)
{
	struct device *dev = pcie->dev;
	u32 id;
	int ret = 0;

	ret = device_property_read_u32(dev, "sky1,pcie-ctrl-id", &id);
	if (ret < 0) {
		dev_err(pcie->dev, "Failed to read sky1,pcie-ctrl-id: %d\n",
			ret);
		return ret;
	}

	if (ID_INVALID(id)) {
		dev_err(dev, "get illegal pcie-ctrl-id %d\n", id);
		ret = -EINVAL;
		return ret;
	}
	pcie->id = id;
	dev_dbg(pcie->dev, "Parse sky1,pcie-ctrl-id=%d\n", id);

	return ret;
}

static void sky1_pcie_parse_link_speed(struct sky1_pcie *pcie)
{
	struct device *dev = pcie->dev;
	u32 link_speed;

	device_property_read_u32(dev, "max-link-speed", &link_speed);
	if ((link_speed < 1) || (link_speed > 4))
		link_speed = pcie->desc->link_speed;
	pcie->link_speed = link_speed;
	dev_dbg(dev, "Parse link speed:%d\n", link_speed);
}

static int sky1_pcie_parse_num_lanes(struct sky1_pcie *pcie)
{
	struct device *dev = pcie->dev;
	u32 lanes;
	int ret = 0;

	ret = device_property_read_u32(dev, "num-lanes", &lanes);
	if (ret) {
		dev_err(dev, "error:%x, lane number:%d\n", ret,
			pcie->num_lanes);
		ret = -EINVAL;
		return ret;
	}

	if ((lanes < 1) || (lanes > pcie->desc->max_lanes))
		lanes = pcie->desc->max_lanes;
	pcie->num_lanes = lanes;
	dev_dbg(dev, "Parse lane number:%d\n", lanes);

	return ret;
}

static int sky1_pcie_parse_max_payload(struct sky1_pcie *pcie)
{
	struct device *dev = pcie->dev;
	u32 max_payload;
	int ret = 0;

	ret = device_property_read_u32(dev, "max-payload", &max_payload);
	if (ret) {
		dev_err(dev, "error:%x, max payload:%d\n", ret,
			pcie->max_payload);
		ret = -EINVAL;
		return ret;
	}

	if ((max_payload != 128) && (max_payload != 256) &&
	    (max_payload != 512))
		return -EINVAL;
	pcie->max_payload = max_payload;

	return ret;
}

static int sky1_pcie_parse_clocks(struct sky1_pcie *pcie)
{
	struct device *dev = pcie->dev;
	struct clk *axi_clk, *apb_clk, *refclk;
	int ret = 0;

	axi_clk = devm_clk_get(dev, "axi_clk");
	if (IS_ERR(axi_clk)) {
		ret = PTR_ERR(axi_clk);
		dev_err(dev, "Failed to get axi_clk\n");
	}
	pcie->pcie_axi_clk = axi_clk;

	apb_clk = devm_clk_get(dev, "apb_clk");
	if (IS_ERR(apb_clk)) {
		ret = PTR_ERR(apb_clk);
		dev_err(dev, "Failed to get apb_clk\n");
	}
	pcie->pcie_apb_clk = apb_clk;

	refclk = devm_clk_get_optional(dev, "refclk_b");
	if (IS_ERR(refclk)) {
		ret = PTR_ERR(refclk);
		dev_err(dev, "failed to get refclk_b\n");
	}
	pcie->pcie_refclk_b = refclk;

	return 0;
}

static void sky1_pcie_parse_plat(struct sky1_pcie *pcie)
{
	if (device_property_read_bool(pcie->dev, "plat-emu"))
		pcie->plat = PCIE_PLAT_EMU;
	else if (device_property_read_bool(pcie->dev, "plat-fpga"))
		pcie->plat = PCIE_PLAT_FPGA;
	else
		pcie->plat = PCIE_PLAT_EVK;
}

static int sky1_pcie_parse_ep_pwr_supply(struct sky1_pcie *pcie)
{
	struct device *dev = pcie->dev;

	pcie->str_pwron = device_property_read_bool(dev, "sky1,str-pwron");
	pcie->std_pwron = device_property_read_bool(dev, "sky1,std-pwron");
	if (device_property_read_u32(dev, "sky1,re-pwron-ms",
				     &pcie->re_pwron_delay_ms))
		pcie->re_pwron_delay_ms = 0;
	if (device_property_read_u32(dev, "sky1,re-pwroff-ms",
				     &pcie->re_pwroff_delay_ms))
		pcie->re_pwroff_delay_ms = 10;

	/* optional power control for device */
	pcie->vsupply = devm_regulator_get_optional(dev, "vcc-pcie");
	if (IS_ERR(pcie->vsupply)) {
		if (PTR_ERR(pcie->vsupply) != -ENODEV)
			return dev_err_probe(dev, PTR_ERR(pcie->vsupply),
					     "failed to get vcc-pcie regulator\n");
		pcie->vsupply = NULL;
	}

	pcie->epsupply = devm_regulator_get_optional(dev, "wlan-en");
	if (IS_ERR(pcie->epsupply)) {
		if (PTR_ERR(pcie->epsupply) != -ENODEV)
			return dev_err_probe(dev, PTR_ERR(pcie->epsupply),
					     "failed to get wlan-en regulator\n");
		pcie->epsupply = NULL;
	}

	return 0;
}

static int sky1_pcie_parse_reset_gpio(struct sky1_pcie *pcie)
{
	struct device *dev = pcie->dev;
	struct gpio_desc *gpiodesc;
	int ret = 0;

	if (pcie->plat == PCIE_PLAT_EMU)
		return ret;

	gpiodesc = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(gpiodesc)) {
		dev_err(dev, "Failed to get reset gpio\n");
		return PTR_ERR(gpiodesc);
	}
	pcie->reset = gpiodesc;

	return ret;
}

static int sky1_pcie_parse_wake_gpio(struct sky1_pcie *pcie)
{
	struct device *dev = pcie->dev;
	struct gpio_desc *gpiodesc;

	if (pcie->plat == PCIE_PLAT_EMU)
		return 0;

	gpiodesc = devm_gpiod_get_optional(dev, "wake", GPIOD_IN);
	if (IS_ERR(gpiodesc)) {
		dev_err(dev, "Failed to get wake gpio\n");
		return PTR_ERR(gpiodesc);
	}
	pcie->wake = gpiodesc;

	if (pcie->wake) {
		dev_dbg(dev, "wakeup-source\n");
		device_init_wakeup(dev, true);
		enable_irq_wake(gpiod_to_irq(pcie->wake));
	}

	return 0;
}

static int sky1_pcie_en_ep_power(struct sky1_pcie *pcie, bool en)
{
	int ret = 0;
	struct device *dev = pcie->dev;

	if (!pcie->vsupply)
		return ret;

	if (en)
		ret = regulator_enable(pcie->vsupply);
	else
		ret = regulator_disable(pcie->vsupply);
	if (ret)
		dev_err(dev, "fail to %s vcc-pcie regulator\n",
			en ? "enable" : "disable");

	return ret;
}

/**
 * sky1_pcie_safe_power_cycle - Safely perform power cycle on PCIe endpoint device
 * @pcie: PCIe controller private data structure
 *
 * This function implements a safe power cycle (on->off->on) for the PCIe
 * endpoint device. Mainly used for device reset or re-initialization scenarios.
 *
 * Return: 0 on success, negative value on failure
 */
static int sky1_pcie_safe_power_cycle(struct sky1_pcie *pcie)
{
	int ret;

	if ((pcie->plat == PCIE_PLAT_EMU) || (pcie->plat == PCIE_PLAT_FPGA))
		return 0;

	if (!pcie->vsupply)
		return 0;

	/* Power cycle: turn on -> off -> on */
	ret = sky1_pcie_en_ep_power(pcie, true);
	if (ret < 0)
		return ret;

	if (!pcie->re_pwron_delay_ms)
		return 0;

	ret = sky1_pcie_en_ep_power(pcie, false);
	if (ret < 0)
		return ret;

	/* Wait for capacitor discharge after power off */
	msleep(pcie->re_pwroff_delay_ms);

	ret = sky1_pcie_en_ep_power(pcie, true);
	if (ret < 0)
		return ret;
	/* Wait for endpoint initialization after power on. */
	msleep(pcie->re_pwron_delay_ms);
	dev_info(pcie->dev,
		 "power cycle completed, pwroff = %d ms, pwron = %d ms\n",
		 pcie->re_pwroff_delay_ms, pcie->re_pwron_delay_ms);
	return 0;
}

static int sky1_pcie_en_ep_on(struct sky1_pcie *pcie, bool en)
{
	int ret = 0;
	struct device *dev = pcie->dev;

	if (!pcie->epsupply)
		return ret;

	if (en)
		ret = regulator_enable(pcie->epsupply);
	else
		ret = regulator_disable(pcie->epsupply);
	if (ret)
		dev_err(dev, "fail to %s wlan-en regulator\n",
			en ? "enable" : "disable");
	return ret;
}

static void sky1_pcie_reset_ep_assert(struct sky1_pcie *pcie)
{
	if (pcie->reset == NULL)
		return;

	gpiod_set_value_cansleep(pcie->reset, 0);
	usleep_range(PERST_DELAY_US, PERST_DELAY_US + 500);
}

static void sky1_pcie_reset_ep_deassert(struct sky1_pcie *pcie)
{
	if (pcie->reset == NULL)
		return;

	msleep(100);
	gpiod_set_value_cansleep(pcie->reset, 1);
	usleep_range(PERST_DELAY_US, PERST_DELAY_US + 500);
}

static int sky1_pcie_parse_reset_ctrl(struct sky1_pcie *pcie)
{
	struct device *dev = pcie->dev;
	struct reset_control *rst;
	int ret = 0;

	rst = devm_reset_control_get_exclusive(dev, "pcie_reset");
	if (IS_ERR(rst)) {
		dev_err(dev, "Failed to get reset gpio\n");
		return PTR_ERR(rst);
	}
	pcie->rst = rst;

	return ret;
}

static int sky1_pcie_parse_local_irq(struct sky1_pcie *pcie)
{
	struct device *dev = pcie->dev;
	struct platform_device *pdev = to_platform_device(dev);

	/* local (some error statuss) */
	pcie->local_irq = platform_get_irq_byname(pdev, "local");
	if (pcie->local_irq < 0) {
		dev_err(pcie->dev, "missing local IRQ resource\n");
		return pcie->local_irq;
	}

	return 0;
}

static int sky1_pcie_parse_aer_irq(struct sky1_pcie *pcie)
{
	struct device *dev = pcie->dev;
	struct platform_device *pdev = to_platform_device(dev);
	struct device_node *np = pdev->dev.of_node;

	pcie->is_aer_uncor_panic = of_property_read_bool(np, "sky1,aer-uncor-panic");
	pcie->aer_log_ce_ratelimited = of_property_read_bool(np, "myt,aer-log-ce-ratelimited");
	/* AER correctable */
	pcie->aer_c_irq = platform_get_irq_byname(pdev, "aer_c");
	if (pcie->aer_c_irq < 0)
		return pcie->aer_c_irq;

	/* AER uncorrectable fatal*/
	pcie->aer_f_irq = platform_get_irq_byname(pdev, "aer_f");
	if (pcie->aer_f_irq < 0)
		return pcie->aer_f_irq;

	/* AER uncorrectable non fatal*/
	pcie->aer_nf_irq = platform_get_irq_byname(pdev, "aer_nf");
	if (pcie->aer_nf_irq < 0)
		return pcie->aer_nf_irq;

	return 0;
}

static void sky1_pcie_parse_max_aspm_support(struct sky1_pcie *pcie)
{
	struct device *dev = pcie->dev;
	u32 max_aspm_support;
	int ret = 0;

	ret = device_property_read_u32(dev, "max-aspm-support", &max_aspm_support);
	if (ret) {
		dev_dbg(dev, "Get property max-aspm-support fail:%d\n", ret);
		max_aspm_support = 0x2;
	}

	if ((max_aspm_support >= 0) && (max_aspm_support <= 3))
		pcie->max_aspm_support = max_aspm_support;
	else
		pcie->max_aspm_support = 2;
}

static void devm_phy_release(struct device *dev, void *res)
{
	struct phy *phy = *(struct phy **)res;

	phy_put(dev, phy);
}

static struct phy *devm_phy_ref_get(struct device *dev,
			const char *string)
{
	struct phy **ptr, *phy;
	struct fwnode_handle *fwnode;
	struct device *rdev;

	fwnode = fwnode_find_reference(dev_fwnode(dev), string, 0);
	if (IS_ERR_OR_NULL(fwnode))
		return NULL;

	ptr = devres_alloc(devm_phy_release, sizeof(*ptr), GFP_KERNEL);
	if (!ptr)
		return ERR_PTR(-ENOMEM);

	rdev = get_dev_from_fwnode(fwnode_get_parent(fwnode));
	if (IS_ERR_OR_NULL(rdev))
		return NULL;
	phy = phy_get(rdev, fwnode_get_name(fwnode));
	if (!IS_ERR(phy)) {
		*ptr = phy;
		devres_add(dev, ptr);
	} else {
		devres_free(ptr);
	}
	put_device(rdev);

	return phy;
}

static int sky1_pcie_parse_property(struct platform_device *pdev,
			       struct sky1_pcie *pcie)
{
	int ret = 0;

	sky1_pcie_parse_plat(pcie);

	ret = sky1_pcie_parse_ctrl_id(pcie);
	if (ret < 0)
		return ret;
	pcie->desc = sky1_pcie_find_desc_by_id(pcie, pcie->id);
	if (pcie->desc->sbsa)
		pcie->ecam_support_flag = 0x1;

	sky1_pcie_parse_link_speed(pcie);

	ret = sky1_pcie_parse_num_lanes(pcie);
	if (ret < 0)
		return ret;

	ret = sky1_pcie_parse_max_payload(pcie);
	if (ret < 0)
		return ret;

	ret = sky1_pcie_parse_clocks(pcie);
	if (ret < 0)
		return ret;

	ret = sky1_pcie_parse_reset_gpio(pcie);
	if (ret < 0)
		return ret;

	ret = sky1_pcie_parse_wake_gpio(pcie);
	if (ret < 0)
		return ret;

	ret = sky1_pcie_parse_reset_ctrl(pcie);
	if (ret < 0)
		return ret;

	ret = sky1_pcie_parse_mem(pcie);
	if (ret < 0)
		return ret;

	pcie->pcie_phy = devm_phy_get(pcie->dev, "cdns,pcie-phy");
	if (IS_ERR(pcie->pcie_phy))
		pcie->pcie_phy = devm_phy_ref_get(pcie->dev, "cdns,pcie-phy");

	if (IS_ERR(pcie->pcie_phy)) {
		dev_err(pcie->dev, "couldn't get pcie phy\n");
		return PTR_ERR(pcie->pcie_phy);
	}

	ret = sky1_pcie_parse_local_irq(pcie);
	if (ret < 0)
		return ret;

	ret = sky1_pcie_parse_aer_irq(pcie);
	if (ret < 0)
		return ret;
	sky1_pcie_parse_max_aspm_support(pcie);

	sky1_pcie_init_bases(pcie);

	return ret;
}

static int sky1_pcie_ctrl_set_rstn(struct sky1_pcie *pcie, bool en)
{
	struct device *dev = pcie->dev;
	int ret = 0;

	mutex_lock(&sky1_init_mutex);
	if (en) {
		ret = reset_control_deassert(pcie->rst);
		if (ret) {
			mutex_unlock(&sky1_init_mutex);
			dev_err(dev, "deassert pcie_rst err %d\n", ret);
			return ret;
		}
	} else {
		ret = reset_control_assert(pcie->rst);
		if (ret) {
			mutex_unlock(&sky1_init_mutex);
			dev_err(dev, "assert pcie_rst err %d\n", ret);
			return ret;
		}
	}
	mutex_unlock(&sky1_init_mutex);
	dev_dbg(pcie->dev, "%s en:%d\n", __func__, en);
	return ret;
}

static int sky1_pcie_phy_rst(struct sky1_pcie *pcie, int en)
{
	int ret = 0;

	ret = sky1_pcie_set_phy_pnn_rst_n(pcie, en);
	if (ret)
		return ret;

	if ((pcie->plat != PCIE_PLAT_EMU) || (en != 0))
		ret = sky1_pcie_set_phy_rst_n(pcie, en);

	dev_dbg(pcie->dev, "%s en:%d\n", __func__, en);
	return ret;
}

static int sky1_pcie_set_devctrl(struct sky1_pcie *pcie)
{
	struct pci_host_bridge *bridge =
		pci_host_bridge_from_priv(pcie->cdns_pcie_rc);
	struct device *dev = pcie->dev;
	u32 reg, bits, mask = PCI_EXP_DEVCTL_PAYLOAD | PCI_EXP_DEVCTL_READRQ;
	u16 offset;

	offset = cdns_pcie_find_capability(pcie->reg_base, PCI_CAP_ID_EXP);
	if (!offset)
		return -ENODEV;

	bits = (ffs(pcie->max_payload) - 8) << 5;
	bits |= bridge->fixed_mps ?
		(ffs(bridge->fixed_mps) - 8) << 12 : PCI_EXP_DEVCTL_READRQ_1024B;

	reg = sky1_pcie_ctrl_readl_reg(pcie, offset + PCI_EXP_DEVCTL);
	/* The upper half is DevSta (W1C); do not acknowledge it here. */
	reg &= 0xffff;
	reg &= ~mask;
	reg |= bits | PCI_EXP_DEVCTL_NOSNOOP_EN | PCI_EXP_DEVCTL_RELAX_EN;
	sky1_pcie_ctrl_writel_reg(pcie, offset + PCI_EXP_DEVCTL, reg);
	reg = sky1_pcie_ctrl_readl_reg(pcie, offset + PCI_EXP_DEVCTL);
	if ((reg & mask) != bits) {
		dev_err(dev, "RC DevCtl readback failed: %#08x, expected %#08x\n",
			reg, bits);
		return -EIO;
	}
	return 0;
}

static void sky1_pcie_set_preset_val(struct sky1_pcie *pcie)
{
	struct device *dev = pcie->dev;
	u16 offset;

	offset = cdns_pcie_find_ext_capability(pcie->reg_base,
					       PCI_EXT_CAP_ID_SECPCI);
	if (!offset)
		return;
	dev_dbg(dev, "PCI_EXT_CAP_ID_SECPCI offset = 0x%x\n", offset);
	/* GEN3 preset */
	sky1_pcie_ctrl_writel_reg(pcie, offset + 0x0c, 0x27072707);
	sky1_pcie_ctrl_writel_reg(pcie, offset + 0x10, 0x27072707);
	sky1_pcie_ctrl_writel_reg(pcie, offset + 0x14, 0x27072707);
	sky1_pcie_ctrl_writel_reg(pcie, offset + 0x18, 0x27072707);

	offset = cdns_pcie_find_ext_capability(pcie->reg_base,
					       PCI_EXT_CAP_ID_PL_16GT);
	if (!offset)
		return;
	dev_dbg(dev, "PCI_EXT_CAP_ID_PL_16GT offset = 0x%x\n", offset);
	/* GEN4 preset */
	sky1_pcie_ctrl_writel_reg(pcie, offset + PCI_PL_16GT_LE_CTRL,
				  0x66666666);
	sky1_pcie_ctrl_writel_reg(pcie, offset + PCI_PL_16GT_LE_CTRL + 4,
				  0x66666666);
}

static void sky1_pcie_set_refclk(struct sky1_pcie *pcie, bool en)
{
	static atomic_t x211_refclk_enable_cnt = ATOMIC_INIT(0);
	u32 mask = 0, value = 0;
	void __iomem *strap_addr = NULL;;

	/* set ext_refclk_detected_cfg */
	mask |= EXT_REFCLK_DETECTED_CFG;
	value |= EXT_REFCLK_DETECTED_CFG;
	/* set pma_cmn_rescal_insel */
	mask |= PMA_CMN_RESCAL_INSEL;
	value |= PMA_CMN_RESCAL_INSEL;
	/* set refclk0_term_en */
	mask |= REFCLK0_TERM_EN;
	value |= REFCLK0_TERM_EN;
	/* set refclk dig div */
	mask |= PMA_REFCLK_DIG_MASK;
	value |= (0x2 << PMA_REFCLK_DIG_DIV);

	switch (pcie->desc->id) {
	case PCIE_ID_x8:
	case PCIE_ID_x4:
		strap_addr = pcie->strap_base + STRAP_REG(8);
		if (en)
			sky1_pcie_update_bits32(strap_addr, mask, value);
		else
			sky1_pcie_clear_bits32(strap_addr, mask);
		break;
	case PCIE_ID_x2:
	case PCIE_ID_x1_0:
	case PCIE_ID_x1_1:
		strap_addr = pcie->rcsu_base + APP_OFFSET_STRAP_REG + STRAP_REG(40);
		mutex_lock(&sky1_init_mutex);
		if (en) {
			sky1_pcie_update_bits32(strap_addr, mask, value);
			atomic_inc(&x211_refclk_enable_cnt);
		} else {
			atomic_dec(&x211_refclk_enable_cnt);
			if (atomic_read(&x211_refclk_enable_cnt) == 0)
				sky1_pcie_clear_bits32(strap_addr, mask);
		}
		mutex_unlock(&sky1_init_mutex);
		break;
	default:
		break;
	}
}

static void sky1_pcie_set_l0s_disable(struct sky1_pcie *pcie)
{
	u8 offset;
	u32 reg;

	offset = cdns_pcie_find_capability(pcie->reg_base, PCI_CAP_ID_EXP);
	/* Clear L0s from RC's link cap */
	reg = sky1_pcie_ctrl_readl_reg(pcie, offset + PCI_EXP_LNKCAP);
	reg &= ~PCI_EXP_LNKCAP_ASPM_L0S;
	sky1_pcie_ctrl_writel_reg(pcie, offset + PCI_EXP_LNKCAP, reg);
}

static void sky1_pcie_set_l1_disable(struct sky1_pcie *pcie)
{
	u8 offset;
	u32 reg;

	offset = cdns_pcie_find_capability(pcie->reg_base, PCI_CAP_ID_EXP);
	reg = sky1_pcie_ctrl_readl_reg(pcie, offset + PCI_EXP_LNKCAP);
	reg &= ~PCI_EXP_LNKCAP_ASPM_L1;
	sky1_pcie_ctrl_writel_reg(pcie, offset + PCI_EXP_LNKCAP, reg);
}

static void sky1_pcie_filter_msg(struct sky1_pcie *pcie)
{
	u32 reg;

	reg = cdns_pcie_readl(pcie->cdns_pcie, I_HAL_CTRL_DEC_TLP_FILTER);
	/* bit3: LTR, bit6: PTM */
	reg |= BIT(3) | BIT(6);
	cdns_pcie_writel(pcie->cdns_pcie, I_HAL_CTRL_DEC_TLP_FILTER, reg);
}

static int sky1_pcie_init(struct sky1_pcie *pcie)
{
	u32 val = 0;
	struct device *dev = pcie->dev;
	int ret;

	dev_dbg(dev, "%s, %i\n", __func__, __LINE__);
	ret = sky1_pcie_set_devctrl(pcie);
	if (ret)
		return ret;
	if (!(pcie->max_aspm_support & BIT(0)))
		sky1_pcie_set_l0s_disable(pcie);
	if (!(pcie->max_aspm_support & BIT(1)))
		sky1_pcie_set_l1_disable(pcie);
	sky1_pcie_filter_msg(pcie);

	// if set D3hot，it will hang, power state change set to 0
	writel(0, pcie->reg_base + POWER_STATE_CHANGE_CTRL);

	// i_cfg_5 register DLUC(Disable Link Upconfigure Capability) field set to 0
	writel(0x1F000000, (pcie->reg_base + I_CFG_5));

	// physical layer configuration
	writel(0x03141C05, (pcie->reg_base + I_CFG_7));

	// Set Preset Register
	sky1_pcie_set_preset_val(pcie);

	// Enable PCLK Rate override
	writel(0x80000688, (pcie->reg_base + I_CFG_9));

	// Set AXI Slave Response Control for Completion Status errors:
	// resp_cpl_status (bit0-1) = 0x0 (OKAY), instead of default 0x2 (SLVERR)
	val = cdns_pcie_readl(pcie->cdns_pcie, AXI_SLAVE_RESP_CTRL_MEM);
	val &= ~GENMASK(1, 0);
	cdns_pcie_writel(pcie->cdns_pcie, AXI_SLAVE_RESP_CTRL_MEM, val);

	dev_dbg(dev, "%s, %i\n", __func__, __LINE__);
	return 0;
}

static void sky1_pcie_set_strap_pin0(struct sky1_pcie *pcie)
{
	union ustrap_pin0 strap_pin0 = {0};

	strap_pin0.reg = readl(pcie->strap_base + STRAP_REG(0));
	/* clear bypass_phase23 and bypass_remote_eq */
	strap_pin0.strap_bypass_phase23 = 0;
	strap_pin0.strap_bypass_remote_tx_eq = 0;

	/* set iteration timeout */
	strap_pin0.strap_dc_max_eval_iteration = 0x2;

	/* set support preset val */
	strap_pin0.strap_supported_preset = 0x7ff;

	/* Set link speed */
	if (pcie->link_speed <= 0 || pcie->link_speed > 4)
		pcie->link_speed = 1;
	strap_pin0.strap_pcie_rate_max = pcie->link_speed - 1;

	/* Set lane number */
	if (sky1_pcie_get_max_lane_count(pcie) < 0)
		pcie->num_lanes = 1;
	strap_pin0.strap_lane_count_in = sky1_pcie_get_max_lane_count(pcie);

	writel(strap_pin0.reg, pcie->strap_base + STRAP_REG(0));
}

static void sky1_pcie_set_axcache(struct sky1_pcie *pcie)
{
	/* set axcache: Normal Non-cacheable Bufferable */
	writel(0x3, pcie->strap_base + STRAP_REG(5));
}

static u32 sky1_phy_readl_phy_status(struct sky1_pcie *pcie, u32 reg)
{
	void __iomem *base;

	switch (pcie->desc->id) {
	case PCIE_ID_x8:
	case PCIE_ID_x4:
		base = pcie->rcsu_base + APP_OFFSET_STATUS_REG + STRAP_REG(8);
		break;
	case PCIE_ID_x2:
	case PCIE_ID_x1_0:
	case PCIE_ID_x1_1:
		base = pcie->rcsu_base + APP_OFFSET_STATUS_REG + STRAP_REG(40);
		break;
	default:
		break;
	}

	return readl_relaxed(base + reg);
}

static int sky1_pcie_wait_phy_ready(struct sky1_pcie *pcie)
{
	struct device *dev = pcie->dev;
	u32 retries = 0;
	int ret = 0;
	u32 val = 0;

	do {
		mutex_lock(&sky1_init_mutex);
		val = sky1_phy_readl_phy_status(pcie, STATUS_REG(1));
		mutex_unlock(&sky1_init_mutex);
		switch (pcie->desc->id) {
		case PCIE_ID_x8:
		case PCIE_ID_x4:
			val &= BIT(8);
			break;
		case PCIE_ID_x2:
			val &= BIT(10) | BIT(11);
			break;
		case PCIE_ID_x1_0:
			val &= BIT(8);
			break;
		case PCIE_ID_x1_1:
			val &= BIT(9);
			break;
		default:
			break;
		}
		if (!val)
			break;
		retries++;
		usleep_range(SKY1_PHY_DELAY_US_MIN, SKY1_PHY_DELAY_US_MAX);
	} while (retries < SKY1_PHY_TIMEOUT_CNT);

	if (retries >= SKY1_PHY_TIMEOUT_CNT) {
		dev_err(dev, "PHY failed to come up!\n");
		return -ENODEV;
	}

	dev_info(dev, "waiting PHY is ready! retries = %d\n", retries);
	return ret;
}

static int sky1_pcie_reset(struct sky1_pcie *pcie)
{
	int ret = 0;

	dev_dbg(pcie->dev, "%s, id:%d, name:%s\n", __func__, pcie->desc->id,
		pcie->desc->name);

	ret = sky1_pcie_ctrl_set_rstn(pcie, false);
	if (ret < 0)
		goto err_pcie_reset;

	ret = sky1_pcie_phy_rst(pcie, 0x0);
	if (ret < 0)
		goto err_pcie_reset;

	sky1_pcie_ctrl_set_work_mode(pcie);
	sky1_pcie_set_strap_pin0(pcie);
	sky1_pcie_set_axcache(pcie);

	sky1_pcie_set_refclk(pcie, true);

	ret = phy_init(pcie->pcie_phy);
	if (ret < 0) {
		dev_err(pcie->dev, "fail to init phy, err %d\n", ret);
		goto err_pcie_reset;
	}

	ret = sky1_pcie_ctrl_set_rstn(pcie, true);
	if (ret < 0)
		goto err_pcie_reset;

	ret = sky1_pcie_set_pwr_en(pcie, true);
	if (ret < 0)
		goto err_pcie_reset;

	if ((pcie->plat != PCIE_PLAT_FPGA) && (pcie->plat != PCIE_PLAT_EMU)) {
		ret = phy_power_on(pcie->pcie_phy);
		if (ret < 0) {
			phy_exit(pcie->pcie_phy);
			goto err_pcie_reset;
		}
	}

	ret = sky1_pcie_phy_rst(pcie, 0x1);
	if (ret < 0)
		goto err_pcie_reset;

	ret = sky1_pcie_wait_phy_ready(pcie);
	if (ret < 0)
		goto err_pcie_reset;

	dev_dbg(pcie->dev, "%s, %i\n", __func__, __LINE__);

err_pcie_reset:
	return ret;
}

#ifdef CONFIG_ACPI
extern int acpi_pci_probe_root_resources(struct acpi_pci_root_info *info);

static struct resource *devm_pci_create_bus_range(struct device *dev)
{
	struct resource *res;
	u32 bus_range[2];
	int ret;

	res = devm_kzalloc(dev, sizeof(*res), GFP_KERNEL);
	if (!res)
		return NULL;

	ret = device_property_read_u32_array(dev, "bus-range", bus_range,
					   ARRAY_SIZE(bus_range));
	if (ret) {
		devm_kfree(dev, res);
		return NULL;
	}

	res->name = "bus-range";
	res->start = bus_range[0];
	res->end = bus_range[1];
	res->flags = IORESOURCE_BUS;

	return res;
}

int devm_acpi_pci_bridge_init(struct device *dev,
			struct pci_host_bridge *bridge)
{
	struct resource *res = NULL;
	struct resource_entry *entry, *tmp;
	struct list_head list;
	struct acpi_pci_root_info info = { 0 };
	int ret;

	bridge->domain_nr = 0; //TODO: get from _SEG property later

	/* bus range */
	INIT_LIST_HEAD(&list);

	res = devm_pci_create_bus_range(dev);
	if (!res) {
		dev_err(dev, "acpi pci create bus range fail\n");
		goto bus_err;
	}

	pci_add_resource(&list, res);

	/* bar spaces */
	INIT_LIST_HEAD(&info.resources);
	info.bridge = ACPI_COMPANION(dev);

	ret = acpi_pci_probe_root_resources(&info);
	if (ret < 0) {
		dev_err(dev, "acpi pci probe root resources fail\n");
		goto bar_err;
	}

	resource_list_for_each_entry_safe(entry, tmp, &info.resources)
		if (!(entry->res->flags & IORESOURCE_WINDOW))
			resource_list_destroy_entry(entry);

	list_splice_init(&list, &bridge->windows);
	list_splice_init(&info.resources, &bridge->windows);

	return 0;

bar_err:
	resource_list_for_each_entry_safe(entry, tmp, &info.resources)
		resource_list_destroy_entry(entry);
bus_err:
	if (res)
		devm_kfree(dev, res);

	return ret;
}

static void acpi_pci_alloc_host_bridge_release(void *data)
{
	pci_free_host_bridge(data);
}

static struct pci_host_bridge *devm_acpi_pci_alloc_host_bridge(struct device *dev,
						   size_t priv)
{
	int ret;
	struct pci_host_bridge *bridge;

	bridge = pci_alloc_host_bridge(priv);
	if (!bridge)
		return NULL;

	bridge->dev.parent = dev;

	ret = devm_add_action_or_reset(dev, acpi_pci_alloc_host_bridge_release,
				       bridge);
	if (ret)
		return NULL;

	bridge->swizzle_irq = pci_common_swizzle;

	ret = devm_acpi_pci_bridge_init(dev, bridge);
	if (ret)
		return NULL;

	return bridge;
}
#else
static struct pci_host_bridge *devm_acpi_pci_alloc_host_bridge(struct device *dev,
						   size_t priv)
{
	return NULL;
}
#endif

static int sky1_pcie_start_link(struct cdns_pcie *cdns_pcie)
{
	struct sky1_pcie *pcie = dev_get_drvdata(cdns_pcie->dev);
	u32 strap_value;

	sky1_pcie_reset_ep_deassert(pcie);
	strap_value = readl(pcie->strap_base + STRAP_REG(1));
	strap_value |= LINK_TRAINING_ENABLE;
	writel(strap_value, pcie->strap_base + STRAP_REG(1));

	return 0;
}

static void sky1_pcie_stop_link(struct cdns_pcie *cdns_pcie)
{
	struct sky1_pcie *pcie = dev_get_drvdata(cdns_pcie->dev);
	u32 strap_value;

	strap_value = readl(pcie->strap_base + STRAP_REG(1));
	strap_value &= ~LINK_TRAINING_ENABLE;
	writel(strap_value, pcie->strap_base + STRAP_REG(1));
}

static int __init sky1_pcie_linkup_delay(char *str)
{
	int tmp;

	tmp = kstrtoul(str, 0, &linkup_delay);
	if (tmp)
		return tmp;

	return 1;
}
__setup("pcie_linkup_delay=", sky1_pcie_linkup_delay);

static void sky1_pcie_log_linkup_debug(struct sky1_pcie *pcie, u32 ltssm,
				       bool link_complete)
{
	u16 lnksta = 0;
	u16 cap;
	u16 gen = 0;
	u16 lanes = 0;

	cap = cdns_pcie_find_capability(pcie->reg_base, PCI_CAP_ID_EXP);
	if (cap) {
		lnksta = readw(pcie->reg_base + cap + PCI_EXP_LNKSTA);
		gen = lnksta & PCI_EXP_LNKSTA_CLS;
		lanes = (lnksta & PCI_EXP_LNKSTA_NLW) >>
			PCI_EXP_LNKSTA_NLW_SHIFT;
	}

	dev_info_ratelimited(
		pcie->dev,
		"linkup information: training=%s ltssm=%u(%s) negotiated=x%u Gen%u target=x%u Gen%u\n",
		link_complete ? "done" : "not-complete", ltssm,
		sky1_ltssm_sts_name(ltssm), lanes, gen, pcie->num_lanes,
		pcie->link_speed);
}

bool sky1_pcie_link_up(struct cdns_pcie *cdns_pcie)
{
	u32 val;
	u32 ltssm;
	u32 complete;
	void __iomem *status_addr;
	struct sky1_pcie *pcie;

	pcie = dev_get_drvdata(cdns_pcie->dev);
	status_addr = pcie->status_base + STATUS_REG(0);
	val = sky1_pcie_get_bits32(status_addr, ~0);
	ltssm = (val >> PCIE_LTSSM_STATUS_SHIFT) & 0x3f;

	val = cdns_pcie_readl(cdns_pcie, IP_REG_I_DBG_STS_0);
	complete = val & LINK_COMPLETE;
	if (complete) {
		sky1_pcie_log_linkup_debug(pcie, ltssm, true);
	}

	/*
	 * As per PCIe r6.0, sec 6.6.1, a Downstream Port that supports Link
	 * speeds greater than 5.0 GT/s, software must wait a minimum of 100 ms
	 * after Link training completes before sending a Configuration Request.
	 */
	if (complete && (pcie->link_speed > 2))
		msleep(linkup_delay ?: PCIE_RESET_CONFIG_WAIT_MS);

	return (bool)complete;
}

static void sky1_pcie_pme_turn_off(struct sky1_pcie *pcie)
{
	u32 offset;

	offset = CDNS_PCIE_NORMAL_MSG_ROUTING(MSG_ROUTING_BCAST) |
		 CDNS_PCIE_NORMAL_MSG_CODE(MSG_CODE_PME_TURN_OFF);
	writel(0, pcie->msg_base + offset);
	usleep_range(1000, 10000);
}

static const struct cdns_pcie_ops sky1_pcie_ops = {
	.start_link = sky1_pcie_start_link,
	.stop_link = sky1_pcie_stop_link,
	.link_up = sky1_pcie_link_up,
};

static void sky1_pcie_get_linkctrl_offset(struct sky1_pcie *pcie)
{
	/* PCI_EXP_LNKCTL_LD: Link Disable */
	pcie->linkctrl_offset =
		cdns_pcie_find_capability(pcie->reg_base, PCI_CAP_ID_EXP) +
		PCI_EXP_LNKCTL;
}

static void sky1_pcie_set_linksta_slc(struct sky1_pcie *pcie)
{
	u32 val;

	/* Slot Clock Configuration: our design is common clock */
	val = sky1_pcie_ctrl_readl_reg(pcie, pcie->linkctrl_offset);
	val |= PCI_EXP_LNKSTA_SLC << 16;
	sky1_pcie_ctrl_writel_reg(pcie, pcie->linkctrl_offset, val);
}

static void sky1_pcie_set_link_disable(struct sky1_pcie *pcie)
{
	void __iomem *status_addr;
	u32 val, ret;

	val = sky1_pcie_ctrl_readl_reg(pcie, pcie->linkctrl_offset);
	val |= PCI_EXP_LNKCTL_LD;
	sky1_pcie_ctrl_writel_reg(pcie, pcie->linkctrl_offset, val);

	status_addr = pcie->status_base + STATUS_REG(0);
	ret = readl_poll_timeout(status_addr, val,
				 (((val >> PCIE_LTSSM_STATUS_SHIFT) & 0x3f) ==
				  PCIE_LTSSM_DISABLED),
				 10, 100000);
	if (ret)
		dev_dbg(pcie->dev, "link not in DISABLED, 0x%x\n", val);
}

void __iomem *sky1_pcie_own_conf_map_bus(struct pci_bus *bus,
					 unsigned int devfn, int where)
{
	struct pci_host_bridge *bridge = pci_find_host_bridge(bus);
	struct cdns_pcie_rc *rc = pci_host_bridge_priv(bridge);
	struct cdns_pcie *c_pcie = &rc->pcie;
	struct sky1_pcie *pcie = dev_get_drvdata(c_pcie->dev);
	void __iomem *addr;

	if (PCI_SLOT(devfn) > 0)
		return NULL;

	if ((where == PCI_BRIDGE_CONTROL) || (where == pcie->linkctrl_offset))
		addr = pcie->reg_base + where;
	else
		addr = pci_generic_ecam_ops.pci_ops.map_bus(bus, devfn, where);

	return addr;
}
EXPORT_SYMBOL_GPL(sky1_pcie_own_conf_map_bus);

static struct pci_ops sky1_pcie_own_ops = {
	.map_bus = sky1_pcie_own_conf_map_bus,
	.read = pci_generic_config_read,
	.write = pci_generic_config_write,
};

static int sky1_pcie_set_slot_lines(struct device *dev,
				      struct gpio_descs *gpios,
				      const char *name, int value)
{
	unsigned int i, confirmed = 0;
	int ret, error = 0;

	if (!gpios)
		return 0;

	for (i = 0; i < gpios->ndescs; i++) {
		ret = gpiod_direction_output(gpios->desc[i], value);
		if (!ret) {
			ret = gpiod_get_value_cansleep(gpios->desc[i]);
			if (ret >= 0)
				ret = ret == value ? 0 : -EIO;
		}
		if (ret) {
			dev_warn(dev, "%s GPIO %u: setting %d failed (%d)\n",
				 name, i, value, ret);
			error = ret;
			break;
		} else {
			confirmed++;
		}
	}

	dev_notice(dev, "%s %s: %u/%u\n", name, value ? "high" : "low",
		   confirmed, gpios->ndescs);
	return error;
}

static void sky1_pcie_shutdown(struct platform_device *pdev)
{
	struct sky1_pcie *pcie = platform_get_drvdata(pdev);

	if (!pcie->slot_power && !pcie->slot_presence_gates)
		return;

	/*
	 * PCI children have completed device shutdown. Keep the sleepable
	 * GPIO expander transactions here, before the syscore callback
	 * switches off the IX8024 core supply.
	 */
	/* Stop AER recovery from accessing endpoints as their rails fall. */
	if (pcie->is_probe) {
		sky1_pcie_disable_irq(pcie);
		pcie->slot_shutdown_irqs_disabled = true;
	}
	/* Leave the presence gates connected during normal teardown. */
	sky1_pcie_set_slot_lines(pcie->dev, pcie->slot_power,
				 "downstream slot EN", 0);
	sky1_pcie_reset_ep_assert(pcie);

	/* Allow the slot rails to discharge before platform reset. */
	msleep(100);
}

static int _sky1_pcie_syscore_op_shutdown(struct sky1_pcie *pcie)
{
	if (!pcie->is_probe)
		return 0;

	if (!pcie->slot_shutdown_irqs_disabled)
		sky1_pcie_disable_irq(pcie);
	sky1_pcie_reset_ep_assert(pcie);
	if (!pcie->std_pwron)
		sky1_pcie_en_ep_power(pcie, false);
	sky1_pcie_en_ep_on(pcie, false);

	return 0;
}

static void sky1_pcie_syscore_op_shutdown(void)
{
	struct sky1_pcie *pcie;

	list_for_each_entry(pcie, &sky1_pcie_list, list) {
		_sky1_pcie_syscore_op_shutdown(pcie);
	}
}

static struct syscore_ops sky1_pcie_syscore_ops = {
	.shutdown = sky1_pcie_syscore_op_shutdown,
};

static void sky1_pcie_x211_enum_lock(struct sky1_pcie *pcie, bool lock)
{
	if (sky1_pcie_is_x211(pcie)) {
		if (lock)
			mutex_lock(&sky1_x211_mutex);
		else
			mutex_unlock(&sky1_x211_mutex);
	}
}

static int sky1_pcie_really_probe(void *p)
{
	struct sky1_pcie *pcie = p;
	struct cdns_pcie_rc *rc = pcie->cdns_pcie_rc;
	struct cdns_pcie *cdns_pcie = pcie->cdns_pcie;
	struct device *dev = pcie->dev;
	int ret = 0;
	unsigned int attempt;

	/*
	 * Y4: after a warm reboot the endpoint/PHY is occasionally left in a
	 * state where one reset-and-train pass fails (observed on a0d0000 /
	 * RTL8127: the whole card disappeared for the boot). Do not abandon
	 * the controller on the first failure: retry the complete sequence,
	 * re-asserting PERST between attempts so the endpoint is function
	 * reset as well, before giving up for this boot.
	 */
	for (attempt = 0; attempt < 3u; attempt++) {
		if (attempt) {
			/* Unwind the failed attempt so the next pass starts
			 * from a clean PHY and a re-asserted PERST. The IRQs
			 * of the previous pass are devm-requested and must be
			 * released before they are requested again.
			 */
			sky1_pcie_enable_irq(pcie, false);
			sky1_pcie_free_irq(pcie);
			phy_power_off(pcie->pcie_phy);
			phy_exit(pcie->pcie_phy);
			/* start_link already deasserted PERST on the failed
			 * attempt; assert it again before the next reset.
			 */
			sky1_pcie_reset_ep_assert(pcie);
			msleep(100);
			dev_info(dev, "sky1 pcie: retrying complete reset sequence, attempt %u/3\n",
				 attempt + 1);
		}
		sky1_pcie_clear_macro_pwr_en(pcie);
		sky1_pcie_x211_enum_lock(pcie, true);
		ret = sky1_pcie_reset(pcie);
		if (ret < 0) {
			sky1_pcie_x211_enum_lock(pcie, false);
			continue;
		}
		sky1_pcie_x211_enum_lock(pcie, false);
		ret = sky1_pcie_init(pcie);
		if (ret)
			break;
		sky1_pcie_get_linkctrl_offset(pcie);
		sky1_pcie_set_linksta_slc(pcie);

		ret = sky1_pcie_request_irq(pcie);
		if (ret < 0) {
			dev_err(dev, "request sky1 irq failed\n");
			continue;
		}
		sky1_pcie_enable_irq(pcie, true);

		ret = cdns_pcie_host_setup(rc);
		if (ret < 0) {
			/* Retry only a training failure: with the link still
			 * down, host_setup bailed out inside start_link /
			 * wait_for_link and no host resources were set up
			 * yet. A failure with the link already up is a
			 * host-setup error and is not retryable here.
			 */
			if (sky1_pcie_link_up(cdns_pcie))
				break;
			continue;
		}
		break;
	}
	if (ret < 0)
		goto err_ecam_free;

	pcie->cfg_base = rc->cfg_base;
	pcie->reg_base = cdns_pcie->reg_base;
	pcie->is_probe = true;
	sky1_pcie_aer_root_command(pcie, true);

	mutex_lock(&sky1_init_mutex);
	/* Register for syscore ops only when first instance probed */
	if (list_empty(&sky1_pcie_list))
		register_syscore_ops(&sky1_pcie_syscore_ops);

	/*
	 * Add the qcom_pcie list of each PCIe instance probed to
	 * the global list so that we use it iterate through each PCIe
	 * instance in the syscore ops.
	 */
	list_add_tail(&pcie->list, &sky1_pcie_list);
	mutex_unlock(&sky1_init_mutex);
	dev_info(dev, "%s end!\n", __func__);
	return 0;

err_ecam_free:
	device_release_driver(dev);
	return ret;
}

static int sky1_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct pci_host_bridge *bridge;
	struct sky1_pcie *pcie;
	struct sky1_pcie_data *data;
	struct cdns_pcie_rc *rc;
	struct cdns_pcie *cdns_pcie;
	struct resource_entry *bus;
	struct task_struct *tsk;
	int ret;

	dev_info(dev, "%s starting!\n", __func__);
	pcie = devm_kzalloc(dev, sizeof(*pcie), GFP_KERNEL);
	if (!pcie)
		return -ENOMEM;

	data = (struct sky1_pcie_data *)device_get_match_data(dev);
	if (!data)
		return -EINVAL;

	pcie->data = data;
	pcie->mode = (u32)data->mode;
	pcie->dev = dev;
	INIT_WORK(&pcie->pme_work, sky1_pcie_pme_work_fn);
	mutex_init(&pcie->pme_mutex);
	mutex_init(&pcie->aer_lock);
	dev_set_drvdata(dev, pcie);

	if (pcie->mode == PCI_MODE_RC) {
		if (!IS_ENABLED(CONFIG_PCIE_CADENCE_HOST)) {
			return -ENODEV;
		}
	}

	ret = sky1_pcie_parse_ep_pwr_supply(pcie);
	if (ret)
		return ret;

	/* Resolve the slot EN provider before touching power or clocks. */
	pcie->slot_power = devm_gpiod_get_array_optional(dev, "myt,slot-power",
							 GPIOD_ASIS);
	if (IS_ERR(pcie->slot_power))
		return dev_err_probe(dev, PTR_ERR(pcie->slot_power),
				     "failed to get downstream slot power GPIOs");

	pcie->slot_presence_gates = devm_gpiod_get_array_optional(dev,
					"myt,slot-presence-gate", GPIOD_ASIS);
	if (IS_ERR(pcie->slot_presence_gates))
		return dev_err_probe(dev, PTR_ERR(pcie->slot_presence_gates),
				     "failed to get slot presence gate GPIOs");

	sky1_pcie_debugfs_init(pcie);

	if (!ACPI_COMPANION(dev))
		bridge = devm_pci_alloc_host_bridge(dev, sizeof(*rc));
	else
		bridge = devm_acpi_pci_alloc_host_bridge(dev, sizeof(*rc));
	if (!bridge)
		return -ENOMEM;

	bus = resource_list_first_type(&bridge->windows, IORESOURCE_BUS);
	if (!bus)
		return -ENODEV;

	ret = sky1_pcie_parse_property(pdev, pcie);
	if (ret < 0)
		return ret;

	if (device_property_present(dev, "sky1,fixed-mps")) {
		u32 mps;

		ret = device_property_read_u32(dev, "sky1,fixed-mps", &mps);
		if (ret || mps != 256 || pcie->max_payload != mps)
			return dev_err_probe(dev, -EINVAL,
					     "fixed MPS/MRRS requires max-payload = 256\n");
		bridge->fixed_mps = mps;
		dev_info(dev, "fixed fabric MPS/MRRS %u/%u\n", mps, mps);
	}

	ret = sky1_pcie_safe_power_cycle(pcie);
	if (ret < 0)
		return ret;

	ret = sky1_pcie_en_ep_on(pcie, true);
	if (ret < 0)
		goto err_vsupply_3v3;

	sky1_pcie_ctrl_set_refclk_b_en(pcie, true);
	sky1_pcie_reset_ep_assert(pcie);

	/* Enable slot supplies and connect x2 presence before releasing PERST#. */
	ret = sky1_pcie_set_slot_lines(dev, pcie->slot_power,
				      "downstream slot EN", 1);
	if (ret)
		goto err_epsupply;
	ret = sky1_pcie_set_slot_lines(dev, pcie->slot_presence_gates,
				      "x2 slot presence gates", 1);
	if (ret)
		goto err_epsupply;

	pm_runtime_enable(dev);
	ret = pm_runtime_get_sync(dev);
	if (ret < 0) {
		dev_err(dev, "pm_runtime_get_sync failed\n");
		goto err_epsupply;
	}

	if (ACPI_COMPANION(dev)) /* conflict in pci_ecam_create */
		remove_resource(pcie->cfg_res);
	pcie->cfg = pci_ecam_create(dev, pcie->cfg_res, bus->res,
				    &pci_generic_ecam_ops);
	if (IS_ERR(pcie->cfg)) {
		ret = PTR_ERR(pcie->cfg);
		goto err_get_sync;
	}
#ifdef CONFIG_ACPI
	if (ACPI_COMPANION(dev))
		pcie->cfg->parent = &ACPI_COMPANION(dev)->dev;
#endif

	/* Dedicated platform IRQs own AER; keep native device reporting. */
	bridge->custom_aer = true;
	bridge->ops = &sky1_pcie_own_ops;
	bridge->child_ops = (struct pci_ops *)&pci_generic_ecam_ops.pci_ops;
	rc = pci_host_bridge_priv(bridge);
	rc->ecam_support_flag = pcie->ecam_support_flag;
	rc->id = pcie->id;
	rc->cfg_base = pcie->cfg->win;
	rc->cfg_res = &pcie->cfg->res;

	cdns_pcie = &rc->pcie;
	cdns_pcie->dev = dev;
	cdns_pcie->ops = &sky1_pcie_ops;
	cdns_pcie->reg_base = pcie->reg_base;
	cdns_pcie->plat_emu = (pcie->plat == PCIE_PLAT_EMU) ? true : false;
	cdns_pcie->msg_res = pcie->msg_res;

	pcie->cdns_pcie = cdns_pcie;
	pcie->cdns_pcie_rc = rc;
	pcie->cfg_base = rc->cfg_base;

	bridge->sysdata = pcie->cfg;
	//DEBUG
	if ((pcie->desc->id != PCIE_ID_x4) && (pcie->plat == PCIE_PLAT_FPGA))
		return 0;

	ret = sky1_pcie_ctrl_set_axi_clk_en(pcie, true);
	if (ret < 0)
		goto err_ecam_free;

	ret = sky1_pcie_ctrl_set_apb_clk_en(pcie, true);
	if (ret < 0)
		goto err_set_clk;

	tsk = kthread_run(sky1_pcie_really_probe, pcie, "sky1-pcie");
	if (IS_ERR(tsk)) {
		dev_err(dev, "start sky1-pcie thread failed\n");
		goto err_th;
	}

	return 0;
err_th:
	sky1_pcie_ctrl_set_apb_clk_en(pcie, false);
err_set_clk:
	sky1_pcie_ctrl_set_axi_clk_en(pcie, false);
err_ecam_free:
	pci_ecam_free(pcie->cfg);
err_get_sync:
	pm_runtime_put(dev);
	pm_runtime_disable(dev);
err_epsupply:
	sky1_pcie_set_slot_lines(dev, pcie->slot_presence_gates,
				 "x2 slot presence gates", 0);
	sky1_pcie_set_slot_lines(dev, pcie->slot_power,
				 "downstream slot EN", 0);
	sky1_pcie_en_ep_on(pcie, false);
	sky1_pcie_ctrl_set_refclk_b_en(pcie, false);
	sky1_pcie_reset_ep_assert(pcie);
err_vsupply_3v3:
	sky1_pcie_en_ep_power(pcie, false);
	sky1_pcie_debugfs_exit(pcie);
	return ret;
}

static int sky1_pcie_remove(struct platform_device *pdev)
{
	struct sky1_pcie *pcie = platform_get_drvdata(pdev);
	struct device *dev = &pdev->dev;

	sky1_pcie_enable_irq(pcie, false);
	sky1_pcie_free_irq(pcie);
	pci_ecam_free(pcie->cfg);
	pm_runtime_put(dev);
	pm_runtime_disable(dev);
	sky1_pcie_debugfs_exit(pcie);
	sky1_pcie_en_ep_on(pcie, false);
	sky1_pcie_en_ep_power(pcie, false);

	/*
	 * Serialize x211 shared-PHY teardown against siblings' reset. clear
	 * + finish print must stay x211-only: non-x211 remove used to clear
	 * shared a/d while an x211 was between assert and deassert.
	 */
	sky1_pcie_x211_enum_lock(pcie, true);
	phy_power_off(pcie->pcie_phy);
	phy_exit(pcie->pcie_phy);
	sky1_pcie_ctrl_set_axi_clk_en(pcie, false);
	sky1_pcie_ctrl_set_apb_clk_en(pcie, false);
	sky1_pcie_ctrl_set_refclk_b_en(pcie, false);
	sky1_pcie_reset_ep_assert(pcie);
	if (sky1_pcie_is_x211(pcie)) {
		mutex_lock(&sky1_init_mutex);
		atomic_dec(&x211_phy_rst_finish_cnt);
		mutex_unlock(&sky1_init_mutex);
	}
	sky1_pcie_set_pwr_en(pcie, false);
	sky1_pcie_ctrl_set_rstn(pcie, false);
	if (!sky1_pcie_is_x211(pcie))
		sky1_pcie_phy_rst(pcie, 0x0);
	sky1_pcie_set_x211_phy_rst_assert(pcie);

	if (sky1_pcie_is_x211(pcie)) {
		mutex_lock(&sky1_init_mutex);
		if (atomic_read(&x211_phy_rst_finish_cnt) == 0)
			sky1_pcie_clear_x211_cnt();
		mutex_unlock(&sky1_init_mutex);
	}
	sky1_pcie_set_refclk(pcie, false);
	sky1_pcie_x211_enum_lock(pcie, false);

	if (pcie->is_probe) {
		list_del(&pcie->list);
		if (list_empty(&sky1_pcie_list))
			unregister_syscore_ops(&sky1_pcie_syscore_ops);
	}

	return 0;
}

static const struct sky1_pcie_data sky1_pcie_rc_data = {
	.mode = PCI_MODE_RC,
	.desc = &sky1_pcie_desc[0],
};

static const struct sky1_pcie_data sky1_pcie_ep_data = {
	.mode = PCI_MODE_EP,
	.desc = &sky1_pcie_desc[0],
};

static const struct of_device_id of_sky1_pcie_match[] = {
	{
		.compatible = "cix,sky1-pcie-host",
		.data = &sky1_pcie_rc_data,
	},
	{
		.compatible = "cix,sky1-pcie-ep",
		.data = &sky1_pcie_ep_data,
	},
	{},
};

static const struct acpi_device_id acpi_sky1_pcie_match[] = {
	{ "CIXH2020", (kernel_ulong_t)&sky1_pcie_rc_data },
	{ "CIXH2021", (kernel_ulong_t)&sky1_pcie_ep_data },
	{},
};
MODULE_DEVICE_TABLE(acpi, acpi_sky1_pcie_match);

#ifdef CONFIG_PM_SLEEP
static int sky1_pcie_suspend_noirq(struct device *dev)
{
	struct sky1_pcie *pcie = dev_get_drvdata(dev);
	int ret = 0;

	if (!pcie->is_probe)
		return 0;

	sky1_pcie_refresh_x8_gpu_resume_delay(pcie);

	sky1_pcie_pme_turn_off(pcie);
	sky1_pcie_stop_link(pcie->cdns_pcie);
	sky1_pcie_set_link_disable(pcie);
	sky1_pcie_reset_ep_assert(pcie);
	phy_power_off(pcie->pcie_phy);
	phy_exit(pcie->pcie_phy);
	sky1_pcie_ctrl_set_axi_clk_en(pcie, false);
	sky1_pcie_ctrl_set_apb_clk_en(pcie, false);
	if (sky1_pcie_is_x211(pcie)) {
		mutex_lock(&sky1_init_mutex);
		atomic_dec(&x211_phy_rst_finish_cnt);
		if (atomic_read(&x211_phy_rst_finish_cnt) == 0)
			sky1_pcie_clear_x211_cnt();
		mutex_unlock(&sky1_init_mutex);
	}
	if (!pcie->str_pwron)
		sky1_pcie_en_ep_power(pcie, false);

	dev_info(dev, "%s\n", __func__);
	return ret;
}

extern int cdns_pcie_host_restore(struct cdns_pcie_rc *rc);

static int sky1_pcie_power_check(struct device *dev)
{
	acpi_handle handle, pwhandle;
	acpi_status status;

	if (!dev || !ACPI_COMPANION(dev)) /* only for acpi boot */
		return 0;

	handle = ACPI_HANDLE(dev);

	status = acpi_get_handle(handle, "PWON", &pwhandle);
	if (ACPI_FAILURE(status))
		return 0;

	status = acpi_evaluate_object(handle, "PWON", NULL, NULL);
	if (ACPI_FAILURE(status)) {
		dev_err(dev, "power on fail, acpi status = %d\n", status);
		return -EFAULT;
	}

	return 0;
}

static int sky1_pcie_resume_noirq(struct device *dev)
{
	struct sky1_pcie *pcie = dev_get_drvdata(dev);
	struct cdns_pcie_rc *rc = pcie->cdns_pcie_rc;
	int ret = 0;

	if (pcie->aer_failed)
		return -EIO;

	if (!pcie->is_probe)
		return 0;

	/*
	 * Some discrete GPUs need extra time after SoC wake before slot power
	 * and link training; only when an x8 display device was seen downstream.
	 */
	if (pcie->x8_vga_resume_delay)
		msleep(SKY1_PCIE_X8_GPU_RESUME_MS);

	if (!pcie->str_pwron)
		sky1_pcie_en_ep_power(pcie, true);

	ret = sky1_pcie_power_check(dev);
	if (ret < 0)
		return ret;

	sky1_pcie_ctrl_set_axi_clk_en(pcie, true);
	sky1_pcie_ctrl_set_apb_clk_en(pcie, true);
	dev_info(dev, "Read STRAP_REG(0) = :0x%x\n",
		 readl(pcie->strap_base + STRAP_REG(0)));

	ret = sky1_pcie_reset(pcie);
	if (ret < 0)
		return ret;

	ret = sky1_pcie_init(pcie);
	if (ret)
		return ret;
	sky1_pcie_set_linksta_slc(pcie);

	ret = cdns_pcie_host_restore(rc);
	if (ret < 0)
		dev_err(dev, "cdns_pcie_host_restore err, err=%d\n", ret);

	sky1_pcie_enable_irq(pcie, true);
	if (!ret)
		sky1_pcie_aer_root_command(pcie, true);
	dev_info(dev, "%s end\n", __func__);
	return ret;
}
#endif

const struct dev_pm_ops sky1_pcie_pm_ops = { SET_NOIRQ_SYSTEM_SLEEP_PM_OPS(
	sky1_pcie_suspend_noirq, sky1_pcie_resume_noirq) };

static struct platform_driver sky1_pcie_driver = {
	.probe  = sky1_pcie_probe,
	.remove = sky1_pcie_remove,
	.shutdown = sky1_pcie_shutdown,
	.driver = {
		.name	= "sky1-pcie",
		.of_match_table = of_sky1_pcie_match,
		.acpi_match_table = acpi_sky1_pcie_match,
		.suppress_bind_attrs = true,
		.pm	= &sky1_pcie_pm_ops,
	},
};
builtin_platform_driver(sky1_pcie_driver);
