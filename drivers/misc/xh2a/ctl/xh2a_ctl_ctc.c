// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2026 Houmo AI Inc.
 * Author: mingdong.wang<mingdong.wang@houmo.ai>
 */

#include <linux/fs.h>
#include <linux/lockdep.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/pm_runtime.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/sort.h>

#include <xh2a_ctl_internal.h>
#include <xh2a_pcie_api.h>
#include "xh2a_ctl.h"
#include "xh2a_ctl_dev.h"
#include "xh2a_ipu_device.h"
#include "xh2a_ipu_ctc.h"

/* CTC runtime ownership moved from SYS to the IPU PCIe client. */
#define xh2a_sys_dev xh2a_ipu_device
#define sys_dev ipu_dev
#define sys_mutex ctc_mutex
#define CTC_UNINIT_VAL XH2A_CTC_UNINIT_VAL
#define xh2a_sys_ctc_reinit_inhibit_get xh2a_ipu_ctc_reinit_inhibit_get
#define xh2a_sys_ctc_reinit_inhibit_put xh2a_ipu_ctc_reinit_inhibit_put
#define xh2a_sys_ctc_phy_prepare xh2a_ipu_ctc_phy_prepare
#define xh2a_sys_ctc_phy_wait_linkup xh2a_ipu_ctc_phy_wait_linkup
#define xh2a_ctc_shutdown_single xh2a_ipu_ctc_shutdown_single

#define LANE_NUM	 2
#define CTC_INIT_TIMEOUT 100

#define CTCSS0_CFG_BASE		0x54000000UL
#define CTC_MAC_PHY_MAC0_OFFSET 0x40000UL
#define CTC_CTRL0_BLOCK		0x200000UL

#define COMMON_STATUS0_OFFSET 0x10
#define COMMON_STATUS1_OFFSET 0x14

#define AOSS_SYSCTRL_CTC_CHIPID_OFFSET	0x70000104

#define CTCSS_LEFT_TX_ADDR  (CTCSS0_CFG_BASE + CTC_CTRL0_BLOCK)
#define CTCSS_RIGHT_TX_ADDR (CTCSS0_CFG_BASE + 2 * 0x800000UL + CTC_CTRL0_BLOCK)
#define CTCSS_LEFT_RX_ADDR  (CTCSS0_CFG_BASE + CTC_CTRL0_BLOCK + 0x4)
#define CTCSS_RIGHT_RX_ADDR \
	(CTCSS0_CFG_BASE + 2 * 0x800000UL + CTC_CTRL0_BLOCK + 0x4)

struct xh2a_ctl_ctc_group {
	int id;
	int size;
	uint32_t dev_id[XH2A_CTL_CTC_CHIP_MAX];
};

struct xh2a_ctl_ctc_dev {
	struct xh2a_ctl_dev_ctx *ctl_ctx;
	struct xh2a_sys_dev *sys_dev;
	uint32_t dev_id;
	int left_dev;
	int right_dev;
	int chip_id;
	int group_id;
	int is_single;
	int visited;
	bool pm_was_suspended;
};

struct xh2a_ctl_ctc_runtime {
	int chip_num;
	int group_num;
	struct xh2a_ctl_ctc_group group[XH2A_CTL_CTC_GROUP_MAX];
	struct xh2a_ctl_ctc_dev dev[XH2A_CTL_CTC_MAX_DEVICES];
};

static int __xh2a_ctl_ctc_build_topology_locked(struct xh2a_ctl_ctx *ctl);

static void __xh2a_ctl_ctc_group_list_clear(struct xh2a_ctl_ctx *ctl)
{
	struct xh2a_ctl_ctc_group_entry *entry;
	struct xh2a_ctl_ctc_group_entry *tmp;

	list_for_each_entry_safe(entry, tmp, &ctl->ctc_group_list, node) {
		list_del(&entry->node);
		kfree(entry);
	}
}

static void xh2a_ctl_ctc_group_list_free(struct list_head *head)
{
	struct xh2a_ctl_ctc_group_entry *entry;
	struct xh2a_ctl_ctc_group_entry *tmp;

	list_for_each_entry_safe(entry, tmp, head, node) {
		list_del(&entry->node);
		kfree(entry);
	}
}

void xh2a_ctl_ctc_group_list_clear(struct xh2a_ctl_ctx *ctl)
{
	if (!ctl)
		return;

	mutex_lock(&ctl->lock);
	__xh2a_ctl_ctc_group_list_clear(ctl);
	mutex_unlock(&ctl->lock);
}

static int xh2a_ctl_ctc_group_list_prepare(struct xh2a_ctl_ctc_runtime *rt,
					   struct list_head *new_list)
{
	struct xh2a_ctl_ctc_group_entry *entry;
	int group_idx;
	int chip_idx;

	INIT_LIST_HEAD(new_list);

	for (group_idx = 0; group_idx < rt->group_num; group_idx++) {
		entry = kzalloc(sizeof(*entry), GFP_KERNEL);
		if (!entry) {
			xh2a_ctl_ctc_group_list_free(new_list);
			return -ENOMEM;
		}

		entry->group_id = rt->group[group_idx].id;
		entry->chip_num = rt->group[group_idx].size;
		for (chip_idx = 0; chip_idx < entry->chip_num; chip_idx++)
			entry->live_dev_id[chip_idx] =
				rt->group[group_idx].dev_id[chip_idx];
		list_add_tail(&entry->node, new_list);
	}

	return 0;
}

static void xh2a_ctl_ctc_group_list_commit(struct xh2a_ctl_ctx *ctl,
					   struct list_head *new_list)
{
	LIST_HEAD(old_list);

