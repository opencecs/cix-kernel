// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2024 Houmo AI Inc.
 * Author: hongxing.ma<hongxing.ma@houmo.ai>
 */

#include <linux/delay.h>
#include "xh2a_pcie.h"

static inline void
xh2a_pcie_hdma_chan_irq_enable(struct xh2a_pcie_hdma_chan *chan,
			       uint32_t irq_mask)
{
	uint32_t val;

	val = readl(chan->ch_regs + PCIE_HDMA_CHAN_INT_SET);
	val |= irq_mask;
	writel(val, chan->ch_regs + PCIE_HDMA_CHAN_INT_SET);
}

static inline void
xh2a_pcie_hdma_chan_irq_disable(struct xh2a_pcie_hdma_chan *chan,
				uint32_t irq_mask)
{
	uint32_t val;

	val = readl(chan->ch_regs + PCIE_HDMA_CHAN_INT_SET);
	val &= ~irq_mask;
	writel(val, chan->ch_regs + PCIE_HDMA_CHAN_INT_SET);
}

static inline void
xh2a_pcie_hdma_chan_irq_clear(struct xh2a_pcie_hdma_chan *chan)
{
	writel(PCIE_HDMA_INT_ABORT_CLR | PCIE_HDMA_INT_WM_CLR |
		       PCIE_HDMA_INT_STOP_CLR,
	       chan->ch_regs + PCIE_HDMA_CHAN_INT_CLR);
}

static inline void xh2a_pcie_hdma_chan_enable(struct xh2a_pcie_hdma_chan *chan)
{
	writel(PCIE_HDMA_CHAN_EN_BIT, chan->ch_regs + PCIE_HDMA_CHAN_EN);
}

static inline void xh2a_pcie_hdma_chan_disable(struct xh2a_pcie_hdma_chan *chan)
{
	writel(PCIE_HDMA_CHAN_DIS, chan->ch_regs + PCIE_HDMA_CHAN_EN);
}

static inline void xh2a_pcie_hdma_chan_start(struct xh2a_pcie_hdma_chan *chan)
{
	writel(PCIE_HDMA_CHAN_START_BIT, chan->ch_regs + PCIE_HDMA_CHAN_DB);
}

static inline void xh2a_pcie_hdma_chan_stop(struct xh2a_pcie_hdma_chan *chan)
{
	dev_info(chan->hdma->dev, "%s: channel %d STATUS[0x%x]", __func__,
		 chan->id, readl(chan->ch_regs + PCIE_HDMA_CHAN_INT_STS) >> 3);
	writel(PCIE_HDMA_CHAN_STOP_BIT, chan->ch_regs + PCIE_HDMA_CHAN_DB);
}

static inline void xh2a_pcie_hdma_chan_prepare(struct xh2a_pcie_hdma_chan *chan,
					       dma_addr_t dma_dst,
					       dma_addr_t dma_src, size_t len)
{
	xh2a_pcie_hdma_chan_irq_clear(chan);
	xh2a_pcie_hdma_chan_enable(chan);
	writel(0, chan->ch_regs + PCIE_HDMA_CHAN_CTRL1);
	writel(cpu_to_le32(dma_src & 0xffffffff),
	       chan->ch_regs + PCIE_HDMA_CHAN_SAR_LO);
	writel(cpu_to_le32(dma_src >> 32),
	       chan->ch_regs + PCIE_HDMA_CHAN_SAR_HI);
	writel(cpu_to_le32(dma_dst & 0xffffffff),
	       chan->ch_regs + PCIE_HDMA_CHAN_DAR_LO);
	writel(cpu_to_le32(dma_dst >> 32),
	       chan->ch_regs + PCIE_HDMA_CHAN_DAR_HI);
	writel(len, chan->ch_regs + PCIE_HDMA_CHAN_TRSIZE);
	writel(chan->hdma->msi->address_hi,
	       chan->ch_regs + PCIE_HDMA_CHAN_STOP_MSI_HI);
	writel(chan->hdma->msi->address_lo,
	       chan->ch_regs + PCIE_HDMA_CHAN_STOP_MSI_LO);
	writel(chan->hdma->msi->address_hi,
	       chan->ch_regs + PCIE_HDMA_CHAN_ABRT_MSI_HI);
	writel(chan->hdma->msi->address_lo,
	       chan->ch_regs + PCIE_HDMA_CHAN_ABRT_MSI_LO);
	writel(chan->hdma->msi->data, chan->ch_regs + PCIE_HDMA_CHAN_MSI_DATA);
	xh2a_pcie_hdma_chan_irq_enable(chan, PCIE_HDMA_RAIE | PCIE_HDMA_RSIE |
						     PCIE_HDMA_LAIE |
						     PCIE_HDMA_LSIE);
}

