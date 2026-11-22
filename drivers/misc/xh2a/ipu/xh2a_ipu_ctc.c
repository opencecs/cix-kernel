// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2024 Houmo AI Inc.
 * Author: hongxing.ma<hongxing.ma@houmo.ai>
 */

#include <linux/module.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <xh2a_host_drv.h>
#include <xh2a_pcie_api.h>
#include "xh2a_ipu_device.h"
#include "xh2a_ipu_ctc.h"

/* The register sequence is unchanged; its ownership is now the IPU client. */
#define xh2a_sys_dev xh2a_ipu_device
#define sys_mutex ctc_mutex
#define CTC_UNINIT_VAL XH2A_CTC_UNINIT_VAL

#define CTCSS_NUM	 4
#define LANE_NUM	 2
#define CTC_MARGIN_STEPS 9

#define CTCSS0_CFG_BASE 0x54000000UL
#define CTCSS1_CFG_BASE 0x54800000UL
#define CTCSS2_CFG_BASE 0x55000000UL
#define CTCSS3_CFG_BASE 0x55800000UL

#define CTC_MAC_PHY_BLOCK	0x0
#define CTC_MAC_PHY_TOP_OFFSET	0x0
#define CTC_MAC_PHY_MAC0_OFFSET 0x40000UL
#define CTC_MAC_PHY_MAC1_OFFSET 0x80000UL
#define CTC_MAC_PHY_PHY_OFFSET	0xC0000UL

#define CTC_CTRL0_BLOCK 0x200000UL
#define CTC_CTRL1_BLOCK 0x300000UL

/* TOP CSR OFFSET */
#define CRG_CTRL_OFFSET		 0x4
#define LOW_POWER_CTRL_OFFSET	 0x14
#define POWER_CTRL_STATUS_OFFSET 0x114
#define SRAM_CTRL_STATUS_OFFSET	 0x11C

/* TOP CSR BIT */
#define SRAM_ROM_SD	      BIT(2)
#define PMA_ISOLATE_RX_SIGDET BIT(1)
#define PMA_ISOLATE_EN	      BIT(0)
#define PMA_PWR_EN	      BIT(14)
#define PHY_SRAM_BYPASS	      BIT(1)
#define PHY_SRAM_INIT_DONE    BIT(4)
#define PHY_SRAM_EXT_LD_DONE  BIT(2)

#define PIPE_LANE0_PCLK_EN  BIT(0)
#define PIPE_LANE1_PCLK_EN  BIT(1)
#define PHY_RESET	    BIT(2)
#define PIPE_LANE01_PERST_N BIT(3)
#define PIPE_LANE0_RESET_N  BIT(4)
#define PIPE_LANE1_RESET_N  BIT(5)
#define PIPE_LANE0_MAC_RSTN BIT(6)
#define PIPE_LANE1_MAC_RSTN BIT(7)
#define PIPE_LANE0_CSR_RSTN BIT(8)
#define PIPE_LANE1_CSR_RSTN BIT(9)

/* MAC CSR OFFSET */
#define COMMON_CONTROL0_OFFSET 0x0
#define COMMON_CONTROL1_OFFSET 0x4
#define COMMON_STATUS0_OFFSET  0x10
#define COMMON_STATUS1_OFFSET  0x14
#define MARGIN_CONTROL0_OFFSET 0xF0
#define MARGIN_CONTROL2_OFFSET 0xF8
#define MARGIN_STATUS_OFFSET   0x100

/* MAC CSR BIT */
#define PIPE_LANE_PHYSTATUS BIT(31)
#define UPSTREAM_PORT	    BIT(0)
#define LTSSM_ENABLE	    BIT(2)

/* AOSS */
#define AOSS_SYSCTRL_CTC_CHIPID_OFFSET	  0x70000104
#define AOSS_LCRG_NOC_CLK_OFFSET	  0x70001044
#define AOSS_LCRG_SOFT_RESET_ASYNC_OFFSET 0x7000103C
#define AOSS_NIU_CTC_CTRL_BASE		  0x70030000
#define AOSS_NIU_CTC_STATUS_BASE	  0x70030004
#define CTC_NIU_MASK			  0xffffffC0
#define CTC_NIU_CONNECT			  0x2AAAAA80

