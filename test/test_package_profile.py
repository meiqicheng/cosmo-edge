#!/usr/bin/python3
"""Regression tests for Open/Protected permanent MD5 package policy."""

from __future__ import annotations

import copy
import hashlib
import importlib.util
import io
import json
import pathlib
import shutil
import struct
import subprocess
import tarfile
import tempfile
import unittest


REPOSITORY = pathlib.Path(__file__).resolve().parents[1]
SOPHON_SDK = REPOSITORY / "prebuild/model-guard-v2"
SOPHON_MANIFEST_FILE = "share/cosmo-model-guard/SDK-MANIFEST.json"
SOPHON_HEADER_FILE = "share/cosmo-model-guard/cosmo_model_guard_v2.h"
spec = importlib.util.spec_from_file_location(
    "package_verifier", REPOSITORY / "scripts/verify_package_contents.py"
)
assert spec and spec.loader
verifier = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verifier)

gate_spec = importlib.util.spec_from_file_location(
    "glibc_gate", REPOSITORY / "tools/package/glibc_gate.py"
)
assert gate_spec and gate_spec.loader
glibc_gate = importlib.util.module_from_spec(gate_spec)
gate_spec.loader.exec_module(glibc_gate)


class PackageProfileTests(unittest.TestCase):
    def make_package(
        self,
        profile: str,
        model: bytes = b"plain-model",
        model_type: str = "yolov8_det",
        target_chip: str | None = None,
        platform_chip: str | None = None,
        platform_runtime: str | None = None,
        runtime_data_dir: str | None = None,
        runtime_app_data_dir: str = "/appfs/cosmo_wander/cwai_data",
        model_bundle_scope: str | None = None,
        include_bundle_license: bool = True,
        omit_required_file: str | None = None,
        include_model_guard: bool | None = None,
        rknn_terms: bytes | None = None,
        model_guard_runtime: bytes | None = None,
        model_file_name: str = "model.nn",
    ) -> pathlib.Path:
        if include_model_guard is None:
            include_model_guard = profile == "production-release"
        root = "cosmo-V1.5.0"
        directory = pathlib.Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, directory)
        initial = directory / f"{root}.tar.gz"
        executable_files = verifier.REQUIRED_EXECUTABLES
        regular_files = verifier.REQUIRED_FILES
        with tarfile.open(initial, "w:gz") as archive:
            root_info = tarfile.TarInfo(root)
            root_info.type = tarfile.DIRTYPE
            root_info.mode = 0o755
            archive.addfile(root_info)
            for name in sorted(verifier.REQUIRED_DIRS):
                info = tarfile.TarInfo(f"{root}/{name}")
                info.type = tarfile.DIRTYPE
                info.mode = 0o755
                archive.addfile(info)
            files = set(executable_files) | set(regular_files)
            files.add("lib/libavcodec.so.58.134.100")
            if target_chip in ("rk3576", "rv1126b"):
                files.update(verifier.RKNN_LICENSE_FILES)
            if include_model_guard:
                files.update(
                    {
                        "lib/libcosmo_model_guard.so.2.0.0",
                        verifier.MODEL_GUARD_TERMS_FILE,
                    }
                )
            if profile == "production-release":
                files.add("bin/cosmo-model-provision")
            rknn_guard = include_model_guard and target_chip == "rk3576"
            sophon_production = profile == "production-release" and not rknn_guard
            if sophon_production:
                files.update({SOPHON_MANIFEST_FILE, SOPHON_HEADER_FILE})
            guard_payload = b"RKNN Guard ABI1 fixture (not an admitted SDK)"
            header_payload = b"/* RKNN ABI1 fixture */"
            provision_payload = b"#!/bin/sh\n"
            if rknn_guard:
                files.discard(verifier.MODEL_GUARD_RUNTIME_FILE)
                files.update({verifier.MODEL_GUARD_RKNN_RUNTIME_FILE,
                              verifier.MODEL_GUARD_RUNTIME_HASH_FILE,
                              verifier.MODEL_GUARD_RKNN_SDK_MANIFEST_FILE,
                              verifier.MODEL_GUARD_RKNN_HEADER_FILE})
            for name in sorted(files):
                if name == omit_required_file:
                    continue
                if name == "share/cosmo/runtime-paths.env":
                    selected_data_dir = runtime_data_dir or (
                        "/userdata/cwaiuserdata"
                        if target_chip in ("rk3576", "rv1126b")
                        else "/data/cwaiuserdata"
                    )
                    data = (
                        f"COSMO_PACKAGE_DATA_DIR={selected_data_dir}\n"
                        f"COSMO_PACKAGE_APP_DATA_DIR={runtime_app_data_dir}\n"
                    ).encode()
                elif name in ("LICENSE", "share/licenses/cosmo-edge/LICENSE"):
                    data = b"Apache License\nVersion 2.0\nfixture\n"
                elif name in ("NOTICE", "share/licenses/cosmo-edge/NOTICE"):
                    data = b"CosmoEdge\nThird-party software\nfixture\n"
                elif name.endswith("ffmpeg/COPYING.LGPLv2.1"):
                    data = b"GNU LESSER GENERAL PUBLIC LICENSE\nfixture\n"
                elif name == "lib/libavcodec.so.58.134.100":
                    data = b"libavcodec license: LGPL version 2.1 or later\n"
                elif name.endswith("rknn-runtime/LICENSE"):
                    data = rknn_terms or (
                        b"RKNN SDK License\n"
                        b"1. License Grant\n"
                        b"redistribute its modifications or derivative works\n"
                        b"compatible with Products\n"
                    )
                elif rknn_guard and name == verifier.MODEL_GUARD_TERMS_FILE:
                    data = (b"explicitly authorized CosmoEdge fork\n"
                            b"not a general relicensing\n"
                            b"public distribution still requires a separate review\n")
                elif name == verifier.MODEL_GUARD_RKNN_RUNTIME_FILE:
                    data = guard_payload
                elif name == verifier.MODEL_GUARD_RKNN_HEADER_FILE:
                    data = header_payload
                elif name == verifier.MODEL_GUARD_RUNTIME_HASH_FILE:
                    data = (hashlib.sha256(guard_payload).hexdigest() + "  " +
                            verifier.MODEL_GUARD_RKNN_RUNTIME_FILE + "\n").encode()
                elif rknn_guard and name == verifier.MODEL_GUARD_RKNN_SDK_MANIFEST_FILE:
                    data = json.dumps({
                        "release_id": "cmg-sdk-v2.4.0-rknn-abi1",
                        "certificate_storage": "caller-selected",
                        "source": {"repository": "cosmo-model-guard", "tree": "a" * 40},
                        "components": {
                            "include/cosmo_model_guard_rknn_v1.h": hashlib.sha256(header_payload).hexdigest(),
                            "lib/libcosmo_model_guard_rknn.so.1.0.0": hashlib.sha256(guard_payload).hexdigest(),
                            "bin/cosmo-model-provision": hashlib.sha256(provision_payload).hexdigest(),
                        },
                    }).encode()
                elif sophon_production and name == SOPHON_MANIFEST_FILE:
                    data = json.dumps(verifier.SOPHON_RELEASE_MANIFEST).encode()
                elif sophon_production and name == SOPHON_HEADER_FILE:
                    data = (SOPHON_SDK / "include/cosmo_model_guard_v2.h").read_bytes()
                elif sophon_production and name == "bin/cosmo-model-provision":
                    data = (SOPHON_SDK / name).read_bytes()
                elif name == verifier.MODEL_GUARD_TERMS_FILE:
                    data = (
                        REPOSITORY / "prebuild/model-guard-v2/README.md"
                    ).read_bytes()
                elif name == verifier.MODEL_GUARD_RUNTIME_FILE:
                    data = (
                        model_guard_runtime
                        if model_guard_runtime is not None
                        else (
                            REPOSITORY
                            / "prebuild/model-guard-v2/lib/"
                            "libcosmo_model_guard.so.2.0.0"
                        ).read_bytes()
                    )
                elif name in verifier.REQUIRED_LICENSE_FILES:
                    data = b"fixture runtime license\n"
                else:
                    data = (
                        b"#!/bin/sh\n"
                        if name in executable_files or name.endswith("provision")
                        else b"V1.5.0\n"
                    )
                info = tarfile.TarInfo(f"{root}/{name}")
                info.size = len(data)
                info.mode = 0o755 if name in executable_files or name.endswith("provision") else 0o644
                archive.addfile(info, io.BytesIO(data))
            model_path = f"{root}/resource/models/preset/{model_file_name}"
            info = tarfile.TarInfo(model_path)
            info.size = len(model)
            info.mode = 0o644
            archive.addfile(info, io.BytesIO(model))
            config = json.dumps({"model_type": model_type}).encode()
            info = tarfile.TarInfo(f"{root}/resource/models/preset/config.json")
            info.size = len(config)
            info.mode = 0o644
            archive.addfile(info, io.BytesIO(config))
            if target_chip is not None:
                marker = f"{target_chip}\n".encode()
                info = tarfile.TarInfo(f"{root}/share/cosmo/target-chip.txt")
                info.size = len(marker)
                info.mode = 0o644
                archive.addfile(info, io.BytesIO(marker))
            if platform_chip is not None:
                platform_value: dict[str, object] = {
                    "chip": platform_chip,
                    "backend": "rknn",
                }
                if platform_runtime is not None:
                    platform_value["media"] = {
                        "runtime_profile": platform_runtime
                    }
                platform = json.dumps(platform_value).encode()
                info = tarfile.TarInfo(
                    f"{root}/share/cosmo/platform-profile.json"
                )
                info.size = len(platform)
                info.mode = 0o644
                archive.addfile(info, io.BytesIO(platform))
            if model_bundle_scope is not None:
                rknn = b"rv1126b-rknn-fixture"
                package_directory = "prod_RV1126B_9275710_YOLOV8_V1.0.0"
                package_model = f"{root}/resource/models/{package_directory}/model.rknn"
                info = tarfile.TarInfo(package_model)
                info.size = len(rknn)
                info.mode = 0o644
                archive.addfile(info, io.BytesIO(rknn))
                license_data = b"fixture model license\n"
                if include_bundle_license:
                    info = tarfile.TarInfo(
                        f"{root}/resource/licenses/model-assets/AGPL-3.0.txt"
                    )
                    info.size = len(license_data)
                    info.mode = 0o644
                    archive.addfile(info, io.BytesIO(license_data))
                bundle = json.dumps(
                    {
                        "schema_version": 1,
                        "chip": target_chip,
                        "usage_scope": model_bundle_scope,
                        "commercial_delivery": False,
                        "license": {
                            "spdx": "AGPL-3.0-only",
                            "path": "model-artifacts/LICENSES/AGPL-3.0.txt",
                            "sha256": hashlib.sha256(license_data).hexdigest(),
                        },
                        "models": [
                            {
                                "model": "yolov8",
                                "package_directory": package_directory,
                                "artifact": {
                                    "sha256": hashlib.sha256(rknn).hexdigest(),
                                    "size_bytes": len(rknn),
                                },
                            }
                        ],
                    }
                ).encode()
                info = tarfile.TarInfo(f"{root}/resource/model-bundle.json")
                info.size = len(bundle)
                info.mode = 0o644
                archive.addfile(info, io.BytesIO(bundle))
        digest = hashlib.md5(initial.read_bytes(), usedforsecurity=False).hexdigest()
        final = directory / f"{root}-{digest}.tar.gz"
        initial.rename(final)
        return final

    def test_open_accepts_plain_model(self) -> None:
        verifier.verify_package(self.make_package("public-runtime"), "public-runtime")

    def test_boot_log_cleanup_files_are_mandatory(self) -> None:
        for required in (
            "scripts/system-log-cleanup.sh",
            "scripts/system-log-retention.py",
            "scripts/cosmo-log-cleanup.service",
        ):
            with self.subTest(required=required):
                package = self.make_package("public-runtime", omit_required_file=required)
                with self.assertRaisesRegex(verifier.PackageAuditError, "required .*missing"):
                    verifier.verify_package(package, "public-runtime")

    def test_distribution_license_bundle_is_mandatory(self) -> None:
        for required in (
            "LICENSE",
            "NOTICE",
            "share/licenses/third-party/ffmpeg/COPYING.LGPLv2.1",
        ):
            with self.subTest(required=required):
                package = self.make_package(
                    "public-runtime", omit_required_file=required
                )
                with self.assertRaisesRegex(
                    verifier.PackageAuditError, "required file is missing"
                ):
                    verifier.verify_package(package, "public-runtime")

        rockchip_license = next(iter(verifier.RKNN_LICENSE_FILES))
        package = self.make_package(
            "public-runtime",
            target_chip="rv1126b",
            platform_chip="rv1126b",
            omit_required_file=rockchip_license,
        )
        with self.assertRaisesRegex(
            verifier.PackageAuditError, "runtime license is empty"
        ):
            verifier.verify_package(
                package, "public-runtime", target_chip="rv1126b"
            )

        model_guard = self.make_package(
            "public-runtime",
            target_chip="bm1688",
            include_model_guard=True,
            omit_required_file=verifier.MODEL_GUARD_TERMS_FILE,
        )
        with self.assertRaisesRegex(
            verifier.PackageAuditError, "runtime license is empty"
        ):
            verifier.verify_package(
                model_guard, "public-runtime", target_chip="bm1688"
            )

    def test_rockchip_rejects_incomplete_rknn_sdk_terms(self) -> None:
        package = self.make_package(
            "public-runtime",
            target_chip="rk3576",
            platform_chip="rk3576",
            platform_runtime="rk3576-mpp-rga",
            rknn_terms=b"Copyright Statement\nfixture\n",
        )
        with self.assertRaisesRegex(
            verifier.PackageAuditError, "RKNN runtime terms are invalid"
        ):
            verifier.verify_package(
                package, "public-runtime", target_chip="rk3576"
            )

    def test_model_guard_requires_approved_runtime_identity(self) -> None:
        approved = self.make_package(
            "public-runtime",
            target_chip="bm1688",
            include_model_guard=True,
        )
        verifier.verify_package(
            approved, "public-runtime", target_chip="bm1688"
        )

        unapproved = self.make_package(
            "public-runtime",
            target_chip="bm1688",
            include_model_guard=True,
            model_guard_runtime=b"unapproved-runtime\n",
        )
        with self.assertRaisesRegex(
            verifier.PackageAuditError, "not the approved artifact"
        ):
            verifier.verify_package(
                unapproved, "public-runtime", target_chip="bm1688"
            )

    def test_rv1126b_accepts_identified_community_example_bundle(self) -> None:
        package = self.make_package(
            "public-runtime",
            target_chip="rv1126b",
            platform_chip="rv1126b",
            model_bundle_scope="community-example",
        )
        verifier.verify_package(
            package,
            "public-runtime",
            target_chip="rv1126b",
        )

    def test_rv1126b_rejects_bundle_without_packaged_license(self) -> None:
        package = self.make_package(
            "public-runtime",
            target_chip="rv1126b",
            platform_chip="rv1126b",
            model_bundle_scope="community-example",
            include_bundle_license=False,
        )
        with self.assertRaisesRegex(
            verifier.PackageAuditError, "packaged model license is missing"
        ):
            verifier.verify_package(
                package,
                "public-runtime",
                target_chip="rv1126b",
            )

    def test_protected_accepts_encrypted_model(self) -> None:
        verifier.verify_package(
            self.make_package("production-release", b"CEMC" + b"encrypted"),
            "production-release",
        )

    def test_profile_checks_every_named_rknn_segment(self) -> None:
        protected = self.make_package(
            "production-release",
            b"CEMC" + b"encrypted",
            model_file_name="model0.rknn",
        )
        verifier.verify_package(protected, "production-release")

        for profile, payload in (
            ("production-release", b"plain"),
            ("public-runtime", b"CEMC" + b"encrypted"),
        ):
            with self.subTest(profile=profile):
                with self.assertRaises(verifier.PackageAuditError):
                    verifier.verify_package(
                        self.make_package(
                            profile,
                            payload,
                            model_file_name="model1.rknn",
                        ),
                        profile,
                    )

    def test_protected_rejects_plain_vllm_model(self) -> None:
        for model_type in ("qwen3vl", "qwen3_5"):
            with self.subTest(model_type=model_type):
                with self.assertRaises(verifier.PackageAuditError):
                    verifier.verify_package(
                        self.make_package("production-release", model_type=model_type),
                        "production-release",
                    )

    def test_channels_reject_each_others_model_format(self) -> None:
        with self.assertRaises(verifier.PackageAuditError):
            verifier.verify_package(
                self.make_package("public-runtime", b"CEMCencrypted"), "public-runtime"
            )
        with self.assertRaises(verifier.PackageAuditError):
            verifier.verify_package(
                self.make_package("production-release", b"plain"), "production-release"
            )

    def test_target_chip_and_rockchip_profile_are_bound_to_archive(self) -> None:
        with self.assertRaisesRegex(verifier.PackageAuditError, "marker is missing"):
            verifier.verify_package(
                self.make_package("public-runtime"), "public-runtime", "rv1126b"
            )

        with self.assertRaisesRegex(
            verifier.PackageAuditError, "platform profile is missing"
        ):
            verifier.verify_package(
                self.make_package("public-runtime", target_chip="rv1126b"),
                "public-runtime",
                "rv1126b",
            )

        package = self.make_package(
            "public-runtime", target_chip="rv1126b", platform_chip="rv1126b"
        )
        verifier.verify_package(package, "public-runtime", "rv1126b")
        verifier.verify_package(
            package,
            "public-runtime",
            "rv1126b",
            {
                "required_package_paths": ["share/cosmo/platform-profile.json"],
                "forbidden_package_paths": ["lib/librkllmrt.so"],
            },
        )

        with self.assertRaisesRegex(
            verifier.PackageAuditError, "required target package path is missing"
        ):
            verifier.verify_package(
                package,
                "public-runtime",
                "rv1126b",
                {"required_package_paths": ["share/licenses/missing"]},
            )
        with self.assertRaisesRegex(
            verifier.PackageAuditError, "forbidden target package path is present"
        ):
            verifier.verify_package(
                package,
                "public-runtime",
                "rv1126b",
                {"forbidden_package_paths": ["share/cosmo/target-chip.txt"]},
            )

        runtime_package = self.make_package(
            "public-runtime",
            target_chip="rv1126b",
            platform_chip="rv1126b",
            platform_runtime="runtime-a",
        )
        with self.assertRaisesRegex(
            verifier.PackageAuditError, "media runtime profile does not match"
        ):
            verifier.verify_package(
                runtime_package,
                "public-runtime",
                "rv1126b",
                {"media_runtime_profile": "runtime-b"},
            )

        with self.assertRaisesRegex(verifier.PackageAuditError, "target chip mismatch"):
            verifier.verify_package(package, "public-runtime", "rk3576")

        wrong_platform = self.make_package(
            "public-runtime", target_chip="rv1126b", platform_chip="rk3576"
        )
        with self.assertRaisesRegex(
            verifier.PackageAuditError, "platform profile does not match"
        ):
            verifier.verify_package(wrong_platform, "public-runtime", "rv1126b")

    def test_runtime_paths_match_target_chip(self) -> None:
        for target_chip in ("rk3576", "rv1126b"):
            package = self.make_package(
                "public-runtime",
                target_chip=target_chip,
                platform_chip=target_chip,
            )
            verifier.verify_package(package, "public-runtime", target_chip)

        # rk3588 preview keeps the default data root (CosmoRuntimePaths.cmake
        # only routes rk3576/rv1126b to /userdata) and binds a platform profile.
        rk3588 = self.make_package(
            "public-runtime",
            target_chip="rk3588",
            platform_chip="rk3588",
        )
        verifier.verify_package(rk3588, "public-runtime", "rk3588")

        rk3588_wrong_root = self.make_package(
            "public-runtime",
            target_chip="rk3588",
            platform_chip="rk3588",
            runtime_data_dir="/userdata/cwaiuserdata",
        )
        with self.assertRaisesRegex(
            verifier.PackageAuditError, "runtime data directory does not match"
        ):
            verifier.verify_package(rk3588_wrong_root, "public-runtime", "rk3588")

        sophon = self.make_package("public-runtime", target_chip="bm1688")
        verifier.verify_package(sophon, "public-runtime", "bm1688")

        wrong_data_root = self.make_package(
            "public-runtime",
            target_chip="rv1126b",
            platform_chip="rv1126b",
            runtime_data_dir="/data/cwaiuserdata",
        )
        with self.assertRaisesRegex(
            verifier.PackageAuditError, "runtime data directory does not match"
        ):
            verifier.verify_package(wrong_data_root, "public-runtime", "rv1126b")

        wrong_app_root = self.make_package(
            "public-runtime",
            target_chip="bm1688",
            runtime_app_data_dir="/userdata/cwai_data",
        )
        with self.assertRaisesRegex(
            verifier.PackageAuditError, "application directory is incompatible"
        ):
            verifier.verify_package(wrong_app_root, "public-runtime", "bm1688")

    def mutate_package(self, package, changes, reverse=False, additions=None):
        """Rewrite members and always recalculate MD5 so negatives reach the audit."""
        with tarfile.open(package, "r:gz") as source:
            members = [(m, source.extractfile(m).read() if m.isreg() else None)
                       for m in source]
        seen = set()
        rewritten = package.parent / "rewritten.tar.gz"
        with tarfile.open(rewritten, "w:gz") as target:
            for member, data in reversed(members) if reverse else members:
                name = member.name.split("/", 1)[-1]
                if name in changes:
                    seen.add(name)
                    change = changes[name]
                    if change is None:
                        continue
                    if isinstance(change, bytes):
                        data = change
                        member.size = len(data)
                    elif change == "directory":
                        member.type, member.size, data = tarfile.DIRTYPE, 0, None
                    elif change == "symlink":
                        member.type, member.size, data = tarfile.SYMTYPE, 0, None
                        member.linkname = "cosmo"
                    elif change == "not-executable":
                        member.mode = 0o644
                    else:
                        raise AssertionError(change)
                target.addfile(member, io.BytesIO(data) if data is not None else None)
            for name, data in (additions or {}).items():
                self.assertNotIn(name, {m.name.split("/", 1)[-1] for m, _ in members})
                member = tarfile.TarInfo("cosmo-V1.5.0/" + name)
                member.mode, member.size = 0o644, len(data)
                target.addfile(member, io.BytesIO(data))
        self.assertEqual(seen, set(changes), "mutation must affect every requested member")
        digest = hashlib.md5(rewritten.read_bytes(), usedforsecurity=False).hexdigest()
        final = package.parent / f"cosmo-V1.5.0-{digest}.tar.gz"
        rewritten.replace(final)
        return final

    def protected_fixture(self, chip):
        return self.make_package(
            "production-release", b"CEMCfixture", target_chip=chip,
            platform_chip=chip if chip == "rk3576" else None,
            platform_runtime="rk3576-mpp-rga" if chip == "rk3576" else None,
        )

    def test_explicit_protected_targets_and_member_order(self):
        for chip in ("bm1688", "rk3576"):
            with self.subTest(chip=chip):
                package = self.protected_fixture(chip)
                verifier.verify_package(package, "production-release", chip)
                verifier.verify_package(self.mutate_package(package, {}, reverse=True),
                                        "production-release", chip)

    def test_protected_requires_regular_matching_guard_and_executable_provisioner(self):
        for chip in ("bm1688", "cv186x", "rk3576", None, "unspecified"):
            guard = (verifier.MODEL_GUARD_RKNN_RUNTIME_FILE if chip == "rk3576"
                     else verifier.MODEL_GUARD_RUNTIME_FILE)
            for member in (guard, "bin/cosmo-model-provision"):
                for change in (None, "directory", "symlink"):
                    with self.subTest(chip=chip, member=member, change=change):
                        package = self.protected_fixture(chip)
                        verifier.verify_package(package, "production-release", chip)
                        negative = self.mutate_package(package, {member: change})
                        with self.assertRaisesRegex(verifier.PackageAuditError,
                                                    "Guard runtime|provision|SDK component"):
                            verifier.verify_package(negative, "production-release", chip)
            package = self.mutate_package(self.protected_fixture(chip),
                                         {"bin/cosmo-model-provision": "not-executable"})
            with self.assertRaisesRegex(verifier.PackageAuditError, "provision"):
                verifier.verify_package(package, "production-release", chip)

    def test_plaintext_and_missing_members_reach_intended_audit(self):
        for chip in ("bm1688", "rk3576"):
            package = self.protected_fixture(chip)
            for member in ("resource/models/preset/model.nn",):
                with self.subTest(chip=chip, member=member):
                    negative = self.mutate_package(package, {member: b"plain"})
                    with self.assertRaisesRegex(verifier.PackageAuditError, "plaintext"):
                        verifier.verify_package(negative, "production-release", chip)
            for member in verifier.REQUIRED_FILES | verifier.REQUIRED_EXECUTABLES:
                with self.subTest(chip=chip, member=member):
                    negative = self.mutate_package(package, {member: None})
                    with self.assertRaisesRegex(verifier.PackageAuditError, "required .*missing"):
                        verifier.verify_package(negative, "production-release", chip)

    def test_every_encrypted_segment_is_audited(self):
        for chip in ("bm1688", "rk3576"):
            names = ["resource/models/preset/model.nn", "resource/models/preset/model0.rknn",
                     "resource/models/preset/model1.rknn", "resource/models/other/model.nn"]
            package = self.mutate_package(self.protected_fixture(chip), {}, additions={
                name: b"CEMCencrypted segment" for name in names[1:]})
            verifier.verify_package(package, "production-release", chip)
            for name in names:
                with self.subTest(chip=chip, name=name):
                    negative = self.mutate_package(package, {name: b"plaintext segment"})
                    with self.assertRaisesRegex(verifier.PackageAuditError, "plaintext preset model: " + name):
                        verifier.verify_package(negative, "production-release", chip)

    def test_archive_md5_and_controlled_names_are_enforced(self):
        package = self.protected_fixture("bm1688")
        invalid_name = package.parent / ("cosmo-V1.5.0-" + "0" * 32 + ".tar.gz")
        shutil.copy2(package, invalid_name)
        with self.assertRaisesRegex(verifier.PackageAuditError, "MD5"):
            verifier.verify_package(invalid_name, "production-release", "bm1688")
        for filename in ("device-certificate.bin", "product-model-key-v1.bin",
                         "commissioning-ed25519.seed", "product-pepper-v1.bin"):
            negative = self.mutate_package(package, {}, additions={"resource/" + filename: b"opaque"})
            with self.assertRaisesRegex(verifier.PackageAuditError, "controlled secret"):
                verifier.verify_package(negative, "production-release", "bm1688")

    def test_sophon_production_manifest_formatting_and_identity(self):
        package = self.protected_fixture("bm1688")
        manifest = copy.deepcopy(verifier.SOPHON_RELEASE_MANIFEST)
        equivalent = json.dumps(manifest, indent=4, sort_keys=True).encode()
        verifier.verify_package(
            self.mutate_package(package, {SOPHON_MANIFEST_FILE: equivalent}),
            "production-release", "bm1688",
        )
        for field, value in (("release_id", "old-release"), ("unexpected", True)):
            with self.subTest(field=field):
                changed = dict(manifest, **{field: value})
                negative = self.mutate_package(
                    package, {SOPHON_MANIFEST_FILE: json.dumps(changed).encode()}
                )
                with self.assertRaisesRegex(verifier.PackageAuditError, "manifest does not match"):
                    verifier.verify_package(negative, "production-release", "bm1688")

    def test_sophon_production_requires_manifest_and_header(self):
        package = self.protected_fixture("bm1688")
        for member in (SOPHON_MANIFEST_FILE, SOPHON_HEADER_FILE):
            for change in (None, "directory", "symlink", b"invalid"):
                with self.subTest(member=member, change=change):
                    negative = self.mutate_package(package, {member: change})
                    with self.assertRaisesRegex(
                        verifier.PackageAuditError, "manifest|SDK component|header"
                    ):
                        verifier.verify_package(negative, "production-release", "bm1688")

    def test_sophon_production_rejects_mismatched_components(self):
        package = self.protected_fixture("bm1688")
        for member in (
            verifier.MODEL_GUARD_RUNTIME_FILE,
            "bin/cosmo-model-provision",
            SOPHON_HEADER_FILE,
        ):
            with self.subTest(member=member):
                negative = self.mutate_package(package, {member: b"old-sdk-component"})
                with self.assertRaisesRegex(
                    verifier.PackageAuditError, "approved artifact|hash|SHA|SDK component"
                ):
                    verifier.verify_package(negative, "production-release", "bm1688")

    def test_sophon_rejects_self_consistent_unapproved_sdk(self):
        package = self.protected_fixture("bm1688")
        manifest = copy.deepcopy(verifier.SOPHON_RELEASE_MANIFEST)
        payload = b"unapproved-provisioner"
        manifest["components"]["bin/cosmo-model-provision"] = hashlib.sha256(payload).hexdigest()
        negative = self.mutate_package(package, {
            "bin/cosmo-model-provision": payload,
            SOPHON_MANIFEST_FILE: json.dumps(manifest).encode(),
        })
        with self.assertRaisesRegex(verifier.PackageAuditError, "manifest does not match"):
            verifier.verify_package(negative, "production-release", "bm1688")

    def test_rknn_manifest_formatting_and_identity(self):
        package = self.protected_fixture("rk3576")
        name = verifier.MODEL_GUARD_RKNN_SDK_MANIFEST_FILE
        with tarfile.open(package) as archive:
            manifest = json.load(archive.extractfile("cosmo-V1.5.0/" + name))
        equivalent = json.dumps(manifest, indent=4, sort_keys=True).encode()
        verifier.verify_package(self.mutate_package(package, {name: equivalent}),
                                "production-release", "rk3576")
        manifest["release_id"] = "incorrect-identity"
        with self.assertRaisesRegex(verifier.PackageAuditError, "manifest does not match"):
            verifier.verify_package(self.mutate_package(package, {name: json.dumps(manifest).encode()}),
                                    "production-release", "rk3576")

    def test_rv1126b_protected_remains_unsupported(self):
        package = self.make_package("production-release", b"CEMCfixture",
                                    target_chip="rv1126b", platform_chip="rv1126b")
        with self.assertRaisesRegex(verifier.PackageAuditError, "RV1126B packages are unsupported"):
            verifier.verify_package(package, "production-release", "rv1126b")

    def test_compatibility_target_does_not_count_as_explicit_chip(self):
        for chip in (None, "unspecified"):
            package = self.protected_fixture(chip)
            verifier.verify_package(package, "production-release")
            with self.assertRaisesRegex(verifier.PackageAuditError, "marker is missing|target chip mismatch"):
                verifier.verify_package(package, "production-release", "bm1688")

        self.assertIn("    defaults:\n      run:\n        shell: bash\n", build_job)
        self.assertIn("set -euo pipefail", build_job)


    def test_obsolete_release_payload_and_secrets_rejected(self):
        package = self.protected_fixture("bm1688")
        for name in (".release-bootstrap/old", "bin/release_updater", "resource/models/preset/device.cert"):
            with tarfile.open(package) as source:
                members = [(m, source.extractfile(m).read() if m.isreg() else None) for m in source]
            initial = package.parent / "extra.tar.gz"
            with tarfile.open(initial, "w:gz") as target:
                for member, data in members:
                    target.addfile(member, io.BytesIO(data) if data is not None else None)
                data = b"-----BEGIN PRIVATE KEY-----" if name.endswith("cert") else b"old"
                member = tarfile.TarInfo("cosmo-V1.5.0/" + name)
                member.size = len(data)
                target.addfile(member, io.BytesIO(data))
            digest = hashlib.md5(initial.read_bytes(), usedforsecurity=False).hexdigest()
            final = initial.parent / f"cosmo-V1.5.0-{digest}.tar.gz"
            initial.replace(final)
            with self.assertRaisesRegex(verifier.PackageAuditError, "obsolete|private key"):
                verifier.verify_package(final, "production-release", "bm1688")

    def test_nightly_sophon_build_retries_cargo_downloads(self) -> None:
        workflow = (
            REPOSITORY / ".github/workflows/nightly-build-test-sophon.yml"
        ).read_text(encoding="utf-8")
        build_job = workflow.split("  build-sophon:", 1)[1].split(
            "\n  test-sophon:", 1
        )[0]

        self.assertIn('CARGO_HTTP_TIMEOUT: "120"', build_job)
        self.assertIn('CARGO_NET_RETRY: "5"', build_job)

    def test_nightly_sophon_escapes_generated_catch2_filters(self) -> None:
        workflow = (
            REPOSITORY / ".github/workflows/nightly-build-test-sophon.yml"
        ).read_text(encoding="utf-8")

        self.assertIn("write_catch2_test_spec()", workflow)
        self.assertIn('escaped="${escaped//,/\\\\,}"', workflow)
        self.assertEqual(workflow.count('write_catch2_test_spec "$name"'), 2)

    def test_container_builds_bound_and_cache_npm_connections(self) -> None:
        npmrc = (REPOSITORY / "src/web/.npmrc").read_text(encoding="utf-8")
        self.assertIn("registry=https://registry.npmmirror.com/", npmrc)
        self.assertIn("maxsockets=1", npmrc)
        self.assertIn("prefer-offline=true", npmrc)
        self.assertIn("update-notifier=false", npmrc)

        lock = json.loads(
            (REPOSITORY / "src/web/package-lock.json").read_text(encoding="utf-8")
        )
        locked_packages = [
            metadata
            for path, metadata in lock["packages"].items()
            if "node_modules/" in path and not metadata.get("link")
        ]
        self.assertTrue(locked_packages)
        self.assertTrue(all(package.get("integrity") for package in locked_packages))
        self.assertTrue(
            all(
                package.get("resolved", "").startswith(
                    "https://cdn.npmmirror.com/packages/"
                )
                for package in locked_packages
            )
        )

        npm_builder = (
            REPOSITORY / "scripts/build_npm_dependencies.sh"
        ).read_text(encoding="utf-8")
        self.assertIn("cache add", npm_builder)
        self.assertIn("ci --offline", npm_builder)
        self.assertIn("package-lock.json has incomplete entries", npm_builder)

        web_cmake = (REPOSITORY / "cmake/web_frontend.cmake").read_text(
            encoding="utf-8"
        )
        self.assertIn("build_npm_dependencies.sh", web_cmake)
        self.assertIn("set(WEB_ENTRY", web_cmake)
        self.assertIn("OUTPUT  ${WEB_ENTRY}", web_cmake)
        self.assertIn(
            "add_custom_target(web_frontend ALL DEPENDS ${WEB_ENTRY})", web_cmake
        )
        self.assertIn(
            "add_dependencies(web_frontend ${EXECUTABLE_NAME})", web_cmake
        )
        self.assertNotIn("WEB_STAMP", web_cmake)
        self.assertNotIn("web_unified.stamp", web_cmake)

        for compose_name in ("docker-compose.sophon.yml", "docker-compose.rockchip.yml"):
            compose = (REPOSITORY / compose_name).read_text(encoding="utf-8")
            self.assertIn('NPM_CONFIG_MAXSOCKETS: "${NPM_CONFIG_MAXSOCKETS:-1}"', compose)
            self.assertIn("cosmo-npm-cache:/root/.npm", compose)
            self.assertIn('NPM_CONFIG_PREFER_OFFLINE: "${NPM_CONFIG_PREFER_OFFLINE:-true}"', compose)

    def test_sophon_chip_selection_preserves_package_output_contract(self) -> None:
        entrypoint = (REPOSITORY / "scripts/build_sophon_package.sh").read_text(
            encoding="utf-8"
        )
        cmake = (REPOSITORY / "CMakeLists.txt").read_text(encoding="utf-8")

        self.assertIn(
            'output_dir="/build_output/${COSMO_MODEL_GUARD_BUILD_PROFILE}/${chip}"',
            entrypoint,
        )
        self.assertIn("package_artifacts=(build/packages/*.tar.gz)", entrypoint)
        self.assertIn(
            'printf \'%s\\n\' "${chip}" > "${output_dir}/TARGET_CHIP"', entrypoint
        )
        self.assertIn('sha256sum -- "${package_name}" > SHA256SUMS', entrypoint)
        self.assertIn('DESTINATION share/cosmo', cmake)
        self.assertIn('"${COSMO_TARGET_CHIP_NORMALIZED}\\n"', cmake)
        self.assertIn('set(CPACK_OUTPUT_FILE_PREFIX', cmake)
        self.assertIn('scripts/package_md5_rename.sh', cmake)

    def test_sophon_package_entrypoint_behavior(self) -> None:
        bash, test_environment = self.find_bash()
        if not bash:
            self.skipTest("bash is not available")
        test_environment.pop("COSMO_MODEL_GUARD_BUILD_PROFILE", None)

        source = (REPOSITORY / "scripts/build_sophon_package.sh").read_text(
            encoding="utf-8"
        )
        source = source.replace(
            'output_dir="/build_output/${COSMO_MODEL_GUARD_BUILD_PROFILE}/${chip}"',
            'output_dir="$PWD/export/${COSMO_MODEL_GUARD_BUILD_PROFILE}/${chip}"',
        )

        with tempfile.TemporaryDirectory(dir=REPOSITORY) as temporary_directory:
            workspace = pathlib.Path(temporary_directory)
            scripts = workspace / "scripts"
            scripts.mkdir()
            (workspace / "build/packages").mkdir(parents=True)
            entrypoint = scripts / "build_sophon_package.sh"
            write_text_lf(entrypoint, source)
            build_stub = scripts / "build.sh"
            write_text_lf(
                build_stub,
                "#!/bin/bash\n"
                "set -euo pipefail\n"
                'printf "%s\\n" "$*" > build-invocation.txt\n'
                "printf package > "
                "build/packages/cosmo-V1.1.0-deadbeef.tar.gz\n",
            )
            entrypoint.chmod(0o755)
            build_stub.chmod(0o755)

            def run(*arguments: str) -> subprocess.CompletedProcess[str]:
                return subprocess.run(
                    [bash, "scripts/build_sophon_package.sh", *arguments],
                    cwd=workspace,
                    env=test_environment,
                    check=False,
                    capture_output=True,
                    text=True,
                    encoding="utf-8",
                )

            default_result = run()
            self.assertEqual(default_result.returncode, 0, default_result.stderr)
            self.assertEqual(
                (workspace / "build-invocation.txt").read_text(encoding="utf-8"),
                "-T -c bm1688\n",
            )
            exported = (
                workspace
                / "export/public-runtime/bm1688/cosmo-V1.1.0-deadbeef.tar.gz"
            )
            self.assertEqual(exported.read_text(encoding="utf-8"), "package")
            self.assertEqual(
                (workspace / "export/public-runtime/bm1688/TARGET_CHIP").read_text(
                    encoding="utf-8"
                ),
                "bm1688\n",
            )

            explicit_bm1688_result = run("--chip", "bm1688")
            self.assertEqual(
                explicit_bm1688_result.returncode, 0, explicit_bm1688_result.stderr
            )
            self.assertEqual(
                (workspace / "build-invocation.txt").read_text(encoding="utf-8"),
                "-T -c bm1688\n",
            )

            cv186x_result = run("--chip", "cv186x")
            self.assertEqual(cv186x_result.returncode, 0, cv186x_result.stderr)
            self.assertEqual(
                (workspace / "build-invocation.txt").read_text(encoding="utf-8"),
                "-T -c cv186x\n",
            )
            cv186x_exported = (
                workspace
                / "export/public-runtime/cv186x/cosmo-V1.1.0-deadbeef.tar.gz"
            )
            self.assertEqual(cv186x_exported.read_text(encoding="utf-8"), "package")
            self.assertEqual(
                (workspace / "export/public-runtime/cv186x/TARGET_CHIP").read_text(
                    encoding="utf-8"
                ),
                "cv186x\n",
            )

            test_environment["COSMO_MODEL_GUARD_BUILD_PROFILE"] = (
                "production-release"
            )
            production_result = run("--chip", "bm1688")
            self.assertEqual(
                production_result.returncode, 0, production_result.stderr
            )
            production_exported = (
                workspace
                / "export/production-release/bm1688/cosmo-V1.1.0-deadbeef.tar.gz"
            )
            self.assertEqual(
                production_exported.read_text(encoding="utf-8"), "package"
            )

            invalid_result = run("--chip", "unsupported-chip")
            self.assertNotEqual(invalid_result.returncode, 0)
            self.assertIn("unsupported Sophon chip", invalid_result.stderr)

    def test_sophon_build_resolves_chip_resource_directory(self) -> None:
        bash, test_environment = self.find_bash()
        if not bash:
            self.skipTest("bash is not available")

        with tempfile.TemporaryDirectory(dir=REPOSITORY) as temporary_directory:
            project_root = pathlib.Path(temporary_directory)
            scripts = project_root / "scripts"
            scripts.mkdir()
            build_script = scripts / "build.sh"
            write_text_lf(
                build_script,
                (REPOSITORY / "scripts/build.sh")
                .read_text(encoding="utf-8")
                .replace(
                    'if [ -z "${PROJECT_ROOT_PATH:-}" ]; then',
                    'PROJECT_ROOT_PATH="$(cd "$(dirname "$0")/.." && pwd -P)"\n'
                    'if [ -z "${PROJECT_ROOT_PATH:-}" ]; then',
                ),
            )
            build_script.chmod(0o755)
            for chip in ("bm1688", "cv186x"):
                (project_root / f"data/resource/aiboxresource_{chip}").mkdir(
                    parents=True
                )

            def run(*arguments: str) -> subprocess.CompletedProcess[str]:
                return subprocess.run(
                    [bash, "scripts/build.sh", *arguments],
                    cwd=project_root,
                    env=test_environment,
                    check=False,
                    capture_output=True,
                    text=True,
                    encoding="utf-8",
                )

            default_result = run()
            self.assertIn(
                "Sophon chip: bm1688",
                default_result.stdout,
                default_result.stderr,
            )
            self.assertIn("aiboxresource_bm1688", default_result.stdout)

            cv186x_result = run("-c", "cv186x")
            self.assertIn("Sophon chip: cv186x", cv186x_result.stdout)
            self.assertIn("aiboxresource_cv186x", cv186x_result.stdout)

            invalid_result = run("-c", "unsupported-chip")
            self.assertNotEqual(invalid_result.returncode, 0)
            self.assertIn("unsupported Sophon chip", invalid_result.stderr)

            conflict_result = run("-c", "bm1688", "-m", "resource")
            self.assertNotEqual(conflict_result.returncode, 0)
            self.assertIn("-c and -m cannot be used together", conflict_result.stderr)

    def test_shared_rockchip_builder_has_isolated_target_profiles(self) -> None:
        compose = (REPOSITORY / "docker-compose.rockchip.yml").read_text(
            encoding="utf-8"
        )
        compatibility_compose = (
            REPOSITORY / "docker-compose.rk3576.yml"
        ).read_text(encoding="utf-8")
        compatibility_dockerfile = (REPOSITORY / "Dockerfile.rk3576").read_text(
            encoding="utf-8"
        )
        dockerfile = (REPOSITORY / "Dockerfile.rockchip").read_text(
            encoding="utf-8"
        )
        workflow = (REPOSITORY / ".github/workflows/ci-build-rockchip.yml").read_text(
            encoding="utf-8"
        )
        entrypoint = (REPOSITORY / "scripts/build_rockchip_package.sh").read_text(
            encoding="utf-8"
        )
        dockerignore = (REPOSITORY / ".dockerignore").read_text(encoding="utf-8")
        builder_dockerignore = (
            REPOSITORY / "Dockerfile.rockchip.dockerignore"
        ).read_text(encoding="utf-8")
        builder_lock = json.loads(
            (REPOSITORY / "config/rockchip-build/builder-lock.json").read_text(
                encoding="utf-8"
            )
        )
        build = (REPOSITORY / "scripts/build_rknn.sh").read_text(encoding="utf-8")
        root_cmake = (REPOSITORY / "CMakeLists.txt").read_text(encoding="utf-8")
        cmake = (REPOSITORY / "cmake/rkllm.cmake").read_text(encoding="utf-8")
        media_cmake = (REPOSITORY / "cmake/rockchip_media.cmake").read_text(
            encoding="utf-8"
        )

        pinned_builder = (
            "ghcr.io/cosmo-wander-ai/cosmo_edge-build-env_rockchip@sha256:"
            "0810c23042cbe86d3a1c91f848b9849a34d94222f3ad7b7418913a26da19e71b"
        )
        self.assertIn(pinned_builder, compose)
        self.assertIn(pinned_builder, compatibility_dockerfile)
        self.assertFalse(
            (REPOSITORY / ".github/workflows/ci-build-rk3576.yml").exists()
        )
        self.assertIn("scripts/build_rockchip_package.sh", compose)
        self.assertIn("COSMO_TARGET_CHIP", compose)
        self.assertIn("file: docker-compose.rockchip.yml", compatibility_compose)
        self.assertIn('command: ["--chip", "rk3576"', compatibility_compose)
        self.assertIn("rk3576", workflow)
        self.assertIn("rv1126b", workflow)
        self.assertRegex(
            workflow,
            r"- chip: rv1126b\s+models: include",
        )
        self.assertIn("cosmo_edge-build-env_rockchip", workflow)
        self.assertIn("packages: write", workflow)
        workflow_triggers = workflow.split("\npermissions:", 1)[0]
        self.assertIn("  schedule:", workflow_triggers)
        self.assertIn("    - cron: '12 18 * * *'", workflow_triggers)
        self.assertIn("  workflow_dispatch:", workflow_triggers)
        for disabled_trigger in (
            "  pull_request:",
            "  push:",
            "  workflow_call:",
        ):
            self.assertNotIn(disabled_trigger, workflow_triggers)
        self.assertIn(
            "if: github.event_name == 'workflow_dispatch'",
            workflow,
        )
        self.assertIn(
            '(cd "build_output/${CHIP}" && sha256sum -c SHA256SUMS)',
            workflow,
        )
        self.assertIn("878f9361fd3afa7e167b7079918918f78d2c1c2a", dockerfile)
        self.assertIn("install_rkllm_sdk.py", dockerfile)
        self.assertIn("/opt/rockchip-media/rk3576", dockerfile)
        self.assertIn("/opt/rockchip-media/rv1126b", dockerfile)
        self.assertIn("-ffile-prefix-map=", dockerfile)
        self.assertIn("--chip <rk3576|rk3588|rv1126b>", entrypoint)
        self.assertIn("--target-chip", entrypoint)
        self.assertIn("/build_rknn", dockerignore.splitlines())
        self.assertIn("/3rd/srs-*/trunk/objs", dockerignore.splitlines())
        self.assertEqual(builder_dockerignore.splitlines()[0], "**")
        self.assertIn("!config/rockchip-build/**", builder_dockerignore.splitlines())
        self.assertIn("!scripts/install_rkllm_sdk.py", builder_dockerignore.splitlines())
        self.assertTrue(builder_lock["targets"]["rk3576"]["rkllm_required"])
        self.assertFalse(builder_lock["targets"]["rv1126b"]["rkllm_required"])
        self.assertEqual(builder_lock["common"]["rkllm"]["version"], "1.3.0")
        self.assertIn(builder_lock["common"]["rkllm"]["revision"], dockerfile)
        self.assertIn(
            "share/licenses/rockchip-media/librga/COPYING",
            builder_lock["targets"]["rv1126b"]["required_package_paths"],
        )
        self.assertIn(
            "lib/librkllmrt.so",
            builder_lock["targets"]["rv1126b"]["forbidden_package_paths"],
        )
        self.assertIn("--target-policy-lock", entrypoint)
        self.assertNotEqual(
            builder_lock["targets"]["rk3576"]["media_root"],
            builder_lock["targets"]["rv1126b"]["media_root"],
        )
        self.assertIn('lib/librkllmrt.so LICENSE', build)
        self.assertIn('-DCOSMO_TARGET_CHIP="${TARGET_CHIP}"', build)
        self.assertIn('[-c rk3576|rk3588|rv1126b]', build)
        self.assertIn('-DCOSMO_RKLLM_REQUIRED="${RKLLM_REQUIRED}"', build)
        self.assertGreaterEqual(
            root_cmake.count('PATTERN "model-artifacts" EXCLUDE'), 2
        )
        self.assertIn('set(RKLLM_RUNTIME_LICENSE "${COSMO_RKLLM_ROOT}/LICENSE")', cmake)
        self.assertIn("media_sysroot_lock.py", build)
        self.assertIn("rockchip-media-manifest.json", media_cmake)

    def test_rknn_platform_profiles_share_backend_and_separate_artifacts(self) -> None:
        rk3576 = json.loads(
            (REPOSITORY / "config/rknn/platforms/rk3576.json").read_text(
                encoding="utf-8"
            )
        )
        rv1126b = json.loads(
            (REPOSITORY / "config/rknn/platforms/rv1126b.json").read_text(
                encoding="utf-8"
            )
        )
        toolchain_lock = json.loads(
            (REPOSITORY / "config/rknn/toolchain-lock.json").read_text(
                encoding="utf-8"
            )
        )
        builder_lock = json.loads(
            (REPOSITORY / "config/rockchip-build/builder-lock.json").read_text(
                encoding="utf-8"
            )
        )
        runtime_lock = json.loads(
            (REPOSITORY / "config/rockchip-media/runtime-lock.json").read_text(
                encoding="utf-8"
            )
        )
        for profile, chip in ((rk3576, "rk3576"), (rv1126b, "rv1126b")):
            self.assertEqual(profile["backend"], "rknn")
            self.assertEqual(profile["chip"], chip)
            self.assertEqual(profile["conversion"]["target_platform"], chip)
            self.assertTrue(profile["media"]["cpu_fallback"])
            self.assertEqual(profile["media"]["default_backend"], "rockchip")
            self.assertEqual(
                profile["media"]["runtime_lock"],
                "../../rockchip-media/runtime-lock.json",
            )
            self.assertTrue(profile["qualification"]["requires_target_bound_evidence"])
            self.assertEqual(
                profile["qualification"]["status"],
                toolchain_lock["qualification"][chip]["status"],
            )
        self.assertNotEqual(
            rk3576["media"]["runtime_profile"],
            rv1126b["media"]["runtime_profile"],
        )
        self.assertNotEqual(
            rk3576["packaging"]["legacy_models_directory"],
            rv1126b["packaging"]["legacy_models_directory"],
        )
        rv_artifact_manifest_path = (
            REPOSITORY / rv1126b["packaging"]["artifact_manifest"]
        )
        rv_artifact_manifest = json.loads(
            rv_artifact_manifest_path.read_text(encoding="utf-8")
        )
        self.assertEqual(rv_artifact_manifest["chip"], "rv1126b")
        self.assertEqual(
            rv_artifact_manifest["usage_scope"], "community-example"
        )
        self.assertFalse(rv_artifact_manifest["commercial_delivery"])
        self.assertEqual(
            {record["model"] for record in rv_artifact_manifest["models"]},
            {"helmet", "yolov8"},
        )
        for record in rv_artifact_manifest["models"]:
            artifact = record["artifact"]
            artifact_path = REPOSITORY / artifact["path"]
            self.assertEqual(artifact_path.stat().st_size, artifact["size_bytes"])
            self.assertEqual(
                hashlib.sha256(artifact_path.read_bytes()).hexdigest(),
                artifact["sha256"],
            )
        for profile in (rk3576, rv1126b):
            chip = profile["chip"]
            self.assertEqual(
                builder_lock["targets"][chip]["media_runtime_profile"],
                profile["media"]["runtime_profile"],
            )
            self.assertIn(
                profile["media"]["runtime_profile"], runtime_lock["runtimes"]
            )
        rv_runtime = runtime_lock["runtimes"][rv1126b["media"]["runtime_profile"]]
        self.assertEqual(
            rv_runtime["artifacts"]["lib/librockchip_mpp.so.0"]["sha256"],
            "b3f15d57a7516bab1e6167b8244afaff8f27b0b7d34813328db8420a7019820b",
        )

    def test_rk3588_preview_binds_cpu_media_to_frozen_bullseye_builder(self) -> None:
        profile = json.loads(
            (REPOSITORY / "config/rknn/platforms/rk3588.json").read_text(
                encoding="utf-8"
            )
        )
        builder_lock = json.loads(
            (REPOSITORY / "config/rockchip-build/builder-lock-rk3588.json").read_text(
                encoding="utf-8"
            )
        )
        shared_lock = json.loads(
            (REPOSITORY / "config/rockchip-build/builder-lock.json").read_text(
                encoding="utf-8"
            )
        )
        dockerfile = (REPOSITORY / "Dockerfile.rk3588-bullseye").read_text(
            encoding="utf-8"
        )
        compose = (REPOSITORY / "docker-compose.rk3588.yml").read_text(
            encoding="utf-8"
        )
        entrypoint = (REPOSITORY / "scripts/build_rockchip_package.sh").read_text(
            encoding="utf-8"
        )

        # Platform profile: RKNN inference with CPU/FFmpeg media, no RKLLM.
        self.assertEqual(profile["backend"], "rknn")
        self.assertEqual(profile["chip"], "rk3588")
        self.assertEqual(profile["conversion"]["target_platform"], "rk3588")
        self.assertEqual(profile["media"]["default_backend"], "cpu")
        self.assertEqual(profile["media"]["runtime_profile"], "cpu-ffmpeg-debian11-v1")
        self.assertTrue(profile["qualification"]["requires_target_bound_evidence"])

        # Dedicated lock: rk3588 only, glibc 2.31 policy, ffmpeg root set,
        # and the shared rk3576/rv1126b lock stays untouched.
        self.assertNotIn("rk3588", shared_lock["targets"])
        target = builder_lock["targets"]["rk3588"]
        self.assertEqual(list(builder_lock["targets"]), ["rk3588"])
        self.assertEqual(
            target["media_runtime_profile"], profile["media"]["runtime_profile"]
        )
        self.assertFalse(target["rkllm_required"])
        self.assertEqual(target["media_root"], "")
        self.assertEqual(builder_lock["common"]["glibc_max"], "2.31")
        self.assertEqual(builder_lock["common"]["ffmpeg_root"], "/opt/ffmpeg-debian11")
        self.assertIsNone(builder_lock["common"]["rkllm"])
        self.assertIn("lib/libavcodec.so.58", target["required_package_paths"])
        self.assertIn("lib/librkllmrt.so", target["forbidden_package_paths"])
        self.assertIn("lib/librockchip_mpp.so", target["forbidden_package_paths"])

        # Builder image freezes the bullseye base by digest, verifies the
        # RKNN runtime sha256, assembles the FFmpeg root, and bakes in the
        # dedicated builder lock behind the package-script env override.
        self.assertIn(
            "debian:bullseye@sha256:"
            "99cdf7792e25416bd801861ccd8e2fb27fb527b25e8d9a8704ebc3ead2015675",
            dockerfile,
        )
        self.assertIn(
            "ghcr.io/cosmo-wander-ai/cosmo_edge-build-env_rk3576@sha256:"
            "135d25d0baf14e7918726f7efb040a0627926aedd5825f52fab6c1cd208da348",
            dockerfile,
        )
        self.assertIn(
            "d31fc19c85b85f6091b2bd0f6af9d962d5264a4e410bfb536402ec92bac738e8",
            dockerfile,
        )
        self.assertIn("/opt/ffmpeg-debian11", dockerfile)
        self.assertIn("COPY config/rockchip-build/builder-lock-rk3588.json", dockerfile)
        self.assertIn(
            "COSMO_ROCKCHIP_BUILDER_LOCK=/opt/cosmo/"
            "rockchip-builder-lock-rk3588.json",
            dockerfile,
        )
        self.assertIn("builder-versions-rk3588.json", dockerfile)

        # Package entry point selects the chip-specific lock and enforces
        # the glibc gate for rk3588.
        self.assertIn("builder-lock-rk3588.json", entrypoint)
        self.assertIn("glibc_gate.py", entrypoint)
        self.assertIn('command:\n      - --chip', compose)
        self.assertIn("${COSMO_TARGET_CHIP:-rk3588}", compose)
        self.assertIn("Dockerfile.rk3588-bullseye", compose)

    def test_glibc_gate_scans_verneed_and_enforces_policy(self) -> None:
        def elf64_verneed(version_names: list[bytes]) -> bytes:
            """Build a minimal ELF64 little-endian image with a verneed section."""
            dynstr = b"\x00" + b"\x00".join(version_names) + b"\x00"
            name_offsets = []
            cursor = 1
            for name in version_names:
                name_offsets.append(cursor)
                cursor += len(name) + 1
            verneed = struct.pack("<HHIII", 1, len(name_offsets), 0, 16, 0)
            for index, offset in enumerate(name_offsets):
                is_last = index == len(name_offsets) - 1
                verneed += struct.pack(
                    "<IHHII", 0, 0, index + 1, offset, 0 if is_last else 16
                )
            blob = bytearray(b"\x7fELF" + bytes([2, 1, 1]) + bytes(9))
            blob += bytes(48)  # pad e_ident to 64-byte ELF header
            dynstr_offset = len(blob)
            blob += dynstr
            verneed_offset = len(blob)
            blob += verneed
            shstrtab = b"\x00.dynstr\x00.gnu.version_r\x00"
            shstrtab_offset = len(blob)
            blob += shstrtab
            sections = [
                (0, 0, 0, 0),  # SHT_NULL
                (3, dynstr_offset, len(dynstr), 0),  # SHT_STRTAB .dynstr
                (0x6FFFFFFE, verneed_offset, len(verneed), 1),  # verneed -> dynstr
                (3, shstrtab_offset, len(shstrtab), 0),  # section names
            ]
            (shoff_holder,) = [len(blob)]
            blob += bytes(len(sections) * 64)
            e_shoff = shoff_holder
            struct.pack_into("<Q", blob, 0x28, e_shoff)
            struct.pack_into("<HH", blob, 0x3A, 64, len(sections))
            for index, (sh_type, sh_offset, sh_size, sh_link) in enumerate(sections):
                base = e_shoff + index * 64
                struct.pack_into("<II", blob, base, 0, sh_type)
                struct.pack_into("<QQ", blob, base + 16, 0, sh_offset)
                struct.pack_into("<QQ", blob, base + 32, sh_size, 0)
                struct.pack_into("<II", blob, base + 40, sh_link, 0)
            return bytes(blob)

        good = elf64_verneed([b"GLIBC_2.17"])
        bad = elf64_verneed([b"GLIBC_2.17", b"GLIBC_2.34"])
        self.assertEqual(glibc_gate.glibc_requirements(good), {"GLIBC_2.17"})
        self.assertEqual(
            glibc_gate.glibc_requirements(bad), {"GLIBC_2.17", "GLIBC_2.34"}
        )
        self.assertEqual(glibc_gate.scan_bytes("good.so", good, "2.31"), [])
        violations = glibc_gate.scan_bytes("bad.so", bad, "2.31")
        self.assertEqual(len(violations), 1)
        self.assertIn("GLIBC_2.34 > 2.31", violations[0])
        with self.assertRaises(glibc_gate.GlibcGateError):
            glibc_gate.scan_bytes("text.txt", b"definitely not an elf", "2.31")

        report = glibc_gate.run_scan(
            [("lib/good.so", good), ("etc/config.json", b"{}")], "2.31"
        )
        self.assertEqual(report["status"], "PASS")
        self.assertEqual(report["elf_files"], 1)
        failing = glibc_gate.run_scan([("lib/bad.so", bad)], "2.31")
        self.assertEqual(failing["status"], "FAIL")

    def test_shared_rknn_and_rockchip_sources_do_not_fork_by_chip(self) -> None:
        source_roots = (
            REPOSITORY / "src/nn/device/rknn",
            REPOSITORY / "src/media",
        )
        chip_pattern = re.compile(r"\b(?:rk3576|rv1126b)\b", re.IGNORECASE)
        source_files = sorted(
            path
            for root in source_roots
            for path in root.rglob("*")
            if path.suffix in {".cc", ".cpp", ".h", ".hpp"}
        )

        self.assertTrue(source_files)
        for path in source_files:
            relative = path.relative_to(REPOSITORY)
            self.assertIsNone(
                chip_pattern.search(path.name),
                f"chip-specific backend source file is not allowed: {relative}",
            )
            source = path.read_text(encoding="utf-8")
            code_without_comments = re.sub(
                r"//.*?$|/\*.*?\*/",
                "",
                source,
                flags=re.MULTILINE | re.DOTALL,
            )
            self.assertIsNone(
                chip_pattern.search(code_without_comments),
                f"chip-specific backend branching must move to platform data: {relative}",
            )


if __name__ == "__main__":
    unittest.main()