	mutex_lock(&ctl->lock);
	list_splice_init(&ctl->ctc_group_list, &old_list);
	list_splice_init(new_list, &ctl->ctc_group_list);
	ctl->ctc_topology_dirty = false;
	mutex_unlock(&ctl->lock);

	xh2a_ctl_ctc_group_list_free(&old_list);
}

static int xh2a_ctl_ctc_cmp_dev(const void *a, const void *b)
{
	const struct xh2a_ctl_ctc_dev *da = a;
	const struct xh2a_ctl_ctc_dev *db = b;

	if (da->dev_id < db->dev_id)
		return -1;
	if (da->dev_id > db->dev_id)
		return 1;
	return 0;
}

static int xh2a_ctl_ctc_readl(void *handle, uint64_t addr, uint32_t *val)
{
	return xh2a_pcie_pio_readl(handle, addr, val);
}

static int xh2a_ctl_ctc_writel(void *handle, uint64_t addr, uint32_t val)
{
	return xh2a_pcie_pio_writel(handle, addr, val);
}

static void xh2a_ctl_ctc_write_sysctrl_chip_id(
	struct xh2a_ctl_dev_ctx *ctl_ctx, int chip_id)
{
	if (!ctl_ctx || !ctl_ctx->pcie_dev)
		return;

	xh2a_ctl_ctc_writel(ctl_ctx->pcie_dev,
			    AOSS_SYSCTRL_CTC_CHIPID_OFFSET, chip_id);
}

static int xh2a_ctl_ctc_find_ctx_locked(struct xh2a_ctl_ctx *ctl,
					uint32_t dev_id,
					struct xh2a_ctl_dev_ctx **out)
{
	struct xh2a_ctl_dev_ctx *ctl_ctx;

	list_for_each_entry(ctl_ctx, &ctl->ctl_list, node) {
		if (ctl_ctx->dev_id != dev_id)
			continue;
		*out = ctl_ctx;
		return 0;
	}

	return -ENODEV;
}

static int xh2a_ctl_get_ipu_dev(struct xh2a_ctl_dev_ctx *ctl_ctx,
				struct xh2a_ipu_device **ipu_dev)
{
	struct xh2a_pcie_client *client = NULL;
	int ret;

	if (!ctl_ctx || !ctl_ctx->pcie_dev || !ipu_dev)
		return -EINVAL;

	ret = xh2a_pcie_get_client(ctl_ctx->pcie_dev, &client,
				   XH2A_IPU_DEVICE_NAME);
	if (ret || !client || !client->client_data)
		return -ENODEV;

	*ipu_dev = client->client_data;
	return 0;
}

static int xh2a_ctl_ctc_is_uninit(const struct xh2a_sys_dev *sys_dev)
{
	return sys_dev->ctc_chip_id == CTC_UNINIT_VAL;
}

static int xh2a_ctl_ctc_collect_devs(struct xh2a_ctl_ctx *ctl,
				     struct xh2a_ctl_ctc_runtime *rt)
{
	struct xh2a_ctl_dev_ctx *ctl_ctx;
	int n = 0;

	memset(rt, 0, sizeof(*rt));

	mutex_lock(&ctl->lock);
	list_for_each_entry(ctl_ctx, &ctl->ctl_list, node) {
		struct xh2a_sys_dev *sys_dev = NULL;
		bool ipu_ready;
		bool online;
		bool removing;
		int ret;

		if (n >= XH2A_CTL_CTC_MAX_DEVICES) {
			pr_err("%s: too many CTC devices, max=%u\n", __func__,
			       XH2A_CTL_CTC_MAX_DEVICES);
			mutex_unlock(&ctl->lock);
			return -E2BIG;
		}

		mutex_lock(&ctl_ctx->lock);
		ipu_ready = ctl_ctx->ipu_ready;
		online = ctl_ctx->online;
		removing = ctl_ctx->removing;
		mutex_unlock(&ctl_ctx->lock);

		if (removing || !online) {
			mutex_unlock(&ctl->lock);
			return -ENODEV;
		}
		if (!ipu_ready) {
			mutex_unlock(&ctl->lock);
			return -EAGAIN;
		}

		ret = xh2a_ctl_get_ipu_dev(ctl_ctx, &sys_dev);
		if (ret) {
			mutex_unlock(&ctl->lock);
			return -EAGAIN;
		}

		rt->dev[n].ctl_ctx = ctl_ctx;
		rt->dev[n].sys_dev = sys_dev;
		rt->dev[n].dev_id = ctl_ctx->dev_id;
		rt->dev[n].chip_id = -1;
		rt->dev[n].group_id = -1;
		rt->dev[n].left_dev = -1;
		rt->dev[n].right_dev = -1;
		n++;
	}
	mutex_unlock(&ctl->lock);

	rt->chip_num = n;
	sort(rt->dev, rt->chip_num, sizeof(rt->dev[0]), xh2a_ctl_ctc_cmp_dev,
	     NULL);

	return rt->chip_num;
}

static int xh2a_ctl_ctc_runtime_pm_get(struct xh2a_ctl_ctc_runtime *rt)
{
	int i;
	int ret;
	struct device *dev;

	if (!rt)
		return -EINVAL;

	for (i = 0; i < rt->chip_num; i++) {
		if (!rt->dev[i].sys_dev || !rt->dev[i].sys_dev->parent_dev) {
			ret = -ENODEV;
			goto err_put;
		}

		dev = rt->dev[i].sys_dev->parent_dev;
		rt->dev[i].pm_was_suspended = pm_runtime_suspended(dev);
		ret = pm_runtime_get_sync(dev);
		if (ret < 0) {
			pm_runtime_put_noidle(dev);
			goto err_put;
		}
	}

	return 0;

err_put:
	while (--i >= 0) {
		struct device *dev = rt->dev[i].sys_dev->parent_dev;

		pm_runtime_mark_last_busy(dev);
		pm_runtime_put_sync(dev);
	}

	return ret;
}

