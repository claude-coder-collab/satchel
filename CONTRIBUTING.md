# Contributing

Thanks for your interest in Satchel.

- **License.** Contributions are accepted under the project's license, AGPL-3.0-only. There is no
  contributor agreement to sign.
- **Licenses of dependencies and copied code.** Anything we link or copy must be compatible with
  AGPL-3.0-only, and each new dependency gets a license check first. Prefer permissive licenses
  (zlib, BSD, MIT, Apache-2.0), especially in the core. Code under GPL-3.0, LGPL-3.0 or
  AGPL-3.0, or GPL-2.0-or-later, may be copied with its notices kept. Never copy GPL-2.0-only
  code, which the AGPL cannot include.
- **Headers.** Every source file starts with
  `SPDX-License-Identifier: AGPL-3.0-only` and the copyright line.
- **Build and test.** See the [README](README.md). `clang-format` (the repository's
  `.clang-format`) and `clang-tidy` must pass; CI runs both.
- **Test builds.** `python3 tools/qa_build.py [--label pr65]` makes an unsigned package for testers
  with the same Qt and settings as the release (it installs that Qt with `pip install aqtinstall`
  if needed). Packages built against Homebrew's Qt may not start on older macOS.
- **Specification.** Behaviour changes update [docs/IMPLEMENTATION.md](docs/IMPLEMENTATION.md).
