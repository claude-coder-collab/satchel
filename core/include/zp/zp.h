/* SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
 * Copyright (c) 2026 Venn Audio Ltd.
 *
 * Satchel C API. Every binding and application goes through this header.
 *
 * Conventions:
 *  - Functions returning int return a ZP_* status code (ZP_OK on success).
 *  - Functions returning a pointer return NULL on failure.
 *  - After a failure, zp_last_error() describes it for the calling thread.
 *  - All strings are UTF-8. Strings returned inside structs stay valid until the object that
 *    produced them is freed or modified.
 *  - No exceptions cross this boundary.
 */
#ifndef ZP_H
#define ZP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- status codes ---------------------------------------------------------------------- */

enum {
    ZP_OK = 0,
    ZP_IO_ERROR = 1,
    ZP_SOURCE_CHANGED = 2,
    ZP_CONFLICTS_UNRESOLVED = 3,
    ZP_UNSAFE_PATH = 4,
    ZP_UNSUPPORTED_METHOD = 5,
    ZP_CRC_MISMATCH = 6,
    ZP_HASH_MISMATCH = 7,
    ZP_CORRUPT_ARCHIVE = 8,
    ZP_CANCELLED = 9,
    ZP_INVALID_ARGUMENT = 10,
    ZP_INVALID_NAME = 11,
    ZP_NAME_COLLISION = 12,
    ZP_DECISION_REQUIRED = 13,
    ZP_INCOMPLETE_GROUP = 14,
    ZP_INTERNAL = 15
};

/* Entry kinds */
enum {
    ZP_KIND_FILE = 0,
    ZP_KIND_DIRECTORY = 1,
    ZP_KIND_SYMLINK = 2
};

/* Plan entry codecs */
enum {
    ZP_CODEC_GENERAL = 0,
    ZP_CODEC_FLAC = 1,
    ZP_CODEC_FLAC_MONO = 2,
    ZP_CODEC_GENERATED = 3,
    ZP_CODEC_KEPT = 4
};

/* Zip methods as stored */
enum {
    ZP_METHOD_STORE = 0,
    ZP_METHOD_DEFLATE = 8
};

enum {
    ZP_CONFLICT_COLLISION = 0,
    ZP_CONFLICT_INVALID_NAME = 1
};

enum {
    ZP_WARNING_SYMLINK_SKIPPED = 0,
    ZP_WARNING_FLAC_FALLBACK = 1
};

enum {
    ZP_RESOLVE_RENAME = 0,
    ZP_RESOLVE_SKIP = 1,
    ZP_RESOLVE_DISABLE_FLAC = 2
};

enum {
    ZP_OVERWRITE_ASK = 0,
    ZP_OVERWRITE_SKIP = 1,
    ZP_OVERWRITE_REPLACE = 2
};

enum {
    ZP_ISSUE_SYMLINK_SKIPPED = 0,
    ZP_ISSUE_UNSUPPORTED_METHOD = 1,
    ZP_ISSUE_UNSAFE_PATH = 2,
    ZP_ISSUE_NAME_COLLISION = 3,
    ZP_ISSUE_INCOMPLETE_GROUP = 4,
    ZP_ISSUE_EXISTS_AT_DESTINATION = 5
};

enum {
    ZP_DECISION_WRITE = 0,
    ZP_DECISION_SKIP = 1,
    ZP_DECISION_REPLACE = 2,
    ZP_DECISION_UNDECIDED = 3
};

typedef struct zp_context zp_context_t;
typedef struct zp_stream zp_stream_t;
typedef struct zp_input zp_input_t;
typedef struct zp_plan zp_plan_t;
typedef struct zp_build_result zp_build_result_t;
typedef struct zp_reader zp_reader_t;
typedef struct zp_sink zp_sink_t;
typedef struct zp_xplan zp_xplan_t;
typedef struct zp_editor zp_editor_t;

/* Progress callback: return 0 to continue, non-zero to cancel. */
typedef int (*zp_progress_fn)(void* user, uint64_t done, uint64_t total);

const char* zp_last_error(void);
const char* zp_status_name(int status);
const char* zp_version(void);

/* ---- context: owns the thread pool and memory budget ----------------------------------- */

zp_context_t* zp_context_create(int threads, uint64_t memory_budget); /* 0 = defaults */
void zp_context_free(zp_context_t* ctx);

/* ---- streams --------------------------------------------------------------------------- */

zp_stream_t* zp_stream_open_file(const char* path);
/* Writes to a temporary file next to `path`; zp_stream_commit atomically replaces `path`.
 * Freeing an uncommitted stream removes the temporary file. */
