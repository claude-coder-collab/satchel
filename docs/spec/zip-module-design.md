# Zip Archive Module — Component Design

Companion to the main spec (v4), Sections 2, 6 and 7. Location: `core/src/io/zip/`.

## 1. Scope

- Two-phase archive creation: plan (names, methods, conflicts, warnings) then execute.
- Streaming write and read of store (0) and deflate (8) entries, Zip64 when required.
- Extraction with path safety and symlink skipping.
- Editing existing archives via rewrite + atomic replace.
- Archive metadata in the EOCD comment.

Codec internals (parallel deflate, FLAC/RF64 pipeline) live in `core/src/codecs/` and are consumed through the codec registry. This module only routes data to them.

## 2. Dependencies

| Dependency | Use |
|---|---|
| minizip-ng (`MZ_ZLIB=ON` against zlib-ng, `MZ_ZIP64=ON`, other backends off) | Zip record read/write |
| zlib-ng | Deflate and CRC32 (via codec registry); pinned version |
| utf8proc | NFC normalization and case folding for names and collision checks (MIT, builds for WASM) |

## 3. Layering

```
C API
  └─ ArchivePlanner ─► ArchivePlan ─► ArchiveBuilder ─► EntryWriter ─► minizip-ng
                                                         └─ codec registry (store/deflate/flac)
  └─ ArchiveReader ─► ArchiveExtractor ─► codec registry
  └─ ArchiveEditor (uses Reader + Planner + Builder)
Shared: IChunkedStream, InputSource, PathPolicy, ArchiveMetadata, Zip64Policy, Status
```

## 4. Streams and Inputs

```
IChunkedStream
    read(uint8_t* buf, size_t len) -> size_t
    write(const uint8_t* buf, size_t len) -> size_t
    seekable() -> bool
    seek(uint64_t pos) -> Status
    tell() -> uint64_t
    size() -> optional<uint64_t>
    reopen() -> Status              // re-readable sources only; needed for the two-pass FLAC fallback
```

```
MinizipStreamAdapter            // implements minizip-ng's mz_stream vtable over IChunkedStream
```

```
InputSource                     // abstraction over "things to archive"
    enumerate() -> vector<InputItem>

InputItem
    source_path: string         // as given by the platform
    archive_path: string        // relative path inside the archive (pre-normalization)
    kind: FILE | DIRECTORY | SYMLINK
    size: uint64_t
    mtime: timestamp
    unix_mode: uint32_t
    open() -> unique_ptr<IChunkedStream>

FilesystemInputSource : InputSource     // native; lstat-based, never follows links;
                                        // Windows junctions/reparse points reported as SYMLINK
BrowserFileListInputSource : InputSource // WASM; built from JS File objects; never reports SYMLINK
```

## 5. Path Policy

```
PathPolicy
    normalize(string) -> string              // UTF-8 validate, NFC, '/' separators, strip leading './'
    collision_key(string) -> string          // normalize + case-fold
    validate_for_archive(string) -> Status   // no empty segments, no '..', not absolute, no drive letters
    validate_for_extraction(string dest_root, string entry) -> Status
                                             // resolved path must stay under dest_root
```

## 6. Planning

```
ArchivePlanner
    ArchivePlanner(const PlannerOptions&, CodecRegistry&)
    plan(InputSource&) -> ArchivePlan
    replan(ArchivePlan&, const vector<Resolution>&) -> ArchivePlan   // apply user decisions, re-check

PlannerOptions
    flac_enabled: bool              // default true
    deflate_level: int              // default 6
    flac_level: int                 // default 5
```

