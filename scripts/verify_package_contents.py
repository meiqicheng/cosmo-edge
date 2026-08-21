#!/usr/bin/python3
"""Audit permanent MD5 upgrade packages for the Open and Protected editions."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re
import stat
import tarfile


class PackageAuditError(RuntimeError):
    pass


PROFILES = ("public-runtime", "production-release")
TARGET_CHIPS = ("bm1688", "cv186x", "rk3576", "rk3588", "rv1126b", "unspecified")
REQUIRED_DIRS = {"bin", "files", "font", "lib", "resource", "scripts", "web"}
REQUIRED_EXECUTABLES = {
    "bin/cosmo-engine",
    "scripts/install.sh",
    "scripts/inte_run_start.sh",
    "scripts/run_start.sh",
    "scripts/start.sh",
    "scripts/stop.sh",
}
RUNTIME_PATHS_FILE = "share/cosmo/runtime-paths.env"
REQUIRED_FILES = {"bin/version.txt", "scripts/common.sh", RUNTIME_PATHS_FILE}
RUNTIME_DATA_DIRS = {
    "default": "/data/cwaiuserdata",
    "rockchip": "/userdata/cwaiuserdata",
}
RUNTIME_APP_DATA_DIR = "/appfs/cosmo_wander/cwai_data"
PRIVATE_MARKERS = (
    b"-----BEGIN PRIVATE KEY-----",
    b"-----BEGIN ENCRYPTED PRIVATE KEY-----",
    b"-----BEGIN RSA PRIVATE KEY-----",
    b"-----BEGIN EC PRIVATE KEY-----",
    b"-----BEGIN OPENSSH PRIVATE KEY-----",
)
FORBIDDEN_BASENAMES = {
    "commissioning-ed25519.seed",
    "device-certificate.bin",
    "product-model-key-v1.bin",
    "product-pepper-v1.bin",
    "release-private-key.o",
}


def archive_md5(path: pathlib.Path) -> str:
    digest = hashlib.md5(usedforsecurity=False)
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def verify_archive_name(path: pathlib.Path) -> None:
    match = re.fullmatch(
        r"cosmo-[Vv]\d+\.\d+\.\d+-([0-9a-fA-F]{32})\.tar\.gz", path.name
    )
    if match is None or match.group(1).lower() != archive_md5(path):
        raise PackageAuditError("archive name must contain its exact MD5 digest")


def parse_runtime_paths(data: bytes) -> dict[str, str]:
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError as error:
        raise PackageAuditError("runtime path declaration is not UTF-8") from error

    expected_keys = {
        "COSMO_PACKAGE_DATA_DIR",
        "COSMO_PACKAGE_APP_DATA_DIR",
    }
    values: dict[str, str] = {}
    for line in text.splitlines():
        if not line:
            continue
        if "=" not in line:
            raise PackageAuditError("runtime path declaration contains an invalid line")
        key, value = line.split("=", 1)
        if key not in expected_keys:
            raise PackageAuditError(
                f"runtime path declaration contains an unsupported key: {key}"
            )
        if key in values:
            raise PackageAuditError(
                f"runtime path declaration contains a duplicate key: {key}"
            )
        if not value.startswith("/") or any(character.isspace() for character in value):
            raise PackageAuditError(
                f"runtime path declaration contains an invalid path: {key}"
            )
        values[key] = value

    missing = expected_keys.difference(values)
    if missing:
        raise PackageAuditError(
            f"runtime path declaration is missing: {', '.join(sorted(missing))}"
        )
    return values


def verify_package(
    path: pathlib.Path,
    profile: str,
    target_chip: str | None = None,
    target_policy: dict[str, object] | None = None,
) -> None:
    if not path.is_absolute() or path.is_symlink() or not path.is_file():
        raise PackageAuditError("archive must be one absolute regular file")
    verify_archive_name(path)
    entries: dict[str, tarfile.TarInfo] = {}
    contents: dict[str, bytes] = {}
    root: str | None = None
    with tarfile.open(path, "r:gz") as archive:
        for member in archive:
            parts = pathlib.PurePosixPath(member.name).parts
            if not parts or any(part in ("", ".", "..") for part in parts):
                raise PackageAuditError(f"unsafe archive member: {member.name}")
            root = parts[0] if root is None else root
            if parts[0] != root:
                raise PackageAuditError("archive must contain exactly one package root")
            relative = pathlib.PurePosixPath(*parts[1:]).as_posix()
            if not relative or relative == ".":
                continue
            if relative in entries or not (member.isdir() or member.isreg() or member.issym()):
                raise PackageAuditError(f"unsupported or duplicate member: {relative}")
            entries[relative] = member
            if member.isreg():
                stream = archive.extractfile(member)
                if stream is None:
                    raise PackageAuditError(f"cannot read member: {relative}")
                data = stream.read()
                contents[relative] = data
                if any(marker in data for marker in PRIVATE_MARKERS):
                    raise PackageAuditError(f"private key material is forbidden: {relative}")

    for directory in REQUIRED_DIRS:
        if directory not in entries or not entries[directory].isdir():
            raise PackageAuditError(f"required directory is missing: {directory}")
    for filename in REQUIRED_FILES:
        if filename not in entries or not entries[filename].isreg():
            raise PackageAuditError(f"required file is missing: {filename}")
    for filename in REQUIRED_EXECUTABLES:
        entry = entries.get(filename)
        if entry is None or not entry.isreg() or not (entry.mode & stat.S_IXUSR):
            raise PackageAuditError(f"required executable is missing: {filename}")

    runtime_paths = parse_runtime_paths(contents[RUNTIME_PATHS_FILE])
    if runtime_paths["COSMO_PACKAGE_APP_DATA_DIR"] != RUNTIME_APP_DATA_DIR:
        raise PackageAuditError(
            "runtime application directory is incompatible with the installation layout"
        )

    for filename in entries:
        if pathlib.PurePosixPath(filename).name.lower() in FORBIDDEN_BASENAMES:
            raise PackageAuditError(f"controlled secret is forbidden: {filename}")
        if filename.startswith(".release-bootstrap/") or "release_updater" in filename:
            raise PackageAuditError(f"obsolete signed-release material is forbidden: {filename}")

    if target_policy is not None:
        for field in ("required_package_paths", "forbidden_package_paths"):
            values = target_policy.get(field, [])
            if not isinstance(values, list) or not all(
                isinstance(value, str) and value for value in values
            ):
                raise PackageAuditError(f"target package policy {field} must be a string list")
        for required in target_policy.get("required_package_paths", []):
            if required not in entries:
                raise PackageAuditError(
                    f"required target package path is missing: {required}"
                )
        for forbidden in target_policy.get("forbidden_package_paths", []):
            if forbidden in entries:
                raise PackageAuditError(
                    f"forbidden target package path is present: {forbidden}"
                )

    marker_name = "share/cosmo/target-chip.txt"
    actual_chip: str | None = None
    if marker_name in entries:
        marker = entries.get(marker_name)
        if marker is None or not marker.isreg():
            raise PackageAuditError(f"target chip marker is invalid: {marker_name}")
        try:
            actual_chip = contents[marker_name].decode("utf-8").strip().lower()
        except UnicodeDecodeError as error:
            raise PackageAuditError("target chip marker is not UTF-8") from error
        if actual_chip not in TARGET_CHIPS:
            raise PackageAuditError(f"unsupported target chip marker: {actual_chip}")

    if target_chip is not None:
        if actual_chip is None:
            raise PackageAuditError(f"target chip marker is missing: {marker_name}")
        if actual_chip != target_chip:
            raise PackageAuditError(
                f"target chip mismatch: expected {target_chip}, package contains {actual_chip}"
            )

    effective_chip = target_chip or actual_chip
    if effective_chip is not None:
        expected_data_dir = (
            RUNTIME_DATA_DIRS["rockchip"]
            if effective_chip in ("rk3576", "rv1126b")
            else RUNTIME_DATA_DIRS["default"]
        )
        if runtime_paths["COSMO_PACKAGE_DATA_DIR"] != expected_data_dir:
            raise PackageAuditError(
                "runtime data directory does not match target chip: "
                f"expected {expected_data_dir} for {effective_chip}"
            )
    elif runtime_paths["COSMO_PACKAGE_DATA_DIR"] not in RUNTIME_DATA_DIRS.values():
        raise PackageAuditError("runtime data directory is unsupported")

    if target_chip is not None:
        if target_chip in ("rk3576", "rk3588", "rv1126b"):
            platform_name = "share/cosmo/platform-profile.json"
            platform_entry = entries.get(platform_name)
            if platform_entry is None or not platform_entry.isreg():
                raise PackageAuditError(
                    f"Rockchip platform profile is missing: {platform_name}"
                )
            try:
                platform = json.loads(contents[platform_name].decode("utf-8"))
            except (UnicodeDecodeError, json.JSONDecodeError) as error:
                raise PackageAuditError(
                    "Rockchip platform profile is not valid UTF-8 JSON"
                ) from error
            if not isinstance(platform, dict) or platform.get("chip") != target_chip:
                raise PackageAuditError(
                    f"Rockchip platform profile does not match {target_chip}"
                )
            if platform.get("backend") != "rknn":
                raise PackageAuditError("Rockchip platform profile must use RKNN")
            if target_policy is not None:
                expected_runtime = target_policy.get("media_runtime_profile")
                platform_media = platform.get("media")
                actual_runtime = (
                    platform_media.get("runtime_profile")
                    if isinstance(platform_media, dict)
                    else None
                )
                if expected_runtime and actual_runtime != expected_runtime:
                    raise PackageAuditError(
                        "Rockchip media runtime profile does not match target policy"
                    )

                manifest_name = "share/cosmo/platform/rockchip-media-manifest.json"
                if manifest_name in contents:
                    try:
                        manifest = json.loads(contents[manifest_name].decode("utf-8"))
                    except (UnicodeDecodeError, json.JSONDecodeError) as error:
                        raise PackageAuditError(
                            "Rockchip media manifest is not valid UTF-8 JSON"
                        ) from error
                    if (
                        not isinstance(manifest, dict)
                        or manifest.get("runtime_profile") != expected_runtime
                    ):
                        raise PackageAuditError(
                            "Rockchip media manifest does not match target policy"
                        )

    provision = entries.get("bin/cosmo-model-provision")
    if profile == "public-runtime" and provision is not None:
        raise PackageAuditError("Open package must not contain the provisioning tool")
    if profile == "production-release" and (
        provision is None or not provision.isreg() or not (provision.mode & stat.S_IXUSR)
    ):
        raise PackageAuditError("Protected package requires cosmo-model-provision")

    models = {
        name: data
        for name, data in contents.items()
        if name.startswith("resource/models/") and name.endswith("/model.nn")
    }
    for name, data in models.items():
        encrypted = data.startswith(b"CEMC")
        if profile == "public-runtime" and encrypted:
            raise PackageAuditError(f"Open package contains an encrypted preset model: {name}")
        if profile == "production-release" and not encrypted:
            raise PackageAuditError(f"Protected package contains a plaintext preset model: {name}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--archive", required=True)
    parser.add_argument("--build-profile", required=True, choices=PROFILES)
    parser.add_argument("--target-chip", choices=TARGET_CHIPS)
    parser.add_argument("--target-policy-lock", type=pathlib.Path)
    arguments = parser.parse_args()
    try:
        target_policy = None
        if arguments.target_policy_lock is not None:
            if arguments.target_chip is None:
                raise PackageAuditError(
                    "--target-policy-lock requires --target-chip"
                )
            lock = json.loads(
                arguments.target_policy_lock.read_text(encoding="utf-8")
            )
            target_policy = lock["targets"][arguments.target_chip]
            if not isinstance(target_policy, dict):
                raise PackageAuditError("selected target package policy must be an object")
        verify_package(
            pathlib.Path(arguments.archive),
            arguments.build_profile,
            arguments.target_chip,
            target_policy,
        )
    except (
        OSError,
        KeyError,
        TypeError,
        UnicodeError,
        json.JSONDecodeError,
        tarfile.TarError,
        PackageAuditError,
    ) as error:
        parser.error(str(error))
    print(f"Verified {arguments.build_profile} MD5 upgrade package: {arguments.archive}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
