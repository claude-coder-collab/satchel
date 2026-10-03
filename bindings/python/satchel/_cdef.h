/* SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
 * Generated from core/include/zp/zp.h by tools/gen_cdef.py. Do not edit. */
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
enum {
    ZP_KIND_FILE = 0,
    ZP_KIND_DIRECTORY = 1,
    ZP_KIND_SYMLINK = 2
};
enum {
    ZP_CODEC_GENERAL = 0,
    ZP_CODEC_FLAC = 1,
    ZP_CODEC_FLAC_MONO = 2,
    ZP_CODEC_GENERATED = 3,
    ZP_CODEC_KEPT = 4
};
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
typedef int (*zp_progress_fn)(void* user, uint64_t done, uint64_t total);
const char* zp_last_error(void);
int zp_last_status(void);
const char* zp_status_name(int status);
const char* zp_version(void);
zp_context_t* zp_context_create(int threads, uint64_t memory_budget);
void zp_context_free(zp_context_t* ctx);
zp_stream_t* zp_stream_open_file(const char* path);
zp_stream_t* zp_stream_create_file(const char* path);
int zp_stream_commit(zp_stream_t* stream);
zp_stream_t* zp_stream_memory(void);
zp_stream_t* zp_stream_memory_from(const uint8_t* data, size_t len);
int zp_stream_memory_data(const zp_stream_t* stream, const uint8_t** data, size_t* len);
typedef struct zp_stream_callbacks
{
    void* user;
    int64_t (*read)(void* user, uint8_t* buf, size_t len);
    int64_t (*write)(void* user, const uint8_t* buf, size_t len);
    int (*seek)(void* user, uint64_t pos);
    int64_t (*size)(void* user);
} zp_stream_callbacks_t;
zp_stream_t* zp_stream_from_callbacks(const zp_stream_callbacks_t* callbacks);
void zp_stream_free(zp_stream_t* stream);
zp_input_t* zp_input_from_paths(const char* const* paths, size_t count);
zp_input_t* zp_input_memory(void);
int zp_input_memory_add_file(zp_input_t* input, const char* archive_path, const uint8_t* data, size_t len, int64_t mtime, uint32_t unix_mode);
int zp_input_memory_add_directory(zp_input_t* input, const char* archive_path, int64_t mtime, uint32_t unix_mode);
void zp_input_free(zp_input_t* input);
typedef struct zp_plan_options
{
    int flac_enabled;
    int deflate_level;
    int flac_level;
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
    uint16_t channel_count;
    int fallback_reason;
    const char* fallback_detail;
    const char* restored_name;
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
    const char* new_name;
} zp_resolution_t;
zp_plan_t* zp_plan_create(zp_context_t* ctx, zp_input_t* input, const zp_plan_options_t* options);
size_t zp_plan_entry_count(const zp_plan_t* plan);
int zp_plan_get_entry(const zp_plan_t* plan, size_t i, zp_plan_entry_t* out);
size_t zp_plan_conflict_count(const zp_plan_t* plan);
int zp_plan_get_conflict(const zp_plan_t* plan, size_t i, zp_conflict_t* out);
size_t zp_plan_warning_count(const zp_plan_t* plan);
int zp_plan_get_warning(const zp_plan_t* plan, size_t i, zp_warning_t* out);
int zp_plan_resolve(zp_plan_t* plan, const zp_resolution_t* resolutions, size_t count);
int zp_plan_executable(const zp_plan_t* plan);
uint64_t zp_plan_total_bytes(const zp_plan_t* plan);
void zp_plan_free(zp_plan_t* plan);
typedef struct zp_build_options
{
    uint64_t small_entry_threshold;
    const char* readme_template;
    const char* temp_dir;
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
const char* zp_default_readme_template(void);
int zp_build(zp_plan_t* plan, zp_stream_t* output, const zp_build_options_t* options, zp_progress_fn progress, void* user, zp_build_result_t** out_result);
size_t zp_build_result_entry_count(const zp_build_result_t* result);
int zp_build_result_get_entry(const zp_build_result_t* result, size_t i, zp_entry_result_t* out);
int zp_build_result_zip64(const zp_build_result_t* result);
void zp_build_result_free(zp_build_result_t* result);
typedef struct zp_entry_info
{
    const char* name;
    int kind;
    int method;
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
zp_reader_t* zp_reader_open(zp_context_t* ctx, zp_stream_t* input);
size_t zp_reader_entry_count(const zp_reader_t* reader);
int zp_reader_get_entry(const zp_reader_t* reader, size_t i, zp_entry_info_t* out);
int zp_reader_get_app_version(const zp_reader_t* reader, char* buf, size_t len);
int zp_reader_zip64(const zp_reader_t* reader);
void zp_reader_free(zp_reader_t* reader);
zp_sink_t* zp_sink_filesystem(const char* destination);
zp_sink_t* zp_sink_null(void);
void zp_sink_free(zp_sink_t* sink);
typedef struct zp_extract_options
{
    int restore_wav;
    int include_readme;
    int overwrite;
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
zp_xplan_t* zp_extract_plan(zp_reader_t* reader, const size_t* sel, size_t n, zp_sink_t* sink, const zp_extract_options_t* options);
size_t zp_xplan_issue_count(const zp_xplan_t* xplan);
int zp_xplan_get_issue(const zp_xplan_t* xplan, size_t i, zp_extract_issue_t* out);
size_t zp_xplan_item_count(const zp_xplan_t* xplan);
int zp_xplan_get_item(const zp_xplan_t* xplan, size_t i, zp_extract_item_t* out);
int zp_xplan_decide(zp_xplan_t* xplan, size_t item, int decision);
int zp_extract(zp_xplan_t* xplan, zp_progress_fn progress, void* user);
size_t zp_xplan_outcome_count(const zp_xplan_t* xplan);
int zp_xplan_get_outcome(const zp_xplan_t* xplan, size_t i, zp_extract_outcome_t* out);
void zp_xplan_free(zp_xplan_t* xplan);
zp_editor_t* zp_editor_open(zp_context_t* ctx, zp_stream_t* input);
int zp_editor_add(zp_editor_t* editor, zp_input_t* input);
int zp_editor_remove(zp_editor_t* editor, size_t i);
int zp_editor_rename(zp_editor_t* editor, size_t i, const char* new_name);
int zp_editor_replace(zp_editor_t* editor, size_t i, zp_input_t* input);
zp_plan_t* zp_editor_plan(zp_editor_t* editor);
int zp_editor_commit(zp_editor_t* editor, zp_stream_t* output, zp_progress_fn progress, void* user);
void zp_editor_free(zp_editor_t* editor);