```
ArchivePlan
    entries: vector<PlanEntry>
    conflicts: vector<Conflict>     // blocking; execute refused while non-empty
    warnings: vector<Warning>       // non-blocking; caller must show them before execute
    executable() -> bool            // conflicts.empty()

PlanEntry
    item: InputItem
    output_name: string             // normalized; e.g. take1.flac for converted WAVs
    codec: FLAC | FLAC_MONO | GENERAL | GENERATED   // GENERAL = deflate with store heuristic at execute time
    group_id: optional<uuid>        // FLAC_MONO members
    channel_index: optional<uint16_t>
    flac_fallback_reason: optional<FallbackReason>
    snapshot: {size, mtime}         // re-checked at execute

Conflict
    collision_key: string
    entries: vector<index>          // all plan entries mapping to the same key

Warning
    kind: SYMLINK_SKIPPED | FLAC_FALLBACK
    source_path: string
    detail: string

Resolution
    entry: index
    action: RENAME(new_name) | SKIP | DISABLE_FLAC
```

Planner rules:
- If any entry is FLAC or FLAC_MONO, a `README` plan entry (kind GENERATED) is added at the archive root (main spec 6.5), included in collision checks. Its content is rendered at execute time from the template and the final plan.
- Directories become directory entries; SYMLINK items are dropped and produce `SYMLINK_SKIPPED` warnings.
- PCM eligibility (WAV, RF64, AIFF, AIFF-C, CAF, Wave64) is decided **conclusively** at plan time by a header-only container walk (seeks, no full read), per main spec 7.2. Execute never changes the decision, so output names are stable.
- ≤ 8 channels → one `PlanEntry` (`take1.flac`). > 8 channels → one `PlanEntry` per channel (`take1_chNN.flac`) sharing a `group_id`; the group is one unit for skip/rename resolutions.
- Ineligible PCM files keep their name, use store, and get a `FLAC_FALLBACK` warning with the reason.
- Collisions are computed over `collision_key(output_name)` across all entries, including directories.
- `replan` rejects a `RENAME` whose new name fails `validate_for_archive` or creates a new collision.

## 7. Building

```
ArchiveBuilder
    ArchiveBuilder(IChunkedStream& output, CodecRegistry&, const BuilderOptions&)
    execute(const ArchivePlan&, ProgressSink&) -> BuildResult   // refuses if !plan.executable()
    set_metadata(const ArchiveMetadata&)                         // written into EOCD on finish

BuilderOptions
    threads: int                    // 0 = hardware concurrency
    small_entry_threshold: uint64_t // default 4 MiB

BuildResult
    per_entry: vector<EntryResult>  // method used, sizes, CRC32, warnings
    status: Status
```

Execution model: see Section 8 (Threading and Pipeline).
- Before reading each entry, `snapshot` is compared with the current size/mtime. A mismatch fails that entry with `SOURCE_CHANGED`.
- Store heuristic (GENERAL entries): deflate the first sample window; if the saving is below the threshold, the entry is written as store.

```
EntryWriter                     // one per entry, internal
    writes local file header (UTF-8 flag set; extended timestamp; unix mode in external attrs)
    streams codec output, computes CRC32
    if output.seekable(): seeks back and patches sizes/CRC into the local header
    else: writes a data descriptor
    returns the central directory record to ArchiveBuilder
```

```
Zip64Policy
    needs_zip64_entry(optional<uint64_t> size_hint) -> bool   // unknown size => reserve Zip64 extra field
    needs_zip64_archive(uint64_t cd_offset, uint64_t entry_count) -> bool
```

## 8. Threading and Pipeline

All parallelism is owned by the core. Third-party libraries (libFLAC, zlib-ng) run single-threaded and never create threads.

### 8.1 Threads

| Thread | Count | Job |
|---|---|---|
| Reader | 1 | Reads input in plan order, cuts segments, applies backpressure |
| Hasher | 1 | SHA-256 of whole original files and MD5 of audio for STREAMINFO (both sequential by nature) |
| Workers | N | Compress or encode segments |
| Writer | 1 | Writes finished segments in plan order, combines CRC32 |

- N = `threads` (0 = hardware concurrency).
- The pool is created once per library context (`zp_context_create(threads, memory_budget)`) and shared by every build, extraction and edit in that context.
- WASM: all threads are preallocated at startup (Emscripten `PTHREAD_POOL_SIZE` = N + 3). N defaults to `navigator.hardwareConcurrency`, capped so the memory budget fits the WASM memory limit.

