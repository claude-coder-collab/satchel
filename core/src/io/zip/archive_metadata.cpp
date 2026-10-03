// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "io/zip/archive_metadata.hpp"

#include "common/bytes.hpp"
#include "common/product.hpp"

#include <algorithm>
#include <format>

namespace zp
{

namespace
{

std::uint32_t magic_value()
{
    const auto& m = product::archive_magic;
    return (static_cast<std::uint32_t>(m[0]) << 24) | (static_cast<std::uint32_t>(m[1]) << 16) | (static_cast<std::uint32_t>(m[2]) << 8) | m[3];
}

bool printable_ascii(std::string_view s)
{
    return std::ranges::all_of(s, [](char c) { return c >= 0x20 && c < 0x7F; });
}

}

ArchiveMetadata ArchiveMetadata::current(std::string readme_name)
{
    ArchiveMetadata m;
    m.magic = magic_value();
    m.app_version = std::format("{} {}", product::app_name, product::version());
    m.readme_name = std::move(readme_name);
    return m;
}

std::vector<std::uint8_t> ArchiveMetadata::serialize() const
{
    ByteWriter w;
    w.u32be(magic);
    w.u8(schema_version);
    w.u16le(static_cast<std::uint16_t>(app_version.size()));
    w.str(app_version);
    w.u16le(static_cast<std::uint16_t>(readme_name.size()));
    w.str(readme_name);
    return w.take();
}

std::optional<ArchiveMetadata> ArchiveMetadata::parse(std::span<const std::uint8_t> bytes)
{
    ByteReader r(bytes);
    ArchiveMetadata m;
    const auto magic = r.u32be();
    if (!magic || *magic != magic_value())
        return std::nullopt;
    m.magic = *magic;
    const auto schema = r.u8();
    if (!schema || *schema == 0)
        return std::nullopt;
    m.schema_version = *schema;
    const auto read_string = [&r](std::string& out) {
        const auto len = r.u16le();
        if (!len)
            return false;
        const auto b = r.bytes(*len);
        if (!b)
            return false;
        out.assign(b->begin(), b->end());
        return true;
    };
    if (!read_string(m.app_version) || !read_string(m.readme_name))
        return std::nullopt;
    return m;
}

std::string ArchiveMetadata::to_comment() const
{
    std::string head = printable_ascii(app_version) ? std::format("Created with {}. ", app_version) : std::string{};
    return head + std::string(comment_marker) + base64_encode(serialize());
}

std::optional<ArchiveMetadata> ArchiveMetadata::from_comment(std::string_view comment)
{
    const auto pos = comment.rfind(comment_marker);
    if (pos == std::string_view::npos)
        return std::nullopt;
    auto payload = comment.substr(pos + comment_marker.size());
    while (!payload.empty() && (payload.back() == '\r' || payload.back() == '\n' || payload.back() == ' '))
        payload.remove_suffix(1);
    const auto bytes = base64_decode(payload);
    if (!bytes)
        return std::nullopt;
    return parse(*bytes);
}

}
