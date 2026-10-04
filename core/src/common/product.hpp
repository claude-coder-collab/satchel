// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

// Placeholder product identity. Every value here is provisional and must be
// replaced before the first public release (see docs/IMPLEMENTATION.md).
namespace zp::product
{

inline constexpr std::string_view app_name = "Satchel";
inline constexpr std::string_view dearchiver_url = "https://claude-coder-collab.github.io/satchel/";

// EOCD comment magic (main spec 6.4). Placeholder, not registered anywhere.
inline constexpr std::array<std::uint8_t, 4> archive_magic{ 'S', 'T', 'C', 'H' };

// FLAC APPLICATION block ID (main spec 7.2). Placeholder until registered with Xiph.
inline constexpr std::array<std::uint8_t, 4> flac_application_id{ 'S', 't', 'c', 'h' };

std::string_view version() noexcept;

}
