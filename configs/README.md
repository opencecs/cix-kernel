# Y4 6.6.89 编译配置

本目录是 Y4（CIX Sky1 / CP8187）6.6.89 内核的专用配置，配合内核树使用。

| 文件 | 作用 |
|---|---|
| `base-6.6.89.config` | 完整不动点配置；直接作为 `.config` 使用即可复现构建 |
| `board-6.6.89.config` | Y4 板级选项片段（Mali/VPU/R8127 等） |
| `debian.config` | Debian/产品选项片段（如 `CONFIG_XH2A_DRV=n`） |

推荐构建方式（直接用完整配置）：

    cp configs/base-6.6.89.config <obj>/.config
    make -C <kernel> O=<obj> ARCH=arm64 CROSS_COMPILE=<toolchain>- olddefconfig
    make -C <kernel> O=<obj> ARCH=arm64 CROSS_COMPILE=<toolchain>- Image modules dtbs

维护/重新生成 `base-6.6.89.config` 时，用四个输入合并（厂商配置在树内）：

    merge_config.sh -m -O <obj> \
      configs/base-6.6.89.config \
      <kernel>/arch/arm64/configs/cix.config \
      configs/board-6.6.89.config \
      configs/debian.config
    make -C <kernel> O=<obj> ARCH=arm64 CROSS_COMPILE=<toolchain>- olddefconfig

注意：

- `arch/arm64/configs/cix.config` 是内核树内的 CIX 厂商配置，不在本目录重复。
- 本目录只放 Y4/Debian 专用配置；6.1 基线、厂商参考副本和迁移记录不在此处。
- 实际构建编排脚本由独立的编译脚本仓库维护；本目录只提供配置。
