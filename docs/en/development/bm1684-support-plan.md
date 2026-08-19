# BM1684 Support Development Plan

> Branch: `feature/bm1684-support`
> Status: Reviewed — key decisions confirmed, implementation pending
> Goal: Extend existing Sophon support (BM1688 / CV186X) to Sophon BM1684 (SM5 SoC module).
> Confirmed: target is **SM5 SoC module**; BM1684 dev device available; first release is
> **single-stream detector + classifier only**; device SDK is **libsophon-0.5.1**
> (linux-aarch64 at `E:\Gdsc\projects\dev\devlibs\linux-aarch64\libsophon-0.5.1`),
> **NOT** `3rd/libsophon-0.4.11`; model conversion runs in **WSL2** (Ubuntu 22.04+, Docker Desktop as fallback).
> Decided: SDK is **copied to `3rd/libsophon-0.5.1/`** (tracked, CI reproducible);
> VPU decode **keeps deprecated `bmvpu_dec_*`** (symbols verified still exported; new API migration is tech debt);
> conversion environment **WSL2**.

---

## 1. Background (verified facts)

CosmoEdge currently supports two Sophon lines — **BM1688** and **CV186X** (both BM1684X-family
SoCs) — plus Rockchip (RK3576 / rv1126b) and x86. Sophon-related code:

| Layer | Location | Notes |
| --- | --- | --- |
| Inference | `src/nn/device/sophon/` | BMRT inference (`sophon_net_node.cc`), preprocessing nodes, YOLO decode, Qwen3-VL/Qwen3.5 VLM |
| Media | `src/media/VideoDecoderSophon.cc`, `VideoFrameProcSophon.cc` | bmvpu decode, VPSS/VPP + bmcv resize/cvtcolor/JPEG |
| Memory | `src/mem/` (`AllocatorSophon.cc`, `DeviceContextSophon.cc`) | `bm_malloc_device_byte_heap_mask(..., heap=3, ...)` device memory pool |
| Device SDK (BM1688/CV186X) | `3rd/libsophon-0.4.11/` | libsophon 0.4.11 (bmlib / bmrt / bmcv / bmvd / bmvenc) |
| Device SDK (BM1684, this task) | `3rd/libsophon-0.5.1/` (linux-aarch64, **committed**) | bmlib / bmrt / bmcv / bmvideo / bmvpuapi / bmvppapi / bmjpuapi; **no bmvd/bmvenc** |
| Model wrapping | `prebuild/model-guard-v2/` + `libcosmo_model_guard.so` | `.bmodel` wrapped in CENN `model.nn` |

Chip identity (`chip_type`) is registered/validated in these places — **all must be extended**:

1. `scripts/build.sh` — `-c <chip>` allowlist: `bm1688|cv186x`
2. `scripts/build_sophon_package.sh` / `build_sophon_package.ps1` — `--chip` allowlist
3. `CMakeLists.txt` — `COSMO_TARGET_CHIP` regex: `^(bm1688|cv186x|rk3576|rv1126b|unspecified)$`
4. `src/util/NnBackendConstants.h` — `kSupportedChips[] = {"BM1688", "CV186X"}` (import gate on `config.json` `chip_type`)
5. `data/resource/aiboxresource_<chip>/` — platform resource sets
6. `scripts/verify_sophon_open_benchmark_models.py`, `scripts/validate-public-v1.1-multistream-benchmark.mjs` — platform maps
7. CI: `.github/workflows/nightly-build-test-sophon.yml`; build env: `docker-compose.sophon.yml`
8. Docs: `docs/guide/build.md`, `docs/guide/configuration.md`, `docs/reference/models.md`,
   `docs/tutorials/05-model-porting/`, `docs/development/backend.md`, `docs/development/ci.md`, README, CHANGELOG

Model artifact contract: `.bmodel` wrapped in CENN `model.nn`, dir `prod_<TOKEN>_<alg>_<name>_<ver>/`,
`chip_type` in `config.json` is the compatibility gate. **BM1684 bmodels are NOT interchangeable with
BM1688/CV186X** — they must be converted and verified separately.

### 1.1 SDK version families (verified from sophon-demo official dependency table)

Sophon SDK ships as **two separate release lines that are NOT interchangeable** (sophon-demo):

