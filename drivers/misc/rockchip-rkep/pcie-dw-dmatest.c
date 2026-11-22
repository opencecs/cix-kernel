// SPDX-License-Identifier: (GPL-2.0+ OR MIT)
/*
 * Copyright (c) 2022 Rockchip Electronics Co., Ltd.
 */

#include <linux/init.h>
#include <linux/iopoll.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/err.h>
#include <linux/random.h>
#include <linux/refcount.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/rwsem.h>
#include <linux/workqueue.h>
#include <linux/xarray.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>

#include "pcie-dw-dmatest.h"
#include "rockchip-pcie-dma.h"

static int test_size = 0x20;
module_param_named(size,  test_size, int, 0644);
MODULE_PARM_DESC(size, "each packet size in bytes");

static unsigned int cycles_count = 1;
module_param(cycles_count, uint, 0644);
MODULE_PARM_DESC(cycles_count, "how many erase cycles to do (default 1)");

static unsigned int chn_en = 1;
module_param(chn_en, uint, 0644);
MODULE_PARM_DESC(chn_en, "Each bits for one dma channel, up to 2 channels, (default enable channel 0)");

static unsigned int rw_test = 3;
module_param(rw_test, uint, 0644);
MODULE_PARM_DESC(rw_test, "Read/Write test, 1-read 2-write 3-both(default 3)");

static unsigned long bus_addr = 0x3c000000;
module_param(bus_addr, ulong, 0644);
MODULE_PARM_DESC(bus_addr, "Dmatest chn0 bus_addr(remote), chn1 add offset 0x100000, (default 0x3c000000)");

static unsigned long local_addr = 0x3c000000;
module_param(local_addr, ulong, 0644);
MODULE_PARM_DESC(local_addr, "Dmatest chn0 local_addr(local), chn1 add offset 0x100000, (default 0x3c000000)");

static unsigned int test_dev;
module_param(test_dev, uint, 0644);
MODULE_PARM_DESC(test_dev, "Choose dma_obj device,(default 0)");

static bool is_wired;
module_param_named(is_wired, is_wired, bool, 0644);
MODULE_PARM_DESC(is_wired, "Transfer is triggered by wired DMA(default false)");

#define PCIE_DMA_CHANEL_MAX_NUM		2
#define PCIE_DMA_LL_MAX_NUM		1024 /* Unrestricted, tentative value */

struct rkep_test_job {
	struct work_struct work;
	struct dma_trx_obj *obj;
	u32 channel;
	u64 bus;
	u64 local;
	u32 size;
	u32 cycles;
	u32 directions;
	bool wired;
	int result;
};

struct pcie_dw_dmatest_dev {
	struct dma_trx_obj *obj;
	struct mutex submit_lock;
	refcount_t job_refs;
	struct rkep_test_job jobs[PCIE_DMA_CHANEL_MAX_NUM];
	int status;	/* zero when live, -ENODEV on removal, -EIO on DMA fault */
	bool published;	/* protected by rkep_test_devices_lock */
	u32 id;
	u8 gen;
	u8 lanes;

	bool wr_irq_en;
	bool rd_irq_en;
	struct completion rd_done[PCIE_DMA_CHANEL_MAX_NUM];
	struct completion wr_done[PCIE_DMA_CHANEL_MAX_NUM];

	struct mutex rd_lock[PCIE_DMA_CHANEL_MAX_NUM];	/* Corresponding to each read DMA channel */
	struct mutex wr_lock[PCIE_DMA_CHANEL_MAX_NUM];	/* Corresponding to each write DMA channel */

	struct dma_table rd_tbl_buf[PCIE_DMA_CHANEL_MAX_NUM];
	struct dma_table wr_tbl_buf[PCIE_DMA_CHANEL_MAX_NUM];
};

/* The debug selector is allocated per live PCI function. The mutex covers
 * publication and handing an object to a parameter callback.
 */
static DEFINE_XARRAY_ALLOC(rkep_test_devices);
static DEFINE_MUTEX(rkep_test_devices_lock);
static void rkep_test_work(struct work_struct *work);

