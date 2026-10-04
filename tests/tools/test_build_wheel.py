# SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
# Copyright (c) 2026 Venn Audio Ltd.
from __future__ import annotations

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))

import build_wheel  # noqa: E402


def test_set_version_replaces_the_project_version() -> None:
    text = '[project]\nname = "satchel"\nversion = "0.1.0"\n'
    assert build_wheel.set_version(text, "1.2.3") == '[project]\nname = "satchel"\nversion = "1.2.3"\n'


@pytest.mark.parametrize("bad", ["v1.0", "1.0-beta", ""])
def test_set_version_rejects_non_pep440(bad: str) -> None:
    with pytest.raises(ValueError):
        build_wheel.set_version('version = "0.1.0"\n', bad)


def test_library_names() -> None:
    assert build_wheel.library_name("win32") == "satchel.dll"
    assert build_wheel.library_name("darwin") == "libsatchel.dylib"
    assert build_wheel.library_name("linux") == "libsatchel.so"
