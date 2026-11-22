# SPDX-License-Identifier: GPL-2.0
# External test module. Set RKEP_DRIVER_DIR when building a private copy.
RKEP_DRIVER_DIR ?= $(abspath $(src)/../../../../../drivers/misc/rockchip-rkep)
ccflags-y += -I$(RKEP_DRIVER_DIR)
obj-m := rkep_dma_regression.o
