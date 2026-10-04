// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "logic.hpp"

#include <catch2/catch_test_macros.hpp>

#include <format>
#include <set>

using namespace satchel_gui;
namespace fs = std::filesystem;

TEST_CASE("Simple mode drop rule", "[gui]")
{
    CHECK(drop_action({}) == DropAction::None);
    CHECK(drop_action({ "a.zip", "b.ZIP" }) == DropAction::ExtractEach);
    CHECK(drop_action({ "a.zip", "take.wav" }) == DropAction::CompressAll);
    CHECK(drop_action({ "folder" }) == DropAction::CompressAll);
}

TEST_CASE("file-manager actions force a verb", "[gui]")
{
    CHECK(drop_action({ "a.zip" }, Intent::Compress) == DropAction::CompressAll);
    CHECK(drop_action({ "a.zip", "take.wav" }, Intent::Extract) == DropAction::ExtractEach);
    CHECK(drop_action({ "take.wav" }, Intent::Extract) == DropAction::None);
    CHECK(action_items({ "a.zip", "take.wav", "b.zip" }, DropAction::ExtractEach) == std::vector<fs::path>{ "a.zip", "b.zip" });
    CHECK(action_items({ "a.zip", "take.wav" }, DropAction::CompressAll).size() == 2);
    CHECK(action_items({ "a.zip" }, DropAction::None).empty());
}

TEST_CASE("output naming never overwrites", "[gui]")
{
    std::set<fs::path> existing{ "/r/Recordings.zip", "/r/Recordings 2.zip", "/r/take.zip", "/r/session" };
    const auto exists = [&](const fs::path& p) { return existing.contains(p); };
    CHECK(unique_path("/r/new.zip", exists) == fs::path("/r/new.zip"));
    CHECK(unique_path("/r/Recordings.zip", exists) == fs::path("/r/Recordings 3.zip"));
    CHECK(compress_output({ "/r/take.wav" }, std::nullopt, exists) == fs::path("/r/take.wav.zip"));
    CHECK(compress_output({ "/r/take" }, std::nullopt, exists) == fs::path("/r/take 2.zip"));
    CHECK(compress_output({ "/r/a.wav", "/r/b.wav" }, std::nullopt, exists) == fs::path("/r/r.zip"));
    CHECK(compress_output({ "/r/a.wav" }, fs::path("/out"), exists) == fs::path("/out/a.wav.zip"));
    CHECK(extract_output("/r/session.zip", std::nullopt, exists) == fs::path("/r/session 2"));
    CHECK(extract_output("/r/other.zip", std::nullopt, exists) == fs::path("/r/other"));
    existing.insert("/r/notes.txt");
    CHECK(extract_output("/r/notes.txt.zip", std::nullopt, exists) == fs::path("/r/notes.txt 2"));
}

TEST_CASE("multi-mono members become one row", "[gui]")
{
    std::vector<ListedEntry> entries;
    entries.push_back({ 0, "dir/notes.txt", false, 10, 5, "Deflate", {}, 1, std::nullopt, 0, 0 });
    for (const std::uint16_t ch : { std::uint16_t{ 2 }, std::uint16_t{ 1 }, std::uint16_t{ 3 } })
        entries.push_back({ ch, std::format("dir/t_ch0{}.flac", ch), false, 100, 60, "FLAC", "t.wav", 2, std::string("dir/|t.wav"), ch, 3 });
    entries.push_back({ 4, "dir/x.flac", false, 50, 40, "FLAC", "x.wav", 3, std::nullopt, 0, 0 });
    const auto rows = group_rows(entries);
    REQUIRE(rows.size() == 3);
    CHECK(rows[1].name == "dir/t.wav — 3 channels");
    CHECK(rows[1].size == 300);
    CHECK(rows[1].packed == 180);
    CHECK(rows[1].entries == std::vector<std::size_t>{ 1, 2, 3 });
    REQUIRE(rows[1].children.size() == 3);
    CHECK(rows[1].children[0].name == "dir/t_ch01.flac");
    CHECK(rows[1].children[2].method == "FLAC channel 3/3");
    CHECK(rows[2].restores_to == "x.wav");
    CHECK(percent_saved(300, 180) == 40);
    CHECK(percent_saved(0, 0) == 0);
}

TEST_CASE("timecode formatting", "[gui]")
{
    const std::uint32_t rate = 48000;
    const std::uint64_t one_hour = 3600ull * rate;
    CHECK(format_timecode(one_hour + 12 * rate / 25, rate, "25") == "01:00:00:12");
    CHECK(format_timecode(one_hour + rate / 2, rate) == "01:00:00.500");
    CHECK(format_timecode(0, rate, "24") == "00:00:00:00");
    // 29.97 drop frame matches real time every ten minutes; after one minute it is 2 frames behind.
    CHECK(format_timecode(60ull * rate, rate, "30000/1001 DF") == "00:00:59;28");
    CHECK(format_timecode(600ull * rate, rate, "30000/1001 DF") == "00:10:00;00");
    CHECK(format_timecode(rate * 3600ull, rate, "30000/1001 NDF") == "00:59:56:12");
    CHECK(format_timecode(1, 0, "25").empty());
}

TEST_CASE("settings defaults match the spec", "[gui]")
{
    const Settings s;
    CHECK(s.flac);
    CHECK(s.flac_level == 5);
    CHECK(s.deflate_level == 6);
    CHECK(s.threads == 0);
    CHECK(s.restore_audio);
    CHECK_FALSE(s.include_readme);
    CHECK(s.overwrite == Overwrite::Ask);
    CHECK_FALSE(s.verify_after_build);
    CHECK_FALSE(s.associate_zip);
    CHECK(s.check_updates);
    CHECK_FALSE(s.simple_output_folder);
}

TEST_CASE("copy as CLI command", "[gui]")
{
    Settings s;
    CHECK(cli_create_command(s, "/out/a b.zip", { "/in/x.wav", "/in/it's" }) == "satchel create '/out/a b.zip' /in/x.wav '/in/it'\\''s'");
    s.flac = false;
    s.deflate_level = 9;
    s.threads = 4;
    CHECK(cli_create_command(s, "o.zip", { "i" }) == "satchel create --no-flac --deflate-level 9 --threads 4 o.zip i");
    Settings e;
    e.restore_audio = false;
    e.include_readme = true;
    e.overwrite = Overwrite::Replace;
    CHECK(cli_extract_command(e, "a.zip", "out") == "satchel extract --keep-flac --include-readme --overwrite replace a.zip -d out");
    CHECK(savings_text(4200000000, 2300000000) == "4.2 GB → 2.3 GB, 45% smaller");
}
