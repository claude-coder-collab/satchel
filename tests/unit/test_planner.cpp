// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "io/zip/planner.hpp"
#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace zp;

namespace
{

std::size_t collisions(const ArchivePlan& p)
{
    std::size_t n = 0;
    for (const auto& c : p.conflicts)
        n += c.kind == ConflictKind::Collision ? 1 : 0;
    return n;
}

}

TEST_CASE("plan lists files and directories in input order", "[planner]")
{
    MemoryInputSource in;
    in.add_directory("dir");
    in.add_file("dir/a.txt", test::text_bytes("a"));
    in.add_file("b.bin", test::random_bytes(10, 1));
    const auto plan = test::plan_of(in);
    REQUIRE(plan.entries.size() == 3);
    CHECK(plan.entries[0].output_name == "dir");
    CHECK(plan.entries[0].is_directory());
    CHECK(plan.entries[1].output_name == "dir/a.txt");
    CHECK(plan.entries[2].codec == PlanCodec::General);
    CHECK(plan.executable());
    CHECK(plan.warnings.empty());
}

TEST_CASE("collisions: exact, case-only, normalization-only, file vs directory", "[planner]")
{
    SECTION("exact")
    {
        MemoryInputSource in;
        in.add_file("a.txt", {});
        in.add_file("a.txt", {});
        auto p = test::plan_of(in);
        CHECK(collisions(p) == 1);
        CHECK_FALSE(p.executable());
    }
    SECTION("case only")
    {
        MemoryInputSource in;
        in.add_file("Take1.WAV", {});
        in.add_file("take1.wav", {});
        auto p = test::plan_of(in);
        REQUIRE(p.conflicts.size() == 1);
        CHECK(p.conflicts[0].entries == std::vector<std::size_t>{ 0, 1 });
    }
    SECTION("normalization only")
    {
        MemoryInputSource in;
        in.add_file("Cafe\xCC\x81.txt", {});
        in.add_file("Caf\xC3\xA9.txt", {});
        CHECK(collisions(test::plan_of(in)) == 1);
    }
    SECTION("file vs directory with the same key")
    {
        MemoryInputSource in;
        in.add_directory("Audio");
        in.add_file("audio", {});
        CHECK(collisions(test::plan_of(in)) == 1);
    }
    SECTION("file used as a folder")
    {
        MemoryInputSource in;
        in.add_file("a", {});
        in.add_file("A/b", {});
        auto p = test::plan_of(in);
        REQUIRE(collisions(p) == 1);
        CHECK(p.conflicts[0].entries == std::vector<std::size_t>{ 0, 1 });
    }
    SECTION("distinct names are fine")
    {
        MemoryInputSource in;
        in.add_file("a", {});
        in.add_file("ab", {});
        in.add_directory("c");
        in.add_file("c/a", {});
        CHECK(test::plan_of(in).executable());
    }
}

TEST_CASE("symlinks produce warnings and no entries", "[planner]")
{
    MemoryInputSource in;
    in.add_file("a", {});
    in.add_symlink("link");
    auto p = test::plan_of(in);
    REQUIRE(p.entries.size() == 1);
    REQUIRE(p.warnings.size() == 1);
    CHECK(p.warnings[0].kind == WarningKind::SymlinkSkipped);
    CHECK(p.warnings[0].source_path == "memory:link");
    CHECK(p.executable());
}

TEST_CASE("invalid names block execution", "[planner]")
{
    MemoryInputSource in;
    in.add_file("bad\xFF.txt", {});
    in.add_file("ok.txt", {});
    auto p = test::plan_of(in);
    REQUIRE(p.conflicts.size() == 1);
    CHECK(p.conflicts[0].kind == ConflictKind::InvalidName);
    CHECK(p.conflicts[0].entries == std::vector<std::size_t>{ 0 });
}

TEST_CASE("replan applies each resolution type", "[planner]")
{
    MemoryInputSource in;
    in.add_file("a.txt", test::text_bytes("1"));
    in.add_file("A.TXT", test::text_bytes("2"));
    in.add_file("b.txt", test::text_bytes("3"));
    const auto p = test::plan_of(in);
    REQUIRE_FALSE(p.executable());

    SECTION("rename")
    {
        auto r = ArchivePlanner::replan(p, { { 1, ResolutionAction::Rename, "c.txt" } });
        REQUIRE(r);
        CHECK(r->executable());
        CHECK(r->entries[1].output_name == "c.txt");
    }
    SECTION("skip")
    {
        auto r = ArchivePlanner::replan(p, { { 0, ResolutionAction::Skip, {} } });
        REQUIRE(r);
        CHECK(r->executable());
        REQUIRE(r->entries.size() == 2);
        CHECK(r->entries[0].output_name == "A.TXT");
    }
    SECTION("rename that still collides is rejected")
    {
        auto r = ArchivePlanner::replan(p, { { 1, ResolutionAction::Rename, "B.txt" } });
        REQUIRE_FALSE(r);
        CHECK(r.error().status == Status::NameCollision);
    }
    SECTION("invalid rename is rejected")
    {
        CHECK(ArchivePlanner::replan(p, { { 1, ResolutionAction::Rename, "../x" } }).error().status == Status::InvalidName);
        CHECK(ArchivePlanner::replan(p, { { 1, ResolutionAction::Rename, "" } }).error().status == Status::InvalidName);
    }
    SECTION("out of range")
    {
        CHECK(ArchivePlanner::replan(p, { { 9, ResolutionAction::Skip, {} } }).error().status == Status::InvalidArgument);
    }
    SECTION("unresolved conflicts remain after a partial replan")
    {
        auto r = ArchivePlanner::replan(p, { { 2, ResolutionAction::Rename, "d.txt" } });
        REQUIRE(r);
        CHECK_FALSE(r->executable());
    }
}

TEST_CASE("build is refused with unresolved conflicts", "[planner][builder]")
{
    MemoryInputSource in;
    in.add_file("a", {});
    in.add_file("A", {});
    auto b = test::build(in);
    CHECK(b.result.status == Status::ConflictsUnresolved);
    CHECK(b.zip.empty());
}
