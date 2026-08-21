---
title: RK3588 / RKNN Integration
description: Stable-release build, runtime, model, and validation boundaries for Rockchip RK3588.
prev:
  text: Build Guide
  link: /en/guide/build
next:
  text: macOS Docker Preview
  link: /en/guide/macos-docker-preview
---

# RK3588 / RKNN Integration Guide

> Status: this page is the external integration boundary for RK3588 support.
> Implementation progress, material inventory and per-stage review records live in
> the [RK3588 Support Extension Plan](/development/rk3588-support-plan) (Chinese).
> Items marked **UNVERIFIED** below have not completed on-device evidence and must
> not be treated as release claims.

## Scope

The RK3588 integration reuses the RK3576 RKNN/MPP/RGA backend without changing the
behavior of the CPU, CUDA, or Sophon backends:

- RKNN Runtime 2.3.2 executes static-batch detector and classifier models.
- RKLLM Runtime 1.3.0 works with an RKNN vision encoder to execute Qwen3.5
  multimodal models.
- Rockchip MPP performs H.264/H.265 decode and encode.
- The decoder uses delayed Copy-out: frames are sampled or discarded before a
  host I420 copy is requested.
- RGA performs the Rockchip frame-processing operations required by preview and
  OSD paths.
- Full DMA-BUF zero-copy is outside the supported release boundary.

Differences from RK3576 are limited to chip-related items:

1. `.rknn` models must be re-converted with RKNN-Toolkit2
   (`target_platform=rk3588`).
2. The package chip label (`target-chip.txt` / `COSMO_TARGET_CHIP`) is `rk3588`.
3. The NPU devfreq node name differs (RK3588 exposes `fdab0000.npu`); the code
   probes `/sys/class/devfreq/*npu*` instead of hardcoding a path.
4. Board OS baseline (see "Frozen Toolchain Identities" below).

**RK3588 long-term stability validation is not complete**: NPU inference has
on-device evidence, but end-to-end service acceptance, 12-hour long-run and
Qwen3.5 multimodal acceptance remain **UNVERIFIED**.

## Repository and Evidence Boundary

The repository owns product code, build definitions, unit tests, reproducible
model tooling, and deployable RK3588 resources:

- `data/resource/aiboxresource_rk3588/` — RK3588 algorithms and models
  (`chip_type: RK3588`).
- `config/rknn/models/helmet_rk3588.json`, `yolov8_rk3588.json` — conversion specs.
- `docker-compose.rockchip.yml` + `scripts/build_rockchip_package.sh --chip rk3588`
  — unified build/package entry.
- `config/rknn/toolchain-lock.json` — toolchain lock (`target_platforms` lists both chips).

On-device raw logs, metric streams, screenshots, exported events, and generated
HTML/XML/JSON reports are external validation artifacts and must not be added to
the source tree. Release evidence manifests must bind the source commit/tree,
final package SHA-256, device/firmware/runtime versions, model and dataset hashes,
thresholds, cleanup state, and measured values.

Device addresses, account data, local backup paths, and reusable credentials must
never enter version control configuration or evidence.

## Frozen Toolchain Identities

The machine-readable toolchain and model-input lock is
`config/rknn/toolchain-lock.json`, which declares
`target_platforms: ["rk3576", "rk3588"]`:

- RKNN-Toolkit2 2.3.2
- RKNN Model Zoo 2.3.2
- Ubuntu 22.04 x86_64 conversion host with Python 3.10
- The RKNN/RKLLM runtimes and Toolkit are shared across chips: the
  `librknnrt.so` SHA-256 (`d31fc19c...`) is byte-identical across the build image,
  the frozen devlibs bundle and the toolchain lock (verified).
- `device_baseline.rk3588`: kernel/driver/system runtime are all `null` with
  status `UNVERIFIED` — board baseline awaits on-device measurement.

### Board OS baseline (two measured observations, neither fully closed)

| Observation | Device | OS | glibc | Compatible with build artifacts? |
| --- | --- | --- | --- | --- |
| A (support plan record) | Telpo V15B | Debian 11 (bullseye) | 2.31 | ❌ Ubuntu 22.04 artifacts require up to GLIBC_2.35 |
| B (this session) | SmartDev V15BS | Debian 12 (bookworm) | 2.36 | ✅ Ubuntu 22.04 artifacts run directly |

