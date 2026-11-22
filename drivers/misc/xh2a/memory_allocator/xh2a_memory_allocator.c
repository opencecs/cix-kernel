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
#include <linux/vmalloc.h>
#include <linux/memory.h>
#include <linux/bitops.h>
#include <linux/math64.h>
#include <linux/sched.h>
#include <xh2a_host_drv.h>
#include <xh2a_pcie_api.h>
#include <xh2a_efuse.h>
#include <version_compat.h>
#include <xh2a_chrdev.h>
#include "xh2a_memory_allocator.h"
#include "allocator_algorithm.h"

struct mempool_parameter {
	char name[XH2A_MEMORY_ALLOCATOR_DEVICE_NAME_LEN];
	uint64_t start_addr;
	uint64_t total_size;
	uint32_t minor_offset;
};

static const struct mempool_parameter mempool_tbl[] = {
	{ XH2A_MEMORY_ALLOCATOR_MEMPOOL_DDR_NAME,
	  XH2A_MEMORY_ALLOCATOR_MEMPOOL_DDR_START_ADDR,
	  XH2A_MEMORY_ALLOCATOR_MEMPOOL_DDR_TOTAL_SIZE,
	  XH2A_CHRDEV_SUB_MEMORY_ALLOCATOR },
	{ XH2A_MEMORY_ALLOCATOR_MEMPOOL_SPM0_NAME,
	  XH2A_MEMORY_ALLOCATOR_MEMPOOL_SPM0_START_ADDR,
	  XH2A_MEMORY_ALLOCATOR_MEMPOOL_SPM0_TOTAL_SIZE,
	  XH2A_CHRDEV_SUB_MEMORY_ALLOCATOR + 1 },
	{ XH2A_MEMORY_ALLOCATOR_MEMPOOL_SPM1_NAME,
	  XH2A_MEMORY_ALLOCATOR_MEMPOOL_SPM1_START_ADDR,
	  XH2A_MEMORY_ALLOCATOR_MEMPOOL_SPM1_TOTAL_SIZE,
	  XH2A_CHRDEV_SUB_MEMORY_ALLOCATOR + 2 },
};
#define MEMPOOL_TABLE_SIZE \
	((sizeof(mempool_tbl) / sizeof(struct mempool_parameter)))

static void xh2a_memory_allocator_device_safe_release(struct kref *kref)
{
	struct xh2a_memory_allocator_dev *allocator_dev = container_of(
		kref, struct xh2a_memory_allocator_dev, dev_refcnt);

	allocator_core_destroy(&allocator_dev->allocator_mempool);
	kfree(allocator_dev);
}

static int xh2a_memory_allocator_get_drvdata(void *drvdata)
{
	struct xh2a_memory_allocator_dev *allocator_dev = drvdata;

	if (atomic_read(&allocator_dev->dev_initialized) == 0) {
		pr_err("%s: device not initialized\n", __func__);
		return -EAGAIN;
	}

	if (atomic_read(&allocator_dev->dev_removed)) {
		pr_err("%s: device mark removed\n", __func__);
		return -ENODEV;
	}

	if (kref_get_unless_zero(&allocator_dev->dev_refcnt) == 0) {
		pr_err("%s: device is removing...\n", __func__);
		return -ENODEV;
	}

	return 0;
}

static int xh2a_memory_allocator_open(struct inode *inode, struct file *filp)
{
	struct xh2a_memory_allocator_dev *allocator_dev = NULL;
	struct xh2a_memory_allocator_file_handle *fh = NULL;
	int ret;

	ret = xh2a_chrdev_get_drvdata(inode, xh2a_memory_allocator_get_drvdata,
				      (void **)&allocator_dev);
	if (ret)
		return ret;

	fh = kzalloc(sizeof(struct xh2a_memory_allocator_file_handle),
		     GFP_KERNEL);

	if (!fh) {
		dev_err(allocator_dev->cdev_dev, "%s: kzalloc failed\n",
			__func__);
		kref_put(&allocator_dev->dev_refcnt,
			 xh2a_memory_allocator_device_safe_release);
		return -ENOMEM;
	}

	fh->allocator = allocator_dev;
	fh->pid = current->pid;
	fh->tgid = current->tgid;
	INIT_LIST_HEAD(&fh->buffer_object_list);
	INIT_LIST_HEAD(&fh->node);

	if (atomic_read(&allocator_dev->dev_removed)) {
		kref_put(&allocator_dev->dev_refcnt,
			 xh2a_memory_allocator_device_safe_release);
		kfree(fh);
		pr_err("%s: device mark removed\n", __func__);
		return -ENODEV;
	}

	mutex_lock(&allocator_dev->allocator_mutex);
	list_add_tail(&fh->node, &allocator_dev->file_handle_list);
	mutex_unlock(&allocator_dev->allocator_mutex);

	filp->private_data = fh;

	return 0;
}

