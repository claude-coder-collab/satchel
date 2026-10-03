// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "io/file_system.hpp"
#include "io/zip/editor.hpp"
#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <optional>
#include <vector>

using namespace zp;

namespace
{

std::vector<std::uint8_t> original_archive()
{
    MemoryInputSource in;
    in.add_directory("docs", 1'000'000'000);
    in.add_file("docs/a.txt", test::text_like(30000, 1), 2'000'000'000);
    in.add_file("docs/b.bin", test::random_bytes(70000, 2), 3'000'000'000);
    in.add_file("c.txt", test::text_bytes("ccc"), 4'000'000'000);
    auto b = test::build(in);
    REQUIRE(b.result.status == Status::Ok);
    return b.zip;
}

std::vector<std::uint8_t> raw_of(ArchiveReader& r, std::size_t i)
{
    auto s = r.open_raw(i).value();
    std::vector<std::uint8_t> out(static_cast<std::size_t>(r.entries()[i].compressed_size));
    REQUIRE(s->read_full(out).value() == out.size());
    return out;
}

std::size_t index_of(ArchiveReader& r, const std::string& name)
{
    for (std::size_t i = 0; i < r.entries().size(); ++i)
    {
        if (r.entries()[i].name == name)
            return i;
    }
    FAIL("missing " << name);
    return 0;
}

std::vector<std::uint8_t> commit(ArchiveEditor& ed)
{
    MemoryStream out;
    ProgressSink progress;
    auto r = ed.commit(out, progress);
    REQUIRE(r.status == Status::Ok);
    return out.data();
}

}

TEST_CASE("editor operations", "[editor]")
{
    const auto zip = original_archive();
    MemoryStream in(zip);
    ArchiveEditor ed(test::shared_context());
    REQUIRE(ed.open(in));
    REQUIRE(ed.plan().entries.size() == 4);

    SECTION("no changes keeps entries byte-identical")
    {
        const auto out = commit(ed);
        MemoryStream a(zip);
        MemoryStream b(out);
        auto ra = ArchiveReader::open(a).value();
        auto rb = ArchiveReader::open(b).value();
        REQUIRE(ra->entries().size() == rb->entries().size());
        for (std::size_t i = 0; i < ra->entries().size(); ++i)
        {
            CHECK(ra->entries()[i].name == rb->entries()[i].name);
            CHECK(ra->entries()[i].crc32 == rb->entries()[i].crc32);
            CHECK(ra->entries()[i].mtime == rb->entries()[i].mtime);
            CHECK(raw_of(*ra, i) == raw_of(*rb, i));
        }
    }
    SECTION("remove")
    {
        REQUIRE(ed.remove(3));
        auto x = test::extract_all(commit(ed));
        CHECK_FALSE(x.files.contains("c.txt"));
        CHECK(x.files.size() == 2);
    }
    SECTION("rename")
    {
        REQUIRE(ed.rename(1, "docs/renamed.txt"));
        auto x = test::extract_all(commit(ed));
        CHECK(x.files.contains("docs/renamed.txt"));
        CHECK(x.files["docs/renamed.txt"].data == test::text_like(30000, 1));
        CHECK_FALSE(ed.rename(1, "../bad"));
    }
    SECTION("add and replace combined")
    {
        MemoryInputSource more;
        more.add_file("new.txt", test::text_bytes("new"));
        REQUIRE(ed.add(more));
        InputItem replacement = more.enumerate().value().front();
        replacement.archive_path = "c.txt";
        REQUIRE(ed.replace(3, replacement));
        REQUIRE(ed.remove(2));
        auto x = test::extract_all(commit(ed));
        CHECK(x.files["new.txt"].data == test::text_bytes("new"));
        CHECK(x.files["c.txt"].data == test::text_bytes("new"));
        CHECK_FALSE(x.files.contains("docs/b.bin"));
        CHECK(x.files["docs/a.txt"].data == test::text_like(30000, 1));
    }
    SECTION("conflicting addition blocks commit")
    {
        MemoryInputSource more;
        more.add_file("C.TXT", test::text_bytes("dup"));
        REQUIRE(ed.add(more));
        CHECK_FALSE(ed.plan().executable());
        MemoryStream out;
        ProgressSink progress;
        CHECK(ed.commit(out, progress).status == Status::ConflictsUnresolved);
    }
    SECTION("out of range")
    {
        CHECK_FALSE(ed.remove(99));
        CHECK_FALSE(ed.rename(99, "x"));
    }
}

TEST_CASE("kept entries from third-party archives stay byte-identical", "[editor]")
{
    const auto zip = test::craft_zip({
        { .name = "stored.txt", .data = test::text_bytes("abc") },
        { .name = "caf\x82.txt", .data = test::text_bytes("cp437") },
        { .name = "link", .data = test::text_bytes("x"), .external_attributes = 0120777u << 16 },
    });
    MemoryStream in(zip);
    ArchiveEditor ed(test::shared_context());
    REQUIRE(ed.open(in));
    CHECK(ed.plan().warnings.size() == 1);
    const auto out = commit(ed);
    MemoryStream a(zip);
    MemoryStream b(out);
    auto ra = ArchiveReader::open(a).value();
    auto rb = ArchiveReader::open(b).value();
    REQUIRE(rb->entries().size() == 2);
    CHECK(rb->entries()[1].name == "caf\xC3\xA9.txt");
    CHECK((rb->entries()[1].flags & 0x0800) != 0);
    CHECK(raw_of(*ra, 1) == raw_of(*rb, index_of(*rb, "caf\xC3\xA9.txt")));
}

TEST_CASE("archives with unsupported methods cannot be edited", "[editor]")
{
    const auto zip = test::craft_zip({ { .name = "weird.lzma", .data = test::random_bytes(40, 7), .method = 14 } });
    MemoryStream in(zip);
    ArchiveEditor ed(test::shared_context());
    auto r = ed.open(in);
    REQUIRE_FALSE(r);
    CHECK(r.error().status == Status::UnsupportedMethod);
}

namespace
{

class FailingStream final : public IChunkedStream
{
public:
    FailingStream(IChunkedStream& inner, std::uint64_t fail_after) :
        inner_(inner),
        left_(fail_after)
    {
    }
    Result<std::size_t> write(const std::uint8_t* buf, std::size_t len) override
    {
        if (len > left_)
            return fail(Status::IoError, "injected failure");
        left_ -= len;
        return inner_.write(buf, len);
    }
    [[nodiscard]] bool seekable() const override { return true; }
    VoidResult seek(std::uint64_t pos) override { return inner_.seek(pos); }
    [[nodiscard]] std::uint64_t tell() const override { return inner_.tell(); }
    [[nodiscard]] std::optional<std::uint64_t> size() const override { return inner_.size(); }

private:
    IChunkedStream& inner_;
    std::uint64_t left_;
};

}

TEST_CASE("failure mid-commit leaves the original intact and no temp file", "[editor]")
{
    test::TempDir dir;
    const auto path = dir.write("archive.zip", original_archive());
    const auto before = test::read_file(path);
    {
        auto input = FileStream::open(path, FileMode::Read).value();
        ArchiveEditor ed(test::shared_context());
        REQUIRE(ed.open(*input));
        REQUIRE(ed.remove(0));
        auto temp = AtomicFileStream::create(path).value();
        FailingStream failing(*temp, 50000);
        ProgressSink progress;
        auto r = ed.commit(failing, progress);
        CHECK(r.status == Status::IoError);
    }
    CHECK(test::read_file(path) == before);
    std::size_t files = 0;
    for ([[maybe_unused]] const auto& e : std::filesystem::directory_iterator(dir.path()))
        ++files;
    CHECK(files == 1);
}

TEST_CASE("in-place commit replaces the archive atomically", "[editor]")
{
    test::TempDir dir;
    const auto path = dir.write("archive.zip", original_archive());
    {
        auto input = FileStream::open(path, FileMode::Read).value();
        ArchiveEditor ed(test::shared_context());
        REQUIRE(ed.open(*input));
        REQUIRE(ed.rename(3, "renamed.txt"));
        auto temp = AtomicFileStream::create(path).value();
        ProgressSink progress;
        REQUIRE(ed.commit(*temp, progress).status == Status::Ok);
        REQUIRE(temp->commit());
    }
    auto x = test::extract_all(test::read_file(path));
    CHECK(x.files.contains("renamed.txt"));
}
