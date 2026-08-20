# AX650N 芯片支持开发计划

> 分支：`feature/axera-ax650-support`
> 基线：`feature/multi-chip-support`（0a1e7ba9，含 BM1684 + RK3588 支持）
> 目标：在现有 Sophon（BM1688/CV186X/BM1684）与 Rockchip（RK3576/RK3588）基础上，
> 新增爱芯元智（AXERA）**AX650N**（SoC 模块）支持。
> 状态：**计划中（Reviewed）**
> 已确认决策（2026-08-20）：
> - **AXCL SDK**：暂无现成 SDK，需从爱芯官方渠道获取（HuggingFace/官网/axcl-runtime 仓库）
> - **模型转换环境**：WSL2 Ubuntu + Python 3.10（复用 BM1684 TPU-MLIR 已验证环境）
> - **媒体后端**：初期复用 CPU/FFmpeg 软解 + AXERA NPU 推理；AXERA 硬件媒体（IVPS/VDEC）列为后续技术债
> ⚠️ **真机状态**：AX650N 真机/开发板暂未确认在身边——Phase 0 实测、Phase 5 真机联调
> 阻塞；无真机可推进项（构建冒烟、资源集、模型转换、纯代码、文档）优先执行。

---

## 1. 背景与目标

### 业务目标

在不影响现有 Sophon / Rockchip / x86 稳定版的前提下，让同一份 CosmoEdge 代码能够
编译、打包、部署并在 **爱芯元智 AX650N**（SoC 模块）上完成推理，达到与其他平台
相同的验收门槛。首发范围参照 BM1684：**单路检测 + 分类**。

### 平台事实（已核实，来源：AXERA-TECH 官方仓库/文档）

| 项 | 值 |
| --- | --- |
| 芯片 | 爱芯元智 **AX650N**（AX650A/AX650N 同族，终端计算 SoC） |
| 开发板 | Sipeed **AXera-Pi Pro**（基于 AX650N） |
| 模型格式 | `.axmodel`（Pulsar2 工具链从 ONNX 转换，INT8 量化；原生格式，无需封装） |
| 模型转换工具 | **Pulsar2**（支持 AX650A/AX650N/AX630C/AX620Q；HuggingFace `AXERA-TECH/Pulsar2` 下载） |
| 运行时 SDK | **AXCL**（`axcl-runtime`，`libaxcl.so`/`libaxcl_npu.so`，`AXCL_ENGINE_*` API）；AX650 传统栈为 **ax-engine**（`libax_engine.so`，`AX_ENGINE_*` API）——**需以官方最新 SDK 确认为准** |
| 官方参考 | `AXERA-TECH/ax-samples`（BSD-3-Clause）、`ax-engine`、`axcl-runtime`、`ax-pipeline` |
| 平台 | AArch64 Linux |

**注意**：ax-samples 中 `ax_asr_api` 显示 AX650/AX630C/AX620Q 传统上走 `ax_engine_impl`
（`AX_ENGINE_*`），AX8850 走 `axcl_engine_impl`（`AXCL_ENGINE_*`）；而 `axcl-runtime`
仓库同时托管 AXCL 实现并支持 AX650。**两条 API 线在拿到官方 SDK 前都保留为备选**，
计划按 AXCL 主线（新统一 SDK），ax-engine 为降级备选。

### 边界（AGENTS.md 约束）

- `src/nn/`、`src/infer/`、模型模板、公开 API、新增第三方依赖、大架构调整需走项目
  正常 issue/评审流程。
- x86 或 mock 上的成功不构成 AX650N 设备验收；结论必须按实际测试的层级汇报。
- 未知或未验证的事实保持 `UNVERIFIED`，不得用猜测替代。

---

## 2. 现有平台接入模式盘点（已完成核实）

CosmoEdge 的芯片支持通过"后端三选一 + 芯片白名单 + 资源集"模式接入。
AX650N 需要新增**第四后端**（`COSMO_NN_USE_AXERA_BACKEND`），以下接入点全部需扩展：

