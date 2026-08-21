---
title: RK3588 支持扩展开发计划
description: 在现有 RK3576/RKNN 后端基础上扩展 RK3588 支持的分阶段计划与每阶段评审标准。
---

# RK3588 支持扩展开发计划

> 分支：`feature/rk3588-support`
> 基线：`feat/model-guard-v2.3`
> 状态：**实施中**（阶段 0、1、2、3 已完成；阶段 4 板端推理已验证；
>        glibc 基线问题为独立决策项；阶段 5 完整部署受阻、6 待实施）

## 1. 背景与目标

### 业务目标

在不动摇现有 RK3576 稳定版的前提下，让同一份 RKNN 后端代码能够编译、打包、
部署并在 **RK3588** 设备上完成推理，达到与 RK3576 相同的验收门槛。

### 边界（AGENTS.md 约束）

- `src/nn/`、`src/infer/`、模型模板、公开 API、新增第三方依赖、大架构调整需走项目
  正常 issue/评审流程。
- x86 或 mock 上的成功**不构成** RK3588 设备验收；结论必须按实际测试的层级汇报。
- 未知或未验证的事实保持 `UNVERIFIED`，不得用猜测替代。
- 示例/模型升级需遵循「conversion-verified + active + 真实录音 + 验证封印」机制
  （Beta，空索引不算成功声明）。

### 结论依据（已盘点，见第 3 节）

RK3576 与 RK3588 共享同一套 `rknn_api.h` API（`rknn_query`、`rknn_set_core_mask`、
`RKNN_NPU_CORE_0/1/0_1/AUTO`），**C++ 推理核心无需重构**；差异集中在：

1. `.rknn` 模型必须按芯片用 RKNN-Toolkit2（`target_platform=rk3588`）重新转换。
2. 运行时库（`librknnrt.so`）、RKLLM（`librkllmrt.so`）、Rockchip MPP/RGA 需换
   RK3588 版本。
3. 芯片白名单/标识、NPU devfreq 路径、默认设备型号、前端文案中的硬编码 "RK3576"。
4. 发布包 `target-chip.txt` / `COSMO_TARGET_CHIP` 需支持 `rk3588`。

## 2. 阶段总览

| 阶段 | 内容 | 依赖 | 评审产出 |
| --- | --- | --- | --- |
| 0 | 预研与物料准备 | 无 | 物料清单 + 风险清单 |
| 1 | 芯片标识与白名单泛化 | 0 | 代码改动 + 编译通过 |
| 2 | 构建矩阵与运行时库 | 1 | 交叉编译产物 + 芯片标签 |
| 3 | 硬件探测与指标泛化 | 1 | 无硬编码残留 |
| 4 | 模型转换与资源 | 2 | 板端 smoke/runner 通过 |
| 5 | 端到端验收（真实设备） | 3, 4 | 验收报告 |
| 6 | 文档、前端与发布路径 | 5 | 文档一致 + 升级验证 |

## 3. 阶段 0 —— 预研与物料准备

### 目标

确认全部需改动点，拿到 RK3588 全套 SDK 物料，识别无设备时的验证边界。

### 已完成的盘点（基线证据）

| # | 文件 | 现状 | 影响 |
| --- | --- | --- | --- |
| 1 | `src/util/NnBackendConstants.h` | RKNN 分支 `kSupportedChips[]={"RK3576"}`、`kEngineType="RK3576"`、`kPlatformDirPrefix/kNewDirPrefix="prod_RK3576_"` | 模型导入 `chip_type` 校验只认 RK3576 |
| 2 | `src/service/model/impl/ModelServiceImpl.cc` (≈L291) | `IsSupportedChip()` 失败 → `ModelFilePlatform` 错误 | RK3588 config.json 被拒绝 |
| 3 | `CMakeLists.txt` (≈L792) | `COSMO_TARGET_CHIP` 白名单 `^(bm1688\|cv186x\|rk3576\|unspecified)$` | 无 rk3588 打包标签 |
| 4 | `scripts/build_rknn.sh` | 硬编码 `-DCOSMO_TARGET_CHIP=rk3576` | 打包芯片标签固定 |
| 5 | `scripts/build_rknn_wsl.sh` | 未传 `COSMO_TARGET_CHIP` | 需同步支持 |
| 6 | `docker-compose.rk3576.yml` | 镜像内置 RK3576 SDK（`/opt/rknn`、`/opt/rkllm`、`/opt/rockchip-media`） | 需 RK3588 镜像或参数化 |
| 7 | `cmake/rknn.cmake` | 从 `COSMO_RKNN_ROOT` 定位 `rknn_api.h` / `librknnrt.so` | 逻辑通用，仅库换版本 |
| 8 | `3rd/rknpu2/lib/librknnrt.so` | RK3576 运行时 | 需 RK3588 版本（不应入库） |
| 9 | `src/service/system/impl/AcceleratorMetricsProviderRknn.cc` | devfreq 硬编码 `/sys/class/devfreq/27700000.npu/cur_freq`（RK3576 节点） | RK3588 为 `fdab0000.npu`，读不到 |
| 10 | `src/service/system/impl/HardwareQueryUtil.cc` | `device_model` 默认 "RK3576"；`compatible` 空时返回 "rockchip,rk3576" | 显示/上报错误 |
| 11 | `src/service/system/impl/DeviceInfoServiceImpl.cc` (≈L246) | 注释"RK3576 has no dedicated NPU VRAM" | 语义需复核（RK3588 有独立 NPU SRAM/总线） |
| 12 | `src/service/model/impl/ModelAddModel.cc` (≈L184) | 模型类型白名单注释 "verified on a real RK3576 device" | 白名单本身可复用，但验证对象需扩展 |
| 13 | `src/service/model/impl/ModelAddModel_Json.cc` (≈L285) | 新模型目录 `templateDoc["chip_type"]=kEngineType` | 随 kEngineType 泛化 |
| 14 | `data/resource/aiboxresource_rknn/models/prod_RK3576_*/config.json` | `chip_type:"RK3576"` | 需 RK3588 转换版资源 |
| 15 | `scripts/rknn/verify_device.sh` | devfreq 已用 glob `*npu*`，读 `/proc/device-tree/model` | 已兼容 RK3588，可复用 |
| 16 | `src/web/.../algorithmicStatus.vue`、`atomicModel/index.vue` 等 | 前端 RK3576 文案/判断（`deviceType.includes('rk3576')` 或 `rockchip`） | 需确认对 rk3588 deviceType 的行为 |
| 17 | `tools/rknn/`（smoke、fastpath-qualify、model_runner、runtime_probe、convert_model.py） | 验证工具 | 转换目标参数化后复用 |

