# RK182X host DMA registry regression

This external test module exercises the production DMA helper in an ARM64 Linux guest. It has no PCI driver or PCI device dependency and is outside the product Kbuild. Use the kernel Image that matches the prepared object tree, toolchain, configuration and Module.symvers.

From the kernel source directory on the build server:

```sh
src=$PWD
work=$(mktemp -d)
cp tools/testing/selftests/drivers/rkep/{Makefile,rkep_dma_regression.c} "$work/"
make -C "$KERNEL_OBJECTS" M="$work" RKEP_DRIVER_DIR="$src/drivers/misc/rockchip-rkep" \
    ARCH=arm64 CROSS_COMPILE=aarch64-none-linux-gnu- modules
```

Place the resulting `rkep_dma_regression.ko` in the guest initramfs. Boot an isolated QEMU ARM64 virt machine with four virtual CPUs and the matching product kernel. In the guest, load and unload the test module repeatedly:

```sh
for n in 1 2 3; do
    insmod /rkep_dma_regression.ko || exit 1
    rmmod rkep_dma_regression || exit 1
done
dmesg | grep 'rkep-dma-regression:'
```

Each successful initialization logs PASS. The test publishes thirteen software devices concurrently using the real XArray and workqueues, checks that every selector resolves to its device, injects two allocation failures and a publication failure, and tests removal while polling, while waiting for an interrupt and while both asynchronous diagnostic channels are active. It checks that buffer release stays blocked throughout ioctl DMA and both asynchronous channels, and that the buffer lock is released after removal. It verifies selector reuse, both interrupt timeout returns, an abort concurrent with DONE, and per-function failure notification. DMA callbacks emulate progress and never submit hardware transfers. Endpoint enumeration, physical DMA, MSI delivery, AER, reset and firmware upgrade need separate hardware coverage.
