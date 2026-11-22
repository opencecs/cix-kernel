# RK182X PCIe host driver 1.2.0

The shared Y4/Y12 ARM64 kernel imports the Rockchip driver from
`rk182x-driver-1.2.0-linux(1).zip` (SHA256
`b41b2451a7708b709928a7e6ef552d82584cd5cac5a467ea2abd07f3a024db2b`).
The host driver reports function version `0x00030301`. That value and the
archive version are separate from the endpoint's BAR0 protocol version.

## Host integration

Both products build `CONFIG_MYT_RK182X=m`. Since kernel commit `91a3861ff`, the host uses the vendor probe flow. The PCI aliases match `1d87:182a` and `1d87:356a`. The module enables the PCI function and bus mastering, requests its regions, maps BAR0 and BAR2, and maps BAR4 when present. It registers one misc device per PCI function. Use the node for the card's actual BDF, for example `/dev/pcie-rkep-0000:e1:00.0`.

The released module has no `endpoint_abi` parameter or separate discovery-only mode. See [pcie-rkep.c](pcie-rkep.c) and [rk-pcie-ep.h](rk-pcie-ep.h) for the implemented ioctls, including DMA and slot reset. A successful probe confirms host binding. Firmware compatibility, inference, and recovery require separate testing with the matching card and SDK.

## DMA and device removal

A function with BAR4 acquires a diagnostic DMA selector from a dynamic registry. The selector is local to the running kernel and can be reused after removal. Applications should identify their card by the BDF in the device name. A function with no BAR4 follows the vendor control path and returns `EOPNOTSUPP` for a host DMA transfer. A registration error prevents that function from publishing a usable node.

DMA timeouts and aborts stop further I/O on the affected function. The driver does not reset the shared switch or sibling endpoints as part of this error path. During unbind the driver stops new requests, drains active work and revokes existing memory mappings. An open descriptor keeps its old identity across rebind. It returns `ENODEV` after disconnect, and a revoked mapping faults with `SIGBUS`. Reopen the node after successful reprobe. On a frozen PCI channel the driver completes software teardown before returning to PCI error recovery. Recovery initiated by the PCI core may still affect devices sharing a downstream link.

On detach, a failed DMA stop may trigger a function-level reset (FLR) if that function supports it. If the driver still cannot confirm isolation, it retains the affected function's currently allocated DMA memory and refuses to bind that BDF again until host reboot. This covers buffers owned by this driver. The SDK remains responsible for the lifetime of any externally supplied DMA addresses.

The [DMA regression test](../../../tools/testing/selftests/drivers/rkep/README.md) exercises registration, error propagation and work cancellation using software devices on the product ARM64 kernel. Physical DMA, endpoint firmware and switch error recovery are qualified separately.

## Userspace and validation

The host supplies the kernel module and device nodes. The deployment container supplies the endpoint firmware, runtime, models, and associated tools. The rootfs recipe and host policy live in the [Debian system repository](https://gitlab.moyunteng.com/myt-y4-y12/debian-y4).

For a host binding check, use `lspci -nnk` and enumerate `/dev/pcie-rkep-*`. Match each complete domain:bus:device.function to a single node. Use `modinfo -p pcie-rkep` to check the parameters of the installed module. These checks do not require an endpoint reset or firmware download.

Record DMA, mixed M50/RK182X loads, hot removal, recovery and firmware upgrades independently for the board, cards and SDK under test. Y4 has four downstream slots. Y12 has twelve. A single-card result does not establish behavior with all slots populated.
