PWD		:= $(realpath $(dir $(lastword $(MAKEFILE_LIST))))

Q		?= @

DEVICE_NOR ?= UNKNOWN

# must be same as PACKAGE_VERSION in dkms.conf
XH2A_HOST_DRIVER_RELEASE	:= v1.5.0
XH2A_HOST_DRIVER_BUILDTIME	:= $(shell date  +"%Y-%m-%d %H:%M:%S %Z")


ENABLE_CLANG ?= $(shell grep -w 'CONFIG_CC_IS_CLANG=y' $(srctree)/.config 2> /dev/null)
ifneq ($(ENABLE_CLANG),)
ccflags-y	+= -Werror
else
ccflags-y	+= -Wall -Wunused-but-set-variable -Wmaybe-uninitialized
endif

ccflags-y	+= -DXH2A_HOST_DRIVER_RELEASE='"$(XH2A_HOST_DRIVER_RELEASE)"'
ccflags-y	+= -DXH2A_HOST_DRIVER_BUILDTIME='"$(XH2A_HOST_DRIVER_BUILDTIME)"'

ccflags-y	+= -DHOST_KERNEL_VERSION_ABOVE_5_5

ccflags-y	+= -I$(PWD)/include
ccflags-y	+= -I$(PWD)/public

HM_ADDRESS_LAYOUT_FILE := $(PWD)/public/houmo_address_layout.h

xh2a_drv-objs := xh2a_host_drv.o xh2a_chrdev.o
xh2a_drv-objs += ctl/xh2a_ctl.o ctl/xh2a_ctl_lock.o ctl/xh2a_ctl_ctc.o
xh2a_drv-objs += ctl/xh2a_ctl_reset.o
xh2a_drv-objs += pcie/xh2a_pcie.o pcie/xh2a_pcie_hdma.o pcie/xh2a_pcie_api.o pcie/xh2a_pcie_misc.o pcie/xh2a_pcie_efuse.o
xh2a_drv-objs += sys/xh2a_sys.o
ifneq ($(wildcard $(HM_ADDRESS_LAYOUT_FILE)),)
ccflags-y += -DHM_ADDRESS_LAYOUT
endif
xh2a_drv-objs += fast_memory/xh2a_fast_memory.o
xh2a_drv-objs += memory_allocator/xh2a_memory_allocator.o memory_allocator/allocator_algorithm.o
xh2a_drv-objs += memory_allocator/xh2a_memory_allocator_spm_api.o
xh2a_drv-objs += memory_transfer/xh2a_memory_transfer.o memory_transfer/xh2a_memory_transfer_sysdma.o
xh2a_drv-objs += transport/xh2a_transport.o transport/transport_protocol.o
xh2a_drv-objs += qspi/xh2a_qspi.o
xh2a_drv-objs += qspi/axi_crc/xh2a_axi_crc.o
ifeq ($(DEVICE_NOR), MICRON)
xh2a_drv-objs += qspi/nor_flash/device_nor_mt25.o
ccflags-y += -DDEVICE_NOR_MICRON
else ifeq ($(DEVICE_NOR), GIGADEVICE)
xh2a_drv-objs += qspi/nor_flash/device_nor_gd25.o
ccflags-y += -DDEVICE_NOR_GIGADEVICE
else ifeq ($(DEVICE_NOR), UNKNOWN)
ccflags-y += -DDEVICE_NOR_UNKNOWN
endif
xh2a_drv-objs += i2c/xh2a_i2c.o

-include $(PWD)/ipu/ipu.mk

-include $(PWD)/rpmsg_lite/rpmsg_lite.mk

obj-$(CONFIG_XH2A_DRV)	+= xh2a_drv.o
