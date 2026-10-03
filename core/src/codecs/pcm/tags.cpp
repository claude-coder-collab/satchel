// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "codecs/pcm/tags.hpp"

#include "io/zip/path_policy.hpp"

#include <algorithm>
#include <cstring>
#include <format>
#include <map>

namespace zp
{

namespace
{

struct Chunk
{
    std::string id;
    std::span<const std::uint8_t> body;
};

std::uint32_t le32(const std::uint8_t* p)
{
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint32_t be32(const std::uint8_t* p)
{
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) | (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}

std::uint64_t le64(const std::uint8_t* p)
{
    return static_cast<std::uint64_t>(le32(p)) | (static_cast<std::uint64_t>(le32(p + 4)) << 32);
}

std::vector<Chunk> chunks_of(const std::vector<flac::ForeignRecord>& records, const PcmLayout& layout)
{
    std::vector<Chunk> out;
    for (std::size_t i = 1; i < records.size(); ++i)
    {
        const auto& b = records[i].bytes;
        std::size_t header = 8;
        std::uint64_t size = 0;
        switch (layout.container)
        {
            case PcmContainer::Wav:
            case PcmContainer::Rf64:
                if (b.size() < 8)
                    continue;
                size = le32(b.data() + 4);
                break;
            case PcmContainer::Aiff:
            case PcmContainer::Aifc:
                if (b.size() < 8)
                    continue;
                size = be32(b.data() + 4);
                break;
            case PcmContainer::Wave64:
                if (b.size() < 24)
                    continue;
                header = 24;
                size = le64(b.data() + 16) - 24;
                break;
            case PcmContainer::Caf:
                continue;
        }
        const auto body_size = static_cast<std::size_t>(std::min<std::uint64_t>(size, b.size() - header));
        out.push_back({ std::string(reinterpret_cast<const char*>(b.data()), 4), std::span<const std::uint8_t>(b).subspan(header, body_size) });
    }
    return out;
}

std::string latin1_or_utf8(std::span<const std::uint8_t> raw)
{
    std::string s(raw.begin(), raw.end());
    if (const auto nul = s.find('\0'); nul != std::string::npos)
        s.resize(nul);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\n' || s.back() == '\t'))
        s.pop_back();
    if (PathPolicy::is_valid_utf8(s))
        return s;
    std::string out;
    for (const char c : s)
    {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x80)
            out += c;
        else
        {
            out += static_cast<char>(0xC0 | (u >> 6));
            out += static_cast<char>(0x80 | (u & 0x3F));
        }
    }
    return out;
}

std::string xml_unescape(std::string_view s)
{
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] != '&')
        {
            out += s[i];
            continue;
        }
        const auto end = s.find(';', i);
        if (end == std::string_view::npos)
        {
            out += s[i];
            continue;
        }
        const auto ent = s.substr(i + 1, end - i - 1);
        if (ent == "amp")
            out += '&';
        else if (ent == "lt")
            out += '<';
        else if (ent == "gt")
            out += '>';
        else if (ent == "quot")
            out += '"';
        else if (ent == "apos")
            out += '\'';
        else
        {
            out += s.substr(i, end - i + 1);
        }
        i = end;
    }
    return out;
}

std::string trim(const std::string& s)
{
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

}

std::string ixml_text(std::string_view xml, std::string_view element)
{
    const auto open = std::format("<{}>", element);
    const auto close = std::format("</{}>", element);
    const auto a = xml.find(open);
    if (a == std::string_view::npos)
        return {};
    const auto b = xml.find(close, a + open.size());
    if (b == std::string_view::npos)
        return {};
    return trim(xml_unescape(xml.substr(a + open.size(), b - a - open.size())));
}