static void rkep_wake_dma_waiters(struct pcie_dw_dmatest_dev *d)
{
	int chn;

	for (chn = 0; chn < PCIE_DMA_CHANEL_MAX_NUM; chn++) {
		complete_all(&d->rd_done[chn]);
		complete_all(&d->wr_done[chn]);
	}
}

/* A failed in-flight transfer may still be active in the endpoint. Block
 * additional submissions while the host driver's fault work fences this card.
 * transfer_failed must only mark/schedule recovery, never wait for this I/O.
 */
void pcie_dw_dmatest_irq_abort(struct dma_trx_obj *obj)
{
	struct pcie_dw_dmatest_dev *d = obj->priv;

	/* Used in hard IRQ context as well as after a synchronous timeout. */
	if (cmpxchg(&d->status, 0, -EIO) == 0) {
		if (obj->transfer_failed)
			obj->transfer_failed(obj);
		rkep_wake_dma_waiters(d);
	}
}

bool pcie_dw_dmatest_failed(struct dma_trx_obj *obj)
{
	struct pcie_dw_dmatest_dev *d;

	if (!obj)
		return false;
	d = obj->priv;
	return READ_ONCE(d->status) == -EIO;
}

static int rkep_dma_status(struct dma_trx_obj *obj, u32 chn, enum dma_dir dir)
{
	struct pcie_dw_dmatest_dev *d = obj->priv;
	int status = READ_ONCE(d->status);

	if (status)
		return status;
	return obj->get_dma_status(obj, chn, dir);
}

static int rk_pcie_dma_wait_for_finished(struct dma_trx_obj *obj, struct dma_table *table)
{
	struct pcie_dw_dmatest_dev *dmatest_dev = (struct pcie_dw_dmatest_dev *)obj->priv;
	int ret = 0, err;
	u64 poll_size;
	u32 theoretical_us;
	u32 bandwidth_mbps;
	u32 timeout_us;
	unsigned long sleep_us;

	/*
	 * As each get_dma_status cost 2us, polling status for small data:
	 *     Gen2x1 is abort 4KB/us;
	 *     Gen3x4 is abort 24KB/us;
	 * 16us is tolerant:
	 *     Gen2x1 64KB 16us;
	 *     Gen3x4 384KB 16us;
	 */
	switch (dmatest_dev->gen) {
	case 1:
		bandwidth_mbps = 250 * dmatest_dev->lanes;
		poll_size = 0x8000 * dmatest_dev->lanes;
		break;
	case 2:
		bandwidth_mbps = 500 * dmatest_dev->lanes;
		poll_size = 0x10000 * dmatest_dev->lanes;
		break;
	case 3:
	default:
		bandwidth_mbps = 800 * dmatest_dev->lanes;
		poll_size = 0x18000 * dmatest_dev->lanes;
		break;
	}

	theoretical_us = (u32)(table->buf_size / bandwidth_mbps);

	if (table->buf_size > poll_size && theoretical_us > 20)
		usleep_range(min(theoretical_us * 8 / 10, 1000U), min(theoretical_us, 2000U));

	timeout_us = (theoretical_us * 2) + 100000;

	sleep_us = (table->buf_size > poll_size) ? 20 : 0;

	err = read_poll_timeout(rkep_dma_status, ret,
				(ret == 1 || ret < 0),
				sleep_us, timeout_us, false,
				obj, table->chn, table->dir);

	if (err == -ETIMEDOUT) {
		ret = -ETIMEDOUT;
		goto err_out;
	}

	if (ret == -ENODEV)
		return ret;

	if (ret < 0) {
		ret = -EIO;
		goto err_out;
	}

	dev_dbg(obj->dev, "dma_wait size:%zu, gen:%d, lanes:%d\n",
		table->buf_size, dmatest_dev->gen, dmatest_dev->lanes);
	return READ_ONCE(dmatest_dev->status);

err_out:
	dev_err(obj->dev, "DMA wait timeout/error! size:%zu, gen:%d, lanes:%d\n",
		table->buf_size, dmatest_dev->gen, dmatest_dev->lanes);
	if (!READ_ONCE(dmatest_dev->status) && obj->dma_debug)
		obj->dma_debug(obj, table);
	return ret;
}