- **BM1684 / BM1684X**: LIBSOPHON **>= 0.5.0**, TPU-MLIR >= 1.15, SOPHON-FFMPEG >= 0.7.3, SOPHON-OPENCV >= 0.7.3, SOPHON-SAIL >= 3.8.0
- **BM1688 / CV186X**: LIBSOPHON >= 0.4.9 (repo's 0.4.11 belongs here)

So this task uses `libsophon-0.5.1` (satisfies >= 0.5.0), **not** `3rd/libsophon-0.4.11`.
Library sets differ: 0.4.11 has `libbmvd.so`/`libbmvenc.so` (BM1688/CV186X only);
0.5.1 has `libbmvideo.so.0.14.0`/`libbmvpuapi`/`libbmvppapi`/`libbmjpuapi` and **no bmvd/bmvenc**.
0.5.1 ships BM1684 firmware (`bm1684_ddr.bin`, `bm1684_tcm.bin`, `bmtpu.ko`/`vpu.ko`/`jpu.ko`).

**Library rename mapping (verified against .so symbol tables — `bmvpu_*` symbols unchanged):**

| 0.4.11 lib | 0.5.1 lib | Exported symbols |
| --- | --- | --- |
| `libbmvd.so` (link name `bmvd`) | `libbmvideo.so.0.14.0` (link name `bmvideo`) | `bmvpu_dec_*` (decode) |
| `libbmvenc.so` (link name `bmvenc`) | `libbmvpuapi.so.0.14.0` (link name `bmvpuapi`) | `bmvpu_enc_*` (encode) |

In 0.5.1 the `bmvpu_dec_*` / `bmvpu_enc_*` symbols are unchanged — only the host library was
renamed — and `bm_vpudec_interface.h` still exists (just marked deprecated). Top-level
`CMakeLists.txt:61-62` already sets `-Wno-deprecated-declarations` globally, so keeping the
deprecated API produces **no compile warnings**.

---

## 2. Key differences (SDK/API layer verified; runtime layer needs device)

| Dimension | BM1688 / CV186X (0.4.11) | BM1684 (0.5.1) | Impact |
| --- | --- | --- | --- |
| Compute | BM1688: 16 TOPS INT8 (+2 TOPS) | 17.6 TOPS INT8 | Similar |
| FP16 | Supported (TPU-MLIR `bm1684x`) | **No FP16 in hardware — INT8 only** | Models must be INT8; accuracy re-evaluation required |
| Transformer/VLM | Qwen3-VL etc. | **No transformer engine, 16 GB** | VLM unavailable — build-time disable |
| bmodel target | TPU-MLIR `--target bm1684x` | TPU-MLIR `--target bm1684` | Different toolchain flags |
| Form factor | SoC (onboard VPU/VPSS) | **SM5 SoC module (confirmed)** | VPU decode available; media pipeline mirrors BM1688 |
| Video decode lib | `libbmvd.so` (link name `bmvd`) | **renamed** to `libbmvideo.so.0.14.0` (link name `bmvideo`); `bmvpu_dec_*` symbols unchanged | Link target `bmvd` → `bmvideo` |
| VPU decode API | `bm_vpudec_interface.h` normal | Same header exists but marked `deprecated`; log enum renamed (`BMVPU_DEC_LOG_LEVEL_ERR` → `BMVPU_DEC_LOG_LEVEL_ERROR`) | **Decision: keep deprecated API** (symbols still present, minimal change; new API migration is tech debt); fix enum name |
| Encode/JPEG | `libbmvenc.so`, `libbmjpeg` | **renamed** to `libbmvpuapi.so.0.14.0` (link name `bmvpuapi`) + `libbmjpuapi` | Link target `bmvenc` → `bmvpuapi` (not blocking first release) |
| Video processing | VPSS/VPP + bmcv | VPP + bmcv (`libbmvppapi`/`libvpp`) | `VideoFrameProcSophon` size constraints need device check |
| Memory heap | `heap_mask=3` verified | Heap semantics TBD on device | Verify `bm_malloc_device_byte_heap_mask` parameters |
| Chip probe | — | `bmrt_arch_info.h` includes `BM1684`; `bm_get_chip_id` | Add runtime chip identity check |
| On-device firmware | `bm1688_firmware*.bin`, `bmtpu.ko` | `bm1684_ddr.bin`, `bm1684_tcm.bin`, `bmtpu.ko`/`vpu.ko`/`jpu.ko` | Deployment/install scripts need BM1684 firmware + drivers |

---

## 3. Work breakdown (Phase 0 → Phase 6)

### Phase 0 — Recon & decisions (partially done)

Confirmed:
- [x] Hardware form factor: SM5 SoC module (VPU decode available)
- [x] Device SDK: libsophon-0.5.1 (linux-aarch64, satisfies BM1684 requirement >= 0.5.0)
- [x] SDK/API differences verified: library sets differ, `bmvpu_dec_*`/`bmvpu_enc_*` still exported (host libs renamed), log enum renamed
- [x] SDK bundling: copy to `3rd/libsophon-0.5.1/` (tracked, CI reproducible; done in Phase 1)
- [x] VPU decode API: **keep deprecated `bmvpu_dec_*`** (symbols in `libbmvideo`; top-level already `-Wno-deprecated-declarations`; minimal change; re-evaluate new API after device validation)
- [x] Conversion environment: **WSL2 (Ubuntu 22.04+, x86_64)**, TPU-MLIR >= 1.15 (pip); Docker Desktop as fallback
- [x] First-release scope: single-stream detector + classifier only

On device (to do):
- [ ] Confirm on-device libsophon runtime version (`/opt/sophon/libsophon` or preinstalled 0.5.x) and firmware loading (0.5.1 `data/load.sh`)
- [ ] Measure `bm_malloc_device_byte_heap_mask(..., heap=3, ...)` semantics and `bm_get_chip_id` return values
- [ ] Measure VPP/VPU availability and whether `VideoFrameProcSophon` size constraints (`kSophonVppMinDimension` etc.) apply
- [ ] Confirm ONNX sources + calibration data for YOLOV8n / helmet
- [ ] Deliverable: `docs/development/bm1684-facts.md`

### Phase 1 — Build & packaging (can proceed in parallel without device)

- [x] **SDK integration**: copied `libsophon-0.5.1` to `3rd/libsophon-0.5.1/` (data/include/lib + Apache 2.0 LICENSE,
  symlinks expanded to real files, no bin test tools, consistent with 0.4.11); `cmake/device.cmake` parameterized —
  added `COSMO_LIBSOPHON_ROOT` (CACHE, defaults per chip: `bm1688/cv186x` → `3rd/libsophon-0.4.11`, `bm1684` → `3rd/libsophon-0.5.1`),
  external path override supported (e.g. `E:\Gdsc\projects\dev\devlibs\linux-aarch64\libsophon-0.5.1`)
- [x] `src/media/CMakeLists.txt` link target: no change needed — `device.cmake` maps link names `bmvd`/`bmvenc` to
  `libbmvideo.so`/`libbmvpuapi.so` (auto-detected via `EXISTS`); `bmvpu_dec_*`/`bmvpu_enc_*` symbols verified identical
- [x] `src/media/VideoDecoderSophon.cc:45`: decode log enum `BMVPU_DEC_LOG_LEVEL_ERR` → conditional via
  `COSMO_LIBSOPHON_NEW_VIDEO_API` (defined by device.cmake per SDK) selecting `ERROR` (0.5.1)/`ERR` (0.4.11);
  encode enums are identical across both SDKs — no change needed (headers compared)
- [x] `scripts/build.sh`: allowlist + Usage + resource dir mapping for `bm1684`
- [x] `scripts/build_sophon_package.sh` / `.ps1`: `--chip bm1684` allowlist + Usage
- [x] `CMakeLists.txt`: `COSMO_TARGET_CHIP` regex + `bm1684`
- [x] `src/util/NnBackendConstants.h`: `kSupportedChips` + `"BM1684"` (import gate)
- [x] `test/test_package_profile.py`: chip allowlist assertions updated
- [x] Local static validation: bash syntax check passed; `test_package_profile.py` 15/15 passed
- [ ] `docker-compose.sophon.yml`: confirm existing image completes bm1684 cross-compile/package on Linux/WSL2
- [ ] Local smoke `./scripts/build.sh -c bm1684`: cannot cross-compile on Windows; run in WSL2/build env (empty resource set OK)
- [ ] CI (`.github/workflows/nightly-build-test-sophon.yml`): **deferred to Phase 2** (add to matrix after `aiboxresource_bm1684` is ready, avoid nightly failures)

### Phase 2 — Resource set & model conversion (needs Linux + device)

- [ ] Create `data/resource/aiboxresource_bm1684/` skeleton from `aiboxresource_bm1688/`
- [ ] Model conversion via AGENTS.md agent workflow (model-conversion task):
  `scripts/agent/start.sh` → `doctor.sh --task model-conversion` → `convert_model.sh` → `verify.sh`
  - Conversion env: **WSL2 (Ubuntu 22.04+, x86_64, decided)**; Docker Desktop (Linux container) as fallback
  - TPU-MLIR: **>= 1.15** (sophon-demo 0.3.x requirement; pip install or official docker), must support `--target bm1684`
  - ONNX → bmodel: TPU-MLIR, `--target bm1684`, **INT8 quantization**
  - Calibrate YOLOV8n (640×640), helmet (224×224); compare accuracy before/after
- [ ] Wrap bmodel into CENN `model.nn` via `prebuild/model-guard-v2`; `config.json` `chip_type: "BM1684"`
- [ ] Update verification scripts (`verify_sophon_open_benchmark_models.py`, `validate-public-v1.1-...mjs`)
- [ ] Deliverable: BM1684 YOLOV8n / helmet artifacts with SHA-256 + verification records

### Phase 3 — Runtime adaptation (core code, needs device)

- [ ] `src/util/NnBackendConstants.h`: add `"BM1684"` to `kSupportedChips`
- [ ] Runtime chip probe in `SophonDevice`/`SophonContext`: `bm_get_chip_id` / `bmrt_get_chip_type`
  vs `config.json chip_type`; refuse mismatched models with clear logs
- [ ] Media pipeline (SM5 SoC confirmed — VPU decode; **keep deprecated `bmvpu_dec_*`, decided**):
  - Fix `VideoDecoderSophon.cc:45`: `BMVPU_DEC_LOG_LEVEL_ERR` → `BMVPU_DEC_LOG_LEVEL_ERROR` (enum renamed, verified)
  - Link target: `src/media/CMakeLists.txt:87` → `bmvideo bmvpuapi` (symbols verified)
  - Tech debt (non-blocking): after device validation, evaluate migrating to the new 0.5.1 VPU API
    (`bmvpu.h`/`bmvpu_types.h`/`bmvpu_logging.h`)
  - Adjust `VideoFrameProcSophon` VPSS-specific size constraints (`kSophonVppMinDimension=16`) to BM1684 VPP semantics (measured)
- [ ] Memory: verify heap mask on BM1684; select per-chip heap in `AllocatorSophon`/`DeviceContextSophon`
- [ ] VLM gating: disable/hide `qwen3vl` / `qwen3_5` on BM1684 builds; document capability gap
- [ ] Audit INT8 dequant assumptions in `sophon_yolo_decode_npu_node.cc` etc.

### Phase 4 — Docs & release materials

- [ ] Update `docs/guide/build.md`, `docs/guide/configuration.md`, `docs/reference/models.md`,
  `docs/tutorials/05-model-porting/`, `docs/development/backend.md`, `docs/development/ci.md`,
  README, CHANGELOG; sync `docs/en/**`

### Phase 5 — Verification & benchmark

- [ ] On-device smoke: engine start, BM1684 bmodel load, **single-stream** inference (first release scope)
- [ ] Benchmark (optional, non-blocking): YOLOV8n / helmet FPS on BM1684 vs BM1688
- [ ] Cross-regression: BM1688 / CV186X builds unaffected
- [ ] Record conversion SHA-256, engine SHA-256, verification records per AGENTS.md

### Phase 6 — Definition of Done

- [ ] `./scripts/build.sh -c bm1684` produces target package with chip recorded
- [ ] On-device: detector + classifier run; `chip_type` gate works
- [ ] No regression on BM1688/CV186X; CI matrix includes BM1684
- [ ] All code, scripts, docs updated; verification scripts pass

---

## 4. Risks

| Risk | Impact | Mitigation |
| --- | --- | --- |
| No FP16 on BM1684 (INT8 only) | Accuracy drop | Calibration + accuracy comparison |
| VLM unavailable | Feature gap | **Build-time disable** (qwen3vl/qwen3_5) + UI hide + docs |
| SDK family mismatch (0.4.11 vs 0.5.1) | Compile/link failures | Confirmed switch to 0.5.1; parameterize device.cmake; fix media link targets |
| VPU decode API deprecated + enum rename | Compile failure/API risk | **Mitigated**: symbols still exported (`libbmvideo`), top-level already `-Wno-deprecated-declarations`; only enum name + link target need fixing; new API migration is tech debt |
| Heap/VPP semantic differences | Allocation failures | Measure on device; per-chip parameters |
| bmodels not interchangeable | Wrong-model load | Runtime chip probe + import gate |

---

## 5. Open questions

Confirmed (from user):
1. ✅ Target: SM5 SoC module (VPU decode pipeline)
2. ✅ BM1684 dev device available (Phase 0 / 3 / 5 on real hardware)
3. ✅ First release: single-stream detector + classifier only (VLM build-time disabled; multi-stream benchmark out of scope)
4. ✅ Conversion env: WSL2 (Ubuntu 22.04+, TPU-MLIR >= 1.15 pip); Docker Desktop as fallback

Decided this round (user confirmation + symbol-table evidence):
5. ✅ SDK bundling: copy to `3rd/libsophon-0.5.1/` (tracked, CI reproducible; copy + device.cmake parameterization in Phase 1)
6. ✅ VPU decode API: keep deprecated `bmvpu_dec_*` (`bmvpu_dec_*`/`bmvpu_enc_*` verified still exported; new API migration is tech debt after device validation)
7. ✅ Conversion environment: **WSL2 (Ubuntu 22.04+)**; Docker Desktop as fallback

Remaining (non-blocking):
8. ✅ SDK committed to `3rd/libsophon-0.5.1/` (~106 MB: data 31.6 + include 1.7 + lib 72.8;
   LICENSE carried over as Apache 2.0 — same bmlib licensing lineage as 0.4.11; size in line with 0.4.11, no blocker)
9. Evaluate new 0.5.1 VPU API migration after device validation (tech debt)
