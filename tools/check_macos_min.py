#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
# Copyright (c) 2026 Venn Audio Ltd.
"""Fails if any Mach-O binary in a bundle needs a newer macOS than the deployment target."""

from __future__ import annotations

import argparse
import subprocess
import sys
from collections.abc import Iterator
from pathlib import Path

MACHO_MAGICS = {
    bytes.fromhex(m) for m in ("feedface", "cefaedfe", "feedfacf", "cffaedfe", "cafebabe", "bebafeca")
}


def version_tuple(version: str) -> tuple[int, ...]:
    return tuple(int(part) for part in version.split("."))


def is_macho(path: Path) -> bool:
    if path.is_symlink() or not path.is_file():
        return False
    with path.open("rb") as f:
        return f.read(4) in MACHO_MAGICS


def macho_files(root: Path) -> Iterator[Path]:
    if is_macho(root):
        yield root
        return
    for path in sorted(root.rglob("*")):
        if is_macho(path):
            yield path


def min_versions(otool_output: str) -> list[str]:
    """Minimum macOS versions in `otool -l` output, one per architecture slice."""
    versions = []
    command = ""
    for line in otool_output.splitlines():
        fields = line.split()
        if len(fields) < 2:
            continue
        key, value = fields[0], fields[1]
        if key == "cmd":
            command = value
        elif (key, command) in {("minos", "LC_BUILD_VERSION"), ("version", "LC_VERSION_MIN_MACOSX")}:
            versions.append(value)
    return versions


def too_new(binaries: dict[Path, list[str]], target: str) -> dict[Path, str]:
    limit = version_tuple(target)
    result = {}
    for path, versions in binaries.items():
        newest = max(versions, key=version_tuple, default=None)
        if newest is not None and version_tuple(newest) > limit:
            result[path] = newest
    return result


def scan(root: Path) -> dict[Path, list[str]]:
    binaries = {}
    for path in macho_files(root):
        output = subprocess.run(
            ["otool", "-l", "-arch", "all", str(path)], check=True, capture_output=True, text=True
        ).stdout
        binaries[path] = min_versions(output)
    return binaries


def check(root: Path, target: str) -> bool:
    binaries = scan(root)
    if not binaries:
        print(f"No Mach-O binaries in {root}", file=sys.stderr)
        return False
    offenders = too_new(binaries, target)
    for path, version in offenders.items():
        print(f"{path.relative_to(root.parent)} needs macOS {version}", file=sys.stderr)
    if not offenders:
        print(f"{len(binaries)} binaries in {root.name} run on macOS {target}")
    return not offenders


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bundle", type=Path)
    parser.add_argument("--target", required=True, help="deployment target, e.g. 13.3")
    args = parser.parse_args()
    if not check(args.bundle, args.target):
        sys.exit(1)


if __name__ == "__main__":
    main()
