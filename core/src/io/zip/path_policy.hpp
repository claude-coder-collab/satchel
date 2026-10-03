// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#pragma once

#include "common/status.hpp"

#include <string>
#include <string_view>

namespace zp
{

class PathPolicy
{
public:
    // UTF-8 validation, NFC, '\' -> '/', leading "./" and trailing '/' removed.
    static Result<std::string> normalize(std::string_view path);
    // normalize() + Unicode case folding; equal keys collide on case-insensitive filesystems.
    static Result<std::string> collision_key(std::string_view path);
    // A normalized name: relative, no empty, "." or ".." segments, no drive letter, no NUL.
    static VoidResult validate_for_archive(std::string_view normalized);
    // Validates a (possibly hostile) entry name and returns the relative path to create under
    // dest_root. '\' counts as a separator; empty and "." segments are dropped.
    static Result<std::string> sanitize_for_extraction(std::string_view entry_name);
    static VoidResult validate_for_extraction(std::string_view dest_root, std::string_view entry_name);

    static bool is_valid_utf8(std::string_view s);
};

}
