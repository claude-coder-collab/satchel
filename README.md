# Satchel

Lossless packaging of professional media into standard zip archives. PCM audio (WAV, RF64, AIFF,
CAF, Wave64) is compressed with FLAC and restored bit-exactly; everything else is deflated or
stored. Archives open in any unzip tool.

> Satchel is a working name. Product name, URLs and registered identifiers are placeholders
> (see [docs/IMPLEMENTATION.md](docs/IMPLEMENTATION.md)).

## Status

Early development. The native zip layer (planning, collisions, symlink handling, path safety,
parallel build pipeline, reading, extraction, editing, C API) is in place. Deflate, FLAC, WASM,
bindings, CLI and GUI follow the order in the [main spec](docs/spec/main-spec.md), section 10.

## Building

Requirements: CMake ≥ 3.28, Ninja, and a C++26 compiler (Clang ≥ 18, GCC ≥ 14) or MSVC 2022.
Dependencies are downloaded at pinned versions during configuration.

```sh
cmake --preset clang
cmake --build build/clang --config Release
ctest --test-dir build/clang -C Release --output-on-failure
```

Other presets: `gcc`, `msvc`, `clang-asan` (AddressSanitizer + UBSan), `clang-tsan`.

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
