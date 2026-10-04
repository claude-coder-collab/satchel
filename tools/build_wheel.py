#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
# Copyright (c) 2026 Venn Audio Ltd.
"""Builds a platform wheel of the Python binding with libsatchel bundled inside the package."""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PACKAGE = ROOT / "bindings" / "python"
LIBRARY_NAMES = {"win32": "satchel.dll", "darwin": "libsatchel.dylib"}


def library_name(platform: str) -> str:
    return LIBRARY_NAMES.get(platform, "libsatchel.so")


def set_version(pyproject: str, version: str) -> str:
    """Returns `pyproject` with the [project] version replaced."""
    if not re.fullmatch(r"\d+(\.\d+)*([ab]|rc)?\d*(\.dev\d+)?", version):
        raise ValueError(f"not a PEP 440 version: {version}")
    updated, count = re.subn(r'(?m)^version = "[^"]*"$', f'version = "{version}"', pyproject, count=1)
    if count != 1:
        raise ValueError("pyproject.toml has no version line")
    return updated


def build(library: Path, platform_tag: str, out_dir: Path, version: str | None) -> Path:
    """Copies the package and `library` to a temporary tree, builds the wheel and retags it."""
    out_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        tree = Path(tmp) / "python"
        shutil.copytree(PACKAGE, tree, ignore=shutil.ignore_patterns("__pycache__", "*.so", "*.dylib", "*.dll", "build", "dist", "*.egg-info"))
        shutil.copyfile(library.resolve(), tree / "satchel" / library_name(sys.platform))
        shutil.copyfile(ROOT / "LICENSE", tree / "LICENSE")
        if version:
            pyproject = tree / "pyproject.toml"
            pyproject.write_text(set_version(pyproject.read_text(encoding="utf-8"), version), encoding="utf-8")
        dist = Path(tmp) / "dist"
        subprocess.run([sys.executable, "-m", "pip", "wheel", "--no-deps", "--no-build-isolation", "-w", str(dist), str(tree)], check=True)
        (pure,) = dist.glob("*.whl")
        subprocess.run([sys.executable, "-m", "wheel", "tags", "--remove", "--python-tag", "py3", "--abi-tag", "none", "--platform-tag", platform_tag, str(pure)], check=True)
        (wheel,) = dist.glob("*.whl")
        target = out_dir / wheel.name
        shutil.move(wheel, target)
        return target


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--library", type=Path, required=True, help="built libsatchel (.so, .dylib or .dll)")
    parser.add_argument("--platform-tag", required=True, help="e.g. manylinux_2_28_x86_64, macosx_14_0_arm64, win_amd64")
    parser.add_argument("--version", help="package version (default: pyproject.toml)")
    parser.add_argument("--out", type=Path, default=Path("dist"))
    args = parser.parse_args()
    print(build(args.library, args.platform_tag, args.out, args.version))


if __name__ == "__main__":
    main()
