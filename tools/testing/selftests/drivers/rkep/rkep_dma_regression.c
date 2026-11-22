// SPDX-License-Identifier: GPL-2.0
/* Offline test of the shipping DMA helpers. No PCI function or physical DMA. */
#include <linux/completion.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/xarray.h>
#include <linux/workqueue.h>

static bool fail_publish;
static unsigned int fail_allocation;

static void *test_alloc(struct device *dev, size_t size, gfp_t flags)
{
	if (fail_allocation && !--fail_allocation)
		return NULL;
	return devm_kzalloc(dev, size, flags);
}

static int test_xa_alloc(struct xarray *xa, u32 *id, void *entry,
			 struct xa_limit limit, gfp_t flags)
{
	if (fail_publish)
		return -ENOMEM;
	return xa_alloc(xa, id, entry, limit, flags);
}

#define devm_kzalloc test_alloc
#define xa_alloc test_xa_alloc
#include "pcie-dw-dmatest.c"
#undef xa_alloc
#undef devm_kzalloc

/* Thirteen exceeds both the old limit and the twelve-slot Y12. */
#define TEST_COUNT 13

struct slot {
	struct device *dev;
	struct dma_trx_obj *obj;
	struct work_struct publish;
	struct rw_semaphore buffers;
	int result;
};
static struct slot slots[TEST_COUNT];
static DECLARE_COMPLETION(launch);
static DECLARE_COMPLETION(transfer_started);
static int transfer_result;
static unsigned int failure_count;
static struct completion debug_ch_started[2];

static void count_failure(struct dma_trx_obj *obj)
{
	failure_count++;
}

static void publish_slot(struct work_struct *work)
{
	struct slot *slot = container_of(work, struct slot, publish);

	wait_for_completion(&launch);
	slot->result = pcie_dw_dmatest_publish(slot->obj);
}

static void no_config(struct dma_table *table)
{
}

static void fake_start(struct dma_trx_obj *obj, struct dma_table *table)
{
	complete(&transfer_started);
	complete(&debug_ch_started[table->chn]);
}

static int still_running(struct dma_trx_obj *obj, u8 ch, enum dma_dir dir)
{
	return 0;
}

static void transfer(struct work_struct *work)
{
	transfer_result = pcie_dw_local_dma_tobus_block(slots[0].obj, 0, 0, 0, 32);
}
static DECLARE_WORK(transfer_work, transfer);