static void xh2a_ctl_ctc_runtime_pm_put(struct xh2a_ctl_ctc_runtime *rt)
{
	int i;

	if (!rt)
		return;

	for (i = rt->chip_num - 1; i >= 0; i--) {
		struct device *dev;

		if (!rt->dev[i].sys_dev || !rt->dev[i].sys_dev->parent_dev)
			continue;

		dev = rt->dev[i].sys_dev->parent_dev;
		pm_runtime_mark_last_busy(dev);
		pm_runtime_put_sync(dev);
	}
}

static void xh2a_ctl_ctc_reinit_inhibit(struct xh2a_ctl_ctc_runtime *rt)
{
	int i;

	for (i = 0; i < rt->chip_num; i++)
		xh2a_sys_ctc_reinit_inhibit_get(rt->dev[i].sys_dev);
}

static void xh2a_ctl_ctc_reinit_allow(struct xh2a_ctl_ctc_runtime *rt)
{
	int i;

	for (i = rt->chip_num - 1; i >= 0; i--)
		xh2a_sys_ctc_reinit_inhibit_put(rt->dev[i].sys_dev);
}

static bool xh2a_ctl_ctc_needs_phy_prepare(struct xh2a_ctl_ctc_dev *dev)
{
	return xh2a_ctl_ctc_is_uninit(dev->sys_dev) ||
	       dev->sys_dev->ctc_is_single == 1 || dev->pm_was_suspended ||
	       dev->sys_dev->ctc_needs_phy_prepare;
}

static bool xh2a_ctl_ctc_has_prepare_dev(struct xh2a_ctl_ctc_runtime *rt)
{
	int i;

	for (i = 0; i < rt->chip_num; i++) {
		if (xh2a_ctl_ctc_needs_phy_prepare(&rt->dev[i]))
			return true;
	}

	return false;
}

static int xh2a_ctl_ctc_prepare_probe_phy(struct xh2a_ctl_ctc_runtime *rt)
{
	int i;
	int ret;

	for (i = 0; i < rt->chip_num; i++) {
		if (!rt->dev[i].sys_dev)
			return -ENODEV;

		mutex_lock(&rt->dev[i].sys_dev->sys_mutex);
		if (xh2a_ctl_ctc_needs_phy_prepare(&rt->dev[i])) {
			pr_info("%s: prepare dev_id=%u chip_id=%d group_id=%d "
				"is_single=%d pm_was_suspended=%d "
				"needs_phy_prepare=%d fail_info=0x%x\n",
				__func__, rt->dev[i].dev_id,
				rt->dev[i].sys_dev->ctc_chip_id,
				rt->dev[i].sys_dev->ctc_group_id,
				rt->dev[i].sys_dev->ctc_is_single,
				rt->dev[i].pm_was_suspended,
				rt->dev[i].sys_dev->ctc_needs_phy_prepare,
				rt->dev[i].sys_dev->ctc_fail_info);
			ret = xh2a_sys_ctc_phy_prepare(
				rt->dev[i].sys_dev,
				XH2A_IPU_CTC_CRG_RESET_PHY_ONLY);
		} else {
			ret = 0;
		}
		mutex_unlock(&rt->dev[i].sys_dev->sys_mutex);
		if (ret)
			return ret;
	}

	for (i = 0; i < rt->chip_num; i++) {
		if (!xh2a_ctl_ctc_needs_phy_prepare(&rt->dev[i]))
			continue;

		mutex_lock(&rt->dev[i].sys_dev->sys_mutex);
		pr_info("%s: wait linkup dev_id=%u chip_id=%d group_id=%d "
			"is_single=%d needs_phy_prepare=%d fail_info=0x%x\n",
			__func__, rt->dev[i].dev_id,
			rt->dev[i].sys_dev->ctc_chip_id,
			rt->dev[i].sys_dev->ctc_group_id,
			rt->dev[i].sys_dev->ctc_is_single,
			rt->dev[i].sys_dev->ctc_needs_phy_prepare,
			rt->dev[i].sys_dev->ctc_fail_info);
		ret = xh2a_sys_ctc_phy_wait_linkup(rt->dev[i].sys_dev);
		mutex_unlock(&rt->dev[i].sys_dev->sys_mutex);
		if (ret)
			return ret;
	}

	return 0;
}

