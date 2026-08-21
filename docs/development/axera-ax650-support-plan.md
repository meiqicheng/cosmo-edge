# AX650N 芯片支持开发计划

> 分支：`feature/axera-ax650-support`
> 基线：`feature/multi-chip-support`（0a1e7ba9，含 BM1684 + RK3588 支持）
> 目标：在现有 Sophon（BM1688/CV186X/BM1684）与 Rockchip（RK3576/RK3588）基础上，
> 新增爱芯元智（AXERA）**AX650N**（SoC 模块）支持。
> 状态：**实施中**（Phase 0/1/2/3/5 已完成；Phase 6/7 待办）
> 已确认决策（2026-08-20）：
> - **AXCL SDK**：暂无现成 SDK，需从爱芯官方渠道获取（HuggingFace/官网/axcl-runtime 仓库）
> - **模型转换环境**：WSL2 Ubuntu + Python 3.10（复用 BM1684 TPU-MLIR 已验证环境）
> - **媒体后端**：`COSMO_MEDIA_USE_AXERA_BACKEND` 已启用——AX650 VDEC/VENC/IVPS 硬件媒体（MPP 风格，失败自动回退 CPU/FFmpeg）
> - **Pulsar2**：v6.0 独立包（lite）已通过 Magnetar 安装脚本从 ModelScope 下载，
>   安装于 WSL `~/.cache/magnetar/pulsar2/6.0/`（详见第 9 节）
> ⚠️ **真机状态**：AX650N 真机/开发板暂未确认在身边——Phase 0 实测、Phase 5 真机联调
> 阻塞；无真机可推进项（构建冒烟、资源集、模型转换、纯代码、文档）优先执行。

---

## 1. 背景与目标

### 业务目标

在不影响现有 Sophon / Rockchip / x86 稳定版的前提下，让同一份 CosmoEdge 代码能够
编译、打包、部署并在 **爱芯元智 AX650N**（SoC 模块）上完成推理，达到与其他平台
相同的验收门槛。首发范围参照 BM1684：**单路检测 + 分类**。

### 平台事实（已核实，来源：AXERA 官方 SDK/仓库/文档，均本地可用）

| 项 | 值 | 本地位置 |
| --- | --- | --- |
| 芯片 | 爱芯元智 **AX650N**（AX650A/AX650N 同族，终端计算 SoC） | — |
| 开发板 | Sipeed **AXera-Pi Pro**（基于 AX650N）；另有 AX650_emmc_ubuntu_rootfs 镜像 | `hardware/Axera爱芯元智/AX650_emmc_ubuntu_rootfs_desktop_V3.10.2_*.axp` |
| **设备 SDK** | **AX650 SDK V3.10.2**（SoC 模式 = `package/msp/out`：`ax_engine_api.h`/`ax_engine_type.h`/`ax_sys_api.h` + `libax_engine.so`/`libax_sys.so`） | `hardware/Axera爱芯元智/AXERA-TECH/AX650-Community-Hub/AX650_SDK_V3.10.2_20260513151335` |
| 模型格式 | `.axmodel`（Pulsar2 从 ONNX 转换，INT8 量化；**原生格式，无需封装**） | — |
| 模型转换工具 | **Pulsar2 v6.0**（独立包 lite，自包含 `bin/pulsar2`，无需额外装 Python） | WSL `~/.cache/magnetar/pulsar2/6.0/`（安装位置见第 9 节） |
| 推理 API | SoC 模式：`AX_ENGINE_Init` → `AX_ENGINE_CreateHandle`（从内存加载）→ `AX_ENGINE_CreateContext` → `AX_ENGINE_GetIOInfo` → `AX_SYS_MemAlloc`（物理内存）→ `AX_ENGINE_RunSync` → `AX_ENGINE_DestroyHandle`/`Deinit` | `ax_engine_api.h` + `ax_sys_api.h`（SDK 内） |
| 官方示例 | `ax-samples`（BSD-3-Clause）`examples/ax650/ax_yolov8_steps.cc` 等 | `hardware/Axera爱芯元智/AXERA-TECH/ax-samples` |
| 媒体 API | `ax_ivps_api.h`（图像处理）、`ax_adec/ax_venc`（编解码）、`ax_isp_api.h` | SDK `msp/out/include/` |
| 烧录工具 | AXDL V1.23.33.1 | `hardware/Axera爱芯元智/AXDL_V1.23.33.1` |

