---
title: 构建指南
description: x86 Docker、Sophon、Rockchip 和 CPU 测试构建路径。
prev:
  text: 文档首页
  link: /
next:
  text: RK3576 / RKNN 集成
  link: /guide/rk3576-rknn-development
---

# 构建指南

本文只记录当前仓库中已经确认的构建路径。历史文档或旧脚本中出现过、但当前仓库无法验证的路径，不作为公开支持路径。

> **💡 Docker Compose 版本提示**
> 本文档统一使用最新的 Docker Compose V2 命令格式 (`docker compose`)。如果你使用的是旧版 Docker 环境（如自带独立的 V1 插件），请将文中的 `docker compose` 替换为带横杠的 `docker-compose`。
> Linux 上也可以直接使用 `./scripts/docker-compose.sh`；它会检测 Compose V2/V1，
> 并在当前账号无权访问 Docker daemon 时明确请求一次 `sudo`。若不希望使用 sudo，
> 请先按 Docker 官方方式授予当前账号 daemon 访问权限并重新登录。

## 构建路径总览

| 路径 | 用途 | 是否启动服务 | 输出 |
| --- | --- | --- | --- |
| x86 Docker 开发运行环境 | 首次体验、开发评估、生成 x86 发布包 | 是 | `build_output/` |
| macOS Docker Preview | Apple Silicon 上体验单路 x86 工作流 | 是 | `build_output/macos-x86/` |
| Sophon Open 构建 | 交叉编译可安装的公开源码构建包 | 否 | `build_output/public-runtime/<chip>/` |
| Rockchip 构建 | 使用共享 RKNN 构建镜像为 RK3576 或 RV1126B 交叉编译 | 否 | `build_output/<chip>/` |
| CPU 测试构建 | 构建 `cosmo-tests` | 否 | `build_cpu/cosmo-tests` |

## x86 Docker 开发运行环境

Linux:

```bash
docker compose -f docker-compose.x86.yml up -d --build
```

Windows (PowerShell/CMD):

```powershell
docker compose -f docker-compose.x86.windows.yml up -d --build
```

Apple Silicon macOS (Preview):

```bash
./scripts/macos-docker-preview.sh doctor
./scripts/macos-docker-preview.sh up
```

Mac 路径显式运行 `linux/amd64`，使用独立卷并只绑定回环地址。它不启用
Model Guard，也不构成原生 arm64 或 NPU 性能证据。完整说明和验收范围见
[macOS Docker Preview](./macos-docker-preview.md)。

该路径来自：

- `docker-compose.x86.yml` (Linux)
- `docker-compose.x86.windows.yml` (Windows)
- `docker-compose.x86.macos.yml` (Apple Silicon macOS Preview)
- `Dockerfile.x86`
- `scripts/build_cpu.sh`

已确认构建参数：

| 参数 | 值 |
| --- | --- |
| `COSMO_TARGET_ARCH` | `x86_64` |
| `COSMO_NN_USE_SOPHON_BACKEND` | `OFF` |
| `COSMO_NN_USE_CPU_BACKEND` | `ON` |
| `COSMO_ENABLE_OPENH264` | `ON` |
| `COSMO_DEV_MODE` | `ON` |
| `RESOURCE_DIR` | `data/resource/aiboxresource_x86` |

构建完成后：

- Web 控制台通过 `http://127.0.0.1:8080` 访问。
- 发布包和构建产物导出到 `build_output/`。
- 运行数据保存在 Docker volume `cosmo-x86-data`。
- 资源目录挂载到 Docker volume `cosmo-x86-app-resource`。

## Sophon 构建产物

公开构建入口默认使用
`COSMO_MODEL_GUARD_BUILD_PROFILE=public-runtime`：

Linux / Bash：

```bash
# 省略型号时默认 bm1688
./scripts/docker-compose.sh -f docker-compose.sophon.yml run --rm cosmo-sophon-package

# 显式选择型号
./scripts/docker-compose.sh -f docker-compose.sophon.yml run --rm cosmo-sophon-package --chip bm1688
./scripts/docker-compose.sh -f docker-compose.sophon.yml run --rm cosmo-sophon-package --chip cv186x
```

