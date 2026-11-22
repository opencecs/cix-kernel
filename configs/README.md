# 6.6.89 内核配置参考

Y4 和 Y12 的正式整机构建使用各自已验收的内核 bundle。需要从源码重建时，使用对应 bundle 中的 `config-<kernel.release>`，按[内核构建说明](../README.md#构建当前内核包)生成镜像、设备树和模块。

这里的文件记录 6.6.89 的参考配置和功能选项。它们可供维护与对比，当前正式构建的完整选项以已验收 bundle 为准。

| 文件 | 内容 |
| --- | --- |
| `base-6.6.89.config` | 早期完整配置参考 |
| `board-6.6.89.config` | Mali、VPU、网卡、片上 NPU 等板级功能选项 |
| `debian.config` | Debian 文件系统、容器、网络和 PWM 等选项参考 |

仓库根目录的 `debian.config` 是独立的产品配置片段。`arch/arm64/configs/cix.config` 是树内的 CIX 基础配置。用完整发布配置开始重建后，修改某个功能时，再按需要比较具体选项与已验收配置的差异。