/* MULTI-CHIP */
#define CTCSS_LEFT_TX_ADDR  (CTCSS0_CFG_BASE + CTC_CTRL0_BLOCK)
#define CTCSS_RIGHT_TX_ADDR (CTCSS0_CFG_BASE + 2 * 0x800000UL + CTC_CTRL0_BLOCK)
#define CTCSS_LEFT_RX_ADDR  (CTCSS0_CFG_BASE + CTC_CTRL0_BLOCK + 0x4)
#define CTCSS_RIGHT_RX_ADDR \
	(CTCSS0_CFG_BASE + 2 * 0x800000UL + CTC_CTRL0_BLOCK + 0x4)
#define CTC_ROUTE_MAGIC 0xABC0

#define CTC_INIT_TIMEOUT 100
#define CTC_CLOCK_UNABLE 0xCCCC

static int ctcss_phy_init(struct xh2a_sys_dev *sys_dev, int ctcss_id)
{
	uint64_t top_base, mac_base;
	uint32_t reg_val, lane_idx;
	int timeout = CTC_INIT_TIMEOUT;

	top_base =
		CTCSS0_CFG_BASE + ctcss_id * 0x800000 + CTC_MAC_PHY_TOP_OFFSET;
	mac_base =
		CTCSS0_CFG_BASE + ctcss_id * 0x800000 + CTC_MAC_PHY_MAC0_OFFSET;

	dev_dbg(sys_dev->cdev_dev, "CTCSS%u phyinit.\n", ctcss_id);

	/* step: b.1, set VPH 1.5V */
	xh2a_pcie_pio_readl(sys_dev->private_data,
			    top_base + POWER_CTRL_STATUS_OFFSET, &reg_val);
	reg_val |= 0x20;
	xh2a_pcie_pio_writel(sys_dev->private_data,
			     top_base + POWER_CTRL_STATUS_OFFSET, reg_val);
	dev_dbg(sys_dev->cdev_dev,
		"C: POWER_CTRL_STATUS_OFFSET=0x%llx, val=0x%x\n",
		top_base + POWER_CTRL_STATUS_OFFSET, reg_val);

	/* step: c */
	xh2a_pcie_pio_readl(sys_dev->private_data,
			    top_base + LOW_POWER_CTRL_OFFSET, &reg_val);
	reg_val &= ~SRAM_ROM_SD;
	xh2a_pcie_pio_writel(sys_dev->private_data,
			     top_base + LOW_POWER_CTRL_OFFSET, reg_val);
	dev_dbg(sys_dev->cdev_dev,
		"C: LOW_POWER_CTRL_OFFSET=0x%llx, val=0x%x\n",
		top_base + LOW_POWER_CTRL_OFFSET, reg_val);
	/* step: d */
	reg_val &= ~PMA_ISOLATE_RX_SIGDET;
	xh2a_pcie_pio_writel(sys_dev->private_data,
			     top_base + LOW_POWER_CTRL_OFFSET, reg_val);
	dev_dbg(sys_dev->cdev_dev,
		"D: LOW_POWER_CTRL_OFFSET=0x%llx, val=0x%x\n",
		top_base + LOW_POWER_CTRL_OFFSET, reg_val);
	/* step: e */
	reg_val &= ~PMA_ISOLATE_EN;
	xh2a_pcie_pio_writel(sys_dev->private_data,
			     top_base + LOW_POWER_CTRL_OFFSET, reg_val);
	dev_dbg(sys_dev->cdev_dev,
		"E: LOW_POWER_CTRL_OFFSET=0x%llx, val=0x%x\n",
		top_base + LOW_POWER_CTRL_OFFSET, reg_val);
	/* step: f */
	xh2a_pcie_pio_readl(sys_dev->private_data,
			    top_base + POWER_CTRL_STATUS_OFFSET, &reg_val);
	while (!(reg_val & PMA_PWR_EN)) {
		xh2a_pcie_pio_readl(sys_dev->private_data,
				    top_base + POWER_CTRL_STATUS_OFFSET,
				    &reg_val);
		usleep_range(1000, 1000);
		if (--timeout <= 0) {
			sys_dev->ctc_fail_info |= 1U << ((ctcss_id / 2) * 16);
			dev_err(sys_dev->cdev_dev,
				"CTCSS%u PMA power ready timeout: status=0x%x\n",
				ctcss_id, reg_val);
			return -ETIMEDOUT;
		}
	}
	timeout = CTC_INIT_TIMEOUT;
	dev_dbg(sys_dev->cdev_dev,
		"F: POWER_CTRL_STATUS_OFFSET=0x%llx, val=0x%x\n",
		top_base + POWER_CTRL_STATUS_OFFSET, reg_val);
	/* step: g */

	xh2a_pcie_pio_readl(sys_dev->private_data,
			    top_base + SRAM_CTRL_STATUS_OFFSET, &reg_val);
	reg_val &= ~PHY_SRAM_BYPASS;
	/* reg_val |= PHY_SRAM_BYPASS; */
	xh2a_pcie_pio_writel(sys_dev->private_data,
			     top_base + SRAM_CTRL_STATUS_OFFSET, reg_val);
	dev_dbg(sys_dev->cdev_dev,
		"G: SRAM_CTRL_STATUS_OFFSET=0x%llx, val=0x%x\n",
		top_base + SRAM_CTRL_STATUS_OFFSET, reg_val);
	/* step: h */
	xh2a_pcie_pio_readl(sys_dev->private_data, top_base + CRG_CTRL_OFFSET,
			    &reg_val);
	reg_val &= ~PHY_RESET;
	xh2a_pcie_pio_writel(sys_dev->private_data, top_base + CRG_CTRL_OFFSET,
			     reg_val);
	dev_dbg(sys_dev->cdev_dev, "H: CRG_CTRL_OFFSET=0x%llx, val=0x%x\n",
		top_base + CRG_CTRL_OFFSET, reg_val);
	/* step: i */
	usleep_range(1, 1);
	dev_dbg(sys_dev->cdev_dev, "I: Udelay 1\n");
	/* step: j */
	xh2a_pcie_pio_readl(sys_dev->private_data, top_base + CRG_CTRL_OFFSET,
			    &reg_val);
	reg_val |= PIPE_LANE01_PERST_N;
	xh2a_pcie_pio_writel(sys_dev->private_data, top_base + CRG_CTRL_OFFSET,
			     reg_val);
	dev_dbg(sys_dev->cdev_dev, "J: CRG_CTRL_OFFSET=0x%llx, val=0x%x\n",
		top_base + CRG_CTRL_OFFSET, reg_val);
	/* step: k */
	usleep_range(1, 1);
	dev_dbg(sys_dev->cdev_dev, "K: Udelay 1\n");
	/* step: l */
	xh2a_pcie_pio_readl(sys_dev->private_data,
			    top_base + SRAM_CTRL_STATUS_OFFSET, &reg_val);
	while (!(reg_val & PHY_SRAM_INIT_DONE)) {
		xh2a_pcie_pio_readl(sys_dev->private_data,
				    top_base + SRAM_CTRL_STATUS_OFFSET,
				    &reg_val);
		usleep_range(1000, 1000);
		if (--timeout <= 0) {
			sys_dev->ctc_fail_info |= 1U << ((ctcss_id / 2) * 16);
			return CTC_CLOCK_UNABLE;
		}
	}

	timeout = CTC_INIT_TIMEOUT;

	dev_dbg(sys_dev->cdev_dev,
		"L: SRAM_CTRL_STATUS_OFFSET=0x%llx, val=0x%x\n",
		top_base + SRAM_CTRL_STATUS_OFFSET, reg_val);
	/* step: m: if need to change SRAM */
	/* step: n */
	xh2a_pcie_pio_readl(sys_dev->private_data,
			    top_base + SRAM_CTRL_STATUS_OFFSET, &reg_val);
	reg_val |= PHY_SRAM_EXT_LD_DONE;
	xh2a_pcie_pio_writel(sys_dev->private_data,
			     top_base + SRAM_CTRL_STATUS_OFFSET, reg_val);
	dev_dbg(sys_dev->cdev_dev,
		"N: SRAM_CTRL_STATUS_OFFSET=0x%llx, val=0x%x\n",
		top_base + SRAM_CTRL_STATUS_OFFSET, reg_val);
	/* step: o */

	xh2a_pcie_pio_readl(sys_dev->private_data, top_base + CRG_CTRL_OFFSET,
			    &reg_val);
	reg_val |= (PIPE_LANE0_RESET_N | PIPE_LANE1_RESET_N);
	xh2a_pcie_pio_writel(sys_dev->private_data, top_base + CRG_CTRL_OFFSET,
			     reg_val);
	dev_dbg(sys_dev->cdev_dev, "O: CRG_CTRL_OFFSET=0x%llx, val=0x%x\n",
		top_base + CRG_CTRL_OFFSET, reg_val);
	/* step: p */

	xh2a_pcie_pio_readl(sys_dev->private_data, top_base + CRG_CTRL_OFFSET,
			    &reg_val);
	reg_val |= (PIPE_LANE0_CSR_RSTN | PIPE_LANE1_CSR_RSTN);
	xh2a_pcie_pio_writel(sys_dev->private_data, top_base + CRG_CTRL_OFFSET,
			     reg_val);
	dev_dbg(sys_dev->cdev_dev, "P: CRG_CTRL_OFFSET=0x%llx, val=0x%x\n",
		top_base + CRG_CTRL_OFFSET, reg_val);
	/* step: q */
	for (lane_idx = 0; lane_idx < LANE_NUM; lane_idx++) {
		mac_base = CTCSS0_CFG_BASE + ctcss_id * 0x800000 +
			   CTC_MAC_PHY_MAC0_OFFSET + lane_idx * 0x40000;

		xh2a_pcie_pio_readl(sys_dev->private_data,
				    mac_base + COMMON_STATUS0_OFFSET, &reg_val);
		while (reg_val & PIPE_LANE_PHYSTATUS) {
			xh2a_pcie_pio_readl(sys_dev->private_data,
					    mac_base + COMMON_STATUS0_OFFSET,
					    &reg_val);
			usleep_range(1000, 1000);
			if (--timeout <= 0) {
				sys_dev->ctc_fail_info |=
					1U << ((ctcss_id / 2) * 16);
				return -ETIMEDOUT;
			}
		}
		dev_dbg(sys_dev->cdev_dev,
			"Q: COMMON_STATUS0_OFFSET=0x%llx, val=0x%x\n",
			mac_base + COMMON_STATUS0_OFFSET, reg_val);
	}
	/* step: r */

	xh2a_pcie_pio_readl(sys_dev->private_data, top_base + CRG_CTRL_OFFSET,
			    &reg_val);
	reg_val |= (PIPE_LANE0_MAC_RSTN | PIPE_LANE1_MAC_RSTN);
	xh2a_pcie_pio_writel(sys_dev->private_data, top_base + CRG_CTRL_OFFSET,
			     reg_val);
	dev_dbg(sys_dev->cdev_dev, "R: CRG_CTRL_OFFSET=0x%llx, val=0x%x\n",
		top_base + CRG_CTRL_OFFSET, reg_val);
	/* step: s */
	usleep_range(1, 1);
	dev_dbg(sys_dev->cdev_dev, "S: Udelay 1\n");
	/* step: t */

	xh2a_pcie_pio_readl(sys_dev->private_data, top_base + CRG_CTRL_OFFSET,
			    &reg_val);
	reg_val |= (PIPE_LANE0_PCLK_EN | PIPE_LANE1_PCLK_EN);
	xh2a_pcie_pio_writel(sys_dev->private_data, top_base + CRG_CTRL_OFFSET,
			     reg_val);
	dev_dbg(sys_dev->cdev_dev, "T: CRG_CTRL_OFFSET=0x%llx, val=0x%x\n",
		top_base + CRG_CTRL_OFFSET, reg_val);
	/* step: u */
	for (lane_idx = 0; lane_idx < LANE_NUM; lane_idx++) {
		mac_base = CTCSS0_CFG_BASE + ctcss_id * 0x800000 +
			   CTC_MAC_PHY_MAC0_OFFSET + lane_idx * 0x40000;
		xh2a_pcie_pio_readl(sys_dev->private_data,
				    mac_base + COMMON_CONTROL0_OFFSET,
				    &reg_val);
		/* ctcss0/1: RC, ctcss2/3: EP */
		if (ctcss_id < 2)
			reg_val &= ~UPSTREAM_PORT;
		else
			reg_val |= UPSTREAM_PORT; /* 0:RC, 1:EP */

		xh2a_pcie_pio_writel(sys_dev->private_data,
				     mac_base + COMMON_CONTROL0_OFFSET,
				     reg_val);
		dev_dbg(sys_dev->cdev_dev,
			"U: COMMON_CONTROL0_OFFSET=0x%llx, val=0x%x\n",
			mac_base + COMMON_CONTROL0_OFFSET, reg_val);
	}
	/* step: w */
	for (lane_idx = 0; lane_idx < LANE_NUM; lane_idx++) {
		mac_base = CTCSS0_CFG_BASE + ctcss_id * 0x800000 +
			   CTC_MAC_PHY_MAC0_OFFSET + lane_idx * 0x40000;
		xh2a_pcie_pio_readl(sys_dev->private_data,
				    mac_base + COMMON_CONTROL0_OFFSET,
				    &reg_val);
		reg_val |= LTSSM_ENABLE;
		xh2a_pcie_pio_writel(sys_dev->private_data,
				     mac_base + COMMON_CONTROL0_OFFSET,
				     reg_val);
		dev_dbg(sys_dev->cdev_dev,
			"W: COMMON_CONTROL0_OFFSET=0x%llx, val=0x%x\n",
			mac_base + COMMON_CONTROL0_OFFSET, reg_val);
	}

	return 0;
}