Windows PowerShell：

```powershell
# 省略型号时默认 bm1688
.\scripts\build_sophon_package.ps1

# 显式选择型号
.\scripts\build_sophon_package.ps1 -Chip bm1688
.\scripts\build_sophon_package.ps1 -Chip cv186x
```

两个支持的配置使用相互隔离的输出目录：

| 配置 | 用途 | 输出目录 | 部署状态 |
| --- | --- | --- | --- |
| Open（内部配置 `public-runtime`，默认） | 使用仓库内运行时 SDK 完成公开的 aarch64 编译、链接、打包和测试验证 | `build_output/public-runtime/<chip>/` | 明文模型，无需设备授权 |
| Protected（内部配置 `production-release`） | 在受控环境中使用完整正式 SDK 和设备授权工具构建 | `build_output/production-release/<chip>/` | 加密模型，需要设备授权 |

每个芯片目录同时包含 `TARGET_CHIP` 和 `SHA256SUMS`，压缩包内部还包含
`share/cosmo/target-chip.txt`。即使两个芯片当前选择的公开模型字节一致，完整安装包也
必须具有不同哈希；必须从各自目录取包，不能用相同包名推断芯片兼容性。

Compose 会在首次构建时按 `package-lock.json` 串行填充 npm 缓存，随后完全离线安装；
同一工作目录中的 BM1688、CV186X 与 RK3576 构建共享该缓存。这样可规避 npm 10.2
在部分网络上建立大量 CDN 连接后无法退出的问题。删除 Compose 卷会触发重新填充。

两种配置都生成 `cosmo-V<版本号>-<32位md5>.tar.gz`。同一格式既可以在 main
分支部署的管理页面升级，也可以在后续任意版本继续升级。应用包不签名；两种配置
只在模型是否加密以及是否包含 `cosmo-model-provision` 上有区别。

### 将 Sophon 构建包交给部署流程

构建完成后，先在对应芯片目录核对目标标记和 SHA-256：

```bash
chip=bm1688 # 或 cv186x
cat "build_output/public-runtime/${chip}/TARGET_CHIP"
(cd "build_output/public-runtime/${chip}" && sha256sum -c SHA256SUMS)
```

