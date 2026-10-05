# SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
# Copyright (c) 2026 Venn Audio Ltd.
"""Browser end-to-end tests of apps/web (Playwright). SATCHEL_WEB_DIR points to the built app."""

from __future__ import annotations

import base64
import io
import math
import os
import socket
import struct
import sys
import threading
import zipfile
from pathlib import Path

import pytest

playwright = pytest.importorskip("playwright.sync_api")

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from serve_web import make_server  # noqa: E402

WEB_DIR = Path(os.environ.get("SATCHEL_WEB_DIR", ROOT / "build" / "wasm" / "web"))
BROWSERS = [b for b in os.environ.get("SATCHEL_BROWSERS", "chromium").split(",") if b]
# Emulated phones, "Device name:engine" separated by commas, e.g. "iPhone 15:webkit,Pixel 7:chromium".
DEVICES = [tuple(d.rsplit(":", 1)) for d in os.environ.get("SATCHEL_DEVICES", "").split(",") if d]
TARGETS = [(engine, None) for engine in BROWSERS] + [(engine, name) for name, engine in DEVICES]
if not (WEB_DIR / "satchel.wasm").exists():
    pytest.skip("browser app not built; set SATCHEL_WEB_DIR", allow_module_level=True)


def make_wav(frames: int = 30000, channels: int = 2) -> bytes:
    samples = bytearray()
    for f in range(frames):
        for c in range(channels):
            samples += struct.pack("<h", int(8000 * math.sin(f * (0.01 + 0.002 * c))))
    fmt = struct.pack("<HHIIHH", 1, channels, 48000, 48000 * channels * 2, channels * 2, 16)
    body = b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", len(samples)) + bytes(samples)
    return b"RIFF" + struct.pack("<I", len(body)) + body


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def serve(isolate: bool):
    server = make_server(WEB_DIR, free_port(), isolate)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    return server, f"http://127.0.0.1:{server.server_address[1]}/"


class Target:
    """A launched browser plus the context options of an emulated device (if any)."""

    def __init__(self, browser, options: dict) -> None:
        self.browser = browser
        self.options = options

    def new_context(self, **kwargs):
        return self.browser.new_context(**self.options, **kwargs)


@pytest.fixture(scope="module", params=TARGETS, ids=lambda t: t[1] or t[0])
def browser(request):
    engine, device = request.param
    with playwright.sync_playwright() as p:
        b = getattr(p, engine).launch()
        yield Target(b, dict(p.devices[device]) if device else {})
        b.close()


@pytest.fixture()
def page(browser):
    server, url = serve(True)
    context = browser.new_context(accept_downloads=True, service_workers="block")
    page = context.new_page()
    errors: list[str] = []
    page.on("pageerror", lambda e: errors.append(str(e)))
    page.goto(url)
    page.wait_for_selector("body[data-ready=true]", timeout=30000)
    yield page
    context.close()
    server.shutdown()
    assert not errors, errors


WAV = make_wav()


def read_opfs_file(page, path: str) -> bytes:
    data = page.evaluate(
        """async (path) => {
            let dir = await navigator.storage.getDirectory();
            const parts = path.split('/');
            for (const p of parts.slice(0, -1)) dir = await dir.getDirectoryHandle(p);
            const file = await (await dir.getFileHandle(parts.at(-1))).getFile();
            const bytes = new Uint8Array(await file.arrayBuffer());
            let s = '';
            for (let i = 0; i < bytes.length; i += 0x8000) s += String.fromCharCode(...bytes.subarray(i, i + 0x8000));
            return btoa(s);
        }""",
        path,
    )
    return base64.b64decode(data)


def test_create_then_open_and_restore(page, tmp_path: Path) -> None:
    page.set_input_files("#create-input", [
        {"name": "take1.wav", "mimeType": "audio/wav", "buffer": WAV},
        {"name": "notes.txt", "mimeType": "text/plain", "buffer": b"notes " * 1000},
    ])
    page.wait_for_selector("#plan-entries tbody tr")
    assert "take1.flac" in page.inner_text("#plan-entries")
    page.click("#build")
    page.wait_for_selector("#result .ok")
    with page.expect_download() as download:
        page.click("#result a[download]")
    archive = tmp_path / "out.zip"
    download.value.save_as(archive)
    with zipfile.ZipFile(archive) as z:
        assert z.testzip() is None
        assert sorted(z.namelist()) == ["README - How to restore original audio.txt", "notes.txt", "take1.flac"]

    page.set_input_files("#open-input", str(archive))
    page.wait_for_selector("#entries tbody tr")
    assert "take1.wav" in page.inner_text("#entries")
    page.click("#verify")
    page.wait_for_selector("#result .ok:has-text('verified')")
    page.click("#extract")
    page.wait_for_selector("#result :text('extracted')")
    if page.get_attribute("body", "data-storage") == "opfs":
        extract_dir = page.evaluate(
            """async () => { const root = await navigator.storage.getDirectory();
                for await (const [name] of root.entries()) if (name.startsWith('extract-')) return name; }"""
        )
        assert read_opfs_file(page, f"{extract_dir}/take1.wav") == WAV
        assert read_opfs_file(page, f"{extract_dir}/notes.txt") == b"notes " * 1000
    else:
        with page.expect_download() as download:
            page.click("#result a:has-text('take1.wav')")
        restored = tmp_path / "take1.wav"
        download.value.save_as(restored)
        assert restored.read_bytes() == WAV