> Note: the repository aarch64 build baseline is Ubuntu 22.04 (glibc 2.35);
> measured artifacts require up to GLIBC_2.35 (`prebuild/ffmpeg/aarch64` plus
> source-built openssl/curl etc.). Debian 12 (glibc 2.36) runs them directly;
> Debian 11 (glibc 2.31) requires the bullseye rebuild path
> (`Dockerfile.rk3588-bullseye`, including a Debian 11 ffmpeg supply). The
> decision and evidence are recorded in stage 5 of the
> [RK3588 Support Extension Plan](/development/rk3588-support-plan) (Chinese).

## Runtime Safety Boundary

Keep the board's system RKNN runtime as the rollback baseline. Package RKNN
Runtime 2.3.2 beside CosmoEdge and select it with executable RPATH or a
task-local `LD_LIBRARY_PATH`; do not overwrite `/usr/lib/librknnrt.so`.
Production inference uses the native C API and does not depend on `rknn_server`.

> ⚠️ Known board-firmware constraint (UNVERIFIED): on the SmartDev V15BS device
> measured this session, the `npu@fdab0000` node in the boot resource image has
> `status="disabled"` (vendor firmware disables NPU). Editing the DTB directly
> made u-boot refuse to boot (likely FIT signature verification; not confirmed).
> Confirm the target device exposes `/dev/rknpu` before deployment.

## Model and Preprocessing Contract

The first supported models are the same generation as RK3576 but must be
re-converted with `target_platform=rk3588`:

1. Helmet classification: `1x3x224x224`, ONNX opset 19.
2. YOLOv8 detection: `1x3x640x640`, converted to ONNX opset 19 / IR 9.

RK3588-converted artifacts ship with the repository
(`data/resource/aiboxresource_rk3588/models/`):

- `prod_RK3588_7982161_helmet_V1.0.0/` (classify, `chip_type: RK3588`)
- `prod_RK3588_9275710_YOLOV8_V1.0.0/` (detect, `chip_type: RK3588`)
- `prod_RK3588_7000001_qwen3_5_V1.0.0/` (`model.rkllm` + `vision.rknn`, `chip_type: RK3588`)

Qwen3.5 multimodal is a separate model contract. An importable directory
contains at least:

- `model.rkllm`: a language model targeting RK3588;
- `vision.rknn`: a vision encoder whose image-token count and embedding width
  match the language model;
- `tokenizer.json`: the tokenizer from the exact conversion source model;
- `config.json`: `model_type` is `qwen3_5` and `runtime_backend` is `rkllm`.

Record SHA-256 for the four files as one set. The presence of `librkllmrt.so`,
a text-only model load, or an isolated `vision.rknn` run does not prove
multimodal capability.

CosmoEdge owns resize, channel order and normalization; conversion must not bake
mean/std transforms again. CosmoEdge feeds float32 NCHW tensors and the RKNN
boundary performs one explicit NCHW-to-NHWC copy. Outputs are requested as
float32 with the existing post-processors as the behavioral baseline. Production
YOLO models expose three box/class heads; the `yolov8_dfl_v1` host adapter runs
DFL and sigmoid, then rebuilds the logical `[1,84,8400]` contract.

## Reproducible Conversion

Conversion commands match RK3576 except for the spec file and
`--target-platform`:

```bash
python tools/rknn/convert_model.py \
  --spec config/rknn/models/yolov8_rk3588.json --model yolov8-heads.onnx \
  --output yolov8-heads-int8-rk3588.rknn --quantize \
  --dataset yolov8-calibration/dataset.txt
```

`config/rknn/models/helmet_rk3588.json` and `yolov8_rk3588.json` record
`target_platform: rk3588`, input shapes, mean/scale and ONNX source SHA-256.
Calibration and numeric-consistency samples carry no labels and cannot replace a
labeled precision/recall/F1 validation set.

## Build and Deployment

RK3588 and RK3576 share the same digest-pinned builder image (its bundled SDKs
are byte-identical to the frozen devlibs bundle, hashes verified) and the same
compose service; select the chip at run time with `--chip`:

