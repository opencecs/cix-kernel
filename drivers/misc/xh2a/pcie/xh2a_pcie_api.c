// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2024 Houmo AI Inc.
 * Author: hongxing.ma<hongxing.ma@houmo.ai>
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/uaccess.h>
#include <linux/pm_runtime.h>

#include <xh2a_host_drv.h>
#include <xh2a_pcie_api.h>
#include <version_compat.h>
#include "xh2a_pcie.h"

static inline int xh2a_pcie_iatu_bar_map(struct xh2a_pcie_dev *p_xh2a,
					 uint64_t base, int barid)
{
	int retries = 0;
	uint32_t val;

	writel((barid << 8),
	       XH2A_PCIE_IATU_IB_REG(barid, PCIE_ATU_UNR_REGION_CTRL2));
	writel((base & 0xffffffff),
	       XH2A_PCIE_IATU_IB_REG(barid, PCIE_ATU_UNR_LOWER_TARGET));
	writel(((base >> 32) & 0xffffffff),
	       XH2A_PCIE_IATU_IB_REG(barid, PCIE_ATU_UNR_UPPER_TARGET));
	writel((0 | PCIE_ATU_FUNC_NUM(0)),
	       XH2A_PCIE_IATU_IB_REG(barid, PCIE_ATU_UNR_REGION_CTRL1));
	writel((PCIE_ATU_ENABLE | PCIE_ATU_BAR_MODE_ENABLE | (barid << 8)),
	       XH2A_PCIE_IATU_IB_REG(barid, PCIE_ATU_UNR_REGION_CTRL2));

	do {
		val = readl(XH2A_PCIE_IATU_IB_REG(barid,
						  PCIE_ATU_UNR_REGION_CTRL2));
		retries++;
	} while ((retries < 100) && !(val & PCIE_ATU_ENABLE));

	if (retries < 100)
		return 0;

	return -1;
}

static inline void xh2a_pcie_iatu_bar_unmap(struct xh2a_pcie_dev *p_xh2a,
					    int barid)
{
	writel((barid << 8),
	       XH2A_PCIE_IATU_IB_REG(barid, PCIE_ATU_UNR_REGION_CTRL2));
}

#define XH2A_RAW_COPY_DIR_HOST_TO_DEV 0
#define XH2A_RAW_COPY_DIR_DEV_TO_HOST 1

static inline void raw_copy(char *data, char *addr, uint64_t tz, int dir)
{
	if (dir == XH2A_RAW_COPY_DIR_DEV_TO_HOST)
		memcpy_fromio(data, addr, tz);
	else if (dir == XH2A_RAW_COPY_DIR_HOST_TO_DEV)
		memcpy_toio(addr, data, tz);
	else
		pr_err("%s: wrong parameter dir %d", __func__, dir);
}

static inline int xh2a_pcie_iatu_regbar_copy(struct xh2a_pcie_dev *p_xh2a,
					     uint64_t paddr, char *user_data,
					     uint64_t size, int dir)
{
	unsigned long flags;
	uint64_t offset;
	uint64_t base;
	uint64_t used;
	uint64_t tz;
	int ret;

	spin_lock_irqsave(&p_xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);
	if (unlikely(xh2a_pcie_mmio_blocked(p_xh2a))) {
		spin_unlock_irqrestore(
			&p_xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);
		return -ESHUTDOWN;
	}

	base = paddr & ~(p_xh2a->bar0_len - 1);
	offset = paddr & (p_xh2a->bar0_len - 1);
	used = 0;

	if (size + offset <= p_xh2a->bar0_len) {
		tz = size;
		ret = xh2a_pcie_iatu_bar_map(p_xh2a, base, XH2A_PCIE_REGBAR);

		if (ret != 0)
			goto out;

		raw_copy(user_data, p_xh2a->bar0_mem + offset, tz, dir);
		used += tz;
	} else {
		do {
			tz = p_xh2a->bar0_len - offset;
			ret = xh2a_pcie_iatu_bar_map(p_xh2a, base,
						     XH2A_PCIE_REGBAR);

			if (ret != 0)
				goto out;

			raw_copy(user_data + used, p_xh2a->bar0_mem + offset,
				 tz, dir);
			used += tz;
			base += p_xh2a->bar0_len;
			offset = 0;
		} while (size - used > p_xh2a->bar0_len);

		tz = size - used;
		ret = xh2a_pcie_iatu_bar_map(p_xh2a, base, XH2A_PCIE_REGBAR);

		if (ret != 0)
			goto out;

		raw_copy(user_data + used, p_xh2a->bar0_mem + offset, tz, dir);
		used += tz;
		base += p_xh2a->bar0_len;
		offset = 0;
	}

out:
	xh2a_pcie_iatu_bar_unmap(p_xh2a, XH2A_PCIE_REGBAR);
	spin_unlock_irqrestore(&p_xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);

	if (ret) {
		dev_err(&p_xh2a->pdev->dev, "%s failed to map bar%d, %d\n",
			__func__, XH2A_PCIE_REGBAR, ret);
		return -EBUSY;
	}

	return ret;
}

int xh2a_pcie_membar_getinfo(void *handle, uint64_t *bar_addr,
			     uint64_t *bar_size)
{
	struct xh2a_pcie_dev *p_xh2a;

	if (IS_ERR_OR_NULL(handle)) {
		pr_err("%s: handle is null\n", __func__);
		return -EINVAL;
	}

	p_xh2a = (struct xh2a_pcie_dev *)handle;

	*bar_addr = p_xh2a->bar2_base;
	*bar_size = p_xh2a->bar2_len;

	return 0;
}

int xh2a_pcie_membar_map(void *handle, uint64_t paddr)
{
	unsigned long gate_flags;
	int ret = -1;
	uint64_t base;
	struct xh2a_pcie_dev *p_xh2a;

	if (IS_ERR_OR_NULL(handle)) {
		pr_err("%s: handle is null\n", __func__);
		return -EINVAL;
	}

	p_xh2a = (struct xh2a_pcie_dev *)handle;

	spin_lock_irqsave(&p_xh2a->iatu_lock[XH2A_PCIE_REGBAR], gate_flags);
	if (unlikely(xh2a_pcie_mmio_blocked(p_xh2a))) {
		spin_unlock_irqrestore(
			&p_xh2a->iatu_lock[XH2A_PCIE_REGBAR], gate_flags);
		return -ESHUTDOWN;
	}

	if (p_xh2a->membar_lockmap != 0)
		goto out;

	p_xh2a->membar_lockmap = 1;

	base = paddr & ~(p_xh2a->bar2_len - 1);
	ret = xh2a_pcie_iatu_bar_map(p_xh2a, base, XH2A_PCIE_MEMBAR);

	if (ret != 0) {
		p_xh2a->membar_lockmap = 0;
		goto out;
	}

out:
	spin_unlock_irqrestore(&p_xh2a->iatu_lock[XH2A_PCIE_REGBAR],
			       gate_flags);

	if (ret) {
		dev_err(&p_xh2a->pdev->dev, "%s failed to map bar%d, %d\n",
			__func__, XH2A_PCIE_MEMBAR, ret);
		return -EBUSY;
	}

	return ret;
}

