// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2022-2025 Houmo AI Inc.
 * Author: shuangyou.wang<shuangyou.wang@houmo.ai>
 */

#ifndef __VERSION_COMPAT_H__
#define __VERSION_COMPAT_H__

#include <linux/version.h>
#include <linux/uaccess.h>
#include <linux/timer.h>

#define __VERSION_CMP(a, b, c, op) \
	(LINUX_VERSION_CODE op KERNEL_VERSION(a, b, c))
#define KERNEL_VERSION_EQ(a, b, c) __VERSION_CMP(a, b, c, ==)
#define KERNEL_VERSION_NE(a, b, c) __VERSION_CMP(a, b, c, !=)
#define KERNEL_VERSION_GT(a, b, c) __VERSION_CMP(a, b, c, >)
#define KERNEL_VERSION_GE(a, b, c) __VERSION_CMP(a, b, c, >=)
#define KERNEL_VERSION_LT(a, b, c) __VERSION_CMP(a, b, c, <)
#define KERNEL_VERSION_LE(a, b, c) __VERSION_CMP(a, b, c, <=)

#if KERNEL_VERSION_GE(5, 0, 0) || !defined(VERIFY_READ)
#define access_read_ok(addr, size)  access_ok(addr, size)
#define access_write_ok(addr, size) access_ok(addr, size)
#else
#define access_read_ok(addr, size)  access_ok(VERIFY_READ, addr, size)
#define access_write_ok(addr, size) access_ok(VERIFY_WRITE, addr, size)
#endif