static int xh2a_ctl_ctc_discover_neighbors(struct xh2a_ctl_ctc_runtime *rt,
					   int idx, int *left_idx,
					   int *right_idx)
{
	uint32_t pattern = 0x10000 + 0xAB00 + rt->dev[idx].dev_id;
	uint32_t val;
	int i;
	void *handle = rt->dev[idx].ctl_ctx->pcie_dev;
	int ret;

	*left_idx = -1;
	*right_idx = -1;

	pr_debug("%s: probe dev_id=%u pattern=0x%08x left_tx=0x%llx "
		"right_tx=0x%llx left_rx=0x%llx right_rx=0x%llx\n",
		__func__, rt->dev[idx].dev_id, pattern,
		(unsigned long long)CTCSS_LEFT_TX_ADDR,
		(unsigned long long)CTCSS_RIGHT_TX_ADDR,
		(unsigned long long)CTCSS_LEFT_RX_ADDR,
		(unsigned long long)CTCSS_RIGHT_RX_ADDR);

	ret = xh2a_ctl_ctc_writel(handle, CTCSS_LEFT_TX_ADDR, pattern);
	if (ret)
		return ret;

	for (i = 0; i < rt->chip_num; i++) {
		if (i == idx)
			continue;
		ret = xh2a_ctl_ctc_readl(rt->dev[i].ctl_ctx->pcie_dev,
					 CTCSS_RIGHT_RX_ADDR, &val);
		if (ret)
			return ret;
		pr_debug("%s: dev_id=%u left_tx peer_dev_id=%u "
			"right_rx_val=0x%08x match=%d\n",
			__func__, rt->dev[idx].dev_id, rt->dev[i].dev_id,
			val, (val & 0xFFFF) == (pattern & 0xFFFF));
		if ((val & 0xFFFF) == (pattern & 0xFFFF)) {
			*left_idx = i;
			break;
		}
	}

	ret = xh2a_ctl_ctc_writel(handle, CTCSS_RIGHT_TX_ADDR, pattern);
	if (ret)
		return ret;

	for (i = 0; i < rt->chip_num; i++) {
		if (i == idx)
			continue;
		ret = xh2a_ctl_ctc_readl(rt->dev[i].ctl_ctx->pcie_dev,
					 CTCSS_LEFT_RX_ADDR, &val);
		if (ret)
			return ret;
		pr_debug("%s: dev_id=%u right_tx peer_dev_id=%u "
			"left_rx_val=0x%08x match=%d\n",
			__func__, rt->dev[idx].dev_id, rt->dev[i].dev_id,
			val, (val & 0xFFFF) == (pattern & 0xFFFF));
		if ((val & 0xFFFF) == (pattern & 0xFFFF)) {
			*right_idx = i;
			break;
		}
	}

	pr_debug("%s: dev_id=%d, left_dev_id=%d, right_dev_id=%d\n", __func__,
		rt->dev[idx].dev_id,
		(*left_idx != -1) ? rt->dev[*left_idx].dev_id : -1,
		(*right_idx != -1) ? rt->dev[*right_idx].dev_id : -1);

	return 0;
}

static int xh2a_ctl_ctc_topology_init(struct xh2a_ctl_ctc_runtime *rt)
{
	int i;
	int group_id = 0;
	int visited_num = 0;

	for (i = 0; i < rt->chip_num; i++) {
		int ret;

		ret = xh2a_ctl_ctc_discover_neighbors(rt, i,
						      &rt->dev[i].left_dev,
						      &rt->dev[i].right_dev);
		if (ret)
			return ret;
	}

	for (i = 0; i < rt->chip_num; i++) {
		int left = rt->dev[i].left_dev;
		int right = rt->dev[i].right_dev;

		if (left != -1 && rt->dev[left].right_dev != i) {
			pr_err("%s: asymmetric left link dev_id=%u left_dev_id=%u\n",
			       __func__, rt->dev[i].dev_id,
			       rt->dev[left].dev_id);
			return -EINVAL;
		}
		if (right != -1 && rt->dev[right].left_dev != i) {
			pr_err("%s: asymmetric right link dev_id=%u right_dev_id=%u\n",
			       __func__, rt->dev[i].dev_id,
			       rt->dev[right].dev_id);
			return -EINVAL;
		}
	}

	for (i = 0; i < rt->chip_num; i++) {
		rt->dev[i].visited = 0;
		rt->dev[i].chip_id = -1;
		rt->dev[i].group_id = -1;
		rt->dev[i].is_single = 0;
	}

	for (i = 0; i < rt->chip_num; i++) {
		int cur;
		int chip_id;

		if (rt->dev[i].left_dev != -1 || rt->dev[i].visited)
			continue;
		if (rt->dev[i].right_dev == -1)
			continue;
		if (group_id >= XH2A_CTL_CTC_GROUP_MAX)
			return -E2BIG;

		cur = i;
		chip_id = 0;
		while (cur != -1 && !rt->dev[cur].visited) {
			if (chip_id >= XH2A_CTL_CTC_CHIP_MAX)
				return -E2BIG;

			rt->dev[cur].chip_id = chip_id;
			rt->dev[cur].group_id = group_id;
			rt->dev[cur].visited = 1;
			visited_num++;
			rt->group[group_id].dev_id[chip_id] =
				rt->dev[cur].dev_id;
			chip_id++;
			rt->group[group_id].size = chip_id;
			cur = rt->dev[cur].right_dev;
		}
		rt->group[group_id].id = group_id;
		group_id++;
	}

	for (i = 0; i < rt->chip_num; i++) {
		if (rt->dev[i].left_dev != -1 || rt->dev[i].right_dev != -1)
			continue;
		if (group_id >= XH2A_CTL_CTC_GROUP_MAX)
			return -E2BIG;

		rt->dev[i].group_id = group_id;
		rt->dev[i].chip_id = 0;
		rt->dev[i].is_single = 1;
		rt->dev[i].visited = 1;
		visited_num++;
		rt->group[group_id].dev_id[0] = rt->dev[i].dev_id;
		rt->group[group_id].size = 1;
		rt->group[group_id].id = group_id;
		group_id++;
	}

	rt->group_num = group_id;

	if (visited_num != rt->chip_num) {
		pr_err("%s: invalid chain topology visited=%d devices=%d\n",
		       __func__, visited_num, rt->chip_num);
		return -EINVAL;
	}

	return 0;
}