**SoC 模式（AX650N 板端）已定案**：使用 `ax_engine_api.h`（`AX_ENGINE_*`）+ `ax_sys_api.h`
（`AX_SYS_MemAlloc`/`AX_SYS_MemAllocCached`，物理+虚拟地址，NPU DMA 经 phyAddr 访问）。
AXCL（`AXCL_ENGINE_*`）为 PCIe/axcl-runtime 统一栈，**本任务不使用**（SDK 内 axcl 目录保留参考）。

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
| 2 | `src/util/NnBackendConstants.h` | 三后端分支（SOPHON/RKNN/CPU），`kBackendType`/`kEngineType`/`kModelFileExt`/`kSupportedChips[]`/`kNewDirPrefix` | 新增 `COSMO_NN_USE_AXERA_BACKEND` 分支：`kBackendType="AXERA"`、`kEngineType="AX650N"`、`kModelFileExt=".axmodel"`、`kSupportedChips={"AX650N"}`、`kNewDirPrefix="prod_AXERA_"`（厂商级，同 SOPHON 的 `prod_SOPHGO_`/ROCKCHIP 的 `prod_ROCKCHIP_`） |
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
| **AXERA** | **`.axmodel`** | **原生（同 RKNN）** | 型号级 `prod_AX650N_`（内置）；厂商级 `prod_AXERA_`（web 新增） | `AX650N` |

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

### 阶段 0 物料（已确认，均本地可用）

- [x] **AX650 SDK V3.10.2**：`package/msp/out`（SoC 模式）——`ax_engine_api.h`/`ax_sys_api.h` + `libax_engine.so`/`libax_sys.so`，路径：`hardware/Axera爱芯元智/AXERA-TECH/AX650-Community-Hub/AX650_SDK_V3.10.2_20260513151335`
- [x] **Pulsar2 工具链**：自包含 `bin/pulsar2`（自带 python3 与依赖库，无需额外环境），路径：`hardware/Axera爱芯元智/AXERA-TECH/Pulsar2/ax_pulsar2_*_package`
- [x] **官方示例**：`ax-samples/examples/ax650/`（`ax_yolov8_steps.cc`、`ax_classification_steps.cc`、`middleware/io.hpp`），BSD-3-Clause
- [x] **烧录/镜像**：AXDL V1.23.33.1 烧录工具 + AX650_emmc_ubuntu_rootfs_desktop_V3.10.2 根文件系统镜像
- [ ] **AX650N 开发板/SoC 模块**：Sipeed AXera-Pi Pro 或客户 AX650N 模块（真机联调用，待确认）
- [ ] **设备端驱动**：SDK `msp/out` 需随板部署（驱动/固件在 rootfs 镜像内）

### 风险清单（已更新）

| 风险 | 影响 | 缓解 |
| --- | --- | --- |
| 真机不在身边 | 端到端验收阻塞 | 无真机可推进项优先执行 |
| Pulsar2 转换兼容性（onnxslim 前置） | 模型转换阻塞 | 官方要求转换前 `onnxslim` 优化（或 `--onnx_opt.enable_onnxsim true`），与 BM1684 流程可复用环境 |
| AX_SYS_MemAlloc 物理内存模型与 CosmoEdge 内存抽象不匹配 | 后端接入复杂度 | 官方示例 `middleware/io.hpp` 已提供标准做法；初期按 host 内存模型接入 |
| SDK 版本更新（V3.10.2 为当前本地版本） | 兼容性 | 以本地 SDK 为准，文档记录版本

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

