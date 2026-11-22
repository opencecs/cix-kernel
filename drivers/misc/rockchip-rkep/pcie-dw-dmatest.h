/* SPDX-License-Identifier: GPL-2.0+ OR MIT */
/*
 * Copyright (C) 2022 Rockchip Electronics Co., Ltd.
 */
#ifndef __PCIE_DW_DMATEST_H
#define __PCIE_DW_DMATEST_H

struct dma_trx_obj;
struct device;

#if 1 /* Always built-in for standalone KO, original: IS_ENABLED(CONFIG_PCIE_DW_DMATEST) */
struct dma_trx_obj *pcie_dw_dmatest_register(struct device *dev, bool irq_en);
int pcie_dw_dmatest_publish(struct dma_trx_obj *obj);
void pcie_dw_dmatest_unregister(struct dma_trx_obj *obj);
void pcie_dw_dmatest_irq_abort(struct dma_trx_obj *obj);
bool pcie_dw_dmatest_failed(struct dma_trx_obj *obj);
int pcie_dw_wired_dma_frombus_block(struct dma_trx_obj *obj, u32 chn, u64 local_paddr, u64 bus_paddr, u32 size);
int pcie_dw_wired_dma_tobus_block(struct dma_trx_obj *obj, u32 chn, u64 bus_paddr, u64 local_paddr, u32 size);
int pcie_dw_dmatest_irq_en(struct dma_trx_obj *obj, bool wr_irq_en, bool rd_irq_en);
int pcie_dw_local_dma_frombus_block(struct dma_trx_obj *obj, u32 chn,
				    u64 local_paddr, u64 bus_paddr, u32 size);
int pcie_dw_local_dma_tobus_block(struct dma_trx_obj *obj, u32 chn,
				  u64 bus_paddr, u64 local_paddr, u32 size);
int pcie_dw_dmatest_set_bandwidth(struct dma_trx_obj *obj, u8 gen, u8 lanes);
#else
static inline struct dma_trx_obj *pcie_dw_dmatest_register(struct device *dev, bool irq_en)
{
	return ERR_PTR(-EOPNOTSUPP);
}

static inline int pcie_dw_dmatest_publish(struct dma_trx_obj *obj) { return -EOPNOTSUPP; }

static inline void pcie_dw_dmatest_unregister(struct dma_trx_obj *obj) { }
static inline void pcie_dw_dmatest_irq_abort(struct dma_trx_obj *obj) { }
static inline bool pcie_dw_dmatest_failed(struct dma_trx_obj *obj) { return false; }

static inline int pcie_dw_wired_dma_frombus_block(struct dma_trx_obj *obj, u32 chn, u64 local_paddr, u64 bus_paddr, u32 size)
{
	return -ENODEV;
}

static inline int pcie_dw_wired_dma_tobus_block(struct dma_trx_obj *obj, u32 chn, u64 bus_paddr, u64 local_paddr, u32 size)
{
	return -ENODEV;
}

static inline int pcie_dw_dmatest_irq_en(struct dma_trx_obj *obj, bool wr_irq_en, bool rd_irq_en)
{
	return -ENODEV;
}

static inline int pcie_dw_local_dma_frombus_block(struct dma_trx_obj *obj, u32 chn, u64 local_paddr, u64 bus_paddr, u32 size)
{
	return -ENODEV;
}

static inline int pcie_dw_local_dma_tobus_block(struct dma_trx_obj *obj, u32 chn, u64 bus_paddr, u64 local_paddr, u32 size)
{
	return -ENODEV;
}

static inline int pcie_dw_dmatest_set_bandwidth(struct dma_trx_obj *obj, u8 gen, u8 lanes)
{
	return -ENODEV;
}
#endif

#endif
