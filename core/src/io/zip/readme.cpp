// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "io/zip/readme.hpp"

#include "common/product.hpp"

#include <algorithm>
#include <format>
#include <utility>

namespace zp
{

namespace
{

constexpr std::string_view template_text_v1 = "Why are the audio files in this archive FLAC?\n"
                                              "\n"
                                              "This archive was created with {APP_NAME} {APP_VERSION}.\n"
                                              "\n"
                                              "To make the transfer smaller, uncompressed audio files were losslessly\n"
                                              "compressed to FLAC. No audio quality has been lost. You can play and edit\n"
                                              "the FLAC files as they are.\n"
                                              "\n"
                                              "The original files - including all their metadata (timecode, scene/take\n"
                                              "information, markers and everything else) - are stored inside the FLAC\n"
                                              "files and can be restored bit-for-bit.\n"
                                              "\n"
                                              "To get your original files back:\n"
                                              "  1. Go to {DEARCHIVER_URL}\n"
                                              "  2. Open this .zip file there. It runs in your web browser, is free,\n"
                                              "     and needs no installation.\n"
                                              "  3. Extract. Your original files are restored and verified.\n"
                                              "\n"
                                              "Alternative for single .flac files (not *_chNN.flac sets): the free FLAC\n"
                                              "command-line tool (https://xiph.org/flac/, version 1.4.3 or later) can\n"
                                              "restore them:\n"
                                              "  flac -d --keep-foreign-metadata take1.flac\n"
                                              "\n"
                                              "Files that were converted:\n"
                                              "{FILE_LIST}";

void replace_all(std::string& s, std::string_view from, std::string_view to)
{
    for (auto pos = s.find(from); pos != std::string::npos; pos = s.find(from, pos + to.size()))
        s.replace(pos, from.size(), to);
}

std::string utf8_pad(std::string_view s, std::size_t width)
{
    std::size_t cps = 0;
    for (const char c : s)
        cps += (static_cast<unsigned char>(c) & 0xC0) != 0x80 ? 1 : 0;
    std::string out(s);
    if (cps < width)
        out.append(width - cps, ' ');
    return out;
}

std::size_t utf8_length(std::string_view s)
{
    return static_cast<std::size_t>(std::ranges::count_if(s, [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
}

}

std::string_view default_readme_template()
{
    return template_text_v1;
}

std::string render_readme(const ArchivePlan& plan, const std::set<std::size_t>& excluded, std::string_view template_text)
{
    std::vector<std::pair<std::string, std::string>> rows;
    for (std::size_t i = 0; i < plan.entries.size(); ++i)
    {
        const auto& e = plan.entries[i];
        if (!e.is_converted() || excluded.contains(i))
            continue;
        if (!e.group_id)
        {
            rows.emplace_back(e.output_name, e.restored_name());
            continue;
        }
        if (e.channel_index.value_or(1) != 1)
            continue;
        std::string last = e.output_name;
        for (std::size_t j = i + 1; j < plan.entries.size(); ++j)
        {
            const auto& m = plan.entries[j];
            if (m.is_converted() && m.group_id == e.group_id)
                last = m.output_name;
        }
        rows.emplace_back(std::format("{} ... {}", e.output_name, last), std::format("{} ({} channels)", e.restored_name(), e.channel_count));
    }
    std::size_t width = 0;
    for (const auto& [left, right] : rows)
        width = std::max(width, utf8_length(left));
    std::string list;
    for (const auto& [left, right] : rows)
        list += std::format("  {} -> {}\n", utf8_pad(left, width), right);

    std::string text(template_text.empty() ? default_readme_template() : template_text);
    replace_all(text, "{APP_NAME}", product::app_name);
    replace_all(text, "{APP_VERSION}", product::version());
    replace_all(text, "{DEARCHIVER_URL}", product::dearchiver_url);
    replace_all(text, "{FILE_LIST}", list);

    std::string crlf;
    crlf.reserve(text.size() + text.size() / 20);
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] == '\n' && (i == 0 || text[i - 1] != '\r'))
            crlf += '\r';
        crlf += text[i];
    }
    return crlf;
}

}