zp_stream_t* zp_stream_create_file(const char* path);
int zp_stream_commit(zp_stream_t* stream);
zp_stream_t* zp_stream_memory(void);
zp_stream_t* zp_stream_memory_from(const uint8_t* data, size_t len);
/* Pointer into a memory stream's buffer; valid until the stream is written to or freed. */
int zp_stream_memory_data(const zp_stream_t* stream, const uint8_t** data, size_t* len);

typedef struct zp_stream_callbacks
{
    void* user;
    /* Return bytes read (0 at end), or -1 on error. NULL for write-only streams. */
    int64_t (*read)(void* user, uint8_t* buf, size_t len);
    /* Return bytes written, or -1 on error. NULL for read-only streams. */
    int64_t (*write)(void* user, const uint8_t* buf, size_t len);
    /* Absolute seek; return 0 on success. NULL for non-seekable streams. */
    int (*seek)(void* user, uint64_t pos);
    /* Total size, or -1 if unknown. May be NULL. */
    int64_t (*size)(void* user);
} zp_stream_callbacks_t;

zp_stream_t* zp_stream_from_callbacks(const zp_stream_callbacks_t* callbacks);
void zp_stream_free(zp_stream_t* stream);

/* ---- inputs ---------------------------------------------------------------------------- */

/* Each path becomes a top-level entry; folders are archived recursively. */
zp_input_t* zp_input_from_paths(const char* const* paths, size_t count);
zp_input_t* zp_input_memory(void);
int zp_input_memory_add_file(zp_input_t* input, const char* archive_path, const uint8_t* data, size_t len, int64_t mtime, uint32_t unix_mode);
int zp_input_memory_add_directory(zp_input_t* input, const char* archive_path, int64_t mtime, uint32_t unix_mode);
void zp_input_free(zp_input_t* input);

/* ---- planning -------------------------------------------------------------------------- */

typedef struct zp_plan_options
{
    int flac_enabled; /* default 1 */
    int deflate_level; /* default 6 */
    int flac_level; /* default 5 */
} zp_plan_options_t;

void zp_plan_options_init(zp_plan_options_t* options);

typedef struct zp_plan_entry
{
    const char* source_path;
    const char* output_name;
    int kind;
    int codec;
    uint64_t size;
    int64_t mtime;
    uint32_t unix_mode;
    int has_group;
    uint8_t group_id[16];
    uint16_t channel_index;
    int fallback_reason; /* -1 if none */
} zp_plan_entry_t;

typedef struct zp_conflict
{
    int kind;
    const char* collision_key;
    const char* detail;
    const size_t* entries;
    size_t entry_count;
} zp_conflict_t;

typedef struct zp_warning
{
    int kind;
    const char* source_path;
    const char* detail;
} zp_warning_t;

typedef struct zp_resolution
{
    size_t entry;
    int action;
    const char* new_name; /* ZP_RESOLVE_RENAME only */
} zp_resolution_t;

zp_plan_t* zp_plan_create(zp_context_t* ctx, zp_input_t* input, const zp_plan_options_t* options);
size_t zp_plan_entry_count(const zp_plan_t* plan);
int zp_plan_get_entry(const zp_plan_t* plan, size_t i, zp_plan_entry_t* out);
size_t zp_plan_conflict_count(const zp_plan_t* plan);
int zp_plan_get_conflict(const zp_plan_t* plan, size_t i, zp_conflict_t* out);
size_t zp_plan_warning_count(const zp_plan_t* plan);
int zp_plan_get_warning(const zp_plan_t* plan, size_t i, zp_warning_t* out);
/* Applies all resolutions at once (indices refer to the plan before the call). On failure the
 * plan is unchanged. Skipped entries disappear, so indices shift afterwards. */
int zp_plan_resolve(zp_plan_t* plan, const zp_resolution_t* resolutions, size_t count);
int zp_plan_executable(const zp_plan_t* plan);
uint64_t zp_plan_total_bytes(const zp_plan_t* plan);
void zp_plan_free(zp_plan_t* plan);

/* ---- building -------------------------------------------------------------------------- */

typedef struct zp_build_options
{
    uint64_t small_entry_threshold; /* 0 = default */
} zp_build_options_t;

typedef struct zp_entry_result
{
    size_t plan_index;
    const char* name;
    int method;
    uint64_t compressed_size;
    uint64_t uncompressed_size;
    uint32_t crc32;
    int status;
    const char* message;
} zp_entry_result_t;

/* Returns the build status. *out_result (if non-NULL) receives per-entry results even when the
 * build fails; free it with zp_build_result_free. */