static int __init rkep_test_init(void)
{
	struct pcie_dw_dmatest_dev *d;
	unsigned long index;
	unsigned int i, n = 0;
	u32 original_id;
	char *buffer = NULL;
	dma_addr_t mapping = DMA_MAPPING_ERROR;
	int ret = -EINVAL;

	for (i = 0; i < ARRAY_SIZE(debug_ch_started); i++)
		init_completion(&debug_ch_started[i]);

	for (i = 0; i < TEST_COUNT; i++) {
		char name[32];

		snprintf(name, sizeof(name), "rkep-regression-%u", i);
		slots[i].dev = root_device_register(name);
		if (IS_ERR(slots[i].dev))
			goto cleanup;
		slots[i].obj = pcie_dw_dmatest_register(slots[i].dev, false);
		if (IS_ERR(slots[i].obj))
			goto cleanup;
		init_rwsem(&slots[i].buffers);
		slots[i].obj->buffer_lock = &slots[i].buffers;
		slots[i].obj->config_dma_func = no_config;
		slots[i].obj->start_dma_func = fake_start;
		slots[i].obj->get_dma_status = still_running;
		slots[i].obj->transfer_failed = count_failure;
		INIT_WORK(&slots[i].publish, publish_slot);
	}
	for (i = 1; i <= 2; i++) {
		fail_allocation = i;
		if (PTR_ERR_OR_ZERO(pcie_dw_dmatest_register(slots[0].dev, false)) != -ENOMEM)
			goto cleanup;
	}
	fail_publish = true;
	if (pcie_dw_dmatest_publish(slots[0].obj) != -ENOMEM || !xa_empty(&rkep_test_devices))
		goto cleanup;
	fail_publish = false;

	for (i = 0; i < TEST_COUNT; i++)
		queue_work(system_unbound_wq, &slots[i].publish);
	complete_all(&launch);
	for (i = 0; i < TEST_COUNT; i++)
		flush_work(&slots[i].publish);
	xa_for_each(&rkep_test_devices, index, d)
		n++;
	if (n != TEST_COUNT)
		goto cleanup;
	for (i = 0; i < TEST_COUNT; i++) {
		d = slots[i].obj->priv;
		if (slots[i].result || d->id >= TEST_COUNT || xa_load(&rkep_test_devices, d->id) != d)
			goto cleanup;
		if (pcie_dw_dmatest_publish(slots[i].obj) != -EALREADY)
			goto cleanup;
	}
	d = slots[0].obj->priv;
	original_id = d->id;
	test_dev = original_id;
	/* Check the active and missing debug selector without submitting raw DMA. */
	cycles_count = 0;
	if (pcie_dw_dmatest("run", NULL) != -EINVAL)
		goto cleanup;
	cycles_count = 1;

	for (i = 0; i < 2; i++) {
		/* Start a polling transfer, then an interrupt-waiting transfer. */
		pcie_dw_dmatest_irq_en(slots[0].obj, i, i);
		reinit_completion(&transfer_started);
		queue_work(system_unbound_wq, &transfer_work);
		if (!wait_for_completion_timeout(&transfer_started, 5 * HZ))
			goto cleanup;
		if (down_write_trylock(&slots[0].buffers)) {
			up_write(&slots[0].buffers);
			goto cleanup;
		}
		pcie_dw_dmatest_unregister(slots[0].obj);
		flush_work(&transfer_work);
		if (transfer_result != -ENODEV || pcie_dw_dmatest("run", NULL) != -ENODEV)
			goto cleanup;
		if (pcie_dw_local_dma_tobus_block(slots[0].obj, 0, 0, 0, 32) != -ENODEV)
			goto cleanup;
		if (pcie_dw_dmatest_publish(slots[0].obj) || d->id != original_id)
			goto cleanup;
	}
	/* Removal cancels both asynchronous diagnostic workers on this object.
	 * Provide valid local mappings for the cache maintenance calls. The
	 * simulated DMA callbacks do not issue any hardware accesses.
	 */
	if (dma_coerce_mask_and_coherent(slots[0].dev, DMA_BIT_MASK(64)))
		goto cleanup;
	buffer = kzalloc(1024, GFP_KERNEL);
	if (!buffer)
		goto cleanup;
	mapping = dma_map_single(slots[0].dev, buffer, 1024, DMA_TO_DEVICE);
	if (dma_mapping_error(slots[0].dev, mapping))
		goto cleanup;
	rw_test = 2;
	chn_en = 3;
	local_addr = mapping;
	bus_addr = 0;
	test_size = 256;
	for (i = 0; i < ARRAY_SIZE(debug_ch_started); i++)
		reinit_completion(&debug_ch_started[i]);
	if (pcie_dw_dmatest("run", NULL))
		goto cleanup;
	for (i = 0; i < ARRAY_SIZE(debug_ch_started); i++)
		if (!wait_for_completion_timeout(&debug_ch_started[i], 5 * HZ))
			goto cleanup;
	if (down_write_trylock(&slots[0].buffers)) {
		up_write(&slots[0].buffers);
		goto cleanup;
	}
	pcie_dw_dmatest_unregister(slots[0].obj);
	for (i = 0; i < ARRAY_SIZE(d->jobs); i++)
		if (work_busy(&d->jobs[i].work) || d->jobs[i].result != -ENODEV)
			goto cleanup;
	if (!down_write_trylock(&slots[0].buffers))
		goto cleanup;
	up_write(&slots[0].buffers);
	if (failure_count || pcie_dw_dmatest_publish(slots[0].obj))
		goto cleanup;

	/* A timeout poisons only the failed object until explicitly recovered. */
	if (pcie_dw_local_dma_tobus_block(slots[0].obj, 0, 0, 0, 32) != -ETIMEDOUT ||
	    pcie_dw_local_dma_tobus_block(slots[0].obj, 0, 0, 0, 32) != -ENODEV ||
	    failure_count != 1)
		goto cleanup;
	if (READ_ONCE(((struct pcie_dw_dmatest_dev *)slots[1].obj->priv)->status))
		goto cleanup;
	pcie_dw_dmatest_unregister(slots[0].obj);
	if (pcie_dw_dmatest_publish(slots[0].obj) ||
	    pcie_dw_local_dma_frombus_block(slots[0].obj, 0, 0, 0, 32) != -ETIMEDOUT ||
	    failure_count != 2)
		goto cleanup;

	/* Abort takes precedence over DONE in the same hardware IRQ. The
	 * notification is IRQ-safe and duplicate aborts do not queue more work.
	 */
	pcie_dw_dmatest_unregister(slots[0].obj);
	if (pcie_dw_dmatest_publish(slots[0].obj))
		goto cleanup;
	reinit_completion(&transfer_started);
	queue_work(system_unbound_wq, &transfer_work);
	if (!wait_for_completion_timeout(&transfer_started, 5 * HZ))
		goto cleanup;
	local_irq_disable();
	pcie_dw_dmatest_irq_abort(slots[0].obj);
	slots[0].obj->cb(slots[0].obj, 0, DMA_TO_BUS);
	pcie_dw_dmatest_irq_abort(slots[0].obj);
	local_irq_enable();
	flush_work(&transfer_work);
	if (transfer_result != -EIO || failure_count != 3 ||
	    !pcie_dw_dmatest_failed(slots[0].obj) || pcie_dw_dmatest_failed(NULL) ||
	    READ_ONCE(((struct pcie_dw_dmatest_dev *)slots[1].obj->priv)->status))
		goto cleanup;

	ret = 0;
cleanup:
	complete_all(&launch);
	for (i = 0; i < TEST_COUNT; i++)
		if (!IS_ERR_OR_NULL(slots[i].obj))
			pcie_dw_dmatest_unregister(slots[i].obj);
	flush_work(&transfer_work);
	if (mapping != DMA_MAPPING_ERROR)
		dma_unmap_single(slots[0].dev, mapping, 1024, DMA_TO_DEVICE);
	kfree(buffer);
	for (i = 0; i < TEST_COUNT; i++)
		if (!IS_ERR_OR_NULL(slots[i].dev))
			root_device_unregister(slots[i].dev);
	if (!xa_empty(&rkep_test_devices))
		ret = -EINVAL;
	pr_info("rkep-dma-regression: %s (13 devices, concurrent publication, OOM, drain, reuse, timeouts, abort+done)\n",
		ret ? "FAIL" : "PASS");
	return ret;
}

static void __exit rkep_test_exit(void)
{
}

module_init(rkep_test_init);
module_exit(rkep_test_exit);
