# SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
# Copyright (c) 2026 Venn Audio Ltd.
from __future__ import annotations

import sys
import xml.etree.ElementTree as ET
from datetime import datetime, timezone
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))

import make_appcast  # noqa: E402

NS = {"sparkle": make_appcast.SPARKLE_NS}


def test_parse_sign_update() -> None:
    sig = make_appcast.parse_sign_update('sparkle:edSignature="abc+/=" length="1234"\n')
    assert sig == make_appcast.Signature("abc+/=", 1234)
    with pytest.raises(ValueError):
        make_appcast.parse_sign_update("nope")


def test_macos_appcast() -> None:
    xml = make_appcast.appcast("1.2.0", "https://example.invalid/S&T.dmg", make_appcast.Signature("sig", 42), "macos", "13.3",
                               datetime(2026, 10, 4, tzinfo=timezone.utc))
    item = ET.fromstring(xml).find("channel/item")
    assert item is not None
    assert item.findtext("sparkle:version", namespaces=NS) == "1.2.0"
    assert item.findtext("sparkle:minimumSystemVersion", namespaces=NS) == "13.3"
    enclosure = item.find("enclosure")
    assert enclosure is not None
    assert enclosure.get("url") == "https://example.invalid/S&T.dmg"
    assert enclosure.get("length") == "42"
    assert enclosure.get(f"{{{make_appcast.SPARKLE_NS}}}edSignature") == "sig"
    assert enclosure.get(f"{{{make_appcast.SPARKLE_NS}}}os") is None


def test_windows_appcast() -> None:
    xml = make_appcast.appcast("1.2.0", "https://example.invalid/s.msi", make_appcast.Signature("sig", 7), "windows")
    enclosure = ET.fromstring(xml).find("channel/item/enclosure")
    assert enclosure is not None
    assert enclosure.get(f"{{{make_appcast.SPARKLE_NS}}}os") == "windows"
    with pytest.raises(ValueError):
        make_appcast.appcast("1", "u", make_appcast.Signature("s", 1), "linux")
