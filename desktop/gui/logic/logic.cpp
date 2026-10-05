// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "logic.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <format>
#include <map>

namespace satchel_gui
{

namespace
{

std::string lower(std::string s)
{
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::filesystem::path output_dir(const std::filesystem::path& next_to, const std::optional<std::filesystem::path>& output_folder)
{
    return output_folder ? *output_folder : next_to;
}

}

bool is_zip(const std::filesystem::path& p)
{
    return lower(p.extension().string()) == ".zip";
}

DropAction drop_action(const std::vector<std::filesystem::path>& dropped, Intent intent)
{
    if (dropped.empty())
        return DropAction::None;
    switch (intent)
    {
        case Intent::Compress:
            return DropAction::CompressAll;
        case Intent::Extract:
            return std::ranges::any_of(dropped, [](const auto& p) { return is_zip(p); }) ? DropAction::ExtractEach : DropAction::None;
        case Intent::Auto:
            break;
    }
    return std::ranges::all_of(dropped, [](const auto& p) { return is_zip(p); }) ? DropAction::ExtractEach : DropAction::CompressAll;
}

std::vector<std::filesystem::path> action_items(const std::vector<std::filesystem::path>& dropped, DropAction action)
{
    if (action != DropAction::ExtractEach)
        return action == DropAction::None ? std::vector<std::filesystem::path>{} : dropped;
    std::vector<std::filesystem::path> zips;
    std::ranges::copy_if(dropped, std::back_inserter(zips), [](const auto& p) { return is_zip(p); });
    return zips;
}

std::filesystem::path unique_path(const std::filesystem::path& wanted, const ExistsFn& exists, bool folder)
{
    if (!exists(wanted))
        return wanted;
    const auto parent = wanted.parent_path();
    const auto stem = folder ? wanted.filename().u8string() : wanted.stem().u8string();
    const auto ext = folder ? std::u8string() : wanted.extension().u8string();
    for (int n = 2;; ++n)
    {
        const auto number = std::to_string(n);
        auto name = stem;
        name += u8' ';
        name.append(number.begin(), number.end());
        name += ext;
        auto candidate = parent / name;
        if (!exists(candidate))
            return candidate;
    }
}

std::filesystem::path compress_output(const std::vector<std::filesystem::path>& items, const std::optional<std::filesystem::path>& output_folder, const ExistsFn& exists)
{
    if (items.empty())
        return {};
    if (items.size() == 1)
    {
        auto item = items.front();
        if (!item.has_filename())
            item = item.parent_path();
        return unique_path(output_dir(item.parent_path(), output_folder) / (item.filename().u8string() + u8".zip"), exists);
    }
    const auto parent = items.front().parent_path();
    auto name = parent.filename();
    if (name.empty())
        name = "Archive";
    return unique_path(output_dir(parent, output_folder) / (name.u8string() + u8".zip"), exists);
}

std::filesystem::path extract_output(const std::filesystem::path& archive, const std::optional<std::filesystem::path>& output_folder, const ExistsFn& exists)
{
    return unique_path(output_dir(archive.parent_path(), output_folder) / archive.stem(), exists, true);
}

std::vector<Row> group_rows(const std::vector<ListedEntry>& entries)
{
    std::vector<Row> rows;
    std::map<std::string, std::size_t> group_row;
    for (const auto& e : entries)
    {
        Row r{ e.name, e.size, e.packed, e.method, e.restores_to, e.mtime, e.directory, { e.index }, {}, {}, 0, 0 };
        if (!e.group)
        {
            rows.push_back(std::move(r));
            continue;
        }
        auto it = group_row.find(*e.group);
        if (it == group_row.end())
        {
            const auto slash = e.name.rfind('/');
            const auto folder = slash == std::string::npos ? std::string{} : e.name.substr(0, slash + 1);
            Row g{ std::format("{}{} — {} channels", folder, e.restores_to, e.channel_count), 0, 0, "FLAC multi-mono", e.restores_to, e.mtime, false, {}, {}, folder + e.restores_to, 0, e.channel_count };
            it = group_row.emplace(*e.group, rows.size()).first;
            rows.push_back(std::move(g));
        }
        auto& g = rows[it->second];
        g.size += e.size;
        g.packed += e.packed;
        g.mtime = std::max(g.mtime, e.mtime);
        g.entries.push_back(e.index);
        r.method = std::format("FLAC channel {}/{}", e.channel_index, e.channel_count);
        r.channel_index = e.channel_index;
        r.channel_count = e.channel_count;
        g.children.push_back(std::move(r));
    }
    for (auto& r : rows)
    {
        if (r.children.empty())
            continue;
        std::vector<std::pair<std::uint16_t, std::size_t>> order;
        for (std::size_t i = 0; i < r.children.size(); ++i)
        {
            const auto& e = *std::ranges::find(entries, r.children[i].entries.front(), &ListedEntry::index);
            order.emplace_back(e.channel_index, i);
        }
        std::ranges::sort(order);
        std::vector<Row> sorted;
        std::vector<std::size_t> ids;
        for (const auto& [ch, i] : order)
        {
            ids.push_back(r.children[i].entries.front());
            sorted.push_back(std::move(r.children[i]));
        }
        r.children = std::move(sorted);
        r.entries = std::move(ids);
    }
    return rows;
}

int percent_saved(std::uint64_t size, std::uint64_t packed)
{
    if (size == 0)
        return 0;
    return static_cast<int>(std::lround(100.0 * (1.0 - static_cast<double>(packed) / static_cast<double>(size))));
}

std::string format_timecode(std::uint64_t samples, std::uint32_t sample_rate, const std::string& timecode_rate)
{
    if (sample_rate == 0)
        return {};
    std::uint64_t num = 0;
    std::uint64_t den = 1;
    bool drop = false;
    if (!timecode_rate.empty())
    {
        auto text = timecode_rate;
        const auto upper = [](std::string s) {
            std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            return s;
        }(text);
        drop = upper.contains(" DF") || (upper.ends_with("DF") && !upper.ends_with("NDF"));
        const auto space = text.find(' ');
        if (space != std::string::npos)
            text.resize(space);
        const auto slash = text.find('/');
        try
        {
            num = std::stoull(text.substr(0, slash));
            den = slash == std::string::npos ? 1 : std::stoull(text.substr(slash + 1));
        } catch (const std::exception&)
        {
            num = 0;
        }
    }
    if (num == 0 || den == 0)
    {
        const auto ms_total = samples * 1000 / sample_rate;
        const auto ms = ms_total % 1000;
        const auto s = ms_total / 1000;
        return std::format("{:02}:{:02}:{:02}.{:03}", (s / 3600) % 24, (s / 60) % 60, s % 60, ms);
    }
    const auto nominal = static_cast<std::uint64_t>(std::llround(static_cast<double>(num) / static_cast<double>(den)));
    auto frames = samples * num / (static_cast<std::uint64_t>(sample_rate) * den);
    if (drop && (nominal == 30 || nominal == 60))
    {
        const std::uint64_t dropped = nominal == 30 ? 2 : 4;
        const auto per_10min = nominal * 600 - dropped * 9;
        const auto per_min = nominal * 60 - dropped;
        const auto tens = frames / per_10min;
        const auto rem = frames % per_10min;
        frames += dropped * 9 * tens + (rem > dropped ? dropped * ((rem - dropped) / per_min) : 0);
    }
    const auto ff = frames % nominal;
    const auto total_s = frames / nominal;
    return std::format("{:02}:{:02}:{:02}{}{:02}", (total_s / 3600) % 24, (total_s / 60) % 60, total_s % 60, drop ? ';' : ':', ff);
}

std::string shell_quote(const std::string& arg)
{
    if (!arg.empty() && arg.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_./=:,+@%") == std::string::npos)
        return arg;
    std::string out = "'";
    for (const char c : arg)
    {
        if (c == '\'')
            out += "'\\''";
        else
            out += c;
    }
    return out + "'";
}

namespace
{

std::string common_options(const Settings& s)
{
    std::string out;
    if (s.threads > 0)
        out += std::format(" --threads {}", s.threads);
    return out;
}

std::string path_arg(const std::filesystem::path& p)
{
    const auto u8 = p.u8string();
    return shell_quote(std::string(u8.begin(), u8.end()));
}

}

std::string cli_create_command(const Settings& s, const std::filesystem::path& archive, const std::vector<std::filesystem::path>& inputs)
{
    std::string cmd = "satchel create";
    if (!s.flac)
        cmd += " --no-flac";
    if (s.flac && s.flac_level != 5)
        cmd += std::format(" --flac-level {}", s.flac_level);
    if (s.deflate_level != 6)
        cmd += std::format(" --deflate-level {}", s.deflate_level);
    cmd += common_options(s);
    if (s.temp_folder)
        cmd += " --temp-dir " + path_arg(*s.temp_folder);
    cmd += " " + path_arg(archive);
    for (const auto& i : inputs)
        cmd += " " + path_arg(i);
    return cmd;
}

std::string cli_extract_command(const Settings& s, const std::filesystem::path& archive, const std::filesystem::path& destination)
{
    std::string cmd = "satchel extract";
    if (!s.restore_audio)
        cmd += " --keep-flac";
    if (s.include_readme)
        cmd += " --include-readme";
    if (s.overwrite != Overwrite::Ask)
        cmd += s.overwrite == Overwrite::Skip ? " --overwrite skip" : " --overwrite replace";
    cmd += common_options(s);
    cmd += " " + path_arg(archive) + " -d " + path_arg(destination);
    return cmd;
}

std::string human_size(std::uint64_t bytes)
{
    constexpr std::array<const char*, 5> units{ "B", "KB", "MB", "GB", "TB" };
    auto v = static_cast<double>(bytes);
    std::size_t u = 0;
    while (v >= 1000.0 && u + 1 < units.size())
    {
        v /= 1000.0;
        ++u;
    }
    return u == 0 ? std::format("{} B", bytes) : std::format("{:.1f} {}", v, units[u]);
}

std::string savings_text(std::uint64_t input, std::uint64_t output)
{
    const auto saved = percent_saved(input, output);
    return std::format("{} → {}, {}% {}", human_size(input), human_size(output), std::abs(saved), saved >= 0 ? "smaller" : "larger");
}

}
