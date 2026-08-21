---
title: RK3588 / RKNN 集成指南
description: Rockchip RK3588 稳定版的构建、运行时、模型和验证边界。
prev:
  text: 构建指南
  link: /guide/build
next:
  text: macOS Docker Preview
  link: /guide/macos-docker-preview
---

# RK3588 / RKNN 集成指南

> 状态提示：本页是 RK3588 支持的对外集成边界文档。开发实施进度、物料清单与
> 分阶段评审记录见 [RK3588 支持扩展开发计划](/development/rk3588-support-plan)。
> 本页标注 **UNVERIFIED** 的项尚未完成真机证据闭环，不得作为发布声明。

## 能力范围

RK3588 集成沿用 RK3576 的 RKNN/MPP/RGA 后端，不改变 CPU、CUDA 或 Sophon 后端的行为：

- RKNN Runtime 2.3.2 执行静态 batch 的检测和分类模型。
- RKLLM Runtime 1.3.0 配合 RKNN 视觉编码器执行 Qwen3.5 多模态模型。
- Rockchip MPP 执行 H.264/H.265 解码与编码。
- 解码器使用延迟 Copy-out：先对帧进行采样或丢弃，再按需复制宿主机 I420 数据。
- RGA 执行预览与 OSD 路径所需的 Rockchip 图像处理操作。
- 完整 DMA-BUF 零拷贝不属于当前稳定版支持边界。

与 RK3576 的差异集中在芯片相关的四类：

1. `.rknn` 模型必须按芯片用 RKNN-Toolkit2（`target_platform=rk3588`）重新转换。
2. 发布包芯片标签（`target-chip.txt` / `COSMO_TARGET_CHIP`）为 `rk3588`。
3. NPU devfreq 节点名不同（RK3588 为 `fdab0000.npu`），代码已按 sysfs glob 探测。
4. 板端操作系统基线（见下文「固定工具链标识」）。

**RK3588 的长期稳定验证尚未完成**：NPU 推理链路已有板端证据，但完整服务端到端
验收、12 小时长稳与 Qwen3.5 多模态真机验收仍为 **UNVERIFIED**。

## 仓库与证据边界

仓库负责产品代码、构建定义、单元测试、可复现模型工具、可部署 RK3588 资源：

- `data/resource/aiboxresource_rk3588/` —— RK3588 算法与模型（`chip_type: RK3588`）。
- `config/rknn/models/helmet_rk3588.json`、`yolov8_rk3588.json` —— 转换 spec。
- `docker-compose.rockchip.yml` + `scripts/build_rockchip_package.sh --chip rk3588`
  —— 统一构建/打包入口。
- `config/rknn/toolchain-lock.json` —— 工具链锁（`target_platforms` 双芯片）。

板端原始日志、指标流、截图、导出事件以及生成的 HTML/XML/JSON 报告属于外部
验证产物，不应加入源码树。发布证据 manifest 应绑定源码 commit 与 tree、最终
安装包 SHA-256、设备/固件/运行时版本、模型与数据集哈希、阈值、清理状态和实测值。

设备地址、账号数据、本地备份路径和可复用凭据不得进入版本控制配置或证据。

## 固定工具链标识

机器可读的工具链与模型输入锁文件为 `config/rknn/toolchain-lock.json`，当前声明
`target_platforms: ["rk3576", "rk3588"]`：

- RKNN-Toolkit2 2.3.2
- RKNN Model Zoo 2.3.2
- Ubuntu 22.04 x86_64 转换主机与 Python 3.10
- RKNN/RKLLM Runtime 与 Toolkit **跨芯片共享**：`librknnrt.so`
  SHA-256（`d31fc19c...`）在构建镜像、devlibs 冻结包与工具链锁中逐字节一致（已实测）。
- `device_baseline.rk3588`：内核/驱动/系统运行时均为 `null`，状态
  `UNVERIFIED` —— 板端基线等待真机测量闭环。

### 板端操作系统基线（两个实测观察，均未最终闭环）

| 观察 | 设备 | 系统 | glibc | 与构建产物兼容性 |
| --- | --- | --- | --- | --- |
| A（开发计划记录） | Telpo V15B | Debian 11 (bullseye) | 2.31 | ❌ Ubuntu 22.04 基线产物最高需 GLIBC_2.35 |
| B（本次实测） | SmartDev V15BS | Debian 12 (bookworm) | 2.36 | ✅ Ubuntu 22.04 基线产物可直接运行 |

