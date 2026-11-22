# Y4 / Y12 Linux 内核

当前整机版本和验证边界见 [Y4/Y12 开发交接](https://github.com/opencecs/myt-edge/blob/main/docs/DEVELOPMENT-HANDOFF.md)。
M50 和 RK182X 主机驱动由两板共用。RK182X 的 PCI ID 匹配且 probe 成功后，每张卡有一个 `/dev/pcie-rkep-<BDF>`。容器按 PCI 地址选择设备。现行接口见 [RK182X 主机驱动说明](drivers/misc/rockchip-rkep/MYT-INTEGRATION.md)。

## Y4 / Y12 编译与板型分工

同一套 CIX Linux 6.6.89 源码服务 Y4、Y12。公共核心板配置位于
`arch/arm64/boot/dts/cix/sky1-myt-cp6187.dtsi`，包括共同的 CPU/NPU 和内存布局。
板级文件为 `sky1-y4.dts`、`sky1-y12.dts`。

| 板型 | DTB | PCIe 和风扇 |
| --- | --- | --- |
| Y4 | `sky1-y4.dtb` | IX8024 四个 Gen4 ×4 槽。独立 FAN1 由 SoC 控制。FAN3–5 的 PWM 和测速归 MCU |
| Y12 | `sky1-y12.dtb` | IX8024 四个 Gen4 ×2 加八个 Gen4 ×1，八路槽 EN 归 TCA9555；两组 SoC PWM、六路测速 |

Y4 V11 的物理 FAN1 在 UEFI 使用 50% PWM。Linux 驱动在启动保护期间使用全速，温度与测速有效后，SoC 低于 45 °C 使用 30%，45 °C 至低于 70 °C 使用 50%，70 °C 起使用 100%。传感器失效等保护条件见 [FAN1 策略](drivers/hwmon/pwm-fan-y4-policy.h)。MCU 的风扇 1–3 对应物理 FAN3–5，默认 20%。

Y4 V11 的 DP0 接 J8、DP2 接 CON1。设备树关闭未引出的 DP1 及其专用音频、显示管线和 DP PHY。GNOME 虚拟桌面由系统图形栈提供，独立于这两个物理接口。

按下文准备同一份 ARM64 `.config`，再分别生成板型 bundle。第五个参数为 `Y4` 或 `Y12`。
对象树只为同一源码提交和同一配置复用，两条命令顺序运行。每个 bundle 目录必须是新目录。

完整流程见[双板构建手册](https://github.com/opencecs/myt-edge/blob/main/docs/DUAL-BOARD-BUILD.md)。共用 main，板型由构建配置选择。

## 常用入口

| 路径 | 用途 |
| --- | --- |
| configs/ | Y4 完整配置及配置片段，先读 [配置说明](configs/README.md) |
| arch/arm64/boot/dts/cix/ | Sky1 公共核心板及 Y4、Y12 板级设备树 |
| drivers/pci/ | PCIe 控制器、枚举和策略 |
| drivers/rtc/ | RTC 驱动 |
| kernel/printk/ | 内核日志产生、控制台与 /dev/kmsg |
| scripts/build-y4-bundle.sh | 当前 6.6 内核 bundle 生成入口 |

设备树、驱动和配置在本仓库修改；系统服务/应用去 debian-y4，总构建和板载服务去 tools。只使用 main，按明确文件提交，保留已有工作区改动。

## 构建当前内核包

在 Linux 上准备 ARM64 交叉编译器和标准内核构建工具。使用 `CROSS_COMPILE` 指定工具链前缀，默认是 `aarch64-none-linux-gnu-`。选择板型 `Y4` 或 `Y12`。

```sh
make O="$OBJECTS" ARCH=arm64 CROSS_COMPILE="$CROSS_COMPILE" cix.config
scripts/kconfig/merge_config.sh -m -O "$OBJECTS" "$OBJECTS/.config" debian.config
make O="$OBJECTS" ARCH=arm64 CROSS_COMPILE="$CROSS_COMPILE" olddefconfig
CROSS_COMPILE="$CROSS_COMPILE" scripts/build-y4-bundle.sh "$PWD" "$OBJECTS" "$BUNDLE" "" "$BOARD"
```

`OBJECTS` 是内核对象目录，`BUNDLE` 必须是新路径。空的第四个参数省略厂商 PG 编程模块。输出含 Image、对应板型 DTB、模块归档、内核配置和摘要。用私有工厂流程时才传入 PG 源码目录。

将符合 rootfs 兼容性清单的 bundle 传给 [MYT Edge 整机构建](https://github.com/opencecs/myt-edge/blob/main/docs/DUAL-BOARD-BUILD.md) 的 `--kernel-bundle`。若自行编译的内核摘要发生变化，须验证与根文件系统硬件功能的兼容性后再更新清单。

## 验证与交付

先做与修改相关的编译/静态检查，再按影响范围上机核对 Image/DTB/模块版本、dmesg、AER、服务和重启。PCIe 目标是 M50/Optane 交换树稳定 MPS/MRRS 256/256，不回退 128；独立根口按实际设计保留配置。

内核 bundle 不是完整刷机包，也不是可直接上传的系统 OTA。整机构建从内核 bundle 取 Image、DTB 和模块。板级固件由 UEFI 构建提供，在全量包中为 `images/myt-spi.bin`。系统 OTA 随 rootfs 提供配套内核。分别记录源码、校验和与实机验收范围。[整机刷写说明](https://github.com/opencecs/myt-edge/blob/main/docs/BUILD-AND-FLASH-GUIDE.md#4a-完整刷机包)

### Y4 / Y12 加速器运行时电源策略

两板的 `sky1-myt-cp6187.dtsi` 将 Mali GPU 的 `power-policy` 设为 `always_on`，并通过 `myt,keep-runtime-active` 让 AMVX 视频编解码器在空闲时保持活动。策略从各自驱动完成探测后生效。

共用的内核构建脚本启用 `CONFIG_XH2A_KEEP_AWAKE_BY_DEFAULT`。XH2A 驱动对每张实际探测到的 M50 应用 `power/control=on`，空槽无需操作。需要恢复自动挂起时，可在驱动加载时指定 `runtime_pm_enable=1`。验收时等待设备空闲超过 5 秒，分别读取 GPU 的 `power_policy`、VPU 和各 M50 的 `power/control` 及 `runtime_status`。
