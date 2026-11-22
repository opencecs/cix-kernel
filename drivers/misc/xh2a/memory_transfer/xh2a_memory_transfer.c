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
#include <linux/pm_runtime.h>
#include <linux/suspend.h>
#include <linux/version.h>
#include <linux/iommu.h>
#include <linux/pci.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 20, 0)
#include <linux/pci-p2pdma.h>
#endif
#include <xh2a_host_drv.h>
#include <xh2a_pcie_api.h>
#include <xh2a_efuse.h>
#include <xh2a_chrdev.h>
#include <version_compat.h>
#include "xh2a_memory_transfer.h"

#define ADDRESS_2GB 0x80000000ULL

/*
 * Module-global registry of all xh2a memory transfer devices.
 * Used to resolve a peer's struct pci_dev given its device id.
 * Protected by xh2a_transfer_registry_lock; nodes are added in
 * probe() and removed in remove().
 */
static LIST_HEAD(xh2a_transfer_registry);
static DEFINE_SPINLOCK(xh2a_transfer_registry_lock);

/*
 * xh2a_memory_transfer_lookup_pdev() - look up a peer device's pci_dev by id
 * @devid: device index (minor number / pcie device index)
 *
 * The returned pci_dev has its refcount incremented. Caller must release it
 * with pci_dev_put().
 *
 * Return: pci_dev pointer on success, NULL if no device with @devid found.
 */
static struct pci_dev *xh2a_memory_transfer_lookup_pdev(int devid)
{
	struct xh2a_memory_transfer_dev *td;
	struct pci_dev *pdev = NULL;

	spin_lock(&xh2a_transfer_registry_lock);
	list_for_each_entry(td, &xh2a_transfer_registry, registry_node) {
		if (td->devid != devid)
			continue;
		if (td->parent_dev && dev_is_pci(td->parent_dev))
			pdev = pci_dev_get(to_pci_dev(td->parent_dev));
		break;
	}
	spin_unlock(&xh2a_transfer_registry_lock);
	return pdev;
}

/*
 * xh2a_memory_transfer_iommu_blocks_p2p() - check if IOMMU blocks BAR-based P2P
 * @pdev: pci device whose IOMMU domain is checked
 *
 * BAR-based peer-to-peer DMA writes the destination's BAR address directly.
 * If the source device's IOMMU domain is not in identity (passthrough) mode,
 * the BAR address is not in its IOVA mapping table, and IOMMU will reject the
 * request with a DMAR/AMD-Vi fault.
 *
 * Return: true if IOMMU is active and not in identity mode (P2P will be
 * blocked); false otherwise (no IOMMU, or IOMMU in passthrough/identity mode).
 */
static bool xh2a_memory_transfer_iommu_blocks_p2p(struct pci_dev *pdev)
{
	struct iommu_domain *dom;

	/*
	 * xh2a_iommu_present(): real API on 4.18~6.12; fallback via
	 * dev->iommu_group on 3.10~4.17 and 7.0+.
	 */
	if (!xh2a_iommu_present(&pci_bus_type))
		return false;

	/*
	 * xh2a_iommu_get_domain(): real API on 4.18~7.0; NULL on 3.10~4.17.
	 * Both paths are safe to dereference ->type because we check for NULL
	 * first.
	 */
	dom = xh2a_iommu_get_domain(&pdev->dev);
	if (!dom)
		return false;

	/*
	 * IOMMU_DOMAIN_IDENTITY: passthrough, IOVA == PA, P2P works.
	 * IOMMU_DOMAIN_DMA / IOMMU_DOMAIN_DMA_FQ: strict, P2P blocked.
	 * IOMMU_DOMAIN_UNMANAGED: caller-managed, treat as blocked since
	 * we have no explicit P2P map.
	 *
	 * XH2A_IOMMU_DOMAIN_IDENTITY resolves to IOMMU_DOMAIN_IDENTITY
	 * on 4.18+ and to 0 on 3.10~4.17 (pre-define era); both are safe.
	 */
	return dom->type != XH2A_IOMMU_DOMAIN_IDENTITY;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 20, 0) && \
	IS_ENABLED(CONFIG_PCI_P2PDMA)
/*
 * xh2a_memory_transfer_call_p2pdma_distance() - wrapper around the in-tree API
 * @provider: device that exposes the BAR
 * @client: device that issues the DMA against the provider's BAR
 *
 * Only compiled when both the kernel is >= 4.20 (API present) and
 * CONFIG_PCI_P2PDMA is enabled (otherwise the upstream prototype is a stub
 * that always returns -1; we go straight to the legacy walker instead).
 *
 * Return: PCIe hop distance (>= 0) if P2P routing is possible, -1 otherwise.
 */
static int xh2a_memory_transfer_call_p2pdma_distance(struct pci_dev *provider,
						     struct pci_dev *client)
{
	return pci_p2pdma_distance(provider, &client->dev, 0);
}
#endif

/*
 *  Legacy detect path
 *
 *  Used in two scenarios:
 *    (a) kernel < 4.20: pci_p2pdma_distance() did not exist yet. The API
 *        landed in 4.20 (commit c50bce6cc58a).
 *    (b) kernel >= 4.20 but CONFIG_PCI_P2PDMA is not set: pci-p2pdma.h
 *        provides a stub that always returns -1, so the in-tree path is
 *        useless and we must fall back to our own walker.
 *
 *  The walker uses only APIs available since 3.10:
 *    pci_domain_nr(), pci_pcie_type(), pci_find_ext_capability(),
 *    pci_read_config_word().
 *  The IOMMU side reuses xh2a_memory_transfer_iommu_blocks_p2p().
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 20, 0) || \
	!IS_ENABLED(CONFIG_PCI_P2PDMA)

/*
 * xh2a_memory_transfer_common_upstream() - lowest common upstream bridge
 * @a, @b: two PCI endpoints
 *
 * Return: pointer to the lowest bridge that is an ancestor of both @a and @b,
 * or NULL if they share no common ancestor (different PCI domains, or each
 * plugged into a different root complex).
 */
static struct pci_dev *xh2a_memory_transfer_common_upstream(struct pci_dev *a,
							    struct pci_dev *b)
{
	struct pci_dev *pa, *pb;

	if (pci_domain_nr(a->bus) != pci_domain_nr(b->bus))
		return NULL;

	for (pa = a->bus->self; pa; pa = pa->bus->self) {
		for (pb = b->bus->self; pb; pb = pb->bus->self) {
			if (pa == pb)
				return pa;
		}
	}
	return NULL;
}

