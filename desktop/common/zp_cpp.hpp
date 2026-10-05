// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#pragma once

// Header-only C++ conveniences over the C API (core/include/zp/zp.h) for the desktop apps.

#include "zp/zp.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace zpp
{

class Error : public std::runtime_error
{
public:
    Error(int status, const std::string& message) :
        std::runtime_error(message),
        status_(status)
    {
    }
    [[nodiscard]] int status() const { return status_; }
    [[nodiscard]] std::string name() const { return zp_status_name(status_); }

private:
    int status_;
};

inline Error last_error(std::optional<int> status = std::nullopt)
{
    return { status.value_or(zp_last_status()), zp_last_error() };
}

inline void check(int status)
{
    if (status != ZP_OK)
        throw last_error(status);
}

template <typename T>
T* not_null(T* p)
{
    if (!p)
        throw last_error();
    return p;
}

inline std::string str(const char* s)
{
    return s ? std::string(s) : std::string{};
}

template <auto Free>
struct Deleter
{
    template <typename T>
    void operator()(T* p) const
    {
        Free(p);
    }
};

using Context = std::unique_ptr<zp_context_t, Deleter<zp_context_free>>;
using Stream = std::unique_ptr<zp_stream_t, Deleter<zp_stream_free>>;
using Input = std::unique_ptr<zp_input_t, Deleter<zp_input_free>>;
using Plan = std::unique_ptr<zp_plan_t, Deleter<zp_plan_free>>;
using BuildResult = std::unique_ptr<zp_build_result_t, Deleter<zp_build_result_free>>;
using Reader = std::unique_ptr<zp_reader_t, Deleter<zp_reader_free>>;
using Sink = std::unique_ptr<zp_sink_t, Deleter<zp_sink_free>>;
using ExtractionPlan = std::unique_ptr<zp_xplan_t, Deleter<zp_xplan_free>>;
using Editor = std::unique_ptr<zp_editor_t, Deleter<zp_editor_free>>;

inline Input input_from_paths(const std::vector<std::string>& paths)
{
    std::vector<const char*> raw;
    raw.reserve(paths.size());
    for (const auto& p : paths)
        raw.push_back(p.c_str());
    return Input(not_null(zp_input_from_paths(raw.data(), raw.size())));
}

struct PlanEntry
{
    std::size_t index = 0;
    std::string source_path;
    std::string output_name;
    int kind = ZP_KIND_FILE;
    int codec = ZP_CODEC_GENERAL;
    std::uint64_t size = 0;
    std::int64_t mtime = 0;
    std::uint16_t channel_index = 0;
    std::uint16_t channel_count = 0;
    int fallback_reason = -1;
    std::string fallback_detail;
    std::string restored_name;
};

struct Conflict
{
    int kind = ZP_CONFLICT_COLLISION;
    std::string key;
    std::string detail;
    std::vector<std::size_t> entries;
};

struct Warning
{
    int kind = ZP_WARNING_SYMLINK_SKIPPED;
    std::string source_path;
    std::string detail;
};

inline std::vector<PlanEntry> plan_entries(const zp_plan_t* plan)
{
    std::vector<PlanEntry> out;
    zp_plan_entry_t e{};
    for (std::size_t i = 0; i < zp_plan_entry_count(plan); ++i)
    {
        check(zp_plan_get_entry(plan, i, &e));
        out.push_back({ i, str(e.source_path), str(e.output_name), e.kind, e.codec, e.size, e.mtime, e.channel_index, e.channel_count, e.fallback_reason, str(e.fallback_detail), str(e.restored_name) });
    }
    return out;
}

inline std::vector<Conflict> plan_conflicts(const zp_plan_t* plan)
{
    std::vector<Conflict> out;
    zp_conflict_t c{};
    for (std::size_t i = 0; i < zp_plan_conflict_count(plan); ++i)
    {
        check(zp_plan_get_conflict(plan, i, &c));
        out.push_back({ c.kind, str(c.collision_key), str(c.detail), std::vector<std::size_t>(c.entries, c.entries + c.entry_count) });
    }
    return out;
}

inline std::vector<Warning> plan_warnings(const zp_plan_t* plan)
{
    std::vector<Warning> out;
    zp_warning_t w{};
    for (std::size_t i = 0; i < zp_plan_warning_count(plan); ++i)
    {
        check(zp_plan_get_warning(plan, i, &w));
        out.push_back({ w.kind, str(w.source_path), str(w.detail) });
    }
    return out;
}

struct EntryInfo
{
    std::size_t index = 0;
    std::string name;
    int kind = ZP_KIND_FILE;
    int method = 0;
    bool supported = true;
    std::uint64_t compressed_size = 0;
    std::uint64_t uncompressed_size = 0;
    std::uint32_t crc32 = 0;
    std::int64_t mtime = 0;
    std::optional<std::uint32_t> unix_mode;
    bool flac_restorable = false;
    std::optional<std::pair<std::uint16_t, std::uint16_t>> flac_channel; // index, count
    std::string flac_original_name;
    std::optional<std::uint64_t> original_size; // FLAC: size of the file it restores to; multi-mono: channel 1 only
};

inline std::vector<EntryInfo> reader_entries(const zp_reader_t* reader)
{
    std::vector<EntryInfo> out;
    zp_entry_info_t e{};
    for (std::size_t i = 0; i < zp_reader_entry_count(reader); ++i)
    {
        check(zp_reader_get_entry(reader, i, &e));
        EntryInfo info{ i, str(e.name), e.kind, e.method, e.supported != 0, e.compressed_size, e.uncompressed_size, e.crc32, e.mtime, std::nullopt, e.flac_restorable != 0, std::nullopt, str(e.flac_original_name), std::nullopt };
        if (std::uint64_t original = 0; zp_reader_flac_original_size(reader, i, &original) == ZP_OK)
            info.original_size = original;
        if (e.has_unix_mode)
            info.unix_mode = e.unix_mode;
        if (e.has_flac_group)
            info.flac_channel = std::pair{ e.flac_channel_index, e.flac_channel_count };
        out.push_back(std::move(info));
    }
    return out;
}

// Adapts a std::function to zp_progress_fn (return false to cancel).
struct ProgressAdapter
{
    std::function<bool(std::uint64_t, std::uint64_t)> fn;

    static int call(void* user, std::uint64_t done, std::uint64_t total)
    {
        auto* self = static_cast<ProgressAdapter*>(user);
        return self->fn && !self->fn(done, total) ? 1 : 0;
    }
};

}
