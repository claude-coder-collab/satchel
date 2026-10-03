// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "io/zip/planner.hpp"

#include "codecs/pcm/pcm_container.hpp"
#include "common/bytes.hpp"
#include "crypto/hash.hpp"
#include "io/zip/path_policy.hpp"

#include <algorithm>
#include <cstring>
#include <format>
#include <map>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

namespace zp
{

namespace
{

std::size_t last_slash(std::string_view s)
{
    const auto p = s.rfind('/');
    return p == std::string_view::npos ? 0 : p + 1;
}

// Splits "dir/name.ext" into ("dir/name", ".ext"); the extension may be empty.
std::pair<std::string, std::string> split_extension(std::string_view path)
{
    const auto base = last_slash(path);
    const auto dot = path.rfind('.');
    if (dot == std::string_view::npos || dot <= base)
        return { std::string(path), {} };
    return { std::string(path.substr(0, dot)), std::string(path.substr(dot)) };
}

Uuid group_id_for(const InputItem& item, std::string_view stem)
{
    ByteWriter w;
    w.str("satchel multi-mono group\n");
    w.str(stem);
    w.u64le(item.size);
    w.u64le(static_cast<std::uint64_t>(item.mtime_ns));
    const auto digest = Sha256::of(w.data());
    Uuid id{};
    std::memcpy(id.data(), digest.data(), id.size());
    id[6] = static_cast<std::uint8_t>((id[6] & 0x0F) | 0x80);
    id[8] = static_cast<std::uint8_t>((id[8] & 0x3F) | 0x80);
    return id;
}

bool is_group_member(const PlanEntry& e)
{
    return (e.codec == PlanCodec::FlacMono || e.kept_restorable) && e.group_id.has_value();
}

bool same_group(const PlanEntry& a, const PlanEntry& b)
{
    return is_group_member(a) && is_group_member(b) && a.group_id == b.group_id;
}

}

void ArchivePlanner::ensure_readme(ArchivePlan& plan)
{
    std::erase_if(plan.entries, [](const PlanEntry& e) { return e.codec == PlanCodec::Generated; });
    const bool any_flac = std::ranges::any_of(plan.entries, &PlanEntry::is_converted);
    if (!any_flac)
        return;
    std::int64_t newest = 0;
    bool any_file = false;
    for (const auto& e : plan.entries)
    {
        if (e.is_directory() || e.codec == PlanCodec::Generated)
            continue;
        newest = any_file ? std::max(newest, e.item.mtime_ns) : e.item.mtime_ns;
        any_file = true;
    }
    PlanEntry readme;
    readme.item.source_path = "generated:readme";
    readme.item.archive_path = std::string(readme_file_name);
    readme.item.kind = ItemKind::File;
    readme.item.mtime_ns = newest;
    readme.item.unix_mode = 0644;
    readme.output_name = std::string(readme_file_name);
    readme.codec = PlanCodec::Generated;
    readme.snapshot = readme.item.snapshot();
    plan.entries.push_back(std::move(readme));
}

namespace
{
}

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

std::string flac_stem(std::string_view output_name, PlanCodec codec)
{
    std::string name(output_name);
    if (name.size() >= 5 && std::equal(name.end() - 5, name.end(), ".flac", [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == b; }))
        name.resize(name.size() - 5);
    if (codec == PlanCodec::FlacMono)
    {
        const auto pos = name.rfind("_ch");
        if (pos != std::string::npos && pos + 3 < name.size() && std::all_of(name.begin() + static_cast<std::ptrdiff_t>(pos) + 3, name.end(), [](char c) { return c >= '0' && c <= '9'; }))
            name.resize(pos);
    }
    return name;
}

std::string mono_member_name(std::string_view stem, std::uint16_t channel, std::uint16_t channels)
{
    const auto width = std::max<std::size_t>(2, std::to_string(channels).size());
    return std::format("{}_ch{:0{}}.flac", stem, channel, width);
}

std::string PlanEntry::restored_name() const
{
    if (kept_restorable)
    {
        const auto slash = output_name.rfind('/');
        return (slash == std::string::npos ? std::string{} : output_name.substr(0, slash + 1)) + kept_restored_name;
    }
    return flac_stem(output_name, codec) + original_extension;
}

Result<std::vector<PlanEntry>> ArchivePlanner::make_entries(std::vector<InputItem> items, std::vector<Warning>& warnings) const
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