/*
 * xh2a_memory_transfer_bridge_acs_blocks() - read ACS Control of @bridge
 *
 * Returns true iff the bridge has any of the P2P-blocking ACS bits set:
 *   PCI_ACS_RR (Request Redirect),
 *   PCI_ACS_CR (Completion Redirect),
 *   PCI_ACS_EC (Egress Control),
 *   PCI_ACS_UF (Upstream Forwarding).
 * A bridge with no ACS Extended Capability cannot enforce upstream
 * redirection and is treated as transparent for P2P.
 */
static bool xh2a_memory_transfer_bridge_acs_blocks(struct pci_dev *bridge)
{
	const u16 mask = PCI_ACS_RR | PCI_ACS_CR | PCI_ACS_EC;
	int pos;
	u16 ctrl = 0;

	pos = pci_find_ext_capability(bridge, PCI_EXT_CAP_ID_ACS);
	if (!pos)
		return false;
	if (pci_read_config_word(bridge, pos + PCI_ACS_CTRL, &ctrl))
		return false;
	return !!(ctrl & mask);
}

/*
 * xh2a_memory_transfer_acs_path_clear() - check ACS along @child .. @common
 *
 * Walks every bridge from @child->bus->self upward up to and including
 * @common. Returns true if every bridge in that segment is ACS-transparent
 * for P2P; false if any bridge would force the TLP upstream.
 */
static bool xh2a_memory_transfer_acs_path_clear(struct pci_dev *child,
						struct pci_dev *common)
{
	struct pci_dev *p;

	for (p = child->bus->self; p; p = p->bus->self) {
		if (xh2a_memory_transfer_bridge_acs_blocks(p))
			return false;
		if (p == common)
			return true;
	}
	/*
	 * Walked off the top of the hierarchy without ever reaching @common:
	 * @common is not actually an ancestor of @child. Caller bug, but be
	 * defensive and report the path as not clear.
	 */
	return false;
}

/*
 * xh2a_memory_transfer_detect_p2p_legacy() - <4.20 fallback detector
 *
 * Equivalent to (but obviously cruder than) pci_p2pdma_distance():
 *   1. Both devices must live in the same PCI domain and share a common
 *      upstream bridge.
 *   2. If the LCA is a Root Port, the only path between src and dst goes
 *      through the Root Complex. RC-internal P2P forwarding is silicon-
 *      dependent and frequently disabled on x86 - treat as UNSUPPORTED,
 *      matching what mainline pci_p2pdma_distance() reports as
 *      MAP_HOST_BRIDGE.
 *   3. The ACS path from src up to LCA and from dst up to LCA must be
 *      clear of Request/Completion Redirect, Egress Control and Upstream
 *      Forwarding.
 *   4. Neither device's IOMMU domain may force DMA remapping.
 */
static uint32_t xh2a_memory_transfer_detect_p2p_legacy(struct pci_dev *src,
						       struct pci_dev *dst)
{
	struct pci_dev *common;

	if (!src || !dst || src == dst)
		return DRIVER_MEMORY_TRANSFER_P2P_UNSUPPORTED_DISTANCE;

	common = xh2a_memory_transfer_common_upstream(src, dst);
	if (!common)
		return DRIVER_MEMORY_TRANSFER_P2P_UNSUPPORTED_DISTANCE;

	if (pci_pcie_type(common) == PCI_EXP_TYPE_ROOT_PORT)
		return DRIVER_MEMORY_TRANSFER_P2P_UNSUPPORTED_DISTANCE;

	if (!xh2a_memory_transfer_acs_path_clear(src, common) ||
	    !xh2a_memory_transfer_acs_path_clear(dst, common))
		return DRIVER_MEMORY_TRANSFER_P2P_UNSUPPORTED_DISTANCE;

	if (xh2a_memory_transfer_iommu_blocks_p2p(src) ||
	    xh2a_memory_transfer_iommu_blocks_p2p(dst))
		return DRIVER_MEMORY_TRANSFER_P2P_UNSUPPORTED_IOMMU;

	return DRIVER_MEMORY_TRANSFER_P2P_SUPPORTED;
}

#endif /* < 4.20 || !CONFIG_PCI_P2PDMA */

/*
 * xh2a_memory_transfer_detect_p2p() - decide P2P capability from OS state
 * @src_pdev: source device (BAR provider for MRd-style P2P)
 * @dst_pdev: destination device
 *
 * Uses kernel mechanisms only (no probing of real DMA paths):
 *  - kernel >= 4.20 AND CONFIG_PCI_P2PDMA=y: pci_p2pdma_distance() for
 *    topology + ACS check, plus a per-device IOMMU domain check.
 *  - otherwise: hand-rolled topology + ACS + IOMMU walker, see
 *    xh2a_memory_transfer_detect_p2p_legacy(). This branch also handles
 *    "kernel >= 4.20 but CONFIG_PCI_P2PDMA is not set" because the in-tree
 *    pci_p2pdma_distance() then is a stub returning -1.
 *
 * Return: DRIVER_MEMORY_TRANSFER_P2P_SUPPORTED or _UNSUPPORTED_*.
 */
static uint32_t xh2a_memory_transfer_detect_p2p(struct pci_dev *src_pdev,
						struct pci_dev *dst_pdev)
{
	if (!src_pdev || !dst_pdev)
		return DRIVER_MEMORY_TRANSFER_P2P_UNSUPPORTED_DISTANCE;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 20, 0) && \
	IS_ENABLED(CONFIG_PCI_P2PDMA)
	{
		int dist = xh2a_memory_transfer_call_p2pdma_distance(src_pdev,
								     dst_pdev);
		if (dist < 0)
			return DRIVER_MEMORY_TRANSFER_P2P_UNSUPPORTED_DISTANCE;

		if (xh2a_memory_transfer_iommu_blocks_p2p(src_pdev) ||
		    xh2a_memory_transfer_iommu_blocks_p2p(dst_pdev))
			return DRIVER_MEMORY_TRANSFER_P2P_UNSUPPORTED_IOMMU;

		return DRIVER_MEMORY_TRANSFER_P2P_SUPPORTED;
	}
#else
	return xh2a_memory_transfer_detect_p2p_legacy(src_pdev, dst_pdev);
#endif
}

static inline uint64_t xh2a_memory_transfer_p2p_status_key(uint32_t src,
							   uint32_t dst)
{
	return ((uint64_t)src << 32) | dst;
}

static uint32_t xh2a_memory_transfer_p2p_status_get(
	struct xh2a_memory_transfer_dev *transfer_dev, uint32_t src_devid,
	uint32_t dst_devid)
{
	uint64_t key =
		xh2a_memory_transfer_p2p_status_key(src_devid, dst_devid);
	struct p2p_status_entry *entry;
	uint32_t status = DRIVER_MEMORY_TRANSFER_P2P_UNKNOWN;

	if (src_devid == dst_devid)
		return DRIVER_MEMORY_TRANSFER_P2P_UNSUPPORTED_DISTANCE;

	rcu_read_lock();
	hash_for_each_possible_rcu(transfer_dev->p2p_status_ht, entry, node,
				   key) {
		if (entry->src_devid == src_devid &&
		    entry->dst_devid == dst_devid) {
			status = entry->status;
			break;
		}
	}
	rcu_read_unlock();

	return status;
}