def test_download_every_file_without_a_directory_picker(browser, tmp_path: Path) -> None:
    server, url = serve(True)
    context = browser.new_context(accept_downloads=True, service_workers="block")
    context.add_init_script("delete window.showDirectoryPicker")
    page = context.new_page()
    page.goto(url)
    page.wait_for_selector("body[data-ready=true]", timeout=30000)
    page.set_input_files("#create-input", [
        {"name": "take1.wav", "mimeType": "audio/wav", "buffer": WAV},
        {"name": "notes.txt", "mimeType": "text/plain", "buffer": b"notes " * 1000},
    ])
    page.wait_for_selector("#plan-entries tbody tr")
    page.click("#build")
    page.wait_for_selector("#result .ok")
    with page.expect_download() as built:
        page.click("#result a[download]")
    archive = tmp_path / "out.zip"
    built.value.save_as(archive)
    page.set_input_files("#open-input", str(archive))
    page.wait_for_selector("#entries tbody tr")
    page.click("#extract")
    page.wait_for_selector("#result button:has-text('Download all')")
    downloads = []
    page.on("download", lambda d: downloads.append(d))
    page.click("#result button:has-text('Download all')")
    page.wait_for_timeout(2500)
    saved = {}
    for d in downloads:
        target = tmp_path / d.suggested_filename
        d.save_as(target)
        saved[d.suggested_filename] = target.read_bytes()
    assert saved["take1.wav"] == WAV
    assert saved["notes.txt"] == b"notes " * 1000
    context.close()
    server.shutdown()


def test_folder_note_only_without_a_directory_picker(browser) -> None:
    server, url = serve(True)
    try:
        for picker in (True, False):
            context = browser.new_context(service_workers="block")
            if not picker:
                context.add_init_script("delete window.showDirectoryPicker")
            page = context.new_page()
            page.goto(url)
            page.wait_for_selector("body[data-ready=true]", timeout=30000)
            has_picker = page.evaluate("typeof window.showDirectoryPicker === 'function'")
            assert page.is_visible("#folder-note") == (not has_picker)
            if not has_picker:
                assert "Chromium" in page.inner_text("#folder-note")
            context.close()
    finally:
        server.shutdown()


def test_conflict_resolution_ui(page) -> None:
    page.set_input_files("#create-input", [
        {"name": "take1.wav", "mimeType": "audio/wav", "buffer": WAV},
        {"name": "take1.flac", "mimeType": "audio/flac", "buffer": b"not really flac"},
    ])
    page.wait_for_selector(".issue.blocking")
    assert page.is_disabled("#build")
    page.click(".issue.blocking button:has-text('Store unconverted')")
    page.wait_for_selector(".issue.blocking", state="detached")
    assert page.is_enabled("#build")
    assert "take1.wav" in page.inner_text("#plan-entries")


def test_missing_isolation_shows_a_specific_error(browser) -> None:
    server, url = serve(False)
    context = browser.new_context(service_workers="block")
    page = context.new_page()
    page.goto(url)
    page.wait_for_selector("#fatal:not([hidden])")
    assert "cross-origin isolated" in page.inner_text("#fatal")
    context.close()
    server.shutdown()


def test_third_party_zip(page, tmp_path: Path) -> None:
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("docs/a.txt", "alpha " * 100)
        z.writestr("../evil.txt", "x")
    path = tmp_path / "third.zip"
    path.write_bytes(buf.getvalue())
    page.set_input_files("#open-input", str(path))
    page.wait_for_selector("#entries tbody tr")
    assert "another tool" in page.inner_text("#archive-summary")
    page.click("#extract")
    page.wait_for_selector("#result :text('problem')")
    assert "escapes the destination" in page.inner_text("#result")
