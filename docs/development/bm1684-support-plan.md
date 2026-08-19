# BM1684 芯片支持开发计划

> 分支：`feature/bm1684-support`
> 状态：关键决策已确认，待实施（Reviewed）
> 目标：在现有 BM1688 / CV186X Sophon 支持的基础上，扩展对算能 BM1684（SM5 SoC 模块）的支持。
> 已确认：目标设备 **SM5 SoC 模块**；有 BM1684 开发设备；首发范围**仅单路检测+分类**；
> 设备 SDK 使用 **libsophon-0.5.1**（linux-aarch64，`E:\Gdsc\projects\dev\devlibs\linux-aarch64\libsophon-0.5.1`），
> **BM1684 与 BM1684X 均使用 0.5.1 SDK 家族**（`device.cmake` 按 `bm1684`/`bm1684x` 选择）；
> **不使用** `3rd/libsophon-0.4.11`；模型转换环境用 **WSL2**（Ubuntu 22.04+，备选 Docker Desktop）。
> 已定案：SDK **拷贝为 `3rd/libsophon-0.5.1/`**（随仓库走，CI 可复现）；
> VPU 解码 **沿用 deprecated `bmvpu_dec_*`**（符号已实测确认仍导出，改动最小，新 API 迁移列为技术债）；
> 转换环境 **WSL2**。
> ⚠️ **真机状态（2026-08-19）：BM1684 真机暂不在身边**——Phase 0 实测、Phase 3 真机联调、
> Phase 5 验证全部**阻塞**；无真机可推进项（构建冒烟、资源集、模型转换、纯代码、文档）优先执行。

---

## 1. 背景与现状（已核实）

CosmoEdge 当前支持两条 Sophon 产品线：**BM1688** 与 **CV186X**（均为 BM1684X 家族 SoC），
外加 Rockchip（RK3576 / rv1126b）与 x86。Sophon 相关代码结构如下：

| 层 | 位置 | 说明 |
| --- | --- | --- |
| 推理后端 | `src/nn/device/sophon/` | BMRT 推理（`sophon_net_node.cc`）、预处理节点、YOLO 解码、Qwen3-VL/Qwen3.5 VLM |
| 媒体后端 | `src/media/VideoDecoderSophon.cc`、`VideoFrameProcSophon.cc` | bmvpu 硬解、VPSS/VPP + bmcv 缩放/转码/JPEG |
| 内存管理 | `src/mem/`（`AllocatorSophon.cc`、`DeviceContextSophon.cc`） | `bm_malloc_device_byte_heap_mask(..., heap=3, ...)` 设备内存池 |
| 设备 SDK（BM1688/CV186X） | `3rd/libsophon-0.4.11/` | libsophon 0.4.11（bmlib / bmrt / bmcv / bmvd / bmvenc 等） |
| 设备 SDK（BM1684，本任务） | `3rd/libsophon-0.5.1/`（linux-aarch64，**已入库**） | bmlib / bmrt / bmcv / bmvideo / bmvpuapi / bmvppapi / bmjpuapi；**无 bmvd/bmvenc** |
| 模型封装 | `prebuild/model-guard-v2/` + `libcosmo_model_guard.so` | `.bmodel` 封装为 CENN 头 `model.nn` |

芯片身份（`chip_type`）在以下位置登记/校验，**全部需要扩展**：

1. `scripts/build.sh` — `-c <chip>` 白名单：`bm1688|cv186x`
2. `scripts/build_sophon_package.sh` / `build_sophon_package.ps1` — `--chip` 白名单
3. `CMakeLists.txt` — `COSMO_TARGET_CHIP` 正则：`^(bm1688|cv186x|rk3576|rv1126b|unspecified)$`
4. `src/util/NnBackendConstants.h` — `kSupportedChips[] = {"BM1688", "CV186X"}`（模型导入时校验 `config.json` 的 `chip_type`）
5. `data/resource/aiboxresource_<chip>/` — 平台资源集（算法、模型模板、i18n、layout 等）
6. `scripts/verify_sophon_open_benchmark_models.py`、`scripts/validate-public-v1.1-multistream-benchmark.mjs` — 平台映射
7. CI：`.github/workflows/nightly-build-test-sophon.yml`；构建环境：`docker-compose.sophon.yml`
8. 文档：`docs/guide/build.md`、`docs/guide/configuration.md`、`docs/reference/models.md`、
   `docs/tutorials/05-model-porting/`、`docs/development/backend.md`、`docs/development/ci.md`、README、CHANGELOG