## 6. 阶段 2 —— 构建矩阵与运行时库（已完成 ✅）

### 改动点（已落地）

1. `toolchains/aarch64-axera.toolchain.cmake`（新）：ARM GNU gcc-arm-9.2 交叉工具链
   （`/opt/axera/toolchain/gcc-arm-9.2`，WSL 内避免中文路径）；`CMAKE_SYSROOT` 锁定
   `aarch64-none-linux-gnu/libc` 自包含 sysroot，宿主 `/usr/include` 永不进入编译
2. `cmake/axera.cmake`（Phase 1 新增）：`COSMO_AXERA_ROOT` → `ax_engine`/`ax_sys` IMPORTED
3. `cmake/axera_media.cmake`（新）：`ax_comm`/`ax_vdec`/`ax_venc`/`ax_ivps` IMPORTED
4. `cmake/curl.cmake` + `cmake/curl_build_wrapper.sh`（新）：curl 交叉构建修复——
   `CMAKE_IGNORE_PATH` 排除宿主头、`ENABLE_THREADED_RESOLVER=OFF`、禁用 brotli/zstd、
   链接 zlib；wrapper 在构建前清洗 flags.make 的 `-I/usr/include` 并 patch
   `curl_config.h` 补交叉 sysroot 能力宏（HAVE_UNISTD_H/HAVE_SOCKLEN_T/...）
5. 顶层 `CMakeLists.txt`：媒体后端四选一（含 `COSMO_MEDIA_USE_AXERA_BACKEND`）；
   AXERA 分支追加 `COMMON_LIBS`（ax_engine/ax_sys/ax_comm/ax_vdec/ax_venc/ax_ivps）、
   `BUILD_RPATH`+`INSTALL_RPATH="$ORIGIN/../lib"`、
   `-Wl,-rpath-link,<SDK>/lib` + `-Wl,--allow-shlib-undefined`（容忍 SDK 内部依赖
   spdlog/axcl 与板载 libexif、vendor ffmpeg 的 GLIBC_2.33+ 符号）
6. 第三方库全部交叉编译成功：cryptopp/curl/event/fmt/glog/mp4v2/mqtt/openssl/
   sqlitecpp/srs/tokenizers/uSockets/uuid/zlib

### 交叉编译踩坑记录（Windows 宿主 + WSL）

- **中文路径**：`hardware/Axera爱芯元智/` 含非 ASCII，srs configure 参数被截断 →
  复制 SDK/工具链到 `/opt/axera/sdk`、`/opt/axera/toolchain`（无中文）
- **CRLF 脚本**：Windows 检出使 mp4v2/openssl/srs 内嵌脚本与 curl 头文件变 CRLF →
  `tr -d '\r'` 批量修复（含 srs `objs/Makefile`，否则 `-Wall` 变 `-W\ll`）
- **curl 宿主 include 污染**：curl configure 把宿主 `/usr/include` 写入 flags.make
  （宿主 glibc 2.36 的 `__attr_dealloc` 破坏 gcc 9.2）→ wrapper 构建前清理
- **openssl armcap.d.tmp 并行竞争**：`-j2` 串行化解决
- **prebuild symlink 被文本化**：Windows 检出把 git symlink（如 `libavcodec.so`）
  变成文本文件 → WSL 内重建符号链接（`tmp_fix_symlinks.sh`）
- **link-time SDK 依赖**：`libax_comm.so` 依赖 `libspdlog.so`/`libaxclrt_worker.so`
  （SDK 自带，`-rpath-link` 解析）；`libax_venc.so` 依赖 libexif、vendor ffmpeg
  需 GLIBC_2.33+（板载系统库，`--allow-shlib-undefined` 容忍）

### 验收标准（已达成）

- ✅ 交叉编译产物含 AXERA 运行时库与 `target-chip.txt` 芯片标签（`ax650n`）
- ✅ `install/bin/cosmo-engine` 为 aarch64 ELF（RPATH `$ORIGIN/../lib`）
- ✅ `install/lib/` 含全套 libax_* 与第三方运行时（124 个库文件）