static void xh2a_memory_transfer_p2p_status_set(
	struct xh2a_memory_transfer_dev *transfer_dev, uint32_t src_devid,
	uint32_t dst_devid, uint32_t status)
{
	uint64_t key =
		xh2a_memory_transfer_p2p_status_key(src_devid, dst_devid);
	struct p2p_status_entry *entry;
	unsigned long flags;

	if (src_devid == dst_devid)
		return;

	spin_lock_irqsave(&transfer_dev->p2p_status_lock, flags);

	hash_for_each_possible(transfer_dev->p2p_status_ht, entry, node, key) {
		if (entry->src_devid == src_devid &&
		    entry->dst_devid == dst_devid) {
			entry->status = status;
			goto out;
		}
	}

	entry = kzalloc(sizeof(*entry), GFP_ATOMIC);
	if (!entry)
		goto out;

	entry->src_devid = src_devid;
	entry->dst_devid = dst_devid;
	entry->status = status;
	hash_add_rcu(transfer_dev->p2p_status_ht, &entry->node, key);

out:
	spin_unlock_irqrestore(&transfer_dev->p2p_status_lock, flags);
}

static void xh2a_memory_transfer_p2p_status_init(
	struct xh2a_memory_transfer_dev *transfer_dev)
{
	hash_init(transfer_dev->p2p_status_ht);
	spin_lock_init(&transfer_dev->p2p_status_lock);
}

static void xh2a_memory_transfer_p2p_status_cleanup(
	struct xh2a_memory_transfer_dev *transfer_dev)
{
	struct p2p_status_entry *entry;
	struct hlist_node *tmp;
	int bkt;

	spin_lock_irq(&transfer_dev->p2p_status_lock);
	hash_for_each_safe(transfer_dev->p2p_status_ht, bkt, tmp, entry, node) {
		hash_del_rcu(&entry->node);
		kfree(entry);
	}
	spin_unlock_irq(&transfer_dev->p2p_status_lock);

	synchronize_rcu();
}

struct address_section_parameter {
	uint64_t start_addr;
	uint64_t total_size;
};

static struct address_section_parameter valid_address_tbl[] = {
	{ XH2A_MEMORY_TRANSFER_DDR_START_ADDR,
	  XH2A_MEMORY_TRANSFER_DDR_TOTAL_SIZE },
	{ XH2A_MEMORY_TRANSFER_SPM0_START_ADDR,
	  XH2A_MEMORY_TRANSFER_SPM0_TOTAL_SIZE },
	{ XH2A_MEMORY_TRANSFER_SPM1_START_ADDR,
	  XH2A_MEMORY_TRANSFER_SPM1_TOTAL_SIZE },
	{ XH2A_MEMORY_TRANSFER_TIM1_START_ADDR,
	  XH2A_MEMORY_TRANSFER_TIM1_TOTAL_SIZE },
};

#define VALID_ADDRESS_TABLE_SIZE \
	((sizeof(valid_address_tbl) / sizeof(struct address_section_parameter)))

static inline bool is_address_valid(uint64_t addr, uint64_t size)
{
	int i;

	if (addr > ((uint64_t)-1) - size)
		return false;

	for (i = 0; i < VALID_ADDRESS_TABLE_SIZE; i++) {
		if (valid_address_tbl[i].start_addr >
		    ((uint64_t)-1) - valid_address_tbl[i].total_size)
			continue;
		if (addr < valid_address_tbl[i].start_addr)
			continue;
		if (size > valid_address_tbl[i].total_size)
			continue;
		if (addr - valid_address_tbl[i].start_addr <=
		    valid_address_tbl[i].total_size - size)
			return true;
	}

	return false;
}

static inline bool is_address_overlapped(uint64_t src, uint64_t dst,
					 uint64_t size)
{
	if (size == 0)
		return false;

	if (src > ((uint64_t)-1) - size)
		return true;

	if (dst > ((uint64_t)-1) - size)
		return true;

	if (src >= dst)
		return (src - dst) < size;

	return (dst - src) < size;
}

static void xh2a_memory_transfer_device_safe_release(struct kref *kref)
{
	struct xh2a_memory_transfer_dev *transfer_dev =
		container_of(kref, struct xh2a_memory_transfer_dev, dev_refcnt);
	kfree(transfer_dev);
}

static int xh2a_memory_transfer_get_drvdata(void *drvdata)
{
	struct xh2a_memory_transfer_dev *transfer_dev = drvdata;

	if (atomic_read(&transfer_dev->dev_initialized) == 0) {
		pr_err("%s: device not initialized\n", __func__);
		return -EAGAIN;
	}

	if (atomic_read(&transfer_dev->dev_removed)) {
		pr_err("%s: device mark removed\n", __func__);
		return -ENODEV;
	}

	if (kref_get_unless_zero(&transfer_dev->dev_refcnt) == 0) {
		pr_err("%s: device is removing\n", __func__);
		return -ENODEV;
	}

	return 0;
}

static int xh2a_memory_transfer_open(struct inode *inode, struct file *filp)
{
	struct xh2a_memory_transfer_dev *transfer_dev = NULL;
	struct xh2a_memory_transfer_file_handle *fh = NULL;
	int ret;

	ret = xh2a_chrdev_get_drvdata(inode, xh2a_memory_transfer_get_drvdata,
				      (void **)&transfer_dev);
	if (ret)
		return ret;

	fh = kzalloc(sizeof(struct xh2a_memory_transfer_file_handle),
		     GFP_KERNEL);

	if (!fh) {
		dev_err(transfer_dev->cdev_dev, "%s: kzalloc failed\n",
			__func__);
		kref_put(&transfer_dev->dev_refcnt,
			 xh2a_memory_transfer_device_safe_release);
		return -ENOMEM;
	}

	fh->transfer = transfer_dev;
	INIT_LIST_HEAD(&fh->node);
	mutex_init(&fh->file_handle_mutex);

	if (atomic_read(&transfer_dev->dev_removed)) {
		kref_put(&transfer_dev->dev_refcnt,
			 xh2a_memory_transfer_device_safe_release);
		kfree(fh);
		pr_err("%s: device mark removed\n", __func__);
		return -ENODEV;
	}

	mutex_lock(&transfer_dev->transfer_mutex);
	list_add_tail(&fh->node, &transfer_dev->file_handle_list);
	mutex_unlock(&transfer_dev->transfer_mutex);

	filp->private_data = fh;

	return 0;
}

