# Contributing

Thanks for your interest in Satchel.

- **CLA.** Before a pull request from outside Venn Audio Ltd. can be merged, its author signs the
  [Contributor License Agreement](CLA.md) by commenting on the pull request as the CLA bot asks
  (once per person). The CLA lets the project stay dual-licensed (AGPL-3.0 or commercial).
- **Licenses of dependencies.** The core and bindings use permissive dependencies only (zlib, BSD,
  MIT, Apache-2.0). Never copy GPL, LGPL or AGPL code; formats implemented by GPL tools are
  implemented from their published specifications.
- **Headers.** Every source file starts with
  `SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial` and the copyright line.
- **Build and test.** See the [README](README.md). `clang-format` (the repository's
  `.clang-format`) and `clang-tidy` must pass; CI runs both.
- **Specification.** Behaviour changes update [docs/IMPLEMENTATION.md](docs/IMPLEMENTATION.md).