static inline void xh2a_pcie_hdma_enable(struct xh2a_pcie_hdma *hdma)
{
	int i;

	for (i = 0; i < XH2A_PCIE_HDMA_NWRCHAN; i++) {
		xh2a_pcie_hdma_chan_enable(&hdma->wrchan[i]);
		xh2a_pcie_hdma_chan_irq_disable(
			&hdma->wrchan[i], PCIE_HDMA_LAIE | PCIE_HDMA_RAIE |
						  PCIE_HDMA_LSIE |
						  PCIE_HDMA_RSIE);
	}

	for (i = 0; i < XH2A_PCIE_HDMA_NRDCHAN; i++) {
		xh2a_pcie_hdma_chan_enable(&hdma->rdchan[i]);
		xh2a_pcie_hdma_chan_irq_disable(
			&hdma->rdchan[i], PCIE_HDMA_LAIE | PCIE_HDMA_RAIE |
						  PCIE_HDMA_LSIE |
						  PCIE_HDMA_RSIE);
	}
}

static inline void xh2a_pcie_hdma_disable(struct xh2a_pcie_hdma *hdma)
{
	int i;

	for (i = 0; i < XH2A_PCIE_HDMA_NWRCHAN; i++)
		xh2a_pcie_hdma_chan_disable(&hdma->wrchan[i]);

	for (i = 0; i < XH2A_PCIE_HDMA_NRDCHAN; i++)
		xh2a_pcie_hdma_chan_disable(&hdma->rdchan[i]);
}

static inline void
xh2a_pcie_hdma_chan_controller_dump(struct xh2a_pcie_hdma_chan *chan)
{
#ifdef PCIE_HDMA_DEBUG_VERBOSE
	dev_dbg(chan->hdma->dev, "%s: dump hdma channel %d status:\n", __func__,
		chan->id);

	dev_dbg(chan->hdma->dev,
		"\tEN[0x%x] DB[0x%x] LLP_LO[0x%x] LLP_HI[0x%x]",
		readl(chan->ch_regs + PCIE_HDMA_CHAN_EN),
		readl(chan->ch_regs + PCIE_HDMA_CHAN_DB),
		readl(chan->ch_regs + PCIE_HDMA_CHAN_LLP_LO),
		readl(chan->ch_regs + PCIE_HDMA_CHAN_LLP_HI));
	dev_dbg(chan->hdma->dev,
		"\tSAR_LO[0x%x] SAR_HI[0x%x] DAR_LO[0x%x] DAR_HI[0x%x]",
		readl(chan->ch_regs + PCIE_HDMA_CHAN_SAR_LO),
		readl(chan->ch_regs + PCIE_HDMA_CHAN_SAR_HI),
		readl(chan->ch_regs + PCIE_HDMA_CHAN_DAR_LO),
		readl(chan->ch_regs + PCIE_HDMA_CHAN_DAR_HI));
	dev_dbg(chan->hdma->dev, "\tINT_STS[0x%x] INT_SET[0x%x] STS[0x%x]",
		readl(chan->ch_regs + PCIE_HDMA_CHAN_INT_STS),
		readl(chan->ch_regs + PCIE_HDMA_CHAN_INT_SET),
		readl(chan->ch_regs + PCIE_HDMA_CHAN_STS));
	dev_dbg(chan->hdma->dev, "\tTRSZ[0x%x] CTRL1[0x%x]",
		readl(chan->ch_regs + PCIE_HDMA_CHAN_TRSIZE),
		readl(chan->ch_regs + PCIE_HDMA_CHAN_CTRL1));
#else
	(void)chan;
#endif
}

static inline void
xh2a_pcie_hdma_chan_handle_err(struct xh2a_pcie_hdma_chan *chan)
{
	xh2a_pcie_hdma_chan_stop(chan);
	xh2a_pcie_hdma_chan_disable(chan);
}

