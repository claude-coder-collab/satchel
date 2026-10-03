// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "cli.hpp"

#include <CLI/CLI.hpp>
#include <csignal>
#include <iostream>

using namespace satchel_cli;

namespace
{

void on_interrupt(int)
{
    interrupted.store(true);
}

constexpr const char* exit_codes_help = R"(Exit codes:
  0    success
  1    error
  2    invalid command line
  3    unresolved name conflicts (nothing written)
  4    declined: warnings not accepted, or files exist without --overwrite
  5    finished with errors (some entries failed or were refused)
  130  cancelled)";

}

int main(int argc, char** argv)
{
    if (std::signal(SIGINT, on_interrupt) == SIG_ERR)
        print_error("cannot install the interrupt handler");

    CLI::App app{ "Satchel - lossless media packaging as standard zip archives", "satchel" };
    app.footer(exit_codes_help);
    app.require_subcommand(1);
    app.set_version_flag("--version", std::string(zp_version()));

    GlobalOptions g;
    const auto add_globals = [&](CLI::App* sub) {
        sub->add_flag("--json", g.json, "Machine-readable output on stdout");
        sub->add_flag("-q,--quiet", g.quiet, "No progress or summaries");
        sub->add_flag("-y,--yes", g.yes, "Proceed without asking");
        sub->add_option("--threads", g.threads, "Worker threads (0 = all cores)")->check(CLI::NonNegativeNumber);
        sub->add_option("--memory", g.memory_mb, "Memory budget in MB (0 = default)");
    };

    std::string archive;
    std::vector<std::string> paths;
    bool no_flac = false;
    int deflate_level = 6;
    int flac_level = 5;
    std::vector<std::string> renames;
    std::vector<std::string> skips;
    std::vector<std::string> store_unconverted;
    std::string resolutions;
    bool dry_run = false;
    std::string readme_template;
    std::string temp_dir;
    auto* create = app.add_subcommand("create", "Create an archive (PCM audio becomes FLAC)");
    create->add_option("archive", archive, "Output archive, or - for stdout")->required();
    create->add_option("paths", paths, "Files and folders to archive")->required();
    create->add_flag("--no-flac", no_flac, "Do not convert PCM audio to FLAC");
    create->add_option("--deflate-level", deflate_level, "Deflate level 1-9")->check(CLI::Range(1, 9));
    create->add_option("--flac-level", flac_level, "FLAC level 0-8")->check(CLI::Range(0, 8));
    create->add_option("--rename", renames, "Resolve a conflict: NAME=NEW_NAME (NAME: planned name, original name, source path or #index)");
    create->add_option("--skip", skips, "Resolve a conflict by leaving NAME out");
    create->add_option("--store-unconverted", store_unconverted, "Resolve a conflict by keeping NAME as PCM");
    create->add_option("--resolutions", resolutions, R"(JSON file: [{"entry": NAME|INDEX, "action": rename|skip|store-unconverted, "new_name": ...}])");
    create->add_flag("--dry-run", dry_run, "Show the plan without writing");
    create->add_option("--readme-template", readme_template, "Template for the generated readme");
    create->add_option("--temp-dir", temp_dir, "Folder for temporary files (multi-channel audio)");
    add_globals(create);

    std::vector<std::string> entries;
    std::string destination = ".";
    bool keep_flac = false;
    bool include_readme = false;
    std::string overwrite = "ask";
    auto* extract = app.add_subcommand("extract", "Extract an archive (FLAC is restored to the original audio)");
    extract->add_option("archive", archive, "Archive")->required();
    extract->add_option("entries", entries, "Entries to extract (default: all)");
    extract->add_option("-d,--destination", destination, "Destination folder");
    extract->add_flag("--keep-flac", keep_flac, "Extract FLAC files as they are");
    extract->add_flag("--include-readme", include_readme, "Also extract the generated readme");
    extract->add_option("--overwrite", overwrite, "Existing files: ask, skip or replace")->check(CLI::IsMember({ "ask", "skip", "replace" }));
    add_globals(extract);

    auto* list = app.add_subcommand("list", "List the entries of an archive");
    list->add_option("archive", archive, "Archive")->required();
    add_globals(list);

    auto* verify = app.add_subcommand("verify", "Check CRCs and restore audio in memory, writing nothing");
    verify->add_option("archive", archive, "Archive")->required();
    add_globals(verify);

    std::vector<std::string> adds;
    std::vector<std::string> removes;
    std::vector<std::string> edit_renames;
    std::vector<std::string> replaces;
    std::string output;
    auto* edit = app.add_subcommand("edit", "Change an archive (rewritten and replaced atomically)");
    edit->add_option("archive", archive, "Archive")->required();
    edit->add_option("--add", adds, "Add files or folders");
    edit->add_option("--remove", removes, "Remove an entry");
    edit->add_option("--rename", edit_renames, "Rename an entry: NAME=NEW_NAME");
    edit->add_option("--replace", replaces, "Replace an entry's content: NAME=PATH");
    edit->add_option("-o,--output", output, "Write to another file instead of replacing the archive");
    add_globals(edit);

    std::string flac_file;
    std::string restore_output;
    auto* restore = app.add_subcommand("restore", "Restore the original audio file from a single .flac");
    restore->add_option("file", flac_file, "FLAC file")->required();
    restore->add_option("-o,--output", restore_output, "Output file (default: the original name next to the FLAC)");
    add_globals(restore);

    try
    {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e)
    {
        const int code = app.exit(e);
        return code == 0 ? exit_ok : exit_usage;
    }

    try
    {
        if (*create)
            return run_create(g, archive, paths, no_flac, deflate_level, flac_level, renames, skips, store_unconverted, resolutions, dry_run, readme_template, temp_dir);
        if (*extract)
            return run_extract(g, archive, entries, destination, keep_flac, include_readme, overwrite);
        if (*list)
            return run_list(g, archive);
        if (*verify)
            return run_verify(g, archive);
        if (*edit)
            return run_edit(g, archive, adds, removes, edit_renames, replaces, output);
        if (*restore)
            return run_restore(g, flac_file, restore_output);
    } catch (const zpp::Error& e)
    {
        print_error(e.what());
        if (g.json)
            print_json({ { "status", e.name() }, { "message", e.what() } });
        if (e.status() == ZP_CANCELLED)
            return exit_cancelled;
        if (e.status() == ZP_NAME_COLLISION || e.status() == ZP_INVALID_NAME)
            return exit_conflicts;
        return exit_error;
    } catch (const std::invalid_argument& e)
    {
        print_error(e.what());
        return exit_usage;
    } catch (const std::exception& e)
    {
        print_error(e.what());
        return exit_error;
    }
    return exit_usage;
}