static int ctc_phy_init(struct xh2a_sys_dev *sys_dev)
{
	int ret;
	int clk_err_num = 0;
	uint32_t ctcss_id;

	sys_dev->ctc_fail_info = 0;

	for (ctcss_id = 0; ctcss_id < 2; ctcss_id++) {
		ret = ctcss_phy_init(sys_dev, ctcss_id);
		if (ret) {
			if (ret == CTC_CLOCK_UNABLE)
				clk_err_num++;
			break;
		}
	}

	for (ctcss_id = 2; ctcss_id < 4; ctcss_id++) {
		ret = ctcss_phy_init(sys_dev, ctcss_id);
		if (ret) {
			if (ret == CTC_CLOCK_UNABLE)
				clk_err_num++;
			break;
		}
	}

	if (clk_err_num >= 2)
		sys_dev->ctc_is_single = 1;

	return 0;
}

static int ctc_phy_linkup(struct xh2a_sys_dev *sys_dev)
{
	int timeout = CTC_INIT_TIMEOUT;
	uint32_t ctcss_id, start_id = 0, end_id = 4;
	uint64_t mac_base;
	uint32_t reg_val, lane_idx;

	if (sys_dev->ctc_is_single == 1) {
		dev_info(sys_dev->cdev_dev, "single chip, skip ctc linkup\n");
		return 0;
	}

	if ((sys_dev->ctc_fail_info & 0xFFFF) > 0) {
		start_id = 2;
		end_id = 4;
	}

	if ((sys_dev->ctc_fail_info >> 16) > 0) {
		start_id = 0;
		end_id = 2;
	}

	for (ctcss_id = start_id; ctcss_id < end_id; ctcss_id++) {
		if ((sys_dev->ctc_fail_info & 0xF) && ctcss_id == 1)
			continue;

		if ((sys_dev->ctc_fail_info >> 16) && ctcss_id == 3)
			continue;

		for (lane_idx = 0; lane_idx < LANE_NUM; lane_idx++) {
			mac_base = CTCSS0_CFG_BASE + ctcss_id * 0x800000 +
				   CTC_MAC_PHY_MAC0_OFFSET + lane_idx * 0x40000;
			timeout = CTC_INIT_TIMEOUT;
			xh2a_pcie_pio_readl(sys_dev->private_data,
					    mac_base + COMMON_STATUS0_OFFSET,
					    &reg_val);
			while ((reg_val & 0x3) != 0x3) {
				xh2a_pcie_pio_readl(
					sys_dev->private_data,
					mac_base + COMMON_STATUS0_OFFSET,
					&reg_val);
				usleep_range(1000, 1000);
				if (--timeout <= 0) {
					sys_dev->ctc_fail_info |=
						1U << ((ctcss_id / 2) * 16);
					break;
				}
			}

			if (timeout <= 0)
				break;
		}

		if (timeout <= 0)
			continue;

		for (lane_idx = 0; lane_idx < LANE_NUM; lane_idx++) {
			mac_base = CTCSS0_CFG_BASE + ctcss_id * 0x800000 +
				   CTC_MAC_PHY_MAC0_OFFSET + lane_idx * 0x40000;
			timeout = CTC_INIT_TIMEOUT;
			xh2a_pcie_pio_readl(sys_dev->private_data,
					    mac_base + COMMON_STATUS1_OFFSET,
					    &reg_val);
			while ((reg_val & 0x3F) != 0x11) {
				xh2a_pcie_pio_readl(
					sys_dev->private_data,
					mac_base + COMMON_STATUS1_OFFSET,
					&reg_val);
				usleep_range(1000, 1000);
				if (--timeout <= 0) {
					sys_dev->ctc_fail_info |=
						1U << ((ctcss_id / 2) * 16);
					break;
				}
			}

			if (timeout <= 0)
				break;
		}
	}

	return 0;
}

