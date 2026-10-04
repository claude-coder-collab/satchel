#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
# Copyright (c) 2026 Venn Audio Ltd.
"""Serve the browser app with the cross-origin isolation headers it needs."""

from __future__ import annotations

import argparse
import functools
import http.server
from pathlib import Path


class Handler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {**http.server.SimpleHTTPRequestHandler.extensions_map, ".mjs": "text/javascript", ".wasm": "application/wasm"}

    def __init__(self, *args, isolate: bool = True, **kwargs) -> None:
        self.isolate = isolate
        super().__init__(*args, **kwargs)

    def end_headers(self) -> None:
        if self.isolate:
            self.send_header("Cross-Origin-Opener-Policy", "same-origin")
            self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def log_message(self, format: str, *args) -> None:  # noqa: A002 - signature of the base class
        pass


def make_server(directory: Path, port: int, isolate: bool = True) -> http.server.ThreadingHTTPServer:
    handler = functools.partial(Handler, directory=str(directory), isolate=isolate)
    return http.server.ThreadingHTTPServer(("127.0.0.1", port), handler)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path, nargs="?", default=Path("build/wasm/web"))
    parser.add_argument("--port", type=int, default=8000)
    parser.add_argument("--no-isolation", action="store_true", help="omit COOP/COEP (to see the error page)")
    args = parser.parse_args()
    server = make_server(args.directory, args.port, not args.no_isolation)
    print(f"Serving {args.directory} on http://127.0.0.1:{args.port}/")
    server.serve_forever()


if __name__ == "__main__":
    main()
