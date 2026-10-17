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



def unrle(data: bytes, count: int) -> tuple[list[bytes], int]:
    planes = []
    i = 0
    for _ in range(4):
        plane = bytearray()
        while len(plane) < count:
            b = data[i]
            if b < 0x80:
                plane += data[i + 1 : i + 2 + b]
                i += b + 2
            else:
                plane += bytes([data[i + 1]]) * (b - 0x7D)
                i += 2
        assert len(plane) == count
        planes.append(bytes(plane))
    return planes, i


def test_icns_rle_round_trips() -> None:
    for channel in (b"", b"a", b"ab", b"aaa", b"x" * 300, bytes(range(256)) * 2, b"aab" * 50 + b"c" * 131 + b"de"):
        planes, used = unrle(make_icons.icns_rle(channel) * 4, len(channel))
        assert planes[0] == channel and used == 4 * len(make_icons.icns_rle(channel))


def test_icns_uses_iconutil_types_and_argb_for_small_sizes() -> None:
    sizes = (16, 32, 64, 128, 256, 512, 1024)
    pngs = {s: make_icons.png(s) if s <= 64 else b"\x89PNG" + bytes([s % 251]) for s in sizes}
    pixels = {s: make_icons.rgba(s, samples=1) for s in (16, 32)}
    icns = make_icons.icns(pngs, pixels)
    assert icns[:4] == b"icns" and struct.unpack(">I", icns[4:8])[0] == len(icns)
    chunks = {}
    o = 8
    while o < len(icns):
        tag, n = struct.unpack(">4sI", icns[o : o + 8])
        chunks[tag] = icns[o + 8 : o + n]
        o += n
    assert set(chunks) == {b"ic04", b"ic05", b"ic11", b"ic12", b"ic07", b"ic13", b"ic08", b"ic14", b"ic09", b"ic10"}
    assert chunks[b"ic11"] == pngs[32] and chunks[b"ic12"] == pngs[64] and chunks[b"ic10"] == pngs[1024]
    for size, tag in ((16, b"ic04"), (32, b"ic05")):
        assert chunks[tag][:4] == b"ARGB"
        (a, r, g, b), _ = unrle(chunks[tag][4:], size * size)
        src = pixels[size]
        assert (a, r, g, b) == (src[3::4], src[0::4], src[1::4], src[2::4])