### 8.2 Segments

Every entry is cut into fixed-size segments; a small entry is a single segment. Segment sizes are constants, independent of thread count, so output is byte-identical at any thread count.

| Codec | Segment size | Why segments are independent |
|---|---|---|
| Store | 4 MiB | Passthrough |
| Deflate | 1 MiB of input | pigz-style: each segment uses the previous 32 KiB as preset dictionary and ends with a sync flush; the last segment ends the stream |
| FLAC multichannel | 64 blocks × 4,096 samples per channel | FLAC frames are independent; each segment gets its own single-threaded libFLAC encoder with identical settings and fixed block size |
| FLAC multi-mono | Same, per channel | Task = (channel, segment); each channel is appended in order to its own spill file |

### 8.3 Joining Parallel FLAC Segments

- Each segment encoder writes to memory. Metadata writes (write callback with 0 samples) are discarded; only frames are kept.
- The worker renumbers its frames (frame number = segment index × 64 + frame index), recomputing each header's CRC-8 and each frame's CRC-16, per the FLAC format specification (RFC 9639).
- The core writes all metadata blocks itself (STREAMINFO, VORBIS_COMMENT, foreign metadata, project block).
- STREAMINFO: total samples known from the header scan; fixed block size; min/max frame size aggregated by the writer; MD5 from the hasher. Patched at the end together with the SHA-256 placeholder.

### 8.4 Scheduling

- One FIFO task queue in plan order. Segments of a large entry and whole small entries run side by side; there is no separate "large file mode".
- The writer holds a reorder window: a finished segment waits until all earlier segments are written.
- Memory: segments in flight are bounded by `memory_budget` (default 256 MiB native, 128 MiB WASM). The reader blocks when the budget is full.
- CRC32: each worker computes its segment's CRC; the writer combines them with `crc32_combine`.
- Multi-mono: per-channel ordered appends to spill files; copying finished spill files into the archive is a writer job.
- Cancellation: the token is checked between segments; in-flight segments are discarded.

### 8.5 Extraction

- Entries go to separate output files, so there is no ordering constraint: one task per entry, entries in parallel, each decoded single-threaded (inflate is sequential).
- Multi-mono groups: members decoded in parallel, then interleaved by a group task; SHA-256 verified by the hasher.

### 8.6 Bottlenecks to Benchmark Early

- **Hasher**: SHA-256 is sequential. Without CPU SHA instructions (notably in WASM) it may cap throughput for FLAC entries. If it does, a faster tree hash is not an option (the hash identifies the original file), so the mitigation is overlapping hashing across entries.
- **Reader I/O**: on slow disks and browser `File` reads, the reader, not the workers, will set throughput.

## 9. Reading and Extraction

```
ArchiveReader
    open(IChunkedStream& input) -> Status      // EOCD + central directory only
    entries() -> const vector<ZipEntryInfo>&
    open_entry(index) -> unique_ptr<IChunkedStream>   // raw decompressed stream (store/deflate)
    metadata() -> optional<ArchiveMetadata>

ZipEntryInfo
    name: string                    // decoded: UTF-8 if flag set, else CP437
    kind: FILE | DIRECTORY | SYMLINK // SYMLINK from S_IFLNK in external attrs
    method: STORE | DEFLATE | UNSUPPORTED
    compressed_size, uncompressed_size: uint64_t
    crc32: uint32_t
    mtime: timestamp
    unix_mode: optional<uint32_t>
    flac_restorable: bool           // .flac entry whose APPLICATION block carries our ID;
                                    // detected by reading only the FLAC metadata blocks
    flac_group: optional<{group_id, channel_index, channel_count}>   // multi-mono members
```

