# SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
# Copyright (c) 2026 Venn Audio Ltd.
from __future__ import annotations

import json
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


def test_icon_composer_bundle_layers_the_foreground_over_a_solid_fill() -> None:
    files = make_icons.icon_composer(size=32, samples=1)
    manifest = json.loads(files["icon.json"])
    assert manifest["fill"] == {"solid": make_icons.srgb(make_icons.BACKGROUND)}
    assert manifest["supported-platforms"] == {"squares": ["macOS"]}
    (group,) = manifest["groups"]
    names = [layer["name"] for layer in group["layers"]]
    assert names == ["clasp", "flap", "bag"]
    assert [layer["image-name"] for layer in group["layers"]] == [f"{n}.png" for n in names]
    assert set(files) == {"icon.json"} | {f"Assets/{n}.png" for n in names}
    for name in names:
        assert struct.unpack(">II", files[f"Assets/{name}.png"][16:24]) == (32, 32)


def test_icon_composer_layers_fill_the_canvas_with_the_background_square() -> None:
    extent = 2 * make_icons.BACKGROUND_HALF_WIDTH
    bag = make_icons.rgba(100, 1, make_icons.FOREGROUND["bag"], extent)
    alpha = bag[3::4]
    assert alpha[0] == 0 and alpha[50 * 100 + 50] == 255
    body_left = round((0.5 - 0.3 - (0.5 - extent / 2)) / extent * 100)
    row = 70 * 100
    assert alpha[row + body_left - 2] == 0 and alpha[row + body_left + 2] == 255


def test_actool_command_names_the_icon_and_deployment_target(tmp_path: Path) -> None:
    command = make_icons.actool_command(tmp_path / "Satchel.icon", tmp_path)
    assert command[:3] == ["xcrun", "actool", str(tmp_path / "Satchel.icon")]
    assert command[command.index("--app-icon") + 1] == "Satchel"
    assert command[command.index("--minimum-deployment-target") + 1] == make_icons.MACOS_DEPLOYMENT_TARGET
    release = (Path(__file__).resolve().parents[2] / ".github" / "workflows" / "release.yml").read_text()
    assert f"-DCMAKE_OSX_DEPLOYMENT_TARGET={make_icons.MACOS_DEPLOYMENT_TARGET}" in release


def test_compile_catalog_skips_without_xcode(tmp_path: Path, monkeypatch) -> None:
    monkeypatch.setattr(make_icons.shutil, "which", lambda _: None)
    assert not make_icons.compile_catalog(tmp_path / "Satchel.icon", tmp_path / "Assets.car")
    assert not (tmp_path / "Assets.car").exists()


def test_bundle_icon_name_matches_the_catalog() -> None:
    root = Path(__file__).resolve().parents[2]
    plist = (root / "packaging" / "macos" / "Info.plist.in").read_text()
    assert "<key>CFBundleIconName</key>\n    <string>Satchel</string>" in plist
    assert (root / "packaging" / "icons" / "Satchel.icon" / "icon.json").is_file()
    assert (root / "packaging" / "icons" / "Assets.car").is_file()
