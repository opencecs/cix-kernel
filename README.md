# MYT Edge CIX Linux 内核

本仓库的 GitHub `main` 是 Y4、Y12 共用的内核开发和构建入口。整机版本和验证边界见 [Y4/Y12 开发交接](https://github.com/opencecs/myt-edge/blob/main/docs/DEVELOPMENT-HANDOFF.md)。
M50 和 RK182X 主机驱动由两板共用。RK182X 的 PCI ID 匹配且 probe 成功后，每张卡有一个 `/dev/pcie-rkep-<BDF>`。容器按 PCI 地址选择设备。现行接口见 [RK182X 主机驱动说明](drivers/misc/rockchip-rkep/MYT-INTEGRATION.md)。

[支持的硬件](https://github.com/opencecs/myt-edge/blob/main/docs/PLATFORM-OVERVIEW.md)和[整机开发手册](https://github.com/opencecs/myt-edge/blob/main/docs/DEVELOPMENT-HANDOFF.md)说明两种板型的适用场景、各仓库分工和跨仓库验证流程。

## Y4 / Y12 编译与板型分工

同一套 CIX Linux 6.6.89 源码服务 Y4、Y12。公共核心板配置位于
`arch/arm64/boot/dts/cix/sky1-myt-cp6187.dtsi`，包括共同的 CPU/NPU 和内存布局。
板级文件为 `sky1-y4.dts`、`sky1-y12.dts`。

| 板型 | DTB | PCIe 和风扇 |
| --- | --- | --- |
| Y4 | `sky1-y4.dtb` | PCIe 交换机提供四个 Gen4 ×4 槽。独立 FAN1 由 SoC 控制。FAN3–5 的 PWM 和测速归 MCU |
| Y12 | `sky1-y12.dtb` | PCIe 交换机提供四个 Gen4 ×2 加八个 Gen4 ×1，八路槽 EN 归 TCA9555；两组 SoC PWM、六路测速 |

Y4 V11 的物理 FAN1 在 UEFI 使用 50% PWM。Linux 驱动在启动保护期间使用全速，温度与测速有效后，SoC 低于 45 °C 使用 30%，45 °C 至低于 70 °C 使用 50%，70 °C 起使用 100%。传感器失效等保护条件见 [FAN1 策略](drivers/hwmon/pwm-fan-y4-policy.h)。MCU 的风扇 1–3 对应物理 FAN3–5，默认 20%。

Y4 V11 的 DP0 接 J8、DP2 接 CON1。设备树关闭未引出的 DP1 及其专用音频、显示管线和 DP PHY。GNOME 虚拟桌面由系统图形栈提供，独立于这两个物理接口。

## 常用入口

| 路径 | 用途 |
| --- | --- |
| configs/ | 6.6.89 参考配置和选项片段，见 [配置说明](configs/README.md) |
| arch/arm64/boot/dts/cix/ | Sky1 公共核心板及 Y4、Y12 板级设备树 |
| drivers/pci/ | PCIe 控制器、枚举和策略 |
| drivers/rtc/ | RTC 驱动 |
| kernel/printk/ | 内核日志产生、控制台与 /dev/kmsg |
| scripts/build-y4-bundle.sh | 当前 6.6 内核 bundle 生成入口 |

设备树、驱动和内核配置在本仓库修改。系统服务和用户空间配置由 [cix-rootfs](https://github.com/opencecs/cix-rootfs) 维护。整机构建和板载服务由 [myt-edge](https://github.com/opencecs/myt-edge) 维护。公开与内部构建都从这三个 GitHub 仓库固定提交。

## 构建当前内核包

在 Linux 上使用 ARM64 交叉工具链，并检出整机发布清单记录的内核提交。正式整机构建使用对应板型的已验收内核 bundle，按 `SHA256SUMS` 校验后传给 MYT Edge 的 `--kernel-bundle`。应配套提供该 bundle 及其中的 `config-<kernel.release>`，便于同一版的开发和验证。公开版与内部版的整机组装步骤见[双板构建手册](https://github.com/opencecs/myt-edge/blob/main/docs/DUAL-BOARD-BUILD.md)。

以下命令使用已验收的完整配置从源码重建内核。在本仓库根目录运行。将 `QUALIFIED_CONFIG` 设为该配置文件，`OBJECTS` 设为新对象目录，`BUNDLE` 设为尚不存在的输出目录，`BOARD` 设为 `Y4` 或 `Y12`。

```sh
CROSS_COMPILE="${CROSS_COMPILE:-aarch64-none-linux-gnu-}"
mkdir -p "$OBJECTS"
install -m 0644 "$QUALIFIED_CONFIG" "$OBJECTS/.config"
make O="$OBJECTS" ARCH=arm64 CROSS_COMPILE="$CROSS_COMPILE" olddefconfig
CROSS_COMPILE="$CROSS_COMPILE" scripts/build-y4-bundle.sh "$PWD" "$OBJECTS" "$BUNDLE" "" "$BOARD"
(cd "$BUNDLE" && sha256sum -c SHA256SUMS)
```

空的第四个参数用于普通系统内核，保留网卡运行驱动。PG 模块只用于内部工厂构建，输入由工厂流程单独提供。bundle 包含 `Image`、板型匹配的 DTB、内核模块归档、完整配置、`kernel.release` 和校验清单。两板可复用同一源码提交和配置生成的对象目录，分别指定不同的 `BUNDLE`，顺序生成。

## 验证与交付

重建后的内核如与根文件系统清单的摘要不同，先验证驱动和用户态兼容性，再更新对应板型的 [内核输入清单](https://github.com/opencecs/cix-rootfs/tree/main/fetch)。记录源码提交、配置、工具链、各产物摘要以及实机验收范围。核对内核和 DTB 版本、模块加载、网络、PCIe 和重启结果。PCIe 交换树的 MPS/MRRS 验证目标为 256/256。

整机 Fastboot 全量包由 MYT Edge 组装，内含系统盘镜像和板型匹配的 `images/myt-spi.bin`。系统 OTA 升级根文件系统及其配套内核，由设备校验板型、版本和签名。已签名 MCU 制品随系统交付，签名由内部流程完成。安装步骤见[整机刷写和 OTA 指南](https://github.com/opencecs/myt-edge/blob/main/docs/BUILD-AND-FLASH-GUIDE.md)。

### Y4 / Y12 加速器运行时电源策略

两板的 `sky1-myt-cp6187.dtsi` 将 Mali GPU 的 `power-policy` 设为 `always_on`，并通过 `myt,keep-runtime-active` 让 AMVX 视频编解码器在空闲时保持活动。策略从各自驱动完成探测后生效。

共用的内核构建脚本启用 `CONFIG_XH2A_KEEP_AWAKE_BY_DEFAULT`。XH2A 驱动对每张实际探测到的 M50 应用 `power/control=on`，空槽无需操作。需要恢复自动挂起时，可在驱动加载时指定 `runtime_pm_enable=1`。验收时等待设备空闲超过 5 秒，分别读取 GPU 的 `power_policy`、VPU 和各 M50 的 `power/control` 及 `runtime_status`。
