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
- A hasher thread computes the SHA-256 of the whole source file and the FLAC MD5 of the samples. The
  MD5 input is signed little-endian samples of the container width. For multi-mono there is one
  MD5 per channel. Its queue holds at most 16 chunks of 1 MiB, outside the memory budget.
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
- Checks: zip CRC-32 of every member (checked as the stream is consumed), libFLAC's MD5 check,
  sample count, and SHA-256. Any failure deletes the partial output.
- `flac::restore` also rebuilds FLAC files made by `flac --keep-foreign-metadata` (no project
  block, so no SHA-256). The extractor does not restore those automatically, because they do not
  carry our ID (spec).

### 4.9 WebAssembly

- Preset `wasm` (configure with `emcmake cmake --preset wasm`; Emscripten 6.0.10 in CI). All code is
  built with `-pthread`; linking uses `ALLOW_MEMORY_GROWTH` and a 1 MiB stack.
- `bindings/js` builds `satchel.mjs` + `satchel.wasm`: an ES module (`createSatchel()`) that
  exports every `ZP_API` function of `zp.h` (the export list is generated from the header) plus
  `ccall`, `cwrap`, `addFunction` and heap helpers. Maximum memory 512 MB
  (`ZP_WASM_MAXIMUM_MEMORY`), pthread pool `min(hardwareConcurrency, 16) + 3` (N workers + reader,
  hasher and the caller), growable if more threads are needed.
- The module blocks while a job runs, so in a browser it must run inside a Web Worker (main spec 3);
  under Node it can run on the main thread.
- The Catch2 suite is also built for WASM and runs under Node (`-sPROXY_TO_PTHREAD`, `NODERAWFS`);
  `bindings/js/test/smoke.mjs` exercises the module through the C API.
- `ZP_API` in `zp.h` marks exports: `used` + default visibility under Emscripten and GCC/Clang,
  `dllexport`/`dllimport` for a Windows DLL (`ZP_SHARED_BUILD` / `ZP_SHARED`).

### 4.10 C API

`core/include/zp/zp.h`. Additions beyond the design doc's list: stream constructors (file, atomic
file + commit, memory, callbacks), input constructors (paths, memory), option initializers,
`zp_plan_executable`, `zp_plan_total_bytes`, build result accessors, extraction issue/item/outcome
accessors and `zp_xplan_decide`, `zp_sink_filesystem`/`zp_sink_null`, `zp_status_name`,
`zp_version`. The progress callback returns non-zero to cancel. `zp_last_error` is thread-local.

## 5. Status

| Spec step (main spec 10) | Status |
|---|---|
| 1. Zip layer: store, plan/execute, collisions, symlinks, path safety (native) | Done |
| 2. WASM build + OPFS spike | WASM build, Node tests and module done; browser glue (Worker, OPFS, File System Access) and the device spike are pending |
| 3. Deflate: parallel deflate, store heuristic | Done |
| 4. FLAC path | Done: all six containers, multichannel and multi-mono, tags, readme, editor regeneration |
| 5. PHP and Python bindings | Not started |
| 6. CLI, then GUI | Not started |

### Needs hardware, accounts or people (cannot be done in CI)

- iOS Safari / Android Chrome OPFS spike with multi-GB output.
- Windows Explorer and macOS Archive Utility extraction checks (manual).
- Signing certificates, Apple notarization, sparse MSIX package identity.
- Xiph registration of the FLAC application ID; product name, icon and de-archiver URL.

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
- Sanitizers: `clang-asan` and `clang-tsan` presets. clang-tidy: `.clang-tidy` at the root.
