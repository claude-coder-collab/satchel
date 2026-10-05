# Satchel — Implementation Notes

This file is the living companion to the specifications in `docs/spec/`:

- `main-spec.md` — library specification (v4)
- `zip-module-design.md` — zip module component design
- `desktop-ui-spec.md` — desktop application UI

The specs say *what* to build. This file records every decision the specs leave open, every
deliberate deviation, the placeholders still in use, and the current status. Together they are
enough to re-implement the project.

## 1. Product identity (placeholders)

All provisional values live in `core/src/common/product.hpp` and must be replaced before the
first public release.

| Item | Placeholder | Notes |
|---|---|---|
| Product name | `Satchel` | Working name, used in the readme template, `ENCODER` tag, archive comment |
| `{DEARCHIVER_URL}` | `https://claude-coder-collab.github.io/satchel/` | The browser app, deployed by `.github/workflows/pages.yml`; its footer links the source (AGPL §13) |
| EOCD comment magic | `STCH` (0x53544348) | Big-endian in the binary record |
| FLAC APPLICATION ID | `Stch` | Must be registered with Xiph |
| Copyright holder | Venn Audio Ltd. | SPDX: `AGPL-3.0-only OR LicenseRef-Commercial` |
| App icon | `packaging/icons/*` | Drawn by `tools/make_icons.py` (PNG, ICO, ICNS; no dependencies) |
| macOS bundle ID | `com.vennaudio.satchel` | `ZP_BUNDLE_ID` in `cmake/Packaging.cmake` |
| WiX upgrade code | `C228FD8D-4C42-4523-90CC-05246E0FF58B` | Must never change once an MSI ships |
| Debian maintainer | `Venn Audio Ltd. <packages@example.invalid>` | `ZP_PACKAGE_CONTACT` |
| PyPI name | `satchel` | `bindings/python/pyproject.toml`; likely taken on PyPI |
| npm name | `@vennaudio/satchel` | `bindings/js/package.json.in` |
| Update feeds | `https://claude-coder-collab.github.io/satchel/appcast-{macos,windows}.xml` | `ZP_UPDATE_FEED_URL_MACOS/_WINDOWS`; nothing is published there yet |
| Update public key | `Q0RkfO0SbAmepgAaM3ogueEJAZ+bGAqXjl+FdIh9UFU=` | `ZP_UPDATE_PUBLIC_KEY` (EdDSA); its private key was discarded, so replace it with a real key pair (`generate_keys`) and store the private key as the `SPARKLE_PRIVATE_KEY` secret |

## 2. Repository layout

```
core/include/zp/zp.h     C API (the only public interface)
core/src/common/         status codes, byte helpers, product identity
core/src/io/             streams, file system, input sources
core/src/io/zip/         planner, builder, reader, extractor, editor, metadata, path policy
core/src/codecs/         codec registry, store, deflate
core/src/pipeline/       context, thread pools, memory budget
bench/                   zp_bench throughput benchmark
tests/unit, tests/integration, tests/support, tests/capi
cmake/Packaging.cmake    install layout, platform resources, CPack
packaging/               icons, .desktop, Info.plist, Windows resources, WiX patch, signing hook
tools/                   gen_cdef.py, make_icons.py, serve_web.py
docs/spec/               specifications (inputs)
```

Namespace `zp`. C API prefix `zp_` (as in the zip design doc).

## 3. Toolchain and dependencies

- C++26 on GCC/Clang, C++23 on MSVC (`/std:c++latest` is not yet a full C++26 mode); only
  features all three support are used (`std::expected`, `std::format`, `std::span`, ranges).
- CMake ≥ 3.28, Ninja Multi-Config, presets in `CMakePresets.json` (`clang`, `gcc`, `msvc`,
  `clang-asan`, `clang-tsan`).
- Dependencies are fetched with `FetchContent` at pinned versions with SHA-256 hashes
  (`cmake/Dependencies.cmake`):

| Dependency | Version | Notes |
|---|---|---|
| zlib-ng | 2.3.3 | native API (`ZLIB_COMPAT=OFF`), static |
| minizip-ng | 4.2.2 | `MZ_ZLIB=ON` against the fetched zlib-ng; every other backend, crypto, iconv off. There is no `MZ_ZIP64` option in 4.x: Zip64 is always available |
| utf8proc | 2.12.0 | NFC and case folding |
| libFLAC | 1.5.0 | `WITH_ASM=OFF` (no SIMD), `-ffp-contract=off`, no Ogg, no multithreading: encoder output is identical on every CPU |
| Catch2 | 3.16.0 | tests only |

## 4. Decisions and deviations

### 4.1 Archive comment (main spec 6.4)

minizip-ng stores the archive comment as a NUL-terminated string, and `unzip`/7-Zip print the
comment to users, so the binary record of spec 6.4 cannot be stored as-is. The comment is
printable ASCII:

```
Created with <app_version>. zpmeta:<base64 of the binary record>
```

The binary record is exactly the spec layout: `magic (4, big-endian) | schema (1) | u16le len +
app version | u16le len + readme name`. Readers search for the last `zpmeta:` marker, decode, and
require the magic; anything else is a foreign comment (`nullopt`).

### 4.2 Zip writing through minizip-ng

- Entries are written in minizip-ng *raw* mode: the core produces compressed bytes itself
  (parallel segments) and passes CRC and sizes to `mz_zip_entry_write_close`.
- Seekable output: no data descriptor; minizip-ng seeks back and patches CRC/sizes into the local
  header. Non-seekable output: general-purpose bit 3 is set and a data descriptor follows the data;
  the local header carries zero CRC/sizes. Directory entries also use a descriptor on
  non-seekable output (minizip-ng would otherwise seek to patch them).
- Zip64 per entry is decided up front (`Zip64Policy::needs_zip64_entry`): Zip64 is forced when the
  size hint is unknown, when it is within minizip-ng's 2 MiB local-header cushion of 4 GiB, or when
  the local header starts at or beyond 4 GiB; otherwise it is disabled. Size hints: store = file
  size; deflate = size + size/256 + 1 MiB.
- Version made by: Unix (3), 4.5. External attributes: `(S_IFREG|S_IFDIR | mode) << 16`, plus the
  MS-DOS directory bit for directories.
- Extended timestamp (0x5455) with the modification time only, identical in local and central
  headers. No NTFS or Unix1 fields. The DOS date/time field holds *local* time (as Info-ZIP does,
  so Windows Explorer shows the right time); archives are therefore byte-identical across machines
  only when they share a time zone. CI runs in UTC.
- Archive-level Zip64 (EOCD64) is handled by minizip-ng when there are ≥ 65,535 entries or the
  central directory starts beyond 4 GiB.
- Output writes are buffered (1 MiB) in `MinizipStreamAdapter`.

### 4.3 Reading

- The central directory is listed through minizip-ng (with a 64 KiB read-ahead buffer).
- Entry data is read by the core: the local header is parsed only to find where data starts, and
  reads of the shared input are serialized by a mutex so several entries can be decoded in
  parallel.
