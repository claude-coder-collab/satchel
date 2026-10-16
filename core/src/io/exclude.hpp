// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#pragma once

#include "common/status.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace zp
{

enum class ExcludePreset : std::uint8_t {
    System, // operating system clutter: .DS_Store, AppleDouble files, Thumbs.db, ...
    DawCaches, // regenerable waveform and analysis caches written by audio software
};

std::span<const std::string_view> exclude_preset(ExcludePreset preset);

// Name patterns for files and folders to leave out of an archive. A pattern matches one path
// component, never a path: '*' matches any run of characters, '?' one character, '[...]' one
// character from a set ('[a-z]', '[!0-9]'), and '\' makes the next character literal. A trailing
// '/' restricts the pattern to folders. Matching is case-insensitive and Unicode-normalized.
class ExcludeFilter
{
public:
    ExcludeFilter() = default;

    static Result<ExcludeFilter> parse(std::span<const std::string> patterns);
    static Result<ExcludeFilter> parse(std::span<const std::string_view> patterns);

    [[nodiscard]] bool matches(std::string_view name, bool directory) const;
    [[nodiscard]] bool empty() const { return patterns_.empty(); }

private:
    struct Pattern
    {
        std::u32string glob;
        bool directories_only = false;
    };

    std::vector<Pattern> patterns_;
};

}
