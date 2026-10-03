// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#pragma once

#include "codecs/flac/flac_format.hpp"
#include "codecs/pcm/pcm_container.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace zp
{

// Known production fields copied into Vorbis comments (main spec 7.2, "Mirrored tags").
// channel: 1-based channel of a multi-mono member, or nullopt for a multichannel file.
std::vector<std::pair<std::string, std::string>> mirror_tags(const std::vector<flac::ForeignRecord>& records, const PcmLayout& layout, std::optional<std::uint16_t> channel);

// Minimal extraction of a top-level element's text from iXML ("" if absent).
std::string ixml_text(std::string_view xml, std::string_view element);

}