int xh2a_pcie_membar_unmap(void *handle)
{
	unsigned long gate_flags;
	struct xh2a_pcie_dev *p_xh2a;

	if (IS_ERR_OR_NULL(handle)) {
		pr_err("%s: handle is null\n", __func__);
		return -EINVAL;
	}

	p_xh2a = (struct xh2a_pcie_dev *)handle;

	spin_lock_irqsave(&p_xh2a->iatu_lock[XH2A_PCIE_REGBAR], gate_flags);
	if (unlikely(xh2a_pcie_mmio_blocked(p_xh2a))) {
		spin_unlock_irqrestore(
			&p_xh2a->iatu_lock[XH2A_PCIE_REGBAR], gate_flags);
		return -ESHUTDOWN;
	}

	xh2a_pcie_iatu_bar_unmap(p_xh2a, XH2A_PCIE_MEMBAR);
	p_xh2a->membar_lockmap = 0;

	spin_unlock_irqrestore(&p_xh2a->iatu_lock[XH2A_PCIE_REGBAR],
			       gate_flags);

	return 0;
}

int xh2a_pcie_membar_map_no_lock(void *handle, uint64_t paddr)
{
	unsigned long gate_flags;
	int ret;
	uint64_t base;
	struct xh2a_pcie_dev *p_xh2a;

	if (IS_ERR_OR_NULL(handle)) {
		pr_err("%s: handle is null\n", __func__);
		return -EINVAL;
	}

	p_xh2a = (struct xh2a_pcie_dev *)handle;

	spin_lock_irqsave(&p_xh2a->iatu_lock[XH2A_PCIE_REGBAR], gate_flags);
	if (unlikely(xh2a_pcie_mmio_blocked(p_xh2a))) {
		spin_unlock_irqrestore(
			&p_xh2a->iatu_lock[XH2A_PCIE_REGBAR], gate_flags);
		return -ESHUTDOWN;
	}

	base = paddr & ~(p_xh2a->bar2_len - 1);
	ret = xh2a_pcie_iatu_bar_map(p_xh2a, base, XH2A_PCIE_MEMBAR);

	spin_unlock_irqrestore(&p_xh2a->iatu_lock[XH2A_PCIE_REGBAR],
			       gate_flags);

	if (ret != 0) {
		dev_err(&p_xh2a->pdev->dev, "%s failed to map iatu bar%d, %d\n",
			__func__, XH2A_PCIE_MEMBAR, ret);
		ret = -EBUSY;
	}

	return ret;
}

int xh2a_pcie_membar_unmap_no_lock(void *handle)
{
	unsigned long gate_flags;
	struct xh2a_pcie_dev *p_xh2a;

	if (IS_ERR_OR_NULL(handle)) {
		pr_err("%s: handle is null\n", __func__);
		return -EINVAL;
	}

	p_xh2a = (struct xh2a_pcie_dev *)handle;

	spin_lock_irqsave(&p_xh2a->iatu_lock[XH2A_PCIE_REGBAR], gate_flags);
	if (unlikely(xh2a_pcie_mmio_blocked(p_xh2a))) {
		spin_unlock_irqrestore(
			&p_xh2a->iatu_lock[XH2A_PCIE_REGBAR], gate_flags);
		return -ESHUTDOWN;
	}

	xh2a_pcie_iatu_bar_unmap(p_xh2a, XH2A_PCIE_MEMBAR);

	spin_unlock_irqrestore(&p_xh2a->iatu_lock[XH2A_PCIE_REGBAR],
			       gate_flags);
	return 0;
}

static int xh2a_pcie_pio_read(void *handle, uint64_t paddr, uint32_t nbytes,
			      uint64_t *data)
{
	unsigned long flags;
	uint64_t base, offset;
	int ret;
	struct xh2a_pcie_dev *p_xh2a;

	if (IS_ERR_OR_NULL(handle)) {
		pr_err("%s: handle is null\n", __func__);
		return -EINVAL;
	}

	if (!data)
		return -EINVAL;
	/* Some protocol readers ignore errors; never leave a stale descriptor. */
	*data = 0;

	p_xh2a = (struct xh2a_pcie_dev *)handle;

	if (unlikely(atomic_read(&p_xh2a->shutting_down))) {
		dev_err(&p_xh2a->pdev->dev, "%s: device is shutting down\n",
			__func__);
		return -ESHUTDOWN;
	}

	base = paddr & ~(p_xh2a->bar0_len - 1);
	offset = paddr & (p_xh2a->bar0_len - 1);

	spin_lock_irqsave(&p_xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);
	if (unlikely(xh2a_pcie_mmio_blocked(p_xh2a))) {
		spin_unlock_irqrestore(
			&p_xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);
		return -ESHUTDOWN;
	}

	ret = xh2a_pcie_iatu_bar_map(p_xh2a, base, XH2A_PCIE_REGBAR);

	if (ret != 0)
		goto out;

	switch (nbytes) {
	case 8:
		*data = readq(p_xh2a->bar0_mem + (offset & (uint64_t)(-8)));
		break;

	case 2:
		*data = readw(p_xh2a->bar0_mem + (offset & (uint64_t)(-2)));
		break;

	case 1:
		*data = readb(p_xh2a->bar0_mem + offset);
		break;

	case 4:
	default:
		*data = readl(p_xh2a->bar0_mem + (offset & (uint64_t)(-4)));
		break;
	}

out:
	xh2a_pcie_iatu_bar_unmap(p_xh2a, XH2A_PCIE_REGBAR);
	spin_unlock_irqrestore(&p_xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);

	if (ret != 0) {
		dev_err(&p_xh2a->pdev->dev, "%s failed to map iatu bar%d, %d\n",
			__func__, XH2A_PCIE_REGBAR, ret);
		return -EBUSY;
	}

	return ret;
}

static int xh2a_pcie_pio_write(void *handle, uint64_t paddr, uint32_t nbytes,
			       uint64_t data)
{
	unsigned long flags;
	uint64_t base, offset;
	int ret;
	struct xh2a_pcie_dev *p_xh2a;

	if (IS_ERR_OR_NULL(handle)) {
		pr_err("%s: handle is null\n", __func__);
		return -EINVAL;
	}

	p_xh2a = (struct xh2a_pcie_dev *)handle;

	if (unlikely(atomic_read(&p_xh2a->shutting_down))) {
		dev_err(&p_xh2a->pdev->dev, "%s: device is shutting down\n",
			__func__);
		return -ESHUTDOWN;
	}

	base = paddr & ~(p_xh2a->bar0_len - 1);
	offset = paddr & (p_xh2a->bar0_len - 1);

	spin_lock_irqsave(&p_xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);
	if (unlikely(xh2a_pcie_mmio_blocked(p_xh2a))) {
		spin_unlock_irqrestore(
			&p_xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);
		return -ESHUTDOWN;
	}

	ret = xh2a_pcie_iatu_bar_map(p_xh2a, base, XH2A_PCIE_REGBAR);

	if (ret != 0)
		goto out;

	switch (nbytes) {
	case 8:
		writeq(data, p_xh2a->bar0_mem + (offset & (uint64_t)(-8)));
		break;

	case 2:
		writew(data & 0xffff,
		       p_xh2a->bar0_mem + (offset & (uint64_t)(-2)));
		break;

	case 1:
		writeb(data & 0xff, p_xh2a->bar0_mem + offset);
		break;

	case 4:
	default:
		writel(data & 0xffffffff,
		       p_xh2a->bar0_mem + (offset & (uint64_t)(-4)));
		break;
	}

out:
	xh2a_pcie_iatu_bar_unmap(p_xh2a, XH2A_PCIE_REGBAR);
	spin_unlock_irqrestore(&p_xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);

	if (ret != 0) {
		dev_err(&p_xh2a->pdev->dev, "%s failed to map iatu bar%d, %d\n",
			__func__, XH2A_PCIE_REGBAR, ret);
		return -EBUSY;
	}

	return ret;
}