static int xh2a_ctl_ctc_check_linkup(struct xh2a_ctl_ctc_runtime *rt)
{
	int i;

	for (i = 0; i < rt->chip_num; i++) {
		uint32_t ctcss_id;
		uint32_t lane_idx;
		uint32_t reg_val;
		uint64_t mac_base;
		int timeout;
		int start_id;
		int end_id;
		void *handle = rt->dev[i].ctl_ctx->pcie_dev;
		int chip_id = rt->dev[i].chip_id;
		int group_id = rt->dev[i].group_id;
		int group_size;
		int ret;

		if (rt->dev[i].is_single)
			continue;

		pr_info("%s: check dev_id=%u chip_id=%d group_id=%d "
			"is_single=%d pm_was_suspended=%d sys_is_single=%d "
			"needs_phy_prepare=%d fail_info=0x%x\n",
			__func__, rt->dev[i].dev_id, chip_id, group_id,
			rt->dev[i].is_single, rt->dev[i].pm_was_suspended,
			rt->dev[i].sys_dev->ctc_is_single,
			rt->dev[i].sys_dev->ctc_needs_phy_prepare,
			rt->dev[i].sys_dev->ctc_fail_info);

		group_size = rt->group[group_id].size;
		if (chip_id == 0) {
			start_id = 2;
			end_id = 4;
		} else if (chip_id == (group_size - 1)) {
			start_id = 0;
			end_id = 2;
		} else {
			start_id = 0;
			end_id = 4;
		}

		for (ctcss_id = start_id; ctcss_id < end_id; ctcss_id++) {
			for (lane_idx = 0; lane_idx < LANE_NUM; lane_idx++) {
				mac_base = CTCSS0_CFG_BASE +
					   ctcss_id * 0x800000 +
					   CTC_MAC_PHY_MAC0_OFFSET +
					   lane_idx * 0x40000;
				timeout = CTC_INIT_TIMEOUT;
				do {
					ret = xh2a_ctl_ctc_readl(
						handle,
						mac_base +
							COMMON_STATUS0_OFFSET,
						&reg_val);
					if (ret) {
						pr_err("%s: dev_id=%u "
						       "chip_id=%d group_id=%d "
						       "ctcss_id=%u lane=%u "
						       "read COMMON_STATUS0 "
						       "failed ret=%d\n",
						       __func__,
						       rt->dev[i].dev_id,
						       chip_id, group_id,
						       ctcss_id, lane_idx, ret);
						return ret;
					}
					if (reg_val == U32_MAX) {
						pr_err("%s: dev_id=%u "
						       "chip_id=%d group_id=%d "
						       "ctcss_id=%u lane=%u "
						       "COMMON_STATUS0 "
						       "read all-ones\n",
						       __func__,
						       rt->dev[i].dev_id,
						       chip_id, group_id,
						       ctcss_id, lane_idx);
						return -EIO;
					}
					if ((reg_val & 0x3) == 0x3)
						break;
					usleep_range(1000, 1000);
				} while (--timeout > 0);
				if (timeout <= 0) {
					pr_err("%s: dev_id=%u chip_id=%d "
					       "group_id=%d ctcss_id=%u "
					       "lane=%u COMMON_STATUS0 timeout "
					       "reg=0x%x\n",
					       __func__, rt->dev[i].dev_id,
					       chip_id, group_id, ctcss_id,
					       lane_idx, reg_val);
					return -ETIMEDOUT;
				}
			}
			for (lane_idx = 0; lane_idx < LANE_NUM; lane_idx++) {
				mac_base = CTCSS0_CFG_BASE +
					   ctcss_id * 0x800000 +
					   CTC_MAC_PHY_MAC0_OFFSET +
					   lane_idx * 0x40000;
				timeout = CTC_INIT_TIMEOUT;
				do {
					ret = xh2a_ctl_ctc_readl(
						handle,
						mac_base +
							COMMON_STATUS1_OFFSET,
						&reg_val);
					if (ret) {
						pr_err("%s: dev_id=%u "
						       "chip_id=%d group_id=%d "
						       "ctcss_id=%u lane=%u "
						       "read COMMON_STATUS1 "
						       "failed ret=%d\n",
						       __func__,
						       rt->dev[i].dev_id,
						       chip_id, group_id,
						       ctcss_id, lane_idx, ret);
						return ret;
					}
					if (reg_val == U32_MAX) {
						pr_err("%s: dev_id=%u "
						       "chip_id=%d group_id=%d "
						       "ctcss_id=%u lane=%u "
						       "COMMON_STATUS1 "
						       "read all-ones\n",
						       __func__,
						       rt->dev[i].dev_id,
						       chip_id, group_id,
						       ctcss_id, lane_idx);
						return -EIO;
					}
					if ((reg_val & 0x3F) == 0x11)
						break;
					usleep_range(1000, 1000);
				} while (--timeout > 0);
				if (timeout <= 0) {
					pr_err("%s: dev_id=%u chip_id=%d "
					       "group_id=%d ctcss_id=%u "
					       "lane=%u COMMON_STATUS1 timeout "
					       "reg=0x%x\n",
					       __func__, rt->dev[i].dev_id,
					       chip_id, group_id, ctcss_id,
					       lane_idx, reg_val);
					return -ETIMEDOUT;
				}
			}
		}
	}

	return 0;
}