模型产物约定：`.bmodel` 必须以 CENN 头封装进 `model.nn`，目录名 `prod_<TOKEN>_<alg>_<name>_<ver>/`，
`config.json` 内 `chip_type` 为兼容性门禁。**BM1684 的 bmodel 与 BM1688/CV186X 不可互换**
（不同编译目标、不同芯片架构），必须单独转换与验证。

### 1.1 SDK 版本家族（已核实，来源：sophon-demo 官方环境依赖表）

算能 SDK 按芯片家族分**两套发布线，互不通用**（sophon-demo 明确说明）：

- **BM1684 / BM1684X**：LIBSOPHON **>= 0.5.0**、TPU-MLIR >= 1.15、SOPHON-FFMPEG >= 0.7.3、SOPHON-OPENCV >= 0.7.3、SOPHON-SAIL >= 3.8.0
- **BM1688 / CV186X**：LIBSOPHON >= 0.4.9（仓库内 0.4.11 属此线）

因此本任务设备端 SDK 使用 `libsophon-0.5.1`（符合 >= 0.5.0），**不使用** `3rd/libsophon-0.4.11`。
0.5.1 库集合与 0.4.11 不同：0.4.11 含 `libbmvd.so`/`libbmvenc.so`（BM1688/CV186X 专用）；
0.5.1 含 `libbmvideo.so.0.14.0`/`libbmvpuapi`/`libbmvppapi`/`libbmjpuapi`，且**无 bmvd/bmvenc**。
0.5.1 设备端数据含 BM1684 固件（`bm1684_ddr.bin`、`bm1684_tcm.bin`、`bmtpu.ko`/`vpu.ko`/`jpu.ko`）。

**库更名映射（已对 .so 符号表实测确认，`bmvpu_*` 符号原样保留）：**

| 0.4.11 库 | 0.5.1 库 | 导出符号 |
| --- | --- | --- |
| `libbmvd.so`（链接名 `bmvd`） | `libbmvideo.so.0.14.0`（链接名 `bmvideo`） | `bmvpu_dec_*`（解码） |
| `libbmvenc.so`（链接名 `bmvenc`） | `libbmvpuapi.so.0.14.0`（链接名 `bmvpuapi`） | `bmvpu_enc_*`（编码） |

即 0.5.1 中 `bmvpu_dec_*` / `bmvpu_enc_*` **符号不变、仅宿主库更名**，`bm_vpudec_interface.h` 仍存在
（仅标记 deprecated）。顶层 `CMakeLists.txt:61-62` 已全局设置 `-Wno-deprecated-declarations`，
因此沿用 deprecated API **不会产生编译警告**。

---

## 2. BM1684 与现有平台的关键差异（SDK/API 层已实测核实，运行层待真机）