| # | 文件 | 现状（Sophon / RKNN） | AX650N 影响 |
| --- | --- | --- | --- |
| 1 | `src/nn/core/common.h` | `DeviceType` 枚举：`DEVICE_NAIVE=0x0000`、`DEVICE_SOPHON_TPU=0x0007`、`DEVICE_CPU=0x0010`、`DEVICE_RKNN=0x0011`；`UsesHostMemory()` 含 NAIVE/CPU/RKNN | 新增 `DEVICE_AXERA=0x0012`；`UsesHostMemory` 是否含 AXERA 取决于 SDK 内存模型（AXCL 有设备内存，需核实） |
| 2 | `src/util/NnBackendConstants.h` | 三后端分支（SOPHON/RKNN/CPU），`kBackendType`/`kEngineType`/`kModelFileExt`/`kSupportedChips[]`/`kNewDirPrefix` | 新增 `COSMO_NN_USE_AXERA_BACKEND` 分支：`kBackendType="AXERA"`、`kEngineType="AX650N"`、`kModelFileExt=".axmodel"`、`kSupportedChips={"AX650N"}`、`kNewDirPrefix="prod_AXERA_"` |
| 3 | `CMakeLists.txt` | 芯片白名单 `^(bm1688\|cv186x\|bm1684\|bm1684x\|rk3576\|rk3588\|rv1126b\|unspecified)$`；NN 后端三选一互斥 | 白名单加 `ax650n`；后端互斥改四选一 |
| 4 | `cmake/device.cmake` | Sophon 专用（libsophon SDK 选择） | 不适用于 AXERA；需新 `cmake/axera.cmake`（参照 `cmake/rknn.cmake`：`COSMO_AXERA_ROOT` 定位头文件/库） |
| 5 | `cmake/rknn.cmake` | 参照样板：`COSMO_RKNN_ROOT` → `rknn_api.h`/`librknnrt.so` → IMPORTED target | 仿此写 `cmake/axera.cmake`：`axcl.h`（或 `ax_engine_api.h`）/`libaxcl.so`（或 `libax_engine.so`） |
| 6 | `src/nn/CMakeLists.txt` | device 源收集按后端 `if(COSMO_NN_USE_SOPHON_BACKEND)` 等 | 加 `device/axera/*` 收集 + `COSMO_NN_USE_AXERA_BACKEND`/`COSMO_NN_USE_HOST_BACKEND`（若适用）定义 + 链接 axcl |
| 7 | `src/nn/device/` | `sophon/`、`rknn/`、`cpu/`、`host/`、`naive/` | 新增 `axera/`：device 注册、node creator、net node、预处理节点、YOLO 解码适配器（参照 `rknn/` 结构） |
| 8 | `src/media/` | `VideoDecoder*`/`VideoEncoder*`/`VideoFrameProc*` + 工厂（Sophon/Rockchip/CPU） | 初期**复用 CPU/FFmpeg 媒体后端**（同 RKNN 初期模式），AX650 原生 IVPS/VDEC 媒体后端列为后续 |
| 9 | `src/service/system/` | `AcceleratorMetricsProviderRknn.cc`（devfreq）、`HardwareQueryUtil.cc`（device_model） | AX650 需新增 metrics provider（AXCL 查询或 `/sys/class/devfreq`），`HardwareQueryUtil` 加 AX650N 默认型号 |
| 10 | `scripts/` | `build.sh`（Sophon）、`build_rknn.sh`/`build_rknn_wsl.sh`（RKNN） | 新增 `scripts/build_axera.sh`（参照 build_rknn_wsl.sh 结构），或泛化 build.sh |
| 11 | `data/resource/aiboxresource_*` | bm1688/cv186x/bm1684/rk3588/x86/rknn | 新增 `aiboxresource_ax650n/`（model_template、models、algorithm、i18n、layout） |
| 12 | `src/web/` | `atomicModel/index.vue` 硬编码 `deviceType.includes('rk3576')`/`rockchip` | 需确认/扩展对 `ax650n` deviceType 的行为 |
| 13 | 验证脚本 | `verify_sophon_open_benchmark_models.py`、`validate-public-v1.1-*.mjs` 平台映射 | 需加 `ax650n` 映射 |
| 14 | CI / Docker | `.github/workflows/`、`docker-compose.*.yml` | 待真机准入后扩展 |

### 模型产物约定（对照）

| 平台 | 产物 | 封装 | 目录前缀 | config.json `chip_type` |
| --- | --- | --- | --- | --- |
| Sophon | `.bmodel` | CENN 头 → `model.nn` | `prod_BM1688_`/`prod_SOPHGO_` | `BM1688`/`CV186X`/`BM1684` |
| RKNN | `.rknn` | 原生 | `prod_ROCKCHIP_` | `RK3576`/`RK3588` |
| **AXERA** | **`.axmodel`** | **原生（同 RKNN）** | `prod_AXERA_` | `AX650N` |

---

## 3. 阶段总览