static int xh2a_memory_allocator_release(struct inode *inode, struct file *filp)
{
	struct xh2a_memory_allocator_dev *allocator_dev = NULL;
	struct xh2a_memory_allocator_file_handle *fh = NULL;
	struct xh2a_memory_allocator_buffer_object *pos, *n;

	fh = filp->private_data;
	filp->private_data = NULL;

	if (!fh)
		return 0;

	allocator_dev = fh->allocator;

	if (!allocator_dev) {
		kfree(fh);
		return 0;
	}

	mutex_lock(&allocator_dev->allocator_mutex);
	list_for_each_entry_safe(pos, n, &fh->buffer_object_list, node) {
		allocator_core_free(&allocator_dev->allocator_mempool,
				    pos->start, pos->size);
		list_del(&pos->node);
		kfree(pos);
	}
	list_del_init(&fh->node);
	mutex_unlock(&allocator_dev->allocator_mutex);
	kfree(fh);
	kref_put(&allocator_dev->dev_refcnt,
		 xh2a_memory_allocator_device_safe_release);
	return 0;
}

static int xh2a_memory_allocator_ioctl_alloc(
	struct xh2a_memory_allocator_dev *allocator_dev,
	struct xh2a_memory_allocator_file_handle *fh, unsigned long arg)
{
	int ret;
	struct xh2a_memory_allocator_request_arg ioc_request;
	struct xh2a_memory_allocator_buffer_object *bo;
	uint64_t allocated_paddr = 0;

	if (!allocator_dev->start_addr || !allocator_dev->total_size) {
		dev_err(allocator_dev->cdev_dev,
			"%s: allocator not initialized\n", __func__);
		ret = -EINVAL;
		goto out;
	}

	ret = copy_from_user(&ioc_request, (void __user *)arg,
			     sizeof(ioc_request));

	if (ret != 0) {
		dev_err(allocator_dev->cdev_dev, "%s: copy_from_user failed\n",
			__func__);
		ret = -EFAULT;
		goto out;
	}

	if (ioc_request.size == 0 ||
	    (ioc_request.size > allocator_dev->total_size)) {
		dev_err(allocator_dev->cdev_dev, "%s: size invalid\n",
			__func__);
		ret = -EINVAL;
		goto out;
	}

	allocated_paddr = allocator_core_alloc(
		&allocator_dev->allocator_mempool, ioc_request.size);

	if (!allocated_paddr) {
		dev_err(allocator_dev->cdev_dev, "%s: alloc failed\n",
			__func__);
		ret = -ENOMEM;
		goto out;
	}

	bo = kzalloc(sizeof(struct xh2a_memory_allocator_buffer_object),
		     GFP_KERNEL);

	if (!bo) {
		dev_err(allocator_dev->cdev_dev, "%s: kzalloc failed\n",
			__func__);
		ret = -ENOMEM;
		allocator_core_free(&allocator_dev->allocator_mempool,
				    allocated_paddr, ioc_request.size);
		goto out;
	}

	bo->start = allocated_paddr;
	bo->size = ioc_request.size;
	bo->pid = current->pid;
	bo->tgid = current->tgid;

	ioc_request.start = allocated_paddr;
	ret = copy_to_user((void __user *)arg, &ioc_request,
			   sizeof(ioc_request));

	if (ret) {
		ret = -EFAULT;
		dev_err(allocator_dev->cdev_dev, "%s: copy_to_user failed\n",
			__func__);
		allocator_core_free(&allocator_dev->allocator_mempool,
				    allocated_paddr, ioc_request.size);
		kfree(bo);
		goto out;
	}

	/*  add to fh list */
	list_add_tail(&bo->node, &fh->buffer_object_list);

out:
	return ret;
}

static int xh2a_memory_allocator_ioctl_free(
	struct xh2a_memory_allocator_dev *allocator_dev,
	struct xh2a_memory_allocator_file_handle *fh, unsigned long arg)
{
	int ret;
	struct xh2a_memory_allocator_request_arg ioc_request;
	struct xh2a_memory_allocator_buffer_object *pos, *n;
	int free_cnt = 0;

	ret = copy_from_user(&ioc_request, (void __user *)arg,
			     sizeof(ioc_request));

	if (ret != 0) {
		dev_err(allocator_dev->cdev_dev, "%s: copy_from_user failed\n",
			__func__);
		ret = -EFAULT;
		goto out;
	}

	list_for_each_entry_safe(pos, n, &fh->buffer_object_list, node) {
		if (pos->start == ioc_request.start) {
			list_del(&pos->node);
			allocator_core_free(&allocator_dev->allocator_mempool,
					    pos->start, pos->size);
			kfree(pos);
			free_cnt++;
			goto out;
		}
	}

