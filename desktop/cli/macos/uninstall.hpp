// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace satchel_macos
{

struct CommandResult
{
    int status = 0;
    std::string output;
};

using Runner = std::function<CommandResult(const std::vector<std::string>&)>;

struct UninstallOptions
{
    std::filesystem::path home;
    std::vector<std::filesystem::path> apps;
    bool keep_settings = false;
    bool dry_run = false;
};

struct UninstallReport
{
    std::vector<std::string> done;
    std::vector<std::string> failed;
};

#ifdef SATCHEL_BUNDLE_ID
inline constexpr std::string_view default_bundle_id = SATCHEL_BUNDLE_ID;
#else
inline constexpr std::string_view default_bundle_id = "com.vennaudio.satchel";
#endif

// Runs argv[0] (searched in PATH) and captures stdout; status 127 when it cannot be started.
CommandResult run_process(const std::vector<std::string>& argv);

// `Path = ….appex` lines of `pluginkit -mAvvv`.
std::vector<std::filesystem::path> parse_registered_extensions(std::string_view pluginkit_output);

std::vector<std::filesystem::path> default_app_paths(const std::filesystem::path& home);
std::vector<std::filesystem::path> user_data_paths(const std::filesystem::path& home, std::string_view bundle_id);

// Quits the app, unregisters the Quick Look extension, moves the app to the Trash, deletes settings
// and flushes the Finder services. Only read-only commands run when `dry_run` is set.
UninstallReport uninstall(const UninstallOptions& options, const Runner& runner);

}