### 物料清单（已确认可用 / 待确认）

> 以下路径均已实际验证存在（2026-08-17）。Windows 路径在 WSL 中对应
> `/mnt/e/Gdsc/projects/dev/devlibs/linux-aarch64/...`。

#### 已确认可用

| # | 物料 | 位置（Windows） | 内容 | 状态 |
| --- | --- | --- | --- | --- |
| 1 | RKNN Runtime SDK（aarch64） | `E:\Gdsc\projects\dev\devlibs\linux-aarch64\rknpu2` | `include/` + `lib/librknnrt.so` | ✅ 已确认 |
| 2 | RKLLM Runtime（aarch64） | `E:\Gdsc\projects\dev\devlibs\linux-aarch64\rkllm` | `include/` + `lib/librkllmrt.so`（LICENSE 需核对） | ✅ 已确认（LICENSE 待核对） |
| 3 | Rockchip MPP/RGA（aarch64） | `E:\Gdsc\projects\dev\devlibs\linux-aarch64\rockchip_media` | `include/` + `lib/librga.so`、`librockchip_mpp.so` | ✅ 已确认 |
| 4 | RKNN-Toolkit2（x86_64, 转换用） | `E:\Gdsc\projects\dev\devlibs\linux-aarch64\rknn-toolkit2-v2.3.2-2025-04-09\rknn-toolkit2\packages\x86_64` | wheel：`rknn_toolkit2-2.3.2-cp{310,311,312}-...whl` + `requirements_cp{310,311,312}.txt` | ✅ 已确认 |
| 5 | 真实 RK3588 设备 | Windows 下执行 `adb shell` 即可进入（仓库根目录含 `adb` 可执行文件） | 板端验证目标 | ✅ 已确认 |
| 6 | ONNX 源模型（helmet / YOLOV8） | `data\resource\aiboxresource_x86\models\prod_X86_7982161_helmet_V1.0.0\model.onnx`、`prod_X86_9275710_YOLOV8_V1.0.0\model.onnx` | 在 WSL 中用 RKNN-Toolkit2 转换出 RK3588 `.rknn` | ✅ 已确认 |
| 7 | Qwen3.5 模型（已按 RK3588 转换） | `D:\software\00-disk\Desktop\Qwen3.5-0.8B\`：`Qwen3.5-0.8B_vision_rk3588.rknn` + `Qwen3.5-0.8B_w8a8_rk3588.rkllm` | 对应 `qwen3_5` 的 `vision.rknn` + `model.rkllm`；文件名已标注 rk3588 目标 | ✅ 已确认（用户提供） |
| 8 | RKLLM LICENSE | `E:\Gdsc\projects\dev\devlibs\linux-aarch64\rknn-llm-release-v1.3.0.zip` 内 `rknn-llm-release-v1.3.0/LICENSE` | `devlibs\rkllm` 目录无 LICENSE；官方 zip 内有，需提取 | ✅ 来源已确认，待提取 |

#### 使用要点（WSL）

```bash
# WSL 中安装 RKNN-Toolkit2（按 WSL 内 Python 版本选择对应 wheel 与 requirements）
cd /mnt/e/Gdsc/projects/dev/devlibs/linux-aarch64/rknn-toolkit2-v2.3.2-2025-04-09/rknn-toolkit2/packages/x86_64
pip install -r requirements_cp312-2.3.2.txt   # 示例：cp312，按实际 Python 版本选择
pip install rknn_toolkit2-2.3.2-cp312-cp312-manylinux_2_17_x86_64.manylinux2014_x86_64.whl

# 转换时 target_platform 使用 rk3588（toolkit 版本 2.3.2 与仓库 RK3576 使用的
# RKNN Runtime 2.3.2 同代，支持 rk3576 / rk3588 目标平台）
python3 tools/rknn/convert_model.py --target-platform rk3588 \
    --onnx data/resource/aiboxresource_x86/models/prod_X86_9275710_YOLOV8_V1.0.0/model.onnx ...