	if (free_cnt != 1) {
		dev_err(allocator_dev->cdev_dev, "%s: free 0x%llx %d times\n",
			__func__, ioc_request.start, free_cnt);
		ret = -EINVAL;
		goto out;
	}

out:
	return ret;
}

static int xh2a_memory_allocator_ioctl_infomem(
	struct xh2a_memory_allocator_dev *allocator_dev,
	struct xh2a_memory_allocator_file_handle *fh, unsigned long arg)
{
	int ret;
	struct xh2a_memory_allocator_info_mem_arg ioc_infomem;

	ret = copy_from_user(&ioc_infomem, (void __user *)arg,
			     sizeof(ioc_infomem));

	if (ret != 0) {
		dev_err(allocator_dev->cdev_dev, "%s: copy_from_user failed\n",
			__func__);
		ret = -EFAULT;
		goto out;
	}
	ioc_infomem.start = allocator_dev->start_addr;
	ioc_infomem.total_size = allocator_dev->total_size;
	ioc_infomem.free_size = allocator_dev->allocator_mempool.free_size;
	ioc_infomem.max_free_buffer_size =
		allocator_core_get_max_free_buffer_size(
			&allocator_dev->allocator_mempool);

	ret = copy_to_user((void __user *)arg, &ioc_infomem,
			   sizeof(ioc_infomem));

	if (ret) {
		dev_err(allocator_dev->cdev_dev, "%s: copy_to_user failed\n",
			__func__);
		ret = -EFAULT;
		goto out;
	}

out:
	return ret;
}

static long xh2a_memory_allocator_ioctl(struct file *filp, unsigned int cmd,
					unsigned long arg)
{
	int ret;
	struct xh2a_memory_allocator_dev *allocator_dev = NULL;
	struct xh2a_memory_allocator_file_handle *fh = NULL;

	fh = filp->private_data;

	if (!fh) {
		pr_err("%s: fh is NULL\n", __func__);
		return -EINVAL;
	}

	allocator_dev = fh->allocator;

	if (!allocator_dev) {
		pr_err("%s: allocator is NULL\n", __func__);
		return -EINVAL;
	}

	if (atomic_read(&allocator_dev->dev_removed)) {
		pr_err("%s: device mark removed\n", __func__);
		return -ENODEV;
	}

	ret = mutex_lock_interruptible(&allocator_dev->allocator_mutex);

	if (ret != 0) {
		dev_err(allocator_dev->cdev_dev,
			"%s: mutex_lock_interruptible failed\n", __func__);
		return -ERESTARTSYS;
	}

	switch (cmd) {
	case IOCTL_XH2A_MEMORY_ALLOCATOR_ALLOC:
		ret = xh2a_memory_allocator_ioctl_alloc(allocator_dev, fh, arg);
		break;

	case IOCTL_XH2A_MEMORY_ALLOCATOR_FREE:
		ret = xh2a_memory_allocator_ioctl_free(allocator_dev, fh, arg);
		break;

	case IOCTL_XH2A_MEMORY_ALLOCATOR_GET_MEM_INFO:
		ret = xh2a_memory_allocator_ioctl_infomem(allocator_dev, fh,
							  arg);
		break;

	default:
		dev_err(allocator_dev->cdev_dev, "%s: unknown cmd 0x%x\n",
			__func__, cmd);
		break;
	}

	mutex_unlock(&allocator_dev->allocator_mutex);

	return ret;
}

static const struct file_operations xh2a_memory_allocator_fops = {
	.owner = THIS_MODULE,
	.open = xh2a_memory_allocator_open,
	.release = xh2a_memory_allocator_release,
	.unlocked_ioctl = xh2a_memory_allocator_ioctl,
};

struct xh2a_dump_buf {
	char *buf;
	size_t size;
	size_t len;
	bool overflow;
};

/*
 * Free hole histogram: bucket @i counts holes whose length in segments is
 * within [2^i, 2^(i+1) - 1]. Bounds the output to a fixed number of lines
 * no matter how many buffer objects the pool holds.
 */
#define XH2A_FRAG_HIST_ORDERS 64

/*
 * struct xh2a_frag_stats - fragmentation counters for one memory pool
 * @segment_size: allocation granularity in bytes
 * @allocated_size: bytes marked used in the pool bitmap
 * @free_size: bytes marked free in the pool bitmap
 * @max_free_hole: largest contiguous free run in bytes, before alignment
 * @free_hole_count: number of contiguous free runs
 * @alloc_run_count: number of contiguous allocated runs
 * @bo_count: buffer objects allocated through ioctl
 * @bo_request_total: sum of the sizes those buffers asked for
 * @bo_occupied_total: sum of the segment aligned sizes they really occupy
 * @hist_holes: free hole count per size order
 * @hist_bytes: free bytes per size order
 */
