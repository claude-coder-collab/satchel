# SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
# Copyright (c) 2026 Venn Audio Ltd.
"""Nautilus (nautilus-python) context-menu entries: Compress / Extract with Satchel."""

from __future__ import annotations

import subprocess
from dataclasses import dataclass
from pathlib import Path

APP = "satchel-gui"


@dataclass(frozen=True)
class Entry:
    name: str
    label: str
    command: list[str]


def menu_entries(paths: list[str]) -> list[Entry]:
    """The actions offered for the selected local paths (none for an empty selection)."""
    if not paths:
        return []
    entries = [Entry("Satchel::compress", "Compress with Satchel", [APP, "--compress", *paths])]
    zips = [p for p in paths if Path(p).suffix.lower() == ".zip"]
    if zips:
        entries.append(Entry("Satchel::extract", "Extract with Satchel", [APP, "--extract", *zips]))
    if len(paths) == 1 and zips:
        entries.append(Entry("Satchel::browse", "Open in Satchel", [APP, "--full", zips[0]]))
    return entries


try:
    import gi

    gi.require_version("Nautilus", "4.0")
    from gi.repository import GObject, Nautilus
except (ImportError, ValueError):
    Nautilus = None

if Nautilus is not None:

    class SatchelMenuProvider(GObject.GObject, Nautilus.MenuProvider):
        def get_file_items(self, files: list) -> list:
            paths = [f.get_location().get_path() for f in files]
            if any(p is None for p in paths):
                return []
            items = []
            for entry in menu_entries(paths):
                item = Nautilus.MenuItem(name=entry.name, label=entry.label)
                item.connect("activate", lambda _item, command=entry.command: subprocess.Popen(command))
                items.append(item)
            return items
