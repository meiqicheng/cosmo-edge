#!/usr/bin/env python3
"""Check that the Model Guard SDK contains the interface CosmoEdge uses."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import re
import subprocess
import sys
from typing import NoReturn


EXPECTED_V2_EXPORTS = {
    "CmgV2CloseArtifact@@CMG_2.0",
    "CmgV2GetArtifactInfo@@CMG_2.0",
    "CmgV2LoadSophonSegment@@CMG_2.0",
    "CmgV2OpenArtifact@@CMG_2.0",
}
REQUIRED_HEADER_LINES = {
    "#define CMG_V2_ABI_MAJOR UINT32_C(2)",
    "#define CMG_V2_ARTIFACT_INFO_SIZE UINT32_C(72)",
    "#define CMG_V2_SOPHON_LOAD_OPTIONS_SIZE UINT32_C(16)",
}
REQUIRED_HEADER_FUNCTIONS = (
    "CmgV2OpenArtifact",
    "CmgV2GetArtifactInfo",
    "CmgV2LoadSophonSegment",
    "CmgV2CloseArtifact",
)
ADMISSION_PUBLIC_RUNTIME = "public-runtime"
ADMISSION_PRODUCTION_RELEASE = "production-release"
ADMISSION_TEST_FIXTURE = "test-fixture"
ADMISSION_PROFILES = (
    ADMISSION_PUBLIC_RUNTIME,
    ADMISSION_PRODUCTION_RELEASE,
    ADMISSION_TEST_FIXTURE,
)
TEST_FIXTURE_MARKER_NAME = "TEST_FIXTURE_DO_NOT_DEPLOY"
TEST_FIXTURE_MARKER_CONTENT = b"COSMO_MODEL_GUARD_V2_TEST_FIXTURE_DO_NOT_DEPLOY\n"


def load_release_manifest() -> dict:
    path = (
        pathlib.Path(__file__).resolve().parents[1]
        / "prebuild/model-guard-v2/SDK-MANIFEST.json"
    )
    return json.loads(path.read_text(encoding="utf-8"))


def verify_manifest(data: bytes, components: dict[str, str]) -> None:
    try:
        manifest = json.loads(data.decode("utf-8", "strict"))
    except (UnicodeError, ValueError) as error:
        raise RuntimeError("Sophon SDK manifest is invalid") from error
    if manifest != load_release_manifest():
        fail("Sophon SDK manifest does not match the repository release")
    if manifest.get("components") != components:
        fail(
            "Sophon SDK component hashes do not match the release manifest; "
            "restore the complete matching SDK"
        )


def fail(message: str) -> NoReturn:
    raise RuntimeError(message)


def checked_directory(path: pathlib.Path) -> None:
    if not path.is_dir():
        fail(f"SDK directory is missing: {path}")


def checked_file(
    path: pathlib.Path,
    maximum_size: int,
    *,
    allow_empty: bool = False,
) -> bytes:
    try:
        data = path.read_bytes()
    except OSError as error:
        fail(f"cannot read SDK file {path}: {error}")
    if (not allow_empty and not data) or len(data) > maximum_size:
        fail(f"SDK file size is invalid: {path}")
    return data


def checked_symlink(path: pathlib.Path, expected_target: str) -> None:
    if not path.is_symlink() or os.readlink(path) != expected_target:
        fail(f"SDK linker alias is invalid: {path}")


def run_tool(tool: pathlib.Path, arguments: list[str]) -> str:
    if not tool.is_absolute() or not tool.is_file():
        fail(f"inspection tool is missing: {tool}")
    environment = dict(os.environ)
    environment["LC_ALL"] = "C"
    completed = subprocess.run(
        [str(tool), *arguments],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        env=environment,
    )
    if completed.returncode != 0:
        fail(f"inspection tool failed: {tool.name}")
    return completed.stdout


def verify_header(header: bytes) -> None:
    if not header or len(header) > 128 * 1024 or b"\x00" in header:
        fail("Model Guard v2 header size/content rejected")
    try:
        text = header.decode("utf-8", "strict")
    except UnicodeError as error:
        raise RuntimeError("Model Guard v2 header is not UTF-8") from error
    if not REQUIRED_HEADER_LINES.issubset(set(text.splitlines())):
        fail("Model Guard v2 header constants are incompatible")
    for function in REQUIRED_HEADER_FUNCTIONS:
        if len(re.findall(rf"\b{re.escape(function)}\s*\(", text)) != 1:
            fail(f"Model Guard v2 header declaration rejected: {function}")


def verify_elf(
    library: pathlib.Path,
    readelf: pathlib.Path,
    nm: pathlib.Path,
) -> None:
    header = run_tool(readelf, ["-h", str(library)])
    if re.search(r"^\s*Type:\s+DYN\b", header, re.MULTILINE) is None:
        fail("model guard SDK library is not a shared ELF image")

    dynamic = run_tool(readelf, ["-d", str(library)])
    runpaths = re.findall(r"\(RUNPATH\).*\[([^]]+)\]", dynamic)
    if runpaths != ["$ORIGIN"]:
        fail("model guard SDK RUNPATH must be exactly $ORIGIN")

    symbols = run_tool(nm, ["-D", "--defined-only", str(library)])
    exports = {
        fields[2]
        for line in symbols.splitlines()
        if len(fields := line.split()) == 3 and fields[1] in {"T", "W"}
    }
    if exports != EXPECTED_V2_EXPORTS:
        fail(f"model guard SDK exports are incompatible: {sorted(exports)}")


def verify_provision_tool(tool: pathlib.Path, readelf: pathlib.Path) -> None:
    header = run_tool(readelf, ["-h", str(tool)])
    if re.search(r"^\s*Type:\s+(?:DYN|EXEC)\b", header, re.MULTILINE) is None:
        fail("cosmo-model-provision is not an ELF executable")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--admission-profile", choices=ADMISSION_PROFILES, required=True
    )
    parser.add_argument("--sdk-root", required=True)
    parser.add_argument("--readelf", required=True)
    parser.add_argument("--nm", required=True)
    arguments = parser.parse_args()
    public_runtime = arguments.admission_profile == ADMISSION_PUBLIC_RUNTIME
    test_fixture = arguments.admission_profile == ADMISSION_TEST_FIXTURE

    root = pathlib.Path(arguments.sdk_root)
    if not root.is_absolute():
        fail("SDK root must be absolute")
    include_directory = root / "include"
    library_directory = root / "lib"
    share_directory = root / "share/cosmo-model-guard"
    for directory in (root, include_directory, library_directory):
        checked_directory(directory)

    header_path = include_directory / "cosmo_model_guard_v2.h"
    library_path = library_directory / "libcosmo_model_guard.so.2.0.0"
    header = checked_file(header_path, 128 * 1024)
    library = checked_file(library_path, 32 * 1024 * 1024)
    checked_symlink(
        library_directory / "libcosmo_model_guard.so.2", library_path.name
    )
    checked_symlink(
        library_directory / "libcosmo_model_guard.so",
        "libcosmo_model_guard.so.2",
    )

    marker_path = share_directory / TEST_FIXTURE_MARKER_NAME
    marker: bytes | None = None
    if marker_path.exists():
        marker = checked_file(marker_path, len(TEST_FIXTURE_MARKER_CONTENT))
        if marker != TEST_FIXTURE_MARKER_CONTENT:
            fail("Model Guard SDK test-fixture marker content is invalid")
    if test_fixture and marker is None:
        fail("test-fixture admission requires the exact non-production SDK marker")
    if not test_fixture and marker is not None:
        fail(
            "Model Guard test fixtures are forbidden for public-runtime and "
            "production-release admission"
        )

    provision_path: pathlib.Path | None = None
    provision: bytes | None = None
    if not public_runtime:
        provision_path = root / "bin/cosmo-model-provision"
        provision = checked_file(provision_path, 32 * 1024 * 1024)

    verify_header(header)
    verify_elf(
        library_path,
        pathlib.Path(arguments.readelf),
        pathlib.Path(arguments.nm),
    )
    if provision_path is not None:
        verify_provision_tool(provision_path, pathlib.Path(arguments.readelf))

    if arguments.admission_profile == ADMISSION_PRODUCTION_RELEASE:
        verify_manifest(
            checked_file(root / "SDK-MANIFEST.json", 128 * 1024),
            {
                "include/cosmo_model_guard_v2.h": hashlib.sha256(header).hexdigest(),
                "lib/libcosmo_model_guard.so.2.0.0": hashlib.sha256(library).hexdigest(),
                "bin/cosmo-model-provision": hashlib.sha256(provision).hexdigest(),
            },
        )

    print(f"admission_profile={arguments.admission_profile}")
    print(f"verified_sdk_root={root}")
    print(f"header_sha256={hashlib.sha256(header).hexdigest()}")
    print(f"library_sha256={hashlib.sha256(library).hexdigest()}")
    if provision is not None:
        print(f"provision_tool_sha256={hashlib.sha256(provision).hexdigest()}")
    if public_runtime:
        print("sdk_profile=public-runtime")
    elif test_fixture:
        print("sdk_profile=TEST-FIXTURE-DO-NOT-DEPLOY")
    else:
        print("sdk_profile=production")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError) as error:
        print(f"model guard v2 SDK verification failed: {error}", file=sys.stderr)
        raise SystemExit(1)