struct xh2a_frag_stats {
	uint64_t segment_size;
	uint64_t allocated_size;
	uint64_t free_size;
	uint64_t max_free_hole;
	uint64_t free_hole_count;
	uint64_t alloc_run_count;
	uint64_t bo_count;
	uint64_t bo_request_total;
	uint64_t bo_occupied_total;
	uint64_t hist_holes[XH2A_FRAG_HIST_ORDERS];
	uint64_t hist_bytes[XH2A_FRAG_HIST_ORDERS];
};

static void xh2a_dump_buf_init(struct xh2a_dump_buf *db, char *buf, size_t size)
{
	db->buf = buf;
	db->size = size;
	db->len = 0;
	db->overflow = false;
	buf[0] = '\0';
}

static __printf(2, 3) void xh2a_dump_buf_printf(struct xh2a_dump_buf *db,
						const char *fmt, ...)
{
	va_list args;
	size_t avail;
	int n;

	if (db->overflow)
		return;

	avail = db->size - db->len;

	va_start(args, fmt);
	n = vsnprintf(db->buf + db->len, avail, fmt, args);
	va_end(args);

	/* vsnprintf returns the length it wanted; >= avail means it got cut */
	if (n < 0 || (size_t)n >= avail) {
		db->overflow = true;
		return;
	}

	db->len += (size_t)n;
}

static int xh2a_frag_region_cb(struct allocator_mempool *mem,
			       const struct allocator_dump_region *region,
			       void *ctx)
{
	struct xh2a_frag_stats *st = ctx;
	uint64_t segs;
	int order;

	if (region->allocated) {
		st->alloc_run_count++;
		return 0;
	}

	st->free_hole_count++;
	if (region->size > st->max_free_hole)
		st->max_free_hole = region->size;

	segs = region->size >> mem->segment_shift;
	if (!segs)
		return 0;

	order = fls64(segs) - 1;
	if (order >= XH2A_FRAG_HIST_ORDERS)
		order = XH2A_FRAG_HIST_ORDERS - 1;

	st->hist_holes[order]++;
	st->hist_bytes[order] += region->size;
	return 0;
}

static void
xh2a_frag_account_bos(struct xh2a_memory_allocator_dev *allocator_dev,
		      struct xh2a_frag_stats *st)
{
	struct xh2a_memory_allocator_file_handle *fh_pos;
	struct xh2a_memory_allocator_buffer_object *bo_pos;

	list_for_each_entry(fh_pos, &allocator_dev->file_handle_list, node) {
		list_for_each_entry(bo_pos, &fh_pos->buffer_object_list, node) {
			st->bo_count++;
			st->bo_request_total += bo_pos->size;
			st->bo_occupied_total +=
				ALIGN(bo_pos->size, st->segment_size);
		}
	}
}

static void xh2a_frag_emit(struct xh2a_dump_buf *db,
			   const struct xh2a_frag_stats *st,
			   uint64_t start_addr, uint64_t total_size,
			   uint64_t max_free_buffer_size)
{
	uint64_t internal_waste = st->bo_occupied_total - st->bo_request_total;
	uint64_t external_frag_pct = 0;
	int order;

	if (st->free_size)
		external_frag_pct =
			div64_u64((st->free_size - st->max_free_hole) * 100,
				  st->free_size);

	xh2a_dump_buf_printf(db, "memory pool information:\n");
	xh2a_dump_buf_printf(db, "start_addr = 0x%llx\n",
			     (unsigned long long)start_addr);
	xh2a_dump_buf_printf(db, "total_size = 0x%llx\n",
			     (unsigned long long)total_size);
	xh2a_dump_buf_printf(db, "segment_size = 0x%llx\n",
			     (unsigned long long)st->segment_size);
	xh2a_dump_buf_printf(db, "allocated_size = 0x%llx\n",
			     (unsigned long long)st->allocated_size);
	xh2a_dump_buf_printf(db, "free_size = 0x%llx\n",
			     (unsigned long long)st->free_size);

	xh2a_dump_buf_printf(db, "\nexternal fragmentation:\n");
	xh2a_dump_buf_printf(db, "free_hole_count = %llu\n",
			     (unsigned long long)st->free_hole_count);
	xh2a_dump_buf_printf(db, "alloc_run_count = %llu\n",
			     (unsigned long long)st->alloc_run_count);
	xh2a_dump_buf_printf(db, "max_free_hole = 0x%llx\n",
			     (unsigned long long)st->max_free_hole);
	xh2a_dump_buf_printf(db, "max_free_buffer_size = 0x%llx\n",
			     (unsigned long long)max_free_buffer_size);
	xh2a_dump_buf_printf(db, "external_frag_pct = %llu\n",
			     (unsigned long long)external_frag_pct);
	xh2a_dump_buf_printf(db, "hole_distribution:\n");

	for (order = 0; order < XH2A_FRAG_HIST_ORDERS; order++) {
		if (!st->hist_holes[order])
			continue;

		xh2a_dump_buf_printf(db,
				     "  segs [%llu, %llu] holes %llu bytes "
				     "0x%llx\n",
				     1ULL << order, (2ULL << order) - 1,
				     (unsigned long long)st->hist_holes[order],
				     (unsigned long long)st->hist_bytes[order]);
	}

	xh2a_dump_buf_printf(db, "\ninternal fragmentation:\n");
	xh2a_dump_buf_printf(db, "bo_count = %llu\n",
			     (unsigned long long)st->bo_count);
	xh2a_dump_buf_printf(db, "bo_request_total = 0x%llx\n",
			     (unsigned long long)st->bo_request_total);
	xh2a_dump_buf_printf(db, "bo_occupied_total = 0x%llx\n",
			     (unsigned long long)st->bo_occupied_total);
	xh2a_dump_buf_printf(db, "internal_waste = 0x%llx\n",
			     (unsigned long long)internal_waste);
	xh2a_dump_buf_printf(db, "(buffer object list: sysfs alloc_details)\n");
}

