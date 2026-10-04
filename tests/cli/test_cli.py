# SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
# Copyright (c) 2026 Venn Audio Ltd.
"""End-to-end tests of the satchel command-line tool. Set SATCHEL_CLI to the built binary."""

from __future__ import annotations

import json
import math
import os
import struct
import subprocess
import sys
import zipfile
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]


def find_cli() -> Path:
    env = os.environ.get("SATCHEL_CLI")
    if env:
        return Path(env)
    exe = "satchel.exe" if sys.platform == "win32" else "satchel"
    for preset in ("clang", "gcc", "msvc"):
        for config in ("Release", "Debug"):
            p = ROOT / "build" / preset / "desktop" / "cli" / config / exe
            if p.exists():
                return p
    pytest.skip("satchel CLI not built; set SATCHEL_CLI")


CLI = find_cli()


def run(*args: str, cwd: Path | None = None, input: bytes | None = None) -> subprocess.CompletedProcess[bytes]:
    return subprocess.run([str(CLI), *args], cwd=cwd, input=input if input is not None else b"", capture_output=True, timeout=120)


def make_wav(frames: int = 20000, channels: int = 2) -> bytes:
    samples = bytearray()
    for f in range(frames):
        for c in range(channels):
            samples += struct.pack("<h", int(8000 * math.sin(f * (0.01 + 0.002 * c))))
    fmt = struct.pack("<HHIIHH", 1, channels, 48000, 48000 * channels * 2, channels * 2, 16)
    body = b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", len(samples)) + bytes(samples)
    return b"RIFF" + struct.pack("<I", len(body)) + body


@pytest.fixture()
def session(tmp_path: Path) -> Path:
    src = tmp_path / "session"
    src.mkdir()
    (src / "take1.wav").write_bytes(make_wav())
    (src / "multi.wav").write_bytes(make_wav(frames=3000, channels=10))
    (src / "notes.txt").write_text("notes " * 500)
    return src


def test_create_list_verify_extract(tmp_path: Path, session: Path) -> None:
    archive = tmp_path / "out.zip"
    r = run("create", str(archive), str(session))
    assert r.returncode == 0, r.stderr
    with zipfile.ZipFile(archive) as z:
        assert z.testzip() is None
        names = z.namelist()
    assert "session/take1.flac" in names
    assert "session/multi_ch10.flac" in names

    listing = json.loads(run("list", "--json", str(archive)).stdout)
    assert listing["created_by"].startswith("Satchel")
    take = next(e for e in listing["entries"] if e["name"] == "session/take1.flac")
    assert take["method"] == "flac" and take["restores_to"] == "take1.wav"
    member = next(e for e in listing["entries"] if e["name"] == "session/multi_ch03.flac")
    assert member["channel"] == {"index": 3, "count": 10}

    verify = run("verify", "--json", str(archive))
    assert verify.returncode == 0
    assert json.loads(verify.stdout)["errors"] == 0

    dest = tmp_path / "dest"
    r = run("extract", str(archive), "-d", str(dest))
    assert r.returncode == 0, r.stderr
    for name in ("take1.wav", "multi.wav", "notes.txt"):
        assert (dest / "session" / name).read_bytes() == (session / name).read_bytes()

    again = run("extract", str(archive), "-d", str(dest))
    assert again.returncode == 4
    assert run("extract", str(archive), "-d", str(dest), "--overwrite", "skip").returncode == 0
    keep = run("extract", str(archive), "-d", str(tmp_path / "flac"), "--keep-flac", "--include-readme")
    assert keep.returncode == 0
    assert (tmp_path / "flac" / "session" / "take1.flac").exists()
    assert (tmp_path / "flac" / "README - How to restore original audio.txt").exists()


