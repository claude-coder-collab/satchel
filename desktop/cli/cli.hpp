// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#pragma once

#include "zp_cpp.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace satchel_cli
{

// Process exit codes (documented in docs/IMPLEMENTATION.md and `satchel --help`).
enum ExitCode : int {
    exit_ok = 0,
    exit_error = 1,
    exit_usage = 2,
    exit_conflicts = 3,
    exit_declined = 4,
    exit_partial = 5,
    exit_cancelled = 130,
};

struct GlobalOptions
{
    bool json = false;
    bool quiet = false;
    bool yes = false;
    int threads = 0;
    std::uint64_t memory_mb = 0;
};

extern std::atomic<bool> interrupted;

bool stdin_is_tty();
bool stderr_is_tty();

// y/N question on stderr; false when stdin is not a terminal.
bool confirm(const std::string& question);

// Progress line on stderr (terminal only, not with --json/--quiet). Returns false once the user
// pressed Ctrl+C.
class ProgressBar
{
public:
    ProgressBar(std::string label, const GlobalOptions& options);
    ProgressBar(const ProgressBar&) = delete;
    ProgressBar& operator=(const ProgressBar&) = delete;
    ProgressBar(ProgressBar&&) = delete;
    ProgressBar& operator=(ProgressBar&&) = delete;
    ~ProgressBar();

    bool update(std::uint64_t done, std::uint64_t total);
    zpp::ProgressAdapter& adapter() { return adapter_; }

private:
    std::string label_;
    bool enabled_;
    bool drawn_ = false;
    std::chrono::steady_clock::time_point start_;
    std::chrono::steady_clock::time_point last_;
    zpp::ProgressAdapter adapter_;
};

zpp::Context make_context(const GlobalOptions& options);
std::string human_size(std::uint64_t bytes);
std::string method_name(int method);
std::string_view codec_name(int codec);
void print_json(const nlohmann::json& j);
void print_error(const std::string& message);

int run_create(const GlobalOptions& g, const std::string& archive, const std::vector<std::string>& paths, bool no_flac, int deflate_level, int flac_level, const std::vector<std::string>& renames, const std::vector<std::string>& skips, const std::vector<std::string>& store_unconverted, const std::string& resolutions_file, bool dry_run, const std::string& readme_template_file, const std::string& temp_dir);
int run_extract(const GlobalOptions& g, const std::string& archive, const std::vector<std::string>& names, const std::string& destination, bool keep_flac, bool include_readme, const std::string& overwrite);
int run_list(const GlobalOptions& g, const std::string& archive);
int run_verify(const GlobalOptions& g, const std::string& archive);
int run_edit(const GlobalOptions& g, const std::string& archive, const std::vector<std::string>& adds, const std::vector<std::string>& removes, const std::vector<std::string>& renames, const std::vector<std::string>& replaces, const std::string& output);
int run_restore(const GlobalOptions& g, const std::string& file, const std::string& output);
int run_preview(const std::string& file, bool html);
#ifdef __APPLE__
int run_uninstall(const GlobalOptions& g, const std::vector<std::string>& extra_apps, bool keep_settings, bool dry_run);
#endif

}