static void ctc_crg_init(struct xh2a_sys_dev *sys_dev,
			 enum xh2a_ipu_ctc_crg_reset_mode reset_mode)
{
	uint32_t reg_val;
	uint32_t reset_mask = 0x3C0000;

	if (reset_mode == XH2A_IPU_CTC_CRG_RESET_PHY_AND_CTRL) {
		/* CTC reset must reset both the PHY and CTC ctrl subsystems. */
		reset_mask = 0x3FFC00;
	}

	/* step: a */
	xh2a_pcie_pio_readl(sys_dev->private_data,
			    AOSS_LCRG_SOFT_RESET_ASYNC_OFFSET, &reg_val);
	reg_val &= ~reset_mask;
	xh2a_pcie_pio_writel(sys_dev->private_data,
			     AOSS_LCRG_SOFT_RESET_ASYNC_OFFSET, reg_val);
	reg_val |= reset_mask;
	xh2a_pcie_pio_writel(sys_dev->private_data,
			     AOSS_LCRG_SOFT_RESET_ASYNC_OFFSET, reg_val);
	dev_dbg(sys_dev->cdev_dev, "A: cfg_soft_rstn_async=0x%x, val=0x%x\n",
		AOSS_LCRG_SOFT_RESET_ASYNC_OFFSET, reg_val);