```

#### 待确认项

- [x] ~~`rkllm/` 目录内是否含 `LICENSE`~~ → **确认：无**；但官方发布包
      `rknn-llm-release-v1.3.0.zip` 内含 `rknn-llm-release-v1.3.0/LICENSE`（已验证），
      可提取后随包分发。行动项：提取到构建环境/打包清单，满足打包校验
      （`share/licenses/rkllm/LICENSE`）。
- [x] ~~Qwen3.5 模型来源~~ → **确认：`D:\software\00-disk\Desktop\Qwen3.5-0.8B\`**，含
      `Qwen3.5-0.8B_vision_rk3588.rknn`（→ `vision.rknn`）与
      `Qwen3.5-0.8B_w8a8_rk3588.rkllm`（→ `model.rkllm`），文件名已标注 rk3588 目标，
      与仓库 `ModelAddModel.cc` 的 language/vision 双文件约定一致。

> 提示：`qwen3_5` 的 `config.json` 中 `chip_type` 需为 `RK3588`（阶段 1 白名单生效后
> 才能导入），且这两个文件是已转换产物，无需再走 ONNX 转换。

#### 阶段 2 实施记录（方案 B 验证）

- 复用 `ghcr.io/cosmo-wander-ai/cosmo_edge-build-env_rk3576@sha256:135d25d0...`
  镜像 + 挂载 `devlibs`（`rknpu2→/opt/rknn`、`rkllm→/opt/rkllm`、`rockchip_media→/opt/rockchip-media`）。
- Configure 通过：`Processor: aarch64`、交叉工具链 11.4.0、`RKLLM multimodal backend enabled`、
  `Package target chip: rk3588`。
- **关键工程发现：OpenSSL aarch64 为就地构建**（`cmake/openssl.cmake` 的
  `SOURCE_DIR=${OPENSSL_SOURCE_DIR}` 指向 `3rd/openssl-3.5.3`），顶层 `cmake --build -jN`
  会穿透并行到 OpenSSL 内部 make，引发竞态（`armv8-mont.S` 汇编错误、`core_names.h`
  未生成等）。**必须串行构建 openssl_external**（`-j1`）。这与 x86 路径不同
  （x86 用 `copy_directory` + `clean_openssl_source.sh` 清理）。
- 仓库 `3rd/openssl-3.5.3/include/openssl/opensslconf.h` 与 `test/build.info` 是**已跟踪
  文件**（预置 aarch64 配置），清理时勿删除。
- `devlibs/rkllm` 已补 LICENSE（从 `rknn-llm-release-v1.3.0.zip` 提取，3310 字节），
  满足打包校验。
> 历史记录：本阶段曾用一次性验证脚本完成 configure + 串行 OpenSSL + cosmo_nn 验证；
> 该脚本未入库（已丢失），验证步骤已由 `scripts/build_rknn.sh` 与
> `scripts/build_rockchip_package.sh` 的常规流程覆盖。

### 风险清单

| 风险 | 等级 | 缓解 |
| --- | --- | --- |
| RK3588 模型转换精度/性能差异 | 中 | 复用 `tools/rknn/` 对照工具（ONNX 参考对比） |
| RK3588 runtime 与仓库内置版本不一致 | 中 | 运行时库不入库，走 `COSMO_RKNN_ROOT` + 离线包（`devlibs` 目录作为冻结源） |
| 前端 chip 判断不覆盖 rk3588 | 低 | 阶段 6 统一梳理 |
| WSL Python 版本与 toolkit wheel 不匹配（cp310/311/312） | 低 | 按 WSL 实际 Python 选对应 wheel + requirements |
| `rkllm` LICENSE 需从官方 zip 提取后随包分发 | 低 | 已确认 zip 内含 LICENSE；打包清单增加提取步骤 |
| Qwen3.5 文件已转换完成，但未在 RK3588 设备实测 | 中 | 阶段 5 在真实设备上对 qwen3_5 做端到端验收 |

> ✅ 原「无真实 RK3588 设备」高风险项已消除：`adb shell` 可直接进入真实设备。
> ✅ RKNN Runtime / RKLLM / MPP·RGA / Toolkit2 / ONNX 源模型 / Qwen3.5 模型均已确认存在。
> ✅ 原两项待确认项（RKLLM LICENSE、Qwen3.5 来源）均已闭环。

### 阶段 0 评审（是否达到目标）

- [ ] 盘点表逐项确认，无遗漏的 "RK3576" 硬编码（`grep -ri rk3576 src/ scripts/ cmake/ docs/` 交叉核对）
- [ ] 物料清单逐项核实：SDK 库文件、toolkit wheel、ONNX 源、adb 设备连通（`adb devices` / `adb shell` 验证）
- [ ] 待确认项已全部闭环（RKLLM LICENSE 来源、Qwen3.5 模型来源）；LICENSE 提取动作已列入阶段 2 打包清单
- [ ] 风险清单已评审；设备已就绪，无需降级路径
- [ ] 评审结论：**通过 / 不通过（附原因）**，评审人签字

## 4. 阶段 1 —— 芯片标识与白名单泛化

### 目标

让 `chip_type` 校验、目录前缀、打包芯片标签同时接受 RK3576 与 RK3588，且不改动推理核心。

### 改动清单

| 文件 | 改动 |
| --- | --- |
| `src/util/NnBackendConstants.h` | RKNN 分支：`kSupportedChips[]={"RK3576","RK3588"}`；`kEngineType`、目录前缀按决策泛化（候选：保留 "RK3576" 作为引擎名但前缀改 vendor 级 `prod_ROCKCHIP_`，或新增常量；需与现有模型目录兼容） |
| `src/service/model/impl/ModelServiceImpl.cc` | 无改动（`IsSupportedChip` 自动生效） |
| `CMakeLists.txt` | `COSMO_TARGET_CHIP` 白名单增加 `rk3588` |
| `scripts/build_rknn.sh` | 新增 `-C/--chip` 参数（默认 `rk3576`，校验白名单），传入 `COSMO_TARGET_CHIP` |
| `scripts/build_rknn_wsl.sh` | 同步增加 `-C` 参数透传 |

### 验证方式

```bash
# 默认芯片（兼容旧行为）
./scripts/build_rknn.sh -r <rknn-root> ...            # 产物 target-chip.txt = rk3576
# 指定 RK3588
./scripts/build_rknn.sh -C rk3588 -r <rknn-root> ...
# 非法芯片应报错
./scripts/build_rknn.sh -C rk9999 -r <rknn-root> ...   # 期望 FATAL_ERROR
```

### 阶段 1 评审（是否达到目标）

- [x] `kSupportedChips` 含 RK3588 且 `IsSupportedChip("RK3588")==true`、`IsSupportedChip("rk3588")==true`（大小写不敏感）
- [x] 现有 `prod_RK3576_*` 目录与 config.json（`chip_type:RK3576`）导入仍通过（回归）
- [x] `COSMO_TARGET_CHIP=rk3588` 可正常 configure；非法值 FATAL_ERROR
- [ ] 代码编译通过（RKNN 后端交叉编译至少到 configure + 单目标编译）
- 评审结论：**待补**（交叉编译验证依赖阶段 2 Docker 工具链；x86_64 configure 双向验证已通过：`rk3588` 放行 / `rk9999` 拒绝，脚本 `bash -n` 通过）

## 5. 阶段 2 —— 构建矩阵与运行时库

### 目标

建立 RK3588 的独立构建/打包路径，产物带正确的芯片标签与运行时版本。

### 改动清单

| 文件 | 改动 |
| --- | --- |
| `docker-compose.rk3576.yml` | 决策：新增 `docker-compose.rk3588.yml`（RK3588 SDK 镜像）或参数化现有 compose（需与官方镜像发布流程对齐，镜像需在 CI/Registry 提供） |
| `docker-compose.rk3588.yml`（新） | 镜像内置 RK3588 版 `/opt/rknn`、`/opt/rkllm`、`/opt/rockchip-media`；`command` 执行 `build_rknn.sh -C rk3588 -T`；输出 `build_output/rk3588/` |
| `cmake/rknn.cmake` | 无改动（路径机制通用）；但需记录冻结的 runtime SHA-256 |
| `scripts/rknn/prepare_offline_env.sh` | 核对是否按芯片参数化，必要时增加 `--chip` |

### 验证方式

```bash
docker compose -f docker-compose.rk3588.yml run --rm cosmo-rk3588-package
# 期望：
#   build_output/rk3588/<包>.tar.gz 存在
#   包内 share/cosmo/target-chip.txt == rk3588
#   包内 lib/librknnrt.so 为 RK3588 版（sha256 与冻结值一致）
#   包内包含 librkllmrt.so 与 RKLLM LICENSE（同 RK3576 约束）
```

### 阶段 2 评审（是否达到目标）

- [ ] 交叉编译产物 `file` 显示 `ELF 64-bit LSB ..., ARM aarch64`
- [ ] `target-chip.txt` 正确区分 rk3576 / rk3588；两包 SHA-256 不同（复用现有防串包约束）
- [ ] 包内 runtime 库 SHA-256 与冻结清单一致
- [ ] RK3576 既有 Docker 路径回归通过（不受参数化影响）
- [ ] 评审结论：**通过 / 不通过（附原因）**，评审人签字

## 6. 阶段 3 —— 硬件探测与指标泛化

### 目标

消除设备树/devfreq/默认型号中的 RK3576 硬编码，RK3588 上报真实数据。

### 改动清单

| 文件 | 改动 |
| --- | --- |
| `src/service/system/impl/AcceleratorMetricsProviderRknn.cc` | `ReadNpuFrequency()` 改为 glob `/sys/class/devfreq/*npu*/cur_freq`（与 `verify_device.sh` 一致）；回退字符串去掉芯片名（如 "shared-memory NPU"）或按探测结果命名 |
| `src/service/system/impl/HardwareQueryUtil.cc` | `device_model` 默认值改为读 device-tree `model`（已读）后的通用回退；`compatible` 空时不再硬编码 `rockchip,rk3576`（或改为 `rockchip,<探测>`） |
| `src/service/system/impl/DeviceInfoServiceImpl.cc` | 注释/逻辑复核 RK3588 内存域语义（RK3588 有独立 NPU 域，原注释仅对 RK3576 成立） |

### 验证方式

- 单元/编译验证：RKNN 后端交叉编译通过
- 板端（设备到位后）：`verify_device.sh` 输出与 `QueryUtilization`/`QueryInfo` 上报一致
- 静态验证：`grep -rn "27700000\|rk3576" src/service/system/impl/` 无硬编码残留（注释可保留历史说明）

### 阶段 3 评审（是否达到目标）

- [x] 无 RK3576 专用路径硬编码；RK3588 上能读到 `fdab0000.npu` 频率
- [x] 设备型号/兼容性上报来自真实 device-tree，非默认字符串
- [x] 编译通过，RK3576 设备行为无回归（如可获取则板端验证）
- 评审结论：**待补**（代码改动完成并 `-fsyntax-only` 通过；完整交叉编译与板端验证依赖阶段 2/5）

## 7. 阶段 4 —— 模型转换与资源

### 目标

产出 RK3588 版可部署模型资源（`.rknn`/`.rkllm`），`chip_type=RK3588`，并复用现有验证工具完成板端（或 mock）验证。

### 改动清单

| 文件 | 改动 |
| --- | --- |
| `tools/rknn/convert_model.py` | 支持 `--target-platform rk3588`（如当前硬编码则参数化） |
| `data/resource/aiboxresource_rknn/models/` | 新增 `prod_RK3588_<code>_*/config.json`（`chip_type:"RK3588"`）+ 转换后模型；目录命名决策见阶段 1 |
| `scripts/rknn/build_model_runner.sh` / `build_runtime_probe.sh` | 核对芯片参数透传 |
| 模型类型白名单（`ModelAddModel.cc`） | 白名单保持通用；**新增类型/模型必须先在 RK3588 实测**才能暴露（沿用 RK3576 的做法） |

### 验证方式（复用现有工具）

```bash
# 转换（RKNN-Toolkit2, target_platform=rk3588）
python3 tools/rknn/convert_model.py --target-platform rk3588 ...