> 说明：仓库 aarch64 构建基线为 Ubuntu 22.04（glibc 2.35），实测产物最高
> glibc 需求 2.35（`prebuild/ffmpeg/aarch64` 与源码编译的 openssl/curl 等）。
> Debian 12（glibc 2.36）可直接运行；Debian 11（glibc 2.31）需要 bullseye
> 重建路径（`Dockerfile.rk3588-bullseye`，含 Debian 11 版 ffmpeg 供应）。
> 该决策与证据记录见 [RK3588 支持扩展开发计划](/development/rk3588-support-plan) 阶段 5。

## 运行时安全边界

保留板端系统 RKNN 运行时作为回滚基线。将 RKNN Runtime 2.3.2 与 CosmoEdge 一起
打包，通过可执行文件 RPATH 或任务局部 `LD_LIBRARY_PATH` 选择它；不要覆盖
`/usr/lib/librknnrt.so`。生产推理使用原生 C API，不依赖 `rknn_server`。

> ⚠️ 已知板端固件约束（UNVERIFIED 待闭环）：本次实测的 SmartDev V15BS 设备，
> 其 boot 分区资源镜像中 `npu@fdab0000` 节点 `status="disabled"`（厂商固件禁用
> NPU）。直接修改 DTB 状态后 u-boot 拒绝启动（疑似 FIT 签名校验，未最终确认）。
> 部署前必须先确认目标设备 NPU 设备树节点为启用状态（`/dev/rknpu` 存在），
> 详见故障排查与开发计划。

## 模型与预处理约定

首批支持的模型与 RK3576 同代，但必须使用 `target_platform=rk3588` 重新转换：

1. 安全帽分类：`1x3x224x224`，ONNX opset 19。
2. YOLOv8 检测：`1x3x640x640`，转换为 ONNX opset 19 / IR 9。

已随仓库提供 RK3588 转换产物（`data/resource/aiboxresource_rk3588/models/`）：

- `prod_RK3588_7982161_helmet_V1.0.0/`（classify，`chip_type: RK3588`）
- `prod_RK3588_9275710_YOLOV8_V1.0.0/`（detect，`chip_type: RK3588`）
- `prod_RK3588_7000001_qwen3_5_V1.0.0/`（`model.rkllm` + `vision.rknn`，`chip_type: RK3588`）

Qwen3.5 多模态模型属于另一份模型合同。一个可导入目录至少包含：

- `model.rkllm`：目标平台为 RK3588 的语言模型；
- `vision.rknn`：与语言模型的图像 token 数、embedding 宽度匹配的视觉编码器；
- `tokenizer.json`：与转换源模型完全一致的分词器；
- `config.json`：`model_type` 为 `qwen3_5`，并声明 `runtime_backend: rkllm`。

四个文件必须作为一组记录 SHA-256。仅有 `librkllmrt.so`、仅能加载文本模型，或仅能
运行 `vision.rknn`，都不能证明多模态能力。

CosmoEdge 负责 resize、通道顺序和归一化。转换过程不得再次固化 mean/std 变换。
CosmoEdge 提供 float32 NCHW 张量；RKNN 边界执行一次显式 NCHW 到 NHWC 拷贝。
输出请求为 float32，以现有后处理器为最终行为基准。生产 YOLO 模型提供三组
box/class head，`yolov8_dfl_v1` 宿主适配器执行 DFL 和 sigmoid，再重建逻辑
`[1,84,8400]` 约定。

## 可复现转换

转换命令与 RK3576 一致，仅 spec 文件与 `--target-platform` 不同：

```bash
python tools/rknn/convert_model.py \
  --spec config/rknn/models/yolov8_rk3588.json --model yolov8-heads.onnx \
  --output yolov8-heads-int8-rk3588.rknn --quantize \
  --dataset yolov8-calibration/dataset.txt
```

`config/rknn/models/helmet_rk3588.json` 与 `yolov8_rk3588.json` 记录了
`target_platform: rk3588`、输入形状、mean/scale 与 ONNX 源 SHA-256。
校准样本和数值一致性样本没有标签，不能替代带标签的 precision/recall/F1 验收集。

## 构建与部署

RK3588 与 RK3576 共用同一固定 digest 构建镜像（其内置 SDK 与 devlibs 冻结包
逐字节一致，哈希已验证）与同一 compose 服务，运行时用 `--chip` 选择：

