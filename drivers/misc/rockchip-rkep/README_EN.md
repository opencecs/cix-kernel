> MYT: This is the original vendor guide. See [MYT-INTEGRATION.md](MYT-INTEGRATION.md) for the shared kernel configuration and restrictions.


# RK1820 PCIe Endpoint (EP) Device Driver User Guide

## Driver Files Structure

```shell
.
├── pcie-rkep
│   ├── Makefile
│   ├── pcie-dw-dmatest.c
│   ├── pcie-dw-dmatest.h
│   ├── pcie-rkep.c
│   ├── rk-pcie-ep.h
│   └── rockchip-pcie-dma.h
├── README_CN.md
└── README_EN.md
```

## Driver Compilation and Installation Guide

The compilation of the driver can be carried out in two ways: the KO method and the built-in method. You can choose the appropriate compilation method according to the actual situation.

### Built-in Compilation

If customers are using the official SDK of Rockchip SoC, check whether the SDK natively supports the PCIe EP driver. Check Kernel repository for the following commit:

```shell
commit 50915f514cbc808af1bfc3fe6b85fb1d865c2845
Author: Jon Lin <jon.lin@rock-chips.com>
Date:   Tue May 19 23:30:05 2026 +0800

    misc: rockchip: pcie-rkep: add PCIE_EP_RESET_SLOT ioctl for endpoint slot reset
```

If there are existing patches, turn on the following switches to enable the support for the related functions:

```shell
# Enable Rockchip PCIe EP feature support
CONFIG_PCIE_FUNC_RKEP=y
CONFIG_PCIE_DW_DMATEST=y
```

### KO Compilation

If there are no relevant submissions in the Kernel repository, then the driver KO file can be compiled using the in-environment SDK of RK182X.

1. This standalone driver includes all necessary dependency files (local include paths). No extra headers needed:

```shell
obj-m := pcie-rkep.o                         # Target driver module name
KERNELDIR ?= /path/to/your/RK3588/kernel     # Point to the compiled kernel source directory
PWD := $(shell pwd)                          # Current driver source path

# Enable non-GKI features (firmware download sysfs interface)
# Remove this line for GKI (Generic Kernel Image) targets
ccflags-y := -DCONFIG_NO_GKI

modules:
    $(MAKE) -C $(KERNELDIR) M=$(PWD) modules # Compile the module

clean:
    $(MAKE) -C $(KERNELDIR) M=$(PWD) clean   # Clean up compilation output
```

2. Use the cross-compilation toolchain for compilation (replace `CROSS_COMPILE` and `ARCH` according to the target architecture):

```shell
make CROSS_COMPILE=aarch64-none-linux-gnu- ARCH=arm64
```

  - `CROSS_COMPILE`: Specifies the cross-compiler prefix (e.g., `aarch64-none-linux-gnu-`)
  - `ARCH`: Specifies the target CPU architecture (e.g., `arm64`)

3. After successful compilation, a driver module file is generated in the current directory:

```shell
$ file pcie-rkep.ko
pcie-rkep.ko: ELF 64-bit LSB relocatable, ARM aarch64, version 1 (SYSV), BuildID[sha1]=... , with debug_info, not stripped
```

4. Push the generated `pcie-rkep.ko` to the target system and load the driver:

```shell
adb push pcie-rkep.ko /usr/lib/modules/pcie-rkep.ko
insmod /usr/lib/modules/pcie-rkep.ko
```

5. After the driver is successfully loaded, the system will create a device node:

```shell
root@rk3588-buildroot:~# ls -l /dev/pcie-rkep-*
crw------- 1 root root 10, 121 Jan  1 12:02 /dev/pcie-rkep-0001:11:00.0
```

6. During system startup, the `rknn3_startup` service script will automatically complete the driver installation before executing the RK1820 firmware download operation.
