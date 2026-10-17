// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "io/exclude.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace zp;

namespace
{

ExcludeFilter filter(std::vector<std::string> patterns)
{
    auto f = ExcludeFilter::parse(patterns);
    REQUIRE(f);
    return *f;
}

}

TEST_CASE("exclude globs match whole names", "[unit][exclude]")
{
    const auto f = filter({ "*.tmp", "._*", "a?c", "file[0-9].txt", "[!x]y", "Thumbs.db" });
    CHECK(f.matches("x.tmp", false));
    CHECK(f.matches(".tmp", false));
    CHECK_FALSE(f.matches("x.tmp.keep", false));
    CHECK(f.matches("._song.wav", false));
    CHECK_FALSE(f.matches("song._wav", false));
    CHECK(f.matches("abc", false));
    CHECK_FALSE(f.matches("abbc", false));
    CHECK(f.matches("file7.txt", false));
    CHECK_FALSE(f.matches("fileA.txt", false));
    CHECK(f.matches("ay", false));
    CHECK_FALSE(f.matches("xy", false));
    CHECK(f.matches("Thumbs.db", true));
    CHECK_FALSE(f.matches("", false));
}

TEST_CASE("exclude matching is case-insensitive and normalization-insensitive", "[unit][exclude]")
{
    const auto f = filter({ "thumbs.db", "café*", "Å?" });
    CHECK(f.matches("THUMBS.DB", false));
    CHECK(f.matches("Café notes", false));
    CHECK(f.matches("åx", false));
    CHECK_FALSE(f.matches("cafe", false));
}

TEST_CASE("trailing slash restricts a pattern to folders", "[unit][exclude]")
{
    const auto f = filter({ "__MACOSX/", "build" });
    CHECK(f.matches("__MACOSX", true));
    CHECK_FALSE(f.matches("__MACOSX", false));
    CHECK(f.matches("build", true));
    CHECK(f.matches("build", false));
}

TEST_CASE("escapes and sets with special characters", "[unit][exclude]")
{
    const auto f = filter({ "\\*star", "[]x]", "q\\?", "[a\\-]z" });
    CHECK(f.matches("*star", false));
    CHECK_FALSE(f.matches("bigstar", false));
    CHECK(f.matches("]", false));
    CHECK(f.matches("x", false));
    CHECK(f.matches("q?", false));
    CHECK_FALSE(f.matches("qq", false));
    CHECK(f.matches("-z", false));
    CHECK_FALSE(f.matches("bz", false));
}

TEST_CASE("names that are not UTF-8 still match", "[unit][exclude]")
{
    const auto f = filter({ "*.TMP" });
    CHECK(f.matches(std::string("\xff\xfe.tmp"), false));
}

TEST_CASE("invalid exclude patterns are rejected", "[unit][exclude]")
{
    for (const auto& bad : std::vector<std::string>{ "", "/", "a/b", "a//", "x\\", "[abc", "[]", std::string("a\0b", 3) })
    {
        const std::vector<std::string> one{ bad };
        CHECK_FALSE(ExcludeFilter::parse(one));
    }
}

TEST_CASE("presets parse and cover the usual clutter", "[unit][exclude]")
{
    auto sys = ExcludeFilter::parse(exclude_preset(ExcludePreset::System));
    REQUIRE(sys);
    for (const auto* name : { ".DS_Store", "._take1.wav", "Thumbs.db", "desktop.ini", "~$report.docx", "Icon\r" })
        CHECK(sys->matches(name, false));
    for (const auto* name : { "__MACOSX", ".Spotlight-V100", ".Trashes", "$RECYCLE.BIN", "System Volume Information" })
        CHECK(sys->matches(name, true));
    for (const auto* name : { "take1.wav", ".gitignore", "Icon", "notes.txt", "session.ptx" })
        CHECK_FALSE(sys->matches(name, false));

    auto daw = ExcludeFilter::parse(exclude_preset(ExcludePreset::DawCaches));
    REQUIRE(daw);
    for (const auto* name : { "WaveCache.wfm", "kick.wav.asd", "kick.wav.reapeaks", "vox.pkf" })
        CHECK(daw->matches(name, false));
    CHECK_FALSE(daw->matches("kick.wav", false));
}
