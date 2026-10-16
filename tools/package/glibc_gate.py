#!/usr/bin/env python3
"""Package-level glibc compatibility gate.

Scans every ELF file inside an install package (a directory or a .tar.gz
archive) for required symbol versions (SHT_GNU_verneed) and fails when any
GLIBC_x.y requirement exceeds the policy maximum (e.g. 2.31 for Debian 11
bullseye targets).

Usage:
    python3 glibc_gate.py --archive pkg.tar.gz --max 2.31
    python3 glibc_gate.py --path ./install/ --max 2.31 [--json out.json]

Exit codes: 0 = pass, 1 = policy violation or unreadable ELF, 2 = usage error.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import struct
import sys
import tarfile

SHT_GNU_VERNEED = 0x6FFFFFFE

ELF_MAGIC = b"\x7fELF"


class GlibcGateError(Exception):
    """Raised when an ELF file cannot be parsed."""


def _parse_verneed(data: bytes) -> list[str]:
    """Return every version string required by an ELF's GNU verneed section."""
    if len(data) < 64 or data[:4] != ELF_MAGIC:
        raise GlibcGateError("not an ELF file")
    ei_class, ei_data = data[4], data[5]
    if ei_class not in (1, 2):
        raise GlibcGateError("unsupported EI_CLASS")
    if ei_data == 1:
        endian = "<"
    elif ei_data == 2:
        endian = ">"
    else:
        raise GlibcGateError("unsupported EI_DATA")

    is64 = ei_class == 2
    # Section header offset/size and count.
    if is64:
        (e_shoff,) = struct.unpack_from(endian + "Q", data, 0x28)
        (e_shentsize, e_shnum) = struct.unpack_from(endian + "HH", data, 0x3A)
        # ELF64 Shdr: sh_offset at +24, sh_size at +32.
        value_fmt = endian + "QQ"
        value_base = 24
    else:
        (e_shoff,) = struct.unpack_from(endian + "I", data, 0x20)
        (e_shentsize, e_shnum) = struct.unpack_from(endian + "HH", data, 0x2E)
        # ELF32 Shdr: sh_offset at +16, sh_size at +20.
        value_fmt = endian + "II"
        value_base = 16

    if e_shoff == 0 or e_shnum == 0:
        return []  # no section headers (e.g. stripped static object)

    sections: list[tuple[int, int, int, int]] = []
    for index in range(e_shnum):
        base = e_shoff + index * e_shentsize
        if base + e_shentsize > len(data):
            raise GlibcGateError("section header table truncated")
        sh_name, sh_type = struct.unpack_from(endian + "II", data, base)
        sh_offset, sh_size = struct.unpack_from(value_fmt, data, base + value_base)
        sh_link, _sh_info = struct.unpack_from(endian + "II", data, base + 40)
        sections.append((sh_type, sh_offset, sh_size, sh_link))

    versions: list[str] = []
    for sh_type, sh_offset, sh_size, sh_link in sections:
        if sh_type != SHT_GNU_VERNEED:
            continue
        if sh_link >= len(sections):
            raise GlibcGateError("verneed sh_link out of range")
        str_type, str_off, str_size, _ = sections[sh_link]
        if str_type != 3:  # SHT_STRTAB
            raise GlibcGateError("verneed string table is not SHT_STRTAB")
        dynstr = data[str_off : str_off + str_size]

        offset = sh_offset
        remaining = sh_size
        while remaining >= 16:
            vn_version, vn_cnt, vn_file, vn_aux, vn_next = struct.unpack_from(
                endian + "HHIII", data, offset
            )
            if vn_version != 1:
                raise GlibcGateError("unsupported verneed version")
            aux_offset = offset + vn_aux
            for _ in range(vn_cnt):
                (
                    _vna_hash,
                    _vna_flags,
                    _vna_other,
                    vna_name,
                    vna_next,
                ) = struct.unpack_from(endian + "IHHII", data, aux_offset)
                name_end = dynstr.find(b"\x00", vna_name)
                if name_end == -1:
                    raise GlibcGateError("unterminated version name")
                versions.append(dynstr[vna_name:name_end].decode("ascii", "replace"))
                if vna_next == 0:
                    break
                aux_offset += vna_next
            if vn_next == 0:
                break
            offset += vn_next
            remaining -= vn_next
    return versions


def _version_tuple(version: str) -> tuple[int, ...]:
    return tuple(int(part) for part in version.split("."))


def glibc_requirements(data: bytes) -> set[str]:
    """GLIBC_x.y strings required by one ELF image (empty for non-glibc needs)."""
    return {v for v in _parse_verneed(data) if v.startswith("GLIBC_")}


def scan_bytes(name: str, data: bytes, max_version: str) -> list[str]:
    """Return violation strings for one ELF image."""
    try:
        requirements = glibc_requirements(data)
    except GlibcGateError as error:
        raise GlibcGateError(f"{name}: {error}") from error
    limit = _version_tuple(max_version)
    violations = []
    for requirement in sorted(requirements):
        if _version_tuple(requirement[len("GLIBC_") :]) > limit:
            violations.append(f"{name}: requires {requirement} > {max_version}")
    return violations


def _iter_archive_members(archive: pathlib.Path):
    with tarfile.open(archive, mode="r:*") as bundle:
        for member in bundle:
            if not member.isfile():
                continue
            handle = bundle.extractfile(member)
            if handle is None:
                continue
            yield member.name, handle.read()


def _iter_directory_files(root: pathlib.Path):
    for path in sorted(root.rglob("*")):
        if path.is_file() and not path.is_symlink():
            yield str(path.relative_to(root)), path.read_bytes()


def run_scan(entries, max_version: str) -> dict[str, object]:
    scanned = 0
    elf_files = 0
    violations: list[str] = []
    for name, data in entries:
        scanned += 1
        if not data.startswith(ELF_MAGIC):
            continue
        elf_files += 1
        violations.extend(scan_bytes(name, data, max_version))
    return {
        "max_glibc": max_version,
        "files_scanned": scanned,
        "elf_files": elf_files,
        "violations": violations,
        "status": "FAIL" if violations else "PASS",
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--archive", type=pathlib.Path)
    source.add_argument("--path", type=pathlib.Path)
    parser.add_argument("--max", default="2.31", help="maximum allowed GLIBC version")
    parser.add_argument("--json", type=pathlib.Path, help="write a JSON report")
    arguments = parser.parse_args(argv)

    try:
        _version_tuple(arguments.max)
    except ValueError:
        parser.error(f"invalid --max value: {arguments.max}")

    if arguments.archive is not None:
        if not arguments.archive.is_file():
            parser.error(f"archive not found: {arguments.archive}")
        entries = _iter_archive_members(arguments.archive)
    else:
        if not arguments.path.is_dir():
            parser.error(f"directory not found: {arguments.path}")
        entries = _iter_directory_files(arguments.path)

    try:
        report = run_scan(entries, arguments.max)
    except (GlibcGateError, tarfile.TarError, OSError) as error:
        print(f"glibc-gate: ERROR: {error}", file=sys.stderr)
        return 1

    if arguments.json is not None:
        arguments.json.write_text(
            json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )

    print(
        f"glibc-gate: scanned {report['files_scanned']} files "
        f"({report['elf_files']} ELF), policy GLIBC<={arguments.max}: "
        f"{report['status']}"
    )
    violations = report["violations"]
    assert isinstance(violations, list)
    for violation in violations:
        print(f"glibc-gate: VIOLATION: {violation}", file=sys.stderr)
    return 0 if report["status"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