| 维度 | BM1688 / CV186X（0.4.11） | BM1684（0.5.1） | 影响 |
| --- | --- | --- | --- |
| 算力 | BM1688：16 TOPS INT8（+2 TOPS） | 17.6 TOPS INT8 | 单芯片算力相近 |
| FP16 | 支持（TPU-MLIR `bm1684x` 目标） | **硬件不支持 FP16，仅 INT8** | 模型必须 INT8 量化；精度需重新评估 |
| Transformer/VLM | 支持 Qwen3-VL 等 | **无 Transformer 引擎，16GB 内存** | VLM 不可用，构建期禁用 |
| bmodel 目标 | TPU-MLIR `--target bm1684x` | TPU-MLIR `--target bm1684` | 转换工具链参数不同 |
| 形态 | SoC（板载 VPU/VPSS） | **SM5 SoC 模块（已确认）** | VPU 硬解可用，媒体管线与 BM1688 同构 |
| 视频解码库 | `libbmvd.so`（链接名 `bmvd`） | **更名**为 `libbmvideo.so.0.14.0`（链接名 `bmvideo`），`bmvpu_dec_*` 符号原样保留 | 链接目标 `bmvd` → `bmvideo` |
| VPU 解码 API | `bm_vpudec_interface.h` 正常 | 同头文件存在但标记 `deprecated`；日志枚举改名（`BMVPU_DEC_LOG_LEVEL_ERR` → `BMVPU_DEC_LOG_LEVEL_ERROR`） | **决策：沿用 deprecated API**（符号仍在，改动最小；新 API 迁移列为技术债）；修枚举名 |
| 编码/JPEG | `libbmvenc.so`、`libbmjpeg` | **更名**为 `libbmvpuapi.so.0.14.0`（链接名 `bmvpuapi`）+ `libbmjpuapi` | 链接目标 `bmvenc` → `bmvpuapi`（首发不阻塞） |
| 视频处理 | VPSS/VPP + bmcv | VPP + bmcv（`libbmvppapi`/`libvpp`） | `VideoFrameProcSophon` 尺寸约束需实测调整 |
| 内存堆 | `heap_mask=3` 已验证 | 堆语义待设备实测 | `bm_malloc_device_byte_heap_mask` 参数需确认 |
| 芯片探测 | — | `bmrt_arch_info.h` 含 `BM1684`；`bm_get_chip_id` | 新增运行时芯片身份校验 |
| 设备端固件 | `bm1688_firmware*.bin`、`bmtpu.ko` | `bm1684_ddr.bin`、`bm1684_tcm.bin`、`bmtpu.ko`/`vpu.ko`/`jpu.ko` | 部署/安装脚本需带 BM1684 对应固件与驱动 |

### 2.1 历史遗留宏 `COSMO_NN_SOPHON_1684X`（重要发现）

在适配媒体层 0.5.x API 时发现：**NN 层源码里早已存在按 `COSMO_NN_SOPHON_1684X` 宏区分的分支**，
且该宏的语义恰好覆盖了 0.5.x SDK 的 API 变化。这是本任务最重要的代码级发现之一。

**发现过程**：`src/nn/utils/net_utils.cc`（16 处）、`src/nn/device/sophon/` 下 6 个文件
（`sophon_affine_crop_node.cc`、`sophon_dino_encode_node.cc`、`sophon_filter.cc`、
`sophon_image_guard.h`、`sophon_normalize_node.cc`、`sophon_sequence_node.cc`，共 32 处）
都含有如下模式的分支：

```cpp
#ifdef COSMO_NN_SOPHON_1684X
    bm_image_destroy(image);        // 按值传参（bm_image）
#else
    bm_image_destroy(&image);       // 按指针传参（bm_image*）
#endif
```

**宏语义（推断）**：`COSMO_NN_SOPHON_1684X` 表示"1684X 家族的传值风格 API"——即
BM1688/CV186X（0.4.x 时代 SDK）的 `bm_image_destroy` 已按值传参，而 BM1684 当时是
按指针传参，所以代码用该宏区分两支。

**关键事实**：
- 全仓 grep 确认 **`COSMO_NN_SOPHON_1684X` 从未被 `#define` 定义**（不在 CMakeLists、
  cmake/、任何头文件中）——历史遗留的空门控
- 因此所有 `#ifdef` 分支**从未启用**，一直走 `#else`（按指针 `&x`）分支——这与 0.4.11
  SDK 下 BM1684 的 `bm_image_destroy(bm_image*)` 签名一致，所以现有 BM1688/CV186X
  构建一直正常（注意：现有 Sophon 构建均用 0.4.11，走 `&x` 分支恰好正确）

**0.5.1 SDK 的变化**：`bm_image_destroy` 签名改为**按值传参**（`bm_image_destroy(bm_image)`），
BM1684 也被统一为传值 API。此时 `#else` 分支的 `&x`（传 `bm_image**`）编译失败。

**解决方案（已实施）**：`cmake/device.cmake` 在选中 0.5.x SDK 时定义
`COSMO_NN_SOPHON_1684X`，让所有历史分支自动切到按值调用——**无需逐个修改 NN 层 32 处**：

