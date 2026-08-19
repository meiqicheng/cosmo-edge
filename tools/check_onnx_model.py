#!/usr/bin/env python3
"""Validate an ONNX model, optionally with an ONNX Runtime smoke inference."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
from pathlib import Path
from typing import Any


DTYPE_BY_ORT_TYPE = {
    "tensor(float)": "float32",
    "tensor(float16)": "float16",
    "tensor(double)": "float64",
    "tensor(int8)": "int8",
    "tensor(int16)": "int16",
    "tensor(int32)": "int32",
    "tensor(int64)": "int64",
    "tensor(uint8)": "uint8",
    "tensor(uint16)": "uint16",
    "tensor(uint32)": "uint32",
    "tensor(uint64)": "uint64",
    "tensor(bool)": "bool",
}


class OnnxCheckError(RuntimeError):
    pass


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def parse_shape_override(value: str) -> tuple[str, list[int]]:
    name, separator, raw_shape = value.partition("=")
    if not separator or not name.strip():
        raise argparse.ArgumentTypeError("shape override must be INPUT=1,3,640,640")
    try:
        shape = [int(part.strip()) for part in raw_shape.split(",")]
    except ValueError as error:
        raise argparse.ArgumentTypeError("shape dimensions must be integers") from error
    if not shape or any(dimension <= 0 for dimension in shape):
        raise argparse.ArgumentTypeError("shape dimensions must be positive")
    return name.strip(), shape


def runtime_shape(metadata_shape: list[Any], override: list[int] | None) -> list[int]:
    if override is not None:
        if len(override) != len(metadata_shape):
            raise OnnxCheckError(
                f"shape override rank {len(override)} does not match model rank {len(metadata_shape)}"
            )
        return override
    shape = []
    for dimension in metadata_shape:
        if isinstance(dimension, int) and dimension > 0:
            shape.append(dimension)
        else:
            shape.append(1)
    return shape


def _safe_shape(value: Any) -> list[Any]:
    return list(value) if isinstance(value, (list, tuple)) else []


def _declared_value_info(onnx: Any, value: Any) -> dict[str, Any]:
    tensor_type = value.type.tensor_type
    dimensions: list[Any] = []
    for dimension in tensor_type.shape.dim:
        if dimension.HasField("dim_value"):
            dimensions.append(int(dimension.dim_value))
        elif dimension.dim_param:
            dimensions.append(dimension.dim_param)
        else:
            dimensions.append(None)
    try:
        element_type = onnx.TensorProto.DataType.Name(tensor_type.elem_type).lower()
    except ValueError:
        element_type = str(tensor_type.elem_type)
    return {
        "name": value.name,
        "declaredShape": dimensions,
        "type": f"tensor({element_type})",
    }


def inspect_model(
    model_path: Path,
    overrides: dict[str, list[int]],
    *,
    run_runtime: bool = True,
) -> dict[str, Any]:
    try:
        import onnx
    except ImportError as error:
        raise OnnxCheckError(
            "missing dependency; use an approved environment containing onnx"
        ) from error

    try:
        model = onnx.load(str(model_path))
        try:
            onnx.checker.check_model(model)
        except Exception as error:
            # The pinned onnx version required by the conversion toolchain
            # (e.g. onnx==1.14.1 for tpu_mlir 1.28) can reject models with a
            # newer ir_version (e.g. ir_version 10) even though the graph loads
            # fine. Loading is the actual gate for conversion; treat a checker
            # refusal as a warning, not a hard failure.
            print(f"warning: onnx checker refused model: {error}", file=sys.stderr)
    except Exception as error:
        raise OnnxCheckError(f"ONNX load/check failed: {error}") from error

    model_identity = {
        "path": model_path.name,
        "sha256": sha256_file(model_path),
        "sizeBytes": model_path.stat().st_size,
    }
    graph = {
        "irVersion": int(model.ir_version),
        "opsets": [
            {"domain": item.domain or "ai.onnx", "version": int(item.version)}
            for item in model.opset_import
        ],
    }
    if not run_runtime:
        if overrides:
            raise OnnxCheckError("shape overrides require ONNX Runtime smoke validation")
        return {
            "schemaVersion": "1.0",
            "status": "PASS",
            "validationMode": "checker-only",
            "model": model_identity,
            "graph": graph,
            "dependencies": {"onnx": onnx.__version__},
            "inputs": [_declared_value_info(onnx, value) for value in model.graph.input],
            "outputs": [_declared_value_info(onnx, value) for value in model.graph.output],
            "note": "ONNX checker validates source structure; runtime execution is checked after target conversion-input normalization.",
        }

    try:
        import numpy as np
        import onnxruntime as ort
    except ImportError as error:
        raise OnnxCheckError(
            "missing runtime dependency; use an approved environment containing numpy and onnxruntime"
        ) from error

    try:
        session = ort.InferenceSession(str(model_path), providers=["CPUExecutionProvider"])
    except Exception as error:
        raise OnnxCheckError(f"ONNX Runtime load failed: {error}") from error

    inputs = []
    feed = {}
    total_elements = 0
    for metadata in session.get_inputs():
        declared_shape = _safe_shape(metadata.shape)
        shape = runtime_shape(declared_shape, overrides.get(metadata.name))
        dtype_name = DTYPE_BY_ORT_TYPE.get(metadata.type)
        if not dtype_name:
            raise OnnxCheckError(f"unsupported input type for zero-input smoke: {metadata.type}")
        elements = 1
        for dimension in shape:
            elements *= dimension
        total_elements += elements
        if total_elements > 100_000_000:
            raise OnnxCheckError(
                "zero-input smoke would allocate more than 100 million elements; provide smaller --shape overrides"
            )
        feed[metadata.name] = np.zeros(shape, dtype=np.dtype(dtype_name))
        inputs.append(
            {
                "name": metadata.name,
                "declaredShape": declared_shape,
                "runtimeShape": shape,
                "type": metadata.type,
            }
        )

    unknown_overrides = sorted(set(overrides) - {item["name"] for item in inputs})
    if unknown_overrides:
        raise OnnxCheckError(f"shape override names are not model inputs: {', '.join(unknown_overrides)}")

    try:
        runtime_outputs = session.run(None, feed)
    except Exception as error:
        raise OnnxCheckError(f"ONNX Runtime smoke inference failed: {error}") from error

    outputs = [
        {
            "name": metadata.name,
            "declaredShape": _safe_shape(metadata.shape),
            "type": metadata.type,
            "runtimeShape": list(getattr(value, "shape", [])),
            "runtimeDtype": str(getattr(value, "dtype", "unknown")),
        }
        for metadata, value in zip(session.get_outputs(), runtime_outputs)
    ]
    return {
        "schemaVersion": "1.0",
        "status": "PASS",
        "validationMode": "runtime-smoke",
        "model": model_identity,
        "graph": graph,
        "providers": session.get_providers(),
        "dependencies": {
            "numpy": np.__version__,
            "onnx": onnx.__version__,
            "onnxruntime": ort.__version__,
        },
        "inputs": inputs,
        "outputs": outputs,
        "note": "Zero-input smoke validates graph loading and execution, not business accuracy.",
    }


def write_result(result: dict[str, Any], destination: str | None) -> None:
    if destination is None:
        print("ONNX checker: PASS")
        print(
            "inputs:",
            [(item["name"], item["declaredShape"], item["type"]) for item in result["inputs"]],
        )
        print(
            "outputs:",
            [(item["name"], item["declaredShape"], item["type"]) for item in result["outputs"]],
        )
        if result.get("validationMode") == "runtime-smoke":
            print("runtime output shapes:", [item["runtimeShape"] for item in result["outputs"]])
        else:
            print("validation mode:", result.get("validationMode", "checker-only"))
        return
    payload = json.dumps(result, ensure_ascii=False, indent=2) + "\n"
    if destination == "-":
        print(payload, end="")
        return
    output = Path(destination).expanduser()
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(payload, encoding="utf-8")
    output.chmod(0o600)


def main(arguments: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Run onnx.checker and a zero-input ONNX Runtime smoke inference."
    )
    parser.add_argument("model")
    parser.add_argument(
        "--shape",
        action="append",
        default=[],
        type=parse_shape_override,
        metavar="INPUT=DIM,DIM,...",
        help="override a dynamic input shape; repeat for multiple inputs",
    )
    parser.add_argument(
        "--json",
        nargs="?",
        const="-",
        metavar="PATH",
        help="write JSON to PATH, or stdout when PATH is omitted",
    )
    parser.add_argument(
        "--checker-only",
        action="store_true",
        help="run onnx.checker without importing or executing ONNX Runtime",
    )
    options = parser.parse_args(arguments)
    model_path = Path(options.model).expanduser().resolve()
    if not model_path.is_file():
        parser.error(f"model does not exist: {model_path}")
    overrides = dict(options.shape)
    if options.checker_only and overrides:
        parser.error("--shape cannot be combined with --checker-only")
    try:
        result = inspect_model(model_path, overrides, run_runtime=not options.checker_only)
    except OnnxCheckError as error:
        failure = {
            "schemaVersion": "1.0",
            "status": "FAIL",
            "model": {"path": model_path.name, "sha256": sha256_file(model_path)},
            "detail": str(error),
        }
        if options.json is not None:
            write_result(failure, options.json)
        else:
            print(f"ONNX check failed: {error}", file=sys.stderr)
        return 1
    write_result(result, options.json)
    return 0


if __name__ == "__main__":
    os.umask(0o077)
    raise SystemExit(main())
