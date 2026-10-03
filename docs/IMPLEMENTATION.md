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
| `{DEARCHIVER_URL}` | `https://satchel.invalid/open` | Also the AGPL source-offer location |
| EOCD comment magic | `STCH` (0x53544348) | Big-endian in the binary record |
| FLAC APPLICATION ID | `Stch` | Must be registered with Xiph |
| Copyright holder | Venn Audio Ltd. | SPDX: `AGPL-3.0-only OR LicenseRef-Commercial` |

## 2. Repository layout

```
core/include/zp/zp.h     C API (the only public interface)
core/src/common/         status codes, byte helpers, product identity
core/src/io/             streams, file system, input sources
core/src/io/zip/         planner, builder, reader, extractor, editor, metadata, path policy
core/src/codecs/         codec registry, store, deflate
core/src/pipeline/       context, thread pools, memory budget
tests/unit, tests/integration, tests/support, tests/capi
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
  plus two service threads (reader, hasher). The writer runs on the calling thread. Jobs in one
  context are serialized by a mutex. (WASM pool size N + 3 remains correct.)
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

### 4.8 C API

`core/include/zp/zp.h`. Additions beyond the design doc's list: stream constructors (file, atomic
file + commit, memory, callbacks), input constructors (paths, memory), option initializers,
`zp_plan_executable`, `zp_plan_total_bytes`, build result accessors, extraction issue/item/outcome
accessors and `zp_xplan_decide`, `zp_sink_filesystem`/`zp_sink_null`, `zp_status_name`,
`zp_version`. The progress callback returns non-zero to cancel. `zp_last_error` is thread-local.

## 5. Status

| Spec step (main spec 10) | Status |
|---|---|
| 1. Zip layer: store, plan/execute, collisions, symlinks, path safety (native) | Done |
| 2. WASM build + OPFS spike | Not started |
| 3. Deflate: parallel deflate, store heuristic | Done |
| 4. FLAC path | Not started |
| 5. PHP and Python bindings | Not started |
| 6. CLI, then GUI | Not started |

### Needs hardware, accounts or people (cannot be done in CI)

- iOS Safari / Android Chrome OPFS spike with multi-GB output.
- Windows Explorer and macOS Archive Utility extraction checks (manual).
- Signing certificates, Apple notarization, sparse MSIX package identity.
- Xiph registration of the FLAC application ID; product name, icon and de-archiver URL.

## 6. Testing

- `ctest` (or `zp_tests`) runs everything; tags: `[path] [planner] [reader] [zip_writer] [zip64]
  [codec] [stream] [metadata] [integration] [extract] [editor] [filesystem] [interop] [capi]`.
- Interop tests run Info-ZIP `unzip`/`zip` and 7-Zip when they are installed and skip otherwise.
- Sanitizers: `clang-asan` and `clang-tsan` presets. clang-tidy: `.clang-tidy` at the root.