```bash
# 显式选择 rk3588
./scripts/docker-compose.sh -f docker-compose.rockchip.yml run --rm cosmo-rockchip-package --chip rk3588
sha256sum build_output/rk3588/cosmo-*.tar.gz
```

构建流程要点：

- `build_rockchip_package.sh --chip rk3588` 删除旧 `build_rknn/`，调用
  `build_rknn.sh -C rk3588 -T`；资源目录按芯片选择
  （rk3588 → `data/resource/aiboxresource_rk3588`）。
- 包校验强制：恰好一个包、含 `lib/librkllmrt.so`、含
  `share/licenses/rkllm/LICENSE`、`target-chip.txt == rk3588`、
  `librknnrt.so` SHA-256 == 冻结值。
- 输出 `build_output/rk3588/` + `TARGET_CHIP` + `SHA256SUMS`，不启动应用服务。
- 同时生成 aarch64 验证程序：`cosmo-tests`、`cosmo-rknn-backend-smoke`、
  `cosmo-rknn-fastpath-qualify`。

当前可用产物：`build_output/rk3588/cosmo-V1.1.0-3e982955....tar.gz`
（Ubuntu 22.04 基线，`target-chip.txt=rk3588`）。

运行时应隔离可变数据目录和包内应用目录（同 RK3576）：

```bash
export COSMO_DATA_DIR=/data/cwaiuserdata
export COSMO_APP_DATA_DIR=/appfs/cosmo_wander/cwai_data
export LD_LIBRARY_PATH="$COSMO_APP_DATA_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
```

## 可复用验收场景

场景工具与 RK3576 共用，但真机验收必须以 RK3588 设备为目标执行：

- 客户旅程场景：有界时间内运行 1 路 × 5 FPS，覆盖登录、模型/任务/通道可见性、
  真实原始与算法 HTTP-FLV 播放、OSD 差异、事件、重连、停止/启动恢复和清理。
- 长稳场景：4 路 × 5 FPS 运行 12 小时，启用算法预览客户端并使用
  `--gate-hours 12` 审计。

**UNVERIFIED**：上述场景尚未在 RK3588 真机上完成（NPU 启用与板端基线待闭环）。

## Qwen3.5 多模态真机验收

正式验收必须在 RK3588 真机上输入一张固定测试图，并得到非空、与画面相关的文本结果。
纯文本问答只证明 RKLLM 语言侧，不能代替本项。验收记录至少包含：安装包和四个
模型文件的 SHA-256、RKLLM/Toolkit/驱动版本、目标平台、量化类型、视觉模型输入输出
形状、测试图哈希、返回文本和退出状态。

**UNVERIFIED**：尚未在 RK3588 真机执行多模态验收。

## 已验证发布边界

以下为**已实测**结论：

- **板端 RKNN 推理链路**（开发计划阶段 5 记录，Telpo V15B / Debian 11）：
  设备原生编译 `tools/rknn/rknn_device_probe.c` 后 `rknn_init OK`
  （sdk 2.3.2 / driver 0.9.3），helmet 模型加载、`rknn_run`、输出有效。
- **安装包构建与校验**：`build_rockchip_package.sh --chip rk3588` 全链路成功，
  aarch64 + `target-chip.txt=rk3588` + 包校验通过。
- **运行时跨芯片共享**：`librknnrt.so` / `librkllmrt.so` SHA-256 在构建镜像、
  devlibs 与工具链锁中一致（已实测）。
- **NPU devfreq 泛化**：代码按 `/sys/class/devfreq/*npu*` 探测，
  RK3588 的 `fdab0000.npu` 与 RK3576 的 `27700000.npu` 均覆盖。

以下为 **UNVERIFIED / 未闭环**：

- 完整服务端到端部署与 `cosmo.service` 启动（受 glibc 基线决策与 NPU 启用影响）。
- 12 小时长稳、16 路单算法 / 8 路双算法阶梯边界（RK3576 数值不可直接套用）。
- Qwen3.5 多模态真机验收。
- `toolchain-lock.json` 中 `device_baseline.rk3588`（内核/驱动/运行时）。
- SmartDev V15BS 设备 NPU 节点 `status="disabled"` 的启用方案（u-boot FIT
  校验规则未最终确认）。

这些结论与产物绑定；源码、模型、运行时或安装包变化后必须重新验证。已接受的发布
记录应保留不可变安装包 SHA-256、业务精度结果、凭据安全日志、事件留存结果、清理
状态和实测值。原始验证产物继续保留在源码树之外。