static int xh2a_memory_transfer_release(struct inode *inode, struct file *filp)
{
	struct xh2a_memory_transfer_dev *transfer_dev = NULL;
	struct xh2a_memory_transfer_file_handle *fh = NULL;

	fh = filp->private_data;
	filp->private_data = NULL;

	if (!fh)
		return 0;

	transfer_dev = fh->transfer;

	if (!transfer_dev) {
		kfree(fh);
		return 0;
	}

	mutex_lock(&transfer_dev->transfer_mutex);
	if (fh->file_handle_lockmap) {
		xh2a_pcie_membar_unmap(transfer_dev->private_data);
	}

	if (transfer_dev->lock_dev && (transfer_dev->lock_dev_owner == fh)) {
		transfer_dev->lock_dev = 0;
		transfer_dev->lock_dev_owner = NULL;
		wake_up_interruptible(&transfer_dev->lock_dev_wq);
	}

	list_del_init(&fh->node);
	mutex_unlock(&transfer_dev->transfer_mutex);
	kfree(fh);
	kref_put(&transfer_dev->dev_refcnt,
		 xh2a_memory_transfer_device_safe_release);
	return 0;
}

static int xh2a_memory_transfer_ioctl_core_transfer(
	struct xh2a_memory_transfer_dev *transfer_dev, unsigned long arg)
{
	int ret;
	struct xh2a_memory_transfer_request_arg ioc_request;
	uint64_t done_size = 0;
	uint64_t xfer_size = 0;

	ret = copy_from_user(&ioc_request, (void __user *)arg,
			     sizeof(ioc_request));

	if (ret != 0) {
		dev_err(transfer_dev->cdev_dev, "%s: copy_from_user failed\n",
			__func__);
		return -EFAULT;
	}

	if (ioc_request.type == DRIVER_MEMORY_TRANSFER_TYPE_HOST_TO_DEVICE ||
	    ioc_request.type == DRIVER_MEMORY_TRANSFER_TYPE_INNER_DEVICE ||
	    ioc_request.type == DRIVER_MEMORY_TRANSFER_TYPE_PEER_TO_PEER) {
		if (!is_address_valid(ioc_request.dst_addr, ioc_request.size)) {
			dev_err(transfer_dev->cdev_dev,
				"%s: device dst address/size invalid\n",
				__func__);
			return -EINVAL;
		}
	}

	if (ioc_request.type == DRIVER_MEMORY_TRANSFER_TYPE_DEVICE_TO_HOST ||
	    ioc_request.type == DRIVER_MEMORY_TRANSFER_TYPE_INNER_DEVICE ||
	    ioc_request.type == DRIVER_MEMORY_TRANSFER_TYPE_PEER_TO_PEER_ALT) {
		if (!is_address_valid(ioc_request.src_addr, ioc_request.size)) {
			dev_err(transfer_dev->cdev_dev,
				"%s: device src address/size invalid\n",
				__func__);
			return -EINVAL;
		}
	}

	if (ioc_request.type == DRIVER_MEMORY_TRANSFER_TYPE_INNER_DEVICE) {
		if (is_address_overlapped(ioc_request.src_addr,
					  ioc_request.dst_addr,
					  ioc_request.size)) {
			dev_err(transfer_dev->cdev_dev,
				"%s: device src/dst address overlapped\n",
				__func__);
			return -EINVAL;
		}
	}

	while (done_size != ioc_request.size) {
		if (ioc_request.size - done_size < ADDRESS_2GB)
			xfer_size = ioc_request.size - done_size;
		else
			xfer_size = ADDRESS_2GB;

		if (ioc_request.type ==
		    DRIVER_MEMORY_TRANSFER_TYPE_HOST_TO_DEVICE)
			ret = xh2a_pcie_dma_write_mem_userspace(
				transfer_dev->private_data,
				ioc_request.dst_addr + done_size,
				(void __user *)(ioc_request.src_addr +
						done_size),
				xfer_size);

		if (ioc_request.type ==
		    DRIVER_MEMORY_TRANSFER_TYPE_DEVICE_TO_HOST)
			ret = xh2a_pcie_dma_read_mem_userspace(
				transfer_dev->private_data,
				ioc_request.src_addr + done_size,
				(void __user *)(ioc_request.dst_addr +
						done_size),
				xfer_size);

		if (ioc_request.type ==
		    DRIVER_MEMORY_TRANSFER_TYPE_INNER_DEVICE)
			ret = xh2a_memory_transfer_sysdma_memcpy(
				&transfer_dev->sysdma,
				ioc_request.src_addr + done_size,
				ioc_request.dst_addr + done_size, xfer_size);

		if (ioc_request.type ==
		    DRIVER_MEMORY_TRANSFER_TYPE_PEER_TO_PEER)
			ret = xh2a_pcie_dma_mrd_direct(
				transfer_dev->private_data,
				ioc_request.src_addr + done_size,
				ioc_request.dst_addr + done_size, xfer_size);

		if (ioc_request.type ==
		    DRIVER_MEMORY_TRANSFER_TYPE_PEER_TO_PEER_ALT)
			ret = xh2a_pcie_dma_mwr_direct(
				transfer_dev->private_data,
				ioc_request.src_addr + done_size,
				ioc_request.dst_addr + done_size, xfer_size);

		if (ret == -ENODEV) {
			pr_err("%s: device is removed\n", __func__);
			return ret;
		}
		if (ret != 0) {
			dev_err(transfer_dev->cdev_dev,
				"%s: xfer[0x%llx->0x%llx][0x%llx] failed %d\n",
				__func__, ioc_request.src_addr,
				ioc_request.dst_addr, ioc_request.size, ret);
			/*
			 * Do NOT mutate the P2P status cache here. The cache
			 * is populated authoritatively by the OS-mechanism
			 * detection in ioctl_core_p2p_status; a transient DMA
			 * error must not poison it into permanent UNSUPPORTED.
			 */
			return ret;
		}

		done_size += xfer_size;
	};

	return 0;
}

static int xh2a_memory_transfer_ioctl_core_memset(
	struct xh2a_memory_transfer_dev *transfer_dev, unsigned long arg)
{
	int ret;
	struct xh2a_memory_transfer_memset_arg ioc_request;

	ret = copy_from_user(&ioc_request, (void __user *)arg,
			     sizeof(ioc_request));

	if (ret != 0) {
		dev_err(transfer_dev->cdev_dev, "%s: copy_from_user failed\n",
			__func__);
		return -EFAULT;
	}

	if (!is_address_valid(ioc_request.paddr, ioc_request.size)) {
		dev_err(transfer_dev->cdev_dev,
			"%s: device dst address/size invalid\n", __func__);
		return -EINVAL;
	}
	ret = xh2a_memory_transfer_sysdma_memset(&transfer_dev->sysdma,
						 ioc_request.paddr,
						 ioc_request.size,
						 ioc_request.c);