```cmake
if(EXISTS "${DEVICE_LIB_DIR}/libbmvideo.so")
    add_compile_definitions(COSMO_NN_SOPHON_1684X)
endif()
```

**宏统一决策**：媒体层新增的 0.5.x 适配（解码日志枚举 `BMVPU_DEC_LOG_LEVEL_ERROR`、
编码器 `bmvpu_enc_encode` 同步模型、`bmcv_padding_atrr_t` 拼写、`bm_image_destroy` 按值）
也全部挂到 `COSMO_NN_SOPHON_1684X` 门下（commit `9ff30b77`），代码库**统一一种 API 门控**，
语义为"libsophon 0.5.x 家族的 API 风格"，与 NN 层既有分支一致。

---

## 3. 工作分解（Phase 0 → Phase 6）

### Phase 0 — 前置调研与决策（部分已完成）

已确认：
- [x] 目标硬件形态：SM5 SoC 模块（VPU 硬解可用）
- [x] 设备 SDK：libsophon-0.5.1（linux-aarch64，符合 sophon-demo 对 BM1684 的 LIBSOPHON >= 0.5.0 要求）
- [x] SDK/API 层差异核实：库集合不同、`bmvpu_dec_*`/`bmvpu_enc_*` 符号实测仍导出（宿主库更名）、日志枚举改名
- [x] SDK 入库：拷贝为 `3rd/libsophon-0.5.1/`（随仓库走，CI 可复现；Phase 1 执行拷贝）
- [x] VPU 解码 API：**沿用 deprecated `bmvpu_dec_*`**（符号在 `libbmvideo`，顶层已 `-Wno-deprecated-declarations`，改动最小；真机验证后再评估新 API 迁移）
- [x] 转换环境：**WSL2（Ubuntu 22.04+，x86_64）**，TPU-MLIR >= 1.15（pip 安装）；Docker Desktop 为备选
- [x] 首发范围：仅单路检测+分类

待真机执行（⚠️ 真机暂不在身边，以下全部阻塞）：
- [ ] 在设备上确认 libsophon 运行时版本（`/opt/sophon/libsophon` 或设备预装 0.5.x）与固件加载方式（0.5.1 `data/load.sh`）
- [ ] 实测 `bm_malloc_device_byte_heap_mask(..., heap=3, ...)` 堆语义与 `bm_get_chip_id` 返回值
- [ ] 实测 VPP/VPU 可用性与 `VideoFrameProcSophon` 尺寸约束（`kSophonVppMinDimension` 等）是否适用
- [ ] 确认基准模型（YOLOV8n 检测、helmet 分类）的 ONNX 源与量化校准数据来源
- [ ] 产物：`docs/development/bm1684-facts.md`（实测事实清单，区别于本计划）

### Phase 1 — 构建与打包链路（无真机可并行开发）

- [x] **SDK 接入**：已拷贝 `libsophon-0.5.1` 为 `3rd/libsophon-0.5.1/`（data/include/lib + Apache 2.0 LICENSE，
  符号链接已展开为实体，无 bin 测试工具，与 0.4.11 一致）；`cmake/device.cmake` 已参数化——引入
  `COSMO_LIBSOPHON_ROOT`（CACHE，默认按 chip：`bm1688/cv186x` → `3rd/libsophon-0.4.11`，`bm1684` → `3rd/libsophon-0.5.1`），
  支持外部路径注入（如 `E:\Gdsc\projects\dev\devlibs\linux-aarch64\libsophon-0.5.1`）
- [x] `src/media/CMakeLists.txt` 链接目标：无需改动——`device.cmake` 将链接名 `bmvd`/`bmvenc` 映射到
  `libbmvideo.so`/`libbmvpuapi.so`（0.5.1 按 `EXISTS` 探测自动选择），`bmvpu_dec_*`/`bmvpu_enc_*` 符号已实测一致
- [x] `src/media/VideoDecoderSophon.cc:45`：解码日志枚举 `BMVPU_DEC_LOG_LEVEL_ERR` → 按
  `COSMO_NN_SOPHON_1684X`（device.cmake 按 SDK 定义）条件编译选择 `ERROR`（0.5.1）/`ERR`（0.4.11）；
  编码枚举两个 SDK 完全一致，无需改动（已对比头文件确认）