```bash
# Explicitly select rk3588
./scripts/docker-compose.sh -f docker-compose.rockchip.yml run --rm cosmo-rockchip-package --chip rk3588
sha256sum build_output/rk3588/cosmo-*.tar.gz
```

Build flow highlights:

- `build_rockchip_package.sh --chip rk3588` removes the old `build_rknn/` and
  calls `build_rknn.sh -C rk3588 -T`; the resource tree is selected per chip
  (rk3588 → `data/resource/aiboxresource_rk3588`).
- Package validation is mandatory: exactly one artifact, includes
  `lib/librkllmrt.so`, includes `share/licenses/rkllm/LICENSE`,
  `target-chip.txt == rk3588`, and `librknnrt.so` SHA-256 matches the frozen value.
- Output goes to `build_output/rk3588/` plus `TARGET_CHIP` and `SHA256SUMS`,
  without starting application services.
- Also builds the aarch64 validation programs: `cosmo-tests`,
  `cosmo-rknn-backend-smoke`, and `cosmo-rknn-fastpath-qualify`.

Current artifact: `build_output/rk3588/cosmo-V1.1.0-3e982955....tar.gz`
(Ubuntu 22.04 baseline, `target-chip.txt=rk3588`).

Isolate the mutable data directory from the in-package app directory (same as
RK3576):

```bash
export COSMO_DATA_DIR=/data/cwaiuserdata
export COSMO_APP_DATA_DIR=/appfs/cosmo_wander/cwai_data
export LD_LIBRARY_PATH="$COSMO_APP_DATA_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
```

## Reusable Acceptance Scenarios

Scenario tooling is shared with RK3576, but on-device acceptance must run on an
RK3588 device:

- Customer journey: bounded-time 1 channel × 5 FPS covering login, model/task/
  channel visibility, real raw and algorithm HTTP-FLV playback, OSD diff, events,
  reconnect, stop/start recovery and cleanup.
- Long-run: 4 channels × 5 FPS for 12 hours with the algorithm preview client
  enabled and `--gate-hours 12` audit.

**UNVERIFIED**: these scenarios have not yet completed on an RK3588 device (NPU
enablement and board baseline pending).

## Qwen3.5 Multimodal Device Acceptance

Formal acceptance must feed a fixed test image on a real RK3588 device and get a
non-empty, image-related text result. Text-only Q&A only proves the RKLLM
language side. The record must include at least the package and four model-file
SHA-256 values, RKLLM/Toolkit/driver versions, target platform, quantization
type, vision input/output shapes, test-image hash, returned text, and exit
status.

**UNVERIFIED**: multimodal acceptance has not been executed on an RK3588 device.

## Verified Release Boundaries

Measured conclusions:

- **On-device RKNN inference path** (support plan stage 5, Telpo V15B / Debian 11):
  `tools/rknn/rknn_device_probe.c` built natively on the device reported
  `rknn_init OK` (sdk 2.3.2 / driver 0.9.3), helmet model loaded, `rknn_run`
  produced valid output.
- **Package build and validation**: `build_rockchip_package.sh --chip rk3588`
  succeeded end to end (aarch64 + `target-chip.txt=rk3588` + package checks).
- **Cross-chip shared runtimes**: `librknnrt.so` / `librkllmrt.so` SHA-256 match
  across the build image, devlibs and the toolchain lock (verified).
- **NPU devfreq generalization**: code probes `/sys/class/devfreq/*npu*`,
  covering both RK3588 `fdab0000.npu` and RK3576 `27700000.npu`.

UNVERIFIED / not closed:

- Full service deployment and `cosmo.service` startup (blocked by the glibc
  baseline decision and NPU enablement).
- 12-hour long-run and 16/8-channel staircase boundaries (RK3576 numbers must
  not be reused).
- Qwen3.5 multimodal device acceptance.
- `device_baseline.rk3588` in `toolchain-lock.json` (kernel/driver/runtime).
- SmartDev V15BS NPU node `status="disabled"` enablement (u-boot FIT
  verification rule not confirmed).

These conclusions are bound to the artifacts; source, model, runtime, or package
changes require re-validation. Accepted release records must retain immutable
package SHA-256, business-accuracy results, credential-security logs, event
retention results, cleanup state, and measured values. Raw validation artifacts
stay outside the source tree.
