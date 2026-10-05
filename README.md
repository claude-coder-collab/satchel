# Satchel

Lossless packaging of professional media into standard zip archives. PCM audio (WAV, RF64, AIFF,
CAF, Wave64) is compressed with FLAC and restored bit-exactly; everything else is deflated or
stored. Archives open in any unzip tool.

> Satchel is a working name. Product name, URLs and registered identifiers are placeholders
> (see [docs/IMPLEMENTATION.md](docs/IMPLEMENTATION.md)).

## Status

All steps of the [main spec](docs/spec/main-spec.md) are implemented; what remains needs
certificates, accounts or real devices (see [implementation status](docs/IMPLEMENTATION.md#5-status)).

| Component | Where |
|---|---|
| Core library and C API (`zp_*`) | `core/`, [zp.h](core/include/zp/zp.h) |
| Command-line tool `satchel` (create, extract, list, verify, edit, restore, preview) | `desktop/cli` |
| Desktop app (Qt 6, Simple and Full modes) | `desktop/gui` |
| Quick Look extension, Finder services (macOS) | `desktop/macos`, `desktop/gui/app/mac_services.mm` |
| Preview handler and Explorer commands (Windows) | `desktop/windows` |
| Browser app (WebAssembly, OPFS) | `apps/web`, [live](https://claude-coder-collab.github.io/satchel/) |
| Python, PHP and JavaScript bindings | `bindings/` |
| Packages (deb, AppImage, dmg, MSI, wheels, npm) | `.github/workflows/release.yml` |

Packages are built unsigned until signing certificates exist.

## Documentation

- [Main specification](docs/spec/main-spec.md)
- [Zip module design](docs/spec/zip-module-design.md)
- [Desktop UI specification](docs/spec/desktop-ui-spec.md)
- [Implementation notes](docs/IMPLEMENTATION.md): decisions, deviations, status
- C API: [core/include/zp/zp.h](core/include/zp/zp.h)

## License

Copyright © 2026 Venn Audio Ltd.

Dual-licensed under the [GNU Affero General Public License v3.0](LICENSE) or a commercial license
(see [COMMERCIAL.md](COMMERCIAL.md)). Contributions require a CLA so that dual licensing remains
possible.