static int rk_pcie_local_dma_frombus_block(struct dma_trx_obj *obj, u32 chn,
					   u64 local_paddr, u64 bus_paddr, u32 size)
{
	struct pcie_dw_dmatest_dev *dmatest_dev = (struct pcie_dw_dmatest_dev *)obj->priv;
	struct dma_table *table;
	int ret;

	if (chn >= PCIE_DMA_CHANEL_MAX_NUM)
		return -EINVAL;

	mutex_lock(&dmatest_dev->rd_lock[chn]);

	table = &dmatest_dev->rd_tbl_buf[chn];
	memset(table, 0, sizeof(struct dma_table));

	if (dmatest_dev->rd_irq_en)
		reinit_completion(&dmatest_dev->rd_done[chn]);
	if (READ_ONCE(dmatest_dev->status)) {
		ret = -ENODEV;
		goto unlock;
	}

	table->buf_size = size;
	table->bus = bus_paddr;
	table->local = local_paddr;
	table->chn = chn;
	table->dir = DMA_FROM_BUS;

	obj->config_dma_func(table);
	obj->start_dma_func(obj, table);

	if (dmatest_dev->rd_irq_en) {
		ret = wait_for_completion_interruptible_timeout(&dmatest_dev->rd_done[chn], HZ);
		if (ret < 0)
			dev_err(obj->dev, "%s interrupted\n", __func__);
		else if (ret == 0)
			ret = -ETIMEDOUT;
		else
			ret = READ_ONCE(dmatest_dev->status);
	} else {
		ret = rk_pcie_dma_wait_for_finished(obj, table);
	}
	if (ret)
		pcie_dw_dmatest_irq_abort(obj);
unlock:
	mutex_unlock(&dmatest_dev->rd_lock[chn]);

	return ret;
}

static int rk_pcie_local_dma_tobus_block(struct dma_trx_obj *obj, u32 chn,
					 u64 bus_paddr, u64 local_paddr, u32 size)
{
	struct pcie_dw_dmatest_dev *dmatest_dev = (struct pcie_dw_dmatest_dev *)obj->priv;
	struct dma_table *table;
	int ret;

	if (chn >= PCIE_DMA_CHANEL_MAX_NUM)
		return -EINVAL;

	mutex_lock(&dmatest_dev->wr_lock[chn]);

	table = &dmatest_dev->wr_tbl_buf[chn];
	memset(table, 0, sizeof(struct dma_table));

	if (dmatest_dev->wr_irq_en)
		reinit_completion(&dmatest_dev->wr_done[chn]);
	if (READ_ONCE(dmatest_dev->status)) {
		ret = -ENODEV;
		goto unlock;
	}

	table->buf_size = size;
	table->bus = bus_paddr;
	table->local = local_paddr;
	table->chn = chn;
	table->dir = DMA_TO_BUS;

	obj->config_dma_func(table);
	obj->start_dma_func(obj, table);

	if (dmatest_dev->wr_irq_en) {
		ret = wait_for_completion_interruptible_timeout(&dmatest_dev->wr_done[chn], HZ);
		if (ret < 0)
			dev_err(obj->dev, "%s interrupted\n", __func__);
		else if (ret == 0)
			ret = -ETIMEDOUT;
		else
			ret = READ_ONCE(dmatest_dev->status);
	} else {
		ret = rk_pcie_dma_wait_for_finished(obj, table);
	}
	if (ret)
		pcie_dw_dmatest_irq_abort(obj);
unlock:
	mutex_unlock(&dmatest_dev->wr_lock[chn]);

	return ret;
}

static int rk_pcie_dma_interrupt_handler_call_back(struct dma_trx_obj *obj, u32 chn, enum dma_dir dir)
{
	struct pcie_dw_dmatest_dev *dmatest_dev = (struct pcie_dw_dmatest_dev *)obj->priv;

	if (chn >= PCIE_DMA_CHANEL_MAX_NUM)
		return -EINVAL;

	if (dir == DMA_FROM_BUS)
		complete(&dmatest_dev->rd_done[chn]);
	else
		complete(&dmatest_dev->wr_done[chn]);

	return 0;
}

struct dma_trx_obj *pcie_dw_dmatest_register(struct device *dev, bool irq_en)
{
	struct dma_trx_obj *obj;
	struct pcie_dw_dmatest_dev *dmatest_dev;
	int i;