static inline void
xh2a_pcie_hdma_chan_block_xfer_complete(struct xh2a_pcie_hdma_chan *chan)
{
	complete(&chan->xfer_completion);
}

static void xh2a_pcie_hdma_work_handler(struct work_struct *work)
{
	unsigned long flags;
	struct xh2a_pcie_dev *p_xh2a;
	int i;
	uint32_t chan_status, int_status;
	struct xh2a_pcie_hdma_chan *chan;
	struct xh2a_pcie_hdma *hdma =
		container_of(work, struct xh2a_pcie_hdma, hdma_work);
	p_xh2a = container_of(hdma, struct xh2a_pcie_dev, hdma);
	spin_lock_irqsave(&p_xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);
	if (xh2a_pcie_mmio_blocked(p_xh2a) || hdma->teardown)
		goto unlock;
	dev_dbg(hdma->dev, "%s: %p\n", __func__, hdma);

	for (i = 0; i < XH2A_PCIE_HDMA_NWRCHAN; i++) {
		chan = &hdma->wrchan[i];
		chan_status = readl(chan->ch_regs + PCIE_HDMA_CHAN_STS);
		int_status = readl(chan->ch_regs + PCIE_HDMA_CHAN_INT_STS);

		xh2a_pcie_hdma_chan_controller_dump(chan);

		if (chan->dma_active && (chan_status == PCIE_HDMA_CHAN_STS_STOPPED) &&
		    (int_status & PCIE_HDMA_INT_STOP_CLR)) {
			dev_dbg(hdma->dev, "%s: wr channel %d stopped",
				__func__, i);
			xh2a_pcie_hdma_chan_irq_clear(chan);
			chan->dma_active = false;
			chan->result = 0;
			xh2a_pcie_hdma_chan_block_xfer_complete(chan);
		} else if (chan->dma_active && (chan_status == PCIE_HDMA_CHAN_STS_ABORTED) &&
			   (int_status & PCIE_HDMA_INT_ABORT_CLR)) {
			dev_dbg(hdma->dev, "%s: wr channel %d aborted %x",
				__func__, i, int_status);
			xh2a_pcie_hdma_chan_irq_clear(chan);
			chan->dma_active = false;
			xh2a_pcie_hdma_chan_handle_err(chan);
			chan->result = -EIO;
			xh2a_pcie_hdma_chan_block_xfer_complete(chan);
		} else {
			/* supperess debug outputs */
			/* dev_dbg(hdma->dev, "%s: ignore wr channel %d",
			   __func__, i); */
		}
	}

	for (i = 0; i < XH2A_PCIE_HDMA_NRDCHAN; i++) {
		chan = &hdma->rdchan[i];
		chan_status = readl(chan->ch_regs + PCIE_HDMA_CHAN_STS);
		int_status = readl(chan->ch_regs + PCIE_HDMA_CHAN_INT_STS);

		xh2a_pcie_hdma_chan_controller_dump(chan);

		if (chan->dma_active && (chan_status == PCIE_HDMA_CHAN_STS_STOPPED) &&
		    (int_status & PCIE_HDMA_INT_STOP_CLR)) {
			dev_dbg(hdma->dev, "%s: rd channel %d stopped",
				__func__, i);
			xh2a_pcie_hdma_chan_irq_clear(chan);
			chan->dma_active = false;
			chan->result = 0;
			xh2a_pcie_hdma_chan_block_xfer_complete(chan);
		} else if (chan->dma_active && (chan_status == PCIE_HDMA_CHAN_STS_ABORTED) &&
			   (int_status & PCIE_HDMA_INT_ABORT_CLR)) {
			dev_dbg(hdma->dev, "%s: rd channel %d aborted %x",
				__func__, i, int_status);
			xh2a_pcie_hdma_chan_irq_clear(chan);
			chan->dma_active = false;
			xh2a_pcie_hdma_chan_handle_err(chan);
			chan->result = -EIO;
			xh2a_pcie_hdma_chan_block_xfer_complete(chan);
		} else {
			/* supperess debug outputs */
			/* dev_dbg(hdma->dev, "%s: ignore rd channel %d",
			   __func__, i); */
		}
	}
unlock:
	spin_unlock_irqrestore(&p_xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);
}