int xh2a_pcie_pio_readb(void *handle, uint64_t paddr, uint8_t *value)
{
	int ret;
	uint64_t v = 0;

	ret = xh2a_pcie_pio_read(handle, paddr, 1, &v);
	*value = v & 0xff;
	return ret;
}
EXPORT_SYMBOL(xh2a_pcie_pio_readb);

int xh2a_pcie_pio_readw(void *handle, uint64_t paddr, uint16_t *value)
{
	int ret;
	uint64_t v = 0;

	ret = xh2a_pcie_pio_read(handle, paddr, 2, &v);
	*value = v & 0xffff;
	return ret;
}
EXPORT_SYMBOL(xh2a_pcie_pio_readw);

int xh2a_pcie_pio_readl(void *handle, uint64_t paddr, uint32_t *value)
{
	int ret;
	uint64_t v = 0;

	ret = xh2a_pcie_pio_read(handle, paddr, 4, &v);
	*value = v & 0xffffffff;
	return ret;
}
EXPORT_SYMBOL(xh2a_pcie_pio_readl);

int xh2a_pcie_pio_readq(void *handle, uint64_t paddr, uint64_t *value)
{
	int ret;

	ret = xh2a_pcie_pio_read(handle, paddr, 8, value);
	return ret;
}
EXPORT_SYMBOL(xh2a_pcie_pio_readq);

int xh2a_pcie_pio_writeb(void *handle, uint64_t paddr, uint8_t value)
{
	int ret;
	uint64_t v;

	v = value & 0xff;
	ret = xh2a_pcie_pio_write(handle, paddr, 1, v);
	return ret;
}
EXPORT_SYMBOL(xh2a_pcie_pio_writeb);

int xh2a_pcie_pio_writew(void *handle, uint64_t paddr, uint16_t value)
{
	int ret;
	uint64_t v;

	v = value & 0xffff;
	ret = xh2a_pcie_pio_write(handle, paddr, 2, v);
	return ret;
}
EXPORT_SYMBOL(xh2a_pcie_pio_writew);

int xh2a_pcie_pio_writel(void *handle, uint64_t paddr, uint32_t value)
{
	int ret;
	uint64_t v;

	v = value & 0xffffffff;
	ret = xh2a_pcie_pio_write(handle, paddr, 4, v);
	return ret;
}
EXPORT_SYMBOL(xh2a_pcie_pio_writel);

int xh2a_pcie_pio_writeq(void *handle, uint64_t paddr, uint64_t value)
{
	int ret;

	ret = xh2a_pcie_pio_write(handle, paddr, 8, value);
	return ret;
}
EXPORT_SYMBOL(xh2a_pcie_pio_writeq);

int xh2a_pcie_pio_read_mem(void *handle, uint64_t paddr, void *host_dst_addr,
			   uint32_t size)
{
	int ret;
	struct xh2a_pcie_dev *p_xh2a;

	if (IS_ERR_OR_NULL(handle)) {
		pr_err("%s: handle is null\n", __func__);
		return -EINVAL;
	}

	if (IS_ERR_OR_NULL(host_dst_addr)) {
		pr_err("%s: host addr is null\n", __func__);
		return -EFAULT;
	}

	p_xh2a = (struct xh2a_pcie_dev *)handle;

	if (unlikely(atomic_read(&p_xh2a->shutting_down))) {
		dev_err(&p_xh2a->pdev->dev, "%s: device is shutting down\n",
			__func__);
		return -ESHUTDOWN;
	}

	ret = xh2a_pcie_iatu_regbar_copy(p_xh2a, paddr, host_dst_addr, size,
					 XH2A_RAW_COPY_DIR_DEV_TO_HOST);
	return ret;
}
EXPORT_SYMBOL(xh2a_pcie_pio_read_mem);

int xh2a_pcie_pio_write_mem(void *handle, uint64_t paddr, void *host_src_addr,
			    uint32_t size)
{
	int ret;
	struct xh2a_pcie_dev *p_xh2a;

	if (IS_ERR_OR_NULL(handle)) {
		pr_err("%s: handle is null\n", __func__);
		return -EINVAL;
	}

	if (IS_ERR_OR_NULL(host_src_addr)) {
		pr_err("%s: host addr is null\n", __func__);
		return -EFAULT;
	}

	p_xh2a = (struct xh2a_pcie_dev *)handle;

	if (unlikely(atomic_read(&p_xh2a->shutting_down))) {
		dev_err(&p_xh2a->pdev->dev, "%s: device is shutting down\n",
			__func__);
		return -ESHUTDOWN;
	}

	ret = xh2a_pcie_iatu_regbar_copy(p_xh2a, paddr, host_src_addr, size,
					 XH2A_RAW_COPY_DIR_HOST_TO_DEV);
	return ret;
}
EXPORT_SYMBOL(xh2a_pcie_pio_write_mem);

int xh2a_pcie_pio_write_mem_sync(void *handle, uint64_t paddr,
				 void *host_src_addr, uint32_t size)
{
	int ret;
	struct xh2a_pcie_dev *p_xh2a;
	char *rb;
	char rbstack[128];

	if (IS_ERR_OR_NULL(handle)) {
		pr_err("%s: handle is null\n", __func__);
		return -EINVAL;
	}

	if (IS_ERR_OR_NULL(host_src_addr)) {
		pr_err("%s: host addr is null\n", __func__);
		return -EFAULT;
	}

	p_xh2a = (struct xh2a_pcie_dev *)handle;

	if (unlikely(atomic_read(&p_xh2a->shutting_down))) {
		dev_err(&p_xh2a->pdev->dev, "%s: device is shutting down\n",
			__func__);
		return -ESHUTDOWN;
	}

	ret = xh2a_pcie_iatu_regbar_copy(p_xh2a, paddr, host_src_addr, size,
					 XH2A_RAW_COPY_DIR_HOST_TO_DEV);

	if (ret)
		return ret;

	if (size <= sizeof(rbstack))
		rb = rbstack;
	else {
		pr_warn("%s: size too large.\n", __func__);
		rb = kmalloc(size, GFP_KERNEL);
		if (!rb) {
			pr_err("%s: kmalloc failed\n", __func__);
			return -ENOMEM;
		}
	}

	ret = xh2a_pcie_iatu_regbar_copy(p_xh2a, paddr, rb, size,
					 XH2A_RAW_COPY_DIR_DEV_TO_HOST);

	if (rb != rbstack)
		kfree(rb);
	return ret;
}
EXPORT_SYMBOL(xh2a_pcie_pio_write_mem_sync);

