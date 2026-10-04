// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "zp/zp.h"

#include "codecs/flac/restore.hpp"
#include "common/json_writer.hpp"
#include "common/main_thread.hpp"
#include "common/product.hpp"
#include "crypto/hash.hpp"
#include "io/file_system.hpp"
#include "io/input_source.hpp"
#include "io/zip/builder.hpp"
#include "io/zip/editor.hpp"
#include "io/zip/extractor.hpp"
#include "io/zip/planner.hpp"
#include "io/zip/reader.hpp"
#include "io/zip/readme.hpp"
#include "pipeline/context.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <string_view>
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
    mutable std::vector<std::string> restored_names;
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
    mutable std::vector<std::string> original_names;
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
thread_local int last_status = 0;

int set_error(Status s, std::string_view message)
{
    last_status = static_cast<int>(s);
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
        std::int64_t n = -1;
        run_on_main_thread([&] { n = cb_.read(cb_.user, buf, len); });
        if (n < 0 || std::cmp_greater(n, len))
            return fail(Status::IoError, "read callback failed");
        pos_ += static_cast<std::uint64_t>(n);
        return static_cast<std::size_t>(n);
    }
    Result<std::size_t> write(const std::uint8_t* buf, std::size_t len) override
    {
        if (!cb_.write)
            return fail(Status::InvalidArgument, "stream is not writable");
        std::int64_t n = -1;
        run_on_main_thread([&] { n = cb_.write(cb_.user, buf, len); });
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
        int rc = -1;
        run_on_main_thread([&] { rc = cb_.seek(cb_.user, pos); });
        if (rc != 0)
            return fail(Status::IoError, "seek callback failed");
        pos_ = pos;
        return {};
    }
    [[nodiscard]] std::uint64_t tell() const override { return pos_; }
    [[nodiscard]] std::optional<std::uint64_t> size() const override
    {
        if (!cb_.size)
            return std::nullopt;
        std::int64_t s = -1;
        run_on_main_thread([&] { s = cb_.size(cb_.user); });
        if (s < 0)
            return std::nullopt;
        return static_cast<std::uint64_t>(s);
    }
    VoidResult reopen() override { return seek(0); }

private:
    zp_stream_callbacks_t cb_;
    std::uint64_t pos_ = 0;
};

class CallbackItemStream final : public IChunkedStream
{
public:
    CallbackItemStream(zp_read_fn read_fn, void* user, std::size_t item, std::uint64_t item_size) :
        read_(read_fn),
        user_(user),
        item_(item),
        size_(item_size)
    {
    }

    Result<std::size_t> read(std::uint8_t* buf, std::size_t len) override
    {
        const auto want = static_cast<std::size_t>(std::min<std::uint64_t>(len, size_ > pos_ ? size_ - pos_ : 0));
        if (want == 0)
            return 0;
        std::int64_t n = -1;
        run_on_main_thread([&] { n = read_(user_, item_, pos_, buf, want); });
        if (n < 0 || std::cmp_greater(n, want))
            return fail(Status::IoError, "read callback failed");
        pos_ += static_cast<std::uint64_t>(n);
        return static_cast<std::size_t>(n);
    }
    [[nodiscard]] bool seekable() const override { return true; }
    VoidResult seek(std::uint64_t pos) override
    {
        pos_ = pos;
        return {};
    }
    [[nodiscard]] std::uint64_t tell() const override { return pos_; }
    [[nodiscard]] std::optional<std::uint64_t> size() const override { return size_; }
    VoidResult reopen() override { return seek(0); }

private:
    zp_read_fn read_;
    void* user_;
    std::size_t item_;
    std::uint64_t size_;
    std::uint64_t pos_ = 0;
};

class CallbackInputSource final : public InputSource
{
public:
    CallbackInputSource(const zp_input_item_t* items, std::size_t count, zp_read_fn read_fn, void* user) :
        read_(read_fn),
        user_(user)
    {
        for (std::size_t i = 0; i < count; ++i)
        {
            InputItem item;
            item.archive_path = items[i].archive_path ? items[i].archive_path : "";
            item.source_path = "input:" + item.archive_path;
            item.kind = items[i].kind == ZP_KIND_DIRECTORY ? ItemKind::Directory : ItemKind::File;
            item.size = item.kind == ItemKind::File ? items[i].size : 0;
            item.mtime_ns = items[i].mtime * 1'000'000'000;
            item.unix_mode = items[i].unix_mode ? items[i].unix_mode : (item.kind == ItemKind::Directory ? 0755u : 0644u);
            if (item.kind == ItemKind::File)
            {
                const auto size = item.size;
                item.opener = [this, i, size]() -> Result<std::unique_ptr<IChunkedStream>> {
                    return std::make_unique<CallbackItemStream>(read_, user_, i, size);
                };
            }
            items_.push_back(std::move(item));
        }
    }