/* sysfs: pool summary plus external and internal fragmentation */
static ssize_t xh2a_memory_allocator_info_show(struct device *dev,
					       struct device_attribute *attr,
					       char *buf)
{
	struct xh2a_memory_allocator_dev *allocator_dev = dev_get_drvdata(dev);
	struct xh2a_frag_stats *st;
	struct xh2a_dump_buf db;
	uint64_t start_addr, total_size, max_free;
	int ret;

	st = kzalloc(sizeof(*st), GFP_KERNEL);
	if (!st)
		return -ENOMEM;

	ret = mutex_lock_interruptible(&allocator_dev->allocator_mutex);

	if (ret) {
		dev_err(allocator_dev->cdev_dev,
			"%s: mutex_lock_interruptible failed\n", __func__);
		kfree(st);
		return ret;
	}

	start_addr = allocator_dev->start_addr;
	total_size = allocator_dev->total_size;
	st->segment_size = 1ULL
			   << allocator_dev->allocator_mempool.segment_shift;
	st->allocated_size = allocator_dev->allocator_mempool.allocated_size;
	st->free_size = allocator_dev->allocator_mempool.free_size;
	max_free = allocator_core_get_max_free_buffer_size(
		&allocator_dev->allocator_mempool);

	allocator_core_dump(&allocator_dev->allocator_mempool,
			    xh2a_frag_region_cb, st);
	xh2a_frag_account_bos(allocator_dev, st);

	mutex_unlock(&allocator_dev->allocator_mutex);

	/* show() always gets a PAGE_SIZE buffer */
	xh2a_dump_buf_init(&db, buf, PAGE_SIZE);
	xh2a_frag_emit(&db, st, start_addr, total_size, max_free);
	kfree(st);

	if (db.overflow) {
		dev_err(allocator_dev->cdev_dev,
			"%s: report does not fit one page\n", __func__);
		return -EOVERFLOW;
	}

	return db.len;
}
static DEVICE_ATTR_RO(xh2a_memory_allocator_info);

struct xh2a_dump_bo_entry {
	uint64_t start;
	uint64_t size;
	pid_t pid;
	pid_t tgid;
};

/* alloc_details lists at most this many buffer objects. */
#define XH2A_DUMP_BO_CAP       1024
#define XH2A_DUMP_BO_HDR_BYTES 512
#define XH2A_DUMP_BO_LINE_MAX  128

/*
 * Copy up to @cap buffer objects in file handle traversal order. The walk
 * stops as soon as @cap is reached, so the cost never grows with the number
 * of buffers the pool holds. Caller must hold allocator_mutex.
 */
static size_t xh2a_memory_allocator_collect_bos(
	struct xh2a_memory_allocator_dev *allocator_dev,
	struct xh2a_dump_bo_entry *entries, size_t cap, bool *truncated)
{
	struct xh2a_memory_allocator_file_handle *fh_pos;
	struct xh2a_memory_allocator_buffer_object *bo_pos;
	size_t count = 0;

	*truncated = false;

	list_for_each_entry(fh_pos, &allocator_dev->file_handle_list, node) {
		list_for_each_entry(bo_pos, &fh_pos->buffer_object_list, node) {
			if (count == cap) {
				*truncated = true;
				return count;
			}

			entries[count].start = bo_pos->start;
			entries[count].size = bo_pos->size;
			entries[count].pid = bo_pos->pid;
			entries[count].tgid = bo_pos->tgid;
			count++;
		}
	}

	return count;
}