std::vector<std::pair<std::string, std::string>> mirror_tags(const std::vector<flac::ForeignRecord>& records, const PcmLayout& layout, std::optional<std::uint16_t> channel)
{
    std::vector<std::pair<std::string, std::string>> tags;
    const auto add = [&](std::string name, std::string value) {
        if (!value.empty() && PathPolicy::is_valid_utf8(value))
            tags.emplace_back(std::move(name), std::move(value));
    };
    std::map<std::uint16_t, std::string> track_names;

    for (const auto& c : chunks_of(records, layout))
    {
        if (c.id == "bext" && c.body.size() >= 346)
        {
            const auto* p = c.body.data();
            add("DESCRIPTION", latin1_or_utf8(c.body.subspan(0, 256)));
            add("ORIGINATOR", latin1_or_utf8(c.body.subspan(256, 32)));
            add("ORIGINATOR_REFERENCE", latin1_or_utf8(c.body.subspan(288, 32)));
            const auto date = latin1_or_utf8(c.body.subspan(320, 10));
            const auto time = latin1_or_utf8(c.body.subspan(330, 8));
            if (date.size() == 10)
            {
                std::string iso = date;
                for (const auto pos : { 4u, 7u })
                    iso[pos] = '-';
                if (time.size() == 8)
                {
                    std::string t = time;
                    t[2] = ':';
                    t[5] = ':';
                    iso += "T" + t;
                }
                add("DATE", iso);
            }
            add("ORIGINATION_DATE", date);
            add("ORIGINATION_TIME", time);
            const auto ref = static_cast<std::uint64_t>(le32(p + 338)) | (static_cast<std::uint64_t>(le32(p + 342)) << 32);
            add("TIME_REFERENCE", std::to_string(ref));
        }
        else if (c.id == "iXML" || c.id == "ixml")
        {
            const std::string xml(c.body.begin(), c.body.end());
            for (const auto* field : { "PROJECT", "SCENE", "TAKE", "TAPE", "NOTE", "CIRCLED" })
                add(field, ixml_text(xml, field));
            std::size_t pos = 0;
            while ((pos = xml.find("<TRACK>", pos)) != std::string::npos)
            {
                const auto end = xml.find("</TRACK>", pos);
                if (end == std::string::npos)
                    break;
                const auto track = std::string_view(xml).substr(pos, end - pos);
                const auto index = ixml_text(track, "CHANNEL_INDEX");
                const auto name = ixml_text(track, "NAME");
                if (!index.empty() && !name.empty() && std::ranges::all_of(index, [](char ch) { return ch >= '0' && ch <= '9'; }) && index.size() < 6)
                    track_names[static_cast<std::uint16_t>(std::stoi(index))] = name;
                pos = end;
            }
        }
        else if (c.id == "LIST" && c.body.size() >= 4 && std::memcmp(c.body.data(), "INFO", 4) == 0)
        {
            std::size_t pos = 4;
            while (pos + 8 <= c.body.size())
            {
                const std::string sub(reinterpret_cast<const char*>(c.body.data() + pos), 4);
                const auto len = le32(c.body.data() + pos + 4);
                if (pos + 8 + len > c.body.size())
                    break;
                const auto text = latin1_or_utf8(c.body.subspan(pos + 8, len));
                if (sub == "INAM")
                    add("TITLE", text);
                else if (sub == "IART")
                    add("ARTIST", text);
                else if (sub == "ICMT")
                    add("COMMENT", text);
                pos += 8 + len + (len & 1);
            }
        }
        else if (c.id == "NAME")
            add("TITLE", latin1_or_utf8(c.body));
        else if (c.id == "AUTH")
            add("ARTIST", latin1_or_utf8(c.body));
        else if (c.id == "ANNO")
            add("COMMENT", latin1_or_utf8(c.body));
    }

    if (channel)
    {
        if (auto it = track_names.find(*channel); it != track_names.end())
            add("TRACK_NAME", it->second);
        add("CHANNEL", std::to_string(*channel));
        add("CHANNELS", std::to_string(layout.channels));
    }
    else
    {
        for (const auto& [index, name] : track_names)
        {
            if (index >= 1 && index <= layout.channels)
                add(std::format("TRACK_NAME_{:02}", index), name);
        }
    }
    return tags;
}

}
