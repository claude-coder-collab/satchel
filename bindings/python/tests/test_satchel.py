# SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
# Copyright (c) 2026 Venn Audio Ltd.
from __future__ import annotations

import hashlib
import math
import os
import shutil
import struct
import subprocess
import zipfile
from pathlib import Path

import pytest

import satchel

PHP_REFERENCE = Path(__file__).resolve().parent.parent.parent / "php" / "tests" / "reference.php"


def make_wav(frames: int = 20000, channels: int = 2, rate: int = 48000) -> bytes:
    samples = bytearray()
    for f in range(frames):
        for c in range(channels):
            samples += struct.pack("<h", int(8000 * math.sin(f * (0.01 + 0.002 * c))))
    fmt = struct.pack("<HHIIHH", 1, channels, rate, rate * channels * 2, channels * 2, 16)
    body = b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", len(samples)) + bytes(samples)
    return b"RIFF" + struct.pack("<I", len(body)) + body


@pytest.fixture()
def ctx() -> satchel.Context:
    return satchel.Context(threads=2)


def test_version() -> None:
    assert satchel.version()


def test_plan_build_extract_round_trip(ctx: satchel.Context, tmp_path: Path) -> None:
    src = tmp_path / "src" / "project"
    (src / "sub").mkdir(parents=True)
    (src / "notes.txt").write_text("hello " * 1000)
    (src / "sub" / "take1.wav").write_bytes(make_wav())
    (src / "empty").mkdir()
    plan = ctx.plan([src])
    names = [e.output_name for e in plan.entries]
    assert "project/sub/take1.flac" in names
    assert "README - How to restore original audio.txt" in names
    flac_entry = next(e for e in plan.entries if e.codec == "flac")
    assert flac_entry.restored_name == "project/sub/take1.wav"
    assert plan.executable

    seen: list[tuple[int, int]] = []
    out = tmp_path / "out.zip"
    result = plan.build(out, progress=lambda done, total: seen.append((done, total)))
    assert result.ok
    assert seen
    with zipfile.ZipFile(out) as z:
        assert z.testzip() is None
        assert "project/sub/take1.flac" in z.namelist()

    archive = ctx.open(out)
    assert archive.app_version.startswith("Satchel")
    entries = archive.entries
    assert any(e.flac_restorable for e in entries)
    dest = tmp_path / "dest"
    archive.extract(dest)
    assert (dest / "project" / "sub" / "take1.wav").read_bytes() == (src / "sub" / "take1.wav").read_bytes()
    assert (dest / "project" / "notes.txt").read_text() == "hello " * 1000
    assert (dest / "project" / "empty").is_dir()
    assert not (dest / "README - How to restore original audio.txt").exists()

    verify = archive.verify()
    assert all(o.status == 0 for o in verify.outcomes)


def test_conflicts_and_resolutions(ctx: satchel.Context) -> None:
    plan = ctx.plan_memory({"a.txt": b"1", "A.TXT": b"2", "b.txt": b"3"})
    assert not plan.executable
    (conflict,) = plan.conflicts
    assert conflict.kind == "collision"
    with pytest.raises(satchel.SatchelError) as err:
        plan.build_bytes()
    assert err.value.name == "CONFLICTS_UNRESOLVED"
    with pytest.raises(satchel.SatchelError) as err:
        plan.resolve([satchel.Resolution.rename(conflict.entries[1], "b.txt")])
    assert err.value.name == "NAME_COLLISION"
    plan.resolve([satchel.Resolution.rename(conflict.entries[1], "c.txt")])
    assert plan.executable
    data = plan.build_bytes()
    assert data[:2] == b"PK"


def test_cancel_from_progress(ctx: satchel.Context, tmp_path: Path) -> None:
    plan = ctx.plan_memory({f"f{i}.bin": os.urandom(1 << 20) for i in range(8)})
    out = tmp_path / "x.zip"
    with pytest.raises(satchel.SatchelError) as err:
        plan.build(out, progress=lambda done, total: False)
    assert err.value.name == "CANCELLED"
    assert not out.exists()
    assert list(tmp_path.iterdir()) == []


