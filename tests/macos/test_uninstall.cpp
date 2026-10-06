// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "uninstall.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <random>
#include <unistd.h>

using namespace satchel_macos;
namespace fs = std::filesystem;

namespace
{

constexpr const char* pluginkit_output = "+    com.vennaudio.satchel.preview(0.1.0)\n"
                                         "\t            Path = /Users/x/build/Satchel.app/Contents/PlugIns/SatchelPreview.appex\n"
                                         "\t            UUID = 07EB86B9\n\n (1 plug-in)\n";

struct FakeRunner
{
    std::vector<std::vector<std::string>> calls;
    std::string pluginkit_list;
    std::string bundle_id = "com.example.satchel";
    std::string failing;

    CommandResult operator()(const std::vector<std::string>& argv)
    {
        calls.push_back(argv);
        if (argv[0] == "plutil")
            return { 0, bundle_id + "\n" };
        if (argv[0] == "pluginkit" && argv[1] == "-mAvvv")
            return { 0, pluginkit_list };
        return { argv[0] == failing ? 1 : 0, {} };
    }

    bool called(const std::string& program, const std::string& first_argument = {}) const
    {
        return std::ranges::any_of(calls, [&](const auto& c) { return c[0] == program && (first_argument.empty() || c[1] == first_argument); });
    }
};

struct Sandbox
{
    fs::path root = fs::temp_directory_path() / ("satchel-uninstall-" + std::to_string(getpid()) + "-" + std::to_string(std::random_device{}()));
    fs::path app = root / "Applications" / "Satchel.app";

    Sandbox()
    {
        fs::create_directories(app / "Contents" / "PlugIns" / "SatchelPreview.appex");
        fs::create_directories(root / "Library" / "Preferences");
    }
    ~Sandbox()
    {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    Sandbox(const Sandbox&) = delete;
    Sandbox& operator=(const Sandbox&) = delete;
    Sandbox(Sandbox&&) = delete;
    Sandbox& operator=(Sandbox&&) = delete;

    fs::path preferences() const { return root / "Library" / "Preferences" / "com.example.satchel.plist"; }
    UninstallOptions options() const { return { root, { app }, false, false }; }
};

}

TEST_CASE("pluginkit output yields the registered extension paths", "[uninstall]")
{
    const auto paths = parse_registered_extensions(pluginkit_output);
    REQUIRE(paths.size() == 1);
    CHECK(paths[0] == "/Users/x/build/Satchel.app/Contents/PlugIns/SatchelPreview.appex");
    CHECK(parse_registered_extensions("").empty());
}

TEST_CASE("uninstall removes the app, extension registrations and settings", "[uninstall]")
{
    Sandbox sandbox;
    std::ofstream(sandbox.preferences()) << "x";
    fs::create_directories(sandbox.root / "Library" / "Application Support" / "com.example.satchel");
    FakeRunner runner;
    runner.pluginkit_list = pluginkit_output;

    const auto report = uninstall(sandbox.options(), std::ref(runner));

    CHECK(report.failed.empty());
    CHECK_FALSE(fs::exists(sandbox.app));
    CHECK(fs::is_directory(sandbox.root / ".Trash" / "Satchel.app"));
    CHECK_FALSE(fs::exists(sandbox.preferences()));
    CHECK_FALSE(fs::exists(sandbox.root / "Library" / "Application Support" / "com.example.satchel"));
    CHECK(runner.called("defaults", "delete"));
    const auto removed = std::ranges::count_if(runner.calls, [](const auto& c) { return c[0] == "pluginkit" && c[1] == "-r"; });
    CHECK(removed == 2);
    CHECK(runner.calls.back()[1] == "-flush");
}

TEST_CASE("keep_settings leaves user data alone", "[uninstall]")
{
    Sandbox sandbox;
    std::ofstream(sandbox.preferences()) << "x";
    FakeRunner runner;
    auto options = sandbox.options();
    options.keep_settings = true;

    uninstall(options, std::ref(runner));

    CHECK(fs::exists(sandbox.preferences()));
    CHECK_FALSE(runner.called("defaults"));
}

TEST_CASE("dry run only runs read-only commands", "[uninstall]")
{
    Sandbox sandbox;
    FakeRunner runner;
    auto options = sandbox.options();
    options.dry_run = true;

    const auto report = uninstall(options, std::ref(runner));

    CHECK(fs::exists(sandbox.app));
    CHECK_FALSE(report.done.empty());
    CHECK(std::ranges::all_of(report.done, [](const auto& line) { return line.starts_with("would "); }));
    CHECK(std::ranges::all_of(runner.calls, [](const auto& c) { return c[0] == "plutil" || (c[0] == "pluginkit" && c[1] == "-mAvvv"); }));
}

TEST_CASE("quit_running off skips quitting; duplicate app paths are removed once", "[uninstall]")
{
    Sandbox sandbox;
    FakeRunner runner;
    auto options = sandbox.options();
    options.quit_running = false;
    options.apps = { sandbox.app, sandbox.app / ".." / "Satchel.app" };

    const auto report = uninstall(options, std::ref(runner));

    CHECK(report.failed.empty());
    CHECK_FALSE(runner.called("osascript"));
    CHECK(std::ranges::count_if(report.done, [](const auto& line) { return line.starts_with("move "); }) == 1);
}

TEST_CASE("a missing app is not an error", "[uninstall]")
{
    Sandbox sandbox;
    FakeRunner runner;
    auto options = sandbox.options();
    options.apps = { sandbox.root / "nowhere" / "Satchel.app" };

    CHECK(uninstall(options, std::ref(runner)).failed.empty());
}

TEST_CASE("a failing pluginkit is reported", "[uninstall]")
{
    Sandbox sandbox;
    FakeRunner runner;
    runner.failing = "pluginkit";

    const auto report = uninstall(sandbox.options(), std::ref(runner));

    REQUIRE_FALSE(report.failed.empty());
    CHECK(report.failed.front().contains("Quick Look extension"));
}

TEST_CASE("run_process captures output and status", "[uninstall]")
{
    const auto ok = run_process({ "/bin/echo", "hello" });
    CHECK(ok.status == 0);
    CHECK(ok.output == "hello\n");
    CHECK(run_process({ "/usr/bin/false" }).status == 1);
    CHECK(run_process({ "/nonexistent/program" }).status == 127);
}