- [x] `scripts/build.sh`：白名单与 Usage 加入 `bm1684`；资源目录映射 `aiboxresource_bm1684`
- [x] `scripts/build_sophon_package.sh` 与 `.ps1`：`--chip` 白名单与 Usage 加入 `bm1684`
- [x] `CMakeLists.txt`：`COSMO_TARGET_CHIP` 正则加入 `bm1684`
- [x] `src/util/NnBackendConstants.h`：`kSupportedChips` 加入 `"BM1684"`（导入门禁）
- [x] `test/test_package_profile.py`：同步更新芯片白名单断言
- [x] 本机静态验证：bash 语法检查通过；`test_package_profile.py` 15 用例全部通过
- [x] **Windows Docker Desktop 冒烟构建成功**（2026-08-19）：`build_sophon_package.ps1 -Chip bm1684` 产出
  `build_output/public-runtime/bm1684/cosmo-V1.1.0-8f08fb02....tar.gz`（66.7MB，`TARGET_CHIP=bm1684`）；
  bm1688 基线回归同样通过（60MB）——`device.cmake` 0.5.1 SDK 选择、媒体层链接
  `bmvd→bmvideo`/`bmvenc→bmvpuapi`、`COSMO_NN_SOPHON_1684X` 条件编译全部验证生效
- [x] `docker-compose.sophon.yml`：现有构建镜像 `ghcr.io/cosmo-wander-ai/cosmo_edge-build-env_sophon:v1`
  已验证可完成 bm1684 交叉编译与打包（无需新镜像）
