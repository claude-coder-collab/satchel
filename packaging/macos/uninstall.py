#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
# Copyright (c) 2026 Venn Audio Ltd.
"""Removes Satchel from macOS: the app, its Quick Look extension, Finder services and user data."""

from __future__ import annotations

import argparse
import plistlib
import re
import shutil
import subprocess
import sys
import time
from collections.abc import Callable, Sequence
from dataclasses import dataclass, field
from pathlib import Path

DEFAULT_BUNDLE_ID = "com.vennaudio.satchel"
APP_NAME = "Satchel.app"
PBS = "/System/Library/CoreServices/pbs"
LSREGISTER = (
    "/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework"
    "/Support/lsregister"
)

Runner = Callable[[Sequence[str]], subprocess.CompletedProcess[str]]


def run_command(command: Sequence[str]) -> subprocess.CompletedProcess[str]:
    try:
        return subprocess.run(command, capture_output=True, text=True, check=False)
    except OSError as error:
        return subprocess.CompletedProcess(list(command), 127, "", str(error))


@dataclass
class Report:
    done: list[str] = field(default_factory=list)
    failed: list[str] = field(default_factory=list)


def default_app_paths(home: Path) -> list[Path]:
    return [Path("/Applications") / APP_NAME, home / "Applications" / APP_NAME]


def bundle_id_of(app: Path, fallback: str = DEFAULT_BUNDLE_ID) -> str:
    try:
        with (app / "Contents" / "Info.plist").open("rb") as handle:
            return str(plistlib.load(handle).get("CFBundleIdentifier", fallback))
    except (OSError, plistlib.InvalidFileException):
        return fallback


def user_data_paths(home: Path, bundle_id: str) -> list[Path]:
    library = home / "Library"
    return [
        library / "Preferences" / f"{bundle_id}.plist",
        library / "Application Support" / bundle_id,
        library / "Application Support" / "Satchel",
        library / "Caches" / bundle_id,
        library / "HTTPStorages" / bundle_id,
        library / "HTTPStorages" / f"{bundle_id}.binarycookies",
        library / "Saved Application State" / f"{bundle_id}.savedState",
        library / "Logs" / "Satchel",
    ]


def parse_registered_extensions(pluginkit_output: str) -> list[Path]:
    return [Path(match) for match in re.findall(r"^\s*Path = (.+\.appex)\s*$", pluginkit_output, re.MULTILINE)]


def registered_extensions(bundle_id: str, runner: Runner) -> list[Path]:
    result = runner(["pluginkit", "-mAvvv", "-i", f"{bundle_id}.preview"])
    return parse_registered_extensions(result.stdout) if result.returncode == 0 else []


def trash(path: Path, home: Path) -> Path:
    trash_dir = home / ".Trash"
    trash_dir.mkdir(exist_ok=True)
    target = trash_dir / path.name
    if target.exists():
        target = trash_dir / f"{path.stem} {int(time.time())}{path.suffix}"
    shutil.move(str(path), str(target))
    return target


def remove_path(path: Path) -> None:
    if path.is_dir() and not path.is_symlink():
        shutil.rmtree(path)
    else:
        path.unlink()


def uninstall(
    apps: Sequence[Path],
    home: Path,
    keep_settings: bool = False,
    dry_run: bool = False,
    runner: Runner = run_command,
) -> Report:
    report = Report()

    def step(description: str, action: Callable[[], bool]) -> None:
        if dry_run:
            report.done.append(f"would {description}")
        elif action():
            report.done.append(description)
        else:
            report.failed.append(description)

    def command(description: str, argv: Sequence[str], required: bool = True) -> None:
        step(description, lambda: runner(argv).returncode == 0 or not required)

    existing = [app for app in apps if app.exists()]
    bundle_id = bundle_id_of(existing[0]) if existing else DEFAULT_BUNDLE_ID

    script = f'if application id "{bundle_id}" is running then tell application id "{bundle_id}" to quit'
    command("quit Satchel", ["osascript", "-e", script], required=False)

    extensions = {app / "Contents" / "PlugIns" / "SatchelPreview.appex" for app in existing}
    extensions.update(registered_extensions(bundle_id, runner))
    for extension in sorted(extensions):
        command(f"unregister Quick Look extension {extension}", ["pluginkit", "-r", str(extension)])

    for app in existing:
        command(f"unregister {app} from Launch Services", [LSREGISTER, "-u", str(app)], required=False)
        step(f"move {app} to the Trash", lambda app=app: _try(lambda: trash(app, home)))

    if not keep_settings:
        command(f"delete preferences for {bundle_id}", ["defaults", "delete", bundle_id], required=False)
        for path in user_data_paths(home, bundle_id):
            if path.exists():
                step(f"remove {path}", lambda path=path: _try(lambda: remove_path(path)))

    command("refresh Finder services", [PBS, "-flush"], required=False)
    return report


def _try(action: Callable[[], object]) -> bool:
    try:
        action()
    except OSError:
        return False
    return True


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", action="append", type=Path, default=[], help="extra Satchel.app to remove")
    parser.add_argument("--keep-settings", action="store_true", help="keep preferences, caches and saved state")
    parser.add_argument("--dry-run", action="store_true", help="print what would be done")
    parser.add_argument("-y", "--yes", action="store_true", help="do not ask for confirmation")
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    if sys.platform != "darwin":
        print("This uninstaller is for macOS.", file=sys.stderr)
        return 2
    args = parse_args(argv)
    home = Path.home()
    apps = [*default_app_paths(home), *args.app]
    if not args.yes and not args.dry_run:
        answer = input("Remove Satchel, its Quick Look extension, Finder services and settings? [y/N] ")
        if answer.strip().lower() not in {"y", "yes"}:
            return 1
    report = uninstall(apps, home, args.keep_settings, args.dry_run)
    for line in report.done:
        print(f"ok: {line}")
    for line in report.failed:
        print(f"FAILED: {line}", file=sys.stderr)
    return 1 if report.failed else 0


if __name__ == "__main__":
    sys.exit(main())
