// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "codecs/codec.hpp"
#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <format>
#include <vector>

using namespace zp;

namespace
{

void mixed_tree(MemoryInputSource& in)
{
    in.add_directory("project", 1700000000LL * 1'000'000'000);
    in.add_directory("project/empty", 1700000001LL * 1'000'000'000, 0750);
    in.add_file("project/notes.txt", test::text_like(5000, 1), 1700000002LL * 1'000'000'000);
    in.add_file("project/big.bin", test::random_bytes((9u << 20) + 77, 2), 1700000003LL * 1'000'000'000);
    in.add_file("project/zero.bin", {}, 1700000004LL * 1'000'000'000);
    in.add_file("project/run.sh", test::text_bytes("#!/bin/sh\necho hi\n"), 1700000005LL * 1'000'000'000, 0755);
    in.add_file("project/\xE6\x97\xA5\xE6\x9C\xAC.txt", test::text_bytes("unicode"), 1700000006LL * 1'000'000'000);
}

}

TEST_CASE("plan, build, read, extract round-trip is byte-identical", "[integration]")
{
    MemoryInputSource in;
    mixed_tree(in);
    auto b = test::build(in);
    REQUIRE(b.result.status == Status::Ok);
    REQUIRE(b.result.per_entry.size() == 7);

    auto x = test::extract_all(b.zip);
    REQUIRE(x.result.status == Status::Ok);
    CHECK(x.result.files_written == 5);
    CHECK(x.result.directories_created == 2);

    auto items = in.enumerate().value();
    for (const auto& item : items)
    {
        if (item.kind == ItemKind::Directory)
        {
            REQUIRE(x.dirs.contains(item.archive_path));
            CHECK(x.dirs[item.archive_path].mtime == item.mtime_seconds());
            CHECK(x.dirs[item.archive_path].unix_mode == item.unix_mode);
            continue;
        }
        REQUIRE(x.files.contains(item.archive_path));
        const auto& f = x.files[item.archive_path];
        auto s = item.open().value();
        std::vector<std::uint8_t> original(static_cast<std::size_t>(item.size));
        REQUIRE(s->read_full(original).value() == item.size);
        CHECK(f.data == original);
        CHECK(f.mtime == item.mtime_seconds());
        CHECK(f.unix_mode == item.unix_mode);
    }
}

TEST_CASE("output is identical at every thread count", "[integration]")
{
    MemoryInputSource in;
    mixed_tree(in);
    const auto plan = test::plan_of(in);
    const auto one = test::build(plan, 1);
    REQUIRE(one.result.status == Status::Ok);
    for (const int t : { 2, 8 })
    {
        const auto many = test::build(plan, t);
        REQUIRE(many.result.status == Status::Ok);
        CHECK(many.zip == one.zip);
    }
}

TEST_CASE("seekable and non-seekable output carry the same entries", "[integration]")
{
    MemoryInputSource in;
    mixed_tree(in);
    const auto plan = test::plan_of(in);
    const auto seek = test::build(plan, 4, true);
    const auto stream = test::build(plan, 4, false);
    REQUIRE(seek.result.status == Status::Ok);
    REQUIRE(stream.result.status == Status::Ok);
    CHECK(seek.zip != stream.zip);
    auto a = test::extract_all(seek.zip);
    auto b = test::extract_all(stream.zip);
    REQUIRE(b.result.status == Status::Ok);
    CHECK(a.files.size() == b.files.size());
    for (const auto& [name, f] : a.files)
    {
        REQUIRE(b.files.contains(name));
        CHECK(b.files[name].data == f.data);
        CHECK(b.files[name].mtime == f.mtime);
    }
}

TEST_CASE("metadata comment identifies the archive", "[integration]")
{
    MemoryInputSource in;
    in.add_file("a", test::text_bytes("a"));
    auto b = test::build(in);
    MemoryStream s(b.zip);
    auto r = ArchiveReader::open(s).value();
    REQUIRE(r->metadata());
    CHECK(r->metadata()->app_version.starts_with("Satchel "));
    CHECK(r->metadata()->readme_name.empty());
}

TEST_CASE("SOURCE_CHANGED when a file changes between plan and build", "[integration]")
{
    MemoryInputSource in;
    in.add_file("a.txt", test::text_bytes("before"), 1);
    in.add_file("b.txt", test::text_bytes("stable"), 1);
    const auto plan = test::plan_of(in);
    in.replace_data("a.txt", test::text_bytes("after!!"), 2);
    auto b = test::build(plan);
    CHECK(b.result.status == Status::SourceChanged);
    REQUIRE(b.result.per_entry.size() == 2);
    CHECK(b.result.per_entry[0].status == Status::SourceChanged);
    auto x = test::extract_all(b.zip);
    CHECK(x.files.size() == 1);
    CHECK(x.files.contains("b.txt"));
}

TEST_CASE("cancellation stops the build", "[integration]")
{
    MemoryInputSource in;
    for (int i = 0; i < 20; ++i)
        in.add_file(std::format("f{}.bin", i), test::random_bytes(1u << 20, static_cast<std::uint32_t>(i)));
    const auto plan = test::plan_of(in);
    MemoryStream out;
    test::CancelAfter cancel(3);
    ArchiveBuilder builder(out, test::shared_context());
    auto r = builder.execute(plan, cancel);
    CHECK(r.status == Status::Cancelled);
}

TEST_CASE("buffered bytes stay within the memory budget", "[integration]")
{
    MemoryInputSource in;
    in.add_file("large.bin", test::random_bytes(40u << 20, 9));
    for (int i = 0; i < 2000; ++i)
        in.add_file(std::format("small/{:04}.txt", i), test::text_like(100 + static_cast<std::size_t>(i), static_cast<std::uint32_t>(i)));
    const auto plan = test::plan_of(in);
    constexpr std::uint64_t budget = 12u << 20;
    for (const int threads : { 1, 16 })
    {
        auto b = test::build(plan, threads, true, budget);
        REQUIRE(b.result.status == Status::Ok);
        CHECK(b.result.peak_buffered_bytes <= budget);
        CHECK(b.result.peak_buffered_bytes > 0);
    }
}

TEST_CASE("more than 65535 entries uses Zip64 end records", "[integration][zip64]")
{
    MemoryInputSource in;
    constexpr int count = 70000;
    for (int i = 0; i < count; ++i)
        in.add_file(std::format("d{:03}/f{:05}", i / 1000, i), {});
    auto b = test::build(in);
    REQUIRE(b.result.status == Status::Ok);
    CHECK(b.result.zip64);
    MemoryStream s(b.zip);
    auto r = ArchiveReader::open(s).value();
    CHECK(r->entries().size() == count);
    CHECK(r->entries().back().name == std::format("d{:03}/f{:05}", (count - 1) / 1000, count - 1));
}

TEST_CASE("deflate is chosen only when it pays off", "[integration][deflate]")
{
    MemoryInputSource in;
    in.add_file("text.txt", test::text_like(500000, 1));
    in.add_file("noise.bin", test::random_bytes(500000, 2));
    in.add_file("big-text.txt", test::text_like(9u << 20, 3));
    in.add_file("big-noise.bin", test::random_bytes(9u << 20, 4));
    in.add_file("tiny.txt", test::text_bytes("abc"));
    in.add_file("empty", {});
    auto b = test::build(in);
    REQUIRE(b.result.status == Status::Ok);
    const auto& r = b.result.per_entry;
    REQUIRE(r.size() == 6);
    CHECK(r[0].method == ZipMethod::Deflate);
    CHECK(r[0].compressed_size < r[0].uncompressed_size / 3);
    CHECK(r[1].method == ZipMethod::Store);
    CHECK(r[1].compressed_size == r[1].uncompressed_size);
    CHECK(r[2].method == ZipMethod::Deflate);
    CHECK(r[3].method == ZipMethod::Store);
    CHECK(r[4].method == ZipMethod::Store);
    CHECK(r[5].method == ZipMethod::Store);
    auto x = test::extract_all(b.zip);
    REQUIRE(x.result.status == Status::Ok);
    CHECK(x.files["big-text.txt"].data == test::text_like(9u << 20, 3));
    CHECK(x.files["big-noise.bin"].data == test::random_bytes(9u << 20, 4));
}

TEST_CASE("deflate level is honoured", "[integration][deflate]")
{
    MemoryInputSource in;
    in.add_file("text.txt", test::text_like(3u << 20, 5));
    PlannerOptions fast;
    fast.deflate_level = 1;
    PlannerOptions best;
    best.deflate_level = 9;
    auto a = test::build(in, fast);
    auto b = test::build(in, best);
    REQUIRE(a.result.status == Status::Ok);
    REQUIRE(b.result.status == Status::Ok);
    CHECK(b.result.per_entry[0].compressed_size < a.result.per_entry[0].compressed_size);
}

// Compressed bytes must be identical on every platform and CPU (main spec 6.1). The value is
// the CRC-32 over every entry's raw data in archive order.
TEST_CASE("compressed output matches the golden value", "[integration][deflate][golden]")
{
    MemoryInputSource in;
    in.add_file("a.txt", test::text_like(7u << 20, 11));
    in.add_file("b.txt", test::text_like(300000, 12));
    in.add_file("c.bin", test::random_bytes(2u << 20, 13));
    for (int level = 1; level <= 9; level += 4)
    {
        PlannerOptions o;
        o.deflate_level = level;
        auto b = test::build(in, o);
        REQUIRE(b.result.status == Status::Ok);
        MemoryStream s(b.zip);
        auto r = ArchiveReader::open(s).value();
        std::uint32_t crc = 0;
        for (std::size_t i = 0; i < r->entries().size(); ++i)
        {
            auto raw = r->open_raw(i).value();
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(r->entries()[i].compressed_size));
            REQUIRE(raw->read_full(bytes).value() == bytes.size());
            crc = crc32_update(crc, bytes);
        }
        UNSCOPED_INFO("level " << level << " golden crc " << std::hex << crc);
        const std::uint32_t expected = level == 1 ? 0x99ce4a23u : level == 5 ? 0x5f177051u
                                                                             : 0x9f8c29a7u;
        CHECK(crc == expected);
    }
}
