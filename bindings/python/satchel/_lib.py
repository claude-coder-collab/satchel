# SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
# Copyright (c) 2026 Venn Audio Ltd.
"""Loads libsatchel through cffi (ABI mode)."""

from __future__ import annotations

import os
import sys
from pathlib import Path

import cffi

ffi = cffi.FFI()
ffi.cdef((Path(__file__).with_name("_cdef.h")).read_text(encoding="utf-8"))


def _library_names() -> list[str]:
    if sys.platform == "win32":
        return ["satchel.dll"]
    if sys.platform == "darwin":
        return ["libsatchel.dylib"]
    return ["libsatchel.so"]


def _candidates() -> list[Path]:
    paths: list[Path] = []
    env = os.environ.get("SATCHEL_LIBRARY")
    if env:
        paths.append(Path(env))
    here = Path(__file__).resolve().parent
    for name in _library_names():
        paths.append(here / name)
    repo = here.parent.parent.parent
    for preset in ("clang", "gcc", "msvc"):
        for config in ("Release", "Debug"):
            for name in _library_names():
                paths.append(repo / "build" / preset / "core" / config / name)
    return paths


def _load():
    tried = []
    for path in _candidates():
        if path.exists():
            return ffi.dlopen(str(path))
        tried.append(str(path))
    raise OSError("libsatchel not found; set SATCHEL_LIBRARY. Tried: " + ", ".join(tried))


lib = _load()