| 阶段 | 内容 | 依赖 | 评审产出 |
| --- | --- | --- | --- |
| 0 | 预研与物料准备 | 无 | 物料清单 + 风险清单 |
| 1 | 芯片标识与白名单泛化 | 0 | 代码改动 + 编译通过 |
| 2 | 构建矩阵与运行时库 | 1 | 交叉编译产物 + 芯片标签 |
| 3 | AXERA 推理后端接入 | 2 | `device/axera` 编译通过 + 冒烟 |
| 4 | 媒体后端决策 | 1 | 决策记录（初期 CPU 媒体） |
| 5 | 模型转换与资源 | 3 | Pulsar2 转换 `.axmodel` + 资源集 |
| 6 | 端到端验收（真实设备） | 3, 5 | 验收报告 |
| 7 | 文档、前端与发布路径 | 6 | 文档一致 + 升级验证 |

---

## 4. 阶段 0 —— 预研与物料准备

### 目标

确认全部需改动点，拿到 AX650N 全套 SDK 物料，识别无设备时的验证边界。

### 待确认物料清单（已定决策更新）

- [ ] **AXCL SDK（axcl-runtime）**：AArch64 版本，`axcl.h` 头文件 + `libaxcl.so`/`libaxcl_npu.so` 运行库；**暂无现成 SDK，需从爱芯官方渠道获取**（HuggingFace/官网/axcl-runtime 仓库）；确认 AX650N 支持的 API 面（`AXCL_ENGINE_*` vs `AX_ENGINE_*`）
- [ ] **Pulsar2 工具链**：HuggingFace `AXERA-TECH/Pulsar2` 下载，Python 3.10 环境（**WSL2 Ubuntu，复用 BM1684 转换环境**）
- [ ] **AX650N 开发板/SoC 模块**：Sipeed AXera-Pi Pro 或客户 AX650N 模块（真机联调用）
- [ ] **官方示例**：`ax-samples`（BSD-3-Clause）中 YOLOV8/分类示例，作为 `device/axera` 代码参照
- [ ] **设备端内核模块/固件**：`axcl` 设备驱动、`/dev/ax*` 设备节点依赖确认

### 风险清单

| 风险 | 影响 | 缓解 |
| --- | --- | --- |
| AXCL vs ax-engine API 线不确定 | 后端代码方向 | 阶段 0 锁定官方 SDK 后定案；两个 API 面做条件编译隔离 |
| AX650N 设备内存模型（`UsesHostMemory` 归属）不确定 | DeviceType 设计 | 拿 SDK 头文件后确认 `axclrtMalloc` vs host 指针 |
| Pulsar2 转换环境依赖 | 模型转换阻塞 | WSL2 + Python 3.10（复用 BM1684 环境） |
| 真机不在身边 | 端到端验收阻塞 | 无真机可推进项优先执行 |

---

## 5. 阶段 1 —— 芯片标识与白名单泛化

### 改动点

1. `src/nn/core/common.h`：`DeviceType` 加 `DEVICE_AXERA = 0x0012`；`UsesHostMemory()` 按阶段 0 结论决定是否包含 AXERA
2. `src/util/NnBackendConstants.h`：新增 `COSMO_NN_USE_AXERA_BACKEND` 分支
3. `CMakeLists.txt`：芯片白名单加 `ax650n`；NN 后端互斥改四选一；`COSMO_TARGET_CHIP` 校验
4. `scripts/build_axera.sh`（新）：`-c ax650n` 白名单 + 资源集选择（参照 `build_rknn_wsl.sh`）
5. `src/service/system/impl/HardwareQueryUtil.cc`：`device_model` 默认值加 AX650N
6. 前端 `atomicModel/index.vue` 等：确认 `deviceType.includes('ax650n')` 行为

### 验收标准

- 编译通过（x86 冒烟，未选 AXERA 后端时不影响既有平台）
- `COSMO_TARGET_CHIP=ax650n` 能通过 CMake 校验

---

## 6. 阶段 2 —— 构建矩阵与运行时库

### 改动点

1. `cmake/axera.cmake`（新）：`COSMO_AXERA_ROOT` 定位 `axcl.h`/`libaxcl.so`（或 `ax_engine_api.h`/`libax_engine.so`）→ IMPORTED target `axcl`/`axengine`
2. `src/nn/CMakeLists.txt`：收集 `device/axera/*`、定义 `COSMO_NN_USE_AXERA_BACKEND`（+ `COSMO_NN_USE_HOST_BACKEND` 若适用）、链接 axcl
3. 运行时库随包拷贝（参照 `device.cmake` 的 `install(DIRECTORY .../lib/)`）

### 验收标准

- 交叉编译产物含 AXERA 运行时库与 `target-chip.txt` 芯片标签（`ax650n`）
- 既有平台回归通过（Sophon bm1688/bm1684 + RKNN）

---

## 7. 阶段 3 —— AXERA 推理后端接入