static int hdma_available(struct xh2a_pcie_hdma_chan *channels,
			   unsigned long *busy, int count)
{
	int i;

	for (i = 0; i < count; i++)
		if (!test_bit(i, busy) && !READ_ONCE(channels[i].quarantined))
			return i;
	return count;
}

struct xh2a_pcie_hdma_chan *xh2a_pcie_hdma_get_chan(struct xh2a_pcie_hdma *hdma,
						    int chan_type)
{
	struct xh2a_pcie_dev *xh2a = container_of(hdma, struct xh2a_pcie_dev, hdma);
	bool rd = chan_type == XH2A_PCIE_HDMA_CHAN_TYPE_RD;
	struct mutex *lock = rd ? &hdma->rd_mutex : &hdma->wr_mutex;
	wait_queue_head_t *wait = rd ? &hdma->rd_wq : &hdma->wr_wq;
	struct xh2a_pcie_hdma_chan *channels = rd ? hdma->rdchan : hdma->wrchan;
	unsigned long *busy = rd ? hdma->rd_bitmap : hdma->wr_bitmap;
	int count = rd ? XH2A_PCIE_HDMA_NRDCHAN : XH2A_PCIE_HDMA_NWRCHAN;
	long remaining = msecs_to_jiffies(XH2A_PCIE_HDMA_TIMEOUT_MS);
	int index, ret;

	if (!rd && chan_type != XH2A_PCIE_HDMA_CHAN_TYPE_WR)
		return ERR_PTR(-EINVAL);
	if (!kref_get_unless_zero(&xh2a->lifetime))
		return ERR_PTR(-ENODEV);
	for (;;) {
		ret = mutex_lock_interruptible(lock);
		if (ret)
			break;
		if (READ_ONCE(hdma->teardown) || atomic_read(&hdma->suspend_in_progress)) {
			mutex_unlock(lock);
			ret = -ESHUTDOWN;
			break;
		}
		index = hdma_available(channels, busy, count);
		if (index < count) {
			set_bit(index, busy);
			mutex_unlock(lock);
			return &channels[index];
		}
		mutex_unlock(lock);
		remaining = wait_event_interruptible_timeout(*wait,
			READ_ONCE(hdma->teardown) || atomic_read(&hdma->suspend_in_progress) ||
			hdma_available(channels, busy, count) < count, remaining);
		if (remaining <= 0) {
			ret = remaining ? remaining : -ETIMEDOUT;
			break;
		}
	}
	xh2a_pcie_put_lifetime(xh2a);
	return ERR_PTR(ret);
}

void xh2a_pcie_hdma_put_chan(struct xh2a_pcie_hdma *hdma, int chan_type,
			     struct xh2a_pcie_hdma_chan *chan)
{
	struct xh2a_pcie_dev *xh2a = container_of(hdma, struct xh2a_pcie_dev, hdma);
	bool rd = chan_type == XH2A_PCIE_HDMA_CHAN_TYPE_RD;
	struct mutex *lock = rd ? &hdma->rd_mutex : &hdma->wr_mutex;

	mutex_lock(lock);
	clear_bit(chan->id, rd ? hdma->rd_bitmap : hdma->wr_bitmap);
	wake_up_interruptible(rd ? &hdma->rd_wq : &hdma->wr_wq);
	mutex_unlock(lock);
	xh2a_pcie_put_lifetime(xh2a);
}

