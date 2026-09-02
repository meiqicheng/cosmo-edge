# RK3588 model build provenance

Each `model.rknn` in this tree carries a `model.rknn.build.json` record that is
hash-closed against the repository:

- `artifact.path` / `artifact.bytes` / `artifact.sha256` — the `.rknn` itself.
- `source.path` / `source.sha256` — the tracked x86 ONNX export it was derived
  from (via `extract_yolov8_heads.py` / helmet preprocessing).
- `spec.path` / `spec.sha256` — the model spec (`config/rknn/models/*.json`)
  that drove the conversion. The spec must exist and its SHA-256 must match
  the record byte-for-byte; `test_package_profile.py` enforces this
  fail-closed.
- `build.dataset` / `build.dataset_sha256` — the repo-relative calibration
  specification (`calibration/spec.json`). Calibration tensors are regenerated
  deterministically from that spec by `prepare_validation_data.py`; the tensors
  themselves are not stored in git.

## Known boundary: YOLOv8 RKNN input identity

The pinned `yolov8_6head_op19.onnx` (input SHA-256 `17a2cd60…`) was lost with
its agent-run directory. The `model.rknn` in
`prod_RK3588_9275710_YOLOV8_V1.0.0` was reconverted (2026-08-27, artifact
`bce9f866…`) while the spec temporarily recorded the regenerated manual
metadata-downgrade input (`46adbf0d…`). The spec now points at the canonical
regenerated input `17a2cd60…`, which the provenance note in
`config/rknn/models/yolov8.json` records as equivalent to the original pinned
model. Until the model is reconverted from the `17a2cd60…` input on an
admitted RKNN Toolkit2 host and the build record is regenerated, the
conversion input identity remains UNVERIFIED; the artifact itself is verified
and hash-closed against the tracked spec, source, and dataset files.