	if (ret == -ENODEV) {
		pr_err("%s: device is removed\n", __func__);
		return ret;
	}

	if (ret != 0) {
		dev_err(transfer_dev->cdev_dev,
			"%s: memset[0x%llx][0x%llx][%c] failed %d\n", __func__,
			ioc_request.paddr, ioc_request.size, ioc_request.c,
			ret);
		return ret;
	}

	return 0;
}

static int xh2a_memory_transfer_ioctl_core_membar(
	struct xh2a_memory_transfer_dev *transfer_dev,
	struct xh2a_memory_transfer_file_handle *transfer_fh, unsigned long arg)
{
	int ret;
	struct xh2a_memory_transfer_membar_arg ioc_request;

	ret = mutex_lock_interruptible(&transfer_fh->file_handle_mutex);

	if (ret != 0) {
		dev_err(transfer_dev->cdev_dev,
			"%s: mutex_lock_interruptible failed\n", __func__);
		return -ERESTARTSYS;
	}

	ret = copy_from_user(&ioc_request, (void __user *)arg,
			     sizeof(ioc_request));

	if (ret != 0) {
		dev_err(transfer_dev->cdev_dev, "%s: copy_from_user failed\n",
			__func__);
		ret = -EFAULT;
		goto out;
	}

	if (ioc_request.type == XH2A_MEMORY_TRANSFER_MEMBAR_GETINFO) {
		ret = xh2a_pcie_membar_getinfo(transfer_dev->private_data,
					       &ioc_request.bar_addr,
					       &ioc_request.bar_size);

		if (ret != 0) {
			dev_err(transfer_dev->cdev_dev,
				"%s: xh2a_pcie_get_membar_info failed\n",
				__func__);
			ret = -EBUSY;
			goto out;
		}

		ret = copy_to_user((void __user *)arg, &ioc_request,
				   sizeof(ioc_request));

		if (ret) {
			dev_err(transfer_dev->cdev_dev,
				"%s: copy_to_user failed\n", __func__);
			ret = -EBUSY;
			goto out;
		}
	} else if (ioc_request.type == XH2A_MEMORY_TRANSFER_MEMBAR_MAP) {
		if (!transfer_dev->lock_dev ||
		    (transfer_dev->lock_dev_owner != transfer_fh)) {
			dev_err(transfer_dev->cdev_dev,
				"%s: device not locked with this file\n",
				__func__);
			ret = -EBUSY;
			goto out;
		}

		if (transfer_fh->file_handle_lockmap) {
			dev_err(transfer_dev->cdev_dev,
				"%s: file handle already locked membar\n",
				__func__);
			ret = -EBUSY;
			goto out;
		}

		if (!is_address_valid(ioc_request.paddr, 4)) {
			dev_err(transfer_dev->cdev_dev,
				"%s: map address invalid\n", __func__);
			ret = -EINVAL;
			goto out;
		}

		/* ioc.type MEMBAR_MAP is required between MEMBAR_LOCK_DEV and
		 * MEMBAR_UNLOCK_DEV. It is safe to map membar without lock */
		ret = xh2a_pcie_membar_map_no_lock(transfer_dev->private_data,
						   ioc_request.paddr);

		if (ret != 0) {
			dev_err(transfer_dev->cdev_dev,
				"%s: xh2a_pcie_membar_map_no_lock failed\n",
				__func__);
			ret = -EBUSY;
			goto out;
		}

		transfer_fh->file_handle_lockmap = 1;
	} else if (ioc_request.type == XH2A_MEMORY_TRANSFER_MEMBAR_UNMAP) {
		if (!transfer_dev->lock_dev ||
		    (transfer_dev->lock_dev_owner != transfer_fh)) {
			dev_err(transfer_dev->cdev_dev,
				"%s: device not locked with this file\n",
				__func__);
			ret = -EBUSY;
			goto out;
		}

		if (!transfer_fh->file_handle_lockmap) {
			dev_err(transfer_dev->cdev_dev,
				"%s: file handle already unlocked membar\n",
				__func__);
			ret = -EBUSY;
			goto out;
		}

		/* ioc.type MEMBAR_UNMAP is required between MEMBAR_LOCK_DEV and
		 * MEMBAR_UNLOCK_DEV. It is safe to map membar without lock */
		ret = xh2a_pcie_membar_unmap_no_lock(
			transfer_dev->private_data);

		if (ret != 0) {
			dev_err(transfer_dev->cdev_dev,
				"%s: xh2a_pcie_membar_unmap_no_lock failed\n",
				__func__);
			ret = -EBUSY;
			goto out;
		}

		transfer_fh->file_handle_lockmap = 0;
	} else if (ioc_request.type == XH2A_MEMORY_TRANSFER_MEMBAR_LOCK_DEV) {
		ret = mutex_lock_interruptible(&transfer_dev->transfer_mutex);

		if (ret != 0) {
			dev_err(transfer_dev->cdev_dev,
				"%s: mutex_lock_interruptible failed\n",
				__func__);
			ret = -ERESTARTSYS;
			goto out;
		}

		while (transfer_dev->lock_dev != 0) {
			mutex_unlock(&transfer_dev->transfer_mutex);

			ret = wait_event_interruptible(
				transfer_dev->lock_dev_wq,
				transfer_dev->lock_dev == 0);

			if (ret != 0) {
				dev_err(transfer_dev->cdev_dev,
					"%s: wait_event_interruptible failed\n",
					__func__);
				ret = -ERESTARTSYS;
				goto out;
			}

			ret = mutex_lock_interruptible(
				&transfer_dev->transfer_mutex);

			if (ret != 0) {
				dev_err(transfer_dev->cdev_dev,
					"%s: mutex_lock_interruptible failed\n",
					__func__);
				ret = -ERESTARTSYS;
				goto out;
			}
		}

		transfer_dev->lock_dev = 1;
		transfer_dev->lock_dev_owner = transfer_fh;
		mutex_unlock(&transfer_dev->transfer_mutex);
	} else if (ioc_request.type == XH2A_MEMORY_TRANSFER_MEMBAR_UNLOCK_DEV) {
		ret = mutex_lock_interruptible(&transfer_dev->transfer_mutex);

		if (ret != 0) {
			dev_err(transfer_dev->cdev_dev,
				"%s: mutex_lock_interruptible failed\n",
				__func__);
			ret = -ERESTARTSYS;
			goto out;
		}

		if (!transfer_dev->lock_dev ||
		    (transfer_dev->lock_dev_owner != transfer_fh)) {
			dev_err(transfer_dev->cdev_dev,
				"%s: device already unlocked or not owner\n",
				__func__);
			mutex_unlock(&transfer_dev->transfer_mutex);
			ret = -EBUSY;
			goto out;
		}
		transfer_dev->lock_dev = 0;
		transfer_dev->lock_dev_owner = NULL;
		wake_up_interruptible(&transfer_dev->lock_dev_wq);
		mutex_unlock(&transfer_dev->transfer_mutex);
	} else {
		dev_err(transfer_dev->cdev_dev, "%s: invalid request type\n",
			__func__);
		ret = -EINVAL;
		goto out;
	}

out:
	mutex_unlock(&transfer_fh->file_handle_mutex);
	return ret;
}