int xh2a_pcie_hdma_chan_transfer(struct xh2a_pcie_hdma_chan *chan,
				 dma_addr_t dma_dst, dma_addr_t dma_src,
				 size_t len)
{
	struct xh2a_pcie_dev *p_xh2a =
		container_of(chan->hdma, struct xh2a_pcie_dev, hdma);
	unsigned long flags;
	u64 generation;
	bool drain = false;
	int ret;

	spin_lock_irqsave(&p_xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);
	if (xh2a_pcie_mmio_blocked(p_xh2a)) {
		ret = -ESHUTDOWN;
		goto unlock;
	}
	if (atomic_read(&chan->chan_removed) || pci_channel_offline(p_xh2a->pdev)) {
		ret = -ENODEV;
		goto unlock;
	}
	if (atomic_read(&chan->hdma->suspend_in_progress)) {
		ret = -ECANCELED;
		goto unlock;
	}

	if (chan->quarantined || chan->dma_active) {
		ret = -EIO;
		goto unlock;
	}
	generation = chan->hdma->stop_generation;
	xh2a_pcie_hdma_chan_prepare(chan, dma_dst, dma_src, len);
	reinit_completion(&chan->xfer_completion);
	chan->result = -EINPROGRESS;
	chan->dma_active = true;
	xh2a_pcie_hdma_chan_start(chan);
	spin_unlock_irqrestore(&p_xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);

	ret = wait_for_completion_interruptible_timeout(
		&chan->xfer_completion,
		msecs_to_jiffies(XH2A_PCIE_HDMA_TIMEOUT_MS));

	/* Reset can complete the waiter. Never touch its old registers then. */
	spin_lock_irqsave(&p_xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);
	if (xh2a_pcie_mmio_blocked(p_xh2a)) {
		ret = -ESHUTDOWN;
		goto unlock;
	}
	if (atomic_read(&chan->chan_removed) || pci_channel_offline(p_xh2a->pdev)) {
		ret = -ENODEV;
		goto unlock;
	}
	/* A stop-completed old waiter may run only after resume cleared the
	 * suspend flag. It must still report cancellation, never success. */
	if (generation != chan->hdma->stop_generation ||
	    atomic_read(&chan->hdma->suspend_in_progress)) {
		ret = -ECANCELED;
		goto unlock;
	}
	if (ret <= 0) {
		dev_err(chan->hdma->dev, "%s: timeout or interrupted %d\n",
			__func__, ret);
		if (chan->dma_active) {
			/* Never reuse a channel whose completion may arrive late. */
			WRITE_ONCE(chan->quarantined, true);
			drain = true;
			chan->hdma->faulted = true;
			atomic_set(&chan->hdma->suspend_in_progress, 1);
			xh2a_pcie_hdma_chan_stop(chan);
			xh2a_pcie_hdma_chan_disable(chan);
		}
		if (!ret)
			ret = -ETIMEDOUT;
		goto unlock;
	}
	ret = chan->result;
unlock:
	spin_unlock_irqrestore(&p_xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);
	if (drain) {
		int stop = xh2a_pcie_hdma_stop(chan->hdma);

		/*
		 * Keep this function closed after a timeout. Other cards have
		 * independent engines, locks and cancellation generations.
		 */
		pci_clear_master(p_xh2a->pdev);
		if (!pci_wait_for_pending_transaction(p_xh2a->pdev) || stop)
			dev_err(chan->hdma->dev, "HDMA drain unconfirmed (%d)\n", stop);
	}
	return ret;
}

void xh2a_pcie_hdma_wake_waiters_locked(struct xh2a_pcie_hdma *hdma)
{
	int i;

	for (i = 0; i < XH2A_PCIE_HDMA_NWRCHAN; i++)
		complete_all(&hdma->wrchan[i].xfer_completion);
	for (i = 0; i < XH2A_PCIE_HDMA_NRDCHAN; i++)
		complete_all(&hdma->rdchan[i].xfer_completion);
	wake_up_interruptible(&hdma->wr_wq);
	wake_up_interruptible(&hdma->rd_wq);
}

static int xh2a_pcie_hdma_chan_idle(struct xh2a_pcie_hdma_chan *chan)
{
	u32 state;

	if (!chan->dma_active)
		return true;
	state = readl(chan->ch_regs + PCIE_HDMA_CHAN_STS);
	if (state == ~0U)
		return -ENODEV;
	if (state != PCIE_HDMA_CHAN_STS_STOPPED &&
	    state != PCIE_HDMA_CHAN_STS_ABORTED)
		return false;
	chan->dma_active = false;
	return true;
}