    Result<std::vector<InputItem>> enumerate() override { return items_; }

private:
    zp_read_fn read_;
    void* user_;
    std::vector<InputItem> items_;
};

class CallbackOutputFile final : public OutputFile
{
public:
    CallbackOutputFile(const zp_sink_callbacks_t& cb, std::int64_t handle) :
        cb_(cb),
        handle_(handle)
    {
    }
    CallbackOutputFile(const CallbackOutputFile&) = delete;
    CallbackOutputFile& operator=(const CallbackOutputFile&) = delete;
    CallbackOutputFile(CallbackOutputFile&&) = delete;
    CallbackOutputFile& operator=(CallbackOutputFile&&) = delete;
    ~CallbackOutputFile() override
    {
        if (!done_ && cb_.discard)
            run_on_main_thread([&] { cb_.discard(cb_.user, handle_); });
    }

    VoidResult write(std::span<const std::uint8_t> data) override
    {
        int rc = -1;
        run_on_main_thread([&] { rc = cb_.write(cb_.user, handle_, data.data(), data.size()); });
        if (rc != 0)
            return fail(Status::IoError, "write callback failed");
        return {};
    }

    VoidResult commit(std::int64_t mtime, std::optional<std::uint32_t> unix_mode) override
    {
        int rc = -1;
        run_on_main_thread([&] { rc = cb_.commit ? cb_.commit(cb_.user, handle_, mtime, unix_mode.value_or(0), unix_mode ? 1 : 0) : 0; });
        done_ = true;
        if (rc != 0)
            return fail(Status::IoError, "commit callback failed");
        return {};
    }

private:
    zp_sink_callbacks_t cb_;
    std::int64_t handle_;
    bool done_ = false;
};

class CallbackSink final : public OutputSink
{
public:
    explicit CallbackSink(const zp_sink_callbacks_t& cb) :
        cb_(cb)
    {
    }