static int xh2a_ctl_ctc_apply_state(struct xh2a_ctl_ctc_runtime *rt)
{
	int i;

	for (i = 0; i < rt->chip_num; i++) {
		mutex_lock(&rt->dev[i].sys_dev->sys_mutex);
		rt->dev[i].sys_dev->ctc_chip_id = rt->dev[i].chip_id;
		rt->dev[i].sys_dev->ctc_group_id = rt->dev[i].group_id;
		rt->dev[i].sys_dev->ctc_group_num = rt->group_num;
		rt->dev[i].sys_dev->ctc_is_single = rt->dev[i].is_single;
		mutex_unlock(&rt->dev[i].sys_dev->sys_mutex);
	}

	return 0;
}

static int xh2a_ctl_ctc_commit_runtime(struct xh2a_ctl_ctx *ctl,
				       struct xh2a_ctl_ctc_runtime *rt)
{
	LIST_HEAD(new_list);
	int i;
	int ret;

	ret = xh2a_ctl_ctc_group_list_prepare(rt, &new_list);
	if (ret)
		return ret;

	for (i = 0; i < rt->chip_num; i++)
		xh2a_ctl_ctc_write_sysctrl_chip_id(rt->dev[i].ctl_ctx,
						   rt->dev[i].chip_id);

	xh2a_ctl_ctc_group_list_commit(ctl, &new_list);
	return xh2a_ctl_ctc_apply_state(rt);
}

static int xh2a_ctl_ctc_finalize_topology(struct xh2a_ctl_ctx *ctl,
					  struct xh2a_ctl_ctc_runtime *rt,
					  const char *source)
{
	int i;
	int ret;

	ret = xh2a_ctl_ctc_check_linkup(rt);
	if (ret) {
		pr_err("[CTC] topology %s linkup check failed ret=%d\n",
		       source, ret);
		return ret;
	}

	ret = xh2a_ctl_ctc_commit_runtime(ctl, rt);
	if (ret) {
		pr_err("[CTC] topology %s commit failed ret=%d\n", source,
		       ret);
		return ret;
	}

	for (i = 0; i < rt->chip_num; i++) {
		if (rt->dev[i].is_single)
			xh2a_ctc_shutdown_single(rt->dev[i].sys_dev);
	}

	pr_info("[CTC] topology %s: groups=%d devices=%d\n", source,
		rt->group_num, rt->chip_num);

	return 0;
}

static int xh2a_ctl_ctc_build_full_topology(struct xh2a_ctl_ctx *ctl,
					    struct xh2a_ctl_ctc_runtime *rt)
{
	int ret;

	ret = xh2a_ctl_ctc_topology_init(rt);
	if (ret) {
		pr_err("[CTC] physical topology probe failed ret=%d\n", ret);
		return ret;
	}

	return xh2a_ctl_ctc_finalize_topology(ctl, rt,
					      "built from physical links");
}

static int xh2a_ctl_ctc_get_info_by_dev(struct xh2a_ctl_ctx *ctl,
					uint32_t dev_id,
					struct xh2a_ctl_ctc_info *info)
{
	struct xh2a_ctl_ctc_group_entry *entry;
	struct xh2a_ctl_ctc_group_entry *found_entry = NULL;
	struct xh2a_ctl_dev_ctx *ctl_ctx = NULL;
	struct xh2a_sys_dev *sys_dev = NULL;
	uint32_t group_num = 0;
	int chip_id = -1;
	int i;
	int ret = 0;

	if (!ctl || !info)
		return -EINVAL;

	info->chip_id = CTC_UNINIT_VAL;
	info->group_id = CTC_UNINIT_VAL;
	info->group_size = CTC_UNINIT_VAL;
	info->group_num = CTC_UNINIT_VAL;
	info->is_single = CTC_UNINIT_VAL;

	mutex_lock(&ctl->lock);
	if (ctl->ctc_topology_dirty) {
		ret = -EAGAIN;
		goto out_unlock;
	}

	list_for_each_entry(entry, &ctl->ctc_group_list, node) {
		group_num++;
		for (i = 0; i < entry->chip_num; i++) {
			if (!found_entry &&
			    entry->live_dev_id[i] == dev_id) {
				found_entry = entry;
				chip_id = i;
			}
		}
	}

	if (!found_entry) {
		ret = -ENODEV;
		pr_err("%s: dev_id=%u not found in any group\n", __func__,
		       dev_id);
		goto out_unlock;
	}

	ret = xh2a_ctl_ctc_find_ctx_locked(ctl, dev_id, &ctl_ctx);
	if (ret)
		goto out_unlock;

	ret = xh2a_ctl_get_ipu_dev(ctl_ctx, &sys_dev);
	if (ret)
		goto out_unlock;

	mutex_lock(&sys_dev->sys_mutex);
	if (xh2a_ctl_ctc_is_uninit(sys_dev)) {
		mutex_unlock(&sys_dev->sys_mutex);
		ret = -EAGAIN;
		goto out_unlock;
	}
	mutex_unlock(&sys_dev->sys_mutex);

	info->chip_id = chip_id;
	info->group_id = found_entry->group_id;
	info->group_size = found_entry->chip_num;
	info->group_num = group_num;
	info->is_single = (found_entry->chip_num == 1);

out_unlock:
	mutex_unlock(&ctl->lock);
	return ret;
}

long xh2a_ctl_ctc_get(struct file *filp, struct xh2a_ctl_ctc_get_req *req)
{
	struct xh2a_ctl_ctx *ctl = xh2a_ctl_file_ctx_get(filp);
	int ret;

	if (!ctl || !req)
		return -EINVAL;

	ret = xh2a_ctl_ctc_wait_ready(ctl, 0);
	if (ret)
		return ret;

	mutex_lock(&ctl->ctc_mutex);
	ret = xh2a_ctl_ctc_get_info_by_dev(ctl, req->dev_id, &req->info);
	mutex_unlock(&ctl->ctc_mutex);

	return ret;
}

