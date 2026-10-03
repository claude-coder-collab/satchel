// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "io/file_system.hpp"
#include "io/stream.hpp"
#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace zp;

TEST_CASE("memory stream read/write/seek boundaries", "[stream]")
{
    MemoryStream s;
    REQUIRE(s.write_all(test::text_bytes("hello world")));
    CHECK(s.tell() == 11);
    REQUIRE(s.seek(6));
    std::array<std::uint8_t, 16> buf{};
    CHECK(s.read(buf.data(), buf.size()).value() == 5);
    CHECK(s.read(buf.data(), buf.size()).value() == 0);
    REQUIRE(s.seek(20));
    CHECK(s.read(buf.data(), 1).value() == 0);
    REQUIRE(s.write_all(test::text_bytes("!")));
    CHECK(s.size() == 21);
}

TEST_CASE("non-seekable wrapper hides seeking", "[stream]")
{
    MemoryStream inner;
    NonSeekableStream ns(inner);
    CHECK_FALSE(ns.seekable());
    CHECK_FALSE(ns.seek(0));
    CHECK_FALSE(ns.size());
    REQUIRE(ns.write_all(test::text_bytes("abc")));
    CHECK(ns.tell() == 3);
}

TEST_CASE("file stream round trip and atomic replace", "[stream]")
{
    test::TempDir dir;
    const auto target = dir.path() / "out.bin";
    dir.write("out.bin", test::text_bytes("original"));
    {
        auto s = AtomicFileStream::create(target);
        REQUIRE(s);
        REQUIRE((*s)->write_all(test::text_bytes("replaced")));
        CHECK(test::read_file(target) == test::text_bytes("original"));
        REQUIRE((*s)->commit());
    }
    CHECK(test::read_file(target) == test::text_bytes("replaced"));
    {
        auto s = AtomicFileStream::create(target);
        REQUIRE(s);
        REQUIRE((*s)->write_all(test::text_bytes("discarded")));
    }
    CHECK(test::read_file(target) == test::text_bytes("replaced"));
    std::size_t count = 0;
    for ([[maybe_unused]] const auto& e : std::filesystem::directory_iterator(dir.path()))
        ++count;
    CHECK(count == 1);

    auto f = FileStream::open(target, FileMode::Read);
    REQUIRE(f);
    CHECK((*f)->size() == 8);
    REQUIRE((*f)->seek(4));
    std::array<std::uint8_t, 8> buf{};
    CHECK((*f)->read(buf.data(), buf.size()).value() == 4);
}

TEST_CASE("stat_no_follow reports kinds and never follows links", "[stream]")
{
    test::TempDir dir;
    const auto file = dir.write("f.txt", test::text_bytes("12345"));
    dir.mkdir("d");
    auto st = stat_no_follow(file);
    REQUIRE(st);
    CHECK(st->kind == ItemKind::File);
    CHECK(st->size == 5);
    CHECK(stat_no_follow(dir.path() / "d")->kind == ItemKind::Directory);
    CHECK_FALSE(stat_no_follow(dir.path() / "missing"));
#ifndef _WIN32
    std::filesystem::create_symlink(file, dir.path() / "link");
    CHECK(stat_no_follow(dir.path() / "link")->kind == ItemKind::Symlink);
#endif
}
