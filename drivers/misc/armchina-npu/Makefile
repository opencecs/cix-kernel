# SPDX-License-Identifier: GPL-2.0
# ArmChina Zhouyi NPU driver (imported from the cix-bsp handoff, 6.0.1)

ccflags-y += -I$(srctree)/$(src) -I$(srctree)/$(src)/include -I$(srctree)/$(src)/zhouyi
ccflags-y += -DKMD_VERSION=\"6.0.1\"
ccflags-y += -DCONFIG_SKY1
ccflags-y += -DCONFIG_ENABLE_DEVFREQ

obj-$(CONFIG_ARMCHINA_NPU) += armchina_npu.o
armchina_npu-y := aipu.o aipu_common.o aipu_io.o aipu_irq.o aipu_job_manager.o aipu_mm.o aipu_dma_buf.o aipu_priv.o aipu_tcb.o

include $(src)/zhouyi/Makefile
include $(src)/default/Makefile
include $(src)/r329/Makefile
include $(src)/sky1/Makefile
