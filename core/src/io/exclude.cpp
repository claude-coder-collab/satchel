// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "io/exclude.hpp"

#include <utf8proc.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <format>

namespace zp
{

namespace
{

constexpr std::array<std::string_view, 16> system_patterns{
    ".DS_Store",
    "._*",
    "__MACOSX/",
    ".Spotlight-V100/",
    ".Trashes/",
    ".fseventsd/",
    ".TemporaryItems/",
    ".DocumentRevisions-V100/",
    "Icon\r",
    "Thumbs.db",
    "ehthumbs.db",
    "desktop.ini",
    "$RECYCLE.BIN/",
    "System Volume Information/",
    "~$*",
    ".~lock.*#",
};

constexpr std::array<std::string_view, 6> daw_cache_patterns{
    "WaveCache.wfm",
    "*.asd",
    "*.reapeaks",
    "*.pkf",
    "*.gpk",
    "*.sfk",
};

// Case-folded NFC code points; names that are not UTF-8 fall back to bytes with ASCII folding.
std::u32string fold(std::string_view s)
{
    std::u32string out;
    utf8proc_uint8_t* mapped = nullptr;
    const auto options = static_cast<utf8proc_option_t>(UTF8PROC_STABLE | UTF8PROC_COMPOSE | UTF8PROC_CASEFOLD);
    const auto n = utf8proc_map(reinterpret_cast<const utf8proc_uint8_t*>(s.data()), static_cast<utf8proc_ssize_t>(s.size()), &mapped, options);
    if (n < 0)
    {
        for (const auto c : s)
        {
            const auto b = static_cast<unsigned char>(c);
            out.push_back(b >= 'A' && b <= 'Z' ? static_cast<char32_t>(b + ('a' - 'A')) : static_cast<char32_t>(b));
        }
        return out;
    }
    for (utf8proc_ssize_t i = 0; i < n;)
    {
        utf8proc_int32_t cp = 0;
        const auto len = utf8proc_iterate(mapped + i, n - i, &cp);
        if (len <= 0)
            break;
        out.push_back(static_cast<char32_t>(cp));
        i += len;
    }
    std::free(mapped);
    return out;
}

// Position just past the ']' closing the set that starts at p[at] == '[', or npos.
std::size_t set_end(std::u32string_view p, std::size_t at)
{
    auto i = at + 1;
    if (i < p.size() && (p[i] == U'!' || p[i] == U'^'))
        ++i;
    if (i < p.size() && p[i] == U']')
        ++i;
    for (; i < p.size(); ++i)
    {
        if (p[i] == U'\\')
            ++i;
        else if (p[i] == U']')
            return i + 1;
    }
    return std::u32string_view::npos;
}

bool in_set(std::u32string_view p, std::size_t at, std::size_t end, char32_t c)
{
    auto i = at + 1;
    const bool negated = p[i] == U'!' || p[i] == U'^';
    if (negated)
        ++i;
    const auto close = end - 1;
    const auto literal = [&](std::size_t& k) { return p[k] == U'\\' ? p[++k] : p[k]; };
    bool found = false;
    while (i < close)
    {
        const auto lo = literal(i);
        ++i;
        auto hi = lo;
        if (i + 1 < close && p[i] == U'-')
        {
            ++i;
            hi = literal(i);
            ++i;
        }
        found |= lo <= c && c <= hi;
    }
    return found != negated;
}

// Matches one pattern element at p[pi] against c; on success sets next to the following element.
bool match_one(std::u32string_view p, std::size_t pi, char32_t c, std::size_t& next)
{
    switch (p[pi])
    {
        case U'?':
            next = pi + 1;
            return true;
        case U'\\':
            next = pi + 2;
            return p[pi + 1] == c;
        case U'[':
            next = set_end(p, pi);
            return in_set(p, pi, next, c);
        default:
            next = pi + 1;
            return p[pi] == c;
    }
}

bool glob_match(std::u32string_view p, std::u32string_view s)
{
    std::size_t pi = 0;
    std::size_t si = 0;
    auto star_p = std::u32string_view::npos;
    std::size_t star_s = 0;
    while (si < s.size())
    {
        std::size_t next = 0;
        if (pi < p.size() && p[pi] == U'*')
        {
            star_p = ++pi;
            star_s = si;
        }
        else if (pi < p.size() && match_one(p, pi, s[si], next))
        {
            pi = next;
            ++si;
        }
        else if (star_p != std::u32string_view::npos)
        {
            pi = star_p;
            si = ++star_s;
        }
        else
            return false;
    }
    while (pi < p.size() && p[pi] == U'*')
        ++pi;
    return pi == p.size();
}

VoidResult validate(std::string_view raw, std::u32string_view p)
{
    const auto bad = [&](std::string_view why) { return fail(Status::InvalidArgument, std::format("invalid exclude pattern '{}': {}", raw, why)); };
    if (p.empty())
        return bad("empty");
    for (std::size_t i = 0; i < p.size(); ++i)
    {
        if (p[i] == U'/')
            return bad("patterns match a single name and cannot contain '/' except at the end");
        if (p[i] == 0)
            return bad("contains a NUL character");
        if (p[i] == U'\\')
        {
            if (i + 1 == p.size())
                return bad("ends with '\\'");
            ++i;
        }
        else if (p[i] == U'[')
        {
            const auto end = set_end(p, i);
            if (end == std::u32string_view::npos)
                return bad("unclosed '['");
            i = end - 1;
        }
    }
    return {};
}

template <typename S>
Result<ExcludeFilter> parse_all(std::span<const S> patterns, auto add)
{
    ExcludeFilter filter;
    for (const auto& raw : patterns)
    {
        std::string_view text = raw;
        const bool directories_only = text.ends_with('/');
        if (directories_only)
            text.remove_suffix(1);
        auto glob = fold(text);
        if (auto v = validate(raw, glob); !v)
            return std::unexpected(v.error());
        add(filter, std::move(glob), directories_only);
    }
    return filter;
}

}

std::span<const std::string_view> exclude_preset(ExcludePreset preset)
{
    switch (preset)
    {
        case ExcludePreset::System:
            return system_patterns;
        case ExcludePreset::DawCaches:
            return daw_cache_patterns;
    }
    return {};
}

Result<ExcludeFilter> ExcludeFilter::parse(std::span<const std::string> patterns)
{
    return parse_all(patterns, [](ExcludeFilter& f, std::u32string glob, bool dirs) { f.patterns_.push_back({ std::move(glob), dirs }); });
}

Result<ExcludeFilter> ExcludeFilter::parse(std::span<const std::string_view> patterns)
{
    return parse_all(patterns, [](ExcludeFilter& f, std::u32string glob, bool dirs) { f.patterns_.push_back({ std::move(glob), dirs }); });
}

bool ExcludeFilter::matches(std::string_view name, bool directory) const
{
    if (patterns_.empty())
        return false;
    const auto folded = fold(name);
    return std::ranges::any_of(patterns_, [&](const Pattern& p) { return (directory || !p.directories_only) && glob_match(p.glob, folded); });
}

}
