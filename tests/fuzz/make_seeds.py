#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
# Copyright (c) 2026 Venn Audio Ltd.
"""Write small seed inputs for the fuzzers: PCM containers and zip archives."""

from __future__ import annotations

import io
import struct
import sys
import zipfile
from pathlib import Path


def chunk(cid: bytes, data: bytes, fmt: str = "<I") -> bytes:
    b = cid + struct.pack(fmt, len(data)) + data
    return b + (b"\0" if len(data) % 2 else b"")


def wav(frames: int = 500, channels: int = 2) -> bytes:
    fmt = struct.pack("<HHIIHH", 1, channels, 48000, 48000 * channels * 2, channels * 2, 16)
    data = bytes((i * 7) & 0xFF for i in range(frames * channels * 2))
    body = b"WAVE" + chunk(b"fmt ", fmt) + chunk(b"bext", b"d" * 400) + chunk(b"data", data) + chunk(b"LIST", b"INFOINAM\x03\0\0\0abc\0")
    return b"RIFF" + struct.pack("<I", len(body)) + body


def aiff(frames: int = 500) -> bytes:
    rate = b"\x40\x0e\xbb\x80\x00\x00\x00\x00\x00\x00"
    comm = struct.pack(">hIh", 2, frames, 16) + rate
    ssnd = struct.pack(">II", 0, 0) + bytes((i * 5) & 0xFF for i in range(frames * 4))
    body = b"AIFF" + chunk(b"COMM", comm, ">I") + chunk(b"SSND", ssnd, ">I")
    return b"FORM" + struct.pack(">I", len(body)) + body


def main(out: Path) -> None:
    (out / "pcm").mkdir(parents=True, exist_ok=True)
    (out / "archive").mkdir(parents=True, exist_ok=True)
    (out / "pcm" / "a.wav").write_bytes(wav())
    (out / "pcm" / "multi.wav").write_bytes(wav(channels=10))
    (out / "pcm" / "a.aiff").write_bytes(aiff())
    for name, method in (("deflate.zip", zipfile.ZIP_DEFLATED), ("store.zip", zipfile.ZIP_STORED)):
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, "w", method) as z:
            z.writestr("dir/", "")
            z.writestr("dir/a.txt", "alpha " * 100)
            z.writestr("../evil.txt", "x")
            z.writestr("take.wav", wav())
        (out / "archive" / name).write_bytes(buf.getvalue())


if __name__ == "__main__":
    main(Path(sys.argv[1]))