	/* step: b */
	xh2a_pcie_pio_readl(sys_dev->private_data, AOSS_LCRG_NOC_CLK_OFFSET,
			    &reg_val);
	reg_val |= 0x780000;
	xh2a_pcie_pio_writel(sys_dev->private_data, AOSS_LCRG_NOC_CLK_OFFSET,
			     reg_val);
	dev_dbg(sys_dev->cdev_dev, "B: AOSS_NOC_CLK=0x%x, val=0x%x\n",
		AOSS_LCRG_NOC_CLK_OFFSET, reg_val);
}

static void ctc_niu_connect(struct xh2a_sys_dev *sys_dev)
{
	uint32_t niu_val, trycnt;
	uint32_t niu_ctrl_addr = AOSS_NIU_CTC_CTRL_BASE;
	uint32_t niu_status_addr = AOSS_NIU_CTC_STATUS_BASE;

	dev_info(sys_dev->cdev_dev, "ctc niu connect\n");

	trycnt = 0;
	do {
		xh2a_pcie_pio_readl(sys_dev->private_data, niu_ctrl_addr,
				    &niu_val);
		niu_val |= CTC_NIU_CONNECT;
		xh2a_pcie_pio_writel(sys_dev->private_data, niu_ctrl_addr,
				     niu_val);
		xh2a_pcie_pio_readl(sys_dev->private_data, niu_status_addr,
				    &niu_val);
		trycnt++;
	} while (((niu_val & CTC_NIU_MASK) != 0) && (trycnt < 1000));

	if (trycnt >= 1000) {
		dev_err(sys_dev->cdev_dev, "niu connect failed.\n");
	} else {
		dev_info(sys_dev->cdev_dev, "niu connect ok.\n");
	}
}

