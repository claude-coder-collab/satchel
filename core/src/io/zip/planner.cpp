// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "io/zip/planner.hpp"

#include "io/zip/path_policy.hpp"

#include <algorithm>
#include <format>
#include <map>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

namespace zp
{

std::uint64_t ArchivePlan::total_input_bytes() const
{
    std::uint64_t total = 0;
    for (const auto& e : entries)
    {
        if (e.codec != PlanCodec::FlacMono || e.channel_index.value_or(1) == 1)
            total += e.item.size;
    }
    return total;
}

std::string_view fallback_reason_text(FallbackReason r)
{
    switch (r)
    {
        case FallbackReason::NotParsable:
            return "container could not be parsed cleanly";
        case FallbackReason::FloatSamples:
            return "floating-point samples";
        case FallbackReason::UnsupportedEncoding:
            return "compressed or unsupported sample encoding";
        case FallbackReason::BitDepth:
            return "bit depth outside 4-32";
        case FallbackReason::SampleRate:
            return "sample rate above 1,048,575 Hz";
        case FallbackReason::TooManySamples:
            return "more than 2^36 samples per channel";
        case FallbackReason::ChunkTooLarge:
            return "a metadata chunk is too large for a FLAC metadata block";
        case FallbackReason::NotSeekable:
            return "input is not seekable";
    }
    return "unknown";
}

Result<std::vector<PlanEntry>> ArchivePlanner::make_entries(std::vector<InputItem> items, std::vector<Warning>& warnings) const // NOLINT(readability-convert-member-functions-to-static)
{
    std::vector<PlanEntry> entries;
    entries.reserve(items.size());
    for (auto& item : items)
    {
        if (item.kind == ItemKind::Symlink)
        {
            warnings.push_back({ WarningKind::SymlinkSkipped, item.source_path, "symbolic links are not archived" });
            continue;
        }
        if (item.kind != ItemKind::File && item.kind != ItemKind::Directory)
            continue;
        PlanEntry e;
        auto normalized = PathPolicy::normalize(item.archive_path);
        e.output_name = normalized ? *normalized : item.archive_path;
        e.codec = PlanCodec::General;
        e.snapshot = item.snapshot();
        e.item = std::move(item);
        entries.push_back(std::move(e));
    }
    return entries;
}

Result<ArchivePlan> ArchivePlanner::plan(InputSource& input) const
{
    auto items = input.enumerate();
    if (!items)
        return std::unexpected(items.error());
    ArchivePlan plan;
    plan.options = options_;
    auto entries = make_entries(std::move(*items), plan.warnings);
    if (!entries)
        return std::unexpected(entries.error());
    plan.entries = std::move(*entries);
    check(plan);
    return plan;
}

void ArchivePlanner::check(ArchivePlan& plan)
{
    plan.conflicts.clear();
    std::map<std::string, std::vector<std::size_t>> by_key;
    std::map<std::string, std::size_t> file_keys;
    std::vector<std::string> keys(plan.entries.size());

    for (std::size_t i = 0; i < plan.entries.size(); ++i)
    {
        const auto& e = plan.entries[i];
        if (auto v = PathPolicy::validate_for_archive(e.output_name); !v)
        {
            plan.conflicts.push_back({ ConflictKind::InvalidName, e.output_name, { i }, v.error().message });
            continue;
        }
        auto key = PathPolicy::collision_key(e.output_name);
        if (!key)
        {
            plan.conflicts.push_back({ ConflictKind::InvalidName, e.output_name, { i }, key.error().message });
            continue;
        }
        keys[i] = *key;
        by_key[*key].push_back(i);
        if (!e.is_directory())
            file_keys.emplace(*key, i);
    }

    std::set<std::size_t> grouped_once;
    for (auto& [key, idx] : by_key)
    {
        if (idx.size() < 2)
            continue;
        // Members of one multi-mono group never collide with each other by construction.
        plan.conflicts.push_back({ ConflictKind::Collision, key, idx, std::format("{} entries map to the same name", idx.size()) });
    }

    // A file name used as a folder by another entry ("a" and "a/b").
    std::map<std::string, std::vector<std::size_t>> parent_clash;
    for (std::size_t i = 0; i < plan.entries.size(); ++i)
    {
        const auto& key = keys[i];
        if (key.empty())
            continue;
        for (auto pos = key.find('/'); pos != std::string::npos; pos = key.find('/', pos + 1))
        {
            const auto prefix = key.substr(0, pos);
            if (auto it = file_keys.find(prefix); it != file_keys.end())
            {
                auto& list = parent_clash[prefix];
                if (list.empty())
                    list.push_back(it->second);
                list.push_back(i);
            }
        }
    }
    for (auto& [key, idx] : parent_clash)
        plan.conflicts.push_back({ ConflictKind::Collision, key, idx, "a file name is also used as a folder" });

    std::ranges::sort(plan.conflicts, {}, [](const Conflict& c) { return c.entries.front(); });
}

Result<ArchivePlan> ArchivePlanner::replan(const ArchivePlan& plan, const std::vector<Resolution>& resolutions)
{
    ArchivePlan next = plan;
    std::vector<bool> skip(next.entries.size(), false);
    std::set<std::size_t> renamed;

    for (const auto& r : resolutions)
    {
        if (r.entry >= next.entries.size())
            return fail(Status::InvalidArgument, std::format("resolution refers to entry {} of {}", r.entry, next.entries.size()));
        auto& e = next.entries[r.entry];
        if (e.codec == PlanCodec::Generated)
            return fail(Status::InvalidArgument, "the generated readme cannot be renamed or skipped; resolve the input file instead");
        switch (r.action)
        {
            case ResolutionAction::Rename:
            {
                auto n = PathPolicy::normalize(r.new_name);
                if (!n)
                    return std::unexpected(n.error());
                if (auto v = PathPolicy::validate_for_archive(*n); !v)
                    return std::unexpected(v.error());
                e.output_name = std::move(*n);
                renamed.insert(r.entry);
                break;
            }
            case ResolutionAction::Skip:
                skip[r.entry] = true;
                break;
            case ResolutionAction::DisableFlac:
                if (e.codec == PlanCodec::Flac || e.codec == PlanCodec::FlacMono)
                {
                    auto n = PathPolicy::normalize(e.item.archive_path);
                    e.output_name = n ? *n : e.item.archive_path;
                    e.codec = PlanCodec::General;
                    e.group_id.reset();
                    e.channel_index.reset();
                }
                break;
        }
    }

    std::vector<PlanEntry> kept;
    std::vector<std::size_t> new_index(next.entries.size(), static_cast<std::size_t>(-1));
    for (std::size_t i = 0; i < next.entries.size(); ++i)
    {
        if (skip[i])
            continue;
        new_index[i] = kept.size();
        kept.push_back(std::move(next.entries[i]));
    }
    next.entries = std::move(kept);
    check(next);

    for (const auto& c : next.conflicts)
    {
        for (const auto idx : c.entries)
        {
            for (const auto old : renamed)
            {
                if (new_index[old] == idx)
                    return fail(Status::NameCollision, std::format("'{}' collides with another entry", next.entries[idx].output_name));
            }
        }
    }
    return next;
}

}