def test_conflicts_exit_code_and_resolutions(tmp_path: Path) -> None:
    src = tmp_path / "src"
    src.mkdir()
    (src / "take1.wav").write_bytes(make_wav(frames=1000))
    (src / "take1.flac").write_bytes(b"something else")
    archive = tmp_path / "a.zip"
    r = run("create", "--json", str(archive), str(src))
    assert r.returncode == 3
    report = json.loads(r.stdout)
    assert report["status"] == "CONFLICTS_UNRESOLVED"
    assert sorted(report["conflicts"][0]["names"]) == ["src/take1.flac", "src/take1.flac"]
    assert not archive.exists()

    r = run("create", str(archive), str(src), "--rename", "src/take1.wav=src/take1-pcm.flac")
    assert r.returncode == 0, r.stderr
    with zipfile.ZipFile(archive) as z:
        assert "src/take1-pcm.flac" in z.namelist()

    resolutions = tmp_path / "res.json"
    resolutions.write_text(json.dumps([{"entry": str(src / "take1.wav"), "action": "store-unconverted"}]))
    archive2 = tmp_path / "b.zip"
    assert run("create", str(archive2), str(src), "--resolutions", str(resolutions)).returncode == 0
    with zipfile.ZipFile(archive2) as z:
        assert sorted(z.namelist()) == ["src/", "src/take1.flac", "src/take1.wav"]

    bad = run("create", str(tmp_path / "c.zip"), str(src), "--rename", "src/take1.wav=src/take1.flac")
    assert bad.returncode == 3


@pytest.mark.skipif(sys.platform == "win32", reason="symlinks need privileges on Windows")
def test_symlinks_need_yes(tmp_path: Path) -> None:
    src = tmp_path / "src"
    src.mkdir()
    (src / "a.txt").write_text("a")
    (src / "link").symlink_to("a.txt")
    r = run("create", str(tmp_path / "x.zip"), str(src))
    assert r.returncode == 4
    assert b"symbolic links" in r.stderr
    assert run("create", "--yes", str(tmp_path / "x.zip"), str(src)).returncode == 0


def test_stdout_output_is_a_valid_streamed_zip(tmp_path: Path, session: Path) -> None:
    r = run("create", "-", str(session))
    assert r.returncode == 0, r.stderr
    archive = tmp_path / "streamed.zip"
    archive.write_bytes(r.stdout)
    with zipfile.ZipFile(archive) as z:
        assert z.testzip() is None
    dest = tmp_path / "d"
    assert run("extract", str(archive), "-d", str(dest)).returncode == 0
    assert (dest / "session" / "take1.wav").read_bytes() == (session / "take1.wav").read_bytes()


def test_edit_and_restore(tmp_path: Path, session: Path) -> None:
    archive = tmp_path / "e.zip"
    assert run("create", str(archive), str(session)).returncode == 0
    extra = tmp_path / "extra.txt"
    extra.write_text("extra")
    r = run("edit", str(archive), "--remove", "session/notes.txt", "--rename", "session/take1.flac=session/scene1.flac", "--add", str(extra))
    assert r.returncode == 0, r.stderr
    with zipfile.ZipFile(archive) as z:
        names = z.namelist()
    assert "session/notes.txt" not in names
    assert "session/scene1.flac" in names
    assert "extra.txt" in names

    flac_dir = tmp_path / "flac"
    assert run("extract", str(archive), "-d", str(flac_dir), "--keep-flac").returncode == 0
    flac_file = flac_dir / "session" / "scene1.flac"
    assert run("restore", str(flac_file)).returncode == 0
    assert (flac_dir / "session" / "take1.wav").read_bytes() == (session / "take1.wav").read_bytes()
    member = flac_dir / "session" / "multi_ch01.flac"
    assert run("restore", str(member), "-o", str(tmp_path / "m.wav")).returncode == 1


def test_usage_errors(tmp_path: Path) -> None:
    assert run().returncode == 2
    assert run("create").returncode == 2
    assert run("extract", "x.zip", "--overwrite", "maybe").returncode == 2
    missing = run("list", str(tmp_path / "missing.zip"))
    assert missing.returncode == 1
    assert b"cannot open" in missing.stderr
    assert run("--version").stdout.strip()