int xh2a_pcie_dma_read_mem(void *handle, uint64_t paddr, void *host_dst_addr,
			   uint32_t size)
{
	int ret = 0;
	struct xh2a_pcie_dev *p_xh2a;
	uint64_t dev_phy_addr;
	uint32_t xfer_size = 0;
	uint32_t done_size = 0;
	struct xh2a_pcie_hdma_chan *pd_wr = NULL;

	if (IS_ERR_OR_NULL(handle)) {
		pr_err("%s: handle is null\n", __func__);
		return -EINVAL;
	}

	if (IS_ERR_OR_NULL(host_dst_addr)) {
		pr_err("%s: host addr is null\n", __func__);
		return -EFAULT;
	}

	p_xh2a = (struct xh2a_pcie_dev *)handle;

	dev_phy_addr = paddr;

	pd_wr = xh2a_pcie_hdma_get_chan(&p_xh2a->hdma,
					XH2A_PCIE_HDMA_CHAN_TYPE_WR);

	if (IS_ERR(pd_wr))
		return PTR_ERR(pd_wr);

	while (done_size != size) {
		if ((size - done_size) < XH2A_PCIE_HDMA_BUF_SIZE)
			xfer_size = size - done_size;
		else
			xfer_size = XH2A_PCIE_HDMA_BUF_SIZE;

		ret = xh2a_pcie_hdma_chan_transfer(pd_wr, pd_wr->dma_cfg_phys,
						   dev_phy_addr + done_size,
						   xfer_size);

		if (ret != 0) {
			dev_err(&p_xh2a->pdev->dev, "%s: hdma xfer fail %d",
				__func__, ret);
			xh2a_pcie_hdma_put_chan(&p_xh2a->hdma,
						XH2A_PCIE_HDMA_CHAN_TYPE_WR,
						pd_wr);
			goto out;
		}

		memcpy((uint8_t *)host_dst_addr + done_size, pd_wr->dma_cfg_buf,
		       xfer_size);

		done_size += xfer_size;
	}

	xh2a_pcie_hdma_put_chan(&p_xh2a->hdma, XH2A_PCIE_HDMA_CHAN_TYPE_WR,
				pd_wr);

out:
	return ret;
}
EXPORT_SYMBOL(xh2a_pcie_dma_read_mem);

int xh2a_pcie_dma_write_mem(void *handle, uint64_t paddr, void *host_src_addr,
			    uint32_t size)
{
	int ret = 0;
	struct xh2a_pcie_dev *p_xh2a;
	uint64_t dev_phy_addr;
	uint32_t xfer_size = 0;
	uint32_t done_size = 0;
	struct xh2a_pcie_hdma_chan *pd_rd = NULL;

	if (IS_ERR_OR_NULL(handle)) {
		pr_err("%s: handle is null\n", __func__);
		return -EINVAL;
	}

	if (IS_ERR_OR_NULL(host_src_addr)) {
		pr_err("%s: host addr is null\n", __func__);
		return -EFAULT;
	}

	p_xh2a = (struct xh2a_pcie_dev *)handle;

	dev_phy_addr = paddr;

	pd_rd = xh2a_pcie_hdma_get_chan(&p_xh2a->hdma,
					XH2A_PCIE_HDMA_CHAN_TYPE_RD);

	if (IS_ERR(pd_rd))
		return PTR_ERR(pd_rd);

	while (done_size != size) {
		if ((size - done_size) < XH2A_PCIE_HDMA_BUF_SIZE)
			xfer_size = size - done_size;
		else
			xfer_size = XH2A_PCIE_HDMA_BUF_SIZE;

		memcpy(pd_rd->dma_cfg_buf, (uint8_t *)host_src_addr + done_size,
		       xfer_size);

		ret = xh2a_pcie_hdma_chan_transfer(pd_rd,
						   dev_phy_addr + done_size,
						   pd_rd->dma_cfg_phys,
						   xfer_size);

		if (ret != 0) {
			dev_err(&p_xh2a->pdev->dev, "%s: hdma xfer fail %d",
				__func__, ret);
			xh2a_pcie_hdma_put_chan(&p_xh2a->hdma,
						XH2A_PCIE_HDMA_CHAN_TYPE_RD,
						pd_rd);
			goto out;
		}

		done_size += xfer_size;
	}

	xh2a_pcie_hdma_put_chan(&p_xh2a->hdma, XH2A_PCIE_HDMA_CHAN_TYPE_RD,
				pd_rd);

out:
	return ret;
}
EXPORT_SYMBOL(xh2a_pcie_dma_write_mem);

int xh2a_pcie_dma_mrd_direct(void *handle, uint64_t src_paddr,
			     uint64_t dst_paddr, uint32_t size)
{
	int ret;
	struct xh2a_pcie_dev *p_xh2a;
	struct xh2a_pcie_hdma_chan *pd_rd = NULL;

	if (IS_ERR_OR_NULL(handle)) {
		pr_err("%s: handle is null\n", __func__);
		return -EINVAL;
	}

	p_xh2a = (struct xh2a_pcie_dev *)handle;

	pd_rd = xh2a_pcie_hdma_get_chan(&p_xh2a->hdma,
					XH2A_PCIE_HDMA_CHAN_TYPE_RD);

	if (IS_ERR(pd_rd))
		return PTR_ERR(pd_rd);

	ret = xh2a_pcie_hdma_chan_transfer(pd_rd, dst_paddr, src_paddr, size);

	if (ret != 0)
		dev_err(&p_xh2a->pdev->dev, "%s: hdma xfer fail %d", __func__,
			ret);

	xh2a_pcie_hdma_put_chan(&p_xh2a->hdma, XH2A_PCIE_HDMA_CHAN_TYPE_RD,
				pd_rd);

	return ret;
}
EXPORT_SYMBOL(xh2a_pcie_dma_mrd_direct);

int xh2a_pcie_dma_mwr_direct(void *handle, uint64_t src_paddr,
			     uint64_t dst_paddr, uint32_t size)
{
	int ret;
	struct xh2a_pcie_dev *p_xh2a;
	struct xh2a_pcie_hdma_chan *pd_wr = NULL;

	if (IS_ERR_OR_NULL(handle)) {
		pr_err("%s: handle is null\n", __func__);
		return -EINVAL;
	}

	p_xh2a = (struct xh2a_pcie_dev *)handle;

	pd_wr = xh2a_pcie_hdma_get_chan(&p_xh2a->hdma,
					XH2A_PCIE_HDMA_CHAN_TYPE_WR);

	if (IS_ERR(pd_wr))
		return PTR_ERR(pd_wr);

	ret = xh2a_pcie_hdma_chan_transfer(pd_wr, dst_paddr, src_paddr, size);

	if (ret != 0)
		dev_err(&p_xh2a->pdev->dev, "%s: hdma xfer fail %d", __func__,
			ret);

	xh2a_pcie_hdma_put_chan(&p_xh2a->hdma, XH2A_PCIE_HDMA_CHAN_TYPE_WR,
				pd_wr);

	return ret;
}
EXPORT_SYMBOL(xh2a_pcie_dma_mwr_direct);