/*
 * Build the buffer object listing. Returns buffer on success, ERR_PTR on
 * failure. Caller must kvfree() the buffer.
 */
static char *xh2a_memory_allocator_build_details(
	struct xh2a_memory_allocator_dev *allocator_dev, size_t *out_len)
{
	struct xh2a_dump_bo_entry *entries;
	struct xh2a_dump_buf db;
	char *kbuf;
	size_t ksize, shown, i;
	bool truncated;
	int ret;

	entries = kvmalloc(XH2A_DUMP_BO_CAP * sizeof(*entries), GFP_KERNEL);
	if (!entries)
		return ERR_PTR(-ENOMEM);

	ksize = XH2A_DUMP_BO_HDR_BYTES +
		XH2A_DUMP_BO_CAP * XH2A_DUMP_BO_LINE_MAX;
	kbuf = kvmalloc(ksize, GFP_KERNEL);
	if (!kbuf) {
		kvfree(entries);
		return ERR_PTR(-ENOMEM);
	}

	ret = mutex_lock_interruptible(&allocator_dev->allocator_mutex);
	if (ret) {
		kvfree(kbuf);
		kvfree(entries);
		return ERR_PTR(ret);
	}

	shown = xh2a_memory_allocator_collect_bos(allocator_dev, entries,
						  XH2A_DUMP_BO_CAP, &truncated);

	mutex_unlock(&allocator_dev->allocator_mutex);

	xh2a_dump_buf_init(&db, kbuf, ksize);
	xh2a_dump_buf_printf(&db, "allocated buffer objects:\n");
	xh2a_dump_buf_printf(&db, "bo_shown = %zu\n", shown);
	xh2a_dump_buf_printf(&db, "truncated = %d\n", truncated ? 1 : 0);

	if (truncated)
		xh2a_dump_buf_printf(&db,
				     "# OUTPUT TRUNCATED: the listing stops "
				     "after the first\n"
				     "# %d entries and the pool holds more "
				     "than that. Read\n"
				     "# the info attribute for the total "
				     "buffer count.\n",
				     XH2A_DUMP_BO_CAP);

	xh2a_dump_buf_printf(&db, "# Entries follow file handle order, not "
				  "address order.\n");

	for (i = 0; i < shown; i++)
		xh2a_dump_buf_printf(&db,
				     "bo: tgid = %d, pid = %d, addr = 0x%llx, "
				     "size = 0x%llx\n",
				     entries[i].tgid, entries[i].pid,
				     (unsigned long long)entries[i].start,
				     (unsigned long long)entries[i].size);

	kvfree(entries);

	if (db.overflow) {
		dev_err(allocator_dev->cdev_dev,
			"%s: listing buffer too small\n", __func__);
		kvfree(kbuf);
		return ERR_PTR(-EOVERFLOW);
	}

	*out_len = db.len;
	return kbuf;
}

/* sysfs bin: allocated buffer object listing, capped at XH2A_DUMP_BO_CAP */
static ssize_t xh2a_memory_allocator_alloc_details_read(
	struct file *filp, struct kobject *kobj,
	XH2A_BIN_ATTR_CONST struct bin_attribute *attr, char *buf, loff_t off,
	size_t count)
{
	struct device *dev = kobj_to_dev(kobj);
	struct xh2a_memory_allocator_dev *allocator_dev = dev_get_drvdata(dev);
	char *kbuf;
	size_t len = 0;
	ssize_t ret;
	loff_t pos = off;

	if (!allocator_dev)
		return -ENODEV;

	/*
	 * Do not stash state in filp->private_data: on sysfs/kernfs it holds
	 * struct seq_file for kernfs_of(). Overwriting it oopses on next read.
	 * kernfs hands out one PAGE_SIZE at a time, so a full listing costs
	 * one rebuild per page. XH2A_DUMP_BO_CAP keeps that bounded.
	 */
	kbuf = xh2a_memory_allocator_build_details(allocator_dev, &len);
	if (IS_ERR(kbuf))
		return PTR_ERR(kbuf);

	ret = memory_read_from_buffer(buf, count, &pos, kbuf, len);
	kvfree(kbuf);
	return ret;
}

static XH2A_BIN_ATTR_CONST struct bin_attribute xh2a_memory_allocator_alloc_details_attr = {
	.attr = {
		.name = "alloc_details",
		.mode = 0444,
	},
	.read = xh2a_memory_allocator_alloc_details_read,
};

static struct attribute *xh2a_memory_allocator_attrs[] = {
	&dev_attr_xh2a_memory_allocator_info.attr,
	NULL,
};

static XH2A_BIN_ATTR_CONST struct bin_attribute
	*xh2a_memory_allocator_bin_attrs[] = {
		&xh2a_memory_allocator_alloc_details_attr,
		NULL,
	};