int xh2a_pcie_hdma_stop(struct xh2a_pcie_hdma *hdma)
{
	struct xh2a_pcie_dev *xh2a =
		container_of(hdma, struct xh2a_pcie_dev, hdma);
	unsigned long flags, deadline;
	bool idle;
	int i, ret = 0;

	spin_lock_irqsave(&xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);
	hdma->stop_generation++;
	atomic_set(&hdma->suspend_in_progress, 1);
	if (xh2a_pcie_mmio_blocked(xh2a) || pci_channel_offline(xh2a->pdev)) {
		ret = -ESHUTDOWN;
		goto wake;
	}
	for (i = 0; i < XH2A_PCIE_HDMA_NWRCHAN; i++) {
		xh2a_pcie_hdma_chan_stop(&hdma->wrchan[i]);
		xh2a_pcie_hdma_chan_disable(&hdma->wrchan[i]);
	}
	for (i = 0; i < XH2A_PCIE_HDMA_NRDCHAN; i++) {
		xh2a_pcie_hdma_chan_stop(&hdma->rdchan[i]);
		xh2a_pcie_hdma_chan_disable(&hdma->rdchan[i]);
	}
	spin_unlock_irqrestore(&xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);
	/* Keep the gate closed to new transfers, but do not busy-wait with IRQs off. */
	deadline = jiffies + msecs_to_jiffies(100);
	do {
		spin_lock_irqsave(&xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);
		if (xh2a_pcie_mmio_blocked(xh2a) || pci_channel_offline(xh2a->pdev)) {
			ret = -ESHUTDOWN;
			goto wake;
		}
		idle = true;
		for (i = 0; i < XH2A_PCIE_HDMA_NWRCHAN; i++) {
			ret = xh2a_pcie_hdma_chan_idle(&hdma->wrchan[i]);
			if (ret < 0)
				goto wake;
			idle &= ret;
		}
		for (i = 0; i < XH2A_PCIE_HDMA_NRDCHAN; i++) {
			ret = xh2a_pcie_hdma_chan_idle(&hdma->rdchan[i]);
			if (ret < 0)
				goto wake;
			idle &= ret;
		}
		if (idle) {
			ret = 0;
			goto wake;
		}
		spin_unlock_irqrestore(&xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);
		usleep_range(100, 200);
	} while (time_before(jiffies, deadline));
	spin_lock_irqsave(&xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);
	ret = -ETIMEDOUT;
wake:
	xh2a_pcie_hdma_wake_waiters_locked(hdma);
	spin_unlock_irqrestore(&xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);
	return ret;
}

/* Called with the endpoint MMIO gate held, before its final reset write.
 * No completion wait is allowed here. Wake waiters so they return without
 * accessing registers once reset_pending is published under the same lock.
 */
void xh2a_pcie_hdma_quiesce_locked(struct xh2a_pcie_hdma *hdma)
{
	struct xh2a_pcie_dev *p_xh2a =
		container_of(hdma, struct xh2a_pcie_dev, hdma);
	int i;

	lockdep_assert_held(&p_xh2a->iatu_lock[XH2A_PCIE_REGBAR]);
	hdma->stop_generation++;
	atomic_set(&hdma->suspend_in_progress, 1);
	for (i = 0; i < XH2A_PCIE_HDMA_NWRCHAN; i++) {
		xh2a_pcie_hdma_chan_stop(&hdma->wrchan[i]);
		xh2a_pcie_hdma_chan_disable(&hdma->wrchan[i]);
		complete_all(&hdma->wrchan[i].xfer_completion);
	}
	for (i = 0; i < XH2A_PCIE_HDMA_NRDCHAN; i++) {
		xh2a_pcie_hdma_chan_stop(&hdma->rdchan[i]);
		xh2a_pcie_hdma_chan_disable(&hdma->rdchan[i]);
		complete_all(&hdma->rdchan[i].xfer_completion);
	}
}

void xh2a_pcie_hdma_resume(struct xh2a_pcie_hdma *hdma)
{
	struct xh2a_pcie_dev *xh2a = container_of(hdma, struct xh2a_pcie_dev, hdma);
	unsigned long flags;

	spin_lock_irqsave(&xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);
	if (!xh2a_pcie_mmio_blocked(xh2a) && !hdma->teardown && !hdma->faulted)
		atomic_set(&hdma->suspend_in_progress, 0);
	spin_unlock_irqrestore(&xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);
}

int hdma_init(struct xh2a_pcie_hdma *hdma, void __iomem *trgt0_mem,
	      struct msi_msg *msi, struct device *device)
{
	int ret, i;

	hdma->dev = device;