- [x] **Windows 构建链路修复**（既有问题，本次首次 Windows 构建暴露）：
  - `build_sophon_package.ps1`：docker stderr（`Container Creating` 等进度信息）合并到 stdout，
    避免 PowerShell 5.1 `NativeCommandError` 误报失败（`Invoke-Docker` 与 compose run 两处 `2>&1`）
  - `sync-source-volume.sh`：新增 LF 规范化（`tr -d '\r'` + 权限保留），修复 Git for Windows
    CRLF 检出导致 3rd configure 脚本 shebang 无法执行（`No such file or directory`）的问题
    ——覆盖 configure/config/Configure/*.sh/*.pl/*.py 及任意 shebang 文件，不碰二进制
- [ ] CI（`.github/workflows/nightly-build-test-sophon.yml`）：**待真机可用后加**（`test-sophon`
  job 需在 BM1684 真机上跑，真机不可用时矩阵会挂；当前保持 bm1688 单 job）

### Phase 2 — 资源集与模型转换（✅ 两个模型已转换并封装，真机验证延后）

- [x] 建立 `data/resource/aiboxresource_bm1684/`：已从 `aiboxresource_bm1688/` 复制
  `algorithm/ algorithm_template/ i18n/ layout/`（无芯片引用，原样复制）与 `model_template/`（14 个 json，
  `chip_type: "BM1688"` → `"BM1684"` 已替换，并剥离 UTF-8 BOM——BOM 曾导致 chip_type 扫描失效）
- [ ] 待处理：`model_template/dino.json` 的 `groundingdino_bm1688_fp16.bmodel` 文件名
  （VLM 相关，不在首发；届时按 BM1684 转换结果或随 VLM 门禁一并处理）
- [x] **模型转换完成**（TPU-MLIR 1.28.1，ONNX → INT8 bmodel，走 AGENTS.md 工作流）：
  - 转换环境：WSL2（Ubuntu 24.04）内 tpu_mlir 1.28.1（Python 3.10.19 via uv）+ 依赖
    （onnx==1.14.1 / numpy==1.24.3 / onnxsim==0.4.17 / torch CPU / pymlir / mlir.ir）
  - 修复的 tpu_mlir wheel 问题：`pymlir`/`mlir.ir` 不在 sys.path（`__init__.py` 注入
    `tpu_mlir/python`）；自带 `libc.so.6` 与系统 GLIBC 冲突（重命名 `.disabled`）；
    pymlir 0.5 带入的残留 `site-packages/mlir` 命名空间冲突（已移除）
  - **YOLOV8n**：`prod_BM1684_6047042_YOLOV8n_V1.0.0`（640×640，输出 [1,84,8400]，
    model.nn 4.04MB，CENN 封装，chip_type=BM1684）
  - **helmet**：`prod_BM1684_7486163_helmet_V1.0.0`（224×224，输出 [1,2]，
    model.nn 1.74MB，CENN 封装，chip_type=BM1684）
  - 校准：`data/test-video/Safety Helmet.mp4` 抽 30 帧（2fps, 640×640），经
    `run_calibration` 生成校准表（`chip=bm1684 mode=INT8`）
  - CENN 封装格式：80 字节头（magic `CENN` + 版本 + 头大小 + payload 大小）+ bmodel
- [x] 工具代码健壮性修复（commit 555b2426）：`agent_workflow._run` 容忍非 UTF-8
  子进程输出（errors=replace）；`check_onnx_model` 对 checker 拒绝 ir_version 10
  降级为警告（onnx==1.14.1 限制）
- [ ] 验证脚本更新：`scripts/verify_sophon_open_benchmark_models.py`、
  `scripts/validate-public-v1.1-multistream-benchmark.mjs` 加 `bm1684` 平台映射
  （待与基准发布一起处理）
- [ ] 产物验证（待真机）：SHA-256 记录与运行时推理验证

### Phase 3 — 运行时软件适配（核心代码，需真机联调）

- [ ] `src/util/NnBackendConstants.h`：`kSupportedChips` 加入 `"BM1684"`
- [ ] 新增运行时芯片探测：在 `SophonDevice`/`SophonContext` 初始化时调用
  `bm_get_chip_id` / `bmrt_get_chip_type`，与 `config.json chip_type` 比对，
  不一致则拒绝加载模型并给出明确日志（防止 BM1688 bmodel 误装载到 BM1684）
- [ ] 媒体管线（SM5 SoC 已定，走 VPU 硬解；**沿用 deprecated `bmvpu_dec_*`，已定案**）：
  - 适配 `VideoDecoderSophon.cc:45`：`BMVPU_DEC_LOG_LEVEL_ERR` → 0.5.1 的 `BMVPU_DEC_LOG_LEVEL_ERROR`（枚举成员名变化，已实测确认）
  - 链接目标：`src/media/CMakeLists.txt:87` → `bmvideo bmvpuapi`（符号已实测确认）
  - 技术债（非首发阻塞）：真机验证通过后，再评估迁移 0.5.1 新 VPU API（`bmvpu.h`/`bmvpu_types.h`/`bmvpu_logging.h`）
  - 调整 `VideoFrameProcSophon` 中 VPSS 专属尺寸约束（如 `kSophonVppMinDimension=16`）为 BM1684 VPP 语义（实测）
- [ ] 内存：验证 `bm_malloc_device_byte_heap_mask(..., 3, ...)` 在 BM1684 的堆语义；
  如不同，按芯片在 `AllocatorSophon`/`DeviceContextSophon` 选择堆掩码
- [x] VLM 门禁（commit `cb0cf195`）：`ModelServiceImpl::CheckModelValid` 在
  `COSMO_NN_SOPHON_1684X` 构建时拒绝 `model_type` 为 `qwen3vl`/`qwen3_5` 的模型导入
  （返回 `ModelFilePlatform`）——BM1684/BM1684X 无 Transformer 引擎，导入即拦截而非
  运行时失败；`ModelJsonInfo` 新增 `model_type` 字段
- [ ] 运行时芯片探测：**评估后跳过**——BMRT 加载 bmodel 时本身校验芯片匹配
  （BM1684 bmodel 载入 BM1688 会失败），`bm_get_chipid` 探测收益有限；真机验证时
  确认 BMRT 行为即可，不新增探测代码
- [ ] 检查 `sophon_yolo_decode_npu_node.cc` 等后处理的 INT8 反量化假设

### Phase 4 — 文档与发布物料

- [ ] `docs/guide/build.md`、`docs/guide/configuration.md`：芯片列表加 `bm1684`
- [ ] `docs/reference/models.md`：资源集表加 `aiboxresource_bm1684`
- [ ] `docs/tutorials/05-model-porting/model-porting.md`：转换目标加 BM1684（INT8）
- [ ] `docs/development/backend.md`、`docs/development/ci.md`：后端描述与 CI 矩阵更新
- [ ] `README(.zh-CN).md`、`CHANGELOG.md`：多平台列表加 BM1684
- [ ] 英文镜像文档同步（`docs/en/**`）

### Phase 5 — 验证与基准

- [ ] 真机冒烟：引擎启动、加载 BM1684 bmodel、**单路**推理（首发范围：仅单路检测+分类）
- [ ] 性能基准（可选，非首发阻塞）：YOLOV8n 与 helmet 在 BM1684 上的 FPS，与 BM1688 对比
- [ ] 交叉回归：确认 BM1688 / CV186X 构建与运行不受本次改动影响
- [ ] 按 AGENTS.md 记录：转换产物 SHA-256、运行引擎 SHA-256、验证记录（若需对外发布基准）

### Phase 6 — 验收标准（Definition of Done）

- [ ] `./scripts/build.sh -c bm1684` 可产出 `bm1684` 目标包，包内记录目标芯片
- [ ] 真机运行：检测 + 分类模型加载并出结果，`chip_type` 校验生效
- [ ] BM1688/CV186X 无回归；CI 矩阵含 BM1684
- [ ] 模型、媒体、内存、文档全部按上述清单更新，验证脚本通过

---

## 4. 风险与应对

| 风险 | 影响 | 应对 |
| --- | --- | --- |
| BM1684 无 FP16，仅 INT8 | 模型精度下降 | 量化校准 + 精度对比；必要时选更大模型架构补偿 |
| VLM（Qwen3-VL 等）不可用 | 功能差异 | **构建期禁用**（qwen3vl/qwen3_5）+ 前端隐藏 + 文档说明 |
| SDK 家族不同（0.4.11 vs 0.5.1 不兼容） | 编译失败/链接错误 | 已确认改用 0.5.1；device.cmake 参数化；媒体链接目标按符号归属调整 |
| VPU 解码 API deprecated + 枚举改名 | 编译失败/API 风险 | **已缓解**：符号仍导出（`libbmvideo`）、顶层已 `-Wno-deprecated-declarations`；只需改枚举名与链接目标；新 API 迁移列为技术债 |
| 堆掩码/VPP 语义差异 | 内存分配失败 | 真机实测，按芯片选择参数 |
| bmodel 互不可换 | 误装载风险 | 运行时芯片探测 + 导入门禁（`IsSupportedChip`） |

---

## 5. 未决问题

⚠️ **真机暂不可用**（2026-08-19）：Phase 0 实测 / Phase 3 联调 / Phase 5 验证阻塞，恢复后按顺序执行。

已确认（用户提供）：
1. ✅ 目标设备：SM5 SoC 模块（媒体管线走 VPU 硬解）
2. ✅ 有可用 BM1684 开发设备（Phase 0/3/5 可上真机）
3. ✅ 首发范围：仅单路检测+分类（VLM 构建期禁用；多路压测不在首发）
4. ✅ 转换环境：WSL2（Ubuntu 22.04+，TPU-MLIR >= 1.15 pip 安装）；Docker Desktop 备选

本轮已定案（用户确认 + 符号表实证）：
5. ✅ SDK 入库：拷贝为 `3rd/libsophon-0.5.1/`（随仓库走，CI 可复现；Phase 1 执行拷贝与 device.cmake 参数化）
6. ✅ VPU 解码 API：沿用 deprecated `bmvpu_dec_*`（`bmvpu_dec_*`/`bmvpu_enc_*` 符号实测仍导出；新 API 迁移作为真机验证后的技术债）
7. ✅ 转换环境：**WSL2（Ubuntu 22.04+）**；Docker Desktop 备选

遗留（不阻塞实施）：
8. ✅ SDK 已入库 `3rd/libsophon-0.5.1/`（约 106 MB：data 31.6 + include 1.7 + lib 72.8；
   LICENSE 沿用 Apache 2.0——与 0.4.11 同源 bmlib 许可；体积与 0.4.11 同量级，已无阻塞）
9. 真机验证后评估 0.5.1 新 VPU API 迁移（技术债）