static struct attribute_group xh2a_memory_allocator_group = {
	.attrs = xh2a_memory_allocator_attrs,
	.bin_attrs = xh2a_memory_allocator_bin_attrs,
};

static const struct attribute_group *xh2a_memory_allocator_groups[] = {
	&xh2a_memory_allocator_group,
	NULL,
};

/*
 * xh2a_memory_allocator_probe_one() - probe for one memory pool
 * @handle: handle for pcie device
 * @mempool_name: the name of the memory pool
 * @mempool_start: start address of the memory pool
 * @mempool_size: size of the memory pool
 */
static int xh2a_memory_allocator_probe_one(void *handle,
					   const char *mempool_name,
					   uint64_t mempool_start,
					   uint64_t mempool_size,
					   uint32_t minor_offset)
{
	int ret;
	struct xh2a_pcie_client *client = NULL;
	struct xh2a_memory_allocator_dev *allocator_dev = NULL;
	uint64_t size;

	pr_debug("%s: %s\n", __func__, mempool_name);

	allocator_dev =
		kzalloc(sizeof(struct xh2a_memory_allocator_dev), GFP_KERNEL);

	if (allocator_dev == NULL) {
		pr_err("%s: alloc allocator_dev failed\n", __func__);
		return -ENOMEM;
	}

	client = &allocator_dev->client;

	allocator_dev->private_data = handle;

	mutex_init(&allocator_dev->allocator_mutex);

	INIT_LIST_HEAD(&allocator_dev->file_handle_list);

	atomic_set(&allocator_dev->dev_initialized, 0);
	atomic_set(&allocator_dev->dev_removed, 0);
	kref_init(&allocator_dev->dev_refcnt);

	allocator_dev->parent_dev = xh2a_pcie_device_ptr(handle);
	allocator_dev->allocator_devt = xh2a_chrdev_devt(handle, minor_offset);
	allocator_dev->cdev_dev = xh2a_chrdev_create(
		xh2a_memory_allocator_class, allocator_dev->parent_dev,
		allocator_dev->allocator_devt, allocator_dev,
		&xh2a_memory_allocator_fops, xh2a_memory_allocator_groups,
		"%s%d_%s", XH2A_MEMORY_ALLOCATOR_DEVICE_NAME,
		xh2a_pcie_device_index(handle), mempool_name);
	if (IS_ERR(allocator_dev->cdev_dev)) {
		ret = PTR_ERR(allocator_dev->cdev_dev);
		pr_err("%s: xh2a_chrdev_create failed\n", __func__);
		goto err_chrdev_create;
	}

	allocator_dev->allocator_mempool.dev = allocator_dev->cdev_dev;

	size = mempool_size;
	if (strcmp(mempool_name, XH2A_MEMORY_ALLOCATOR_MEMPOOL_DDR_NAME) == 0) {
		ret = xh2a_pcie_efuse_update_ddr_size(handle, &size);
		if ((ret == 0) && (size != 0)) {
			size -= XH2A_DEVICE_SYSTEM_SIZE;
		} else {
			size = mempool_size;
		}
	}

	allocator_dev->start_addr = mempool_start;
	allocator_dev->total_size = size;
	ret = allocator_core_init(&allocator_dev->allocator_mempool,
				  allocator_dev->cdev_dev,
				  allocator_dev->start_addr,
				  allocator_dev->total_size);
	if (ret) {
		dev_err(allocator_dev->cdev_dev,
			"%s: allocator_core_init failed for pool size %llx\n",
			__func__, size);
		goto err_core_init;
	}
	snprintf(client->name, XH2A_PCIE_CLIENT_NAME_LEN,
		 XH2A_MEMORY_ALLOCATOR_DEVICE_NAME "_%s", mempool_name);

	client->work_num = 0;

	client->client_data = allocator_dev;
	client->private_data = handle;

	ret = xh2a_pcie_register_client(handle, client);

	if (ret != 0) {
		pr_err("%s: register client failed %d\n", __func__, ret);
		goto err_client_register;
	}

	atomic_set(&allocator_dev->dev_initialized, 1);

	return 0;

err_client_register:
	allocator_core_destroy(&allocator_dev->allocator_mempool);

err_core_init:
	xh2a_chrdev_destroy(xh2a_memory_allocator_class,
			    allocator_dev->allocator_devt, allocator_dev);

err_chrdev_create:
	kfree(allocator_dev);
	return ret;
}

/*
 * xh2a_memory_allocator_remove_one() - remove one memory pool
 * @handle: handle for pcie device
 * @mempool_name: the name of the memory pool
 */