int xh2a_ipu_ctc_phy_prepare(
	struct xh2a_ipu_device *sys_dev,
	enum xh2a_ipu_ctc_crg_reset_mode reset_mode)
{
	int ret;

	if (!sys_dev)
		return -EINVAL;

	sys_dev->ctc_fail_info = 0;
	sys_dev->ctc_needs_phy_prepare = true;
	ctc_crg_init(sys_dev, reset_mode);
	ctc_niu_connect(sys_dev);

	ret = ctc_phy_init(sys_dev);
	if (!ret)
		sys_dev->ctc_needs_phy_prepare = false;

	return ret;
}

int xh2a_ipu_ctc_phy_wait_linkup(struct xh2a_ipu_device *sys_dev)
{
	if (!sys_dev)
		return -EINVAL;

	return ctc_phy_linkup(sys_dev);
}

int xh2a_ipu_ctc_phy_init_linkup(struct xh2a_ipu_device *sys_dev)
{
	int ret;

	ret = xh2a_ipu_ctc_phy_prepare(
		sys_dev, XH2A_IPU_CTC_CRG_RESET_PHY_ONLY);
	if (ret)
		return ret;

	return xh2a_ipu_ctc_phy_wait_linkup(sys_dev);
}

void xh2a_ipu_ctc_reinit_inhibit_get(struct xh2a_ipu_device *sys_dev)
{
	if (sys_dev)
		atomic_inc(&sys_dev->ctc_reinit_inhibit);
}