int xh2a_pcie_dma_read_mem_userspace(void *handle, uint64_t paddr,
				     void __user *host_dst_addr, uint32_t size)
{
	int ret = 0;
	struct xh2a_pcie_dev *p_xh2a;
	uint64_t dev_phy_addr;
	uint32_t xfer_size = 0;
	uint32_t done_size = 0;
	struct xh2a_pcie_hdma_chan *pd_wr = NULL;

	if (IS_ERR_OR_NULL(handle)) {
		pr_err("%s: handle is null\n", __func__);
		return -EINVAL;
	}

	p_xh2a = (struct xh2a_pcie_dev *)handle;

	dev_phy_addr = paddr;

	if (!access_write_ok(host_dst_addr, size)) {
		dev_err(&p_xh2a->pdev->dev, "%s: access write failed",
			__func__);
		ret = -EFAULT;
		goto out;
	}

	pd_wr = xh2a_pcie_hdma_get_chan(&p_xh2a->hdma,
					XH2A_PCIE_HDMA_CHAN_TYPE_WR);

	if (IS_ERR(pd_wr))
		return PTR_ERR(pd_wr);

	while (done_size != size) {
		if ((size - done_size) < XH2A_PCIE_HDMA_BUF_SIZE)
			xfer_size = size - done_size;
		else
			xfer_size = XH2A_PCIE_HDMA_BUF_SIZE;

		ret = xh2a_pcie_hdma_chan_transfer(pd_wr, pd_wr->dma_cfg_phys,
						   dev_phy_addr + done_size,
						   xfer_size);

		if (ret != 0) {
			dev_err(&p_xh2a->pdev->dev, "%s: hdma xfer fail %d",
				__func__, ret);
			xh2a_pcie_hdma_put_chan(&p_xh2a->hdma,
						XH2A_PCIE_HDMA_CHAN_TYPE_WR,
						pd_wr);
			goto out;
		}

		ret = copy_to_user((uint8_t *)host_dst_addr + done_size,
				   pd_wr->dma_cfg_buf, xfer_size);

		if (ret != 0) {
			dev_err(&p_xh2a->pdev->dev, "%s: copy to user fail %d",
				__func__, ret);
			ret = -EFAULT;
			xh2a_pcie_hdma_put_chan(&p_xh2a->hdma,
						XH2A_PCIE_HDMA_CHAN_TYPE_WR,
						pd_wr);
			goto out;
		}

		done_size += xfer_size;
	}

	xh2a_pcie_hdma_put_chan(&p_xh2a->hdma, XH2A_PCIE_HDMA_CHAN_TYPE_WR,
				pd_wr);

out:
	return ret;
}
EXPORT_SYMBOL(xh2a_pcie_dma_read_mem_userspace);

int xh2a_pcie_dma_write_mem_userspace(void *handle, uint64_t paddr,
				      void __user *host_src_addr, uint32_t size)
{
	int ret = 0;
	struct xh2a_pcie_dev *p_xh2a;
	uint64_t dev_phy_addr;
	uint32_t xfer_size = 0;
	uint32_t done_size = 0;
	struct xh2a_pcie_hdma_chan *pd_rd = NULL;

	if (IS_ERR_OR_NULL(handle)) {
		pr_err("%s: handle is null\n", __func__);
		return -EINVAL;
	}

	p_xh2a = (struct xh2a_pcie_dev *)handle;

	dev_phy_addr = paddr;

	if (!access_read_ok(host_src_addr, size)) {
		dev_err(&p_xh2a->pdev->dev, "%s: access read failed", __func__);
		ret = -EFAULT;
		goto out;
	}

	pd_rd = xh2a_pcie_hdma_get_chan(&p_xh2a->hdma,
					XH2A_PCIE_HDMA_CHAN_TYPE_RD);

	if (IS_ERR(pd_rd))
		return PTR_ERR(pd_rd);

	while (done_size != size) {
		if ((size - done_size) < XH2A_PCIE_HDMA_BUF_SIZE)
			xfer_size = size - done_size;
		else
			xfer_size = XH2A_PCIE_HDMA_BUF_SIZE;

		ret = copy_from_user(pd_rd->dma_cfg_buf,
				     (uint8_t *)host_src_addr + done_size,
				     xfer_size);

		if (ret != 0) {
			dev_err(&p_xh2a->pdev->dev,
				"%s: copy from user fail %d\n", __func__, ret);
			ret = -EFAULT;
			xh2a_pcie_hdma_put_chan(&p_xh2a->hdma,
						XH2A_PCIE_HDMA_CHAN_TYPE_RD,
						pd_rd);
			goto out;
		}

		ret = xh2a_pcie_hdma_chan_transfer(pd_rd,
						   dev_phy_addr + done_size,
						   pd_rd->dma_cfg_phys,
						   xfer_size);

		if (ret != 0) {
			dev_err(&p_xh2a->pdev->dev, "%s: hdma xfer fail %d",
				__func__, ret);
			xh2a_pcie_hdma_put_chan(&p_xh2a->hdma,
						XH2A_PCIE_HDMA_CHAN_TYPE_RD,
						pd_rd);
			goto out;
		}

		done_size += xfer_size;
	}

	xh2a_pcie_hdma_put_chan(&p_xh2a->hdma, XH2A_PCIE_HDMA_CHAN_TYPE_RD,
				pd_rd);

out:
	return ret;
}
EXPORT_SYMBOL(xh2a_pcie_dma_write_mem_userspace);

/*
 * ELBI msg register (PCIE_ELBI_MSG_SET) is 32bit wide. We divide it into
 * two 16bit fields. Lower 16bit is for E2 and upper 16bit is for U7.
 * to generate ELBI msg, we need to set the appropriate mask for both E2
 * and U7, and shift the msg to the correct offset, aka. lower 16bit for E2,
 * and upper 16bit for U7.
 * to clear ELBI msg, we need to write msgclr register (PCIE_ELBI_MSG_CLR) with
 * correct mask.
 */
#define XH2A_PCIE_ELBI_MSG_OFFSET_E2   0
#define XH2A_PCIE_ELBI_MSG_OFFSET_U7   16
#define XH2A_PCIE_ELBI_MSG_MASK_E2_VAL 0xFFFF0000
#define XH2A_PCIE_ELBI_MSG_MASK_U7_VAL 0x0000FFFF
#define XH2A_PCIE_ELBI_MSG_BIT_MAX     16