static int xh2a_memory_transfer_ioctl_core_p2p_status(
	struct xh2a_memory_transfer_dev *transfer_dev, unsigned long arg)
{
	int ret = 0;
	struct xh2a_memory_transfer_p2p_status_arg req;
	struct pci_dev *src_pdev = NULL;
	struct pci_dev *dst_pdev = NULL;
	uint32_t status;

	ret = copy_from_user(&req, (void __user *)arg, sizeof(req));
	if (ret != 0) {
		dev_err(transfer_dev->cdev_dev, "%s: copy_from_user failed\n",
			__func__);
		ret = -EFAULT;
		goto out;
	}

	if (req.src_devid == req.dst_devid) {
		req.status = DRIVER_MEMORY_TRANSFER_P2P_UNSUPPORTED_DISTANCE;
		goto write_back;
	}

	/* 1) Try the cache first. */
	status = xh2a_memory_transfer_p2p_status_get(
		transfer_dev, req.src_devid, req.dst_devid);
	if (status != DRIVER_MEMORY_TRANSFER_P2P_UNKNOWN) {
		req.status = status;
		goto write_back;
	}

	/*
	 * 2) Cache miss: resolve both peers' pci_dev and ask the OS
	 *    (pci_p2pdma_distance + IOMMU domain) whether direct P2P is
	 *    actually possible. No real DMA is issued here.
	 */
	src_pdev = xh2a_memory_transfer_lookup_pdev((int)req.src_devid);
	dst_pdev = xh2a_memory_transfer_lookup_pdev((int)req.dst_devid);
	if (!src_pdev || !dst_pdev) {
		dev_warn(transfer_dev->cdev_dev,
			 "%s: peer pci_dev not found (src=%u dst=%u)\n",
			 __func__, req.src_devid, req.dst_devid);
		status = DRIVER_MEMORY_TRANSFER_P2P_UNSUPPORTED_DISTANCE;
	} else {
		status = xh2a_memory_transfer_detect_p2p(src_pdev, dst_pdev);
	}

	/*
	 * 3) Cache the verdict for both directions.
	 *    pci_p2pdma_distance() is symmetric for our use-case (BAR-based
	 *    P2P), so the inverse pair shares the same status.
	 */
	xh2a_memory_transfer_p2p_status_set(transfer_dev, req.src_devid,
					    req.dst_devid, status);
	xh2a_memory_transfer_p2p_status_set(transfer_dev, req.dst_devid,
					    req.src_devid, status);

	req.status = status;

write_back:
	ret = copy_to_user((void __user *)arg, &req, sizeof(req));
	if (ret != 0) {
		dev_err(transfer_dev->cdev_dev, "%s: copy_to_user failed\n",
			__func__);
		ret = -EFAULT;
		goto out;
	}

out:
	if (src_pdev)
		pci_dev_put(src_pdev);
	if (dst_pdev)
		pci_dev_put(dst_pdev);
	return ret;
}

static long xh2a_memory_transfer_ioctl(struct file *filp, unsigned int cmd,
				       unsigned long arg)
{
	int ret = -EINVAL;
	struct xh2a_memory_transfer_dev *transfer_dev = NULL;
	struct xh2a_memory_transfer_file_handle *fh = NULL;

	fh = filp->private_data;

	if (!fh) {
		pr_err("%s: fh is NULL\n", __func__);
		return -EINVAL;
	}

	transfer_dev = fh->transfer;

	if (!transfer_dev) {
		pr_err("%s: transfer is NULL\n", __func__);
		return -EINVAL;
	}

	if (atomic_read(&transfer_dev->dev_removed)) {
		pr_err("%s: device mark removed\n", __func__);
		return -ENODEV;
	}

	ret = xh2a_pcie_pm_io_status(transfer_dev->private_data);
	if (ret)
		return ret;
	if (atomic_read(&transfer_dev->block_ioctl_flag)) {
		ret = -EAGAIN;
		return ret;
	}

	ret = pm_runtime_resume_and_get(transfer_dev->parent_dev);
	if (ret < 0)
		return ret;
	ret = xh2a_pcie_pm_io_status(transfer_dev->private_data);
	if (ret) {
		pm_runtime_put_noidle(transfer_dev->parent_dev);
		return ret;
	}

	switch (cmd) {
	case IOCTL_XH2A_MEMORY_TRANSFER_COPY_BUFFER:
		ret = xh2a_memory_transfer_ioctl_core_transfer(transfer_dev,
							       arg);
		break;

	case IOCTL_XH2A_MEMORY_TRANSFER_MEMBAR:
		ret = xh2a_memory_transfer_ioctl_core_membar(transfer_dev, fh,
							     arg);
		break;

	case IOCTL_XH2A_MEMORY_TRANSFER_MEMSET:
		ret = xh2a_memory_transfer_ioctl_core_memset(transfer_dev, arg);
		break;

	case IOCTL_XH2A_MEMORY_TRANSFER_P2P_STATUS: {
		ret = xh2a_memory_transfer_ioctl_core_p2p_status(transfer_dev,
								 arg);
		break;
	}

	default:
		ret = -EINVAL;
		dev_err(transfer_dev->cdev_dev, "%s: unknown cmd 0x%x\n",
			__func__, cmd);
		break;
	}

	if ((ret != 0) &&
	    xh2a_pcie_pm_io_status(transfer_dev->private_data) == -EAGAIN) {
		dev_err(transfer_dev->cdev_dev,
			"%s: ioctl failed in sleep process. need retry\n",
			__func__);
		/* s2idle keeps devices' DDR, retry blocked ioctl; otherwise
		 * drop it. */
		if (pm_suspend_target_state == PM_SUSPEND_TO_IDLE)
			ret = -EAGAIN;
		else
			ret = -EINTR;
	}
	pm_runtime_mark_last_busy(transfer_dev->parent_dev);
	pm_runtime_put_sync(transfer_dev->parent_dev);
	return ret;
}

static const struct file_operations xh2a_memory_transfer_fops = {
	.owner = THIS_MODULE,
	.open = xh2a_memory_transfer_open,
	.release = xh2a_memory_transfer_release,
	.unlocked_ioctl = xh2a_memory_transfer_ioctl,
};