- Names: UTF-8 when bit 11 is set and the bytes are valid UTF-8; else the Info-ZIP Unicode Path
  extra field (0x7075) if its CRC matches; else CP437.
- Modification time: extended timestamp if present, else what minizip-ng derives (NTFS or DOS).
- Unix mode is reported only when the host system is Unix (3) or OS X (19) and the high 16 bits of
  the external attributes are non-zero. `S_IFLNK` marks a symlink entry.
- Encrypted entries (bit 0) are reported as unsupported.

### 4.4 Planning

- Input items are enumerated by `InputSource`. `FilesystemInputSource` turns each root into a
  top-level entry named after its last path component and walks directories recursively, sorting
  children by their UTF-8 names (byte order) so output is deterministic. `lstat` on POSIX;
  `GetFileAttributesExW` on Windows, where any reparse point (symlink, junction) is a symlink.
  Non-regular files (devices, FIFOs) are ignored.
- Conflict kinds (extension of the design doc's `Conflict`):
  - `Collision`: equal `collision_key` (NFC + case fold), or a file name that another entry uses as
    a folder (`a` and `a/b`).
  - `InvalidName`: the normalized name fails `validate_for_archive` (invalid UTF-8, absolute, drive
    letter, empty/`.`/`..` segments, backslash, > 65,535 bytes).
- `replan` applies all resolutions at once; indices refer to the plan passed in; skipped entries are
  removed. A rename to an invalid name fails with `INVALID_NAME`; a rename whose entry still sits in
  a conflict afterwards fails with `NAME_COLLISION`. The generated readme can never be the target
  of a resolution.

### 4.5 Building and the pipeline

- One `Context` per library context: N worker threads (N = `threads`, 0 = hardware concurrency)
  plus three service threads (reader, SHA-256 hasher, MD5 hasher). The writer runs on the calling
  thread. Jobs in one context are serialized by a mutex. (WASM pool size: N + 4.)
- The reader cuts each file into fixed segments (store 4 MiB, deflate 1 MiB with the previous
  32 KiB as dictionary); workers compute the segment CRC and encode; the writer reorders by
  sequence number, writes, and combines CRCs with `crc32_combine`.
- Memory budget: the reader acquires `segment size` (store) or `2 × segment size + 1 KiB`
  (deflate) bytes before reading a segment; the writer releases them after writing. A single
  request larger than the budget is granted when nothing else is held. `BuildResult` reports the
  peak.
- Method choice for `GENERAL` entries (main spec 6.1):
  - empty files are stored;
  - entries up to `small_entry_threshold` (4 MiB) are one segment: the worker deflates the whole
    entry and keeps the result only if it saves at least 2 % (`min_deflate_saving`), otherwise the
    entry is stored;
  - larger entries: the reader deflates the first 128 KiB (`sample_window`) at the plan's level and
    picks deflate (1 MiB segments) or store (4 MiB segments) for the whole entry by the same 2 %
    rule.
  This is `small_entry_threshold`'s meaning here; the design doc gives only its default.
- Deflate output is pinned by the zlib-ng version and settings (raw deflate, window 15, memLevel 8,
  default strategy). A golden test checks the CRC-32 of the compressed bytes of a fixed input at
  levels 1, 5 and 9 on every CI platform.
- `SOURCE_CHANGED`: a size/mtime mismatch before reading leaves that entry out and records
  `SOURCE_CHANGED` in its result; the archive is still completed and the build status is
  `SOURCE_CHANGED`. A file that changes *while* it is read (short read or extra data) fails the
  whole build with `SOURCE_CHANGED`.
- Cancellation (progress callback returns false/non-zero) stops the reader, waits for in-flight
  work, and abandons the archive (the minizip state is released without further writes).
  "No partial archive" on native targets comes from `AtomicFileStream` (`zp_stream_create_file`):
  the temporary file is removed unless `zp_stream_commit` is called.

### 4.6 Extraction

- `ExtractionPlan` contains items (entry, sanitized target, decision) and issues. Errors refuse
  entries; everything else is still extracted. `NAME_COLLISION`: all colliding files are refused;
  duplicate *directories* are merged silently; an entry below a path that is a file elsewhere in the
  archive is refused. `EXISTS_AT_DESTINATION` with `ASK` leaves the item `Undecided` and
  `execute` returns `DECISION_REQUIRED` until `decide(item, Skip|Replace)` is called.
- `sanitize_for_extraction` treats `\` as a separator, drops empty and `.` segments, and refuses
  `..`, absolute paths, drive letters, UNC paths and any `:` (Windows alternate data streams).
- The readme recorded in the archive metadata is skipped unless `include_readme` is set or it is
  selected explicitly.
- Files are extracted in parallel (one task per entry). `FilesystemOutputSink` writes to a
  temporary file and renames it into place on commit (no fsync), so a CRC failure, cancellation
  or a replaced file never leaves partial output. It refuses to write through a symbolic link that
  already exists below the destination. Directory times and modes are applied last, deepest first;
  directories always keep owner `rwx` so extraction can continue.
- The extraction result status is the first per-entry error, else `UNSAFE_PATH`/`NAME_COLLISION`
  if the plan refused entries, else `OK`.

### 4.7 Editing

- `ArchiveEditor` indices refer to the current `plan()` order (initially archive order, then
  additions appended).
- Kept entries are copied raw with their CRC, sizes, method, mtime and mode; names are
  re-normalized (third-party names are decoded and re-stored as UTF-8 with bit 11).
- Symlink entries in the source archive are dropped with a `SYMLINK_SKIPPED` warning.
- Archives containing encrypted entries or entries with methods other than store/deflate cannot be
  edited (`UNSUPPORTED_METHOD`): minizip-ng only writes raw entries for methods it was built with.
- In-place native edits: the caller writes to `AtomicFileStream` on the original path (temp file
  in the same directory, fsync, `rename`/`ReplaceFileW`, directory fsync). The input is opened with
  `FILE_SHARE_DELETE` on Windows so the replace can happen while it is open.

### 4.8 FLAC (main spec 7.2)

**Detection (plan time).** Every file of at least 12 bytes is opened and its first bytes checked
for RIFF/WAVE, RF64/WAVE, BW64/WAVE, FORM/AIFF, FORM/AIFC, `caff` v1 and the Wave64 `riff`/`wave`
GUIDs. The extension does not matter. `scan_pcm` walks the chunk headers with seeks and splits the
file into:

    blocks before the audio header | audio header | samples | zero padding | blocks after | trailing

The blocks must tile the file exactly, or the file falls back to store. Rules per container:

| Container | First block | Audio header block | Audio padding |
|---|---|---|---|
| WAV, RF64, BW64 | 12 bytes (`RIFF`/`RF64` + size + `WAVE`) | 8 bytes (`data` + size) | to 2 bytes |
| AIFF, AIFF-C | 12 bytes (`FORM` + size + type) | 16 bytes (`SSND` + size + offset + blockSize) | to 2 bytes |
| Wave64 | 40 bytes (riff GUID + size + wave GUID) | 24 bytes (data GUID + size) | to 8 bytes |
| CAF | 8 bytes (file header) | 16 bytes (`data` + size + edit count) | none |

Other chunks are one block each, *including* their pad byte. These rules match what
`flac --keep-foreign-metadata` 1.5.0 writes. They were found by running the tool on test files,
not by reading its GPL code. The tool's documentation says the main chunk header and the form
type are separate blocks; the tool actually writes them as one 12-byte block.

**Fallback (store, keep name, `FLAC_FALLBACK` warning).** These cases fall back:

- float samples; non-PCM WAVE formats; unknown EXTENSIBLE subformats;
- AIFF-C compression other than `NONE`/`twos`/`sowt`;
- CAF other than integer `lpcm` with one frame per packet;
- bits outside 4–32 or containers wider than 4 bytes;
- a sample rate of 0, above 1,048,575 Hz, or not a whole number;
- 2^36 frames or more;
- a block or trailing data larger than 16,777,211 bytes;
- any structural problem: missing or duplicate fmt/COMM/desc/data, data before the format chunk,
  sizes past the container or file end, placeholder sizes, a partial last frame, non-zero padding
  after the audio, a non-zero SSND offset, an SSND size that disagrees with COMM.

Fallback entries are always stored, not deflated (zip design 6).

**Encoding.**
- FLAC bits per sample is always 8 × the container byte width, never the "valid bits", so any
  low-bit content survives. Zero low bits cost almost nothing thanks to FLAC's wasted-bits
  detection. 8-bit WAV/Wave64 samples are unsigned and are converted to signed (XOR 0x80).
- The fixed block size is 4096 for every level, so STREAMINFO min/max block size = 4096. Levels
  map to libFLAC presets. The encoder tries streamable-subset mode first and drops it when libFLAC
  refuses the settings (for example rates above 655,350 Hz).
- Segments: 64 blocks × 4096 frames. Each one is encoded by a fresh libFLAC encoder; its metadata
  writes are dropped and its frames renumbered (CRC-8 and CRC-16 recomputed). The output is
  byte-identical to one sequential encoder at every level 0–8 (tested).
- Block order: `fLaC`, STREAMINFO, VORBIS_COMMENT (vendor = libFLAC's vendor string), standard
  foreign blocks (layout 0 only), project block (last). No padding or seek table.
- FLAC entries are stored in the zip (method 0) with a Zip64 size hint of
  header + frames × channels × bytes + 34 bytes per block.

**Hashes and patching.**
- `HashJob` computes the SHA-256 of the whole source file and the FLAC MD5 of the samples on two
  service threads (lanes) that read the same queue of chunks; a chunk is dropped once both lanes
  have consumed it, and the producer blocks while either lane is 16 chunks (1 MiB each, outside
  the memory budget) behind. The MD5 input is signed little-endian samples of the container width.
  For multi-mono there is one MD5 per channel.
- SHA-256 uses the CPU's SHA instructions when available (ARMv8 SHA2 when the compiler targets it,
  e.g. Apple silicon; x86-64 SHA-NI detected with CPUID at run time) and the portable code
  otherwise (WASM, older CPUs). `Sha256::set_hardware_enabled(false)` forces the portable path
  (tests compare both). MD5 is fully unrolled (compile-time round constants).
- Seekable output: the header is written with zeroed MD5, frame sizes and SHA-256. At the end the
  writer patches the header in place through the adapter. The entry CRC is
  `crc32_combine(crc(final header), crc(frames), len(frames))`.
- Non-seekable output (multichannel only): two passes. The first reads the whole file for the
  hashes, then the header is written complete. STREAMINFO min/max frame size is 0 ("unknown") in
  this case, the only difference from seekable output.
- Multi-mono members are written to spill files in `BuilderOptions.temp_dir` (default: the system
  temp directory), patched there, then copied into the archive in channel order. They never need
  two passes. Spill files are removed on success, failure and cancel.

**Project block** (all integers big-endian): ID (4) | schema 1 | layout | group ID (16) |
channel index | channel count | u16 name length + name | u32 length + private data | u32 length +
trailing bytes | SHA-256 (32).

- The original name is the file name only (no folders). If the user renamed the FLAC entry, it is
  the new stem plus the original extension.
- Private storage (layout 1 in `_ch01` only, and layout 2): the records encoded as FLAC APPLICATION
  metadata blocks (4-byte header + 4-byte ID + chunk bytes, ID `riff`/`aiff`/`w64 `/`caff`),
  concatenated. It is stored as u32 BE uncompressed length + raw deflate (level 9).
- Trailing bytes go in the project block of the multichannel file or of `_ch01`.
- Group ID: the first 16 bytes of SHA-256("satchel multi-mono group\n" + stem + size + mtime), with
  RFC 9562 version-8/variant bits. The spec says "random"; deriving it keeps archives
  reproducible.

**Mirrored tags.**
- The spec table is implemented from `bext`, `iXML` (simple element search with XML entity
  decoding), `LIST/INFO` and AIFF `NAME`/`AUTH`/`ANNO`.
- Non-UTF-8 text is read as Latin-1.
- `ENCODER` = "Satchel <version>". Multi-mono members add `CHANNEL`, `CHANNELS` and `TRACK_NAME`.

**Names and collisions.**
- `take1.WAV` → `take1.flac`. More than 8 channels → `take1_chNN.flac` (at least 2 digits).
- A converted entry also claims its *restored* name in the collision check, so archives never
  collide on extraction either.
- A multi-mono group is one unit: renaming any member renames all of them (new stem +
  `_chNN.flac`), and skip or "store unconverted" applies to the whole group.
- The readme cannot be the target of a resolution.

**Readme.**
- Template placeholders: `{APP_NAME}`, `{APP_VERSION}`, `{DEARCHIVER_URL}`, `{FILE_LIST}`. The
  default template is the spec text with `{FILE_LIST}` after "Files that were converted:".
- Rows show archive paths, aligned. Output is CRLF and UTF-8 without BOM, deflated like other
  files.
- Its mtime is the newest input file's. Files skipped with `SOURCE_CHANGED` are left out.
- The editor drops the old readme (named in the archive metadata) and builds a new one when any
  converted entry, new or kept, remains.

**Extraction and restore.**
- `.flac` entries are probed by reading their metadata only (`ArchiveReader::flac_header`, cached).
  Probing is lazy: listing stays central-directory-only.
- A FLAC entry with our project block is restorable. The target is the entry's folder plus the
  stored original name, which must be a plain file name.
- Multi-mono: selecting any member selects the group. Members must share group ID, channel count
  and SHA-256, have distinct indices 1..N, and match in sample count and bits per sample;
  otherwise every member gets `INCOMPLETE_GROUP`.
- Restore re-runs `scan_pcm` on a virtual file made of the records, zeros for the audio and the
  trailing bytes, which recovers the container layout and its padding. It then writes records,
  decoded samples, padding and trailing bytes while hashing.
- Checks: zip CRC-32 of every member (checked as the stream is consumed), sample count, and
  SHA-256 of the restored file. libFLAC's MD5 check is enabled only when there is no project block
  (no SHA-256 to compare); otherwise it would repeat what SHA-256 already proves. Any failure
  deletes the partial output.
- `flac::restore` also rebuilds FLAC files made by `flac --keep-foreign-metadata` (no project
  block, so no SHA-256). The extractor does not restore those automatically, because they do not
  carry our ID (spec).

### 4.9 WebAssembly

- Preset `wasm` (configure with `emcmake cmake --preset wasm`; Emscripten 6.0.10 in CI). All code is
  built with `-pthread`; linking uses `ALLOW_MEMORY_GROWTH` and a 1 MiB stack.
- `bindings/js` builds `satchel.mjs` + `satchel.wasm`: an ES module (`createSatchel()`) that
  exports every `ZP_API` function of `zp.h` (the export list is generated from the header) plus
  `ccall`, `cwrap`, `addFunction` and heap helpers. Maximum memory 512 MB
  (`ZP_WASM_MAXIMUM_MEMORY`), pthread pool `min(hardwareConcurrency, 16) + 4` (N workers + reader,
  two hashers and the caller), growable if more threads are needed.
- The module blocks while a job runs, so in a browser it must run inside a Web Worker (main spec 3);
  under Node it can run on the main thread.
- The Catch2 suite is also built for WASM and runs under Node (`-sPROXY_TO_PTHREAD`, `NODERAWFS`);
  `bindings/js/test/smoke.mjs` exercises the module through the C API.
- `ZP_API` in `zp.h` marks exports: `used` + default visibility under Emscripten and GCC/Clang,
  `dllexport`/`dllimport` for a Windows DLL (`ZP_SHARED_BUILD` / `ZP_SHARED`).

### 4.10 Browser (main spec 3, 4)

**C API additions** (useful to every binding, required by the browser):
- `zp_input_from_callbacks`: items described by the caller and read through a callback.
- `zp_sink_from_callbacks`: an extraction destination implemented by the caller.
- `zp_*_describe`: JSON descriptions of a plan, build result, archive listing or extraction plan,
  freed with `zp_free`.

**Threading.** Under Emscripten pthreads, every JavaScript callback (stream, input, sink) is
proxied to the main runtime thread with `emscripten_proxy_sync` (`core/src/common/main_thread.*`).
That is the Web Worker that owns the `File` objects and OPFS handles. It serves proxied calls
while it blocks in `zp_build`/`zp_extract`, because the builder's writer runs on the calling
thread and Emscripten processes the proxy queue during blocking waits. Native builds call the
callbacks directly.

**Module** (`bindings/js`):
- Built with WasmFS (`-sWASMFS -sFORCE_FILESYSTEM`) and native WebAssembly exceptions
  (`-fwasm-exceptions`, used everywhere, including tests).
- `zp_wasm_mount_opfs(path)` (in `wasm_entry.c`) mounts the Origin Private File System through
  WasmFS's OPFS backend. The core's ordinary file code then writes OPFS directly: atomic archive
  output, filesystem extraction sink and multi-mono spill files.

**JavaScript binding** (`bindings/js/src/satchel.mjs`, synchronous, worker-side):
- `Satchel.load()`, `plan(items)` (items are `{path, data|file, lastModified}` or
  `{path, directory: true}`).
- `Plan`: `describe`, `resolve`, `build(writer)`, `buildToPath`, `buildBytes`.
- `Archive`: `describe`, `extract(sinkCallbacks)`, `extractToPath`, `extractToMemory`, `verify`.
- Blobs are read with `FileReaderSync`. Writers: `MemoryWriter`, `SyncHandleWriter`.
- Tests: `test/smoke.mjs` and `test/glue.mjs` under Node. They cover parallel extraction calling
  back into JavaScript from pthreads.

**App** (`apps/web`, assembled into `build/wasm/web` by the `satchel_web` target):
- `worker.mjs` owns the module and mounts OPFS at `/opfs`. Archives are built to OPFS and the
  page offers them through `showSaveFilePicker` (stream copy) or a download link. Extraction goes
  to an OPFS folder, then to a folder picked with `showDirectoryPicker` or per-file downloads.
- The page plans, shows conflicts with Rename / Skip / Store unconverted, lists warnings, builds
  with progress, opens archives, verifies and extracts.
- If OPFS sync access handles are unusable (Playwright's WebKit, some private modes), the worker
  falls back to WasmFS memory and returns Blobs. The footer says "in-memory mode".
- Cross-origin isolation: `coi-serviceworker.js` (MIT, gzuidhof/coi-serviceworker v0.1.7) adds
  COOP/COEP on hosts that cannot send headers, such as GitHub Pages. Without isolation the page
  shows a specific error (tested).
- `tools/serve_web.py` serves the app locally with the headers (`--no-isolation` to test the
  error).

**Deviations and open points.**
- Spec 4 wants Chromium's File System Access output written directly. Synchronous writes to a
  picker file are not possible, so output is staged in OPFS and then copied; the browser needs
  free space for both copies until the staging file is removed.
- The iOS Safari / Android Chrome multi-GB spike still needs real devices.

**Tests:** `tests/web/test_web.py` (Playwright). CI runs Chromium on every change; the weekly Full
workflow runs Chromium, Firefox and WebKit. Firefox could not be launched in the macOS VM used for
development.

### 4.11 Bindings (main spec 5)

- `zp_shared` builds `libsatchel` (`.so`/`.dylib`/`satchel.dll`) from the whole core archive. Only
  `zp_*` symbols are exported (version script on Linux, `-exported_symbol` on macOS, `dllexport`
  via `ZP_SHARED_BUILD` on Windows).
- `tools/gen_cdef.py` turns `zp.h` into plain C declarations (no preprocessor) for cffi and PHP
  FFI: `bindings/python/satchel/_cdef.h` and `bindings/php/src/zp_cdef.h`. CI checks they are
  current.
- Python (`bindings/python`, package `satchel`, cffi ABI mode): `Context`, `Plan` (entries,
  conflicts, warnings, `resolve`, `build` to a file atomically or `build_bytes`), `Archive`
  (entries, `extraction_plan`, `extract`, `verify`), `ExtractionPlan` (issues, items, `decide`),
  `Editor` (`add`, `remove`, `rename`, `replace`, `commit` in place). Errors raise `SatchelError`
  with `status`/`name`. Progress callbacks return `False` to cancel; exceptions raised in a
  callback cancel the job and are re-raised.
- PHP (`bindings/php/src/Satchel.php`, namespace `Satchel`, PHP ≥ 8.1, `ffi.enable=1`): the same
  objects with PHP arrays for results; errors throw `SatchelException`.
- The library is found through `SATCHEL_LIBRARY`, next to the package, or in `build/*/core/*`.
- `zp_last_status()` was added to the C API so bindings can report the status after a NULL
  return.
- Tests: `pytest` in `bindings/python`, `php tests/run.php` in `bindings/php`. Python and PHP
  produce byte-identical archives for the same input (checked when PHP is installed).
- Wheels: `tools/build_wheel.py --library <lib> --platform-tag <tag>` copies the package, the
  library and `LICENSE` into a temporary tree, builds a pure wheel with pip and retags it
  `py3-none-<tag>` (cffi ABI mode needs no per-Python build). The library is built with
  `ZP_PORTABLE_RUNTIME=ON` (static libstdc++/libgcc on Linux, static CRT on MSVC). Linux wheels
  are built in `manylinux_2_28` and checked/retagged by `auditwheel repair`; macOS wheels target
  14.0, universal (`macosx_14_0_universal2`); Windows `win_amd64`. Each wheel is installed in a fresh venv and
  the binding tests run against it (Python 3.10 and 3.13 on Linux).
- npm: target `satchel_npm` (WASM build) assembles `build/wasm/npm`: `package.json` from
  `bindings/js/package.json.in`, the binding as `index.mjs`, the Emscripten `satchel.mjs` +
  `satchel.wasm`, README and LICENSE. `bindings/js/test/package.mjs` checks an installed tarball
  (build and extract in Node).
- Nothing is published to PyPI or npm yet: wheels and the npm tarball are attached to the GitHub
  release.

### 4.12 Command-line tool (main spec 12.1)

`desktop/cli` builds `satchel` (CLI11 2.7.2, nlohmann/json 3.12.0). It uses only the C API, via
the header-only wrapper `desktop/common/zp_cpp.hpp` (RAII handles, typed errors, list readers),
which the GUI will reuse.

| Command | Purpose |
|---|---|
| `create ARCHIVE PATH...` | Plan and build. `ARCHIVE` `-` writes a streamed zip to stdout. `--no-flac`, `--deflate-level`, `--flac-level`, `--rename NAME=NEW`, `--skip NAME`, `--store-unconverted NAME`, `--resolutions FILE.json`, `--dry-run`, `--readme-template FILE`, `--temp-dir DIR` |
| `extract ARCHIVE [ENTRY...]` | `-d DIR` (default `.`), `--keep-flac`, `--include-readme`, `--overwrite ask\|skip\|replace` |
| `list ARCHIVE` | Table or JSON: name, sizes, method (store/deflate/flac/flac-mono), restores-to, channel, mtime, creator |
| `preview FILE [--html]` | `zp_preview` summary of a zip or FLAC file as JSON or HTML (exit 1 if neither) |
| `verify ARCHIVE` | Extraction to the null sink: CRC-32 plus full restore and SHA-256 for FLAC; nothing is written |
| `edit ARCHIVE` | `--add PATH`, `--remove NAME`, `--rename NAME=NEW`, `--replace NAME=PATH`, `-o OUT` (default: replace in place atomically) |
| `restore FILE.flac` | Rebuild the original from a standalone FLAC (ours or `flac --keep-foreign-metadata`), `-o OUT` |

- Every command takes `--json` (machine-readable result on stdout), `-q/--quiet`,
  `-y/--yes`, `--threads` and `--memory` (MB).
- A resolution `NAME` can be a planned output name, the original (restored) name, the source
  path or `#index`. The JSON file holds `[{"entry": NAME|INDEX, "action":
  rename|skip|store-unconverted, "new_name": ...}]`.
- Skipped symlinks: an interactive run asks; a non-interactive run without `--yes` stops with
  exit 4. FLAC fallbacks are printed as warnings and never stop the run.
- Existing files on extraction with `--overwrite ask`: asked per file when interactive, else exit 4.
- The progress bar goes to stderr only on a terminal. Ctrl+C cancels (exit 130); the archive's
  temporary file is removed.
- Exit codes: 0 ok, 1 error, 2 usage, 3 unresolved conflicts, 4 declined, 5 finished with errors
  (`SOURCE_CHANGED`, refused or failed entries), 130 cancelled.
- C API additions for the CLI: `zp_restore_flac`, `zp_flac_original_name`,
  `zp_entry_info_t.flac_original_name`.
- Tests: `tests/cli/test_cli.py` (pytest; `SATCHEL_CLI` points to the binary). It runs on Linux and
  Windows in CI.

### 4.13 Desktop app (desktop UI spec)

`desktop/gui` (Qt 6 Widgets ≥ 6.4, found with `find_package`, built when available).

**Logic** (`desktop/gui/logic`, no Qt, Catch2-tested): Simple-mode drop rule, output naming with
" 2"/" 3" suffixes (at the end for folders), multi-mono row grouping, ratio, timecode formatting
(HH:MM:SS:FF from the iXML rate incl. 29.97/59.94 drop frame, else HH:MM:SS.mmm), Settings
defaults and "Copy as CLI command".

**App** (`desktop/gui/app`, library `satchel_gui_app` plus `Satchel` executable):
- `App` owns one `JobQueue` (a single background thread running jobs in order: compress,
  extract, verify, edit) shared by both modes, the Settings (QSettings) and both windows. The
  last mode is remembered.
- Simple mode (`SimpleWindow`): drop or files on the command line/Dock icon. Zips are extracted
  to `<name>/` next to them; anything else is compressed to `<item>.zip` or `<parent>.zip` next to
  the items, never overwriting. Collisions open a compact sheet with suffixed names. The window
  shows idle, running (progress, throughput, ETA, Cancel) and result card (savings, warnings,
  Reveal, Retry, Open in Full mode). A desktop notification is sent when unfocused.
- Full mode (`MainWindow`):
  - Toolbar with the spec's shortcuts.
  - Archive browser: `ArchiveModel` over the central directory, flat or folder tree, multi-mono
    groups as one expandable row, Verified column, hideable columns. `ArchiveFilter` provides
    search, a method filter and restorable-only. Rows stay in archive order until a column is
    clicked: sorting 100k rows up front cost about 1.3 s.
  - Inspector (HTML) uses `zp_reader_flac_describe` for audio, original, production metadata,
    tracks and the chunk list.
  - Status-bar summary and a Jobs dock (cancel, reveal).
  - Every build goes through `PlanReviewDialog`: entries with method and reason, totals, conflict
    controls (Rename / Skip / Store unconverted), warnings, collapsed options, Copy as CLI command.
  - Extraction: `ExtractDialog` options, then a pre-flight showing issues and existing files.
  - Verify writes nothing and fills the Verified column.
  - Edits (add, delete, rename — renaming a group renames every member) are editor jobs that
    rewrite the archive in place. Double-click opens an entry through a temporary extraction;
    dragging out extracts to a temporary folder first.
- `--smoke-test ARCHIVE` prints the time to list an archive. 100,000 entries list in about
  0.45–0.6 s on the development Mac.
- Tests: `satchel_gui_logic_tests` (Catch2) and `satchel_gui_tests` (Qt Test, offscreen): both
  modes launch, a 100k-entry archive opens (< 3 s allowed on CI), and Simple mode compresses and
  extracts a drop end to end.
- C API addition: `zp_reader_flac_describe`.

**Not done yet (desktop):**
- Windows 11 top-level context-menu entries (sparse MSIX package; needs a signing certificate).
- An optional Linux previewer plugin (Nautilus/Dolphin).
- Translations beyond English. A few display strings come from the Qt-free logic library
  (`savings_text`, multi-mono group labels) and are not translatable yet.
- The per-release manual pass.

**Translations.** `desktop/gui/translations/satchel_en.ts` holds every `tr()` string (English
source; generated with `-no-obsolete -locations none` so it only changes when strings do; refresh
with `cmake --build <dir> --target satchel_lupdate`). `lrelease` output is embedded in
`satchel_gui_app` under `:/i18n` (`ZP_HAS_TRANSLATIONS` when Qt Linguist tools are found; optional).
At startup the app installs Qt's own `qtbase_<locale>` and `:/i18n/satchel_<locale>` translators
for the system locale. Adding a language = adding `satchel_<lang>.ts` to `ts_files`.

**Updates.** `desktop/gui/app/updater.hpp`: `Updater::create(automatic)` returns Sparkle 2 on macOS
(`updater_mac.mm` loads `Contents/Frameworks/Sparkle.framework` with `NSBundle` and drives
`SPUStandardUpdaterController` through a locally declared protocol, so nothing links Sparkle) and
WinSparkle on Windows (`updater_win.cpp`: `LoadLibraryEx("WinSparkle.dll")` from the app folder,
feed URL and EdDSA key set before `win_sparkle_init`); otherwise, or when the library is missing
(development builds, Linux), a no-op updater whose `available()` is false. `App::start_updates()`
runs from `main` only, so tests never start it. The "Check automatically" setting is applied on
every save; Full mode gets "Help › Check for Updates…" (the application menu on macOS) when an
updater is available. Sparkle reads `SUFeedURL` and `SUPublicEDKey` from `Info.plist`.
`ZP_BUNDLE_UPDATER=ON` (release builds) fetches Sparkle 2.10.0 / WinSparkle 0.9.4 (SHA-256 pinned)
and installs them with the app; `sign.cmake` signs Sparkle's XPC services, Autoupdate and
Updater.app before the framework. `tools/make_appcast.py` writes one-item appcasts (tested); the
release `appcast` job signs the dmg and MSI with Sparkle's `sign_update` and attaches
`appcast-macos.xml` / `appcast-windows.xml` only when the `SPARKLE_PRIVATE_KEY` secret exists.
Publishing the appcasts to the feed URLs is a manual release step for now.

### 4.14 C API

`core/include/zp/zp.h`. Additions beyond the design doc's list: stream constructors (file, atomic
file + commit, memory, callbacks), input constructors (paths, memory), option initializers,
`zp_plan_executable`, `zp_plan_total_bytes`, build result accessors, extraction issue/item/outcome
accessors and `zp_xplan_decide`, `zp_sink_filesystem`/`zp_sink_null`, `zp_status_name`,
`zp_version`, `zp_preview`. The progress callback returns non-zero to cancel. `zp_last_error` is thread-local.

### 4.15 Packaging and releases (main spec 12.3)

`cmake/Packaging.cmake` (included from the root) defines the install layout and CPack:

| Platform | Install layout | Package |
|---|---|---|
| Linux | `bin/satchel`, `bin/satchel-gui`, `share/applications/satchel.desktop`, hicolor icons 256/512 | CPack `DEB` (`satchel_<ver>_amd64.deb`, `SHLIBDEPS` against system Qt) and `TGZ`; AppImage by `linuxdeploy` + `linuxdeploy-plugin-qt` from the installed tree |
| macOS | `Satchel.app` (executable `Satchel`, `Info.plist` from `packaging/macos/Info.plist.in`, `satchel.icns`); CLI at `Satchel.app/Contents/Helpers/satchel` (the bundle's `MacOS/Satchel` would clash with `satchel` on a case-insensitive disk) | CPack `DragNDrop` (UDZO dmg with an Applications link) |
| Windows | `bin/satchel.exe`, `bin/satchel-gui.exe` (icon + version resource from `packaging/windows/satchel.rc.in`), Qt DLLs | CPack `WIX` (WiX 5 via `CPACK_WIX_VERSION 4`): Start-menu shortcut "Satchel", `bin` appended to the system `PATH` (`packaging/windows/wix_patch.xml`) |

- Windows: the MSVC runtime is installed app-local in `bin` (`InstallRequiredSystemLibraries`,
  UCRT excluded as it ships with Windows 10+), so the app, CLI and `satchel_shell.dll` (loaded by
  `prevhost.exe`, whose COM loader searches the DLL's folder) need no redistributable; windeployqt
  runs with `--no-compiler-runtime`.
- Qt is deployed at install time with `qt_generate_deploy_app_script` (macdeployqt/windeployqt);
  option `ZP_DEPLOY_QT`, on by default for macOS and Windows, off for Linux (the `.deb` uses the
  distribution's Qt; the AppImage bundles it).
- `packaging/sign.cmake` is CPack's pre-build script: it signs the staged files when
  `SATCHEL_MACOS_SIGN_IDENTITY` (codesign, hardened runtime, timestamp) or `SATCHEL_WINDOWS_CERT` +
  `SATCHEL_WINDOWS_CERT_PASSWORD` (signtool, SHA-256, RFC 3161 timestamp) are set, else does nothing.
- `.github/workflows/release.yml` runs on `v*` tags, on demand, and on pull requests touching
  packaging. The version is the tag without `v` (else the CMake project version) and goes into
  `ZP_VERSION`. Jobs: Linux (deb, tar.gz, AppImage; installs the deb and starts the AppImage under
  Xvfb), macOS (Qt 6.8.3 from `install-qt-action`, universal arm64 + x86_64, deployment target 13.3 (libc++ floating-point `to_chars`), dmg; notarized and
  stapled when Apple credentials exist; checks the bundle links nothing outside itself and the
  system), Windows (MSI; installs it silently and runs `satchel --version`), web (zip of the browser
  app, npm tarball), Python wheels (Linux, macOS, Windows). `publish` writes `SHA256SUMS` and, for tags, creates a draft GitHub release.
- Secrets (all optional; unsigned packages are built without them): `MACOS_CERTIFICATE` (base64
  .p12), `MACOS_CERTIFICATE_PASSWORD`, `MACOS_SIGN_IDENTITY`, `APPLE_ID`, `APPLE_TEAM_ID`,
  `APPLE_APP_PASSWORD`, `WINDOWS_CERTIFICATE` (base64 .pfx), `WINDOWS_CERTIFICATE_PASSWORD`.

### 4.16 Performance (zip design doc 8.6)

`bench/bench.cpp` builds `zp_bench` (option `ZP_BUILD_BENCH`, on by default; also for WASM, run
with `node zp_bench.js`). It generates 24-bit stereo audio (tones + noise), word-salad text and
random bytes (`--size` MiB each), hashes the audio, then builds and extracts (to the null sink)
each case with 1 and all hardware threads, or only `--threads N`; `--case TEXT` runs the cases
whose name contains TEXT (cases: audio FLAC 5/0, 16-ch multi-mono, audio deflate 6, text deflate
6/1, random). `--json` prints machine-readable results. CI runs it with `--size 32` on Linux (GCC) and Windows as a smoke test.

Results with 256 MiB inputs (MiB/s; build = input bytes per second; Apple M-series, 8 threads):

| Case | Build, 1 thread | Build, 8 threads | Extract | Size |
|---|---|---|---|---|
| audio, FLAC 5 | 127 | 370–496 | 250 (1 thread), 980 (8) | 59.7% |
| audio, FLAC 0 | 152 | 430–521 | 260 (1 thread), 970 (8) | 74.2% |
| text, deflate 6 | 54 | 191–308 | 1040–1170 | 18.2% |
| text, deflate 1 | 294–333 | 1420–1800 | 720 | 36.1% |
| random (stored) | 1460–2030 | 2480–2710 | 5800–6900 | 100% |

SHA-256 1.1–1.3 GiB/s with the CPU instructions, MD5 630 MiB/s. Before this work the hasher ran
portable SHA-256 (207 MiB/s) and MD5 (236 MiB/s) back to back on one thread, which capped FLAC
builds at about 106 MiB/s at any thread count; spec 8.6's warning was right.

WASM in Node (same machine, 8 threads): portable SHA-256 190 MiB/s, MD5 150 MiB/s; FLAC 5 builds
at about 170 MiB/s, capped by MD5; extraction of FLAC about 110 MiB/s (single-threaded decode).
FLAC still beats deflate on audio (deflate stores this audio: it saves under 2%).

**Parallel FLAC restore.** `codecs/flac/parallel_decode.{hpp,cpp}`: when an extraction has a single
file, `flac::restore(..., threads = context threads)` decodes a single-stream FLAC in parallel
(not multi-mono, not in WASM, only with a project block). A reader thread reads the stored FLAC
past its metadata and `FrameSplitter` cuts it into frames: a boundary is a frame header with a
valid CRC-8, the next frame number and the stream's sample-rate code and sample-size bits (the
first frame must be number 0). Batches of 64 frames are decoded on worker threads by independent
libFLAC decoders fed a synthetic `fLaC` + STREAMINFO (total samples and MD5 zeroed) header, and
converted to container bytes there; the calling thread emits batches in order, hashing SHA-256
and writing to the sink. libFLAC checks each frame's CRC-16, so a false boundary fails the
restore instead of producing wrong output; the restored file is still checked against SHA-256
and the entry CRC-32. At most 2 × threads batches are in flight. Single 24-bit stereo file:
about 250 MiB/s on one thread, about 980 MiB/s on 8 (was 240 at any thread count).

Multi-mono groups restore with the members decoded in parallel in rounds of 32 blocks
(min(threads, members) threads per round), interleaved on the calling thread (16 channels:
225 → about 390 MiB/s on 8 threads).

Segments: 64 blocks for up to two channels, `64 × 2 / channels` blocks (at least 4) for wider
sources (`flac::segment_blocks`), so a 16-channel segment is about 1.5 MB instead of 50 MB and
several fit in the memory budget; frames are encoded independently, so output bytes do not
change. The multi-mono MD5 lane de-interleaves with width-specialised loops.

Known limits: multi-mono builds (about 100–140 MiB/s for 16 channels on the 6 GiB test VM) are
bounded by the single MD5 lane (one MD5 per channel, all on one thread) and by writing and
copying the spill files. libFLAC's own per-encoder MD5 cannot be disabled through its public API
and costs worker CPU. WASM MD5 is the cap for browser FLAC builds. Large `--size` values on
machines with little RAM measure swapping: the benchmark keeps all inputs and outputs in memory.

### 4.17 Previews and OS integration (desktop UI spec, "OS integration")

- `core/src/io/preview.{hpp,cpp}`: `make_preview(stream, file_name, {max_entries = 500,
  max_probed = 2000})` builds a `Preview` from metadata only. A stream starting with `fLaC` is a
  FLAC file (format, duration, original name and container, layout, multi-mono channel, original
  chunks, tags); anything else must open as a zip (created-by version from our comment, entry and
  file counts, total and packed size, the first 500 entries with method "Stored"/"Deflate"/"FLAC"/
  "Method n" and "restores to", restorable audio count with a multi-mono group counted once). FLAC
  headers of at most 2000 `.flac` entries are read; beyond that the count is a lower bound
  (`restorable_audio_partial`, shown as "at least").
- `preview_json` and `preview_html` render it; the HTML is self-contained (inline CSS, light and dark
  via `prefers-color-scheme`, all text escaped).
- Exposed as `zp_preview(stream, file_name, ZP_PREVIEW_JSON | ZP_PREVIEW_HTML)`, the CLI
  `satchel preview FILE [--html]`, Python `satchel.preview(path, html=False)` and PHP
  `Satchel\Preview::of($path, $html = false)`. The platform previewers call it.
- macOS Quick Look (`desktop/macos`, option `ZP_BUILD_QUICKLOOK`, built on macOS):
  `SatchelPreview.appex`, a data-based preview extension (`QLPreviewProvider`,
  `QLIsDataBasedPreview`) for `public.zip-archive` and `org.xiph.flac`. It calls `zp_preview` through
  `zp.h`, links `zp_core` statically, and returns the HTML (UTF-8, 760×560). Deviation: it is written
  in Objective-C, not Swift, because CMake cannot build Swift for several architectures with Ninja
  and the spec requires a universal (arm64 + x86_64) extension. `satchel_quicklook_tests` builds a
  zip through the C API and checks the provider's HTML and its error for other files. No thread pool or context is created. Bundle ID `<ZP_BUNDLE_ID>.preview`;
  entitlements: app sandbox + user-selected read-only. It is ad-hoc signed with the entitlements at
  build time and copied into
  `Satchel.app/Contents/PlugIns` (`satchel_embed_quicklook`), and installed there.
- Checked on macOS 27: once registered (`pluginkit -a`), Quick Look shows our preview for `.zip`
  instead of the system's (the spec's precedence question). `qlmanage -p -o DIR` crashes in
  qlmanage itself for data-based extension replies (NSDictionary nil key), so automated tests call
  the core instead; the extension was checked visually.
- `packaging/sign.cmake` signs macOS bundles inside-out (dylibs, frameworks, helpers, then each
  `.appex` with its entitlements, then the app, never `--deep`), ad-hoc when no identity is set,
  and verifies with `codesign --verify --deep --strict`. The release smoke test checks both.
- The project's warning flags apply to C and C++ only (`$<COMPILE_LANGUAGE:C,CXX>`), so
  Objective-C targets can link `zp::core` without them.
- File-manager actions call the GUI with a verb: `satchel-gui --compress PATHS` (one archive of
  everything), `--extract ZIPS` (each zip; other items ignored), `--full ZIP` (open in the browser).
  Without a verb the Simple mode drop rule applies (`satchel_gui::drop_action(items, Intent)`,
  `action_items`).
- macOS Finder: Services "Compress with Satchel" (`public.item`) and "Extract with Satchel"
  (`public.zip-archive`) in `Info.plist` `NSServices` (`NSRequiredContext` empty, so they are
  offered as Finder Quick Actions). `desktop/gui/app/mac_services.mm` (Objective-C++, ARC) registers
  the provider with `[NSApp setServicesProvider:]` at startup and forwards the pasteboard's file
  URLs to `App::open_paths` with the Compress/Extract intent on the Qt thread.
  `perform_mac_service()` drives the same selectors from tests; other platforms get a stub.
- Linux: Dolphin service menus `share/kio/servicemenus/satchel-{compress,extract}.desktop`
  (compress for all files and folders; extract and "Open in Satchel" for `application/zip`), and a
  nautilus-python extension `share/nautilus-python/extensions/satchel.py` (the `.deb` suggests
  `python3-nautilus`); its `menu_entries(paths)` logic is unit-tested without Nautilus.
- Windows (WiX patch, registry under HKLM\Software\Classes): ProgID `Satchel.zip` (icon, open =
  `--full "%1"`, verb "Extract with Satchel") offered through `.zip\OpenWithProgids` (never made
  the default), "Extract with Satchel" on `SystemFileAssociations\.zip`, "Compress with Satchel" on
  `*` and `Directory`, and `Applications\satchel-gui.exe`. These are static verbs: on Windows 11 they
  appear under "Show more options". Each verb also names an `ExplorerCommandHandler`, so Explorer
  uses our `IExplorerCommand` (one process for the whole selection; Extract hidden unless a `.zip`
  is selected) and the `command` key is only a fallback. Top-level Windows 11 entries need package
  identity (sparse MSIX), which is still pending.
- Windows shell extension (`desktop/windows`, `satchel_shell.dll`, in-process COM server,
  apartment-threaded, statically linking `zp_core`): CLSID `{8C4F2B1C-E643-456B-A814-5A3F0DB05DFB}`
  is the preview handler (`IPreviewHandler`, `IInitializeWithStream`, `IObjectWithSite`,
  `IOleWindow`) registered for `.zip` and `.flac` (`ShellEx\{8895b1c6-…}`, `PreviewHandlers`,
  `AppID` = the 64-bit prevhost surrogate). It reads through the `IStream` with seeks (the
  archive is never loaded whole), builds a `Preview`, and shows `preview_summary_text` above a
  report ListView (zip: name, size, packed, method, restores to; FLAC: `preview_flac_rows`).
  `{71C262A4-D20B-4503-9034-1243F449AFC8}` / `{5F750275-DF17-4595-BA33-3771E2C5A0D3}` are the
  Compress / Extract `IExplorerCommand`s; they start `satchel-gui.exe` from the DLL's folder with
  a command line built by `command_line.hpp` (quoting per `CommandLineToArgvW`, tested on every
  platform). `satchel_shell_tests` (Windows CI) drives the objects directly: previews of a built
  zip and a FLAC file into a hidden window, refusal of other files, command state and titles over
  real `IShellItemArray`s, and that every object is released.

## 5. Status

| Spec step (main spec 10) | Status |
|---|---|
| 1. Zip layer: store, plan/execute, collisions, symlinks, path safety (native) | Done |
| 2. WASM build + OPFS spike | WASM build, JS binding, browser app with OPFS done; the iOS/Android multi-GB device spike is pending |
| 3. Deflate: parallel deflate, store heuristic | Done |
| 4. FLAC path | Done: all six containers, multichannel and multi-mono, tags, readme, editor regeneration |
| 5. PHP and Python bindings | Done; wheels with the bundled library and the npm package are built by the release workflow |
| 6. CLI, then GUI | CLI done; GUI core done (both modes, browser, Inspector, plan review, extract, verify, edit); packaging, release workflow, OS integration (Quick Look, Finder services, Windows preview handler and Explorer commands, Dolphin/Nautilus actions, file association) and updates done; unsigned until certificates exist |

### Needs hardware, accounts or people (cannot be done in CI)

- iOS Safari / Android Chrome OPFS spike with multi-GB output.
- Windows Explorer and macOS Archive Utility extraction checks (manual).
- Signing certificates, Apple notarization, sparse MSIX package identity.
- An EdDSA key pair for updates (replace `ZP_UPDATE_PUBLIC_KEY`, add the `SPARKLE_PRIVATE_KEY`
  secret) and hosting of the appcasts.
- The per-release manual GUI pass (both modes, dark mode, screen reader, keyboard only) and checks
  of the previewers and context menus in Finder and Explorer.
- Xiph registration of the FLAC application ID; product name, icon and de-archiver URL.

### Contributions (main spec 13)

`CLA.md` (draft individual CLA: copyright license including relicensing, patent license,
representations; marked for legal review), `CONTRIBUTING.md`, and `.github/workflows/cla.yml`
(CLA Assistant Lite v2.6.1 on `pull_request_target` and comments; signatures in
`signatures/version1/cla.json` on the orphan branch `cla-signatures`; the maintainer account and
bots are allow-listed).

## 6. Testing

- `ctest` (or `zp_tests`) runs everything; tags: `[path] [planner] [reader] [zip_writer] [zip64]
  [codec] [stream] [metadata] [integration] [extract] [editor] [filesystem] [interop] [capi]
  [hash] [pcm] [tags] [flac] [readme] [golden]`.
- Golden tests pin the compressed bytes of deflate and FLAC; they pass on arm64, x86-64 and WASM.
- PCM fixtures are generated in C++ (`tests/support/pcm_fixtures.cpp`): every container, bit
  depths 8–32, EXTENSIBLE, odd-sized and trailing chunks, chunks after the audio, trailing bytes,
  float, compressed and placeholder variants.
- Interop with the `flac` tool (when installed): `flac -t` and `flac -d --keep-foreign-metadata` on
  our files, and our restore of files encoded by `flac --keep-foreign-metadata`.
- Interop tests run Info-ZIP `unzip`/`zip` and 7-Zip when they are installed and skip otherwise.
- Hidden `[.large]` tests (`zp_tests "[large]"`, weekly in `full.yml`, about 6 minutes, ~10 GB of
  temporary disk): a 4 GiB + 12345-byte store entry (Zip64, checked with `unzip -t`/`7z t` when
  present); a 4 GiB 24-bit stereo RF64 through FLAC restored bit-exactly under a 96 MiB budget; a
  2 GiB deflate entry with 1 and 64 threads under a 48 MiB budget. Inputs are generated on the fly
  (`GeneratedStream`), and restores are checked by SHA-256 through a hashing sink.
- Channel and rate extremes round-trip bit-exactly: 64 channels (multi-mono), 9 channels at
  192 kHz, 16 channels, and 768 kHz stereo 24-bit / mono 32-bit (non-subset FLAC).
- With ffmpeg installed, extracted FLACs (6-channel, and a member of a 10-channel multi-mono set)
  decode in `ffmpeg` and `ffprobe` shows the channel count and the mirrored tags (bext
  description, iXML track name, ENCODER). Weekly `interop` job in `full.yml`: unzip, 7-Zip, the
  `flac` tool and ffmpeg.
- Browser tests also run on emulated phones: `SATCHEL_DEVICES="iPhone 15:webkit,Pixel 7:chromium"`
  (Playwright device descriptors; weekly). Real iOS/Android devices remain a manual check.
- Sanitizers: `clang-asan` and `clang-tsan` presets. clang-tidy: `.clang-tidy` at the root.