static bool xh2a_ctl_ctc_has_devices_locked(struct xh2a_ctl_ctx *ctl)
{
	return !list_empty(&ctl->ctl_list);
}

struct xh2a_ctl_ctc_topo_ready_snapshot {
	enum xh2a_ctl_ctc_auto_state state;
	bool has_devices;
	bool topology_dirty;
	int err;
};

static bool xh2a_ctl_ctc_wait_condition(struct xh2a_ctl_ctx *ctl)
{
	bool ready;

	mutex_lock(&ctl->lock);
	ready = (ctl->ctc_auto_state == XH2A_CTL_CTC_AUTO_READY &&
		 !ctl->ctc_topology_dirty) ||
		ctl->ctc_auto_state == XH2A_CTL_CTC_AUTO_FAILED ||
		!xh2a_ctl_ctc_has_devices_locked(ctl);
	mutex_unlock(&ctl->lock);

	return ready;
}

static void
xh2a_ctl_ctc_snapshot_ready(struct xh2a_ctl_ctx *ctl,
			    struct xh2a_ctl_ctc_topo_ready_snapshot *snap)
{
	mutex_lock(&ctl->lock);
	snap->state = ctl->ctc_auto_state;
	snap->err = ctl->ctc_auto_errno;
	snap->has_devices = xh2a_ctl_ctc_has_devices_locked(ctl);
	snap->topology_dirty = ctl->ctc_topology_dirty;
	mutex_unlock(&ctl->lock);
}

static bool xh2a_ctl_ctc_has_active_locks(struct xh2a_ctl_ctx *ctl)
{
	struct xh2a_ctl_dev_ctx *ctl_ctx;
	bool active = false;

	mutex_lock(&ctl->lock);
	list_for_each_entry(ctl_ctx, &ctl->ctl_list, node) {
		struct task_struct *task = NULL;

		mutex_lock(&ctl_ctx->lock);
		active = ctl_ctx->refcnt != 0 && ctl_ctx->owner_pid != NULL;
		if (active)
			task = get_pid_task(ctl_ctx->owner_pid, PIDTYPE_TGID);
		active = active && task != NULL;
		mutex_unlock(&ctl_ctx->lock);
		if (task)
			put_task_struct(task);
		if (active)
			break;
	}
	mutex_unlock(&ctl->lock);

	return active;
}

static int
xh2a_ctl_ctc_ready_result(const struct xh2a_ctl_ctc_topo_ready_snapshot *snap)
{
	if (!snap->has_devices)
		return -ENODEV;
	if (snap->state == XH2A_CTL_CTC_AUTO_READY && !snap->topology_dirty)
		return 0;
	if (snap->state == XH2A_CTL_CTC_AUTO_FAILED)
		return snap->err ? snap->err : -EIO;

	return -EAGAIN;
}

int xh2a_ctl_ctc_check_ready(struct xh2a_ctl_ctx *ctl)
{
	struct xh2a_ctl_ctc_topo_ready_snapshot snap;

	if (!ctl)
		return -EINVAL;

	xh2a_ctl_ctc_snapshot_ready(ctl, &snap);
	return xh2a_ctl_ctc_ready_result(&snap);
}

int xh2a_ctl_ctc_wait_ready(struct xh2a_ctl_ctx *ctl, uint32_t timeout_ms)
{
	unsigned long timeout_j = timeout_ms ? msecs_to_jiffies(timeout_ms) : 0;

	if (!ctl)
		return -EINVAL;

	for (;;) {
		struct xh2a_ctl_ctc_topo_ready_snapshot snap;
		int ret;

		xh2a_ctl_ctc_snapshot_ready(ctl, &snap);
		ret = xh2a_ctl_ctc_ready_result(&snap);
		if (ret != -EAGAIN)
			return ret;

		if (timeout_ms) {
			long left = wait_event_interruptible_timeout(
				ctl->ctc_wq, xh2a_ctl_ctc_wait_condition(ctl),
				timeout_j);
			if (left == 0)
				return -ETIMEDOUT;
			if (left < 0)
				return left;
			timeout_j = left;
		} else {
			ret = wait_event_interruptible(
				ctl->ctc_wq, xh2a_ctl_ctc_wait_condition(ctl));
			if (ret)
				return ret;
		}
	}
}

/*
 * Wait for topology readiness with an already computed deadline budget.
 * Unlike xh2a_ctl_ctc_wait_ready(), zero is not an infinite wait here.
 */
int xh2a_ctl_ctc_wait_ready_timeout(struct xh2a_ctl_ctx *ctl,
				     unsigned long timeout_jiffies)
{
	long left = timeout_jiffies;

	if (!ctl)
		return -EINVAL;
	if (!left)
		return -ETIMEDOUT;

	for (;;) {
		struct xh2a_ctl_ctc_topo_ready_snapshot snap;
		int ret;

		xh2a_ctl_ctc_snapshot_ready(ctl, &snap);
		ret = xh2a_ctl_ctc_ready_result(&snap);
		if (ret != -EAGAIN)
			return ret;

		left = wait_event_interruptible_timeout(
			ctl->ctc_wq, xh2a_ctl_ctc_wait_condition(ctl), left);
		if (left == 0)
			return -ETIMEDOUT;
		if (left < 0)
			return left;
	}
}