/*
 * xh2a_memory_transfer_pm_notifier_prepare()
 *     - before pm prepare the memory_transfer device
 * @handle: pcie handle
 * Return: 0 on success, negative error code on failure
 */
static int xh2a_memory_transfer_pm_notifier_prepare(void *handle, bool rollback)
{
	struct xh2a_pcie_client *client = NULL;
	struct xh2a_memory_transfer_dev *transfer_dev;

	xh2a_pcie_get_client(handle, &client, XH2A_MEMORY_TRANSFER_DEVICE_NAME);

	if (client == NULL) {
		pr_debug("%s: client not found\n", __func__);
		return 0;
	}

	transfer_dev = client->client_data;

	if (rollback) {
		xh2a_memory_transfer_sysdma_resume(&transfer_dev->sysdma);
		if (atomic_dec_and_test(&transfer_dev->block_ioctl_flag))
			wake_up_interruptible(&transfer_dev->block_ioctl_wq);
	} else {
		atomic_inc(&transfer_dev->block_ioctl_flag);
		return xh2a_memory_transfer_sysdma_stop(&transfer_dev->sysdma);
	}

	return 0;
}

/*
 * xh2a_memory_transfer_pm_notifier_complete()
 *     - after pm complete the memory_transfer device
 * @handle: pcie handle
 * Return: 0 on success, negative error code on failure
 */
static int xh2a_memory_transfer_pm_notifier_complete(void *handle)
{
	struct xh2a_pcie_client *client = NULL;
	struct xh2a_memory_transfer_dev *transfer_dev;

	xh2a_pcie_get_client(handle, &client, XH2A_MEMORY_TRANSFER_DEVICE_NAME);

	if (client == NULL) {
		pr_debug("%s: client not found\n", __func__);
		return 0;
	}

	transfer_dev = client->client_data;

	xh2a_memory_transfer_sysdma_resume(&transfer_dev->sysdma);
	if (atomic_dec_and_test(&transfer_dev->block_ioctl_flag))
		wake_up_interruptible(&transfer_dev->block_ioctl_wq);

	return 0;
}

/*
 * xh2a_memory_transfer_pm_prepare()
 *     - pm prepare the memory_transfer device
 * @handle: pcie handle
 * Return: 0 on success, negative error code on failure
 */
static int xh2a_memory_transfer_pm_prepare(void *handle, bool is_compatible)
{
	struct xh2a_pcie_client *client = NULL;
	struct xh2a_memory_transfer_dev *transfer_dev;

	(void)is_compatible;
	xh2a_pcie_get_client(handle, &client, XH2A_MEMORY_TRANSFER_DEVICE_NAME);

	if (client == NULL) {
		pr_debug("%s: client not found\n", __func__);
		return 0;
	}

	transfer_dev = client->client_data;

	return xh2a_memory_transfer_sysdma_pm_prepare(&transfer_dev->sysdma);
}

/*
 * xh2a_memory_transfer_pm_complete()
 *     - pm complete the memory_transfer device
 * @handle: pcie handle
 * Return: 0 on success, negative error code on failure
 */
static int xh2a_memory_transfer_pm_complete(void *handle, bool is_compatible)
{
	struct xh2a_pcie_client *client = NULL;
	struct xh2a_memory_transfer_dev *transfer_dev;

	(void)is_compatible;
	xh2a_pcie_get_client(handle, &client, XH2A_MEMORY_TRANSFER_DEVICE_NAME);

	if (client == NULL) {
		pr_debug("%s: client not found\n", __func__);
		return 0;
	}

	transfer_dev = client->client_data;

	/* The notifier commits user admission after every client recovered. */
	return xh2a_memory_transfer_sysdma_pm_complete(&transfer_dev->sysdma);
}

/*
 * xh2a_memory_transfer_probe()
 *     - probe function for memory_transfer device
 * @handle: handle of xh2a_pcie_dev
 * Return: 0 on success, negative error code on failure
 */
static int xh2a_memory_transfer_probe(void *handle)
{
	int ret;
	struct xh2a_pcie_client *client = NULL;
	struct xh2a_memory_transfer_dev *transfer_dev = NULL;
	uint64_t size;

	pr_debug("%s\n", __func__);

	transfer_dev =
		kzalloc(sizeof(struct xh2a_memory_transfer_dev), GFP_KERNEL);

	if (transfer_dev == NULL) {
		pr_err("%s: alloc transfer_dev failed\n", __func__);
		return -ENOMEM;
	}

	client = &transfer_dev->client;

	transfer_dev->private_data = handle;

	mutex_init(&transfer_dev->transfer_mutex);

	transfer_dev->lock_dev = 0;
	transfer_dev->lock_dev_owner = NULL;
	init_waitqueue_head(&transfer_dev->lock_dev_wq);

	INIT_LIST_HEAD(&transfer_dev->file_handle_list);

	atomic_set(&transfer_dev->dev_initialized, 0);
	atomic_set(&transfer_dev->dev_removed, 0);
	kref_init(&transfer_dev->dev_refcnt);

	atomic_set(&transfer_dev->block_ioctl_flag, 0);
	init_waitqueue_head(&transfer_dev->block_ioctl_wq);

	memset(transfer_dev->name_buf, 0, XH2A_MEMORY_TRANSFER_DEVICE_NAME_LEN);
	snprintf(transfer_dev->name_buf, XH2A_MEMORY_TRANSFER_DEVICE_NAME_LEN,
		 XH2A_MEMORY_TRANSFER_DEVICE_NAME "%d",
		 xh2a_pcie_device_index(handle));

	transfer_dev->devid = xh2a_pcie_device_index(handle);
	INIT_LIST_HEAD(&transfer_dev->registry_node);

	transfer_dev->parent_dev = xh2a_pcie_device_ptr(handle);
	transfer_dev->cdev_dev = xh2a_chrdev_create(
		xh2a_memory_transfer_class, transfer_dev->parent_dev,
		xh2a_chrdev_devt(handle, XH2A_CHRDEV_SUB_MEMORY_TRANSFER),
		transfer_dev, &xh2a_memory_transfer_fops, NULL, "%s%d",
		XH2A_MEMORY_TRANSFER_DEVICE_NAME,
		xh2a_pcie_device_index(handle));
	if (IS_ERR(transfer_dev->cdev_dev)) {
		ret = PTR_ERR(transfer_dev->cdev_dev);
		pr_err("%s: xh2a_chrdev_create failed\n", __func__);
		goto err_chrdev_create;
	}

	xh2a_memory_transfer_p2p_status_init(transfer_dev);

	ret = xh2a_memory_transfer_sysdma_init(&transfer_dev->sysdma,
					       transfer_dev->cdev_dev,
					       transfer_dev->private_data);

	if (ret != 0) {
		dev_err(transfer_dev->cdev_dev, "%s: sysdma init failed\n",
			__func__);
		goto err_sysdma_init;
	}

	snprintf(client->name, XH2A_PCIE_CLIENT_NAME_LEN,
		 XH2A_MEMORY_TRANSFER_DEVICE_NAME);

	client->work[0].msi_id = XH2A_PCIE_MSI_ID_CPUSS;
	client->work[0].msi_work = &transfer_dev->sysdma.sysdma_work;
	client->work_num = 1;

	client->client_data = transfer_dev;
	client->private_data = handle;

	client->prepare_cb = xh2a_memory_transfer_pm_prepare;
	client->complete_cb = xh2a_memory_transfer_pm_complete;
	client->notifier_prepare_cb = xh2a_memory_transfer_pm_notifier_prepare;
	client->notifier_complete_cb =
		xh2a_memory_transfer_pm_notifier_complete;

	ret = xh2a_pcie_register_client(handle, client);

	if (ret != 0) {
		pr_err("%s: register client failed %d\n", __func__, ret);
		goto err_sysdma_init;
	}

	ret = xh2a_pcie_efuse_update_ddr_size(handle, &size);
	if ((ret == 0) && (size != 0)) {
		size -= XH2A_DEVICE_SYSTEM_SIZE;
		valid_address_tbl[0].total_size = size;
	}

	/*
	 * Publish to the cross-device registry only after everything else is
	 * ready, so a peer P2P-status query can safely resolve our pci_dev.
	 */
	spin_lock(&xh2a_transfer_registry_lock);
	list_add_tail(&transfer_dev->registry_node, &xh2a_transfer_registry);
	spin_unlock(&xh2a_transfer_registry_lock);

	atomic_set(&transfer_dev->dev_initialized, 1);

	return 0;

err_sysdma_init:
	xh2a_memory_transfer_sysdma_deinit(&transfer_dev->sysdma);

err_chrdev_create:
	xh2a_chrdev_destroy(xh2a_memory_transfer_class,
			    xh2a_chrdev_devt(handle,
					     XH2A_CHRDEV_SUB_MEMORY_TRANSFER),
			    transfer_dev);

	kfree(transfer_dev);
	return ret;
}