static int xh2a_memory_allocator_remove_one(void *handle,
					    const char *mempool_name)
{
	struct xh2a_pcie_client *client = NULL;
	struct xh2a_memory_allocator_dev *allocator_dev;
	struct xh2a_memory_allocator_file_handle *fh_pos, *fh_n;
	struct xh2a_memory_allocator_buffer_object *bo_pos, *bo_n;
	char client_name[XH2A_PCIE_CLIENT_NAME_LEN];

	pr_debug("%s: %s\n", __func__, mempool_name);
	snprintf(client_name, XH2A_PCIE_CLIENT_NAME_LEN,
		 XH2A_MEMORY_ALLOCATOR_DEVICE_NAME "_%s", mempool_name);

	xh2a_pcie_get_client(handle, &client, client_name);

	if (client == NULL) {
		pr_debug("%s: client not found\n", __func__);
		return 0;
	}

	allocator_dev = client->client_data;

	atomic_set(&allocator_dev->dev_removed, 1);

	mutex_lock(&allocator_dev->allocator_mutex);
	list_for_each_entry_safe(fh_pos, fh_n, &allocator_dev->file_handle_list,
				 node) {
		list_for_each_entry_safe(bo_pos, bo_n,
					 &fh_pos->buffer_object_list, node) {
			allocator_core_free(&allocator_dev->allocator_mempool,
					    bo_pos->start, bo_pos->size);
			list_del(&bo_pos->node);
			kfree(bo_pos);
		}
		list_del_init(&fh_pos->node);
	}
	mutex_unlock(&allocator_dev->allocator_mutex);

	xh2a_chrdev_destroy(xh2a_memory_allocator_class,
			    allocator_dev->allocator_devt, allocator_dev);

	xh2a_pcie_unregister_client(handle, client);

	/*
	 * Don't NULL private_data here; chrdev_destroy + dev_removed prevent
	 * new ioctls and in-flight ones rely on the helper APIs to short-
	 * circuit. The kref keeps allocator_dev alive for any open fd.
	 */
	kref_put(&allocator_dev->dev_refcnt,
		 xh2a_memory_allocator_device_safe_release);
	return 0;
}

/*
 * xh2a_memory_allocator_probe()
 *     - probe function for memory_allocator device
 * @handle: handle of xh2a_pcie_dev
 * Return: 0 on success, negative error code on failure
 */
static int xh2a_memory_allocator_probe(void *handle)
{
	int i, ret;

	for (i = 0; i < MEMPOOL_TABLE_SIZE; i++) {
		ret = xh2a_memory_allocator_probe_one(
			handle, mempool_tbl[i].name, mempool_tbl[i].start_addr,
			mempool_tbl[i].total_size, mempool_tbl[i].minor_offset);

		if (ret != 0)
			return ret;
	}

	return 0;
}

/*
 * xh2a_memory_allocator_remove()
 *     - remove the memory_allocator device
 * @handle: pcie handle
 * Return: 0 on success, negative error code on failure
 */
static int xh2a_memory_allocator_remove(void *handle)
{
	int i;

	for (i = 0; i < MEMPOOL_TABLE_SIZE; i++)
		xh2a_memory_allocator_remove_one(handle, mempool_tbl[i].name);

	return 0;
}

/*
 * xh2a_memory_allocator_notifier_call()
 *     - called when the pcie notifies this driver
 * @nb: notifier block
 * @event: event type
 * @data: data from notifier
 * Return:
 *  = NOTIFY_OK - Success
 */
static int xh2a_memory_allocator_notifier_call(struct notifier_block *nb,
					       unsigned long event, void *data)
{
	int ret = 0;

	pr_debug("%s: event = %ld, data = %p\n", __func__, event, data);

	if (event == XH2A_HOST_NOTIFY_PCIE_PROBE)
		ret = xh2a_memory_allocator_probe(data);
	else if (event == XH2A_HOST_NOTIFY_PCIE_REMOVE)
		ret = xh2a_memory_allocator_remove(data);

	if (ret)
		return NOTIFY_STOP;

	return NOTIFY_OK;
}

/*
 * @notifier_call: called when the pcie notifies this driver
 */
static struct notifier_block xh2a_memory_allocator_notifier_block = {
	.notifier_call = xh2a_memory_allocator_notifier_call,
};

/*
 * xh2a_memory_allocator_register_driver()
 *     - register memory_allocator driver
 */
int __init xh2a_memory_allocator_register_driver(void)
{
	pr_debug("%s\n", __func__);
	xh2a_host_register_notifier_chain(
		&xh2a_memory_allocator_notifier_block);
	return 0;
}

/*
 * xh2a_memory_allocator_unregister_driver()
 *     - unregister memory_allocator driver
 */
void __exit xh2a_memory_allocator_unregister_driver(void)
{
	pr_debug("%s\n", __func__);
	xh2a_host_unregister_notifier_chain(
		&xh2a_memory_allocator_notifier_block);
}

MODULE_LICENSE("GPL");