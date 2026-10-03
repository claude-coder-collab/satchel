// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "io/file_system.hpp"
#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace zp;

TEST_CASE("filesystem input walks in byte order and skips links", "[filesystem]")
{
    test::TempDir src;
    src.write("root/b.txt", test::text_bytes("b"));
    src.write("root/a/x.txt", test::text_bytes("x"));
    src.mkdir("root/empty");
#ifndef _WIN32
    std::filesystem::create_symlink(src.path() / "root/b.txt", src.path() / "root/link-file");
    std::filesystem::create_directory_symlink(src.path() / "root/a", src.path() / "root/link-dir");
    std::filesystem::create_symlink(src.path() / "missing", src.path() / "root/broken");
#endif
    FilesystemInputSource in({ src.path() / "root" });
    const auto plan = test::plan_of(in);
    std::vector<std::string> names;
    for (const auto& e : plan.entries)
        names.push_back(e.output_name);
    CHECK(names == std::vector<std::string>{ "root", "root/a", "root/a/x.txt", "root/b.txt", "root/empty" });
#ifndef _WIN32
    CHECK(plan.warnings.size() == 3);
#endif
}

TEST_CASE("disk round trip restores content, mtime and permissions", "[filesystem]")
{
    test::TempDir src;
    const auto data = test::random_bytes(300000, 4);
    const auto f = src.write("pkg/tool.bin", data);
    src.write("pkg/sub/readme.txt", test::text_like(1000, 1));
    std::filesystem::permissions(f, std::filesystem::perms::owner_all | std::filesystem::perms::group_read | std::filesystem::perms::group_exec);
    REQUIRE(set_file_mtime(f, 1600000000LL * 1'000'000'000));

    FilesystemInputSource in({ src.path() / "pkg" });
    test::TempDir work;
    const auto zip_path = work.path() / "pkg.zip";
    {
        auto out = AtomicFileStream::create(zip_path).value();
        ArchiveBuilder builder(*out, test::shared_context());
        ProgressSink progress;
        REQUIRE(builder.execute(test::plan_of(in), progress).status == Status::Ok);
        REQUIRE(out->commit());
    }

    test::TempDir dest;
    auto input = FileStream::open(zip_path, FileMode::Read).value();
    auto reader = ArchiveReader::open(*input).value();
    FilesystemOutputSink sink(dest.path());
    ArchiveExtractor x(*reader, sink, test::shared_context());
    auto plan = x.plan_extraction({}).value();
    ProgressSink progress;
    REQUIRE(x.execute(plan, progress).status == Status::Ok);

    const auto out = dest.path() / "pkg" / "tool.bin";
    CHECK(test::read_file(out) == data);
    const auto st = stat_no_follow(out).value();
    CHECK(st.mtime_ns / 1'000'000'000 == 1600000000);
#ifndef _WIN32
    CHECK(st.unix_mode == 0750u);
#endif
    CHECK(std::filesystem::exists(dest.path() / "pkg" / "sub" / "readme.txt"));
}

#ifndef _WIN32
TEST_CASE("extraction refuses to write through an existing link", "[filesystem]")
{
    test::TempDir outside;
    test::TempDir dest;
    std::filesystem::create_directory_symlink(outside.path(), dest.path() / "dir");
    const auto zip = test::craft_zip({ { .name = "dir/payload.txt", .data = test::text_bytes("x") } });
    MemoryStream s(zip);
    auto reader = ArchiveReader::open(s).value();
    FilesystemOutputSink sink(dest.path());
    ArchiveExtractor x(*reader, sink, test::shared_context());
    auto plan = x.plan_extraction({}).value();
    ProgressSink progress;
    auto r = x.execute(plan, progress);
    CHECK(r.status == Status::UnsafePath);
    CHECK_FALSE(std::filesystem::exists(outside.path() / "payload.txt"));
}
#endif
