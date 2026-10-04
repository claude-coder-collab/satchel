// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "cli.hpp"

#include <chrono>
#include <filesystem>
#include <format>
#include <iostream>
#include <set>

namespace satchel_cli
{

namespace
{

struct Opened
{
    zpp::Context ctx;
    zpp::Stream stream;
    zpp::Reader reader;
};

Opened open_archive(const GlobalOptions& g, const std::string& path)
{
    Opened o{ make_context(g), {}, {} };
    o.stream.reset(zpp::not_null(zp_stream_open_file(path.c_str())));
    o.reader.reset(zpp::not_null(zp_reader_open(o.ctx.get(), o.stream.get())));
    return o;
}

std::string_view issue_name(int kind)
{
    switch (kind)
    {
        case ZP_ISSUE_SYMLINK_SKIPPED:
            return "symlink-skipped";
        case ZP_ISSUE_UNSUPPORTED_METHOD:
            return "unsupported-method";
        case ZP_ISSUE_UNSAFE_PATH:
            return "unsafe-path";
        case ZP_ISSUE_NAME_COLLISION:
            return "name-collision";
        case ZP_ISSUE_INCOMPLETE_GROUP:
            return "incomplete-group";
        case ZP_ISSUE_EXISTS_AT_DESTINATION:
            return "exists";
        default:
            return "unknown";
    }
}

std::string entry_method(const zpp::EntryInfo& e)
{
    if (e.flac_channel)
        return "flac-mono";
    if (e.flac_restorable)
        return "flac";
    return e.supported ? method_name(e.method) : std::format("unsupported ({})", e.method);
}

std::string format_time(std::int64_t t)
{
    const auto tp = std::chrono::sys_seconds(std::chrono::seconds(t));
    return std::format("{:%Y-%m-%d %H:%M}", tp);
}

struct ExtractRun
{
    int status = ZP_OK;
    nlohmann::json issues = nlohmann::json::array();
    nlohmann::json outcomes = nlohmann::json::array();
    std::size_t errors = 0;
    std::size_t written = 0;
};

ExtractRun extract_with(const GlobalOptions& g, Opened& o, zp_sink_t* sink, const std::vector<std::size_t>& selection, zp_extract_options_t options, bool prompt_overwrites, const char* label)
{
    ExtractRun run;
    zpp::ExtractionPlan plan(zpp::not_null(zp_extract_plan(o.reader.get(), selection.empty() ? nullptr : selection.data(), selection.size(), sink, &options)));
    zp_extract_issue_t issue{};
    for (std::size_t i = 0; i < zp_xplan_issue_count(plan.get()); ++i)
    {
        zpp::check(zp_xplan_get_issue(plan.get(), i, &issue));
        run.issues.push_back({ { "kind", issue_name(issue.kind) }, { "entry", zpp::str(issue.name) }, { "detail", zpp::str(issue.detail) }, { "error", issue.is_error != 0 } });
        run.errors += issue.is_error ? 1 : 0;
        if (!g.json && !g.quiet && issue.kind != ZP_ISSUE_EXISTS_AT_DESTINATION)
            std::cerr << std::format("{}: {}: {}\n", issue.is_error ? "error" : "warning", zpp::str(issue.name), zpp::str(issue.detail));
    }
    zp_extract_item_t item{};
    std::vector<std::size_t> undecided;
    for (std::size_t i = 0; i < zp_xplan_item_count(plan.get()); ++i)
    {
        zpp::check(zp_xplan_get_item(plan.get(), i, &item));
        if (item.decision == ZP_DECISION_UNDECIDED)
            undecided.push_back(i);
    }
    if (!undecided.empty())
    {
        if (!prompt_overwrites || !stdin_is_tty())
        {
            for (const auto i : undecided)
            {
                zpp::check(zp_xplan_get_item(plan.get(), i, &item));
                std::cerr << "exists: " << zpp::str(item.target) << '\n';
            }
            print_error("files already exist; use --overwrite skip|replace");
            run.status = ZP_DECISION_REQUIRED;
            return run;
        }
        for (const auto i : undecided)
        {
            zpp::check(zp_xplan_get_item(plan.get(), i, &item));
            const bool replace = confirm(std::format("Replace '{}'?", zpp::str(item.target)));
            zpp::check(zp_xplan_decide(plan.get(), i, replace ? ZP_DECISION_REPLACE : ZP_DECISION_SKIP));
        }
    }
    ProgressBar bar(label, g);
    run.status = zp_extract(plan.get(), zpp::ProgressAdapter::call, &bar.adapter());
    zp_extract_outcome_t outcome{};
    for (std::size_t i = 0; i < zp_xplan_outcome_count(plan.get()); ++i)
    {
        zpp::check(zp_xplan_get_outcome(plan.get(), i, &outcome));
        nlohmann::json jo{ { "target", zpp::str(outcome.target) }, { "status", zp_status_name(outcome.status) } };
        if (outcome.status != ZP_OK)
        {
            jo["message"] = zpp::str(outcome.message);
            ++run.errors;
            if (!g.json)
                std::cerr << std::format("error: {}: {}\n", zpp::str(outcome.target), zpp::str(outcome.message));
        }
        else
            ++run.written;
        run.outcomes.push_back(std::move(jo));
    }
    return run;
}

int exit_for(int status, std::size_t errors)
{
    if (status == ZP_CANCELLED)
        return exit_cancelled;
    if (status == ZP_DECISION_REQUIRED)
        return exit_declined;
    if (status != ZP_OK || errors > 0)
        return exit_partial;
    return exit_ok;
}

}

int run_list(const GlobalOptions& g, const std::string& archive)
{
    auto o = open_archive(g, archive);
    const auto entries = zpp::reader_entries(o.reader.get());
    std::array<char, 256> version{};
    const bool ours = zp_reader_get_app_version(o.reader.get(), version.data(), version.size()) == ZP_OK;
    if (g.json)
    {
        nlohmann::json j{ { "created_by", ours ? nlohmann::json(version.data()) : nlohmann::json(nullptr) }, { "zip64", zp_reader_zip64(o.reader.get()) != 0 } };
        j["entries"] = nlohmann::json::array();
        for (const auto& e : entries)
        {
            nlohmann::json je{ { "name", e.name }, { "kind", e.kind == ZP_KIND_DIRECTORY ? "directory" : e.kind == ZP_KIND_SYMLINK ? "symlink"
                                                                                                                                   : "file" },
                { "size", e.uncompressed_size },
                { "packed", e.compressed_size },
                { "method", entry_method(e) },
                { "crc32", e.crc32 },
                { "mtime", e.mtime } };
            if (!e.flac_original_name.empty())
                je["restores_to"] = e.flac_original_name;
            if (e.flac_channel)
                je["channel"] = { { "index", e.flac_channel->first }, { "count", e.flac_channel->second } };
            j["entries"].push_back(std::move(je));
        }
        print_json(j);
        return exit_ok;
    }
    std::uint64_t size = 0;
    std::uint64_t packed = 0;
    std::cout << std::format("{:>12} {:>12} {:<10} {:<16} {}\n", "Size", "Packed", "Method", "Modified", "Name");
    for (const auto& e : entries)
    {
        size += e.uncompressed_size;
        packed += e.compressed_size;
        std::cout << std::format("{:>12} {:>12} {:<10} {:<16} {}{}\n", e.uncompressed_size, e.compressed_size, entry_method(e), format_time(e.mtime), e.name, e.flac_original_name.empty() ? "" : "  -> " + e.flac_original_name);
    }
    std::cout << std::format("{} entries, {} -> {}; created by {}\n", entries.size(), human_size(size), human_size(packed), ours ? version.data() : "unknown tool");
    return exit_ok;
}

int run_extract(const GlobalOptions& g, const std::string& archive, const std::vector<std::string>& names, const std::string& destination, bool keep_flac, bool include_readme, const std::string& overwrite)
{
    auto o = open_archive(g, archive);
    std::vector<std::size_t> selection;
    if (!names.empty())
    {
        const auto entries = zpp::reader_entries(o.reader.get());
        for (const auto& n : names)
        {
            bool found = false;
            for (const auto& e : entries)
            {
                if (e.name == n || e.name == n + "/")
                {
                    selection.push_back(e.index);
                    found = true;
                }
            }
            if (!found)
                throw std::invalid_argument(std::format("no entry named '{}'", n));
        }
    }
    zp_extract_options_t options{};
    zp_extract_options_init(&options);
    options.restore_wav = keep_flac ? 0 : 1;
    options.include_readme = include_readme ? 1 : 0;
    options.overwrite = overwrite == "skip" ? ZP_OVERWRITE_SKIP : overwrite == "replace" ? ZP_OVERWRITE_REPLACE
                                                                                         : ZP_OVERWRITE_ASK;
    zpp::Sink sink(zpp::not_null(zp_sink_filesystem(destination.c_str())));
    const auto run = extract_with(g, o, sink.get(), selection, options, true, "Extracting");
    if (g.json)
        print_json({ { "status", zp_status_name(run.status) }, { "written", run.written }, { "issues", run.issues }, { "outcomes", run.outcomes } });
    else if (!g.quiet && run.status != ZP_DECISION_REQUIRED)
        std::cerr << std::format("{} file(s) written to {}\n", run.written, destination);
    return exit_for(run.status, run.errors);
}

int run_verify(const GlobalOptions& g, const std::string& archive)
{
    const auto start = std::chrono::steady_clock::now();
    auto o = open_archive(g, archive);
    zp_extract_options_t options{};
    zp_extract_options_init(&options);
    options.include_readme = 1;
    options.overwrite = ZP_OVERWRITE_REPLACE;
    zpp::Sink sink(zpp::not_null(zp_sink_null()));
    const auto run = extract_with(g, o, sink.get(), {}, options, false, "Verifying");
    const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (g.json)
        print_json({ { "status", run.errors == 0 && run.status == ZP_OK ? "OK" : zp_status_name(run.status) }, { "verified", run.written }, { "errors", run.errors }, { "seconds", seconds }, { "issues", run.issues }, { "outcomes", run.outcomes } });
    else if (!g.quiet)
        std::cout << std::format("{} entries verified, {} error(s) in {:.1f} s\n", run.written, run.errors, seconds);
    return exit_for(run.status, run.errors);
}

int run_restore(const GlobalOptions& g, const std::string& file, const std::string& output)
{
    zpp::Stream in(zpp::not_null(zp_stream_open_file(file.c_str())));
    std::filesystem::path target(output);
    if (output.empty())
    {
        std::array<char, 1024> name{};
        zpp::check(zp_flac_original_name(in.get(), name.data(), name.size()));
        const auto source = std::filesystem::path(file);
        target = source.parent_path() / (name[0] != '\0' ? std::filesystem::path(name.data()) : source.stem().concat(".wav"));
    }
    if (std::filesystem::exists(target) && !g.yes && !confirm(std::format("Replace '{}'?", target.string())))
        return exit_declined;
    zpp::Stream out(zpp::not_null(zp_stream_create_file(target.string().c_str())));
    zpp::check(zp_restore_flac(in.get(), out.get()));
    zpp::check(zp_stream_commit(out.get()));
    if (g.json)
        print_json({ { "status", "OK" }, { "output", target.string() } });
    else if (!g.quiet)
        std::cerr << "restored " << target.string() << '\n';
    return exit_ok;
}

int run_edit(const GlobalOptions& g, const std::string& archive, const std::vector<std::string>& adds, const std::vector<std::string>& removes, const std::vector<std::string>& renames, const std::vector<std::string>& replaces, const std::string& output)
{
    auto ctx = make_context(g);
    zpp::Stream in(zpp::not_null(zp_stream_open_file(archive.c_str())));
    zpp::Editor editor(zpp::not_null(zp_editor_open(ctx.get(), in.get())));
    std::vector<zpp::Input> inputs;

    const auto index_of = [&](const std::string& name) {
        zpp::Plan view(zpp::not_null(zp_editor_plan(editor.get())));
        for (const auto& e : zpp::plan_entries(view.get()))
        {
            if (e.output_name == name && e.codec != ZP_CODEC_GENERATED)
                return e.index;
        }
        throw std::invalid_argument(std::format("no entry named '{}'", name));
    };
    const auto split = [](const std::string& s, const char* flag) {
        const auto eq = s.find('=');
        if (eq == std::string::npos)
            throw std::invalid_argument(std::format("{} expects NAME=VALUE, got '{}'", flag, s));
        return std::pair{ s.substr(0, eq), s.substr(eq + 1) };
    };

    for (const auto& r : replaces)
    {
        const auto [name, path] = split(r, "--replace");
        inputs.push_back(zpp::input_from_paths({ path }));
        zpp::check(zp_editor_replace(editor.get(), index_of(name), inputs.back().get()));
    }
    for (const auto& r : renames)
    {
        const auto [name, to] = split(r, "--rename");
        zpp::check(zp_editor_rename(editor.get(), index_of(name), to.c_str()));
    }
    for (const auto& name : removes)
        zpp::check(zp_editor_remove(editor.get(), index_of(name)));
    if (!adds.empty())
    {
        inputs.push_back(zpp::input_from_paths(adds));
        zpp::check(zp_editor_add(editor.get(), inputs.back().get()));
    }

    zpp::Plan view(zpp::not_null(zp_editor_plan(editor.get())));
    const auto conflicts = zpp::plan_conflicts(view.get());
    if (!conflicts.empty())
    {
        const auto entries = zpp::plan_entries(view.get());
        std::cerr << std::format("{} unresolved conflict(s):\n", conflicts.size());
        for (const auto& c : conflicts)
        {
            for (const auto i : c.entries)
                std::cerr << "  " << entries[i].output_name << '\n';
        }
        return exit_conflicts;
    }
    const auto target = output.empty() ? archive : output;
    zpp::Stream out(zpp::not_null(zp_stream_create_file(target.c_str())));
    ProgressBar bar("Rewriting", g);
    zpp::check(zp_editor_commit(editor.get(), out.get(), zpp::ProgressAdapter::call, &bar.adapter()));
    editor.reset();
    in.reset();
    zpp::check(zp_stream_commit(out.get()));
    if (g.json)
        print_json({ { "status", "OK" }, { "output", target } });
    else if (!g.quiet)
        std::cerr << "updated " << target << '\n';
    return exit_ok;
}

int run_preview(const std::string& file, bool html)
{
    zpp::Stream in(zpp::not_null(zp_stream_open_file(file.c_str())));
    const auto name = std::filesystem::path(file).filename().string();
    char* text = zpp::not_null(zp_preview(in.get(), name.c_str(), html ? ZP_PREVIEW_HTML : ZP_PREVIEW_JSON));
    std::cout << text << '\n';
    zp_free(text);
    return exit_ok;
}

}