```
ArchiveExtractor
    ArchiveExtractor(ArchiveReader&, OutputSink&, const ExtractOptions&)
    plan_extraction(selection) -> ExtractionPlan   // warnings + blocking errors, before any write
    execute(const ExtractionPlan&, ProgressSink&) -> ExtractResult

ExtractOptions
    restore_wav: bool               // default true; false = keep .flac
    include_readme: bool            // default false; readme identified via ArchiveMetadata.readme_name
    overwrite: ASK | SKIP | REPLACE

ExtractionPlan warnings/errors
    SYMLINK_SKIPPED (warning)
    UNSUPPORTED_METHOD (warning, entry skipped)
    UNSAFE_PATH (error, entry refused)
    NAME_COLLISION (error after collision_key, e.g. case-only duplicates in third-party archives)
    INCOMPLETE_GROUP (error, multi-mono group cannot be rebuilt)
    EXISTS_AT_DESTINATION (needs decision when overwrite = ASK)
```

- CRC32 is verified for every entry. For restored PCM files, SHA-256 is verified as well (FLAC codec). A mismatch is a hard error and the partial output file is deleted.
- Multi-mono groups: selecting any member selects the whole group. `ExtractionPlan` reports `INCOMPLETE_GROUP` (error) when members are missing, duplicated or inconsistent.
- `OutputSink` is a platform abstraction: native filesystem, browser directory handle, or browser per-file download.

## 10. Editing

```
ArchiveEditor
    open(IChunkedStream& input) -> Status
    add(InputSource&)                 // planned with the same ArchivePlanner rules
    remove(index)
    rename(index, new_name)
    replace(index, InputItem)
    plan() -> ArchivePlan             // merged view: kept entries + additions; conflicts/warnings
    commit(IChunkedStream& output, ProgressSink&) -> Status
```

- Kept entries are copied raw (compressed bytes and CRC unchanged, no recompression).
- The readme is regenerated from the merged plan on every commit, and dropped if no converted entries remain.
- Native commit writes to a temp file in the same directory, fsyncs, then atomically replaces (`rename` / `ReplaceFileW`). On failure the original is untouched and the temp file is removed.
- In the browser, commit writes a new output file; there is no in-place replace.

## 11. Shared Types

```
ArchiveMetadata
    magic: uint32_t
    schema_version: uint8_t
    app_version: string
    readme_name: string             // empty if no readme generated
    serialize() -> vector<uint8_t>
    static parse(bytes) -> optional<ArchiveMetadata>   // nullopt on foreign/garbage comment, never throws

Status                             // error enum; no exceptions cross the C API
    OK, IO_ERROR, SOURCE_CHANGED, CONFLICTS_UNRESOLVED, UNSAFE_PATH,
    UNSUPPORTED_METHOD, CRC_MISMATCH, HASH_MISMATCH, CORRUPT_ARCHIVE, CANCELLED
```

`ProgressSink` provides byte-level progress and cancellation. Cancellation leaves no partial archive on native targets.

## 12. C API

```
/* context: owns the thread pool and memory budget */
zp_context_t* zp_context_create(int threads, uint64_t memory_budget);   /* 0 = defaults */
void          zp_context_free(zp_context_t*);

/* planning */
zp_plan_t*  zp_plan_create(zp_context_t*, zp_input_t* input, const zp_plan_options_t* opts);
size_t      zp_plan_entry_count(const zp_plan_t*);
int         zp_plan_get_entry(const zp_plan_t*, size_t i, zp_plan_entry_t* out);
size_t      zp_plan_conflict_count(const zp_plan_t*);
int         zp_plan_get_conflict(const zp_plan_t*, size_t i, zp_conflict_t* out);
size_t      zp_plan_warning_count(const zp_plan_t*);
int         zp_plan_get_warning(const zp_plan_t*, size_t i, zp_warning_t* out);
int         zp_plan_resolve(zp_plan_t*, const zp_resolution_t* res, size_t n);
void        zp_plan_free(zp_plan_t*);

/* building */
int zp_build(zp_plan_t*, zp_stream_t* output, const zp_build_options_t*,
             zp_progress_fn, void* user, zp_build_result_t** out_result);
void zp_build_result_free(zp_build_result_t*);

/* reading / extraction */
zp_reader_t* zp_reader_open(zp_context_t*, zp_stream_t* input);
size_t       zp_reader_entry_count(const zp_reader_t*);
int          zp_reader_get_entry(const zp_reader_t*, size_t i, zp_entry_info_t* out);
int          zp_reader_get_app_version(const zp_reader_t*, char* buf, size_t len);
zp_xplan_t*  zp_extract_plan(zp_reader_t*, const size_t* sel, size_t n, zp_sink_t*, const zp_extract_options_t*);
int          zp_extract(zp_xplan_t*, zp_progress_fn, void* user);
void         zp_xplan_free(zp_xplan_t*);
void         zp_reader_free(zp_reader_t*);

/* editing */
zp_editor_t* zp_editor_open(zp_context_t*, zp_stream_t* input);
int          zp_editor_add(zp_editor_t*, zp_input_t*);
int          zp_editor_remove(zp_editor_t*, size_t i);
int          zp_editor_rename(zp_editor_t*, size_t i, const char* new_name);
int          zp_editor_replace(zp_editor_t*, size_t i, zp_input_t*);
zp_plan_t*   zp_editor_plan(zp_editor_t*);
int          zp_editor_commit(zp_editor_t*, zp_stream_t* output, zp_progress_fn, void* user);
void         zp_editor_free(zp_editor_t*);
```

