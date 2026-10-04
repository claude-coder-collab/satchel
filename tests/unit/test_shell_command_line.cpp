// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "command_line.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace satchel_shell;

TEST_CASE("Explorer command lines quote paths for CommandLineToArgvW", "[shell]")
{
    CHECK(quote_argument(L"C:\\Music\\take 1.wav") == L"\"C:\\Music\\take 1.wav\"");
    CHECK(quote_argument(L"C:\\") == L"\"C:\\\\\"");
    CHECK(quote_argument(L"a\"b") == L"\"a\\\"b\"");
    CHECK(quote_argument(L"a\\\"b") == L"\"a\\\\\\\"b\"");
    CHECK(quote_argument(L"") == L"\"\"");
}

TEST_CASE("Explorer commands pass the right items", "[shell]")
{
    const std::vector<std::wstring> items{ L"C:\\a.ZIP", L"C:\\b.wav" };
    CHECK(any_zip(items));
    CHECK_FALSE(any_zip({ L"C:\\b.wav", L"zip" }));
    CHECK(command_line(L"C:\\S\\satchel-gui.exe", L"--compress", items) == L"\"C:\\S\\satchel-gui.exe\" --compress \"C:\\a.ZIP\" \"C:\\b.wav\"");
    CHECK(command_line(L"C:\\S\\satchel-gui.exe", L"--extract", items) == L"\"C:\\S\\satchel-gui.exe\" --extract \"C:\\a.ZIP\"");
}
