// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#pragma once

#include <algorithm>
#include <cwctype>
#include <string>
#include <vector>

namespace satchel_shell
{

// Quotes one argument so CommandLineToArgvW (and the MSVC runtime) reads it back unchanged.
inline std::wstring quote_argument(const std::wstring& arg)
{
    std::wstring out = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t c : arg)
    {
        if (c == L'\\')
        {
            ++backslashes;
            continue;
        }
        if (c == L'"')
            out.append(backslashes * 2 + 1, L'\\');
        else
            out.append(backslashes, L'\\');
        backslashes = 0;
        out += c;
    }
    out.append(backslashes * 2, L'\\');
    out += L'"';
    return out;
}

inline bool is_zip(const std::wstring& path)
{
    if (path.size() < 4)
        return false;
    std::wstring ext = path.substr(path.size() - 4);
    std::ranges::transform(ext, ext.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return ext == L".zip";
}

inline bool any_zip(const std::vector<std::wstring>& paths)
{
    return std::ranges::any_of(paths, is_zip);
}

// `"exe" --verb "path1" "path2"…`; for --extract only the zip files are passed.
inline std::wstring command_line(const std::wstring& exe, const std::wstring& verb, const std::vector<std::wstring>& paths)
{
    std::wstring line = quote_argument(exe) + L" " + verb;
    for (const auto& p : paths)
    {
        if (verb == L"--extract" && !is_zip(p))
            continue;
        line += L" " + quote_argument(p);
    }
    return line;
}

}