### 构建镜像（参考 rk3576/sophon 的 Docker 打包体系）

AX650 SDK 为 NDA 许可，不能推送到公开 registry，因此 builder 镜像**本地构建**，
SDK/工具链通过 build context 注入（`sdk/axera/`，git-ignored，由 ps1 从 WSL 暂存）：

| 文件 | 作用 |
| --- | --- |
| `Dockerfile.axera` | ubuntu 22.04 + cmake/gcc/rustup(cargo+aarch64)/node/npm + `sdk/axera/msp-out`→`/opt/axera/sdk/msp-out` + `sdk/axera/gcc-arm-9.2`→`/opt/axera/toolchain/gcc-arm-9.2` |
| `docker-compose.axera.yml` | `cosmo-axera-package` 服务：挂载工作区 + build_output + npm cache，entrypoint=`build_axera_package.sh --chip ax650n` |
| `scripts/build_axera_package.sh` | 锁校验（`config/axera-build/builder-lock.json` vs 镜像内锁）→ symlink/CRLF 修复 → `build_axera.sh` → `verify_package_contents.py` → 输出 `build_output/ax650n/` |
| `scripts/build_axera_package.ps1` | Windows 入口：从 WSL `/opt/axera/...` 暂存 SDK/工具链 → 源码卷同步（ext4）→ restore-symlinks → compose build + run |
| `config/axera-build/builder-lock.json` | SDK/工具链路径与 `required_package_paths` 锁 |
| `config/axera/toolchain-lock.json` | SDK V3.10.2 / gcc-arm-9.2 / Pulsar2 6.0 版本锁 |

使用方式（Windows + Docker Desktop）：

```powershell
.\scripts\build_axera_package.ps1 ax650n
# 产物：build_output\ax650n\cosmo-V1.1.0.<md5>.tar.gz
```

纯 Linux 环境（WSL/Docker in WSL）：

```bash
docker compose -f docker-compose.axera.yml build
docker compose -f docker-compose.axera.yml run --rm cosmo-axera-package --chip ax650n
```

---

## 7. 阶段 3 —— AXERA 推理后端接入

### 推理 API（已由官方 `ax_engine_api.h` / ax-samples `ax_yolov8_steps.cc` 确认）

```cpp
// 初始化（进程一次）
AX_ENGINE_NPU_ATTR_T npu_attr{};
npu_attr.eHardMode = AX_ENGINE_VIRTUAL_NPU_DISABLE;
AX_ENGINE_Init(&npu_attr);

// 从内存加载 .axmodel
AX_ENGINE_HANDLE handle;
AX_ENGINE_CreateHandle(&handle, model_buffer.data(), model_buffer.size());
AX_ENGINE_CreateContext(handle);

// 查询 IO 元数据
AX_ENGINE_IO_INFO_T* io_info;
AX_ENGINE_GetIOInfo(handle, &io_info);  // pInputs[i].pShape/pName/eDataType/eLayout/nSize

// 分配 IO（SoC：AX_SYS_MemAlloc / AX_SYS_MemAllocCached，物理+虚拟地址）
AX_SYS_MemAlloc(&phy, &vir, meta.nSize, 128, (const AX_S8*)"cosmo");
memcpy(io_t->pInputs[i].pVirAddr, input_data, size);  // 输入写入

// 同步推理
AX_ENGINE_RunSync(handle, &io_data);
// 输出从 io_t->pOutputs[i].pVirAddr 读取

// 清理
AX_ENGINE_DestroyHandle(handle);
AX_ENGINE_Deinit();
```

### 新增 `src/nn/device/axera/`（参照 `rknn/` 结构 + 官方示例）

