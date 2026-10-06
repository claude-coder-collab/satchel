// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "../cli.hpp"
#include "uninstall.hpp"

#include <iostream>

namespace satchel_cli
{

int run_uninstall(const GlobalOptions& g, const std::vector<std::string>& extra_apps, bool keep_settings, bool dry_run)
{
    const char* home_env = std::getenv("HOME");
    if (home_env == nullptr || *home_env == '\0')
        throw std::runtime_error("HOME is not set");

    satchel_macos::UninstallOptions options;
    options.home = home_env;
    options.apps = satchel_macos::default_app_paths(options.home);
    options.apps.insert(options.apps.end(), extra_apps.begin(), extra_apps.end());
    options.keep_settings = keep_settings;
    options.dry_run = dry_run;

    if (!dry_run && !g.yes && !confirm("Remove Satchel, its Quick Look extension, Finder services and settings?"))
        return exit_declined;

    const auto report = satchel_macos::uninstall(options, satchel_macos::run_process);
    if (g.json)
        print_json({ { "done", report.done }, { "failed", report.failed } });
    else if (!g.quiet)
        for (const auto& line : report.done)
            std::cerr << "ok: " << line << '\n';
    for (const auto& line : report.failed)
        std::cerr << "FAILED: " << line << '\n';
    return report.failed.empty() ? exit_ok : exit_error;
}

}