int xh2a_pcie_write_bar_msgbit(void *handle, uint32_t msgbit, uint32_t tocpu)
{
	unsigned long flags;
	uint32_t val;
	struct xh2a_pcie_dev *p_xh2a;

	if (IS_ERR_OR_NULL(handle)) {
		pr_err("%s: handle is null\n", __func__);
		return -EINVAL;
	}

	p_xh2a = (struct xh2a_pcie_dev *)handle;

	if (msgbit >= XH2A_PCIE_ELBI_MSG_BIT_MAX) {
		dev_err(&p_xh2a->pdev->dev, "%s: msgbit %d is out of range",
			__func__, msgbit);
		return -EINVAL;
	}

	if (tocpu != XH2A_PCIE_MSG_TO_U7 && tocpu != XH2A_PCIE_MSG_TO_E2)
		return -EINVAL;

	spin_lock_irqsave(&p_xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);
	if (unlikely(xh2a_pcie_mmio_blocked(p_xh2a))) {
		spin_unlock_irqrestore(
			&p_xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);
		return -ESHUTDOWN;
	}

	val = readl(p_xh2a->trgt0_mem + PCIE_ELBI_MSG_SET);
	writel(XH2A_PCIE_ELBI_MSG_MASK_E2_VAL,
	       p_xh2a->trgt0_mem + PCIE_ELBI_MSG_MASK_E2);
#if 0
	writel(XH2A_PCIE_ELBI_MSG_MASK_U7_VAL,
	       p_xh2a->trgt0_mem + PCIE_ELBI_MSG_MASK_U7);
#endif

	if (tocpu == XH2A_PCIE_MSG_TO_U7) {
		val |= (BIT(msgbit) << XH2A_PCIE_ELBI_MSG_OFFSET_U7);
		writel(val, p_xh2a->trgt0_mem + PCIE_ELBI_MSG_SET);
	} else if (tocpu == XH2A_PCIE_MSG_TO_E2) {
		val |= (BIT(msgbit) << XH2A_PCIE_ELBI_MSG_OFFSET_E2);
		writel(val, p_xh2a->trgt0_mem + PCIE_ELBI_MSG_SET);
	}

	spin_unlock_irqrestore(&p_xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);
	return 0;
}
EXPORT_SYMBOL(xh2a_pcie_write_bar_msgbit);

/* Serialize the last reset write with every endpoint MMIO path. The gate
 * stays closed through remove; probe allocates a fresh, open instance.
 * No PM operation, work cancellation or DMA completion wait is done here.
 */
int xh2a_pcie_fullchip_reset(void *handle, bool firmware, uint32_t value)
{
	struct xh2a_pcie_dev *p_xh2a = handle;
	unsigned long flags;
	uint64_t addr = 0x7000106cULL;
	uint32_t msg = 0;
	int ret = 0;

	if (IS_ERR_OR_NULL(handle))
		return -EINVAL;

	spin_lock_irqsave(&p_xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);
	if (xh2a_pcie_mmio_blocked(p_xh2a)) {
		ret = -ESHUTDOWN;
		goto unlock;
	}

	if (firmware) {
		msg = readl(p_xh2a->trgt0_mem + PCIE_ELBI_MSG_SET);
		writel(XH2A_PCIE_ELBI_MSG_MASK_E2_VAL,
		       p_xh2a->trgt0_mem + PCIE_ELBI_MSG_MASK_E2);
	} else {
		ret = xh2a_pcie_iatu_bar_map(p_xh2a,
			addr & ~(p_xh2a->bar0_len - 1), XH2A_PCIE_REGBAR);
		if (ret) {
			xh2a_pcie_iatu_bar_unmap(p_xh2a, XH2A_PCIE_REGBAR);
			ret = -EBUSY;
			goto unlock;
		}
	}

	xh2a_pcie_hdma_quiesce_locked(&p_xh2a->hdma);
	p_xh2a->reset_pending = true;
	if (firmware)
		writel(msg | BIT(7), p_xh2a->trgt0_mem + PCIE_ELBI_MSG_SET);
	else
		writel(value, p_xh2a->bar0_mem +
		       (addr & (p_xh2a->bar0_len - 1)));
	/* In particular, do not unmap iATU after triggering reset. */
unlock:
	spin_unlock_irqrestore(&p_xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);
	return ret;
}

int xh2a_pcie_check_pwrsts(void *handle, uint32_t *pwrsts)
{
	unsigned long flags;
	struct xh2a_pcie_dev *p_xh2a;

	if (IS_ERR_OR_NULL(handle)) {
		pr_err("%s: handle is null\n", __func__);
		return -EINVAL;
	}

	p_xh2a = (struct xh2a_pcie_dev *)handle;
	if (!pwrsts)
		return -EINVAL;
	if (pci_channel_offline(p_xh2a->pdev))
		return -ENODEV;
	spin_lock_irqsave(&p_xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);
	if (unlikely(xh2a_pcie_mmio_blocked(p_xh2a))) {
		spin_unlock_irqrestore(
			&p_xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);
		return -ESHUTDOWN;
	}
	*pwrsts = readl(p_xh2a->trgt0_mem + PCIE_ELBI_PWRSTS);
	spin_unlock_irqrestore(&p_xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);
	return *pwrsts == ~0U ? -ENODEV : 0;
}
EXPORT_SYMBOL(xh2a_pcie_check_pwrsts);

int xh2a_pcie_update_pwrsts(void *handle, uint32_t pwrsts)
{
	unsigned long flags;
	struct xh2a_pcie_dev *p_xh2a;

	if (IS_ERR_OR_NULL(handle)) {
		pr_err("%s: handle is null\n", __func__);
		return -EINVAL;
	}

	p_xh2a = (struct xh2a_pcie_dev *)handle;
	spin_lock_irqsave(&p_xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);
	if (unlikely(xh2a_pcie_mmio_blocked(p_xh2a))) {
		spin_unlock_irqrestore(
			&p_xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);
		return -ESHUTDOWN;
	}
	writel(pwrsts, p_xh2a->trgt0_mem + PCIE_ELBI_PWRSTS);
	spin_unlock_irqrestore(&p_xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);
	return 0;
}
EXPORT_SYMBOL(xh2a_pcie_update_pwrsts);

int xh2a_pcie_register_client(void *handle, struct xh2a_pcie_client *client)
{
	struct xh2a_pcie_dev *p_xh2a;
	int i;
	unsigned long flags;

	if (IS_ERR_OR_NULL(handle)) {
		pr_err("%s: handle is null\n", __func__);
		return -EINVAL;
	}

	p_xh2a = (struct xh2a_pcie_dev *)handle;

	if (client->work_num > XH2A_PCIE_CLIENT_MAX_WORK_NUM) {
		dev_err(&p_xh2a->pdev->dev, "%s: invalid work_num %d\n",
			__func__, client->work_num);
		return -EINVAL;
	}

	for (i = 0; i < client->work_num; i++) {
		if (IS_ERR_OR_NULL(client->work[i].msi_work)) {
			dev_err(&p_xh2a->pdev->dev, "%s: invalid work %d\n",
				__func__, i);
			return -EINVAL;
		}
	}

	spin_lock_irqsave(&p_xh2a->client_list_lock, flags);
	INIT_LIST_HEAD(&client->node);
	list_add_tail(&client->node, &p_xh2a->client_list);
	spin_unlock_irqrestore(&p_xh2a->client_list_lock, flags);
	return 0;
}
EXPORT_SYMBOL(xh2a_pcie_register_client);

