// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "common/bytes.hpp"
#include "io/zip/reader.hpp"
#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace zp;

namespace
{

std::unique_ptr<ArchiveReader> open_bytes(MemoryStream& s)
{
    auto r = ArchiveReader::open(s);
    REQUIRE(r);
    return std::move(*r);
}

}

TEST_CASE("names without the UTF-8 flag are decoded as CP437", "[reader]")
{
    MemoryStream s(test::craft_zip({ { .name = "caf\x82.txt", .data = test::text_bytes("x") }, { .name = "\x8e.txt", .data = {} } }));
    auto r = open_bytes(s);
    REQUIRE(r->entries().size() == 2);
    CHECK(r->entries()[0].name == "caf\xC3\xA9.txt");
    CHECK(r->entries()[1].name == "\xC3\x84.txt");
}

TEST_CASE("UTF-8 flag and Unicode Path extra field", "[reader]")
{
    const std::string utf8 = "\xC3\xA9t\xC3\xA9.txt";
    ByteWriter up;
    up.u16le(0x7075);
    const std::string raw = "ete.txt";
    up.u16le(static_cast<std::uint16_t>(1 + 4 + utf8.size()));
    up.u8(1);
    up.u32le(crc32_update(0, as_bytes(raw)));
    up.str(utf8);
    MemoryStream s(test::craft_zip({
        { .name = utf8, .flags = 0x0800 },
        { .name = raw, .extra = up.take() },
    }));
    auto r = open_bytes(s);
    CHECK(r->entries()[0].name == utf8);
    CHECK(r->entries()[1].name == utf8);
    CHECK(r->entries()[1].raw_name == raw);
}

TEST_CASE("symlinks, directories and modes from external attributes", "[reader]")
{
    MemoryStream s(test::craft_zip({
        { .name = "link", .data = test::text_bytes("target"), .external_attributes = (0120777u << 16) },
        { .name = "dir/", .external_attributes = (040755u << 16) | 0x10 },
        { .name = "dosdir", .version_made_by = 0x0014, .external_attributes = 0x10 },
        { .name = "tool", .data = test::text_bytes("#!"), .external_attributes = (0100755u << 16) },
        { .name = "plain", .version_made_by = 0x0014 },
    }));
    auto r = open_bytes(s);
    const auto& e = r->entries();
    CHECK(e[0].kind == ItemKind::Symlink);
    CHECK(e[1].kind == ItemKind::Directory);
    CHECK(e[1].unix_mode == 0755u);
    CHECK(e[2].kind == ItemKind::Directory);
    CHECK(e[3].kind == ItemKind::File);
    CHECK(e[3].unix_mode == 0755u);
    CHECK_FALSE(e[4].unix_mode);
}

TEST_CASE("unsupported and encrypted methods are reported", "[reader]")
{
    MemoryStream s(test::craft_zip({
        { .name = "lzma.bin", .data = test::random_bytes(10, 1), .method = 14 },
        { .name = "secret.txt", .data = test::random_bytes(10, 2), .flags = 1 },
    }));
    auto r = open_bytes(s);
    CHECK(r->entries()[0].method == EntryMethod::Unsupported);
    CHECK(r->entries()[0].raw_method == 14);
    CHECK(r->entries()[1].method == EntryMethod::Unsupported);
    CHECK(r->entries()[1].encrypted);
    CHECK(r->open_entry(0).error().status == Status::UnsupportedMethod);
}

TEST_CASE("extended timestamp takes precedence over the DOS time", "[reader]")
{
    ByteWriter ext;
    ext.u16le(0x5455);
    ext.u16le(5);
    ext.u8(1);
    ext.u32le(1700000000u);
    MemoryStream s(test::craft_zip({ { .name = "t", .extra = ext.take() } }));
    auto r = open_bytes(s);
    CHECK(r->entries()[0].mtime == 1700000000);
}

TEST_CASE("garbage and truncated archives are rejected", "[reader]")
{
    MemoryStream junk(test::random_bytes(1000, 5));
    CHECK(ArchiveReader::open(junk).error().status == Status::CorruptArchive);
    MemoryStream empty;
    CHECK_FALSE(ArchiveReader::open(empty));
    auto zip = test::craft_zip({ { .name = "a", .data = test::random_bytes(100, 1) } });
    zip.erase(zip.begin() + 10, zip.begin() + 60);
    MemoryStream cut(zip);
    auto r = ArchiveReader::open(cut);
    if (r)
        CHECK_FALSE(((*r)->open_entry(0).has_value() && (*r)->entries()[0].uncompressed_size == 100));
}

TEST_CASE("foreign comments do not produce metadata", "[reader]")
{
    MemoryStream s(test::craft_zip({ { .name = "a" } }, "made by someone else"));
    auto r = open_bytes(s);
    CHECK(r->comment() == "made by someone else");
    CHECK_FALSE(r->metadata());
}