def test_progress_exception_propagates(ctx: satchel.Context, tmp_path: Path) -> None:
    plan = ctx.plan_memory({"a": b"x" * 100})

    def boom(done: int, total: int) -> bool:
        raise RuntimeError("from callback")

    with pytest.raises(RuntimeError, match="from callback"):
        plan.build(tmp_path / "x.zip", progress=boom)


def test_editor_in_place(ctx: satchel.Context, tmp_path: Path) -> None:
    path = tmp_path / "e.zip"
    ctx.plan_memory({"keep.txt": b"keep", "drop.txt": b"drop"}).build(path)
    editor = ctx.edit(path)
    names = [e.output_name for e in editor.entries]
    editor.remove(names.index("drop.txt"))
    editor.rename(0, "kept.txt")
    extra = tmp_path / "new.wav"
    extra.write_bytes(make_wav(frames=500))
    editor.add([extra])
    editor.commit()
    with zipfile.ZipFile(path) as z:
        assert sorted(z.namelist()) == ["README - How to restore original audio.txt", "kept.txt", "new.flac"]


def test_extract_decisions_and_issues(ctx: satchel.Context, tmp_path: Path) -> None:
    path = tmp_path / "a.zip"
    ctx.plan_memory({"x.txt": b"new"}).build(path)
    dest = tmp_path / "d"
    dest.mkdir()
    (dest / "x.txt").write_text("old")
    archive = ctx.open(path)
    xplan = archive.extraction_plan(dest)
    assert [i.kind for i in xplan.issues] == ["exists_at_destination"]
    with pytest.raises(satchel.SatchelError) as err:
        xplan.extract()
    assert err.value.name == "DECISION_REQUIRED"
    xplan.decide(0, "replace")
    xplan.extract()
    assert (dest / "x.txt").read_text() == "new"


def test_errors_are_typed(ctx: satchel.Context, tmp_path: Path) -> None:
    with pytest.raises(satchel.SatchelError) as err:
        ctx.open(tmp_path / "missing.zip")
    assert err.value.name == "IO_ERROR"
    junk = tmp_path / "junk.zip"
    junk.write_bytes(b"not a zip at all" * 10)
    with pytest.raises(satchel.SatchelError) as err:
        ctx.open(junk)
    assert err.value.name == "CORRUPT_ARCHIVE"


def build_reference(ctx: satchel.Context) -> bytes:
    files = {"docs/readme.txt": b"reference " * 300, "audio/take.wav": make_wav(frames=5000), "bin/noise.bin": hashlib.sha256(b"x").digest() * 4000}
    return ctx.plan_memory(files, directories=["docs", "audio", "bin"], mtime=1700000000).build_bytes()


def test_reference_archive_is_reproducible(ctx: satchel.Context) -> None:
    assert build_reference(ctx) == build_reference(satchel.Context(threads=7))


@pytest.mark.skipif(shutil.which("php") is None or not PHP_REFERENCE.exists(), reason="php or the PHP binding not available")
def test_php_produces_the_same_archive(ctx: satchel.Context, tmp_path: Path) -> None:
    out = tmp_path / "php.zip"
    subprocess.run(["php", "-d", "ffi.enable=1", str(PHP_REFERENCE), str(out)], check=True)
    assert out.read_bytes() == build_reference(ctx)


def test_preview(ctx: satchel.Context, tmp_path: Path) -> None:
    archive = tmp_path / "p.zip"
    ctx.plan_memory({"a.txt": b"hello " * 100}).build(archive)
    summary = satchel.preview(archive)
    assert summary["kind"] == "zip"
    assert summary["entries"][0]["name"] == "a.txt"
    assert "<h1>p.zip</h1>" in satchel.preview(archive, html=True)
    with pytest.raises(satchel.SatchelError):
        satchel.preview(Path(__file__))