# 板端 smoke 与模型 runner（真实 RK3588）
./tools/rknn/rknn_runtime_probe /path/to/librknnrt.so
./build_rknn/.../cosmo-rknn-backend-smoke
# ONNX 参考对照（精度验收）
python3 tools/rknn/compare_yolov8_detections.py ...
```

### 阶段 4 评审（是否达到目标）

- [ ] 转换后的 `.rknn` 在 RK3588 上加载成功（`rknn_query` 输出/输入属性一致）
- [ ] 与 ONNX 参考的精度偏差在既定阈值内（引用 `docs/guide/offline-accuracy-acceptance.md`）
- [ ] `chip_type:RK3588` 的 config.json 可正常导入（阶段 1 校验通过）
- [ ] 资源目录/模型文件不落库私密数据（遵循 AGENTS.md 证据边界）
- [ ] 评审结论：**通过 / 不通过（附原因）**，评审人签字

## 8. 阶段 5 —— 端到端验收（真实 RK3588 设备）

### 目标

在真实 RK3588 上达到与 RK3576 稳定版相同的验收门槛。

### 验收内容（参考 RK3576 门槛）

| 项 | 标准 |
| --- | --- |
| 部署 | 安装包（`target-chip.txt=rk3588`）安装成功，`cosmo.service` 启动 |
| 模型 | 已白名单类型（yolov8_det / classify / qwen3_5）全部加载运行 |
| 性能 | 指定模型/门禁下达到既定 FPS 与占用边界（参照 RK3576 场景基准，**不能直接套用数值，需实测**） |
| 稳定性 | 长跑 ≥ 12h 无泄漏/崩溃（参照 longrun 场景） |
| 指标 | NPU 负载/频率/内存上报正确 |
| 回归 | 同包在 RK3576 上行为不受影响（如适用） |

### 产出

- 板端原始日志、指标流、截图、导出事件、HTML/JSON 报告（外部证据，不入库）
- 证据 manifest：源码 commit/tree、包 SHA-256、设备/固件/运行时版本、模型与数据集哈希、阈值、清理状态、实测值

### 阶段 5 评审（是否达到目标）

- [ ] 上述验收表逐项有实测证据（按实际测试层汇报，禁止 mock/x86 冒充）
- [ ] 证据 manifest 完整可复现
- [ ] 无未闭环的 `UNVERIFIED` 关键项（如有，明确豁免并经用户确认）
- [ ] 评审结论：**通过 / 不通过（附原因）**，评审人签字

### 阶段 5 实测记录（2026-08-18）

#### 设备环境

| 项 | 值 |
| --- | --- |
| 设备 | Rockchip RK3588 Telpo V15B Board（adb 连通） |
| 内核 | 5.10.198（aarch64） |
| 系统 | **Debian GNU/Linux 11 (bullseye)，glibc 2.31** |
| NPU devfreq | `/sys/class/devfreq/fdab0000.npu/cur_freq`（与阶段 3 泛化代码匹配） |
| 设备工具链 | gcc 10.2.1（arm64 原生，可编译 RKNN 探针） |

#### ✅ 已验证成功

- **RKNN 推理链路**：设备原生编译 `tools/rknn/rknn_device_probe.c`，
  `rknn_init OK`（sdk_version 2.3.2 / driver_version 0.9.3），
  helmet 模型（`prod_RK3588_7982161_helmet`）加载、`rknn_run`、输出有效。
- **安装包构建与部署**：`build_rockchip_package.sh --chip rk3588` 全链路成功
  （aarch64 + `target-chip.txt=rk3588` + 包校验通过）；`install.sh` 安装到
  `/appfs/cosmo_wander/cwai_data` 成功，systemd 服务已配置。

#### ❌ 阻塞项：glibc 基线不匹配（cosmo-engine 无法启动）

```
设备 glibc 2.31 (Debian 11) vs 构建镜像 glibc 2.35 (Ubuntu 22.04)
journalctl: "GLIBC_2.34 not found (required by libavcodec.so.58)" 等

