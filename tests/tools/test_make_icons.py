# SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
# Copyright (c) 2026 Venn Audio Ltd.
from __future__ import annotations

import struct
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))

import make_icons  # noqa: E402


def test_png_is_valid_rgba() -> None:
    data = make_icons.png(16)
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    width, height, depth, color = struct.unpack(">IIBB", data[16:26])
    assert (width, height, depth, color) == (16, 16, 8, 6)
    idat_len = struct.unpack(">I", data[33:37])[0]
    raw = zlib.decompress(data[41 : 41 + idat_len])
    assert len(raw) == 16 * (1 + 16 * 4)


def test_corners_are_transparent_and_centre_is_opaque() -> None:
    raw = make_icons.render(32, samples=1)
    stride = 1 + 32 * 4
    assert raw[1 + 3] == 0
    centre = 16 * stride + 1 + 16 * 4
    assert raw[centre + 3] == 255


def test_ico_and_icns_directories() -> None:
    images = {16: make_icons.png(16), 32: make_icons.png(32)}
    ico = make_icons.ico(images)
    assert struct.unpack("<HHH", ico[:6]) == (0, 1, 2)
    size, offset = struct.unpack("<II", ico[6 + 8 : 6 + 16])
    assert ico[offset : offset + size] == images[16]
    icns = make_icons.icns(images)
    assert icns[:4] == b"icns" and struct.unpack(">I", icns[4:8])[0] == len(icns)
    assert icns[8:12] == b"icp4"