	dmatest_dev = devm_kzalloc(dev, sizeof(struct pcie_dw_dmatest_dev), GFP_KERNEL);
	if (!dmatest_dev)
		return ERR_PTR(-ENOMEM);

	obj = devm_kzalloc(dev, sizeof(*obj), GFP_KERNEL);
	if (!obj)
		return ERR_PTR(-ENOMEM);

	obj->dev = dev;
	obj->priv = dmatest_dev;
	obj->cb = rk_pcie_dma_interrupt_handler_call_back;

	/* Save for dmatest */
	dmatest_dev->obj = obj;
	for (i = 0; i < PCIE_DMA_CHANEL_MAX_NUM; i++) {
		INIT_WORK(&dmatest_dev->jobs[i].work, rkep_test_work);
		dmatest_dev->jobs[i].obj = obj;
		dmatest_dev->jobs[i].channel = i;
		init_completion(&dmatest_dev->rd_done[i]);
		init_completion(&dmatest_dev->wr_done[i]);
		mutex_init(&dmatest_dev->rd_lock[i]);
		mutex_init(&dmatest_dev->wr_lock[i]);
	}

	/* Enable IRQ transfer as default */
	dmatest_dev->wr_irq_en = irq_en;
	dmatest_dev->rd_irq_en = irq_en;

	mutex_init(&dmatest_dev->submit_lock);
	dmatest_dev->status = -ENODEV;

	/* Define the high speed default gen/lanes to reach high poll_size */
	dmatest_dev->gen = 3;
	dmatest_dev->lanes = 4;

	return obj;
}

/* Called once the endpoint has installed its callbacks, IRQs and BARs. */
int pcie_dw_dmatest_publish(struct dma_trx_obj *obj)
{
	struct pcie_dw_dmatest_dev *d = obj->priv;
	int ret;

	mutex_lock(&rkep_test_devices_lock);
	ret = -EALREADY;
	if (!d->published) {
		ret = xa_alloc(&rkep_test_devices, &d->id, d, xa_limit_32b,
			       GFP_KERNEL);
		if (!ret) {
			d->published = true;
			smp_store_release(&d->status, 0);
		}
	}
	mutex_unlock(&rkep_test_devices_lock);
	return ret;
}

/* The two workers of one diagnostic request share a single buffer pin. */
static void rkep_test_put_buffers(struct pcie_dw_dmatest_dev *d)
{
	if (d->obj->buffer_lock && refcount_dec_and_test(&d->job_refs))
		up_read_non_owner(d->obj->buffer_lock);
}

/* Probe/remove and AER serialize publication for the same PCI function. */
void pcie_dw_dmatest_unregister(struct dma_trx_obj *obj)
{
	struct pcie_dw_dmatest_dev *d;
	int chn;

	if (!obj)
		return;
	d = obj->priv;
	mutex_lock(&rkep_test_devices_lock);
	if (d->published) {
		/* Preserve the error observed by in-flight waiters. */
		cmpxchg(&d->status, 0, -ENODEV);
		d->published = false;
		xa_erase(&rkep_test_devices, d->id);
	}
	mutex_unlock(&rkep_test_devices_lock);

	rkep_wake_dma_waiters(d);
	mutex_lock(&d->submit_lock);
	for (chn = 0; chn < PCIE_DMA_CHANEL_MAX_NUM; chn++) {
		if (cancel_work_sync(&d->jobs[chn].work))
			rkep_test_put_buffers(d);
	}
	mutex_unlock(&d->submit_lock);

	/* Drain transfers started through an ioctl as well as diagnostic work.
	 * The objects remain devm-owned until IRQ teardown has finished.
	 */
	for (chn = 0; chn < PCIE_DMA_CHANEL_MAX_NUM; chn++) {
		mutex_lock(&d->rd_lock[chn]);
		mutex_unlock(&d->rd_lock[chn]);
		mutex_lock(&d->wr_lock[chn]);
		mutex_unlock(&d->wr_lock[chn]);
	}
}