/*
 * xh2a_memory_transfer_remove()
 *     - remove the memory_transfer device
 * @handle: pcie handle
 * Return: 0 on success, negative error code on failure
 */
static int xh2a_memory_transfer_remove(void *handle)
{
	struct xh2a_pcie_client *client = NULL;
	struct xh2a_memory_transfer_dev *transfer_dev;
	struct xh2a_memory_transfer_file_handle *fh_pos, *fh_n;

	pr_debug("%s\n", __func__);

	xh2a_pcie_get_client(handle, &client, XH2A_MEMORY_TRANSFER_DEVICE_NAME);

	if (client == NULL) {
		pr_debug("%s: client not found\n", __func__);
		return 0;
	}

	transfer_dev = client->client_data;

	atomic_set(&transfer_dev->dev_removed, 1);

	/*
	 * Remove from the cross-device registry first so concurrent peer
	 * P2P-status queries cannot pick up a device that is going away.
	 */
	spin_lock(&xh2a_transfer_registry_lock);
	if (!list_empty(&transfer_dev->registry_node))
		list_del_init(&transfer_dev->registry_node);
	spin_unlock(&xh2a_transfer_registry_lock);

	/*
	 * Destroy the chrdev (no new opens after dev_removed=1) and
	 * unregister from PCIe before tearing down internal state. This
	 * prevents in-flight ioctls from touching sysdma/p2p_status/etc.
	 * after they've been freed; the kref keeps transfer_dev itself
	 * alive for any still-open fd until its release().
	 */
	/* sysdma and its drained work still use this device for diagnostics. */
	get_device(transfer_dev->cdev_dev);
	xh2a_chrdev_destroy(xh2a_memory_transfer_class,
			    xh2a_chrdev_devt(handle,
					     XH2A_CHRDEV_SUB_MEMORY_TRANSFER),
			    transfer_dev);

	xh2a_pcie_unregister_client(handle, client);
	/* Unregister prevents future IRQ lookups; drain any handler which has
	 * already copied sysdma_work before cancel_work_sync() below.
	 */
	xh2a_pcie_synchronize_msi(handle, client->work[0].msi_id);

	xh2a_memory_transfer_p2p_status_cleanup(transfer_dev);

	xh2a_memory_transfer_sysdma_deinit(&transfer_dev->sysdma);

	mutex_lock(&transfer_dev->transfer_mutex);
	list_for_each_entry_safe(fh_pos, fh_n, &transfer_dev->file_handle_list,
				 node) {
		list_del_init(&fh_pos->node);
	}
	mutex_unlock(&transfer_dev->transfer_mutex);

	put_device(transfer_dev->cdev_dev);
	kref_put(&transfer_dev->dev_refcnt,
		 xh2a_memory_transfer_device_safe_release);
	return 0;
}

/*
 * xh2a_memory_transfer_notifier_call()
 *     - called when the pcie notifies this driver
 * @nb: notifier block
 * @event: event type
 * @data: data from notifier
 * Return:
 *  = NOTIFY_OK - Success
 */
static int xh2a_memory_transfer_notifier_call(struct notifier_block *nb,
					      unsigned long event, void *data)
{
	int ret = 0;

	pr_debug("%s: event = %ld, data = %p\n", __func__, event, data);

	if (event == XH2A_HOST_NOTIFY_PCIE_PROBE)
		ret = xh2a_memory_transfer_probe(data);
	else if (event == XH2A_HOST_NOTIFY_PCIE_REMOVE)
		ret = xh2a_memory_transfer_remove(data);

	if (ret)
		return NOTIFY_STOP;

	return NOTIFY_OK;
}

/*
 * @notifier_call: called when the pcie notifies this driver
 */
static struct notifier_block xh2a_memory_transfer_notifier_block = {
	.notifier_call = xh2a_memory_transfer_notifier_call,
};

/*
 * xh2a_memory_transfer_register_driver()
 *     - register memory_transfer driver
 */
int __init xh2a_memory_transfer_register_driver(void)
{
	pr_debug("%s\n", __func__);
	xh2a_host_register_notifier_chain(&xh2a_memory_transfer_notifier_block);
	return 0;
}

/*
 * xh2a_memory_transfer_unregister_driver()
 *     - unregister memory_transfer driver
 */
void __exit xh2a_memory_transfer_unregister_driver(void)
{
	pr_debug("%s\n", __func__);
	xh2a_host_unregister_notifier_chain(
		&xh2a_memory_transfer_notifier_block);
}

MODULE_LICENSE("GPL");
