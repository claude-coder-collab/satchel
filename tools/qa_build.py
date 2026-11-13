#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 Satchel contributors
"""Builds an unsigned QA package the way the release workflow does: the same Qt version, CMake flags,
deployment target and architectures, all read from .github/workflows/release.yml."""

from __future__ import annotations

import argparse
import importlib.util
import re
import shlex
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path

import check_macos_min

REPO = Path(__file__).resolve().parents[1]
RELEASE_YML = REPO / ".github" / "workflows" / "release.yml"
PACKAGE_SUFFIXES = (".dmg", ".msi", ".deb", ".tar.gz")


@dataclass(frozen=True)
class Platform:
    job: str
    qt_version_key: str | None = None
    aqt_host: str = ""
    aqt_arch: str = ""
    qt_dir: str = ""


PLATFORMS = {
    "darwin": Platform("macos", "QT_VERSION_MACOS", "mac", "clang_64", "macos"),
    "win32": Platform("windows", "QT_VERSION_WINDOWS", "windows", "win64_msvc2022_64", "msvc2022_64"),
    "linux": Platform("linux"),
}


@dataclass(frozen=True)
class Configure:
    preset: str
    defines: list[str]

    def define(self, name: str) -> str | None:
        prefix = f"-D{name}="
        return next((d.removeprefix(prefix) for d in self.defines if d.startswith(prefix)), None)


def job_block(workflow: str, job: str) -> str:
    match = re.search(rf"^  {re.escape(job)}:\n(.*?)(?=^  \S|\Z)", workflow, re.MULTILINE | re.DOTALL)
    if not match:
        raise ValueError(f"no job {job!r} in the release workflow")
    return match.group(1)


def env_value(workflow: str, key: str) -> str:
    match = re.search(rf"^\s+{re.escape(key)}:\s*\"?([^\"\s]+)\"?\s*$", workflow, re.MULTILINE)
    if not match:
        raise ValueError(f"no {key} in the release workflow")
    return match.group(1)


def release_configure(workflow: str, job: str) -> Configure:
    """The job's `cmake --preset` command, minus arguments that use workflow variables such as the tag version."""
    for line in job_block(workflow, job).splitlines():
        words = shlex.split(line)
        if words[:2] == ["cmake", "--preset"]:
            return Configure(words[2], [w for w in words[3:] if "$" not in w])
    raise ValueError(f"no cmake --preset command in job {job!r}")


def qt_prefix(root: Path, version: str, platform: Platform) -> Path:
    return root / version / platform.qt_dir


def aqt_command(root: Path, version: str, platform: Platform) -> list[str]:
    return [
        sys.executable, "-m", "aqt", "install-qt", platform.aqt_host, "desktop", version, platform.aqt_arch,
        "--outputdir", str(root),
    ]  # fmt: skip


def install_qt(root: Path, version: str, platform: Platform) -> Path:
    prefix = qt_prefix(root, version, platform)
    if (prefix / "lib" / "cmake" / "Qt6").is_dir():
        return prefix
    if importlib.util.find_spec("aqt") is None:
        sys.exit(f"Qt {version} is not in {root} and aqtinstall is missing: pip install aqtinstall")
    root.mkdir(parents=True, exist_ok=True)
    subprocess.run(aqt_command(root, version, platform), check=True, cwd=root)
    return prefix


def agl_workaround(find_wrap_opengl: str, sdk: Path) -> list[str]:
    """Qt 6.8 links AGL unconditionally, but recent macOS SDKs no longer ship it, so point Qt's AGL lookup at
    OpenGL instead."""
    frameworks = sdk / "System" / "Library" / "Frameworks"
    if "WrapOpenGL_AGL" not in find_wrap_opengl or (frameworks / "AGL.framework").exists():
        return []
    return [f"-DWrapOpenGL_AGL={frameworks / 'OpenGL.framework'}"]


def macos_sdk_workarounds(prefix: Path) -> list[str]:
    find_wrap = prefix / "lib" / "cmake" / "Qt6" / "FindWrapOpenGL.cmake"
    if not find_wrap.exists():
        return []
    sdk = subprocess.run(["xcrun", "--show-sdk-path"], check=True, capture_output=True, text=True).stdout.strip()
    return agl_workaround(find_wrap.read_text(encoding="utf-8"), Path(sdk))


def configure_command(configure: Configure, build_dir: Path, prefix: Path | None, extra: list[str]) -> list[str]:
    command = ["cmake", "--preset", configure.preset, "--fresh", "-B", str(build_dir), *configure.defines]
    if prefix is not None:
        command.append(f"-DCMAKE_PREFIX_PATH={prefix}")
    return command + extra


def labelled(package: Path, label: str) -> Path:
    suffix = next(s for s in PACKAGE_SUFFIXES if package.name.endswith(s))
    return package.with_name(f"{package.name.removesuffix(suffix)}-{label}{suffix}")


def new_packages(build_dir: Path, since: float) -> list[Path]:
    return sorted(
        p
        for p in build_dir.iterdir()
        if p.is_file() and p.name.endswith(PACKAGE_SUFFIXES) and p.stat().st_mtime >= since
    )


def staged_app(build_dir: Path, dmg: Path) -> Path:
    return build_dir / "_CPack_Packages" / "Darwin" / "DragNDrop" / dmg.stem / "Satchel.app"


def run(command: list[str], cwd: Path = REPO) -> None:
    print("+", shlex.join(command), flush=True)
    subprocess.run(command, check=True, cwd=cwd)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--label", help="added to package names, e.g. pr65")
    parser.add_argument("--qt-root", type=Path, default=Path.home() / "Qt", help="where official Qt is installed")
    parser.add_argument("--build-dir", type=Path, help="default: build/qa-<platform>")
    args = parser.parse_args()

    key = "linux" if sys.platform.startswith("linux") else sys.platform
    if key not in PLATFORMS:
        sys.exit(f"unsupported platform {sys.platform}")
    platform = PLATFORMS[key]
    workflow = RELEASE_YML.read_text(encoding="utf-8")
    configure = release_configure(workflow, platform.job)
    build_dir = (args.build_dir or REPO / "build" / f"qa-{platform.job}").resolve()

    if key == "win32" and shutil.which("cl") is None:
        sys.exit("cl not found: run from a Visual Studio x64 developer prompt")
    prefix = None
    extra: list[str] = []
    if platform.qt_version_key:
        prefix = install_qt(args.qt_root.expanduser().resolve(), env_value(workflow, platform.qt_version_key), platform)
    if key == "darwin":
        extra = macos_sdk_workarounds(prefix)

    started = time.time()
    run(configure_command(configure, build_dir, prefix, extra))
    run(["cmake", "--build", str(build_dir), "--config", "Release"])
    run(["cpack", "-C", "Release"], cwd=build_dir)

    packages = new_packages(build_dir, started)
    if not packages:
        sys.exit("cpack made no package")
    target = configure.define("CMAKE_OSX_DEPLOYMENT_TARGET")
    for dmg in (p for p in packages if p.suffix == ".dmg" and target):
        if not check_macos_min.check(staged_app(build_dir, dmg), target):
            sys.exit(f"{dmg.name} would not start on macOS {target}")
    if args.label:
        packages = [p.rename(labelled(p, args.label)) for p in packages]
    if key == "linux":
        print("The release also makes an AppImage with linuxdeploy; this script does not.")
    for package in packages:
        print(package)


if __name__ == "__main__":
    main()