#if KERNEL_VERSION_GE(5, 10, 0)
#define XH2A_SYSFS_EMIT(buf, fmt, ...) sysfs_emit(buf, fmt, ##__VA_ARGS__)
#define XH2A_SYSFS_EMIT_AT(buf, at, fmt, ...) \
	sysfs_emit_at(buf, at, fmt, ##__VA_ARGS__)
#else
#define XH2A_SYSFS_EMIT(buf, fmt, ...) \
	scnprintf(buf, PAGE_SIZE, fmt, ##__VA_ARGS__)
#define XH2A_SYSFS_EMIT_AT(buf, at, fmt, ...)                                 \
	(((at) >= PAGE_SIZE) ? 0 :                                            \
			       scnprintf((buf) + (at), PAGE_SIZE - (at), fmt, \
					 ##__VA_ARGS__))
#endif
/*
 * Enterprise kernels may backport APIs across versions.
 * Prefer feature detection over LINUX_VERSION_CODE checks.
 */
#ifdef timer_container_of
#define compat_timer_container_of(var, t, field) \
	timer_container_of(var, t, field)
#else
#ifdef from_timer
#define compat_timer_container_of(var, t, field) from_timer(var, t, field)
#else
#define compat_timer_container_of(var, t, field) \
	container_of(t, typeof(*(var)), field)
#endif
#endif

#ifndef DEVICE_ATTR_RO
#define DEVICE_ATTR_RO(_name) DEVICE_ATTR(_name, 0444, _name##_show, NULL)
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 16, 0)
#define XH2A_BIN_ATTR_CONST const
#else
#define XH2A_BIN_ATTR_CONST
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 2, 0)
#define DEVNODE_ATTR_CONST const
#else
#define DEVNODE_ATTR_CONST
#endif

/*
 * IOMMU API compatibility (kernel 3.10 ~ 7.0)
 *
 * - iommu_present(bus):         exists 4.18 ~ 6.12, removed in 6.13
 * - iommu_get_domain_for_dev(): exists 4.18 ~ 7.0, inline stub (returns NULL)
 *                               on 3.10 ~ 4.17 (pre-extern era)
 * - pci_bus_type:               always available since 2.6.32
 * - IOMMU_DOMAIN_IDENTITY:      defined since 4.18
 *
 * Strategy:
 *   4.18 ~ 6.12  : use real functions, const-cast bus arg for 4.18 ~ 6.5
 *   6.13 ~ 7.0   : fall back to dev->iommu_group != NULL
 *   3.10 ~ 4.17  : fall back to dev->iommu_group != NULL (pre-API era)
 *
 * iommu_present() semantics: "is there an IOMMU driver registered on this bus?"
 * We approximate it as "does this device belong to a non-NULL iommu_group?".
 * The difference (global bus-level vs per-device) is acceptable for the P2P
 * use case because:
 *   (a) on x86/ARM servers the two questions almost always have the same answer
 *   (b) returning false when the question is truly "yes" merely causes P2P to
 * be rejected conservatively (no silent data corruption)
 */

/* 4.18 ~ 6.12 : real iommu_present + iommu_get_domain_for_dev exist */
#if KERNEL_VERSION_GE(4, 18, 0) && !KERNEL_VERSION_GE(6, 13, 0)
#if KERNEL_VERSION_GE(6, 6, 0)
/* 6.6 ~ 6.12 : bus arg is const */
#define xh2a_iommu_present(bus) iommu_present(bus)
#else
/* 4.18 ~ 6.5 : bus arg is non-const, needs cast */
#define xh2a_iommu_present(bus) iommu_present((struct bus_type *)(bus))
#endif
#define xh2a_iommu_get_domain(dev) iommu_get_domain_for_dev(dev)

/* 6.13 ~ 7.0 : iommu_present() removed; use iommu_group as fallback */
#elif KERNEL_VERSION_GE(6, 13, 0)
#define xh2a_iommu_present(bus)                                \
	({                                                     \
		(void)(bus);                                   \
		((struct device *)(bus))->iommu_group != NULL; \
	})
#define xh2a_iommu_get_domain(dev) iommu_get_domain_for_dev(dev)

/* 3.10 ~ 4.17 : APIs absent; use iommu_group as fallback */
#else
#define xh2a_iommu_present(bus)                                \
	({                                                     \
		(void)(bus);                                   \
		((struct device *)(bus))->iommu_group != NULL; \
	})
#define xh2a_iommu_get_domain(dev) ((struct iommu_domain *)NULL)
#endif

/* IOMMU_DOMAIN_IDENTITY exists only 4.18+; treat as 0 on earlier kernels */
#if KERNEL_VERSION_GE(4, 18, 0)
#define XH2A_IOMMU_DOMAIN_IDENTITY IOMMU_DOMAIN_IDENTITY
#else
#define XH2A_IOMMU_DOMAIN_IDENTITY 0
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
#define compat_class_create(name) class_create(name)
#else
#define compat_class_create(name) class_create(THIS_MODULE, name)
#endif

/* static_key_false */
#include <linux/jump_label.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 3, 0)
#define DECLARE_STATIC_KEY_FALSE(name) extern struct static_key_false name
/* DEFINE_STATIC_KEY_FALSE is provided by <linux/jump_label.h> since 4.3 */
#define STATIC_KEY_CHECK(name)	 static_branch_unlikely(&(name))
#define STATIC_KEY_ENABLE(name)	 static_branch_inc(&(name))
#define STATIC_KEY_DISABLE(name) static_branch_dec(&(name))

#elif LINUX_VERSION_CODE >= KERNEL_VERSION(3, 18, 0)
#define DECLARE_STATIC_KEY_FALSE(name) extern struct static_key name
#define DEFINE_STATIC_KEY_FALSE(name) \
	struct static_key name = STATIC_KEY_INIT_FALSE

#define STATIC_KEY_CHECK(name)	 static_key_false(&(name))
#define STATIC_KEY_ENABLE(name)	 static_key_slow_inc(&(name))
#define STATIC_KEY_DISABLE(name) static_key_slow_dec(&(name))

#else
#define DECLARE_STATIC_KEY_FALSE(name) extern struct static_key name
#define DEFINE_STATIC_KEY_FALSE(name)  struct static_key name

#define STATIC_KEY_CHECK(name)	 static_key_false(&(name))
#define STATIC_KEY_ENABLE(name)	 static_key_slow_inc(&(name))
#define STATIC_KEY_DISABLE(name) static_key_slow_dec(&(name))

#endif

#endif /* __VERSION_COMPAT_H__ */