/* ioctl DMA and diagnostic work both retain the host buffers while in use. */
static int rkep_dma_block(struct dma_trx_obj *obj, u32 chn, u64 dst, u64 src,
			  u32 size, enum dma_dir dir)
{
	int ret;

	if (obj->buffer_lock)
		down_read(obj->buffer_lock);
	if (dir == DMA_TO_BUS)
		ret = rk_pcie_local_dma_tobus_block(obj, chn, dst, src, size);
	else
		ret = rk_pcie_local_dma_frombus_block(obj, chn, dst, src, size);
	if (obj->buffer_lock)
		up_read(obj->buffer_lock);
	return ret;
}

int pcie_dw_wired_dma_frombus_block(struct dma_trx_obj *obj, u32 chn,
				    u64 local_paddr, u64 bus_paddr, u32 size)
{
	return rkep_dma_block(obj, chn, local_paddr, bus_paddr, size, DMA_TO_BUS);
}

int pcie_dw_wired_dma_tobus_block(struct dma_trx_obj *obj, u32 chn,
				  u64 bus_paddr, u64 local_paddr, u32 size)
{
	return rkep_dma_block(obj, chn, bus_paddr, local_paddr, size, DMA_FROM_BUS);
}

int pcie_dw_local_dma_frombus_block(struct dma_trx_obj *obj, u32 chn,
				    u64 local_paddr, u64 bus_paddr, u32 size)
{
	return rkep_dma_block(obj, chn, local_paddr, bus_paddr, size, DMA_FROM_BUS);
}

int pcie_dw_local_dma_tobus_block(struct dma_trx_obj *obj, u32 chn,
				  u64 bus_paddr, u64 local_paddr, u32 size)
{
	return rkep_dma_block(obj, chn, bus_paddr, local_paddr, size, DMA_TO_BUS);
}

int pcie_dw_dmatest_irq_en(struct dma_trx_obj *obj, bool wr_irq_en, bool rd_irq_en)
{
	struct pcie_dw_dmatest_dev *dmatest_dev = (struct pcie_dw_dmatest_dev *)obj->priv;
	int chn;

	for (chn = 0; chn < PCIE_DMA_CHANEL_MAX_NUM; chn++)
		mutex_lock(&dmatest_dev->wr_lock[chn]);
	dmatest_dev->wr_irq_en = wr_irq_en;
	for (chn = 0; chn < PCIE_DMA_CHANEL_MAX_NUM; chn++)
		mutex_unlock(&dmatest_dev->wr_lock[chn]);

	for (chn = 0; chn < PCIE_DMA_CHANEL_MAX_NUM; chn++)
		mutex_lock(&dmatest_dev->rd_lock[chn]);
	dmatest_dev->rd_irq_en = rd_irq_en;
	for (chn = 0; chn < PCIE_DMA_CHANEL_MAX_NUM; chn++)
		mutex_unlock(&dmatest_dev->rd_lock[chn]);

	dev_info(obj->dev, "update wr_irq_en=%d rd_irq_en=%d\n", wr_irq_en, rd_irq_en);
	return 0;
}

int pcie_dw_dmatest_set_bandwidth(struct dma_trx_obj *obj, u8 gen, u8 lanes)
{
	struct pcie_dw_dmatest_dev *dmatest_dev = (struct pcie_dw_dmatest_dev *)obj->priv;

	dmatest_dev->gen = gen;
	dmatest_dev->lanes = max_t(u8, lanes, 1);

	return 0;
}