| 文件 | 职责 |
| --- | --- |
| `axera_device.cc/.h` | `TypeDeviceRegister<...>(DEVICE_AXERA)`；设备内存分配（宿主模型，`AX_SYS_MemAlloc` 物理内存或 host 内存） |
| `axera_node_creator.cc/.h` | `NodeCreatorRegister<AxeraNodeCreator>(DEVICE_AXERA)` |
| `axera_net_node.cc/.h` | 加载 `.axmodel`（内存 buffer）、`AX_ENGINE_*` 创建 handle/context/IO、同步 `RunSync`、结果拷贝 |
| `axera_preprocess_node.cc/.h` | 图像预处理（缩放/归一化/布局）——初期用 CPU 节点或 AXERA IVPS |
| `axera_yolov8_adapter.cc/.h` | YOLOV8 输出解码（参照 rknn_yolov8_adapter） |

### 与 RKNN 模式的异同

- **同**：`.axmodel` 原生格式无需封装；node creator/register 机制一致；`NnBackendConstants` 分支一致
- **异**：SDK API 面（`AX_ENGINE_*`/`AX_SYS_MemAlloc` vs `rknn_*`）；输入为物理内存（SoC）；IVPS（AXERA 硬件图像处理）是否启用

### 验收标准

- `device/axera` 编译通过（x86 冒烟 stub，不依赖 SDK 时的桩路径）
- 模型导入路径识别 `chip_type: AX650N`

---

## 8. 阶段 4 —— 媒体后端决策（已定案并落地 ✅）

**已实现（2026-08-21）**：`COSMO_MEDIA_USE_AXERA_BACKEND` 启用 AX650 专用硬件媒体：

- `VideoDecoderAxera`：AX_VDEC 组/通道（H264/H265 流模式、SDK 自动帧池），
  NV12→I420 拷贝出帧；MPP 初始化失败自动回退 `VideoDecoderCpu`（FFmpeg 软解）
- `VideoEncoderAxera`：AX_VENC 通道（H264 编码），失败回退 CPU 编码
- `VideoFrameProcAxera`：Phase-1 委托 CPU 实现（IVPS 硬件路径待真机验证后启用）
- `src/media/CMakeLists.txt`：`MEDIA_AXERA_SRC`（含 CPU 解码/编码/帧处理作为回退，
  `VideoFrameProcCpu.cc` 同时承载 STB_IMAGE_IMPLEMENTATION）
- `src/service/.../AcceleratorMetricsProviderAxera.cc`：AXERA 版 NPU 指标查询
  （当前返回中立值，待 runtime 暴露 npu_perf 计数器）

### 决策记录

- 初期：CPU 媒体 + AXERA 推理（无硬件媒体依赖，真机验证门槛最低）✅ 已确认
- 落地：AXERA 硬件媒体（VDEC/VENC 已实现 + CPU 回退兜底）✅ 2026-08-21
- 后续：IVPS 硬件帧处理路径（需真机验证）

---

## 9. 阶段 5 —— 模型转换与资源

### 9.1 工具链安装位置（已确认，WSL 内）

Pulsar2 是 AXERA 官方模型转换工具（ONNX → `.axmodel`，INT8 量化）；Magnetar 是
Pulsar2 的 AI Agent 自动化流水线（非编译器本体），其 `scripts/install_pulsar2.sh`
揭示了官方独立包的正确下载地址（ModelScope 国内镜像，无需 NDA）。

| 组件 | 位置（WSL 路径） | 说明 |
| --- | --- | --- |
| **Magnetar 仓库** | `/opt/axera/Magnetar` | git clone（gh-proxy 代理），HEAD `0a2160d`，仅脚本/流水线 |
| **Pulsar2 6.0 工具链**（解压后） | `~/.cache/magnetar/pulsar2/6.0/` | `bin/pulsar2` 可用，`version: 6.0 commit: 48520c11` |
| Pulsar2 下载缓存（tar.gz） | `~/.cache/magnetar/ax_pulsar2_6.0.tar.gz` | 716MB 源包（lite） |