void xh2a_ipu_ctc_reinit_inhibit_put(struct xh2a_ipu_device *sys_dev)
{
	if (sys_dev)
		atomic_dec(&sys_dev->ctc_reinit_inhibit);
}

bool xh2a_ipu_ctc_reinit_inhibited(struct xh2a_ipu_device *sys_dev)
{
	return sys_dev && atomic_read(&sys_dev->ctc_reinit_inhibit) != 0;
}

static void xh2a_ctc_shutdown_locked(struct xh2a_sys_dev *sys_dev)
{
	int ctcss_id;
	uint32_t reg_val, top_base;

	/* step: a */
	xh2a_pcie_pio_readl(sys_dev->private_data,
			    AOSS_LCRG_SOFT_RESET_ASYNC_OFFSET, &reg_val);
	reg_val &= ~0x3C0000;
	xh2a_pcie_pio_writel(sys_dev->private_data,
			     AOSS_LCRG_SOFT_RESET_ASYNC_OFFSET, reg_val);
	reg_val |= 0x3C0000;
	xh2a_pcie_pio_writel(sys_dev->private_data,
			     AOSS_LCRG_SOFT_RESET_ASYNC_OFFSET, reg_val);
	dev_dbg(sys_dev->cdev_dev, "A: cfg_soft_rstn_async=0x%x, val=0x%x\n",
		AOSS_LCRG_SOFT_RESET_ASYNC_OFFSET, reg_val);

	/* step: b */
	xh2a_pcie_pio_readl(sys_dev->private_data, AOSS_LCRG_NOC_CLK_OFFSET,
			    &reg_val);
	reg_val |= 0x780000;
	xh2a_pcie_pio_writel(sys_dev->private_data, AOSS_LCRG_NOC_CLK_OFFSET,
			     reg_val);
	dev_dbg(sys_dev->cdev_dev, "B: AOSS_NOC_CLK=0x%x, val=0x%x\n",
		AOSS_LCRG_NOC_CLK_OFFSET, reg_val);

	/* step: c */
	for (ctcss_id = 0; ctcss_id < CTCSS_NUM; ctcss_id++) {
		top_base = CTCSS0_CFG_BASE + ctcss_id * 0x800000 +
			   CTC_MAC_PHY_TOP_OFFSET;

		xh2a_pcie_pio_readl(sys_dev->private_data,
				    top_base + LOW_POWER_CTRL_OFFSET, &reg_val);
		reg_val |= SRAM_ROM_SD;
		xh2a_pcie_pio_writel(sys_dev->private_data,
				     top_base + LOW_POWER_CTRL_OFFSET, reg_val);
		dev_dbg(sys_dev->cdev_dev,
			"C: LOW_POWER_CTRL_OFFSET=0x%x, val=0x%x\n",
			top_base + LOW_POWER_CTRL_OFFSET, reg_val);
	}
	/*
	 * PM resume skips single-chip devices, while a future topology rebuild
	 * must prepare the PHY again before probing physical links.
	 */
	sys_dev->ctc_needs_phy_prepare = true;
}