SSH 安装、Web 升级、恢复边界和重启后的版本验收统一见[部署指南](./deployment.md#ssh安装路径)。
构建指南不重复维护设备安装命令，避免构建入口和部署流程独立演进后出现两套口径。

维护人员在包含完整 Guard SDK 和授权工具的受控环境中使用一条命令构建：

```bash
COSMO_MODEL_GUARD_BUILD_PROFILE=production-release \
  ./scripts/docker-compose.sh -f docker-compose.sophon.yml run --rm cosmo-sophon-package --chip cv186x
```

上例构建 CV186X Protected 包；构建 BM1688 时把末尾型号改为 `bm1688`，或省略型号。

如果受控 SDK 中缺少 `cosmo-model-provision`，Protected 构建会直接失败。
受控生产 SDK 应放在宿主机的
`build_output/model-guard-sdk-production/`，该目录通过现有 Compose 挂载进入
容器且不会提交到 Git。Protected 构建会自动优先使用它；Open 构建不受影响。

Protected 的 CPack 产物本身就是管理页面接受的升级包，不再需要离线应用签名步骤。
Guard 设备证书和模型加密秘密仍属于受控输入，不得写入公开仓库。

该路径来自：

- `scripts/docker-compose.sh`（Linux/macOS：选择 Compose V2 或 V1，并处理 Docker 权限）
- `docker-compose.sophon.yml`
- `scripts/build_sophon_package.sh`
- `scripts/build_sophon_package.ps1`（Windows：构建前自动修复 `.so` 软链接）
- `scripts/build.sh`

已确认行为：

- 基础镜像使用预先构建的 GHCR 镜像：`ghcr.io/cosmo-wander-ai/cosmo_edge-build-env_sophon:v1`（统一的编译环境，加速了本地启动时间）。
- Docker Compose 接受芯片型号参数：`cosmo-sophon-package --chip bm1688` 或
  `cosmo-sophon-package --chip cv186x`。省略 `--chip` 时默认使用 `bm1688`。
- `scripts/build_sophon_package.sh` 把芯片型号传给 `scripts/build.sh -T -c <型号>`；
  `build.sh` 再选择对应资源目录，用户无需传入模型路径。
- 只导出构建产物，不启动服务。
- 芯片型号不会改变 CPack 或 MD5 重命名逻辑；输出隔离到
  `build_output/<profile>/<chip>/`，包名仍为 `cosmo-V<major>.<minor>.<patch>-<md5>.tar.gz`，
  旁边的 `TARGET_CHIP` 与 `SHA256SUMS` 用于阻止同名产物混用。

## Rockchip 构建产物

Rockchip 构建入口使用一个固定 digest 的 GHCR 镜像。aarch64 工具链与 RKNN Runtime
只维护一份；RK3576 和 RV1126B 分别选择隔离的 MPP/RGA 根目录。RKLLM Runtime v1.3.0
固定到官方 commit，但只对 RK3576 构建强制启用并进入发布包：

```bash
./scripts/docker-compose.sh -f docker-compose.rockchip.yml pull cosmo-rockchip-package

COSMO_TARGET_CHIP=rk3576 ./scripts/docker-compose.sh \
  -f docker-compose.rockchip.yml run --rm cosmo-rockchip-package
sha256sum build_output/rk3576/cosmo-*.tar.gz

COSMO_TARGET_CHIP=rv1126b ./scripts/docker-compose.sh \
  -f docker-compose.rockchip.yml run --rm cosmo-rockchip-package
sha256sum build_output/rv1126b/cosmo-*.tar.gz
```

该入口已确认：

- 在 `linux/amd64` 构建容器中执行 aarch64 交叉编译。
- 构建前清理 `build_rknn/`，再通过同一入口调用 `scripts/build_rknn.sh -c <chip> -T`。
- 镜像内的 builder lock 必须与源码 checkout 完全一致，目标平台 profile 的媒体运行时
  也必须命中该 lock，否则拒绝构建。
- RV1126B 的 MPP/RGA sysroot 使用源码版本、ELF 属性与文件哈希封印；构建路径已映射，
  可重复构建不会把一次性工作目录写进二进制。
- RV1126B 包同时携带固定上游 commit 中的 MPP Apache-2.0/MIT 与 RGA `COPYING`
  许可证文本，并通过目标打包策略校验。
- RK3576 包必须包含 `lib/librkllmrt.so` 与许可证；RV1126B 包则必须不包含 RKLLM。
- 唯一发布包、`TARGET_CHIP`、`MEDIA_RUNTIME_PROFILE` 和 `SHA256SUMS` 输出到
  `build_output/<chip>/`，不启动应用服务。
- 同时生成 `build_rknn/cosmo-tests`、`cosmo-rknn-backend-smoke` 和
  `cosmo-rknn-fastpath-qualify` 三个 aarch64 验证程序。
- 使用宿主机网络解析构建依赖，但不发布应用端口。

RV1126B 的 `include` 构建从平台 profile 的 artifact manifest 自动生成并验证 overlay。
仓库默认清单归档两个 AGPL-3.0 社区示例模型，只用于公开示例和 CI，不属于商业模型交付；
包内保留 `resource/model-bundle.json` 和模型许可证。商业或专有模型通过
`COSMO_RKNN_ARTIFACT_MANIFEST` 指向 ignored 任务目录中的独立清单，并接受同样的芯片、
大小和哈希审计。`preserve` 仍可只验证代码、工具链和包结构，但不能替代带模型的设备验收。
旧 `docker-compose.rk3576.yml` 仅作为薄兼容入口保留。

### Debian 11（bullseye）构建流与锁文件切换

生产 Rockchip 设备运行 Debian 11（glibc 2.31）。上面 Ubuntu 22.04（glibc 2.35）
镜像构建的包无法在设备启动（缺少 `GLIBC_2.34/2.35`），因此仓库提供第二条
Debian 11 构建流，从零（from-scratch）组装工具链与三方依赖：

```bash
# 方案一（Ubuntu 22.04 / glibc 2.35，默认锁）
./scripts/docker-compose.sh -f docker-compose.rockchip.yml build
COSMO_TARGET_CHIP=rk3576 ./scripts/docker-compose.sh \
  -f docker-compose.rockchip.yml run --rm cosmo-rockchip-package

# 方案二（Debian 11 / glibc 2.31，bullseye 锁；RK3588 仅此流可用）
docker compose -f docker-compose.rockchip-bullseye.yml build
COSMO_TARGET_CHIP=rk3588 docker compose -f docker-compose.rockchip-bullseye.yml \
  run --rm cosmo-rockchip-bullseye-package
sha256sum build_output/rk3588/cosmo-*.tar.gz
```

两条流的差异与切换：

- 锁文件相互隔离：方案一使用 `config/rockchip-build/builder-lock.json`
  （rk3576=legacy `rk3576-build-env-v1`，rv1126b=repro `mpp-1.1.0-rga-1.10.6-repro-v1`），
  方案二使用 `config/rockchip-build/builder-lock.bullseye.json`
  （三芯片共享 bullseye 媒体 profile `mpp-1.1.0-rga-1.10.6-bullseye-v1`）。
  `scripts/build_rockchip_package.sh` 通过环境变量 `COSMO_BUILDER_LOCK_FILE`
  选择锁文件，镜像内的锁必须与源码 checkout 逐字节一致，否则拒绝构建。
- compose 分别固定各自的 `COSMO_PLATFORM_PROFILE_DIR` 与 `COSMO_BUILDER_LOCK_FILE`
  （bullseye 容器使用镜像内的 bullseye platform profile，不使用仓库默认的
  Ubuntu legacy profile）。
- bullseye 镜像完全 from-scratch：debian:bullseye 基镜像按 digest 冻结，
  arm64 交叉工具链、Node/Rust/RKNN 均按固定 digest 或 SHA-256 校验拉取，
  MPP/RGA 从 git 锁定 commit 交叉构建并 fan out 到 RK3588/RK3576/RV1126B 三份
  chip root，不依赖开发者机器上的 `.rk3588-build-cache` 之类的遗留文件。
- FFmpeg：bullseye 流从官方源码交叉编译 **FFmpeg 4.4.6（LGPL）**（与
  x86_64/Sophon prebuild 完全同版本，soname 58 不变），configure
  使用 `--disable-gpl`，不启用 libx264 等 GPL 组件；打包的 `libavcodec`
  内嵌 "LGPL version 2.1 or later" 身份，与随包 `COPYING.LGPLv2.1` 一致，
  无需任何二进制级豁免。不要用 Debian 官方 ffmpeg 二进制包（其构建启用
  GPL 组件，会导致许可证不匹配）。
- RKLLM 1.3.0 只对 `rkllm_required=true` 的目标打包（方案一仅 RK3576，
  方案二含 RK3588），并只携带其 `share/licenses/rkllm/LICENSE`。

常见失败：

- `builder image lock does not match this source checkout`：容器内的 lock 与
  checkout 不一致。方案二请使用 `docker-compose.rockchip-bullseye.yml`
  （固定 bullseye lock），不要混用 Ubuntu 镜像。
- glibc gate 失败（`GLIBC_2.34` 之类）：包内混入了 Ubuntu 构建的产物；
  确认使用 bullseye 镜像与 bullseye lock 重建。
- FFmpeg 校验失败 `not identified as LGPL-2.1-or-later`：镜像内 FFmpeg 来源
  不是自建 LGPL 构建；重建 bullseye 镜像即可。

包校验与模型 provenance 见 `test/test_package_profile.py`
（`model.rknn.build.json` 引用的 spec 必须存在且哈希逐字节一致），板端证据边界见
[RK3576 / RKNN 集成指南](./rk3576-rknn-development.md)。

## CPU 测试构建

```bash
bash scripts/build_cpu_test.sh
```

该脚本使用 CPU 后端和 `BUILD_TESTS=ON` 配置 CMake，生成：

```sh
build_cpu/cosmo-tests
```