        if (options_.flac_enabled && item.kind == ItemKind::File && item.size >= 12)
        {
            auto stream = item.open();
            if (!stream)
                return std::unexpected(stream.error());
            auto scan = scan_pcm(**stream);
            if (!scan)
                return std::unexpected(scan.error());
            if (scan->recognized && scan->layout)
            {
                auto layout = std::make_shared<const PcmLayout>(std::move(*scan->layout));
                auto [stem, ext] = split_extension(e.output_name);
                e.original_extension = ext;
                e.pcm = layout;
                e.channel_count = layout->channels;
                if (layout->channels <= 8)
                {
                    e.codec = PlanCodec::Flac;
                    e.output_name = stem + ".flac";
                    e.item = std::move(item);
                    entries.push_back(std::move(e));
                }
                else
                {
                    const auto group = group_id_for(item, stem);
                    e.codec = PlanCodec::FlacMono;
                    e.group_id = group;
                    e.item = std::move(item);
                    for (std::uint16_t c = 1; c <= layout->channels; ++c)
                    {
                        PlanEntry member = e;
                        member.channel_index = c;
                        member.output_name = mono_member_name(stem, c, layout->channels);
                        entries.push_back(std::move(member));
                    }
                }
                continue;
            }
            if (scan->recognized && scan->reason)
            {
                e.flac_fallback_reason = scan->reason;
                e.flac_fallback_detail = scan->detail;
                warnings.push_back({ WarningKind::FlacFallback, item.source_path, std::format("stored without FLAC conversion: {} ({})", fallback_reason_text(*scan->reason), scan->detail) });
            }
        }
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
    ensure_readme(plan);
    check(plan);
    return plan;
}

void ArchivePlanner::check(ArchivePlan& plan)
{
    plan.conflicts.clear();
    // Every entry owns its output name; a FLAC entry (or group) also owns the name its original
    // file is restored to, so extraction can never collide either.
    std::map<std::string, std::vector<std::size_t>> by_key;
    std::map<std::string, std::size_t> file_keys;
    std::vector<std::string> keys(plan.entries.size());

    const auto add_key = [&](const std::string& key, std::size_t owner) {
        auto& list = by_key[key];
        if (std::ranges::find(list, owner) == list.end())
            list.push_back(owner);
    };

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
        add_key(*key, i);
        if (!e.is_directory())
            file_keys.emplace(*key, i);
        if (e.is_converted())
        {
            std::size_t owner = i;
            if (is_group_member(e))
            {
                for (std::size_t j = 0; j < i; ++j)
                {
                    if (same_group(plan.entries[j], e))
                    {
                        owner = j;
                        break;
                    }
                }
            }
            if (auto restored = PathPolicy::collision_key(e.restored_name()); restored && *restored != *key)
            {
                add_key(*restored, owner);
                if (owner != i)
                    add_key(*restored, i);
            }
        }
    }

    for (auto& [key, idx] : by_key)
    {
        std::vector<std::size_t> distinct;
        for (const auto i : idx)
        {
            const bool grouped = std::ranges::any_of(distinct, [&](std::size_t d) { return same_group(plan.entries[d], plan.entries[i]); });
            if (!grouped)
                distinct.push_back(i);
        }
        if (distinct.size() < 2)
            continue;
        std::ranges::sort(idx);
        plan.conflicts.push_back({ ConflictKind::Collision, key, idx, std::format("{} entries map to the same name", distinct.size()) });
    }

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

    const auto group_of = [&](std::size_t i) {
        std::vector<std::size_t> members;
        for (std::size_t j = 0; j < next.entries.size(); ++j)
        {
            if (j == i || same_group(next.entries[j], next.entries[i]))
                members.push_back(j);
        }
        return members;
    };

    for (const auto& r : resolutions)
    {
        if (r.entry >= next.entries.size())
            return fail(Status::InvalidArgument, std::format("resolution refers to entry {} of {}", r.entry, next.entries.size()));
        if (next.entries[r.entry].codec == PlanCodec::Generated)
            return fail(Status::InvalidArgument, "the generated readme cannot be renamed or skipped; resolve the input file instead");
        const auto members = group_of(r.entry);
        switch (r.action)
        {
            case ResolutionAction::Rename:
            {
                auto n = PathPolicy::normalize(r.new_name);
                if (!n)
                    return std::unexpected(n.error());
                if (auto v = PathPolicy::validate_for_archive(*n); !v)
                    return std::unexpected(v.error());
                auto& e = next.entries[r.entry];
                if (e.codec == PlanCodec::FlacMono)
                {
                    const auto stem = flac_stem(*n, PlanCodec::FlacMono);
                    for (const auto m : members)
                    {
                        auto& member = next.entries[m];
                        member.output_name = mono_member_name(stem, member.channel_index.value_or(1), member.channel_count);
                        if (auto v = PathPolicy::validate_for_archive(member.output_name); !v)
                            return std::unexpected(v.error());
                        renamed.insert(m);
                    }
                }
                else
                {
                    e.output_name = std::move(*n);
                    renamed.insert(r.entry);
                }
                break;
            }
            case ResolutionAction::Skip:
                for (const auto m : members)
                    skip[m] = true;
                break;
            case ResolutionAction::DisableFlac:
            {
                auto& e = next.entries[r.entry];
                if (!e.is_flac())
                    break;
                for (const auto m : members)
                {
                    if (m != members.front())
                        skip[m] = true;
                }
                auto& first = next.entries[members.front()];
                auto n = PathPolicy::normalize(first.item.archive_path);
                first.output_name = n ? *n : first.item.archive_path;
                first.codec = PlanCodec::General;
                first.group_id.reset();
                first.channel_index.reset();
                first.pcm.reset();
                first.channel_count = 0;
                first.original_extension.clear();
                skip[members.front()] = false;
                break;
            }
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
    ensure_readme(next);
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