int xh2a_pcie_unregister_client(void *handle, struct xh2a_pcie_client *client)
{
	struct xh2a_pcie_dev *p_xh2a;
	struct xh2a_pcie_client *pos, *n;
	unsigned long flags;

	if (IS_ERR_OR_NULL(handle)) {
		pr_err("%s: handle is null\n", __func__);
		return -EINVAL;
	}

	p_xh2a = (struct xh2a_pcie_dev *)handle;

	spin_lock_irqsave(&p_xh2a->client_list_lock, flags);
	list_for_each_entry_safe(pos, n, &p_xh2a->client_list, node) {
		if (pos == client)
			list_del(&pos->node);
	}
	spin_unlock_irqrestore(&p_xh2a->client_list_lock, flags);

	return 0;
}
EXPORT_SYMBOL(xh2a_pcie_unregister_client);

int xh2a_pcie_synchronize_msi(void *handle, int msi_id)
{
	struct xh2a_pcie_dev *p_xh2a = handle;

	if (IS_ERR_OR_NULL(handle))
		return -EINVAL;
	if (msi_id < 0 || msi_id >= p_xh2a->irqs)
		return -EINVAL;

	/* A handler may have copied a work pointer before client unregister. */
	synchronize_irq(p_xh2a->msi_irq[msi_id]);
	return 0;
}


int xh2a_pcie_get_client(void *handle, struct xh2a_pcie_client **client,
			 const char *name)
{
	struct xh2a_pcie_dev *p_xh2a;
	struct xh2a_pcie_client *pos, *n;
	unsigned long flags;

	if (IS_ERR_OR_NULL(handle)) {
		pr_err("%s: handle is null\n", __func__);
		return -EINVAL;
	}

	p_xh2a = (struct xh2a_pcie_dev *)handle;

	spin_lock_irqsave(&p_xh2a->client_list_lock, flags);
	list_for_each_entry_safe(pos, n, &p_xh2a->client_list, node) {
		if (strcmp(pos->name, name) == 0) {
			*client = pos;
			break;
		}
	}
	spin_unlock_irqrestore(&p_xh2a->client_list_lock, flags);

	return 0;
}
EXPORT_SYMBOL(xh2a_pcie_get_client);

int xh2a_pcie_device_index(void *handle)
{
	struct xh2a_pcie_dev *p_xh2a;

	if (IS_ERR_OR_NULL(handle)) {
		pr_err("%s: handle is null\n", __func__);
		return -EINVAL;
	}

	p_xh2a = (struct xh2a_pcie_dev *)handle;

	return p_xh2a->minor;
}
EXPORT_SYMBOL(xh2a_pcie_device_index);

int xh2a_pcie_device_dbdf(void *handle, uint32_t *dbdf)
{
	struct xh2a_pcie_dev *p_xh2a;
	uint32_t domain;
	uint32_t bus;
	uint32_t devfn;

	if (IS_ERR_OR_NULL(handle)) {
		pr_err("%s: handle is null\n", __func__);
		return -EINVAL;
	}

	p_xh2a = (struct xh2a_pcie_dev *)handle;

	domain = pci_domain_nr(p_xh2a->pdev->bus);
	bus = p_xh2a->pdev->bus->number;
	devfn = p_xh2a->pdev->devfn;

	*dbdf = ((domain & 0xffff) << 16) | ((bus & 0xff) << 8) |
		(devfn & 0xff);

	return 0;
}
EXPORT_SYMBOL(xh2a_pcie_device_dbdf);

struct device *xh2a_pcie_device_ptr(void *handle)
{
	struct xh2a_pcie_dev *p_xh2a;

	if (IS_ERR_OR_NULL(handle)) {
		pr_err("%s: handle is null\n", __func__);
		return ERR_PTR(-EINVAL);
	}

	p_xh2a = (struct xh2a_pcie_dev *)handle;

	return &p_xh2a->pdev->dev;
}
EXPORT_SYMBOL(xh2a_pcie_device_ptr);

void xh2a_pcie_stop_runtime_pm(void *handle)
{
	struct xh2a_pcie_dev *p_xh2a;
	if (IS_ERR_OR_NULL(handle)) {
		pr_err("%s: handle is null\n", __func__);
		return;
	}
	p_xh2a = (struct xh2a_pcie_dev *)handle;
	pm_runtime_forbid(&p_xh2a->pdev->dev);
}
EXPORT_SYMBOL(xh2a_pcie_stop_runtime_pm);

static int xh2a_pcie_iatu_reg_batch(void *handle, uint64_t paddr,
				    struct xh2a_pcie_reg_entry *entries,
				    int count, int write, uint32_t nbytes)
{
	unsigned long flags;
	uint64_t base, offset;
	int i, ret;
	struct xh2a_pcie_dev *p_xh2a;

	if (IS_ERR_OR_NULL(handle)) {
		pr_err("%s: handle is null\n", __func__);
		return -EINVAL;
	}

	if (IS_ERR_OR_NULL(entries) || count <= 0) {
		pr_err("%s: invalid entries or count\n", __func__);
		return -EINVAL;
	}

	p_xh2a = (struct xh2a_pcie_dev *)handle;

	if (unlikely(atomic_read(&p_xh2a->shutting_down))) {
		dev_err(&p_xh2a->pdev->dev, "%s: device is shutting down\n",
			__func__);
		return -ESHUTDOWN;
	}

	base = paddr & ~(p_xh2a->bar0_len - 1);
	offset = paddr & (p_xh2a->bar0_len - 1);

	spin_lock_irqsave(&p_xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);
	if (unlikely(xh2a_pcie_mmio_blocked(p_xh2a))) {
		spin_unlock_irqrestore(
			&p_xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);
		return -ESHUTDOWN;
	}

	ret = xh2a_pcie_iatu_bar_map(p_xh2a, base, XH2A_PCIE_REGBAR);
	if (ret != 0)
		goto out;

	if (write) {
		for (i = 0; i < count; i++) {
			void *addr =
				p_xh2a->bar0_mem + offset + entries[i].offset;

			switch (nbytes) {
			case 1:
				writeb(entries[i].val & 0xff, addr);
				break;
			case 2:
				writew(entries[i].val & 0xffff, addr);
				break;
			case 8:
				writeq(entries[i].val, addr);
				break;
			case 4:
			default:
				writel(entries[i].val & 0xffffffff, addr);
				break;
			}
		}
	} else {
		for (i = 0; i < count; i++) {
			void *addr =
				p_xh2a->bar0_mem + offset + entries[i].offset;

			switch (nbytes) {
			case 1:
				entries[i].val = readb(addr);
				break;
			case 2:
				entries[i].val = readw(addr);
				break;
			case 8:
				entries[i].val = readq(addr);
				break;
			case 4:
			default:
				entries[i].val = readl(addr);
				break;
			}
		}
	}

	xh2a_pcie_iatu_bar_unmap(p_xh2a, XH2A_PCIE_REGBAR);

out:
	spin_unlock_irqrestore(&p_xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);

	if (ret != 0) {
		dev_err(&p_xh2a->pdev->dev, "%s failed to map bar%d, %d\n",
			__func__, XH2A_PCIE_REGBAR, ret);
		return -EBUSY;
	}

	return 0;
}

