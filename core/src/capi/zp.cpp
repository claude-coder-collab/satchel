// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "zp/zp.h"

#include "common/product.hpp"
#include "io/file_system.hpp"
#include "io/input_source.hpp"
#include "io/zip/builder.hpp"
#include "io/zip/editor.hpp"
#include "io/zip/extractor.hpp"
#include "io/zip/planner.hpp"
#include "io/zip/reader.hpp"
#include "pipeline/context.hpp"

#include <cstring>
#include <format>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

using namespace zp;

struct zp_context
{
    Context context;
    zp_context(int threads, std::uint64_t budget) :
        context(threads, budget)
    {
    }
};

struct zp_stream
{
    std::unique_ptr<IChunkedStream> stream;
    AtomicFileStream* atomic = nullptr;
    MemoryStream* memory = nullptr;
};

struct zp_input
{
    std::unique_ptr<InputSource> source;
    MemoryInputSource* memory = nullptr;
};

struct zp_plan
{
    Context* context = nullptr;
    ArchivePlan plan;
};

struct zp_build_result
{
    BuildResult result;
};

struct zp_reader
{
    Context* context = nullptr;
    std::unique_ptr<ArchiveReader> reader;
    std::string app_version;
};

struct zp_sink
{
    std::unique_ptr<OutputSink> sink;
};

struct zp_xplan
{
    Context* context = nullptr;
    zp_reader* reader = nullptr;
    zp_sink* sink = nullptr;
    ExtractOptions options;
    ExtractionPlan plan;
    ExtractResult result;
};

struct zp_editor
{
    Context* context = nullptr;
    std::unique_ptr<ArchiveEditor> editor;
};

