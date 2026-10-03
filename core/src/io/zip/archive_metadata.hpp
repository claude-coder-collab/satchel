// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace zp
{

struct ArchiveMetadata
{
    static constexpr std::uint8_t current_schema = 1;
    static constexpr std::string_view comment_marker = "zpmeta:";

    std::uint32_t magic = 0;
    std::uint8_t schema_version = current_schema;
    std::string app_version;
    std::string readme_name;

    static ArchiveMetadata current(std::string readme_name = {});

    // Binary record (main spec 6.4).
    [[nodiscard]] std::vector<std::uint8_t> serialize() const;
    static std::optional<ArchiveMetadata> parse(std::span<const std::uint8_t> bytes);

    // The EOCD comment is printable ASCII: a human-readable line, then the marker and the
    // base64 binary record (see docs/IMPLEMENTATION.md).
    [[nodiscard]] std::string to_comment() const;
    static std::optional<ArchiveMetadata> from_comment(std::string_view comment);

    bool operator==(const ArchiveMetadata&) const = default;
};

}
