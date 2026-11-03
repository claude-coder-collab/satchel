# SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
# Copyright (c) 2026 Venn Audio Ltd.
from __future__ import annotations

import os
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))

import check_macos_min  # noqa: E402
import qa_build  # noqa: E402

WORKFLOW = """\
env:
  QT_VERSION_MACOS: "6.11.3"
  QT_VERSION_WINDOWS: "6.10.3"

jobs:
  macos:
    steps:
      - name: Build
        run: |
          cmake --preset clang -DZP_BUILD_TESTS=OFF -DZP_VERSION="$VERSION" -DCMAKE_OSX_DEPLOYMENT_TARGET=13.3 "-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64"
          cmake --build build/clang --config Release
  wheel:
    steps:
      - run: cmake --preset clang -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0
"""

OTOOL = """\
Satchel.app/Contents/MacOS/Satchel (architecture x86_64):
Load command 9
      cmd LC_BUILD_VERSION
  cmdsize 32
 platform 1
    minos 13.3
      sdk 26.0
Satchel.app/Contents/MacOS/Satchel (architecture arm64):
Load command 9
      cmd LC_VERSION_MIN_MACOSX
  cmdsize 16
  version 14.0
      sdk 14.0
Load command 10
      cmd LC_SOURCE_VERSION
  version 0.0
"""


def test_reads_qt_versions() -> None:
    assert qa_build.env_value(WORKFLOW, "QT_VERSION_MACOS") == "6.11.3"
    assert qa_build.env_value(WORKFLOW, "QT_VERSION_WINDOWS") == "6.10.3"
    with pytest.raises(ValueError):
        qa_build.env_value(WORKFLOW, "QT_VERSION_LINUX")


def test_release_configure_drops_workflow_variables_and_stays_in_its_job() -> None:
    configure = qa_build.release_configure(WORKFLOW, "macos")
    assert configure.preset == "clang"
    assert configure.defines == [
        "-DZP_BUILD_TESTS=OFF",
        "-DCMAKE_OSX_DEPLOYMENT_TARGET=13.3",
        "-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64",
    ]
    assert configure.define("CMAKE_OSX_DEPLOYMENT_TARGET") == "13.3"
    assert configure.define("ZP_VERSION") is None
    with pytest.raises(ValueError):
        qa_build.release_configure(WORKFLOW, "windows")


@pytest.mark.parametrize("job", ["macos", "windows", "linux"])
def test_real_release_workflow_has_build_commands(job: str) -> None:
    workflow = qa_build.RELEASE_YML.read_text(encoding="utf-8")
    configure = qa_build.release_configure(workflow, job)
    assert "-DZP_BUILD_TESTS=OFF" in configure.defines
    platform = next(p for p in qa_build.PLATFORMS.values() if p.job == job)
    if platform.qt_version_key:
        assert qa_build.env_value(workflow, platform.qt_version_key)


def test_configure_command(tmp_path: Path) -> None:
    configure = qa_build.Configure("clang", ["-DZP_BUILD_TESTS=OFF"])
    command = qa_build.configure_command(configure, tmp_path / "b", tmp_path / "qt", ["-DX=1"])
    assert command == [
        "cmake", "--preset", "clang", "--fresh", "-B", str(tmp_path / "b"), "-DZP_BUILD_TESTS=OFF",
        f"-DCMAKE_PREFIX_PATH={tmp_path / 'qt'}", "-DX=1",
    ]  # fmt: skip
    assert "-DCMAKE_PREFIX_PATH" not in " ".join(qa_build.configure_command(configure, tmp_path, None, []))


def test_qt_install_location_and_command(tmp_path: Path) -> None:
    mac = qa_build.PLATFORMS["darwin"]
    assert qa_build.qt_prefix(tmp_path, "6.11.3", mac) == tmp_path / "6.11.3" / "macos"
    command = qa_build.aqt_command(tmp_path, "6.11.3", mac)
    assert command[1:] == ["-m", "aqt", "install-qt", "mac", "desktop", "6.11.3", "clang_64", "--outputdir", str(tmp_path)]


def test_installed_qt_is_reused(tmp_path: Path) -> None:
    platform = qa_build.PLATFORMS["darwin"]
    prefix = qa_build.qt_prefix(tmp_path, "6.11.3", platform)
    (prefix / "lib" / "cmake" / "Qt6").mkdir(parents=True)
    assert qa_build.install_qt(tmp_path, "6.11.3", platform) == prefix


def test_agl_workaround_only_when_sdk_lacks_agl(tmp_path: Path) -> None:
    qt68 = "find_library(WrapOpenGL_AGL NAMES AGL)"
    frameworks = tmp_path / "System" / "Library" / "Frameworks"
    assert qa_build.agl_workaround(qt68, tmp_path) == [f"-DWrapOpenGL_AGL={frameworks / 'OpenGL.framework'}"]
    assert qa_build.agl_workaround("target_link_libraries(WrapOpenGL::WrapOpenGL INTERFACE OpenGL::GL)", tmp_path) == []
    (frameworks / "AGL.framework").mkdir(parents=True)
    assert qa_build.agl_workaround(qt68, tmp_path) == []


@pytest.mark.parametrize(
    ("name", "expected"),
    [
        ("Satchel-0.4.0-Darwin-universal.dmg", "Satchel-0.4.0-Darwin-universal-pr65.dmg"),
        ("Satchel-0.4.0-Linux-x86_64.tar.gz", "Satchel-0.4.0-Linux-x86_64-pr65.tar.gz"),
        ("satchel_0.4.0_amd64.deb", "satchel_0.4.0_amd64-pr65.deb"),
    ],
)
def test_labelled(name: str, expected: str) -> None:
    assert qa_build.labelled(Path("b") / name, "pr65") == Path("b") / expected


def test_new_packages_ignores_old_and_other_files(tmp_path: Path) -> None:
    old = tmp_path / "Satchel-0.3.0-Darwin-universal.dmg"
    old.touch()
    os.utime(old, (1000, 1000))
    (tmp_path / "CMakeCache.txt").touch()
    new = tmp_path / "Satchel-0.4.0-Darwin-universal.dmg"
    new.touch()
    assert qa_build.new_packages(tmp_path, 2000) == [new]


def test_min_versions_reads_every_slice() -> None:
    assert check_macos_min.min_versions(OTOOL) == ["13.3", "14.0"]


def test_too_new_compares_numerically() -> None:
    binaries = {Path("a"): ["13.3", "13.0"], Path("b"): ["13.10"], Path("c"): []}
    assert check_macos_min.too_new(binaries, "13.3") == {Path("b"): "13.10"}
    assert check_macos_min.too_new(binaries, "14") == {}


def test_macho_files(tmp_path: Path) -> None:
    app = tmp_path / "Satchel.app"
    (app / "Contents" / "MacOS").mkdir(parents=True)
    binary = app / "Contents" / "MacOS" / "Satchel"
    binary.write_bytes(bytes.fromhex("cffaedfe") + b"\0" * 28)
    (app / "Contents" / "Info.plist").write_text("<plist/>")
    (app / "Contents" / "MacOS" / "link").symlink_to(binary)
    assert list(check_macos_min.macho_files(app)) == [binary]
    assert list(check_macos_min.macho_files(binary)) == [binary]


def test_check_fails_without_binaries(tmp_path: Path) -> None:
    assert not check_macos_min.check(tmp_path, "13.3")
