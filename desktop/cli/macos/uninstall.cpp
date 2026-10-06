// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "uninstall.hpp"

#include <algorithm>
#include <crt_externs.h>
#include <cstddef>
#include <cstdio>
#include <ctime>
#include <fcntl.h>
#include <regex>
#include <set>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

namespace satchel_macos
{

namespace fs = std::filesystem;

namespace
{

constexpr const char* pbs = "/System/Library/CoreServices/pbs";
constexpr const char* lsregister = "/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister";
constexpr const char* app_name = "Satchel.app";

std::string trimmed(std::string text)
{
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
        text.pop_back();
    return text;
}

class Steps
{
public:
    Steps(const Runner& runner, bool dry_run, UninstallReport& report) :
        runner_(runner),
        dry_run_(dry_run),
        report_(report)
    {
    }

    void command(const std::string& description, const std::vector<std::string>& argv, bool required = true)
    {
        record(description, [&] { return runner_(argv).status == 0 || !required; });
    }

    template <typename F>
    void action(const std::string& description, F&& f)
    {
        record(description, std::forward<F>(f));
    }

private:
    template <typename F>
    void record(const std::string& description, F&& f)
    {
        if (dry_run_)
            report_.done.push_back("would " + description);
        else if (f())
            report_.done.push_back(description);
        else
            report_.failed.push_back(description);
    }

    const Runner& runner_;
    bool dry_run_;
    UninstallReport& report_;
};

bool move_to_trash(const fs::path& path, const fs::path& home)
{
    std::error_code ec;
    const auto trash = home / ".Trash";
    fs::create_directories(trash, ec);
    auto target = trash / path.filename();
    if (fs::exists(fs::symlink_status(target)))
        target = trash / (path.stem().string() + " " + std::to_string(std::time(nullptr)) + path.extension().string());
    fs::rename(path, target, ec);
    return !ec;
}

bool remove_tree(const fs::path& path)
{
    std::error_code ec;
    fs::remove_all(path, ec);
    return !ec;
}

}

CommandResult run_process(const std::vector<std::string>& argv)
{
    if (argv.empty())
        return { 127, {} };
    int fds[2];
    if (pipe(fds) != 0)
        return { 127, {} };

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, fds[0]);
    posix_spawn_file_actions_addclose(&actions, fds[1]);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

    std::vector<char*> args;
    for (const auto& a : argv)
        args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);

    pid_t pid = 0;
    const int spawned = posix_spawnp(&pid, argv[0].c_str(), &actions, nullptr, args.data(), *_NSGetEnviron());
    posix_spawn_file_actions_destroy(&actions);
    close(fds[1]);
    if (spawned != 0)
    {
        close(fds[0]);
        return { 127, {} };
    }

    std::string output;
    char buffer[4096];
    ssize_t n = 0;
    while ((n = read(fds[0], buffer, sizeof buffer)) > 0)
        output.append(buffer, static_cast<std::size_t>(n));
    close(fds[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    return { WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status), std::move(output) };
}

std::vector<fs::path> parse_registered_extensions(std::string_view pluginkit_output)
{
    static const std::regex line(R"(^\s*Path = (.+\.appex)\s*$)");
    std::vector<fs::path> paths;
    const std::string text(pluginkit_output);
    std::size_t start = 0;
    while (start <= text.size())
    {
        const auto end = std::min(text.find('\n', start), text.size());
        std::smatch match;
        const auto row = text.substr(start, end - start);
        if (std::regex_match(row, match, line))
            paths.emplace_back(match[1].str());
        start = end + 1;
    }
    return paths;
}

std::vector<fs::path> default_app_paths(const fs::path& home)
{
    return { fs::path("/Applications") / app_name, home / "Applications" / app_name };
}

std::vector<fs::path> user_data_paths(const fs::path& home, std::string_view bundle_id)
{
    const std::string id(bundle_id);
    const auto library = home / "Library";
    return {
        library / "Preferences" / (id + ".plist"),
        library / "Application Support" / id,
        library / "Application Support" / "Satchel",
        library / "Caches" / id,
        library / "HTTPStorages" / id,
        library / "HTTPStorages" / (id + ".binarycookies"),
        library / "Saved Application State" / (id + ".savedState"),
        library / "Logs" / "Satchel",
    };
}

UninstallReport uninstall(const UninstallOptions& options, const Runner& runner)
{
    UninstallReport report;
    Steps steps(runner, options.dry_run, report);

    std::vector<fs::path> existing;
    for (const auto& app : options.apps)
    {
        std::error_code ec;
        const bool seen = std::ranges::any_of(existing, [&](const fs::path& p) { return fs::equivalent(p, app, ec); });
        if (fs::exists(app) && !seen)
            existing.push_back(app);
    }

    std::string bundle_id(default_bundle_id);
    if (!existing.empty())
    {
        const auto result = runner({ "plutil", "-extract", "CFBundleIdentifier", "raw", "-o", "-", (existing.front() / "Contents" / "Info.plist").string() });
        if (result.status == 0 && !trimmed(result.output).empty())
            bundle_id = trimmed(result.output);
    }

    const auto script = "if application id \"" + bundle_id + "\" is running then tell application id \"" + bundle_id + "\" to quit";
    if (options.quit_running)
        steps.command("quit Satchel", { "osascript", "-e", script }, false);

    std::set<fs::path> extensions;
    for (const auto& app : existing)
        extensions.insert(app / "Contents" / "PlugIns" / "SatchelPreview.appex");
    if (const auto registered = runner({ "pluginkit", "-mAvvv", "-i", bundle_id + ".preview" }); registered.status == 0)
        for (auto& path : parse_registered_extensions(registered.output))
            extensions.insert(std::move(path));
    for (const auto& extension : extensions)
        steps.command("unregister Quick Look extension " + extension.string(), { "pluginkit", "-r", extension.string() });

    for (const auto& app : existing)
    {
        steps.command("unregister " + app.string() + " from Launch Services", { lsregister, "-u", app.string() }, false);
        steps.action("move " + app.string() + " to the Trash", [&] { return move_to_trash(app, options.home); });
    }

    if (!options.keep_settings)
    {
        steps.command("delete preferences for " + bundle_id, { "defaults", "delete", bundle_id }, false);
        for (const auto& path : user_data_paths(options.home, bundle_id))
            if (fs::exists(fs::symlink_status(path)))
                steps.action("remove " + path.string(), [&] { return remove_tree(path); });
    }

    steps.command("refresh Finder services", { pbs, "-flush" }, false);
    return report;
}

}
