// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#pragma once

// Decisions the desktop app makes, kept free of widgets so they can be unit-tested
// (desktop UI spec, "Testing").

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace satchel_gui
{

// ---- Simple mode drop rule -------------------------------------------------------------------

enum class DropAction {
    None,
    ExtractEach,
    CompressAll,
};

bool is_zip(const std::filesystem::path& p);
// Only zips -> extract each; anything else (including a mix) -> compress everything.
DropAction drop_action(const std::vector<std::filesystem::path>& dropped);

// ---- Output naming ---------------------------------------------------------------------------

using ExistsFn = std::function<bool(const std::filesystem::path&)>;

// "Recordings.zip" -> "Recordings 2.zip", "Recordings 3.zip", ... until !exists. For a folder the
// number goes at the end ("take.v2" -> "take.v2 2").
std::filesystem::path unique_path(const std::filesystem::path& wanted, const ExistsFn& exists, bool folder = false);
// One item: "<item name>.zip" next to it. Several: "<parent folder name>.zip" next to them.
// output_folder (Settings) replaces "next to".
std::filesystem::path compress_output(const std::vector<std::filesystem::path>& items, const std::optional<std::filesystem::path>& output_folder, const ExistsFn& exists);
// Folder "<archive name>/" next to the archive (or in output_folder).
std::filesystem::path extract_output(const std::filesystem::path& archive, const std::optional<std::filesystem::path>& output_folder, const ExistsFn& exists);

// ---- Archive browser rows --------------------------------------------------------------------

struct ListedEntry
{
    std::size_t index = 0;
    std::string name;
    bool directory = false;
    std::uint64_t size = 0;
    std::uint64_t packed = 0;
    std::string method; // "Store", "Deflate", "FLAC", "FLAC multi-mono", ...
    std::string restores_to;
    std::int64_t mtime = 0;
    std::optional<std::string> group; // multi-mono group key (folder + restored name)
    std::uint16_t channel_index = 0;
    std::uint16_t channel_count = 0;
};

struct Row
{
    std::string name;
    std::uint64_t size = 0;
    std::uint64_t packed = 0;
    std::string method;
    std::string restores_to;
    std::int64_t mtime = 0;
    bool directory = false;
    std::vector<std::size_t> entries; // archive entries this row stands for (a group has all members)
    std::vector<Row> children; // group members, in channel order
};

// One row per entry, except that the members of a multi-mono group become one row
// ("take2.wav — 16 channels") with the members as children.
std::vector<Row> group_rows(const std::vector<ListedEntry>& entries);

// "% saved" for the Ratio column (0 when nothing to compare).
int percent_saved(std::uint64_t size, std::uint64_t packed);

// ---- Timecode --------------------------------------------------------------------------------

// TIME_REFERENCE (samples since midnight) as HH:MM:SS:FF using the iXML timecode rate
// ("25", "24000/1001", "30000/1001 DF"...), or HH:MM:SS.mmm when the rate is unknown.
std::string format_timecode(std::uint64_t samples_since_midnight, std::uint32_t sample_rate, const std::string& timecode_rate = {});

// ---- Settings --------------------------------------------------------------------------------

enum class Overwrite {
    Ask,
    Skip,
    Replace,
};

struct Settings
{
    bool start_in_last_mode = true;
    std::optional<std::filesystem::path> simple_output_folder; // nullopt: next to source
    bool flac = true;
    int flac_level = 5;
    int deflate_level = 6;
    int threads = 0; // 0: all cores
    std::optional<std::filesystem::path> temp_folder;
    bool restore_audio = true;
    bool include_readme = false;
    Overwrite overwrite = Overwrite::Ask;
    bool verify_after_build = false;
    bool associate_zip = false;
    bool check_updates = true;
};

// ---- "Copy as CLI command" -------------------------------------------------------------------

std::string shell_quote(const std::string& arg);
std::string cli_create_command(const Settings& s, const std::filesystem::path& archive, const std::vector<std::filesystem::path>& inputs);
std::string cli_extract_command(const Settings& s, const std::filesystem::path& archive, const std::filesystem::path& destination);

// ---- Formatting ------------------------------------------------------------------------------

std::string human_size(std::uint64_t bytes);
// "4.2 GB → 2.3 GB, 45% smaller"
std::string savings_text(std::uint64_t input, std::uint64_t output);

}