Windows 侧没有工具链；本地 `hardware/Axera爱芯元智/AXERA-TECH/Pulsar2/ax_pulsar2_*_package`
为**不完整包**（缺 python3 运行时与 yamain 源码），不可用，以 WSL 内安装为准。

**常驻使用方式**：

```bash
export PULSAR2_HOME=/root/.cache/magnetar/pulsar2/6.0
/root/.cache/magnetar/pulsar2/6.0/bin/pulsar2 build --target_hardware AX650 ...
```

### 9.2 后续如何转换模型（可复现流程）

#### 步骤 1：获取 Pulsar2 工具链

Pulsar2 官方以 Docker 镜像（~3 GB，NDA 渠道）或**独立安装包（lite，~700 MB）**分发。
独立包从国内 ModelScope 镜像直接下载，无需 NDA：

```bash
# 下载并解压（示例 v6.0，约 716 MB）
mkdir -p ~/.cache/magnetar/pulsar2/6.0
curl -L -o /tmp/pulsar2.tar.gz \
  "https://modelscope.cn/models/AXERA-TECH/Pulsar2/resolve/master/6.0/ax_pulsar2_6.0_lite_package.tar.gz"
tar -xzf /tmp/pulsar2.tar.gz -C ~/.cache/magnetar/pulsar2/6.0 --strip-components=1

# 验证
~/.cache/magnetar/pulsar2/6.0/bin/pulsar2 version
# version: 6.0
```

> 参考：开源仓库 `AXERA-TECH/Magnetar`（MIT）的 `scripts/install_pulsar2.sh` 提供一键
> 安装（ModelScope 优先、hf-mirror 回退）；`pulsar2_reference_config.json` 是 build
> config 的权威参考。本仓库不内置 Pulsar2。

#### 步骤 2：准备校准数据

Pulsar2 量化需要 Numpy 校准集：`tar.gz` 内含 `{0000..NNNN}.npy`，**float32、带
batch 维、shape 与模型输入完全一致**（NCHW `[1,C,H,W]`）。可用真实图片（推荐 ≥ 8 张）
或合成样本（渐变/纹理/棋盘），合成样本足以跑通流程但真实数据量化精度更稳：

```python
import numpy as np, tarfile, os

np.random.seed(2026)
samples = [np.random.rand(640, 640, 3).astype(np.float32) * 0.4 for _ in range(8)]
npy = []
for i, img in enumerate(samples):
    arr = np.transpose(img, (2, 0, 1))[None, :, :, :].astype(np.float32)  # [1,3,640,640]
    p = f"images_{i:04d}.npy"; np.save(p, arr); npy.append(p)
with tarfile.open("images.tar.gz", "w:gz") as tar:
    for p in npy: tar.add(p, arcname=p)
```

#### 步骤 3：编写 build config 并转换

参考配置（`compiler.check=3` 会在编译期做 ONNX vs AXMODEL 余弦验证）：

```json
{
  "model_type": "ONNX",
  "target_hardware": "AX650",
  "npu_mode": "NPU3",
  "input_shapes": "images:1x3x640x640",
  "onnx_opt": { "enable_onnxsim": true, "model_check": true },
  "quant": {
    "calibration_method": "MinMax",
    "input_configs": [{
      "tensor_name": "images",
      "calibration_dataset": "/abs/path/to/images.tar.gz",
      "calibration_format": "Numpy",
      "calibration_size": 8,
      "calibration_mean": [0, 0, 0],
      "calibration_std": [255, 255, 255]
    }],
    "highest_mix_precision": false,
    "precision_analysis": false
  },
  "compiler": { "check": 3 }
}
```

执行转换（**`calibration_dataset` 必须使用绝对路径**，勿用 Docker 风格 `/workspace/...`）：

```bash
PULSAR2=~/.cache/magnetar/pulsar2/6.0/bin/pulsar2
"$PULSAR2" build \
  --target_hardware AX650 \
  --input model/yolov8n.onnx \
  --output_dir output \
  --config build_config.json
# 产物：output/compiled.axmodel
```

