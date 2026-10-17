# RKNN 平台容器化部署运行

> 适用范围：RK3588 / RK3576 / RV1126B 目标板。
> 状态：**方案与事实已核对，落盘实施未完成**。标注「未实测」的条目必须先在目标板取证。
> 详细变更清单与代码行号见 `output/agent-runs/CONTAINER_IMAGE_DEPLOYMENT_PLAN.md`（2026-10 初版，其中两处事实错误已由本文纠正，见 §6）。

---

## 1. 现状：半镜像化

| 环节 | 现状 | 是否在镜像内 |
| --- | --- | --- |
| 编译 | `docker-compose.rockchip.yml` 在锁定的 GHCR amd64 builder 镜像里执行 `scripts/build_rockchip_package.sh` | 是，但源码用 `.:/workspace` 绑定挂载，不是镜像内 `COPY` |
| 产物 | `cosmo-V<版本>-<md5>.tar.gz`（应用树压缩包，**不是镜像**） | 否 |
| 部署 | 板上 `scripts/install.sh` 事务换树到 `/appfs/cosmo_wander/cwai_data` + 写 `cosmo.service` | 否，裸机 systemd |
| 运行 | systemd `ExecStart=.../scripts/inte_run_start.sh`，root | 否，裸机 |

x86 已有完整容器模式可作模板：`Dockerfile.x86`（builder+runtime 两段）、`docker-compose.x86.yml`、`scripts/docker-entrypoint.x86.sh`。

**关键取舍：tar.gz / Web OTA 通道必须保留**（升级流程与验收都依赖它），容器模式是**叠加**而非替换。

---

## 2. 目标形态

```
宿主 (amd64)  docker build -f Dockerfile.rockchip
                --target package  → /cosmo-packages/*.tar.gz
                --target runtime  → aarch64 rootfs + 完整应用树
             docker save → cosmo-rk3588-<ver>-<md5>.tar
                          │ scp
目标板 (aarch64)  docker load < cosmo-rk3588-*.tar
                docker compose -f docker-compose.rk3588-runtime.yml up -d
                  network_mode: host   # 组播发现 / RTMP / SRS / GB28181 必需
                  devices: /dev/mpp_service /dev/dri /dev/dma_heap …
                  volumes: /userdata/cwaiuserdata   # 持久化
                  restart: unless-stopped
```

---

## 3. 运行期硬依赖（容器必须透传）

| 依赖 | 消费者 | 说明 |
| --- | --- | --- |
| `/dev/mpp_service` | `src/media/VideoFrameCodecRockchip.cc`、`VideoDecoderRockchip.cc`、`VideoEncoderRockchip.cc` | VDEC / VENC / 硬件 JPEG |
| `/dev/dri/renderD*` | RKNN NPU | RK3588 实测为 `renderD129`；老内核可能是 `/dev/rknpu`，**未实测** |
| `/dev/dma_heap/*dma32` | `src/media/DmaHeap32Buffer.cc` | RGA2 / NPU 的 32 位地址分配；缺失会退回 CPU，容量数直接掉 |
| `/dev/rga` | librga | 节点名随内核而变，**未实测** |
| `/dev/videoN` | `src/media/UsbDemuxStrategy.cc` | USB 摄像头，按需 |
| `/sys/kernel/debug/rknpu/load` | `scripts/prepare_rknpu_metrics.sh` | 需要 `mount --bind`，失败已降级为 WARNING |
| `/dev/watchdog` | `src/platform/WatchDog.cc` | **建议不映射**，见 §5 |

---

## 4. 硬件用户态库：容器化的真正难点

**实测事实（2026-10-17，RK3588 Debian 11 板 192.168.111.3）**：

- 部署包 `lib/` 内只有 `librga.so`、`librkllmrt.so`、`librknnrt.so`，**没有 `librockchip_mpp.so`**。
- `ldd bin/cosmo-engine` 显示 `librockchip_mpp.so.1 => /lib/aarch64-linux-gnu/librockchip_mpp.so.1`，即**吃板端系统库**。
- 这不是遗漏，而是 `cmake/rockchip_media.cmake` 的有意决策，注释原文：
  > MPP must remain paired with the board kernel driver. RGA is packaged because
  > the project uses the newer im2d API and has qualified this userspace library
  > against the target RK3588 driver.

后果已经踩到：构建镜像里的 MPP 是 media SDK **1.1.0**（tag 1.1.0, 2026-08-11，`MPP_BUFFER_FLAGS_MASK=0x003f0000`），板端是 Debian 包 **librockchip-mpp1 1.5.0-1**（2022-09-23 树，`FLAGS_MASK=0xf0000`，不导出任何 `mpp_buffer_sync*` 符号）。两条发布线在 buffer 标志、缓存同步 API 上行为不同，已造成两次 SIGSEGV（详见 `output/agent-runs/CORE_MODE_COMPARE_RK3588_20261017.md` 与 `mpp_jpeg_probe/`）。

**因此容器化时不能假设「`LD_LIBRARY_PATH` 指向容器内 `lib/` 即自洽」**——容器里根本没有 MPP 库。三个选项：

| 选项 | 做法 | 代价 |
| --- | --- | --- |
| A（当前策略外延） | 容器从板端 bind-mount `/lib/aarch64-linux-gnu` 或把板端 `librockchip_mpp.so.1` COPY 进镜像 | 镜像与板强耦合，不可跨板分发 |
| B（打进镜像） | 把构建期 MPP 1.1.0 装进镜像 `lib/`，`LD_LIBRARY_PATH` 指向它 | 打破「MPP 与内核驱动配对」约束，需先验证 1.1.0 用户态与板上 `mpp_service` 驱动的 ioctl ABI，**未实测** |
| C（代码收敛） | 维持现状，只使用两条线都实现的 API 子集（本次修复走的就是这条路） | 需要持续审计，见 §6 |