static int xh2a_pcie_iatu_rmw(void *handle, uint64_t paddr, uint64_t mask,
			      uint64_t val, enum xh2a_pcie_rmw_op op,
			      uint32_t nbytes)
{
	unsigned long flags;
	uint64_t base, offset, cur;
	int ret;
	struct xh2a_pcie_dev *p_xh2a;
	void *addr;

	if (IS_ERR_OR_NULL(handle)) {
		pr_err("%s: handle is null\n", __func__);
		return -EINVAL;
	}

	p_xh2a = (struct xh2a_pcie_dev *)handle;

	if (unlikely(atomic_read(&p_xh2a->shutting_down))) {
		dev_err(&p_xh2a->pdev->dev, "%s: device is shutting down\n",
			__func__);
		return -ESHUTDOWN;
	}

	base = paddr & ~(p_xh2a->bar0_len - 1);
	offset = paddr & (p_xh2a->bar0_len - 1);

	spin_lock_irqsave(&p_xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);
	if (unlikely(xh2a_pcie_mmio_blocked(p_xh2a))) {
		spin_unlock_irqrestore(
			&p_xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);
		return -ESHUTDOWN;
	}

	ret = xh2a_pcie_iatu_bar_map(p_xh2a, base, XH2A_PCIE_REGBAR);
	if (ret != 0)
		goto out;

	addr = p_xh2a->bar0_mem + offset;

	switch (nbytes) {
	case 1:
		cur = readb(addr);
		break;
	case 2:
		cur = readw(addr);
		break;
	case 8:
		cur = readq(addr);
		break;
	case 4:
	default:
		cur = readl(addr);
		break;
	}

	if (op == XH2A_PCIE_RMW_OR)
		cur |= (val & mask);
	else
		cur &= ~(val & mask);

	switch (nbytes) {
	case 1:
		writeb(cur, addr);
		break;
	case 2:
		writew(cur, addr);
		break;
	case 8:
		writeq(cur, addr);
		break;
	case 4:
	default:
		writel(cur, addr);
		break;
	}

	xh2a_pcie_iatu_bar_unmap(p_xh2a, XH2A_PCIE_REGBAR);

out:
	spin_unlock_irqrestore(&p_xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);

	if (ret != 0) {
		dev_err(&p_xh2a->pdev->dev, "%s failed to map bar%d, %d\n",
			__func__, XH2A_PCIE_REGBAR, ret);
		return -EBUSY;
	}

	return 0;
}

int xh2a_pcie_pio_write_regb_batch(void *handle, uint64_t paddr,
				   struct xh2a_pcie_reg_entry *entries,
				   int count)
{
	return xh2a_pcie_iatu_reg_batch(handle, paddr, entries, count, 1, 1);
}
EXPORT_SYMBOL(xh2a_pcie_pio_write_regb_batch);

int xh2a_pcie_pio_write_regw_batch(void *handle, uint64_t paddr,
				   struct xh2a_pcie_reg_entry *entries,
				   int count)
{
	return xh2a_pcie_iatu_reg_batch(handle, paddr, entries, count, 1, 2);
}
EXPORT_SYMBOL(xh2a_pcie_pio_write_regw_batch);

int xh2a_pcie_pio_write_regl_batch(void *handle, uint64_t paddr,
				   struct xh2a_pcie_reg_entry *entries,
				   int count)
{
	return xh2a_pcie_iatu_reg_batch(handle, paddr, entries, count, 1, 4);
}
EXPORT_SYMBOL(xh2a_pcie_pio_write_regl_batch);

int xh2a_pcie_pio_write_regq_batch(void *handle, uint64_t paddr,
				   struct xh2a_pcie_reg_entry *entries,
				   int count)
{
	return xh2a_pcie_iatu_reg_batch(handle, paddr, entries, count, 1, 8);
}
EXPORT_SYMBOL(xh2a_pcie_pio_write_regq_batch);

int xh2a_pcie_pio_read_regb_batch(void *handle, uint64_t paddr,
				  struct xh2a_pcie_reg_entry *entries,
				  int count)
{
	return xh2a_pcie_iatu_reg_batch(handle, paddr, entries, count, 0, 1);
}
EXPORT_SYMBOL(xh2a_pcie_pio_read_regb_batch);

int xh2a_pcie_pio_read_regw_batch(void *handle, uint64_t paddr,
				  struct xh2a_pcie_reg_entry *entries,
				  int count)
{
	return xh2a_pcie_iatu_reg_batch(handle, paddr, entries, count, 0, 2);
}
EXPORT_SYMBOL(xh2a_pcie_pio_read_regw_batch);

int xh2a_pcie_pio_read_regl_batch(void *handle, uint64_t paddr,
				  struct xh2a_pcie_reg_entry *entries,
				  int count)
{
	return xh2a_pcie_iatu_reg_batch(handle, paddr, entries, count, 0, 4);
}
EXPORT_SYMBOL(xh2a_pcie_pio_read_regl_batch);

int xh2a_pcie_pio_read_regq_batch(void *handle, uint64_t paddr,
				  struct xh2a_pcie_reg_entry *entries,
				  int count)
{
	return xh2a_pcie_iatu_reg_batch(handle, paddr, entries, count, 0, 8);
}
EXPORT_SYMBOL(xh2a_pcie_pio_read_regq_batch);

int xh2a_pcie_pio_rmw_regb(void *handle, uint64_t paddr, uint8_t mask,
			   uint8_t val, enum xh2a_pcie_rmw_op op)
{
	return xh2a_pcie_iatu_rmw(handle, paddr, mask, val, op, 1);
}
EXPORT_SYMBOL(xh2a_pcie_pio_rmw_regb);

int xh2a_pcie_pio_rmw_regw(void *handle, uint64_t paddr, uint16_t mask,
			   uint16_t val, enum xh2a_pcie_rmw_op op)
{
	return xh2a_pcie_iatu_rmw(handle, paddr, mask, val, op, 2);
}
EXPORT_SYMBOL(xh2a_pcie_pio_rmw_regw);

int xh2a_pcie_pio_rmw_regl(void *handle, uint64_t paddr, uint32_t mask,
			   uint32_t val, enum xh2a_pcie_rmw_op op)
{
	return xh2a_pcie_iatu_rmw(handle, paddr, mask, val, op, 4);
}
EXPORT_SYMBOL(xh2a_pcie_pio_rmw_regl);

int xh2a_pcie_pio_rmw_regq(void *handle, uint64_t paddr, uint64_t mask,
			   uint64_t val, enum xh2a_pcie_rmw_op op)
{
	return xh2a_pcie_iatu_rmw(handle, paddr, mask, val, op, 8);
}
EXPORT_SYMBOL(xh2a_pcie_pio_rmw_regq);