#### 步骤 4：转换后校验与入库

- 转换日志关注 `cosin simularity is X`，低于阈值（如 0.99）说明量化掉点，需换
  校准数据或 `calibration_method`（KL/Percentile/MSE）。
- `.axmodel` 原生入库，无需 CENN 封装；目录约定 `prod_<TOKEN>_<alg>_<name>_<ver>/`，
  其中内置模型用**型号级** token（`prod_AX650N_`，同 Sophon 的 `prod_BM1688_`/Rockchip 的 `prod_RK3576_`），
  web 新增模型用**厂商级** token（`prod_AXERA_`，同 Sophon 的 `prod_SOPHGO_`/Rockchip 的 `prod_ROCKCHIP_`）——
  AXERA 是品牌、AX650N 是型号，厂商级前缀保证后续新型号复用同一命名空间，
  实际芯片以 `config.json` 的 `chip_type` 为准；
  `config.json` 的 `chip_type` 写 `AX650N`、`file_name` 写 `model.axmodel`。
- 资源集放 `data/resource/aiboxresource_ax650n/`（模型 + 共享 algorithm/i18n/layout）。
- 转换通过 ≠ 设备验收：必须在 AX650N 板端验证 Runtime/驱动、数值、图片与视频链路、
  5 FPS 目标与稳定性，结果绑定产物 SHA-256。

### 9.3 实际执行记录（2026-08-20，已完成）

| 项 | 值 |
| --- | --- |
| Pulsar2 版本 | **v6.0**（独立包 lite，ModelScope 下载 716MB，commit `48520c11`） |
| 校准数据 | Numpy 格式 `tar.gz` 内含 `{0000..NNNN}.npy`（float32、带 batch 维、shape 与模型输入一致）；本次用 8 张合成样本（渐变/纹理/棋盘） |
| build config | `onnx_opt.enable_onnxsim: true`、`calibration_method: MinMax`、`npu_mode: NPU3`、`compiler.check: 3`（余弦验证）；`calibration_dataset` 用绝对路径 |
| YOLOV8n 产物 | `compiled.axmodel` 3.64MB（NPU 1 subgraph，4.37 GMACs），**cos 0.9999999956** |
| helmet 产物 | `compiled.axmodel` 1.60MB（NPU 1 subgraph），**cos 0.9999999987** |
| 源 ONNX | `aiboxresource_x86/models/`（YOLOV8n 12.3MB、helmet 5.5MB，与 BM1684 转换同一批） |

### 产物

- **YOLOV8n + 安全帽检测 `.axmodel`**（首发范围，参照 BM1684；模型文件体积/量化信息记录）
- 资源集 `data/resource/aiboxresource_ax650n/`：
  - `model_template/`（yolov8_det.json 等，参照 bm1684 资源集）
  - `models/prod_AX650N_<alg>_<name>_<ver>/config.json`（`chip_type: "AX650N"`）+ `model.axmodel`
  - `algorithm/`、`i18n/`、`layout/`（参照 bm1684 资源集结构）

### 验收标准

- Pulsar2 转换成功，`config.json` 的 `chip_type` 与 `NnBackendConstants.kSupportedChips` 匹配
- `.axmodel` 可直接被 `AX_ENGINE_CreateHandle` 加载（可在 x86 用 `pulsar2 run` 仿真验证）

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

1. ✅ SDK 定位（已确认：AX650 SDK V3.10.2，SoC 模式 `msp/out`，`ax_engine_api.h` + `libax_engine.so`）
2. ✅ 编写 `cmake/axera.cmake` + `scripts/build_axera.sh` + 白名单/常量扩展（Phase 1，已提交）
3. ✅ 用 Pulsar2 转换 YOLOV8n + 安全帽 `.axmodel`（`pulsar2 build --target_hardware AX650`，Phase 5，已提交）
4. ✅ 创建 `aiboxresource_ax650n/` 资源集（Phase 5，已提交）
5. 文档（model-porting AXERA 章节等）