本次崩溃修复采用的是 C：去掉 `MPP_BUFFER_FLAGS_CACHABLE`、缓存同步改走 `RockchipMppCompat.h` 的 no-op shim。C 的配套审计方法是把 `src/` 用到的 MPP 标识符与板端头文件做符号级比对（`output/agent-runs/mpp-headers-board/`）。

---

## 5. 权限与取舍

- `privileged: true` 最省事（`mount --bind` 指标桥、`iptables`、RGA/dma-heap ioctl、host netns sysctl 全通过），**验收阶段先用它**。
- 收紧路径：`cap_add: [NET_ADMIN, SYS_ADMIN]` + 显式 `devices:` + `sysctls:`。`SYS_ADMIN` 是否可去掉取决于 RGA/dma-heap ioctl 的真实需求，**未实测**。
- **不要**加 `cpus:` / `cpu_quota`：RK3588 容量瓶颈已知在 CPU decode，限核会直接压低通道数。
- **不要**映射 `/dev/watchdog`：容器喂宿主看门狗会把整机复位；用 `restart: unless-stopped` 覆盖进程崩溃场景即可。

---

## 6. 已纠正的两处事实错误（原方案 CONTAINER_IMAGE_DEPLOYMENT_PLAN.md）

1. 原文称「包内 `lib/` 已自带 `librockchip_mpp.so`/`librga.so`/`librkllmrt.so`，`LD_LIBRARY_PATH` 指向容器内 `lib/` 即自洽」。**实测：包内没有 `librockchip_mpp.so`**，MPP 走板端系统库。见 §4。
2. 原文把基础镜像列为「`debian:12-slim`，Debian 11 板子要换 `debian:11-slim`，未实测」。**已实测**：192.168.111.3 是 Debian 11 bullseye，`ldd --version` = glibc 2.31 ⇒ 该板运行时基础镜像必须 `debian:11-slim`；构建镜像本身是 Ubuntu 22.04 / glibc 2.35。

---

## 7. 风险与未实测清单

1. **板上有没有 docker/containerd** —— 未实测。没有则需先落 `docker-ce`/`containerd`。
2. **NPU 设备节点名**：老内核 `/dev/rknpu`，新 DRM 化内核 `/dev/dri/renderD129`（.3 板实测后者）。改前必须 `ls /dev | grep -E 'rknpu|renderD|mpp|rga|dma_heap'`。
3. **`/dev/rga` 与 `/dev/dma_heap/*dma32` 是否存在** —— 未实测。
4. **MPP 用户态 ↔ 内核驱动 ioctl ABI**（对应 §4 选项 B）—— 未实测。判据：跑一次 1080p 解码，看 `Insert` 计数增长 + RGA/MPP 计数非 0，不要只看 `baselineFps`。
5. **`bootId` 语义断裂**：Web 升级页靠 `/proc/sys/kernel/random/boot_id` 变化判定升级完成；`compose up -d` 重建容器**不改变** bootId ⇒ 该页面在镜像模式下失效。
6. **端口占用**：镜像模式与裸机 `cosmo.service` 不能并存（host 网络 + 80/1936/1985/18088/8000/9001 冲突）。切换前必须 `systemctl disable --now cosmo.service`。
7. **Protected / `production-release` 包**含 Guard SDK，**只能推私有 registry**。
8. **性能预期**：容器与裸机共用同一内核/驱动，无虚拟化开销；唯一变量是 cgroup 与设备直通正确性。应以「容器 vs 裸机同档位 FPS×通道矩阵」对照验收，而不是只看能起来。

---

## 8. 验证步骤（端到端）

1. 宿主：`docker build -f Dockerfile.rockchip --target package` → 校验 `build_output/rk3588/` 下 tar.gz + `SHA256SUMS` + `TARGET_CHIP`；`tar tzf | wc -l` 与基线对比。
2. 宿主：`docker build -f Dockerfile.rockchip-runtime` → `docker save`；`docker inspect` 确认 `Architecture: arm64`。
3. 板端：`docker load` → `compose up -d` → 容器 `Running`。
4. 板端功能：`curl -s http://<板IP>/` 出 Web；`version.txt` 与镜像标签一致；日志出现 `[RUN_START] Port contract OK`。
5. 板端硬件链路：跑真实视频任务，确认 NPU 计数 > 0、VDEC 出帧计数增长、RGA 计数 > 0。
6. 板端容量：按 V1.1 口径在 5/7/10/15/24 fps 下跑 7463 / 7463002，通道上限 64ch，与裸机基线逐档对照。
7. 持久化：`compose down` + `up -d`，确认用户/摄像头/任务/参数/事件仍在。
8. 回滚：停容器 → `systemctl enable --now cosmo.service` 回裸机，或 `load` 上一版镜像 tag。

---

## 9. 不做的事

- 不替换现有 tar.gz / Web OTA 通道。
- 不改 `src/` 推理与媒体代码（仅可能给 `WatchDog` 加容器模式开关）。
- 不动 `3rd/`、`prebuild/`（仓库硬性约束）。
- 不改动 RKNN2/RKNN3 主从异构后端架构（只改交付与运行形态）。