void xh2a_ipu_ctc_shutdown_single(struct xh2a_ipu_device *sys_dev)
{
	if (!sys_dev)
		return;

	mutex_lock(&sys_dev->sys_mutex);
	if (sys_dev->ctc_chip_id != CTC_UNINIT_VAL &&
	    sys_dev->ctc_is_single == 1)
		xh2a_ctc_shutdown_locked(sys_dev);
	mutex_unlock(&sys_dev->sys_mutex);
}

void xh2a_ipu_ctc_pm_shutdown(struct xh2a_ipu_device *sys_dev)
{
	if (!sys_dev)
		return;

	mutex_lock(&sys_dev->sys_mutex);
	if (sys_dev->ctc_chip_id != CTC_UNINIT_VAL &&
	    sys_dev->ctc_is_single != 1)
		xh2a_ctc_shutdown_locked(sys_dev);
	mutex_unlock(&sys_dev->sys_mutex);
}

/*
 * xh2a_ctc_pm_startup
 *	start CTC while the host resumes from system/runtime low power.
 *	return: 0 success, others failed.
 */
int xh2a_ipu_ctc_pm_startup(struct xh2a_ipu_device *sys_dev)
{
	struct xh2a_ctl_ctx *ctl;
	int ret;

	if (!sys_dev)
		return -EINVAL;
	if (xh2a_ipu_ctc_reinit_inhibited(sys_dev))
		return 0;

	ret = xh2a_ctl_get_ctx(&ctl);
	if (ret)
		return ret;

	xh2a_ctl_ctc_lifecycle_lock(ctl);
	if (xh2a_ipu_ctc_reinit_inhibited(sys_dev)) {
		ret = 0;
		goto out_lifecycle_unlock;
	}

	mutex_lock(&sys_dev->sys_mutex);

	/* if ctc not initialized or single-chip, return */
	if (sys_dev->ctc_chip_id == CTC_UNINIT_VAL ||
	    sys_dev->ctc_is_single == 1) {
		ret = 0;
		goto out_unlock;
	}

	ret = xh2a_ipu_ctc_phy_init_linkup(sys_dev);
	if (ret) {
		dev_err(sys_dev->cdev_dev, "%s: ctc PM startup failed ret=%d\n",
			__func__, ret);
		goto out_unlock;
	}

	xh2a_pcie_pio_writel(sys_dev->private_data,
			     AOSS_SYSCTRL_CTC_CHIPID_OFFSET,
			     sys_dev->ctc_chip_id);

out_unlock:
	mutex_unlock(&sys_dev->sys_mutex);
	out_lifecycle_unlock:
	xh2a_ctl_ctc_lifecycle_unlock(ctl);
	return ret;
}
