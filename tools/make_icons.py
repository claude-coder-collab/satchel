#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
# Copyright (c) 2026 Venn Audio Ltd.
"""Renders the placeholder app icon to PNG, ICO and ICNS without third-party packages."""

from __future__ import annotations

import argparse
import math
import struct
import zlib
from pathlib import Path
from typing import Callable

Color = tuple[float, float, float, float]
Shape = Callable[[float, float], float]

BACKGROUND: Color = (0x24 / 255, 0x57 / 255, 0xD6 / 255, 1.0)
BAG: Color = (0xF6 / 255, 0xF1 / 255, 0xE7 / 255, 1.0)
FLAP: Color = (0xE2 / 255, 0xD6 / 255, 0xBF / 255, 1.0)
CLASP: Color = (0x1D / 255, 0x23 / 255, 0x30 / 255, 1.0)


def rounded_rect(cx: float, cy: float, hw: float, hh: float, r: float) -> Shape:
    def sdf(x: float, y: float) -> float:
        qx = abs(x - cx) - hw + r
        qy = abs(y - cy) - hh + r
        outside = math.hypot(max(qx, 0.0), max(qy, 0.0))
        return outside + min(max(qx, qy), 0.0) - r

    return sdf


def ring(cx: float, cy: float, radius: float, half_width: float, below: float) -> Shape:
    def sdf(x: float, y: float) -> float:
        d = abs(math.hypot(x - cx, y - cy) - radius) - half_width
        return d if y <= below else max(d, y - below)

    return sdf


def flap(top: float, bottom: float, hw_top: float, hw_bottom: float, cx: float) -> Shape:
    def sdf(x: float, y: float) -> float:
        if y < top or y > bottom:
            return max(top - y, y - bottom)
        t = (y - top) / (bottom - top)
        hw = hw_top + (hw_bottom - hw_top) * t
        return abs(x - cx) - hw

    return sdf


LAYERS: list[tuple[Shape, Color]] = [
    (rounded_rect(0.5, 0.5, 0.46, 0.46, 0.2), BACKGROUND),
    (ring(0.5, 0.36, 0.13, 0.03, 0.36), BAG),
    (rounded_rect(0.5, 0.6, 0.3, 0.22, 0.06), BAG),
    (flap(0.38, 0.56, 0.3, 0.2, 0.5), FLAP),
    (rounded_rect(0.5, 0.56, 0.05, 0.035, 0.015), CLASP),
]


def render(size: int, samples: int = 4) -> bytes:
    """Returns straight-alpha RGBA rows for a `size`-pixel square icon."""
    out = bytearray()
    step = 1.0 / (size * samples)
    for py in range(size):
        out.append(0)
        for px in range(size):
            acc = [0.0, 0.0, 0.0, 0.0]
            for sy in range(samples):
                for sx in range(samples):
                    x = (px * samples + sx + 0.5) * step
                    y = (py * samples + sy + 0.5) * step
                    r = g = b = a = 0.0
                    for shape, (cr, cg, cb, ca) in LAYERS:
                        if shape(x, y) <= 0.0:
                            r, g, b, a = cr, cg, cb, ca
                    acc[0] += r * a
                    acc[1] += g * a
                    acc[2] += b * a
                    acc[3] += a
            n = samples * samples
            alpha = acc[3] / n
            if alpha > 0:
                rgb = [round(255 * c / acc[3]) for c in acc[:3]]
            else:
                rgb = [0, 0, 0]
            out.extend((*rgb, round(255 * alpha)))
    return bytes(out)


def png(size: int) -> bytes:
    def chunk(tag: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))

    header = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(render(size), 9)) + chunk(b"IEND", b"")


def ico(images: dict[int, bytes]) -> bytes:
    sizes = sorted(images)
    head = struct.pack("<HHH", 0, 1, len(sizes))
    offset = 6 + 16 * len(sizes)
    entries = b""
    body = b""
    for s in sizes:
        data = images[s]
        entries += struct.pack("<BBBBHHII", s % 256, s % 256, 0, 0, 1, 32, len(data), offset + len(body))
        body += data
    return head + entries + body


ICNS_TYPES = {16: b"icp4", 32: b"icp5", 64: b"icp6", 128: b"ic07", 256: b"ic08", 512: b"ic09", 1024: b"ic10"}


def icns(images: dict[int, bytes]) -> bytes:
    body = b"".join(ICNS_TYPES[s] + struct.pack(">I", 8 + len(images[s])) + images[s] for s in sorted(images) if s in ICNS_TYPES)
    return b"icns" + struct.pack(">I", 8 + len(body)) + body


def write_all(out_dir: Path) -> list[Path]:
    """Writes satchel.png (256), satchel-512.png, satchel.ico and satchel.icns into `out_dir`."""
    out_dir.mkdir(parents=True, exist_ok=True)
    images = {s: png(s) for s in (16, 32, 48, 64, 128, 256, 512, 1024)}
    files = {
        "satchel.png": images[256],
        "satchel-512.png": images[512],
        "satchel.ico": ico({s: images[s] for s in (16, 32, 48, 64, 128, 256)}),
        "satchel.icns": icns(images),
    }
    paths = []
    for name, data in files.items():
        path = out_dir / name
        path.write_bytes(data)
        paths.append(path)
    return paths


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("out_dir", type=Path, nargs="?", default=Path(__file__).resolve().parent.parent / "packaging" / "icons")
    args = parser.parse_args()
    for path in write_all(args.out_dir):
        print(path)


if __name__ == "__main__":
    main()