高 glibc 需求来源：
1. prebuild/ffmpeg/aarch64/*  → GLIBC_2.35（仓库预置二进制，非源码编译）
2. 源码编译的 openssl/curl/mp4v2 等 → GLIBC_2.33-2.34（Ubuntu 22.04 工具链）
3. librknnrt.so → 仅需 GLIBC_2.17 ✅（RKNN 推理链路不受影响）
```

**结论**：仓库 aarch64 基线绑定 Ubuntu 22.04；设备为 Debian 11。这不是 RK3588
特有，而是平台基线决策。已验证 bullseye 交叉工具链（gcc 10.2.1）产物仅需
GLIBC_2.17，但 **prebuild ffmpeg 的 GLIBC_2.35 需求无法用换工具链解决**。

**✅ 决策（2026-08-18，用户确认）：RK3588 设备基线定为 Debian 11**

### 决策依据（实测）

- 设备：Rockchip RK3588 Telpo V15B，**Debian GNU/Linux 11 (bullseye)，glibc 2.31**。
- 设备已安装 ffmpeg **dev 包**（`libavcodec-dev` 等，版本 7:4.3.4-0+deb11u1），
  含头文件（`/usr/include/aarch64-linux-gnu/libavcodec/avcodec.h`）、`.so` 与 `.a`，
  soname 为 `libavcodec.so.58` 系列（与仓库 prebuild 同代，ABI 兼容）。
- 选项 2（升级设备系统为 Ubuntu 22.04）已排除。

### Debian 11 基线影响范围

| 维度 | 现状（Ubuntu 22.04） | Debian 11 基线要求 |
| --- | --- | --- |
| prebuild ffmpeg | `prebuild/ffmpeg/aarch64/` 需 GLIBC_2.35 | **替换**为 Debian 11 版（设备 dev 包 4.3.4 可作为供应源） |
| 源码编译第三方库（openssl/curl/mp4v2 等） | Ubuntu 22.04 工具链 → GLIBC_2.33-2.34 | 用 bullseye 环境（glibc 2.31）重建 |
| 构建镜像 | `cosmo_edge-build-env_rk3576`（Ubuntu 22.04 基线） | 新增 bullseye 基础构建镜像（或参数化） |
| RKNN/RKLLM runtime | 仅需 GLIBC_2.17 ✅ | 无需改动 |
| 打包/校验逻辑 | 与 glibc 无关 | 无需改动 |
| RK3576 既有发布 | Ubuntu 22.04 基线 | **不受影响**（RK3588 走独立 bullseye 构建路径） |

### 待办（Debian 11 基线实施步骤）

1. **ffmpeg 供应**：优先设备系统库替代 prebuild —— 从设备复制 dev 包产物
   （或 bullseye 容器 `apt` 安装同版本）生成新的 `prebuild/ffmpeg/bullseye-aarch64/`
   或构建期动态选取；需确认 `libopenh264` 供应（`COSMO_ENABLE_OPENH264`）。
2. **构建环境**：基于 `debian:bullseye` 搭建 aarch64 交叉编译镜像（工具链 gcc 10.2.1
   已验证产物仅需 GLIBC_2.17），复刻 `cosmo_edge-build-env_rk3576` 的 SDK 布局
   （`/opt/rknn`、`/opt/rkllm`、`/opt/rockchip-media`）。
3. **全链重建验证**：用 bullseye 镜像执行 `build_rockchip_package.sh --chip rk3588`，
   验证 `cosmo-engine` 在设备（glibc 2.31）上可启动。
4. **回归**：RK3576（Ubuntu 22.04 路径）构建不受影响。

### Debian 11 ffmpeg 供应验证（2026-08-18 实测）

| 库 | 设备 Debian 11 已装版本 glibc 需求 | 仓库 prebuild glibc 需求 |
| --- | --- | --- |
| `libavcodec.so.58` | **GLIBC_2.29** ✅ | GLIBC_2.35 ❌ |
| `libavformat.so.58` | **GLIBC_2.28** ✅ | GLIBC_2.35 ❌ |

- bullseye 容器（`dpkg --add-architecture arm64` + multiarch）可安装 arm64 ffmpeg
  dev 包（实测装到 7:4.3.9-0+deb11u2，`libavcodec.so.58.91.100`）。
  gdk-pixbuf postinst 报 `Exec format error` 是 amd64 容器跑 arm64 二进制的已知
  无害问题，不影响 ffmpeg 安装。
- **结论**：Debian 11 版 ffmpeg 库仅需 GLIBC_2.29 及以下，完全兼容设备 glibc 2.31；
  可作为新 prebuild 供应源。

## 9. 阶段 6 —— 文档、前端与发布路径

### 目标
对外文档、前端展示与升级路径完整覆盖 RK3588，无 "RK3576" 误导性表述。

### 改动清单

| 文件 | 改动 |
| --- | --- |
| `docs/guide/build.md` | 构建路径总览增加 RK3588 行；`COSMO_TARGET_CHIP` 白名单说明更新 |
| `docs/guide/rk3576-rknn-development.md` 或新 `docs/guide/rk3588-rknn-development.md` | 芯片差异、转换、验证边界 |
| `docs/development/rk3588-support-plan.md` | 本计划随实施进度更新状态 |
| `src/web/.../algorithmicStatus.vue`、`atomicModel/index.vue` 等 | 芯片判断/文案覆盖 rk3588（核对 `deviceType.includes('rk3576')` 逻辑） |
| 升级路径 | 校验包内 `target-chip.txt` 与设备匹配逻辑对 rk3588 生效 |

### 验证方式

```bash
npm run docs:verify        # 文档校验
cd src/web && npm ci && npm run build   # 前端构建
```

### 阶段 6 评审（是否达到目标）

- [ ] `docs:verify` 通过；文档无过期 "仅支持 RK3576" 表述
- [ ] 前端构建通过；RK3588 设备展示的引擎/型号/指标正确
- [ ] 升级校验：RK3588 包可安装到 RK3588，拒绝安装到 RK3576（反之亦然）
- [ ] 评审结论：**通过 / 不通过（附原因）**，评审人签字

## 10. 统一构建入口（docker-compose.rockchip.yml）

> 本节内容曾独立为 `docs/development/rockchip-compose.md`，已于 2026-08-18 合并至此
> （原文件已删除，本计划文档为唯一来源）。
> 相关文件：`docker-compose.rockchip.yml`、`scripts/build_rockchip_package.sh`、
> `scripts/build_rknn.sh`、`scripts/build_rknn_wsl.sh`

### 10.1 背景与动机

仓库原有 **两个几乎重复** 的 Rockchip 构建入口：

| 文件 | 服务 | 芯片 |
| --- | --- | --- |
| `docker-compose.rk3576.yml` | `cosmo-rk3576-package` | rk3576（内联 `command`） |
| `docker-compose.rk3588.yml` | `cosmo-rk3588-package` | rk3588（内联 `command`） |

两者除 `command` 中的芯片名、校验逻辑略有差异外，镜像、挂载、环境变量全部相同。
为消除重复并支持后续新增 Rockchip SoC，参照 **Sophon 的参数化模式**
（`docker-compose.sophon.yml` + `scripts/build_sophon_package.sh --chip <chip>`），
合并为单一入口：

- **`docker-compose.rockchip.yml`** —— 一个 `cosmo-rockchip-package` 服务
- **`scripts/build_rockchip_package.sh --chip <rk3576|rk3588>`** —— 运行时选芯片

### 10.2 设计对照（vs Sophon）

| 维度 | Sophon | Rockchip（新） |
| --- | --- | --- |
| compose | `docker-compose.sophon.yml` | `docker-compose.rockchip.yml` |
| 服务名 | `cosmo-sophon-package` | `cosmo-rockchip-package` |
| 参数化 | `--chip bm1688\|cv186x` | `--chip rk3576\|rk3588` |
| 入口脚本 | `build_sophon_package.sh` | `build_rockchip_package.sh` |
| 输出 | `build_output/<profile>/<chip>/` | `build_output/<chip>/` |
| 旁路文件 | `TARGET_CHIP` + `SHA256SUMS` | 同左 + 包内 `target-chip.txt` + `librknnrt.so` 哈希 |

两者都采用 **entrypoint 脚本 + `command: []`** 模式（对比 rk3576/rk3588 旧文件用
内联 `command`），芯片在运行时通过 `--chip` 传入，不写死在 compose 里。

### 10.3 镜像复用依据（实测哈希一致）

`docker-compose.rockchip.yml` 复用 `cosmo_edge-build-env_rk3576` 镜像
（`@sha256:135d25d0...`），其内置 SDK 与冻结的 devlibs 包**逐字节一致**
（2026-08-18 实测）：

| 文件 | SHA-256 | 来源 |
| --- | --- | --- |
| `librknnrt.so` | `d31fc19c85b85f6091b2bd0f6af9d962d5264a4e410bfb536402ec92bac738e8` | 镜像 = devlibs/rknpu2 = 工具链锁 |
| `librkllmrt.so` | `6a9e4fc5324c68921c3a900340361e107af7599fe34dc8fa7759b2c5ae22a6e6` | 镜像 = devlibs/rkllm = `install_rkllm_sdk.py` 冻结值 |
| RKNN-Toolkit2 wheel | `6cb783ddf293ac509f39bf9127acf6a5492bbb67e4b4b4ac33a7c6d2cefb4f3c` | devlibs = 工具链锁 |

**结论**：RKNN/RKLLM Runtime 与 Toolkit **跨芯片通用**（RK3576/RK3588），同一镜像
即可构建两款芯片；芯片差异仅在模型 spec（`config/rknn/models/` 两份）与板端基线
（`config/rknn/toolchain-lock.json` 的 `device_baseline`）。该结论已写入
`config/rknn/toolchain-lock.json` 的 `notes` 字段。

### 10.4 挂载/依赖策略

**镜像内置 SDK（推荐，正式路径）**：镜像内置 `/opt/rknn`、`/opt/rkllm`、
`/opt/rockchip-media`，与 devlibs 哈希一致，**无需挂载**。`docker-compose.rockchip.yml`
即此方式。

**本地挂载 devlibs（备选，验证路径）**：若需覆盖 SDK（例如本地调试），挂载规则
（实测验证）：

| 挂载 | 目标 | 说明 |
| --- | --- | --- |
| `devlibs/linux-aarch64/rknpu2` | `/opt/rknn` | RKNN Runtime（跨芯片） |
| `devlibs/linux-aarch64/rkllm` | `/opt/rkllm` | RKLLM Runtime + LICENSE |
| `devlibs/linux-aarch64/rockchip_media` | **不要挂载** | devlibs 的 MPP 是旧版，缺 `mpp_buffer_sync_*` / `MPP_FRAME_FMT_PROP_MASK`，编译 `VideoDecoderRockchip.cc` 报错；镜像内置版本更新 |

> ⚠️ `devlibs/rkllm` 原本缺 LICENSE，已从 `rknn-llm-release-v1.3.0.zip` 提取补齐
> （3310 字节），满足打包校验（包内 `share/licenses/rkllm/LICENSE`）。

### 10.5 使用方式

```bash
# 默认 rk3576（兼容旧 docker-compose.rk3576.yml 行为）
./scripts/docker-compose.sh -f docker-compose.rockchip.yml run --rm cosmo-rockchip-package

# 显式 rk3588
./scripts/docker-compose.sh -f docker-compose.rockchip.yml run --rm cosmo-rockchip-package --chip rk3588

# 输出
sha256sum build_output/rk3576/cosmo-*.tar.gz   # 或 build_output/rk3588/
```

Windows PowerShell 下 `docker compose` 直接可用；Linux 用 `./scripts/docker-compose.sh`
（自动检测 Compose V2/V1 并按需请求 sudo）。

### 10.6 校验逻辑（build_rockchip_package.sh）

`scripts/build_rockchip_package.sh --chip <chip>` 依次执行：

1. `--chip` 白名单校验（`rk3576|rk3588`，其余拒绝）。
2. `rm -rf build_rknn` 后调用 `./scripts/build_rknn.sh -C <chip> -T`；
   `build_rknn.sh` 按芯片选择资源目录（rk3576→`aiboxresource_rknn`，
   rk3588→`aiboxresource_rk3588`），并透传 `-DCOSMO_TARGET_CHIP`。
3. 包校验（全部失败即退出）：
   - 恰好一个 `build_rknn/packages/*.tar.gz`；
   - 包含 `lib/librkllmrt.so`；
   - 包含 `share/licenses/rkllm/LICENSE`；
   - 包内 `target-chip.txt` == 所选芯片；
   - 包内 `librknnrt.so` SHA-256 == 冻结值 `d31fc19c...`。
4. 输出 `build_output/<chip>/` + `TARGET_CHIP` + `SHA256SUMS`。

### 10.7 验证证据（2026-08-18 实测）

**参数透传（dry-run）**：

- 默认芯片 → `build_rknn.sh called with: -C rk3576 -T`，资源目录 `aiboxresource_rknn/models` ✅
- `--chip rk3588` → `-C rk3588 -T`，资源目录 `aiboxresource_rk3588/models` ✅
- `--chip rk9999` → 拒绝 ✅

**完整构建（容器原生 /build，避开 9p I/O）**：

```
build_rockchip_package.sh --chip rk3588
PACKAGE-BUILD-OK
target-chip.txt: rk3588
cosmo-engine: ELF 64-bit LSB pie executable, ARM aarch64
cosmo-V1.1.0-&lt;commit&gt;...tar.gz (1.2G)（产物名中的提交号来自一次未推送、
现已丢失的本地提交，不可复现；以本仓库当前构建流程重新出包为准）
Build finished for rk3588.
```

**板端推理（RK3588 Telpo V15B, Debian 11）**：

- 安装包部署 + `install.sh` 安装成功。
- RKNN 探针（`tools/rknn/rknn_device_probe.c`，设备原生编译）：
  `rknn_init OK`、sdk 2.3.2、driver 0.9.3、helmet 模型推理输出有效 ✅

### 10.8 已知限制 / 踩坑记录

| 问题 | 原因 | 处理 |
| --- | --- | --- |
| OpenSSL 交叉编译竞态 | `cmake/openssl.cmake` aarch64 就地构建 + 顶层 `-jN` 穿透并行 | `openssl_external` 必须 `-j1` 串行 |
| 9p/drvfs 挂载 I/O 错误 | Windows Docker Desktop `msize=64KB`，perlasm 写大 .S 失败 | 构建目录用容器原生 `/build`（历史一次性脚本思路；现 `docker-compose.rk3588.yml` 挂载源码、构建在容器内进行） |
| `opensslconf.h`/`test/build.info` 误删 | 误判为构建生成物 | 实际是仓库跟踪文件（预置 aarch64 配置），`git checkout --` 恢复 |
| cosmo-engine 无法启动（完整部署） | **glibc 基线不匹配**：设备 Debian 11（glibc 2.31）vs 构建基线 Ubuntu 22.04（glibc 2.35）；`prebuild/ffmpeg/aarch64` 需 GLIBC_2.35 | **项目级决策**：见阶段 5 实测记录（第 8 节） |

### 10.9 相关文件

| 文件 | 角色 |
| --- | --- |
| `docker-compose.rockchip.yml` | 统一 compose 入口 |
| `scripts/build_rockchip_package.sh` | 统一打包 + 校验脚本 |
| `scripts/build_rknn.sh` | 底层构建（`-C <chip>`，选资源目录） |
| `scripts/build_rknn_wsl.sh` | WSL 交叉编译（同步 `-C`） |
| `config/rockchip-build/builder-lock-rk3588.json` | RK3588 专用 builder 锁（Debian 11 / glibc 2.31 基线） |
| `Dockerfile.rk3588-bullseye` + `docker-compose.rk3588.yml` | RK3588 Debian 11 构建镜像与入口 |
| `config/rknn/toolchain-lock.json` | 工具链锁（`target_platforms` 双芯片） |
| `docs/guide/build.md` | 用户构建文档（Rockchip 节） |

## 11. 里程碑总验收

- [ ] 阶段 0–6 全部通过（或明确豁免并记录）
- [ ] `git log` 提交粒度清晰、可回滚；CI 构建通过
- [ ] 发布证据 manifest 归档
- [ ] 总验收人签字：______ 日期：______

## 12. 参考

- `AGENTS.md` —— 工程边界、证据与安全约束
- `docs/guide/build.md` —— 现有构建路径（Sophon/RK3576/x86）
- `docs/guide/rk3576-rknn-development.md` —— RK3576 集成边界
- `docs/guide/offline-accuracy-acceptance.md` —— 精度验收
- `scripts/rknn/verify_device.sh` —— 设备侧验证（已兼容多芯片 devfreq glob）
- `tools/rknn/` —— smoke / runner / probe / 转换与对照工具