    bool exists(const std::string& path) override
    {
        int rc = 0;
        if (cb_.exists)
            run_on_main_thread([&] { rc = cb_.exists(cb_.user, path.c_str()); });
        return rc != 0;
    }
    VoidResult make_directory(const std::string& path) override
    {
        int rc = 0;
        if (cb_.make_directory)
            run_on_main_thread([&] { rc = cb_.make_directory(cb_.user, path.c_str()); });
        if (rc != 0)
            return fail(Status::IoError, "cannot create folder '" + path + "'");
        return {};
    }
    VoidResult set_directory_attributes(const std::string&, std::int64_t, std::optional<std::uint32_t>) override { return {}; }
    Result<std::unique_ptr<OutputFile>> create_file(const std::string& path, bool replace) override
    {
        std::int64_t handle = -1;
        run_on_main_thread([&] { handle = cb_.open_file(cb_.user, path.c_str(), replace ? 1 : 0); });
        if (handle < 0)
            return fail(Status::IoError, "cannot create '" + path + "'");
        return std::make_unique<CallbackOutputFile>(cb_, handle);
    }

private:
    zp_sink_callbacks_t cb_;
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

const char* kind_name(ItemKind k)
{
    switch (k)
    {
        case ItemKind::Directory:
            return "directory";
        case ItemKind::Symlink:
            return "symlink";
        default:
            return "file";
    }
}

const char* codec_text(PlanCodec c)
{
    switch (c)
    {
        case PlanCodec::Flac:
            return "flac";
        case PlanCodec::FlacMono:
            return "flac_mono";
        case PlanCodec::Generated:
            return "generated";
        case PlanCodec::Kept:
            return "kept";
        default:
            return "general";
    }
}

const char* issue_text(ExtractIssueKind k)
{
    switch (k)
    {
        case ExtractIssueKind::SymlinkSkipped:
            return "symlink_skipped";
        case ExtractIssueKind::UnsupportedMethod:
            return "unsupported_method";
        case ExtractIssueKind::UnsafePath:
            return "unsafe_path";
        case ExtractIssueKind::NameCollision:
            return "name_collision";
        case ExtractIssueKind::IncompleteGroup:
            return "incomplete_group";
        case ExtractIssueKind::ExistsAtDestination:
            return "exists_at_destination";
    }
    return "unknown";
}

char* dup_string(const std::string& s)
{
    auto* p = static_cast<char*>(std::malloc(s.size() + 1));
    if (!p)
        return nullptr;
    std::memcpy(p, s.data(), s.size() + 1);
    return p;
}

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

char* zp_plan_describe(const zp_plan_t* plan)
{
    ZP_REQUIRE(plan, nullptr);
    return guarded(
        [&]() -> char* {
            JsonWriter j;
            j.begin_object().field("executable", plan->plan.executable()).field("total_bytes", plan->plan.total_input_bytes());
            j.key("entries").begin_array();
            for (const auto& e : plan->plan.entries)
            {
                j.begin_object().field("source_path", e.item.source_path).field("output_name", e.output_name).field("kind", kind_name(e.item.kind));
                j.field("codec", codec_text(e.codec)).field("size", e.item.size).field("mtime", e.item.mtime_seconds());
                if (e.is_converted())
                    j.field("restored_name", e.restored_name());
                if (e.channel_index)
                    j.field("channel_index", static_cast<std::uint64_t>(*e.channel_index)).field("channel_count", static_cast<std::uint64_t>(e.channel_count));
                if (e.flac_fallback_reason)
                    j.field("fallback", std::string(fallback_reason_text(*e.flac_fallback_reason)) + " (" + e.flac_fallback_detail + ")");
                j.end_object();
            }
            j.end_array().key("conflicts").begin_array();
            for (const auto& c : plan->plan.conflicts)
            {
                j.begin_object().field("kind", c.kind == ConflictKind::Collision ? "collision" : "invalid_name").field("key", c.collision_key).field("detail", c.detail);
                j.key("entries").begin_array();
                for (const auto i : c.entries)
                    j.value(static_cast<std::uint64_t>(i));
                j.end_array().end_object();
            }
            j.end_array().key("warnings").begin_array();
            for (const auto& w : plan->plan.warnings)
                j.begin_object().field("kind", w.kind == WarningKind::SymlinkSkipped ? "symlink_skipped" : "flac_fallback").field("source_path", w.source_path).field("detail", w.detail).end_object();
            j.end_array().end_object();
            return dup_string(j.str());
        },
        nullptr
    );
}

char* zp_build_result_describe(const zp_build_result_t* result)
{
    ZP_REQUIRE(result, nullptr);
    return guarded(
        [&]() -> char* {
            JsonWriter j;
            j.begin_object().field("status", status_name(result->result.status)).field("message", result->result.message).field("zip64", result->result.zip64);
            j.key("entries").begin_array();
            for (const auto& e : result->result.per_entry)
            {
                j.begin_object().field("plan_index", static_cast<std::uint64_t>(e.plan_index)).field("name", e.name).field("method", static_cast<int>(e.method));
                j.field("compressed_size", e.compressed_size).field("uncompressed_size", e.uncompressed_size).field("crc32", static_cast<std::uint64_t>(e.crc32));
                j.field("status", status_name(e.status)).field("message", e.message).end_object();
            }
            j.end_array().end_object();
            return dup_string(j.str());
        },
        nullptr
    );
}

char* zp_reader_describe(const zp_reader_t* reader)
{
    ZP_REQUIRE(reader, nullptr);
    return guarded(
        [&]() -> char* {
            JsonWriter j;
            j.begin_object();
            j.key("app_version");
            if (reader->reader->metadata())
                j.value(reader->app_version);
            else
                j.null();
            j.field("zip64", reader->reader->zip64()).key("entries").begin_array();
            for (std::size_t i = 0; i < reader->reader->entries().size(); ++i)
            {
                const auto header = reader->reader->flac_header(i);
                const auto& e = reader->reader->entries()[i];
                j.begin_object().field("index", static_cast<std::uint64_t>(i)).field("name", e.name).field("kind", kind_name(e.kind));
                j.field("method", static_cast<std::uint64_t>(e.raw_method)).field("supported", e.method != EntryMethod::Unsupported);
                j.field("compressed_size", e.compressed_size).field("uncompressed_size", e.uncompressed_size).field("crc32", static_cast<std::uint64_t>(e.crc32)).field("mtime", e.mtime);
                j.field("flac_restorable", e.flac_restorable);
                if (header && header->project)
                    j.field("restores_to", header->project->original_name);
                if (e.flac_group)
                    j.field("channel_index", static_cast<std::uint64_t>(e.flac_group->channel_index)).field("channel_count", static_cast<std::uint64_t>(e.flac_group->channel_count));
                j.end_object();
            }
            j.end_array().end_object();
            return dup_string(j.str());
        },
        nullptr
    );
}

char* zp_reader_flac_describe(zp_reader_t* reader, size_t i)
{
    ZP_REQUIRE(reader && i < reader->reader->entries().size(), nullptr);
    return guarded(
        [&]() -> char* {
            const auto header = reader->reader->flac_header(i);
            if (!header)
            {
                set_error(Status::InvalidArgument, "not a readable FLAC entry");
                return nullptr;
            }
            const auto& si = header->stream_info;
            JsonWriter j;
            j.begin_object().field("sample_rate", static_cast<std::uint64_t>(si.sample_rate)).field("bits_per_sample", static_cast<std::uint64_t>(si.bits_per_sample));
            j.field("channels", static_cast<std::uint64_t>(si.channels)).field("total_samples", si.total_samples);
            j.field("md5", to_hex(si.md5));
            std::vector<flac::ForeignRecord> records = header->foreign;
            if (header->project)
            {
                const auto& p = *header->project;
                const char* layout = p.layout == flac::Layout::Standard ? "standard" : p.layout == flac::Layout::MultiMonoMember ? "multi_mono" : "private";
                j.field("layout", layout).field("original_name", p.original_name).field("sha256", to_hex(p.sha256));
                j.field("channel_index", static_cast<std::uint64_t>(p.channel_index)).field("channel_count", static_cast<std::uint64_t>(p.channel_count));
                j.field("restorable_with_flac_tool", p.layout == flac::Layout::Standard);
                if (p.layout != flac::Layout::Standard)
                {
                    if (auto unpacked = flac::unpack_private(p.private_data))
                        records = std::move(*unpacked);
                }
            }
            if (!records.empty())
            {
                const auto& first = records.front().bytes;
                const auto starts = [&](std::string_view id) { return first.size() >= id.size() && std::equal(id.begin(), id.end(), first.begin()); };
                const char* container = starts("RF64") || starts("BW64") ? "RF64" : starts("RIFF") ? "WAV" : starts("FORM") && first.size() >= 12 && first[8] == 'A' && first[11] == 'C' ? "AIFF-C"
                    : starts("FORM")                                                     ? "AIFF"
                    : starts("caff")                                                     ? "CAF"
                    : starts("riff")                                                     ? "Wave64"
                                                                                         : "unknown";
                j.field("container", container);
                j.key("chunks").begin_array();
                bool after_audio = false;
                for (std::size_t r = 1; r < records.size(); ++r)
                {
                    const auto& b = records[r].bytes;
                    const bool w64 = std::string_view(container) == "Wave64";
                    std::string id = b.size() >= 4 ? std::string(reinterpret_cast<const char*>(b.data()), 4) : std::string{};
                    const bool audio = id == "data" || id == "SSND";
                    j.begin_object().field("id", id).field("size", static_cast<std::uint64_t>(b.size())).field("after_audio", after_audio).field("audio", audio);
                    static constexpr std::array<std::string_view, 16> known{ "fmt ", "data", "bext", "iXML", "LIST", "cue ", "ds64", "junk", "JUNK", "COMM", "SSND", "NAME", "AUTH", "ANNO", "desc", "FVER" };
                    j.field("known", std::ranges::find(known, std::string_view(id)) != known.end() || w64);
                    j.end_object();
                    if (audio)
                        after_audio = true;
                }
                j.end_array();
            }
            j.key("tags").begin_array();
            if (header->comments)
            {
                for (const auto& [k, v] : header->comments->fields)
                    j.begin_array().value(k).value(v).end_array();
            }
            j.end_array().end_object();
            return dup_string(j.str());
        },
        nullptr);
}

char* zp_xplan_describe(const zp_xplan_t* xplan)
{
    ZP_REQUIRE(xplan, nullptr);
    return guarded(
        [&]() -> char* {
            JsonWriter j;
            j.begin_object().field("status", status_name(xplan->result.status)).field("message", xplan->result.message);
            j.key("issues").begin_array();
            for (const auto& i : xplan->plan.issues)
                j.begin_object().field("kind", issue_text(i.kind)).field("entry", static_cast<std::uint64_t>(i.entry)).field("name", i.name).field("detail", i.detail).field("is_error", i.is_error()).end_object();
            j.end_array().key("items").begin_array();
            for (const auto& i : xplan->plan.items)
            {
                const char* decision = i.decision == ItemDecision::Skip ? "skip" : i.decision == ItemDecision::Replace ? "replace"
                    : i.decision == ItemDecision::Undecided                                                            ? "undecided"
                                                                                                                       : "write";
                j.begin_object().field("entry", static_cast<std::uint64_t>(i.entry)).field("target", i.target).field("kind", kind_name(i.kind)).field("decision", decision).field("restore", !i.members.empty()).end_object();
            }
            j.end_array().key("outcomes").begin_array();
            for (const auto& o : xplan->result.outcomes)
                j.begin_object().field("entry", static_cast<std::uint64_t>(o.entry)).field("target", o.target).field("status", status_name(o.status)).field("message", o.message).end_object();
            j.end_array().end_object();
            return dup_string(j.str());
        },
        nullptr
    );
}

void zp_free(void* ptr)
{
    std::free(ptr);
}

const char* zp_last_error(void)
{
    return last_error.c_str();
}

int zp_last_status(void)
{
    return last_status;
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

zp_input_t* zp_input_from_callbacks(const zp_input_item_t* items, size_t count, zp_read_fn read, void* user)
{
    ZP_REQUIRE((items || count == 0) && read, nullptr);
    return guarded(
        [&]() -> zp_input_t* {
            auto* in = new zp_input();
            in->source = std::make_unique<CallbackInputSource>(items, count, read, user);
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
    out->channel_count = e.channel_count;
    out->fallback_reason = e.flac_fallback_reason ? static_cast<int>(*e.flac_fallback_reason) : -1;
    out->fallback_detail = e.flac_fallback_detail.c_str();
    plan->restored_names.resize(plan->plan.entries.size());
    plan->restored_names[i] = e.is_converted() ? e.restored_name() : std::string{};
    out->restored_name = plan->restored_names[i].c_str();
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

const char* zp_default_readme_template(void)
{
    return default_readme_template().data();
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
            if (options && options->readme_template)
                o.readme_template = options->readme_template;
            if (options && options->temp_dir)
                o.temp_dir = path_from_utf8(options->temp_dir);
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
    const auto header = reader->reader->flac_header(i);
    reader->original_names.resize(reader->reader->entries().size());
    reader->original_names[i] = header && header->project ? header->project->original_name : std::string{};
    const auto& e = reader->reader->entries()[i];
    *out = {};
    out->name = e.name.c_str();
    out->flac_original_name = reader->original_names[i].c_str();
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

zp_sink_t* zp_sink_from_callbacks(const zp_sink_callbacks_t* callbacks)
{
    ZP_REQUIRE(callbacks && callbacks->open_file && callbacks->write, nullptr);
    return guarded(
        [&]() -> zp_sink_t* {
            auto* s = new zp_sink();
            s->sink = std::make_unique<CallbackSink>(*callbacks);
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

int zp_restore_flac(zp_stream_t* input, zp_stream_t* output)
{
    ZP_REQUIRE(input && output && input->stream->seekable(), ZP_INVALID_ARGUMENT);
    return guarded(
        [&]() -> int {
            auto& in = *input->stream;
            if (auto r = in.seek(0); !r)
                return set_error(r.error());
            auto header = flac::read_header(in);
            if (!header)
                return set_error(header.error());
            if (header->project && header->project->layout == flac::Layout::MultiMonoMember)
                return set_error(Status::IncompleteGroup, "a multi-mono member cannot be restored on its own; extract the whole group from its archive");
            const auto opener = [&](std::size_t) -> Result<std::unique_ptr<IChunkedStream>> {
                if (auto r = in.seek(0); !r)
                    return std::unexpected(r.error());
                struct Borrowed final : IChunkedStream
                {
                    IChunkedStream& s;
                    explicit Borrowed(IChunkedStream& inner) :
                        s(inner)
                    {
                    }
                    Result<std::size_t> read(std::uint8_t* buf, std::size_t len) override { return s.read(buf, len); }
                    [[nodiscard]] std::uint64_t tell() const override { return s.tell(); }
                };
                return std::unique_ptr<IChunkedStream>(std::make_unique<Borrowed>(in));
            };
            const auto sink = [&](std::span<const std::uint8_t> bytes) -> VoidResult { return output->stream->write_all(bytes); };
            if (auto r = flac::restore({ *header }, opener, sink); !r)
                return set_error(r.error());
            return ZP_OK;
        },
        ZP_INTERNAL
    );
}

int zp_flac_original_name(zp_stream_t* input, char* buf, size_t len)
{
    ZP_REQUIRE(input && buf && len > 0 && input->stream->seekable(), ZP_INVALID_ARGUMENT);
    return guarded(
        [&]() -> int {
            buf[0] = '\0';
            auto& in = *input->stream;
            if (auto r = in.seek(0); !r)
                return set_error(r.error());
            auto header = flac::read_header(in);
            if (!header)
                return set_error(header.error());
            if (header->project)
            {
                const auto& name = header->project->original_name;
                const auto n = std::min(len - 1, name.size());
                std::memcpy(buf, name.data(), n);
                buf[n] = '\0';
            }
            return ZP_OK;
        },
        ZP_INTERNAL
    );
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
