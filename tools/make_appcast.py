#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
# Copyright (c) 2026 Venn Audio Ltd.
"""Writes a one-item Sparkle/WinSparkle appcast for a release package."""

from __future__ import annotations

import argparse
import re
from dataclasses import dataclass
from datetime import datetime, timezone
from email.utils import format_datetime
from pathlib import Path
from xml.sax.saxutils import quoteattr, escape

SPARKLE_NS = "http://www.andymatuschak.org/xml-namespaces/sparkle"


@dataclass(frozen=True)
class Signature:
    ed_signature: str
    length: int


def parse_sign_update(output: str) -> Signature:
    """Parses `sign_update` output: sparkle:edSignature="..." length="..."."""
    sig = re.search(r'sparkle:edSignature="([^"]+)"', output)
    length = re.search(r'length="(\d+)"', output)
    if not sig or not length:
        raise ValueError(f"unexpected sign_update output: {output!r}")
    return Signature(sig.group(1), int(length.group(1)))


def appcast(version: str, url: str, signature: Signature, platform: str, minimum_system_version: str | None = None,
            published: datetime | None = None) -> str:
    """The appcast XML for one release. `platform` is "macos" or "windows"."""
    if platform not in ("macos", "windows"):
        raise ValueError(f"unknown platform: {platform}")
    published = published or datetime.now(timezone.utc)
    attrs = [f"url={quoteattr(url)}", f'length="{signature.length}"', 'type="application/octet-stream"', f"sparkle:edSignature={quoteattr(signature.ed_signature)}"]
    if platform == "windows":
        attrs += ['sparkle:os="windows"', 'sparkle:installerArguments="/passive"']
    minimum = f"\n      <sparkle:minimumSystemVersion>{escape(minimum_system_version)}</sparkle:minimumSystemVersion>" if minimum_system_version else ""
    return f"""<?xml version="1.0" encoding="utf-8"?>
<rss version="2.0" xmlns:sparkle="{SPARKLE_NS}">
  <channel>
    <title>Satchel</title>
    <item>
      <title>Satchel {escape(version)}</title>
      <pubDate>{format_datetime(published)}</pubDate>
      <sparkle:version>{escape(version)}</sparkle:version>
      <sparkle:shortVersionString>{escape(version)}</sparkle:shortVersionString>{minimum}
      <enclosure {" ".join(attrs)} />
    </item>
  </channel>
</rss>
"""


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", required=True)
    parser.add_argument("--url", required=True, help="download URL of the package")
    parser.add_argument("--sign-output", required=True, help="output of Sparkle's sign_update for the package")
    parser.add_argument("--platform", choices=["macos", "windows"], required=True)
    parser.add_argument("--minimum-system-version")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    xml = appcast(args.version, args.url, parse_sign_update(args.sign_output), args.platform, args.minimum_system_version)
    args.out.write_text(xml, encoding="utf-8")


if __name__ == "__main__":
    main()