static int rkep_test_transfer(struct rkep_test_job *job)
{
	struct dma_trx_obj *obj = job->obj;
	struct pcie_dw_dmatest_dev *d = obj->priv;
	ktime_t start = ktime_get();
	u64 elapsed_us, bytes, kb_per_sec;
	u32 i;
	int ret;

	for (i = 0; i < job->cycles; i++) {
		if (READ_ONCE(d->status))
			return -ENODEV;
		if (job->directions & 1) {
			dma_sync_single_for_device(obj->dev, job->local, job->size, DMA_TO_DEVICE);
			if (job->wired)
				ret = rk_pcie_local_dma_tobus_block(obj, job->channel, job->local,
								     job->bus, job->size);
			else
				ret = rk_pcie_local_dma_frombus_block(obj, job->channel, job->local,
								     job->bus, job->size);
			if (ret)
				return ret;
			dma_sync_single_for_cpu(obj->dev, job->local, job->size, DMA_FROM_DEVICE);
		}
		if (job->directions & 2) {
			dma_sync_single_for_device(obj->dev, job->local, job->size, DMA_TO_DEVICE);
			if (job->wired)
				ret = rk_pcie_local_dma_frombus_block(obj, job->channel, job->bus,
								   job->local, job->size);
			else
				ret = rk_pcie_local_dma_tobus_block(obj, job->channel, job->bus,
								   job->local, job->size);
			if (ret)
				return ret;
		}
	}
	elapsed_us = max_t(s64, ktime_us_delta(ktime_get(), start), 1);
	bytes = (u64)job->size * job->cycles * hweight32(job->directions);
	kb_per_sec = mul_u64_u64_div_u64(bytes, USEC_PER_SEC, elapsed_us * 1024);
	dev_info(obj->dev, "DMA test ch%u: %u cycles of %u bytes in %llu us (%llu KiB/s)\n",
		 job->channel, job->cycles, job->size, elapsed_us, kb_per_sec);
	return 0;
}

static void rkep_test_work(struct work_struct *work)
{
	struct rkep_test_job *job = container_of(work, struct rkep_test_job, work);

	job->result = rkep_test_transfer(job);
	rkep_test_put_buffers(job->obj->priv);
	if (job->result)
		dev_dbg(job->obj->dev, "DMA test ch%u stopped: %d\n", job->channel, job->result);
}

static int dma_run(struct pcie_dw_dmatest_dev *d)
{
	int chn;

	if (!chn_en || chn_en > 3 || !rw_test || rw_test > 3 || test_size <= 0 || !cycles_count)
		return -EINVAL;
	for (chn = 0; chn < PCIE_DMA_CHANEL_MAX_NUM; chn++)
		flush_work(&d->jobs[chn].work);
	if (READ_ONCE(d->status))
		return -ENODEV;
	if (d->obj->buffer_lock) {
		down_read_non_owner(d->obj->buffer_lock);
		refcount_set(&d->job_refs, hweight32(chn_en));
	}
	for (chn = 0; chn < PCIE_DMA_CHANEL_MAX_NUM; chn++) {
		struct rkep_test_job *job = &d->jobs[chn];
		u32 offset = chn && chn_en == 3 ? test_size : 0;

		if (!(chn_en & BIT(chn)))
			continue;
		job->size = test_size;
		job->cycles = cycles_count;
		job->directions = rw_test;
		job->wired = is_wired;
		job->bus = bus_addr + offset;
		job->local = local_addr + offset;
		queue_work(system_unbound_wq, &job->work);
		if (chn_en != 3) {
			flush_work(&job->work);
			return job->result;
		}
	}
	return 0;
}

static int pcie_dw_dmatest(const char *val, const struct kernel_param *kp)
{
	struct pcie_dw_dmatest_dev *d;
	unsigned long id;
	int ret = 0;

	mutex_lock(&rkep_test_devices_lock);
	if (sysfs_streq(val, "show")) {
		xa_for_each(&rkep_test_devices, id, d)
			dev_info(d->obj->dev, "test_dev[%lu]%s\n", id,
				 id == test_dev ? " (selected)" : "");
	} else if (sysfs_streq(val, "run")) {
		d = xa_load(&rkep_test_devices, test_dev);
		if (!d)
			ret = -ENODEV;
		else if (!mutex_trylock(&d->submit_lock))
			ret = -EBUSY;
		else {
			mutex_unlock(&rkep_test_devices_lock);
			ret = dma_run(d);
			mutex_unlock(&d->submit_lock);
			return ret;
		}
	} else {
		ret = -EINVAL;
	}
	mutex_unlock(&rkep_test_devices_lock);
	return ret;
}

static const struct kernel_param_ops pcie_dw_dmatest_ops = {
	.set = pcie_dw_dmatest,
	.get = param_get_uint,
};

module_param_cb(dmatest, &pcie_dw_dmatest_ops, &test_dev, 0644);
MODULE_PARM_DESC(dmatest, "test rockchip pcie dma module");

MODULE_AUTHOR("Jon Lin");
MODULE_LICENSE("GPL");
