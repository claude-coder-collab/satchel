// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "common/bytes.hpp"
#include "io/zip/zip_writer.hpp"
#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace zp;

namespace
{

struct LocalHeader
{
    std::uint16_t version_needed;
    std::uint16_t flags;
    std::uint16_t method;
    std::uint32_t crc;
    std::uint32_t csize;
    std::uint32_t usize;
    std::string name;
    std::vector<std::uint8_t> extra;
};

LocalHeader parse_local(const std::vector<std::uint8_t>& zip, std::size_t offset)
{
    ByteReader r(std::span(zip).subspan(offset));
    REQUIRE(r.u32le() == 0x04034b50u);
    LocalHeader h{};
    h.version_needed = *r.u16le();
    h.flags = *r.u16le();
    h.method = *r.u16le();
    r.skip(4);
    h.crc = *r.u32le();
    h.csize = *r.u32le();
    h.usize = *r.u32le();
    const auto n = *r.u16le();
    const auto m = *r.u16le();
    const auto name = *r.bytes(n);
    h.name.assign(name.begin(), name.end());
    const auto extra = *r.bytes(m);
    h.extra.assign(extra.begin(), extra.end());
    return h;
}

std::vector<std::uint8_t> write_one(IChunkedStream& out, const std::vector<std::uint8_t>& data)
{
    auto w = ZipWriter::open(out);
    REQUIRE(w);
    EntryHeader h{ "d\xC3\xA9j\xC3\xA0.txt", false, ZipMethod::Store, 0, 1700000000, 0755, data.size() };
    auto e = (*w)->begin_entry(h);
    REQUIRE(e);
    CHECK_FALSE(e->zip64);
    REQUIRE((*w)->write(data));
    REQUIRE((*w)->end_entry(crc32_update(0, data), data.size(), data.size()));
    REQUIRE((*w)->finish("comment"));
    return {};
}

}

TEST_CASE("seek-back path patches sizes and CRC into the local header", "[zip_writer]")
{
    const auto data = test::text_bytes("hello zip");
    MemoryStream out;
    write_one(out, data);
    const auto& zip = out.data();
    const auto h = parse_local(zip, 0);
    CHECK((h.flags & 0x0800) != 0);
    CHECK((h.flags & 0x0008) == 0);
    CHECK(h.method == 0);
    CHECK(h.crc == crc32_update(0, data));
    CHECK(h.csize == data.size());
    CHECK(h.usize == data.size());
    CHECK(h.name == "d\xC3\xA9j\xC3\xA0.txt");
    REQUIRE(h.extra.size() == 9);
    ByteReader x(h.extra);
    CHECK(x.u16le() == 0x5455);
    CHECK(x.u16le() == 5);
    CHECK(x.u8() == 1);
    CHECK(x.u32le() == 1700000000u);

    MemoryStream in(zip);
    auto r = ArchiveReader::open(in);
    REQUIRE(r);
    const auto& e = (*r)->entries()[0];
    CHECK(e.name == h.name);
    CHECK(e.unix_mode == 0755u);
    CHECK(e.mtime == 1700000000);
    CHECK(e.crc32 == crc32_update(0, data));
    CHECK((*r)->comment() == "comment");
}

TEST_CASE("data-descriptor path for non-seekable output", "[zip_writer]")
{
    const auto data = test::text_bytes("streamed");
    MemoryStream inner;
    NonSeekableStream out(inner);
    write_one(out, data);
    const auto& zip = inner.data();
    const auto h = parse_local(zip, 0);
    CHECK((h.flags & 0x0008) != 0);
    CHECK(h.crc == 0);
    CHECK(h.csize == 0);
    CHECK(h.usize == 0);
    const std::size_t data_at = 30 + h.name.size() + h.extra.size();
    ByteReader d(std::span(zip).subspan(data_at + data.size()));
    CHECK(d.u32le() == 0x08074b50u);
    CHECK(d.u32le() == crc32_update(0, data));
    CHECK(d.u32le() == data.size());
    CHECK(d.u32le() == data.size());

    MemoryStream in(zip);
    auto r = ArchiveReader::open(in);
    REQUIRE(r);
    CHECK((*r)->entries()[0].crc32 == crc32_update(0, data));
    CHECK((*r)->entries()[0].uncompressed_size == data.size());
}

TEST_CASE("unknown size reserves a Zip64 extra field", "[zip_writer]")
{
    MemoryStream out;
    auto w = ZipWriter::open(out);
    REQUIRE(w);
    EntryHeader h{ "big", false, ZipMethod::Store, 0, 0, 0644, std::nullopt };
    auto e = (*w)->begin_entry(h);
    REQUIRE(e);
    CHECK(e->zip64);
    REQUIRE((*w)->write(test::text_bytes("abc")));
    REQUIRE((*w)->end_entry(crc32_update(0, test::text_bytes("abc")), 3, 3));
    REQUIRE((*w)->finish({}));
    const auto lh = parse_local(out.data(), 0);
    CHECK(lh.csize == 0xFFFFFFFFu);
    CHECK(lh.version_needed == 45);
    ByteReader x(lh.extra);
    CHECK(x.u16le() == 0x0001);
    CHECK(x.u16le() == 16);
    CHECK(x.u64le() == 3);
    CHECK(x.u64le() == 3);

    MemoryStream in(out.data());
    auto r = ArchiveReader::open(in);
    REQUIRE(r);
    CHECK((*r)->entries()[0].uncompressed_size == 3);
    auto s = (*r)->open_entry(0);
    REQUIRE(s);
    std::array<std::uint8_t, 8> buf{};
    CHECK((*s)->read(buf.data(), buf.size()).value() == 3);
}

TEST_CASE("directory entries end with a slash and carry the directory attribute", "[zip_writer]")
{
    MemoryStream out;
    auto w = ZipWriter::open(out);
    REQUIRE(w);
    REQUIRE((*w)->begin_entry({ "folder", true, ZipMethod::Store, 0, 0, 0755, 0 }));
    REQUIRE((*w)->end_entry(0, 0, 0));
    REQUIRE((*w)->finish({}));
    CHECK(parse_local(out.data(), 0).name == "folder/");
    MemoryStream in(out.data());
    auto r = ArchiveReader::open(in);
    REQUIRE(r);
    CHECK((*r)->entries()[0].kind == ItemKind::Directory);
}
