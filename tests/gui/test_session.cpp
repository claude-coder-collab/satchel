// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "pcm_fixtures.hpp"
#include "session.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <fstream>
#include <random>
#include <set>
#include <string>
#include <vector>

using namespace satchel_gui;
namespace fs = std::filesystem;

namespace
{

class Fixture
{
public:
    Fixture()
    {
        std::random_device rd;
        dir_ = fs::temp_directory_path() / ("satchel-session-" + std::to_string(rd()));
        fs::create_directories(dir_);
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
    Fixture(Fixture&&) = delete;
    Fixture& operator=(Fixture&&) = delete;
    ~Fixture()
    {
        std::error_code ec;
        fs::remove_all(dir_, ec);
        zp_context_free(ctx);
    }

    fs::path write(const std::string& rel, const std::string& text) const { return write(rel, std::vector<std::uint8_t>(text.begin(), text.end())); }
    fs::path write(const std::string& rel, const std::vector<std::uint8_t>& data) const
    {
        const auto p = dir_ / rel;
        fs::create_directories(p.parent_path());
        std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        return p;
    }
    [[nodiscard]] fs::path path(const std::string& rel) const { return dir_ / rel; }

    // An archive with a.txt, docs/b.txt and docs/c.txt.
    fs::path base_archive()
    {
        const auto out = path("base.zip");
        auto s = Session::create(ctx, out);
        s.apply({ op::Add{ { write("src/a.txt", "alpha") }, "" }, op::Add{ { write("src/docs/b.txt", "beta"), write("src/docs/c.txt", "gamma") }, "docs" } });
        s.commit(out);
        return out;
    }

    zp_context_t* ctx = zp_context_create(2, 0);

private:
    fs::path dir_;
};

std::set<std::string> names_in(const fs::path& archive)
{
    zpp::Context ctx(zp_context_create(1, 0));
    zpp::Stream stream(zpp::not_null(zp_stream_open_file(archive.string().c_str())));
    zpp::Reader reader(zpp::not_null(zp_reader_open(ctx.get(), stream.get())));
    std::set<std::string> out;
    for (const auto& e : zpp::reader_entries(reader.get()))
        out.insert(e.name);
    return out;
}

const SessionRow& row(const Session& s, const std::string& name)
{
    const auto& rows = s.view().rows;
    const auto it = std::ranges::find_if(rows, [&](const SessionRow& r) { return r.key.name == name; });
    REQUIRE(it != rows.end());
    return *it;
}

EntryKey kept(const std::string& name)
{
    return { "archive:" + name, name };
}

}

TEST_CASE("a new session stages additions and writes nothing until commit", "[session]")
{
    Fixture f;
    const auto target = f.path("out/new.zip");
    fs::create_directories(target.parent_path());
    auto s = Session::create(f.ctx, target);
    CHECK(s.is_new());
    CHECK_FALSE(s.has_changes());
    CHECK(s.view().rows.empty());

    s.apply({ op::Add{ { f.write("in/one.txt", "1"), f.write("in/sub/two.txt", "22") }, "" } });
    CHECK(s.has_changes());
    CHECK_FALSE(fs::exists(target));
    CHECK(row(s, "one.txt").state == RowState::Added);
    CHECK(s.view().summary.added == 2);
    CHECK(s.view().summary.added_bytes == 3);

    s.commit(target);
    CHECK(fs::exists(target));
    CHECK_FALSE(s.is_new());
    CHECK_FALSE(s.has_changes());
    CHECK(names_in(target) == std::set<std::string>{ "one.txt", "two.txt" });
    CHECK(row(s, "one.txt").state == RowState::Unchanged);
    CHECK(s.base_entries().size() == 2);
}

TEST_CASE("edits are staged, shown as row states and applied in place", "[session]")
{
    Fixture f;
    const auto archive = f.base_archive();
    auto s = Session::open(f.ctx, archive);
    REQUIRE(s.view().rows.size() == 3);

    s.apply({ op::Rename{ kept("a.txt"), "renamed.txt" } });
    s.apply({ op::Remove{ kept("docs/c.txt") } });
    s.apply({ op::NewFolder{ "empty/" } });
    s.apply({ op::Add{ { f.write("more/d.txt", "delta") }, "docs" } });

    const auto r = row(s, "renamed.txt");
    CHECK(r.state == RowState::Renamed);
    CHECK(r.previous_name == "a.txt");
    CHECK(r.base_index.has_value());
    CHECK(row(s, "docs/c.txt").state == RowState::Deleted);
    CHECK_FALSE(row(s, "docs/c.txt").plan.has_value());
    CHECK(row(s, "docs/d.txt").state == RowState::Added);
    CHECK(row(s, "empty").state == RowState::Added);
    CHECK(row(s, "docs/b.txt").state == RowState::Unchanged);
    const auto& sum = s.view().summary;
    CHECK(sum.renamed == 1);
    CHECK(sum.removed == 1);
    CHECK(sum.added == 2);
    CHECK(names_in(archive).contains("a.txt"));

    s.commit(archive);
    CHECK(names_in(archive) == std::set<std::string>{ "renamed.txt", "docs/b.txt", "docs/d.txt", "empty/" });
    CHECK_FALSE(s.has_changes());
}

TEST_CASE("undo, redo and discard", "[session]")
{
    Fixture f;
    auto s = Session::open(f.ctx, f.base_archive());
    s.apply({ op::Remove{ kept("a.txt") } });
    s.apply({ op::Rename{ kept("docs/b.txt"), "docs/bee.txt" } });
    CHECK(s.can_undo());
    CHECK_FALSE(s.can_redo());

    s.undo();
    CHECK(row(s, "docs/b.txt").state == RowState::Unchanged);
    CHECK(row(s, "a.txt").state == RowState::Deleted);
    CHECK(s.can_redo());
    s.redo();
    CHECK(row(s, "docs/bee.txt").state == RowState::Renamed);

    s.undo();
    s.apply({ op::Remove{ kept("docs/c.txt") } });
    CHECK_FALSE(s.can_redo());

    s.discard();
    CHECK_FALSE(s.has_changes());
    for (const auto& r : s.view().rows)
        CHECK(r.state == RowState::Unchanged);
}

TEST_CASE("a rename onto an existing name is refused and changes nothing", "[session]")
{
    Fixture f;
    auto s = Session::open(f.ctx, f.base_archive());
    s.apply({ op::Remove{ kept("docs/c.txt") } });
    try
    {
        s.apply({ op::Rename{ kept("a.txt"), "DOCS/B.TXT" } });
        FAIL("rename should have been refused");
    } catch (const zpp::Error& e)
    {
        CHECK(e.status() == ZP_NAME_COLLISION);
    }
    CHECK(row(s, "a.txt").state == RowState::Unchanged);
    CHECK(row(s, "docs/c.txt").state == RowState::Deleted);
    CHECK(s.view().conflicts.empty());

    CHECK_THROWS_AS(s.apply({ op::Rename{ kept("nope.txt"), "x" } }), zpp::Error);
    CHECK_THROWS_AS(s.apply({ op::Rename{ kept("a.txt"), "../x" } }), zpp::Error);
    CHECK(s.can_undo());
}

TEST_CASE("folders rename and remove with everything below them", "[session]")
{
    Fixture f;
    auto s = Session::open(f.ctx, f.base_archive());
    s.apply({ op::RenameFolder{ "docs", "papers/" } });
    CHECK(row(s, "papers/b.txt").state == RowState::Renamed);
    CHECK(row(s, "papers/c.txt").previous_name == "docs/c.txt");
    CHECK(s.view().summary.renamed == 2);

    s.apply({ op::RemoveFolder{ "papers" } });
    CHECK(row(s, "docs/b.txt").state == RowState::Deleted);
    CHECK(s.view().summary.removed == 2);
    CHECK_THROWS_AS(s.apply({ op::RemoveFolder{ "papers" } }), zpp::Error);
}

TEST_CASE("replace swaps an entry for a file from disk", "[session]")
{
    Fixture f;
    const auto archive = f.base_archive();
    auto s = Session::open(f.ctx, archive);
    s.apply({ op::Replace{ kept("docs/b.txt"), f.write("new/b.txt", "BETA 2") } });
    const auto r = row(s, "docs/b.txt");
    CHECK(r.state == RowState::Replaced);
    CHECK(r.base_index.has_value());
    CHECK(s.view().summary.replaced == 1);
    CHECK(s.view().summary.removed == 0);
    CHECK(s.view().summary.added == 0);
    s.commit(archive);
    CHECK(names_in(archive) == std::set<std::string>{ "a.txt", "docs/b.txt", "docs/c.txt" });
    const auto& base = s.base_entries();
    const auto b = std::ranges::find_if(base, [](const zpp::EntryInfo& e) { return e.name == "docs/b.txt"; });
    REQUIRE(b != base.end());
    CHECK(b->uncompressed_size == 6);
}

TEST_CASE("existing entries policy", "[session]")
{
    Fixture f;
    const auto archive = f.base_archive();
    const auto newer = f.write("again/a.txt", "fresh");
    const auto older = f.write("old/a.txt", "stale");
    fs::last_write_time(older, fs::file_time_type::clock::now() - std::chrono::hours(24 * 365 * 20));
    fs::last_write_time(newer, fs::file_time_type::clock::now() + std::chrono::hours(1));

    SECTION("Ask leaves a collision")
    {
        auto s = Session::open(f.ctx, archive);
        s.apply({ op::Add{ { newer }, "" } });
        REQUIRE(s.view().conflicts.size() == 1);
        CHECK(s.view().conflicts[0].entries.size() == 2);
        CHECK(row(s, "a.txt").conflicted);
        CHECK_FALSE(s.view().executable);
        CHECK_THROWS_AS(s.commit(archive), zpp::Error);

        s.apply({ op::Resolve{ { newer.string(), "a.txt" }, ZP_RESOLVE_RENAME, "a2.txt" } });
        CHECK(s.view().executable);
        CHECK(row(s, "a2.txt").state == RowState::Added);
    }
    SECTION("Replace")
    {
        auto s = Session::open(f.ctx, archive, { .existing = ExistingEntries::Replace });
        s.apply({ op::Add{ { older }, "" } });
        CHECK(s.view().conflicts.empty());
        CHECK(row(s, "a.txt").state == RowState::Replaced);
    }
    SECTION("Keep")
    {
        auto s = Session::open(f.ctx, archive, { .existing = ExistingEntries::Keep });
        s.apply({ op::Add{ { newer }, "" } });
        CHECK(s.view().conflicts.empty());
        CHECK(row(s, "a.txt").state == RowState::Unchanged);
        CHECK(s.view().summary.added == 0);
    }
    SECTION("Replace if newer")
    {
        auto s = Session::open(f.ctx, archive, { .existing = ExistingEntries::ReplaceIfNewer });
        s.apply({ op::Add{ { older }, "" } });
        CHECK(row(s, "a.txt").state == RowState::Unchanged);
        s.apply({ op::Add{ { newer }, "" } });
        CHECK(row(s, "a.txt").state == RowState::Replaced);
        CHECK(s.view().conflicts.empty());
    }
    SECTION("changing the policy re-plans staged additions")
    {
        auto s = Session::open(f.ctx, archive);
        s.apply({ op::Add{ { newer }, "" } });
        CHECK(s.view().conflicts.size() == 1);
        s.set_options({ .existing = ExistingEntries::Replace });
        CHECK(s.view().conflicts.empty());
        CHECK(s.has_changes());
    }
}

TEST_CASE("session options apply to additions", "[session]")
{
    Fixture f;
    const auto wav = f.write("audio/take.wav", zp::test::make_wav({}));
    auto s = Session::create(f.ctx, f.path("audio.zip"));
    s.apply({ op::Add{ { wav }, "" } });
    CHECK(row(s, "take.flac").plan->codec == ZP_CODEC_FLAC);
    const auto readme = std::ranges::find_if(s.view().rows, [](const SessionRow& r) { return r.generated; });
    REQUIRE(readme != s.view().rows.end());
    CHECK(readme->state == RowState::Added);

    s.set_options({ .flac = false });
    CHECK(row(s, "take.wav").plan->codec == ZP_CODEC_GENERAL);
    CHECK(std::ranges::none_of(s.view().rows, [](const SessionRow& r) { return r.generated; }));

    s.set_options({});
    s.commit(f.path("audio.zip"));
    const auto again = std::ranges::find_if(s.view().rows, [](const SessionRow& r) { return r.generated; });
    REQUIRE(again != s.view().rows.end());
    CHECK(again->state == RowState::Unchanged);
}

TEST_CASE("excludes leave out files inside added folders", "[session]")
{
    Fixture f;
    f.write("proj/keep.txt", "k");
    f.write("proj/.DS_Store", "x");
    auto s = Session::create(f.ctx, f.path("p.zip"), { .excludes = { ".DS_Store" } });
    s.apply({ op::Add{ { f.path("proj") }, "inbox" } });
    CHECK(row(s, "inbox/proj/keep.txt").state == RowState::Added);
    REQUIRE(s.view().excluded.size() == 1);
    CHECK(s.view().excluded[0].archive_path == "inbox/proj/.DS_Store");
}

TEST_CASE("a change whose source vanished is dropped on replay", "[session]")
{
    Fixture f;
    auto s = Session::open(f.ctx, f.base_archive());
    const auto gone = f.write("tmp/gone.txt", "bye");
    s.apply({ op::Add{ { gone }, "" } });
    s.apply({ op::Remove{ kept("a.txt") } });
    s.apply({ op::Remove{ kept("docs/b.txt") } });
    fs::remove(gone);
    s.undo();
    CHECK(s.view().problems.size() == 1);
    CHECK(std::ranges::none_of(s.view().rows, [](const SessionRow& r) { return r.key.name == "gone.txt"; }));
    CHECK(row(s, "a.txt").state == RowState::Deleted);
    CHECK(row(s, "docs/b.txt").state == RowState::Unchanged);
}

TEST_CASE("a failed commit keeps the staged changes", "[session]")
{
    Fixture f;
    const auto archive = f.base_archive();
    auto s = Session::open(f.ctx, archive);
    s.apply({ op::Remove{ kept("a.txt") } });
    CHECK_THROWS_AS(s.commit(f.path("no/such/dir/out.zip")), zpp::Error);
    CHECK(s.has_changes());
    CHECK(row(s, "a.txt").state == RowState::Deleted);

    bool cancelled = false;
    CHECK_THROWS_AS(s.commit(archive, nullptr, [&](std::uint64_t, std::uint64_t) { cancelled = true; return false; }), zpp::Error);
    CHECK(cancelled);
    CHECK(names_in(archive).contains("a.txt"));
    CHECK(s.has_changes());

    s.commit(f.path("copy.zip"));
    CHECK(s.target() == f.path("copy.zip"));
    CHECK(names_in(archive).contains("a.txt"));
    CHECK_FALSE(names_in(f.path("copy.zip")).contains("a.txt"));
}

TEST_CASE("adding a folder that already exists merges into it", "[session]")
{
    Fixture f;
    f.write("one/docs/b.txt", "b");
    f.write("two/docs/c.txt", "c");
    const auto archive = f.path("m.zip");
    auto s = Session::create(f.ctx, archive);
    s.apply({ op::Add{ { f.path("one/docs") }, "" } });
    s.commit(archive);
    s.apply({ op::Add{ { f.path("two/docs") }, "" } });
    CHECK(s.view().conflicts.empty());
    CHECK(row(s, "docs/c.txt").state == RowState::Added);
}
