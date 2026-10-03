// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "io/zip/path_policy.hpp"

#include "io/file_system.hpp"

#include <cstdlib>
#include <format>
#include <utf8proc.h>
#include <vector>

namespace zp
{

namespace
{

Result<std::string> utf8_map(std::string_view s, int options)
{
    utf8proc_uint8_t* out = nullptr;
    const auto n = utf8proc_map(reinterpret_cast<const utf8proc_uint8_t*>(s.data()), static_cast<utf8proc_ssize_t>(s.size()), &out, static_cast<utf8proc_option_t>(options));
    if (n < 0)
        return fail(Status::InvalidName, std::format("invalid UTF-8 in name: {}", utf8proc_errmsg(n)));
    std::string result(reinterpret_cast<const char*>(out), static_cast<std::size_t>(n));
    std::free(out);
    return result;
}

bool has_drive_letter(std::string_view s)
{
    return s.size() >= 2 && s[1] == ':' && ((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z'));
}

std::vector<std::string_view> split(std::string_view s)
{
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (true)
    {
        const auto pos = s.find('/', start);
        parts.push_back(s.substr(start, pos == std::string_view::npos ? std::string_view::npos : pos - start));
        if (pos == std::string_view::npos)
            break;
        start = pos + 1;
    }
    return parts;
}

}

bool PathPolicy::is_valid_utf8(std::string_view s)
{
    const auto* p = reinterpret_cast<const utf8proc_uint8_t*>(s.data());
    auto left = static_cast<utf8proc_ssize_t>(s.size());
    while (left > 0)
    {
        utf8proc_int32_t cp = 0;
        const auto n = utf8proc_iterate(p, left, &cp);
        if (n <= 0 || cp < 0)
            return false;
        p += n;
        left -= n;
    }
    return true;
}

Result<std::string> PathPolicy::normalize(std::string_view path)
{
    if (path.contains('\0'))
        return fail(Status::InvalidName, "name contains a NUL character");
    auto nfc = utf8_map(path, UTF8PROC_STABLE | UTF8PROC_COMPOSE);
    if (!nfc)
        return nfc;
    std::string s = std::move(*nfc);
    for (auto& c : s)
    {
        if (c == '\\')
            c = '/';
    }
    while (s.starts_with("./"))
        s.erase(0, 2);
    while (s.size() > 1 && s.ends_with('/'))
        s.pop_back();
    return s;
}

Result<std::string> PathPolicy::collision_key(std::string_view path)
{
    auto n = normalize(path);
    if (!n)
        return n;
    return utf8_map(*n, UTF8PROC_STABLE | UTF8PROC_COMPOSE | UTF8PROC_CASEFOLD);
}

VoidResult PathPolicy::validate_for_archive(std::string_view normalized)
{
    const auto s = normalized;
    if (s.empty())
        return fail(Status::InvalidName, "empty name");
    if (s.size() > 0xFFFF)
        return fail(Status::InvalidName, "name longer than 65535 bytes");
    if (!is_valid_utf8(s))
        return fail(Status::InvalidName, "name is not valid UTF-8");
    if (s.contains('\0'))
        return fail(Status::InvalidName, "name contains a NUL character");
    if (s.contains('\\'))
        return fail(Status::InvalidName, std::format("'{}' contains a backslash", s));
    if (s.front() == '/')
        return fail(Status::InvalidName, std::format("'{}' is an absolute path", s));
    if (has_drive_letter(s))
        return fail(Status::InvalidName, std::format("'{}' starts with a drive letter", s));
    for (const auto part : split(s))
    {
        if (part.empty())
            return fail(Status::InvalidName, std::format("'{}' has an empty path segment", s));
        if (part == "." || part == "..")
            return fail(Status::InvalidName, std::format("'{}' has a '{}' segment", s, part));
    }
    return {};
}

Result<std::string> PathPolicy::sanitize_for_extraction(std::string_view entry_name)
{
    std::string s(entry_name);
    if (s.contains('\0'))
        return fail(Status::UnsafePath, "entry name contains a NUL character");
    for (auto& c : s)
    {
        if (c == '\\')
            c = '/';
    }
    if (s.starts_with('/'))
        return fail(Status::UnsafePath, std::format("'{}' is an absolute path", entry_name));
    if (has_drive_letter(s))
        return fail(Status::UnsafePath, std::format("'{}' starts with a drive letter", entry_name));
    std::string out;
    for (const auto part : split(s))
    {
        if (part.empty() || part == ".")
            continue;
        if (part == "..")
            return fail(Status::UnsafePath, std::format("'{}' escapes the destination", entry_name));
        if (part.contains(':'))
            return fail(Status::UnsafePath, std::format("'{}' contains ':' (drive or alternate data stream)", entry_name));
        if (!out.empty())
            out += '/';
        out += part;
    }
    if (out.empty())
        return fail(Status::UnsafePath, std::format("'{}' has no usable name", entry_name));
    return out;
}

VoidResult PathPolicy::validate_for_extraction(std::string_view dest_root, std::string_view entry_name)
{
    auto rel = sanitize_for_extraction(entry_name);
    if (!rel)
        return std::unexpected(rel.error());
    const auto root = path_from_utf8(dest_root).lexically_normal();
    const auto full = (root / path_from_utf8(*rel)).lexically_normal();
    const auto back = full.lexically_relative(root);
    if (back.empty() || back.native().starts_with(std::filesystem::path("..").native()))
        return fail(Status::UnsafePath, std::format("'{}' escapes the destination", entry_name));
    return {};
}

}
