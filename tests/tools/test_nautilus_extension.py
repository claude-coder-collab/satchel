# SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
# Copyright (c) 2026 Venn Audio Ltd.
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "packaging" / "linux" / "nautilus"))

import satchel  # noqa: E402


def test_no_selection_offers_nothing() -> None:
    assert satchel.menu_entries([]) == []


def test_files_offer_compress_only() -> None:
    entries = satchel.menu_entries(["/a/take.wav", "/a/docs"])
    assert [e.name for e in entries] == ["Satchel::compress"]
    assert entries[0].command == ["satchel-gui", "--compress", "/a/take.wav", "/a/docs"]


def test_zips_offer_extract_and_browse() -> None:
    entries = satchel.menu_entries(["/a/b.ZIP"])
    assert [e.name for e in entries] == ["Satchel::compress", "Satchel::extract", "Satchel::browse"]
    assert entries[2].command == ["satchel-gui", "--full", "/a/b.ZIP"]
    mixed = satchel.menu_entries(["/a/b.zip", "/a/c.txt"])
    assert mixed[1].command == ["satchel-gui", "--extract", "/a/b.zip"]
    assert len(mixed) == 2