namespace
{

thread_local std::string last_error;

int set_error(Status s, std::string_view message)
{
    last_error = message.empty() ? std::string(status_name(s)) : std::string(message);
    return static_cast<int>(s);
}

int set_error(const Error& e)
{
    return set_error(e.status, e.message);
}

template <typename F>
auto guarded(const F& f, decltype(f()) on_exception) noexcept -> decltype(f())
{
    try
    {
        return f();
    } catch (const std::bad_alloc&)
    {
        set_error(Status::Internal, "out of memory");
    } catch (const std::exception& e)
    {
        set_error(Status::Internal, e.what());
    } catch (...)
    {
        set_error(Status::Internal, "unknown exception");
    }
    return on_exception;
}

#define ZP_REQUIRE(cond, ret)                                               \
    do                                                                      \
    {                                                                       \
        if (!(cond))                                                        \
        {                                                                   \
            set_error(Status::InvalidArgument, "invalid argument: " #cond); \
            return ret;                                                     \
        }                                                                   \
    } while (false)

class CallbackStream final : public IChunkedStream
{
public:
    explicit CallbackStream(const zp_stream_callbacks_t& cb) :
        cb_(cb)
    {
    }

    Result<std::size_t> read(std::uint8_t* buf, std::size_t len) override
    {
        if (!cb_.read)
            return fail(Status::InvalidArgument, "stream is not readable");
        const auto n = cb_.read(cb_.user, buf, len);
        if (n < 0 || std::cmp_greater(n, len))
            return fail(Status::IoError, "read callback failed");
        pos_ += static_cast<std::uint64_t>(n);
        return static_cast<std::size_t>(n);
    }
    Result<std::size_t> write(const std::uint8_t* buf, std::size_t len) override
    {
        if (!cb_.write)
            return fail(Status::InvalidArgument, "stream is not writable");
        const auto n = cb_.write(cb_.user, buf, len);
        if (n < 0 || std::cmp_greater(n, len))
            return fail(Status::IoError, "write callback failed");
        pos_ += static_cast<std::uint64_t>(n);
        return static_cast<std::size_t>(n);
    }
    [[nodiscard]] bool seekable() const override { return cb_.seek != nullptr; }
    VoidResult seek(std::uint64_t pos) override
    {
        if (!cb_.seek)
            return fail(Status::InvalidArgument, "stream is not seekable");
        if (cb_.seek(cb_.user, pos) != 0)
            return fail(Status::IoError, "seek callback failed");
        pos_ = pos;
        return {};
    }
    [[nodiscard]] std::uint64_t tell() const override { return pos_; }
    [[nodiscard]] std::optional<std::uint64_t> size() const override
    {
        if (!cb_.size)
            return std::nullopt;
        const auto s = cb_.size(cb_.user);
        if (s < 0)
            return std::nullopt;
        return static_cast<std::uint64_t>(s);
    }
    VoidResult reopen() override { return seek(0); }

private:
    zp_stream_callbacks_t cb_;
    std::uint64_t pos_ = 0;
};

class CallbackProgress final : public ProgressSink
{
public:
    CallbackProgress(zp_progress_fn fn, void* user) :
        fn_(fn),
        user_(user)
    {
    }
    bool on_progress(std::uint64_t done, std::uint64_t total) override { return !fn_ || fn_(user_, done, total) == 0; }

private:
    zp_progress_fn fn_;
    void* user_;
};

int kind_code(ItemKind k)
{
    switch (k)
    {
        case ItemKind::Directory:
            return ZP_KIND_DIRECTORY;
        case ItemKind::Symlink:
            return ZP_KIND_SYMLINK;
        default:
            return ZP_KIND_FILE;
    }
}

}

extern "C" {

const char* zp_last_error(void)
{
    return last_error.c_str();
}

const char* zp_status_name(int status)
{
    if (status < 0 || status > static_cast<int>(Status::Internal))
        return "UNKNOWN";
    return status_name(static_cast<Status>(status)).data();
}

const char* zp_version(void)
{
    return product::version().data();
}

zp_context_t* zp_context_create(int threads, uint64_t memory_budget)
{
    return guarded([&]() -> zp_context_t* { return new zp_context(threads, memory_budget); }, nullptr);
}

void zp_context_free(zp_context_t* ctx)
{
    delete ctx;
}

zp_stream_t* zp_stream_open_file(const char* path)
{
    ZP_REQUIRE(path, nullptr);
    return guarded(
        [&]() -> zp_stream_t* {
            auto f = FileStream::open(path_from_utf8(path), FileMode::Read);
            if (!f)
            {
                set_error(f.error());
                return nullptr;
            }
            auto* s = new zp_stream();
            s->stream = std::move(*f);
            return s;
        },
        nullptr
    );
}

zp_stream_t* zp_stream_create_file(const char* path)
{
    ZP_REQUIRE(path, nullptr);
    return guarded(
        [&]() -> zp_stream_t* {
            auto f = AtomicFileStream::create(path_from_utf8(path));
            if (!f)
            {
                set_error(f.error());
                return nullptr;
            }
            auto* s = new zp_stream();
            s->atomic = f->get();
            s->stream = std::move(*f);
            return s;
        },
        nullptr
    );
}

int zp_stream_commit(zp_stream_t* stream)
{
    ZP_REQUIRE(stream && stream->atomic, ZP_INVALID_ARGUMENT);
    return guarded(
        [&]() -> int {
            if (auto r = stream->atomic->commit(); !r)
                return set_error(r.error());
            return ZP_OK;
        },
        ZP_INTERNAL
    );
}

zp_stream_t* zp_stream_memory(void)
{
    return zp_stream_memory_from(nullptr, 0);
}

zp_stream_t* zp_stream_memory_from(const uint8_t* data, size_t len)
{
    ZP_REQUIRE(data || len == 0, nullptr);
    return guarded(
        [&]() -> zp_stream_t* {
            auto m = std::make_unique<MemoryStream>(std::vector<std::uint8_t>(data, data + len));
            auto* s = new zp_stream();
            s->memory = m.get();
            s->stream = std::move(m);
            return s;
        },
        nullptr
    );
}

int zp_stream_memory_data(const zp_stream_t* stream, const uint8_t** data, size_t* len)
{
    ZP_REQUIRE(stream && stream->memory && data && len, ZP_INVALID_ARGUMENT);
    *data = stream->memory->data().data();
    *len = stream->memory->data().size();
    return ZP_OK;
}

zp_stream_t* zp_stream_from_callbacks(const zp_stream_callbacks_t* callbacks)
{
    ZP_REQUIRE(callbacks && (callbacks->read || callbacks->write), nullptr);
    return guarded(
        [&]() -> zp_stream_t* {
            auto* s = new zp_stream();
            s->stream = std::make_unique<CallbackStream>(*callbacks);
            return s;
        },
        nullptr
    );
}

void zp_stream_free(zp_stream_t* stream)
{
    delete stream;
}

zp_input_t* zp_input_from_paths(const char* const* paths, size_t count)
{
    ZP_REQUIRE(paths || count == 0, nullptr);
    return guarded(
        [&]() -> zp_input_t* {
            std::vector<std::filesystem::path> roots;
            for (size_t i = 0; i < count; ++i)
            {
                if (!paths[i])
                {
                    set_error(Status::InvalidArgument, "NULL path");
                    return nullptr;
                }
                roots.push_back(path_from_utf8(paths[i]));
            }
            auto* in = new zp_input();
            in->source = std::make_unique<FilesystemInputSource>(std::move(roots));
            return in;
        },
        nullptr
    );
}

zp_input_t* zp_input_memory(void)
{
    return guarded(
        [&]() -> zp_input_t* {
            auto m = std::make_unique<MemoryInputSource>();
            auto* in = new zp_input();
            in->memory = m.get();
            in->source = std::move(m);
            return in;
        },
        nullptr
    );
}

int zp_input_memory_add_file(zp_input_t* input, const char* archive_path, const uint8_t* data, size_t len, int64_t mtime, uint32_t unix_mode)
{
    ZP_REQUIRE(input && input->memory && archive_path && (data || len == 0), ZP_INVALID_ARGUMENT);
    return guarded(
        [&]() -> int {
            input->memory->add_file(archive_path, std::vector<std::uint8_t>(data, data + len), mtime * 1'000'000'000, unix_mode);
            return ZP_OK;
        },
        ZP_INTERNAL
    );
}

int zp_input_memory_add_directory(zp_input_t* input, const char* archive_path, int64_t mtime, uint32_t unix_mode)
{
    ZP_REQUIRE(input && input->memory && archive_path, ZP_INVALID_ARGUMENT);
    return guarded(
        [&]() -> int {
            input->memory->add_directory(archive_path, mtime * 1'000'000'000, unix_mode);
            return ZP_OK;
        },
        ZP_INTERNAL
    );
}

void zp_input_free(zp_input_t* input)
{
    delete input;
}

void zp_plan_options_init(zp_plan_options_t* options)
{
    if (!options)
        return;
    const PlannerOptions d;
    options->flac_enabled = d.flac_enabled ? 1 : 0;
    options->deflate_level = d.deflate_level;
    options->flac_level = d.flac_level;
}

zp_plan_t* zp_plan_create(zp_context_t* ctx, zp_input_t* input, const zp_plan_options_t* options)
{
    ZP_REQUIRE(ctx && input, nullptr);
    return guarded(
        [&]() -> zp_plan_t* {
            PlannerOptions o;
            if (options)
            {
                o.flac_enabled = options->flac_enabled != 0;
                o.deflate_level = options->deflate_level;
                o.flac_level = options->flac_level;
            }
            if (o.deflate_level < 1 || o.deflate_level > 9 || o.flac_level < 0 || o.flac_level > 8)
            {
                set_error(Status::InvalidArgument, "deflate level must be 1-9 and FLAC level 0-8");
                return nullptr;
            }
            auto plan = ArchivePlanner(o).plan(*input->source);
            if (!plan)
            {
                set_error(plan.error());
                return nullptr;
            }
            auto* p = new zp_plan();
            p->context = &ctx->context;
            p->plan = std::move(*plan);
            return p;
        },
        nullptr
    );
}

size_t zp_plan_entry_count(const zp_plan_t* plan)
{
    return plan ? plan->plan.entries.size() : 0;
}

int zp_plan_get_entry(const zp_plan_t* plan, size_t i, zp_plan_entry_t* out)
{
    ZP_REQUIRE(plan && out && i < plan->plan.entries.size(), ZP_INVALID_ARGUMENT);
    const auto& e = plan->plan.entries[i];
    *out = {};
    out->source_path = e.item.source_path.c_str();
    out->output_name = e.output_name.c_str();
    out->kind = kind_code(e.item.kind);
    out->codec = static_cast<int>(e.codec);
    out->size = e.item.size;
    out->mtime = e.item.mtime_seconds();
    out->unix_mode = e.item.unix_mode;
    out->has_group = e.group_id ? 1 : 0;
    if (e.group_id)
        std::memcpy(out->group_id, e.group_id->data(), 16);
    out->channel_index = e.channel_index.value_or(0);
    out->fallback_reason = e.flac_fallback_reason ? static_cast<int>(*e.flac_fallback_reason) : -1;
    return ZP_OK;
}

size_t zp_plan_conflict_count(const zp_plan_t* plan)
{
    return plan ? plan->plan.conflicts.size() : 0;
}

int zp_plan_get_conflict(const zp_plan_t* plan, size_t i, zp_conflict_t* out)
{
    ZP_REQUIRE(plan && out && i < plan->plan.conflicts.size(), ZP_INVALID_ARGUMENT);
    const auto& c = plan->plan.conflicts[i];
    out->kind = static_cast<int>(c.kind);
    out->collision_key = c.collision_key.c_str();
    out->detail = c.detail.c_str();
    out->entries = c.entries.data();
    out->entry_count = c.entries.size();
    return ZP_OK;
}

size_t zp_plan_warning_count(const zp_plan_t* plan)
{
    return plan ? plan->plan.warnings.size() : 0;
}

int zp_plan_get_warning(const zp_plan_t* plan, size_t i, zp_warning_t* out)
{
    ZP_REQUIRE(plan && out && i < plan->plan.warnings.size(), ZP_INVALID_ARGUMENT);
    const auto& w = plan->plan.warnings[i];
    out->kind = static_cast<int>(w.kind);
    out->source_path = w.source_path.c_str();
    out->detail = w.detail.c_str();
    return ZP_OK;
}

int zp_plan_resolve(zp_plan_t* plan, const zp_resolution_t* resolutions, size_t count)
{
    ZP_REQUIRE(plan && (resolutions || count == 0), ZP_INVALID_ARGUMENT);
    return guarded(
        [&]() -> int {
            std::vector<Resolution> list;
            for (size_t i = 0; i < count; ++i)
            {
                const auto& r = resolutions[i];
                if (r.action < ZP_RESOLVE_RENAME || r.action > ZP_RESOLVE_DISABLE_FLAC)
                    return set_error(Status::InvalidArgument, "unknown resolution action");
                if (r.action == ZP_RESOLVE_RENAME && !r.new_name)
                    return set_error(Status::InvalidArgument, "rename needs a new name");
                list.push_back({ r.entry, static_cast<ResolutionAction>(r.action), r.new_name ? r.new_name : "" });
            }
            auto next = ArchivePlanner::replan(plan->plan, list);
            if (!next)
                return set_error(next.error());
            plan->plan = std::move(*next);
            return ZP_OK;
        },
        ZP_INTERNAL
    );
}

int zp_plan_executable(const zp_plan_t* plan)
{
    return plan && plan->plan.executable() ? 1 : 0;
}

uint64_t zp_plan_total_bytes(const zp_plan_t* plan)
{
    return plan ? plan->plan.total_input_bytes() : 0;
}

void zp_plan_free(zp_plan_t* plan)
{
    delete plan;
}

int zp_build(zp_plan_t* plan, zp_stream_t* output, const zp_build_options_t* options, zp_progress_fn progress, void* user, zp_build_result_t** out_result)
{
    ZP_REQUIRE(plan && output, ZP_INVALID_ARGUMENT);
    if (out_result)
        *out_result = nullptr;
    return guarded(
        [&]() -> int {
            BuilderOptions o;
            if (options && options->small_entry_threshold)
                o.small_entry_threshold = options->small_entry_threshold;
            ArchiveBuilder builder(*output->stream, *plan->context, o);
            CallbackProgress p(progress, user);
            auto result = std::make_unique<zp_build_result>();
            result->result = builder.execute(plan->plan, p);
            const auto status = result->result.status;
            if (status != Status::Ok)
                set_error(status, result->result.message);
            if (out_result)
                *out_result = result.release();
            return static_cast<int>(status);
        },
        ZP_INTERNAL
    );
}

size_t zp_build_result_entry_count(const zp_build_result_t* result)
{
    return result ? result->result.per_entry.size() : 0;
}

int zp_build_result_get_entry(const zp_build_result_t* result, size_t i, zp_entry_result_t* out)
{
    ZP_REQUIRE(result && out && i < result->result.per_entry.size(), ZP_INVALID_ARGUMENT);
    const auto& e = result->result.per_entry[i];
    out->plan_index = e.plan_index;
    out->name = e.name.c_str();
    out->method = static_cast<int>(e.method);
    out->compressed_size = e.compressed_size;
    out->uncompressed_size = e.uncompressed_size;
    out->crc32 = e.crc32;
    out->status = static_cast<int>(e.status);
    out->message = e.message.c_str();
    return ZP_OK;
}

int zp_build_result_zip64(const zp_build_result_t* result)
{
    return result && result->result.zip64 ? 1 : 0;
}

void zp_build_result_free(zp_build_result_t* result)
{
    delete result;
}

zp_reader_t* zp_reader_open(zp_context_t* ctx, zp_stream_t* input)
{
    ZP_REQUIRE(ctx && input, nullptr);
    return guarded(
        [&]() -> zp_reader_t* {
            auto r = ArchiveReader::open(*input->stream);
            if (!r)
            {
                set_error(r.error());
                return nullptr;
            }
            auto* reader = new zp_reader();
            reader->context = &ctx->context;
            reader->reader = std::move(*r);
            if (const auto& m = reader->reader->metadata())
                reader->app_version = m->app_version;
            return reader;
        },
        nullptr
    );
}

size_t zp_reader_entry_count(const zp_reader_t* reader)
{
    return reader ? reader->reader->entries().size() : 0;
}

int zp_reader_get_entry(const zp_reader_t* reader, size_t i, zp_entry_info_t* out)
{
    ZP_REQUIRE(reader && out && i < reader->reader->entries().size(), ZP_INVALID_ARGUMENT);
    const auto& e = reader->reader->entries()[i];
    *out = {};
    out->name = e.name.c_str();
    out->kind = kind_code(e.kind);
    out->method = e.raw_method;
    out->supported = e.method != EntryMethod::Unsupported ? 1 : 0;
    out->compressed_size = e.compressed_size;
    out->uncompressed_size = e.uncompressed_size;
    out->crc32 = e.crc32;
    out->mtime = e.mtime;
    out->has_unix_mode = e.unix_mode ? 1 : 0;
    out->unix_mode = e.unix_mode.value_or(0);
    out->flac_restorable = e.flac_restorable ? 1 : 0;
    out->has_flac_group = e.flac_group ? 1 : 0;
    if (e.flac_group)
    {
        std::memcpy(out->flac_group_id, e.flac_group->group_id.data(), 16);
        out->flac_channel_index = e.flac_group->channel_index;
        out->flac_channel_count = e.flac_group->channel_count;
    }
    return ZP_OK;
}

int zp_reader_get_app_version(const zp_reader_t* reader, char* buf, size_t len)
{
    ZP_REQUIRE(reader && buf && len > 0, ZP_INVALID_ARGUMENT);
    if (!reader->reader->metadata())
    {
        buf[0] = '\0';
        return set_error(Status::InvalidArgument, "archive was not created by this library");
    }
    const auto& v = reader->app_version;
    const auto n = std::min(len - 1, v.size());
    std::memcpy(buf, v.data(), n);
    buf[n] = '\0';
    return ZP_OK;
}

int zp_reader_zip64(const zp_reader_t* reader)
{
    return reader && reader->reader->zip64() ? 1 : 0;
}

void zp_reader_free(zp_reader_t* reader)
{
    delete reader;
}

zp_sink_t* zp_sink_filesystem(const char* destination)
{
    ZP_REQUIRE(destination, nullptr);
    return guarded(
        [&]() -> zp_sink_t* {
            auto* s = new zp_sink();
            s->sink = std::make_unique<FilesystemOutputSink>(path_from_utf8(destination));
            return s;
        },
        nullptr
    );
}

zp_sink_t* zp_sink_null(void)
{
    return guarded(
        [&]() -> zp_sink_t* {
            auto* s = new zp_sink();
            s->sink = std::make_unique<NullOutputSink>();
            return s;
        },
        nullptr
    );
}

void zp_sink_free(zp_sink_t* sink)
{
    delete sink;
}

void zp_extract_options_init(zp_extract_options_t* options)
{
    if (!options)
        return;
    const ExtractOptions d;
    options->restore_wav = d.restore_wav ? 1 : 0;
    options->include_readme = d.include_readme ? 1 : 0;
    options->overwrite = static_cast<int>(d.overwrite);
}

zp_xplan_t* zp_extract_plan(zp_reader_t* reader, const size_t* sel, size_t n, zp_sink_t* sink, const zp_extract_options_t* options)
{
    ZP_REQUIRE(reader && sink && (sel || n == 0), nullptr);
    return guarded(
        [&]() -> zp_xplan_t* {
            ExtractOptions o;
            if (options)
            {
                if (options->overwrite < ZP_OVERWRITE_ASK || options->overwrite > ZP_OVERWRITE_REPLACE)
                {
                    set_error(Status::InvalidArgument, "unknown overwrite policy");
                    return nullptr;
                }
                o.restore_wav = options->restore_wav != 0;
                o.include_readme = options->include_readme != 0;
                o.overwrite = static_cast<OverwritePolicy>(options->overwrite);
            }
            ArchiveExtractor extractor(*reader->reader, *sink->sink, *reader->context, o);
            auto plan = extractor.plan_extraction(std::vector<std::size_t>(sel, sel + n));
            if (!plan)
            {
                set_error(plan.error());
                return nullptr;
            }
            auto* x = new zp_xplan();
            x->context = reader->context;
            x->reader = reader;
            x->sink = sink;
            x->options = o;
            x->plan = std::move(*plan);
            return x;
        },
        nullptr
    );
}

size_t zp_xplan_issue_count(const zp_xplan_t* xplan)
{
    return xplan ? xplan->plan.issues.size() : 0;
}

int zp_xplan_get_issue(const zp_xplan_t* xplan, size_t i, zp_extract_issue_t* out)
{
    ZP_REQUIRE(xplan && out && i < xplan->plan.issues.size(), ZP_INVALID_ARGUMENT);
    const auto& issue = xplan->plan.issues[i];
    out->kind = static_cast<int>(issue.kind);
    out->entry = issue.entry;
    out->name = issue.name.c_str();
    out->detail = issue.detail.c_str();
    out->is_error = issue.is_error() ? 1 : 0;
    return ZP_OK;
}

size_t zp_xplan_item_count(const zp_xplan_t* xplan)
{
    return xplan ? xplan->plan.items.size() : 0;
}

int zp_xplan_get_item(const zp_xplan_t* xplan, size_t i, zp_extract_item_t* out)
{
    ZP_REQUIRE(xplan && out && i < xplan->plan.items.size(), ZP_INVALID_ARGUMENT);
    const auto& item = xplan->plan.items[i];
    out->entry = item.entry;
    out->target = item.target.c_str();
    out->kind = kind_code(item.kind);
    out->decision = static_cast<int>(item.decision);
    return ZP_OK;
}

int zp_xplan_decide(zp_xplan_t* xplan, size_t item, int decision)
{
    ZP_REQUIRE(xplan, ZP_INVALID_ARGUMENT);
    ZP_REQUIRE(decision == ZP_DECISION_SKIP || decision == ZP_DECISION_REPLACE, ZP_INVALID_ARGUMENT);
    if (auto r = xplan->plan.decide(item, static_cast<ItemDecision>(decision)); !r)
        return set_error(r.error());
    return ZP_OK;
}

int zp_extract(zp_xplan_t* xplan, zp_progress_fn progress, void* user)
{
    ZP_REQUIRE(xplan, ZP_INVALID_ARGUMENT);
    return guarded(
        [&]() -> int {
            ArchiveExtractor extractor(*xplan->reader->reader, *xplan->sink->sink, *xplan->context, xplan->options);
            CallbackProgress p(progress, user);
            xplan->result = extractor.execute(xplan->plan, p);
            if (xplan->result.status != Status::Ok)
                return set_error(xplan->result.status, xplan->result.message);
            return ZP_OK;
        },
        ZP_INTERNAL
    );
}

size_t zp_xplan_outcome_count(const zp_xplan_t* xplan)
{
    return xplan ? xplan->result.outcomes.size() : 0;
}

int zp_xplan_get_outcome(const zp_xplan_t* xplan, size_t i, zp_extract_outcome_t* out)
{
    ZP_REQUIRE(xplan && out && i < xplan->result.outcomes.size(), ZP_INVALID_ARGUMENT);
    const auto& o = xplan->result.outcomes[i];
    out->entry = o.entry;
    out->target = o.target.c_str();
    out->status = static_cast<int>(o.status);
    out->message = o.message.c_str();
    return ZP_OK;
}

void zp_xplan_free(zp_xplan_t* xplan)
{
    delete xplan;
}

zp_editor_t* zp_editor_open(zp_context_t* ctx, zp_stream_t* input)
{
    ZP_REQUIRE(ctx && input, nullptr);
    return guarded(
        [&]() -> zp_editor_t* {
            auto ed = std::make_unique<ArchiveEditor>(ctx->context);
            if (auto r = ed->open(*input->stream); !r)
            {
                set_error(r.error());
                return nullptr;
            }
            auto* e = new zp_editor();
            e->context = &ctx->context;
            e->editor = std::move(ed);
            return e;
        },
        nullptr
    );
}

int zp_editor_add(zp_editor_t* editor, zp_input_t* input)
{
    ZP_REQUIRE(editor && input, ZP_INVALID_ARGUMENT);
    return guarded(
        [&]() -> int {
            if (auto r = editor->editor->add(*input->source); !r)
                return set_error(r.error());
            return ZP_OK;
        },
        ZP_INTERNAL
    );
}

int zp_editor_remove(zp_editor_t* editor, size_t i)
{
    ZP_REQUIRE(editor, ZP_INVALID_ARGUMENT);
    if (auto r = editor->editor->remove(i); !r)
        return set_error(r.error());
    return ZP_OK;
}

int zp_editor_rename(zp_editor_t* editor, size_t i, const char* new_name)
{
    ZP_REQUIRE(editor && new_name, ZP_INVALID_ARGUMENT);
    return guarded(
        [&]() -> int {
            if (auto r = editor->editor->rename(i, new_name); !r)
                return set_error(r.error());
            return ZP_OK;
        },
        ZP_INTERNAL
    );
}

int zp_editor_replace(zp_editor_t* editor, size_t i, zp_input_t* input)
{
    ZP_REQUIRE(editor && input, ZP_INVALID_ARGUMENT);
    return guarded(
        [&]() -> int {
            auto items = input->source->enumerate();
            if (!items)
                return set_error(items.error());
            for (auto& item : *items)
            {
                if (item.kind != ItemKind::File)
                    continue;
                if (auto r = editor->editor->replace(i, std::move(item)); !r)
                    return set_error(r.error());
                return ZP_OK;
            }
            return set_error(Status::InvalidArgument, "input contains no file");
        },
        ZP_INTERNAL
    );
}

zp_plan_t* zp_editor_plan(zp_editor_t* editor)
{
    ZP_REQUIRE(editor, nullptr);
    return guarded(
        [&]() -> zp_plan_t* {
            auto* p = new zp_plan();
            p->context = editor->context;
            p->plan = editor->editor->plan();
            return p;
        },
        nullptr
    );
}

int zp_editor_commit(zp_editor_t* editor, zp_stream_t* output, zp_progress_fn progress, void* user)
{
    ZP_REQUIRE(editor && output, ZP_INVALID_ARGUMENT);
    return guarded(
        [&]() -> int {
            CallbackProgress p(progress, user);
            const auto r = editor->editor->commit(*output->stream, p);
            if (r.status != Status::Ok)
                return set_error(r.status, r.message);
            return ZP_OK;
        },
        ZP_INTERNAL
    );
}

void zp_editor_free(zp_editor_t* editor)
{
    delete editor;
}
}
