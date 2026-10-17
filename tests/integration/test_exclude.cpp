// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "io/input_source.hpp"
#include "io/zip/editor.hpp"
#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <vector>

using namespace zp;

namespace
{

ExcludeFilter system_filter()
{
    return ExcludeFilter::parse(exclude_preset(ExcludePreset::System)).value();
}

std::vector<std::string> names_of(const ArchivePlan& plan)
{
    std::vector<std::string> names;
    for (const auto& e : plan.entries)
        names.push_back(e.output_name);
    return names;
}

std::vector<std::string> excluded_of(const ArchivePlan& plan)
{
    std::vector<std::string> names;
    for (const auto& x : plan.excluded)
        names.push_back(x.archive_path);
    return names;
}

}

TEST_CASE("excluded items and folders are left out and reported once", "[integration][exclude]")
{
    MemoryInputSource in;
    in.add_directory("proj");
    in.add_file("proj/.DS_Store", test::text_bytes("x"));
    in.add_file("proj/take.txt", test::text_bytes("take"));
    in.add_file("proj/._take.txt", test::text_bytes("x"));
    in.add_directory("proj/__MACOSX");
    in.add_file("proj/__MACOSX/a", test::text_bytes("x"));
    in.add_file("proj/__MACOSX/b", test::text_bytes("x"));
    in.add_file("proj/sub/__MACOSX/c", test::text_bytes("x"));
    in.add_file("proj/sub/keep.txt", test::text_bytes("keep"));
    in.add_file(".DS_Store", test::text_bytes("chosen directly"));
    in.set_exclude(system_filter());

    const auto plan = test::plan_of(in);
    CHECK(names_of(plan) == std::vector<std::string>{ "proj", "proj/take.txt", "proj/sub/keep.txt", ".DS_Store" });
    CHECK(excluded_of(plan) == std::vector<std::string>{ "proj/.DS_Store", "proj/._take.txt", "proj/__MACOSX", "proj/sub/__MACOSX" });
    CHECK(plan.excluded[2].kind == ItemKind::Directory);
    CHECK(plan.excluded[3].source_path.empty());
    CHECK(plan.total_input_bytes() == 4 + 4 + 15);
}

TEST_CASE("no exclude filter keeps everything", "[integration][exclude]")
{
    MemoryInputSource in;
    in.add_file("proj/.DS_Store", test::text_bytes("x"));
    const auto plan = test::plan_of(in);
    CHECK(names_of(plan) == std::vector<std::string>{ "proj/.DS_Store" });
    CHECK(plan.excluded.empty());
}

TEST_CASE("editor additions honour the exclude filter", "[integration][exclude]")
{
    MemoryInputSource first;
    first.add_file("a.txt", test::text_bytes("a"));
    auto b = test::build(first);
    REQUIRE(b.result.status == Status::Ok);

    MemoryStream archive(b.zip);
    ArchiveEditor editor(test::shared_context());
    REQUIRE(editor.open(archive));
    MemoryInputSource more;
    more.add_file("dir/b.txt", test::text_bytes("b"));
    more.add_file("dir/Thumbs.db", test::text_bytes("x"));
    more.set_exclude(system_filter());
    REQUIRE(editor.add(more));
    const auto plan = editor.plan();
    CHECK(names_of(plan) == std::vector<std::string>{ "a.txt", "dir/b.txt" });
    CHECK(excluded_of(plan) == std::vector<std::string>{ "dir/Thumbs.db" });
}

TEST_CASE("filesystem walk does not enter excluded folders", "[integration][exclude][filesystem]")
{
    test::TempDir src;
    src.write("vol/song.txt", test::text_bytes("song"));
    src.write("vol/.DS_Store", test::text_bytes("x"));
    src.write("vol/.Trashes/501/old.txt", test::text_bytes("x"));
    src.write("vol/.DS_Store_backup/keep.txt", test::text_bytes("keep"));
    const auto trashes = src.path() / "vol" / ".Trashes";
#ifndef _WIN32
    std::filesystem::permissions(trashes, std::filesystem::perms::none);
    std::error_code ec;
    const bool unreadable = std::filesystem::directory_iterator(trashes, ec) == std::filesystem::directory_iterator{} && ec;
#endif

    FilesystemInputSource in({ src.path() / "vol" });
    in.set_exclude(system_filter());
    const auto plan = test::plan_of(in);
    CHECK(names_of(plan) == std::vector<std::string>{ "vol", "vol/.DS_Store_backup", "vol/.DS_Store_backup/keep.txt", "vol/song.txt" });
    CHECK(excluded_of(plan) == std::vector<std::string>{ "vol/.DS_Store", "vol/.Trashes" });
    CHECK(plan.excluded[1].source_path == path_to_utf8(trashes));

#ifndef _WIN32
    if (unreadable)
    {
        FilesystemInputSource unfiltered({ src.path() / "vol" });
        CHECK_FALSE(unfiltered.enumerate());
    }
    std::filesystem::permissions(trashes, std::filesystem::perms::owner_all);
#endif
}
