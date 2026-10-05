// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#pragma once

#include "logic.hpp"

#include <QString>

#include <cstdint>
#include <string>

// Translatable display text for values the Qt-free logic library describes in English.
namespace labels
{

// "Store", "Deflate", "FLAC", "FLAC multi-mono", "Unsupported", "Symlink", "Other", "Folder";
// other text is shown as is.
QString method(const std::string& key);
// A row's method column ("FLAC channel 3/16" for multi-mono members).
QString row_method(const satchel_gui::Row& row);
// A row's name ("take.wav — 16 channels" for multi-mono groups).
QString row_name(const satchel_gui::Row& row);
// "NAME — N channels".
QString group_label(const QString& name, int channels);
// "4.2 GB → 2.3 GB, 45% smaller".
QString savings(std::uint64_t input, std::uint64_t output, bool ratio_known = true);

}