### 新增 `src/nn/device/axera/`（参照 `rknn/` 结构）

| 文件 | 职责 |
| --- | --- |
| `axera_device.cc/.h` | `TypeDeviceRegister<...>(DEVICE_AXERA)`；设备内存分配（AXCL 或 host） |
| `axera_node_creator.cc/.h` | `NodeCreatorRegister<AxeraNodeCreator>(DEVICE_AXERA)` |
| `axera_net_node.cc/.h` | 加载 `.axmodel`、`AXCL_ENGINE_*`（或 `AX_ENGINE_*`）创建 handle、绑定 IO、同步推理 |
| `axera_preprocess_node.cc/.h` | 图像预处理（缩放/归一化/布局转换）——初期可用 CPU 节点或 AXERA IVPS |
| `axera_yolov8_adapter.cc/.h` | YOLOV8 输出解码（参照 rknn_yolov8_adapter） |

### 与 RKNN 模式的异同

- **同**：`.axmodel` 原生格式无需封装；node creator/register 机制一致；`NnBackendConstants` 分支一致
- **异**：SDK API 面（AXCL vs RKNN）；内存管理（需按 SDK 定）；IVPS（AXERA 硬件图像处理）是否启用

### 验收标准

- `device/axera` 编译通过；x86/mock 冒烟（若 SDK 支持 x86 仿真）
- 模型导入路径识别 `chip_type: AX650N`

---

## 8. 阶段 4 —— 媒体后端决策

**已定案（2026-08-20）**：初期复用 `COSMO_MEDIA_USE_CPU_BACKEND`（FFmpeg 软解 + 软处理），
推理走 AXERA NPU——与 RKNN 初期模式一致，最快打通端到端。

**后续**：AX650 原生媒体栈（`axcl_vdec`/`axcl_venc`/`axcl_ivps`）作为独立技术债，
需 AArch64 设备验证，列入后续里程碑。

### 决策记录

- 初期：CPU 媒体 + AXERA 推理（无硬件媒体依赖，真机验证门槛最低）✅ 已确认
- 后续：AXERA 硬件媒体（需 SDK 媒体模块 + 真机）

---

## 9. 阶段 5 —— 模型转换与资源

### 转换流程（参照 BM1684 的 TPU-MLIR 流程）

1. **Pulsar2 安装**：WSL2 Ubuntu（Python 3.10），HuggingFace `AXERA-TECH/Pulsar2` wheel
2. **ONNX → .axmodel**：`pulsar2 build`，目标 `AX650`，INT8 量化（需标定集）
3. **产物**：YOLOV8n + 安全帽检测 `.axmodel`（首发范围，参照 BM1684）
4. **资源集**：`data/resource/aiboxresource_ax650n/`：
   - `model_template/`（yolov8_det.json 等，参照 bm1684 资源集）
   - `models/prod_AXERA_<alg>_<name>_<ver>/config.json`（`chip_type: "AX650N"`）+ `model.axmodel`
   - `algorithm/`、`i18n/`、`layout/`（参照 bm1684 资源集结构）

### 验收标准

- Pulsar2 转换成功，`config.json` 的 `chip_type` 与 `NnBackendConstants.kSupportedChips` 匹配
- 模型文件体积/量化信息记录（对标 bm1684 的 model.nn 记录）

---

## 10. 阶段 6/7 —— 验收、文档与发布

### 真机验收（阻塞项）

- 设备准入（AGENTS.md 工作流：start → assess → doctor → verify）
- 单路检测 + 分类场景端到端推理通过
- NPU 负载指标上报（AXCL 查询或 devfreq）

### 文档

- `docs/development/axera-ax650-support-plan.md`（本计划，中/英）
- `docs/guide/build.md`、`docs/guide/configuration.md`、`docs/reference/models.md`
- `docs/tutorials/05-model-porting/model-porting.md`（AXERA 章节）
- README / CHANGELOG

### 发布路径

- `scripts/build_axera.sh` 打包；`target-chip.txt` 含 `ax650n`
- CI 矩阵扩展（真机准入后）

---

## 11. 阶段 0 之后的第一批行动项（无真机可做）

1. 获取 AXCL SDK（AArch64）→ 确认 API 面与内存模型 → 定案 `DeviceType`/`UsesHostMemory`
2. 编写 `cmake/axera.cmake` + `scripts/build_axera.sh` + 白名单/常量扩展
3. 搭建 Pulsar2 转换环境（WSL2）→ 转换 YOLOV8n + 安全帽 `.axmodel`
4. 创建 `aiboxresource_ax650n/` 资源集
5. 文档
