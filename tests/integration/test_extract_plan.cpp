// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace zp;

namespace
{

ExtractionPlan plan_for(const std::vector<std::uint8_t>& zip, OutputSink& sink, ExtractOptions options = {})
{
    MemoryStream s(zip);
    auto r = ArchiveReader::open(s).value();
    ArchiveExtractor x(*r, sink, test::shared_context(), options);
    return x.plan_extraction({}).value();
}

bool has_issue(const ExtractionPlan& p, ExtractIssueKind k, const std::string& name)
{
    for (const auto& i : p.issues)
    {
        if (i.kind == k && i.name == name)
            return true;
    }
    return false;
}

}

TEST_CASE("extraction plan refuses unsafe paths and skips symlinks", "[extract]")
{
    const auto zip = test::craft_zip({
        { .name = "ok.txt", .data = test::text_bytes("ok") },
        { .name = "../evil.txt", .data = test::text_bytes("x") },
        { .name = "/abs.txt", .data = test::text_bytes("x") },
        { .name = "a/../../b.txt", .data = test::text_bytes("x") },
        { .name = "..\\win.txt", .data = test::text_bytes("x") },
        { .name = "C:\\drive.txt", .data = test::text_bytes("x") },
        { .name = "link", .data = test::text_bytes("/etc/passwd"), .external_attributes = 0120777u << 16 },
    });
    MemoryOutputSink sink;
    const auto p = plan_for(zip, sink);
    REQUIRE(p.items.size() == 1);
    CHECK(p.items[0].target == "ok.txt");
    for (const auto* n : { "../evil.txt", "/abs.txt", "a/../../b.txt", "..\\win.txt", "C:\\drive.txt" })
        CHECK(has_issue(p, ExtractIssueKind::UnsafePath, n));
    CHECK(has_issue(p, ExtractIssueKind::SymlinkSkipped, "link"));
    CHECK(p.error_count() == 5);

    auto x = test::extract_all(zip);
    CHECK(x.files.size() == 1);
    CHECK(x.result.status == Status::UnsafePath);
}

TEST_CASE("case-only duplicates in third-party archives are refused", "[extract]")
{
    const auto zip = test::craft_zip({
        { .name = "Readme.txt", .data = test::text_bytes("1") },
        { .name = "README.TXT", .data = test::text_bytes("2") },
        { .name = "other.txt", .data = test::text_bytes("3") },
        { .name = "dir/", .external_attributes = 040755u << 16 },
        { .name = "DIR/", .external_attributes = 040755u << 16 },
    });
    MemoryOutputSink sink;
    const auto p = plan_for(zip, sink);
    CHECK(has_issue(p, ExtractIssueKind::NameCollision, "Readme.txt"));
    CHECK(has_issue(p, ExtractIssueKind::NameCollision, "README.TXT"));
    CHECK_FALSE(has_issue(p, ExtractIssueKind::NameCollision, "dir/"));
    CHECK(p.items.size() == 2);
}

TEST_CASE("unsupported methods are skipped with a warning", "[extract]")
{
    const auto zip = test::craft_zip({
        { .name = "a.lzma", .data = test::random_bytes(5, 1), .method = 14 },
        { .name = "b.txt", .data = test::text_bytes("b") },
    });
    MemoryOutputSink sink;
    const auto p = plan_for(zip, sink);
    CHECK(has_issue(p, ExtractIssueKind::UnsupportedMethod, "a.lzma"));
    CHECK(p.error_count() == 0);
    CHECK(p.items.size() == 1);
}

TEST_CASE("existing destination files follow the overwrite policy", "[extract]")
{
    const auto zip = test::craft_zip({ { .name = "a.txt", .data = test::text_bytes("new") }, { .name = "b.txt", .data = test::text_bytes("b") } });
    MemoryStream s(zip);
    auto r = ArchiveReader::open(s).value();

    SECTION("ask requires a decision")
    {
        MemoryOutputSink sink;
        sink.add_existing("a.txt");
        ArchiveExtractor x(*r, sink, test::shared_context(), { .overwrite = OverwritePolicy::Ask });
        auto p = x.plan_extraction({}).value();
        CHECK(p.needs_decisions());
        CHECK(has_issue(p, ExtractIssueKind::ExistsAtDestination, "a.txt"));
        ProgressSink progress;
        CHECK(x.execute(p, progress).status == Status::DecisionRequired);
        REQUIRE(p.decide(0, ItemDecision::Replace));
        auto res = x.execute(p, progress);
        CHECK(res.status == Status::Ok);
        CHECK(sink.files()["a.txt"].data == test::text_bytes("new"));
    }
    SECTION("skip")
    {
        MemoryOutputSink sink;
        sink.add_existing("a.txt");
        ArchiveExtractor x(*r, sink, test::shared_context(), { .overwrite = OverwritePolicy::Skip });
        auto p = x.plan_extraction({}).value();
        CHECK_FALSE(p.needs_decisions());
        ProgressSink progress;
        auto res = x.execute(p, progress);
        CHECK(res.skipped == 1);
        CHECK(sink.files()["a.txt"].data.empty());
        CHECK(sink.files()["b.txt"].data == test::text_bytes("b"));
    }
}

TEST_CASE("CRC mismatch fails the entry and leaves no output", "[extract]")
{
    const auto zip = test::craft_zip({
        { .name = "bad.txt", .data = test::text_bytes("payload"), .crc = 0x12345678u },
        { .name = "good.txt", .data = test::text_bytes("fine") },
    });
    auto x = test::extract_all(zip);
    CHECK(x.result.status == Status::CrcMismatch);
    CHECK_FALSE(x.files.contains("bad.txt"));
    CHECK(x.files.contains("good.txt"));
}

TEST_CASE("selection and the readme option", "[extract]")
{
    MemoryInputSource in;
    in.add_file("a", test::text_bytes("a"));
    in.add_file("b", test::text_bytes("b"));
    auto b = test::build(in);
    MemoryStream s(b.zip);
    auto r = ArchiveReader::open(s).value();
    MemoryOutputSink sink;
    ArchiveExtractor x(*r, sink, test::shared_context());
    auto p = x.plan_extraction({ 1 }).value();
    REQUIRE(p.items.size() == 1);
    CHECK(p.items[0].target == "b");
}