int zp_build(zp_plan_t* plan, zp_stream_t* output, const zp_build_options_t* options, zp_progress_fn progress, void* user, zp_build_result_t** out_result);
size_t zp_build_result_entry_count(const zp_build_result_t* result);
int zp_build_result_get_entry(const zp_build_result_t* result, size_t i, zp_entry_result_t* out);
int zp_build_result_zip64(const zp_build_result_t* result);
void zp_build_result_free(zp_build_result_t* result);

/* ---- reading and extraction ------------------------------------------------------------ */

typedef struct zp_entry_info
{
    const char* name;
    int kind;
    int method; /* ZP_METHOD_* or the raw method number when unsupported */
    int supported;
    uint64_t compressed_size;
    uint64_t uncompressed_size;
    uint32_t crc32;
    int64_t mtime;
    int has_unix_mode;
    uint32_t unix_mode;
    int flac_restorable;
    int has_flac_group;
    uint8_t flac_group_id[16];
    uint16_t flac_channel_index;
    uint16_t flac_channel_count;
} zp_entry_info_t;

/* The reader keeps using `input`; free the reader before the stream. */
zp_reader_t* zp_reader_open(zp_context_t* ctx, zp_stream_t* input);
size_t zp_reader_entry_count(const zp_reader_t* reader);
int zp_reader_get_entry(const zp_reader_t* reader, size_t i, zp_entry_info_t* out);
/* Copies the creating application's version (from the archive comment) into buf. */
int zp_reader_get_app_version(const zp_reader_t* reader, char* buf, size_t len);
int zp_reader_zip64(const zp_reader_t* reader);
void zp_reader_free(zp_reader_t* reader);

zp_sink_t* zp_sink_filesystem(const char* destination);
/* Discards output: extraction then only verifies CRCs (and hashes for restored audio). */
zp_sink_t* zp_sink_null(void);
void zp_sink_free(zp_sink_t* sink);

typedef struct zp_extract_options
{
    int restore_wav; /* default 1 */
    int include_readme; /* default 0 */
    int overwrite; /* ZP_OVERWRITE_*, default ASK */
} zp_extract_options_t;

void zp_extract_options_init(zp_extract_options_t* options);

typedef struct zp_extract_issue
{
    int kind;
    size_t entry;
    const char* name;
    const char* detail;
    int is_error;
} zp_extract_issue_t;

typedef struct zp_extract_item
{
    size_t entry;
    const char* target;
    int kind;
    int decision;
} zp_extract_item_t;

typedef struct zp_extract_outcome
{
    size_t entry;
    const char* target;
    int status;
    const char* message;
} zp_extract_outcome_t;

/* sel = NULL / n = 0: every entry. The plan keeps using reader and sink. */
zp_xplan_t* zp_extract_plan(zp_reader_t* reader, const size_t* sel, size_t n, zp_sink_t* sink, const zp_extract_options_t* options);
size_t zp_xplan_issue_count(const zp_xplan_t* xplan);
int zp_xplan_get_issue(const zp_xplan_t* xplan, size_t i, zp_extract_issue_t* out);
size_t zp_xplan_item_count(const zp_xplan_t* xplan);
int zp_xplan_get_item(const zp_xplan_t* xplan, size_t i, zp_extract_item_t* out);
/* ZP_DECISION_SKIP or ZP_DECISION_REPLACE for an item that exists at the destination. */
int zp_xplan_decide(zp_xplan_t* xplan, size_t item, int decision);
int zp_extract(zp_xplan_t* xplan, zp_progress_fn progress, void* user);
size_t zp_xplan_outcome_count(const zp_xplan_t* xplan);
int zp_xplan_get_outcome(const zp_xplan_t* xplan, size_t i, zp_extract_outcome_t* out);
void zp_xplan_free(zp_xplan_t* xplan);

/* ---- editing --------------------------------------------------------------------------- */

/* The editor keeps using `input`; free the editor before the stream. Entry indices refer to
 * the current zp_editor_plan() order. */
zp_editor_t* zp_editor_open(zp_context_t* ctx, zp_stream_t* input);
int zp_editor_add(zp_editor_t* editor, zp_input_t* input);
int zp_editor_remove(zp_editor_t* editor, size_t i);
int zp_editor_rename(zp_editor_t* editor, size_t i, const char* new_name);
/* Replaces entry i with the first file of `input`. */
int zp_editor_replace(zp_editor_t* editor, size_t i, zp_input_t* input);
zp_plan_t* zp_editor_plan(zp_editor_t* editor);
int zp_editor_commit(zp_editor_t* editor, zp_stream_t* output, zp_progress_fn progress, void* user);
void zp_editor_free(zp_editor_t* editor);

#ifdef __cplusplus
}
#endif

#endif