All functions return `Status` codes or NULL on failure; `zp_last_error()` gives a message for the calling thread. Strings are UTF-8.

## 13. Tests

### 13.1 Unit

- `PathPolicy`: NFC vs. NFD inputs, case folding (incl. non-ASCII), `..`, absolute, drive-letter and empty-segment paths.
- Planner:
  - collisions: exact, case-only, normalization-only, `take1.wav` + `take1.flac`, file vs. directory with same key
  - symlinks and junctions produce warnings and no entries
  - each PCM store reason is detected by header walk alone, for every supported container
  - > 8 channels expands to one entry per channel with a shared group ID; generated names take part in collision checks
  - `replan` applies each resolution type and rejects renames that collide or are invalid
- `EntryWriter`: seek-back path and data-descriptor path produce valid records; UTF-8 flag; extended timestamp; unix mode.
- `Zip64Policy`: size and entry-count thresholds at −1, 0, +1; unknown size.
- `ArchiveMetadata`: round-trip; foreign, truncated and empty comments return nullopt.
- `ArchiveReader`: CP437 fallback for names without the UTF-8 flag; `S_IFLNK` detection; unsupported methods reported.
- Extractor planning: unsafe paths, symlink entries, case-only duplicates, existing-destination handling.

### 13.2 Integration

- Plan → build → read → extract round-trip for mixed trees (files, empty dirs, WAV, RF64, already-compressed media), byte-identical.
- Output identical for thread counts 1, 2, 8 and for seekable vs. non-seekable output (aside from descriptor vs. patched headers).
- Parallel FLAC: output passes `flac -t`; frame numbers continuous; STREAMINFO min/max frame size and MD5 correct; expected byte-identical to a single sequential libFLAC encoder with the same settings (investigate any difference).
- Memory budget: peak memory stays under `memory_budget` with 1 thread and with 64 threads, on a multi-GB entry and on 100,000 small entries.
- `SOURCE_CHANGED` when a file is modified between plan and build.
- Build refused with unresolved conflicts.
- Multi-GB entry under a memory cap; > 65,535 entries (Zip64 entry count).
- Editor: each operation alone and combined; kept entries byte-identical to the originals; failure injected mid-commit leaves the original intact and no temp file.
- Third-party archives (Info-ZIP, 7-Zip, Windows Explorer, macOS Archive Utility): list and extract; archives containing symlinks and malicious paths are handled per the extraction plan.
- Cancellation mid-build and mid-extract leaves no partial outputs.

### 13.3 End-to-End

- Archives from this module extract with Info-ZIP `unzip`, macOS Archive Utility, Windows Explorer and 7-Zip, including > 4 GB and > 65,535-entry archives.
- CLI create → GUI browse/edit → CLI extract, consistent results.
- Browser: plan with collisions → resolve in UI → build to OPFS/File System Access → native extract, byte-identical.