static int __xh2a_ctl_ctc_build_topology_locked(struct xh2a_ctl_ctx *ctl)
{
	struct xh2a_ctl_ctc_runtime *rt;
	bool pm_held = false;
	bool reinit_inhibited = false;
	bool has_prepare_dev = false;
	int ret = 0;

	if (!ctl)
		return -EINVAL;

	rt = kzalloc(sizeof(*rt), GFP_KERNEL);
	if (!rt)
		return -ENOMEM;

	ret = xh2a_ctl_ctc_collect_devs(ctl, rt);
	if (ret < 0)
		goto out_free;

	if (rt->chip_num == 0) {
		ret = -ENODEV;
		goto out_free;
	}

	/* Do not reset PHYs while an existing client owns a device lock. */
	if (xh2a_ctl_ctc_has_active_locks(ctl)) {
		ret = -EAGAIN;
		goto out_free;
	}

	xh2a_ctl_ctc_reinit_inhibit(rt);
	reinit_inhibited = true;
	ret = xh2a_ctl_ctc_runtime_pm_get(rt);
	if (ret)
		goto out_free;
	pm_held = true;

	has_prepare_dev = xh2a_ctl_ctc_has_prepare_dev(rt);
	if (has_prepare_dev) {
		ret = xh2a_ctl_ctc_prepare_probe_phy(rt);
		if (ret)
			goto out_release;
	}

	ret = xh2a_ctl_ctc_build_full_topology(ctl, rt);
out_release:
	if (pm_held)
		xh2a_ctl_ctc_runtime_pm_put(rt);
out_free:
	if (reinit_inhibited)
		xh2a_ctl_ctc_reinit_allow(rt);
	kfree(rt);
	return ret;
}

int xh2a_ctl_ctc_build_topology_auto_locked(struct xh2a_ctl_ctx *ctl)
{
	if (!ctl)
		return -EINVAL;

	lockdep_assert_held(&ctl->ctc_mutex);

	return __xh2a_ctl_ctc_build_topology_locked(ctl);
}

int xh2a_ctl_ctc_recover_devmask_locked(struct xh2a_ctl_ctx *ctl,
					const unsigned long *devmask)
{
	struct xh2a_ctl_ctc_runtime *rt;
	bool affected_group[XH2A_CTL_CTC_GROUP_MAX] = { 0 };
	bool target_seen = false;
	bool pm_held = false;
	bool reinit_inhibited = false;
	int i;
	int ret;

	rt = kzalloc(sizeof(*rt), GFP_KERNEL);
	if (!rt)
		return -ENOMEM;

	ret = xh2a_ctl_ctc_collect_devs(ctl, rt);
	if (ret < 0)
		goto out_free;
	if (rt->chip_num == 0) {
		ret = -ENODEV;
		goto out_free;
	}

	for (i = 0; i < rt->chip_num; i++) {
		struct xh2a_sys_dev *sys_dev = rt->dev[i].sys_dev;
		int group_id;

		if (!test_bit(rt->dev[i].dev_id, devmask))
			continue;

		target_seen = true;
		mutex_lock(&sys_dev->sys_mutex);
		group_id = sys_dev->ctc_group_id;
		mutex_unlock(&sys_dev->sys_mutex);
		if (group_id == CTC_UNINIT_VAL || group_id < 0 ||
		    group_id >= XH2A_CTL_CTC_GROUP_MAX) {
			ret = -ENODEV;
			goto out_free;
		}
		affected_group[group_id] = true;
	}

	if (!target_seen) {
		ret = -ENODEV;
		goto out_free;
	}

	xh2a_ctl_ctc_reinit_inhibit(rt);
	reinit_inhibited = true;
	ret = xh2a_ctl_ctc_runtime_pm_get(rt);
	if (ret)
		goto out_free;
	pm_held = true;

	for (i = 0; i < rt->chip_num; i++) {
		if (!test_bit(rt->dev[i].dev_id, devmask))
			continue;

		mutex_lock(&rt->dev[i].sys_dev->sys_mutex);
		ret = xh2a_sys_ctc_phy_prepare(
			rt->dev[i].sys_dev,
			XH2A_IPU_CTC_CRG_RESET_PHY_ONLY);
		mutex_unlock(&rt->dev[i].sys_dev->sys_mutex);
		if (ret)
			goto out_release;
	}

	for (i = 0; i < rt->chip_num; i++) {
		struct xh2a_sys_dev *sys_dev = rt->dev[i].sys_dev;
		int group_id;

		mutex_lock(&sys_dev->sys_mutex);
		group_id = sys_dev->ctc_group_id;
		if (group_id >= 0 && group_id < XH2A_CTL_CTC_GROUP_MAX &&
		    affected_group[group_id])
			ret = xh2a_sys_ctc_phy_wait_linkup(sys_dev);
		else
			ret = 0;
		mutex_unlock(&sys_dev->sys_mutex);
		if (ret)
			goto out_release;
	}

	ret = xh2a_ctl_ctc_topology_init(rt);
	if (ret)
		goto out_release;

	ret = xh2a_ctl_ctc_check_linkup(rt);
	if (ret)
		goto out_release;

	ret = xh2a_ctl_ctc_commit_runtime(ctl, rt);
	if (ret)
		goto out_release;

	for (i = 0; i < rt->chip_num; i++) {
		if (rt->dev[i].is_single)
			xh2a_ctc_shutdown_single(rt->dev[i].sys_dev);
	}

	pr_info("[CTC] recovery rebuilt topology: groups=%d devices=%d\n",
		rt->group_num, rt->chip_num);

out_release:
	if (pm_held)
		xh2a_ctl_ctc_runtime_pm_put(rt);
out_free:
	if (reinit_inhibited)
		xh2a_ctl_ctc_reinit_allow(rt);
	kfree(rt);
	return ret;
}

MODULE_LICENSE("GPL");