	atomic_set(&hdma->pcie_dma_xfer_complete, 0);
	atomic_set(&hdma->pcie_dma_read, 0);
	atomic_set(&hdma->pcie_dma_write, 0);
	atomic_set(&hdma->suspend_in_progress, 0);
	hdma->stop_generation = 0;
	hdma->reg_base = (uint8_t *)trgt0_mem + PCIE_HDMA_REGS_OFFSET;
	hdma->num_chans = XH2A_PCIE_HDMA_NWRCHAN + XH2A_PCIE_HDMA_NRDCHAN;
	hdma->msi = msi;

	dev_dbg(hdma->dev, "%s: 0x%llx, 0x%llx, 0x%llx\n", __func__,
		dma_get_required_mask(device), DMA_BIT_MASK(64),
		DMA_BIT_MASK(32));
	/*
	 * The M50/XH2A card is a 32-bit PCI requester and the platform runs
	 * PCIe 1:1 (no IOMMU, no dma-ranges inbound translation). Requesting
	 * a 64-bit coherent mask here let the 16x2MiB HDMA staging buffers
	 * land above 4GB (observed at 0x64c800000 and 0x650e00000), where
	 * the card cannot reach them: every HDMA transfer then waits out
	 * and returns -ERESTARTSYS, surfaced by the API layer as -512
	 * ("hdma xfer fail"). The placement depends on the free-page layout
	 * at allocation time, so the failure is probabilistic: the same
	 * board boots fine and then fails after a driver rebind. Cap the
	 * coherent mask at 32 bits so dma_alloc_coherent keeps the staging
	 * buffers inside the DMA32 zone; 16x2MiB = 32MiB fits easily.
	 * Verified on Y4 2026-11-13: with the patched module all buffers
	 * sit at ~2.1GB and the probe-time spm_restore_snapshot transfer
	 * succeeds where the stock module failed on rebind.
	 */
	ret = dma_set_coherent_mask(device, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	for (i = 0; i < XH2A_PCIE_HDMA_NWRCHAN; i++) {
		hdma->wrchan[i].hdma = hdma;
		hdma->wrchan[i].id = i;
		hdma->wrchan[i].ch_regs =
			hdma->reg_base + i * PCIE_HDMA_CHAN_BLK_SIZE * 2;
		hdma->wrchan[i].dma_cfg_buf = dma_alloc_coherent(
			hdma->dev, XH2A_PCIE_HDMA_BUF_SIZE,
			&hdma->wrchan[i].dma_cfg_phys, GFP_KERNEL);

		if (hdma->wrchan[i].dma_cfg_buf == NULL) {
			dev_err(hdma->dev,
				"%s: dma_alloc_coherent failed for wr channel "
				"%d\n",
				__func__, i);
			ret = -ENOMEM;
			goto dma_alloc_fail;
		}

		dev_dbg(hdma->dev, "%s: dma_alloc_coherent %p, 0x%llx\n",
			__func__, hdma->wrchan[i].dma_cfg_buf,
			hdma->wrchan[i].dma_cfg_phys);

		init_completion(&hdma->wrchan[i].xfer_completion);
		atomic_set(&hdma->wrchan[i].chan_removed, 0);
	}

	for (i = 0; i < XH2A_PCIE_HDMA_NRDCHAN; i++) {
		hdma->rdchan[i].hdma = hdma;
		hdma->rdchan[i].id = i;
		hdma->rdchan[i].ch_regs = hdma->reg_base +
					  i * PCIE_HDMA_CHAN_BLK_SIZE * 2 +
					  PCIE_HDMA_CHAN_BLK_SIZE;
		hdma->rdchan[i].dma_cfg_buf = dma_alloc_coherent(
			hdma->dev, XH2A_PCIE_HDMA_BUF_SIZE,
			&hdma->rdchan[i].dma_cfg_phys, GFP_KERNEL);

		if (hdma->rdchan[i].dma_cfg_buf == NULL) {
			dev_err(hdma->dev,
				"%s: dma_alloc_coherent failed for rd channel "
				"%d\n",
				__func__, i);
			ret = -ENOMEM;
			goto dma_alloc_fail;
		}

		dev_dbg(hdma->dev, "%s: dma_alloc_coherent %p, 0x%llx\n",
			__func__, hdma->rdchan[i].dma_cfg_buf,
			hdma->rdchan[i].dma_cfg_phys);

		init_completion(&hdma->rdchan[i].xfer_completion);
		atomic_set(&hdma->rdchan[i].chan_removed, 0);
	}

	mutex_init(&hdma->wr_mutex);
	mutex_init(&hdma->rd_mutex);

	init_waitqueue_head(&hdma->wr_wq);
	init_waitqueue_head(&hdma->rd_wq);

	bitmap_zero(hdma->wr_bitmap, XH2A_PCIE_HDMA_NWRCHAN);
	bitmap_zero(hdma->rd_bitmap, XH2A_PCIE_HDMA_NRDCHAN);

	xh2a_pcie_hdma_enable(hdma);

	INIT_WORK(&hdma->hdma_work, xh2a_pcie_hdma_work_handler);
	return 0;

dma_alloc_fail:

	for (i = 0; i < XH2A_PCIE_HDMA_NWRCHAN; i++) {
		if (hdma->wrchan[i].dma_cfg_buf != NULL)
			dma_free_coherent(hdma->dev, XH2A_PCIE_HDMA_BUF_SIZE,
					  hdma->wrchan[i].dma_cfg_buf,
					  hdma->wrchan[i].dma_cfg_phys);
	}

	for (i = 0; i < XH2A_PCIE_HDMA_NRDCHAN; i++) {
		if (hdma->rdchan[i].dma_cfg_buf != NULL)
			dma_free_coherent(hdma->dev, XH2A_PCIE_HDMA_BUF_SIZE,
					  hdma->rdchan[i].dma_cfg_buf,
					  hdma->rdchan[i].dma_cfg_phys);
	}

	return ret;
}

int xh2a_pcie_hdma_begin_remove(struct xh2a_pcie_hdma *hdma)
{
	struct xh2a_pcie_dev *xh2a =
		container_of(hdma, struct xh2a_pcie_dev, hdma);
	unsigned long flags;
	int i;

	/* The IRQ producer checks teardown under this same gate. */
	spin_lock_irqsave(&xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);
	WRITE_ONCE(hdma->teardown, true);
	atomic_set(&hdma->suspend_in_progress, 1);
	for (i = 0; i < XH2A_PCIE_HDMA_NWRCHAN; i++)
		atomic_set(&hdma->wrchan[i].chan_removed, 1);
	for (i = 0; i < XH2A_PCIE_HDMA_NRDCHAN; i++)
		atomic_set(&hdma->rdchan[i].chan_removed, 1);
	spin_unlock_irqrestore(&xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);
	return xh2a_pcie_hdma_stop(hdma);
}

void hdma_deinit(struct xh2a_pcie_hdma *hdma)
{
	int i, ret;

	ret = xh2a_pcie_hdma_begin_remove(hdma);
	cancel_work_sync(&hdma->hdma_work);
	/* A reset request or cleared PCI BME is not a DMA completion. Keep any
	 * allocation whose ownership could not be recovered; never recycle it. */
	for (i = 0; i < XH2A_PCIE_HDMA_NWRCHAN; i++) {
		if (!hdma->wrchan[i].dma_cfg_buf)
			continue;
		if (hdma->wrchan[i].dma_active || test_bit(i, hdma->wr_bitmap)) {
			dev_err(hdma->dev, "retaining HDMA write buffer %d (stop %d)\n", i, ret);
			continue;
		}
		dma_free_coherent(hdma->dev, XH2A_PCIE_HDMA_BUF_SIZE,
			hdma->wrchan[i].dma_cfg_buf, hdma->wrchan[i].dma_cfg_phys);
		hdma->wrchan[i].dma_cfg_buf = NULL;
	}
	for (i = 0; i < XH2A_PCIE_HDMA_NRDCHAN; i++) {
		if (!hdma->rdchan[i].dma_cfg_buf)
			continue;
		if (hdma->rdchan[i].dma_active || test_bit(i, hdma->rd_bitmap)) {
			dev_err(hdma->dev, "retaining HDMA read buffer %d (stop %d)\n", i, ret);
			continue;
		}
		dma_free_coherent(hdma->dev, XH2A_PCIE_HDMA_BUF_SIZE,
			hdma->rdchan[i].dma_cfg_buf, hdma->rdchan[i].dma_cfg_phys);
		hdma->rdchan[i].dma_cfg_buf = NULL;
	}
}
