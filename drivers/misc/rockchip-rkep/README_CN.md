> MYT：以下为原厂资料，当前共享内核的配置及限制见 [MYT-INTEGRATION.md](MYT-INTEGRATION.md)。


# RK1820 PCIe Endpoint (EP) 设备驱动使用指南

## 驱动文件结构

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

## 驱动编译与安装

驱动的编译分为两种方式，KO 方式与内建方式，可根据实际情况选择合适的编译方式。

### 内建编译

若客户使用的是 Rockchip SoC 的官方 SDK，请检查 SDK 是否已原生支持 PCIe EP 驱动，Kernel 仓库下是否已集成如下提交：

```shell
commit 50915f514cbc808af1bfc3fe6b85fb1d865c2845
Author: Jon Lin <jon.lin@rock-chips.com>
Date:   Tue May 19 23:30:05 2026 +0800

    misc: rockchip: pcie-rkep: add PCIE_EP_RESET_SLOT ioctl for endpoint slot reset
```

若已有相关补丁，开启以下开关以开启相关功能支持：

```shell
# 启用 Rockchip PCIe EP 功能支持
CONFIG_PCIE_FUNC_RKEP=y
CONFIG_PCIE_DW_DMATEST=y
```

### KO 编译

若 Kernel 仓库下没有相关提交，则可以使用 RK182X SDK 内环境编译驱动 KO 文件。

1. 此独立驱动已包含全部必要的依赖文件（本地 include 路径），无需额外头文件：

```shell
obj-m := pcie-rkep.o                          # 目标驱动模块名
KERNELDIR ?= /path/to/your/RK3588/kernel      # 指向已编译的内核源码目录
PWD := $(shell pwd)                           # 当前驱动源码路径

# 默认启用非 GKI 特性（固件下载 sysfs 接口）
# GKI 内核需移除此行
ccflags-y := -DCONFIG_NO_GKI

modules:
    $(MAKE) -C $(KERNELDIR) M=$(PWD) modules  # 编译模块

clean:
    $(MAKE) -C $(KERNELDIR) M=$(PWD) clean    # 清理编译输出
```

2. 使用交叉编译工具链进行编译（根据目标架构替换 `CROSS_COMPILE` 和 `ARCH`）：

```shell
# PC 端执行
make CROSS_COMPILE=aarch64-none-linux-gnu- ARCH=arm64
```

  - `CROSS_COMPILE`: 指定交叉编译器前缀 (e.g., `aarch64-none-linux-gnu-`)
  - `ARCH`: 指定目标 CPU 架构 (e.g., `arm64`)

3. 成功编译后，在当前目录生成驱动模块文件：

```shell
$ file pcie-rkep.ko
pcie-rkep.ko: ELF 64-bit LSB relocatable, ARM aarch64, version 1 (SYSV), BuildID[sha1]=... , with debug_info, not stripped
```

4. 将编译生成的 `pcie-rkep.ko` 驱动文件部署至：`/usr/lib/modules/pcie-rkep.ko`，加载驱动：

```shell
adb push pcie-rkep.ko /usr/lib/modules/pcie-rkep.ko
insmod /usr/lib/modules/pcie-rkep.ko
```

5. 驱动加载成功后，系统将创建设备节点：

```shell
root@rk3588-buildroot:~# ls -l /dev/pcie-rkep-*
crw------- 1 root root 10, 121 Jan  1 12:02 /dev/pcie-rkep-0001:11:00.0
```

6. 系统启动过程中，`rknn3_startup` 脚本会在执行 RK1820 固件下载操作前，自动完成驱动安装，因此后续启动将不需要再手动加载 ko 模块
