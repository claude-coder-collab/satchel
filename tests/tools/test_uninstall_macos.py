# SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
# Copyright (c) 2026 Venn Audio Ltd.
from __future__ import annotations

import plistlib
import subprocess
import sys
from collections.abc import Sequence
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "packaging" / "macos"))

import uninstall  # noqa: E402

PLUGINKIT_OUTPUT = """+    com.vennaudio.satchel.preview(0.1.0)
\t            Path = /Users/x/build/Satchel.app/Contents/PlugIns/SatchelPreview.appex
\t            UUID = 07EB86B9

 (1 plug-in)
"""


class FakeRunner:
    def __init__(self, pluginkit_output: str = "", fail: set[str] | None = None) -> None:
        self.calls: list[list[str]] = []
        self.pluginkit_output = pluginkit_output
        self.fail = fail or set()

    def __call__(self, command: Sequence[str]) -> subprocess.CompletedProcess[str]:
        self.calls.append(list(command))
        stdout = self.pluginkit_output if command[:2] == ["pluginkit", "-mAvvv"] else ""
        code = 1 if command[0] in self.fail else 0
        return subprocess.CompletedProcess(list(command), code, stdout, "")


def make_app(root: Path, bundle_id: str = "com.example.satchel") -> Path:
    app = root / "Applications" / "Satchel.app"
    (app / "Contents" / "PlugIns" / "SatchelPreview.appex").mkdir(parents=True)
    with (app / "Contents" / "Info.plist").open("wb") as handle:
        plistlib.dump({"CFBundleIdentifier": bundle_id}, handle)
    return app


def test_parse_registered_extensions() -> None:
    assert uninstall.parse_registered_extensions(PLUGINKIT_OUTPUT) == [
        Path("/Users/x/build/Satchel.app/Contents/PlugIns/SatchelPreview.appex")
    ]
    assert uninstall.parse_registered_extensions("") == []


def test_bundle_id_falls_back_without_plist(tmp_path: Path) -> None:
    assert uninstall.bundle_id_of(tmp_path / "missing.app") == uninstall.DEFAULT_BUNDLE_ID
    assert uninstall.bundle_id_of(make_app(tmp_path)) == "com.example.satchel"


def test_uninstall_removes_app_extension_and_settings(tmp_path: Path) -> None:
    app = make_app(tmp_path)
    prefs = tmp_path / "Library" / "Preferences" / "com.example.satchel.plist"
    support = tmp_path / "Library" / "Application Support" / "com.example.satchel"
    prefs.parent.mkdir(parents=True)
    support.mkdir(parents=True)
    prefs.write_bytes(b"x")
    (support / "a").write_bytes(b"x")
    runner = FakeRunner(PLUGINKIT_OUTPUT)

    report = uninstall.uninstall([app], tmp_path, runner=runner)

    assert not report.failed
    assert not app.exists()
    assert (tmp_path / ".Trash" / "Satchel.app").is_dir()
    assert not prefs.exists()
    assert not support.exists()
    commands = [call[:2] for call in runner.calls]
    assert ["pluginkit", "-r"] in commands
    assert ["defaults", "delete"] in commands
    unregistered = [call[2] for call in runner.calls if call[:2] == ["pluginkit", "-r"]]
    assert str(app / "Contents/PlugIns/SatchelPreview.appex") in unregistered
    assert "/Users/x/build/Satchel.app/Contents/PlugIns/SatchelPreview.appex" in unregistered
    assert runner.calls[-1] == [uninstall.PBS, "-flush"]


def test_keep_settings_leaves_user_data(tmp_path: Path) -> None:
    app = make_app(tmp_path)
    prefs = tmp_path / "Library" / "Preferences" / "com.example.satchel.plist"
    prefs.parent.mkdir(parents=True)
    prefs.write_bytes(b"x")
    runner = FakeRunner()

    uninstall.uninstall([app], tmp_path, keep_settings=True, runner=runner)

    assert prefs.exists()
    assert not any(call[0] == "defaults" for call in runner.calls)


def test_dry_run_changes_nothing(tmp_path: Path) -> None:
    app = make_app(tmp_path)
    runner = FakeRunner()

    report = uninstall.uninstall([app], tmp_path, dry_run=True, runner=runner)

    assert app.exists()
    assert all(call[:2] == ["pluginkit", "-mAvvv"] for call in runner.calls)
    assert report.done and all(line.startswith("would ") for line in report.done)


def test_missing_app_still_cleans_up_without_error(tmp_path: Path) -> None:
    runner = FakeRunner()
    report = uninstall.uninstall([tmp_path / "Applications" / "Satchel.app"], tmp_path, runner=runner)
    assert not report.failed


def test_failed_command_is_reported(tmp_path: Path) -> None:
    report = uninstall.uninstall([make_app(tmp_path)], tmp_path, runner=FakeRunner(fail={"pluginkit"}))
    assert any("Quick Look extension" in line for line in report.failed)
