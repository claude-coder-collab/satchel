#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
# Copyright (c) 2026 Venn Audio Ltd.
"""Generate the preprocessor-free C declarations used by the Python (cffi) and PHP (FFI) bindings."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HEADER = ROOT / "core" / "include" / "zp" / "zp.h"
TARGETS = [
    ROOT / "bindings" / "python" / "satchel" / "_cdef.h",
    ROOT / "bindings" / "php" / "src" / "zp_cdef.h",
]
BANNER = (
    "/* SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial\n"
    " * Generated from core/include/zp/zp.h by tools/gen_cdef.py. Do not edit. */\n"
)


def cdef(header: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", header, flags=re.S)
    lines: list[str] = []
    for line in text.splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("#") or stripped == 'extern "C" {':
            continue
        lines.append(line.rstrip())
    body = "\n".join(lines)
    body = body.replace("ZP_API ", "")
    # Drop the closing brace of extern "C" (the last line).
    if body.endswith("\n}"):
        body = body[: -len("\n}")]
    return BANNER + body.strip() + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="fail if the generated files are out of date")
    args = parser.parse_args()
    expected = cdef(HEADER.read_text(encoding="utf-8"))
    stale = [t for t in TARGETS if not t.exists() or t.read_text(encoding="utf-8") != expected]
    if args.check:
        for t in stale:
            print(f"{t.relative_to(ROOT)} is out of date; run tools/gen_cdef.py", file=sys.stderr)
        return 1 if stale else 0
    for t in TARGETS:
        t.write_text(expected, encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
