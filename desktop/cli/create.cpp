// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "cli.hpp"

#include <charconv>
#include <cstdio>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <sstream>

#ifdef _WIN32
    #include <fcntl.h>
    #include <io.h>
#endif

namespace satchel_cli
{

namespace
{

struct Request
{
    std::string selector;
    int action = ZP_RESOLVE_SKIP;
    std::string new_name;
};

std::optional<std::size_t> find_entry(const std::vector<zpp::PlanEntry>& entries, const std::string& selector)
{
    if (selector.starts_with('#'))
    {
        std::size_t i = 0;
        const auto digits = std::string_view(selector).substr(1);
        const auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), i);
        if (ec != std::errc{} || end != digits.data() + digits.size() || i >= entries.size())
            return std::nullopt;
        return i;
    }
    for (const auto& e : entries)
    {
        if (e.output_name == selector)
            return e.index;
    }
    for (const auto& e : entries)
    {
        if (e.restored_name == selector || e.source_path == selector)
            return e.index;
    }
    return std::nullopt;
}

int action_from(const std::string& name)
{
    if (name == "rename")
        return ZP_RESOLVE_RENAME;
    if (name == "skip")
        return ZP_RESOLVE_SKIP;
    if (name == "store-unconverted" || name == "disable_flac" || name == "disable-flac")
        return ZP_RESOLVE_DISABLE_FLAC;
    throw std::invalid_argument(std::format("unknown resolution action '{}'", name));
}

std::vector<Request> collect(const std::vector<std::string>& renames, const std::vector<std::string>& skips, const std::vector<std::string>& store, const std::string& file)
{
    std::vector<Request> out;
    for (const auto& r : renames)
    {
        const auto eq = r.find('=');
        if (eq == std::string::npos)
            throw std::invalid_argument(std::format("--rename expects NAME=NEW_NAME, got '{}'", r));
        out.push_back({ r.substr(0, eq), ZP_RESOLVE_RENAME, r.substr(eq + 1) });
    }
    for (const auto& s : skips)
        out.push_back({ s, ZP_RESOLVE_SKIP, {} });
    for (const auto& s : store)
        out.push_back({ s, ZP_RESOLVE_DISABLE_FLAC, {} });
    if (!file.empty())
    {
        std::ifstream f(file);
        if (!f)
            throw std::invalid_argument(std::format("cannot read resolution file '{}'", file));
        const auto j = nlohmann::json::parse(f);
        for (const auto& item : j)
        {
            Request r;
            const auto& entry = item.at("entry");
            r.selector = entry.is_number() ? std::format("#{}", entry.get<std::size_t>()) : entry.get<std::string>();
            r.action = action_from(item.at("action").get<std::string>());
            if (r.action == ZP_RESOLVE_RENAME)
                r.new_name = item.at("new_name").get<std::string>();
            out.push_back(std::move(r));
        }
    }
    return out;
}

std::string fallback_name(int reason)
{
    static const std::array<const char*, 8> names{ "not-parsable", "float-samples", "unsupported-encoding", "bit-depth", "sample-rate", "too-many-samples", "chunk-too-large", "not-seekable" };
    return reason >= 0 && static_cast<std::size_t>(reason) < names.size() ? names[static_cast<std::size_t>(reason)] : "";
}

nlohmann::json plan_json(const std::vector<zpp::PlanEntry>& entries, const std::vector<zpp::Conflict>& conflicts, const std::vector<zpp::Warning>& warnings)
{
    nlohmann::json j;
    j["entries"] = nlohmann::json::array();
    for (const auto& e : entries)
    {
        nlohmann::json je{ { "index", e.index }, { "source", e.source_path }, { "name", e.output_name }, { "codec", codec_name(e.codec) }, { "kind", e.kind == ZP_KIND_DIRECTORY ? "directory" : "file" }, { "size", e.size } };
        if (!e.restored_name.empty())
            je["restores_to"] = e.restored_name;
        if (e.fallback_reason >= 0)
            je["fallback"] = { { "reason", fallback_name(e.fallback_reason) }, { "detail", e.fallback_detail } };
        j["entries"].push_back(std::move(je));
    }
    j["conflicts"] = nlohmann::json::array();
    for (const auto& c : conflicts)
    {
        nlohmann::json names = nlohmann::json::array();
        for (const auto i : c.entries)
            names.push_back(entries[i].output_name);
        j["conflicts"].push_back({ { "kind", c.kind == ZP_CONFLICT_COLLISION ? "collision" : "invalid-name" }, { "key", c.key }, { "detail", c.detail }, { "entries", c.entries }, { "names", names } });
    }
    j["warnings"] = nlohmann::json::array();
    for (const auto& w : warnings)
        j["warnings"].push_back({ { "kind", w.kind == ZP_WARNING_SYMLINK_SKIPPED ? "symlink-skipped" : "flac-fallback" }, { "source", w.source_path }, { "detail", w.detail } });
    return j;
}

std::int64_t write_stdout(void*, const std::uint8_t* buf, std::size_t len)
{
    const auto n = std::fwrite(buf, 1, len, stdout);
    return n == len ? static_cast<std::int64_t>(n) : -1;
}

}

int run_create(const GlobalOptions& g, const std::string& archive, const std::vector<std::string>& paths, bool no_flac, int deflate_level, int flac_level, const std::vector<std::string>& renames, const std::vector<std::string>& skips, const std::vector<std::string>& store_unconverted, const std::string& resolutions_file, bool dry_run, const std::string& readme_template_file, const std::string& temp_dir)
{
    auto ctx = make_context(g);
    auto input = zpp::input_from_paths(paths);
    zp_plan_options_t options{};
    zp_plan_options_init(&options);
    options.flac_enabled = no_flac ? 0 : 1;
    options.deflate_level = deflate_level;
    options.flac_level = flac_level;
    zpp::Plan plan(zpp::not_null(zp_plan_create(ctx.get(), input.get(), &options)));

    const auto requests = collect(renames, skips, store_unconverted, resolutions_file);
    if (!requests.empty())
    {
        const auto entries = zpp::plan_entries(plan.get());
        std::vector<zp_resolution_t> resolutions;
        for (const auto& r : requests)
        {
            const auto index = find_entry(entries, r.selector);
            if (!index)
                throw std::invalid_argument(std::format("no planned entry matches '{}'", r.selector));
            resolutions.push_back({ *index, r.action, r.action == ZP_RESOLVE_RENAME ? r.new_name.c_str() : nullptr });
        }
        zpp::check(zp_plan_resolve(plan.get(), resolutions.data(), resolutions.size()));
    }

    const auto entries = zpp::plan_entries(plan.get());
    const auto conflicts = zpp::plan_conflicts(plan.get());
    const auto warnings = zpp::plan_warnings(plan.get());

    if (!conflicts.empty())
    {
        if (g.json)
        {
            auto j = plan_json(entries, conflicts, warnings);
            j["status"] = "CONFLICTS_UNRESOLVED";
            print_json(j);
        }
        else
        {
            std::cerr << std::format("{} unresolved conflict(s); resolve with --rename, --skip, --store-unconverted or --resolutions:\n", conflicts.size());
            for (const auto& c : conflicts)
            {
                std::cerr << "  " << c.detail << ":\n";
                for (const auto i : c.entries)
                    std::cerr << std::format("    #{} {}  (from {})\n", i, entries[i].output_name, entries[i].source_path);
            }
        }
        return exit_conflicts;
    }

    bool symlinks = false;
    if (!g.json && !g.quiet)
    {
        for (const auto& w : warnings)
            std::cerr << "warning: " << w.source_path << ": " << w.detail << '\n';
    }
    for (const auto& w : warnings)
        symlinks |= w.kind == ZP_WARNING_SYMLINK_SKIPPED;
    if (symlinks && !g.yes && !dry_run)
    {
        if (!stdin_is_tty())
        {
            print_error("symbolic links will be skipped; pass --yes to proceed without them");
            return exit_declined;
        }
        if (!confirm("Symbolic links will not be archived. Continue?"))
            return exit_declined;
    }

    if (dry_run)
    {
        if (g.json)
            print_json(plan_json(entries, conflicts, warnings));
        else
        {
            for (const auto& e : entries)
                std::cout << std::format("{:<10} {}{}\n", codec_name(e.codec), e.output_name, e.restored_name.empty() ? "" : "  -> " + e.restored_name);
        }
        return exit_ok;
    }

    std::string readme_template;
    if (!readme_template_file.empty())
    {
        std::ifstream f(readme_template_file, std::ios::binary);
        if (!f)
            throw std::invalid_argument(std::format("cannot read readme template '{}'", readme_template_file));
        readme_template.assign(std::istreambuf_iterator<char>(f), {});
    }
    zp_build_options_t build{};
    build.readme_template = readme_template_file.empty() ? nullptr : readme_template.c_str();
    build.temp_dir = temp_dir.empty() ? nullptr : temp_dir.c_str();

    const bool to_stdout = archive == "-";
    zpp::Stream out;
    if (to_stdout)
    {
#ifdef _WIN32
        _setmode(_fileno(stdout), _O_BINARY);
#endif
        zp_stream_callbacks_t cb{};
        cb.write = write_stdout;
        out.reset(zpp::not_null(zp_stream_from_callbacks(&cb)));
    }
    else
        out.reset(zpp::not_null(zp_stream_create_file(archive.c_str())));

    ProgressBar bar("Compressing", g);
    zp_build_result_t* raw_result = nullptr;
    const int status = zp_build(plan.get(), out.get(), &build, zpp::ProgressAdapter::call, &bar.adapter(), &raw_result);
    zpp::BuildResult result(raw_result);
    const std::string message = zp_last_error();
    if (status == ZP_OK || status == ZP_SOURCE_CHANGED)
    {
        if (!to_stdout)
            zpp::check(zp_stream_commit(out.get()));
        else if (std::fflush(stdout) != 0)
            throw zpp::Error(ZP_IO_ERROR, "cannot write to stdout");
    }

    std::uint64_t in_total = 0;
    std::uint64_t out_total = 0;
    nlohmann::json results = nlohmann::json::array();
    for (std::size_t i = 0; result && i < zp_build_result_entry_count(result.get()); ++i)
    {
        zp_entry_result_t r{};
        zpp::check(zp_build_result_get_entry(result.get(), i, &r));
        const auto& e = entries[r.plan_index];
        in_total += e.codec == ZP_CODEC_FLAC_MONO && e.channel_index > 1 ? 0 : e.size;
        out_total += r.compressed_size;
        nlohmann::json jr{ { "name", zpp::str(r.name) }, { "method", e.codec == ZP_CODEC_FLAC || e.codec == ZP_CODEC_FLAC_MONO ? "flac" : method_name(r.method) }, { "size", r.uncompressed_size }, { "packed", r.compressed_size }, { "status", zp_status_name(r.status) } };
        if (r.status != ZP_OK)
            jr["message"] = zpp::str(r.message);
        if (e.fallback_reason >= 0)
            jr["fallback"] = { { "reason", fallback_name(e.fallback_reason) }, { "detail", e.fallback_detail } };
        results.push_back(std::move(jr));
    }

    if (g.json)
    {
        auto j = plan_json(entries, conflicts, warnings);
        j["status"] = zp_status_name(status);
        j["results"] = results;
        j["input_bytes"] = in_total;
        j["archive_bytes"] = out_total;
        if (status != ZP_OK)
            j["message"] = message;
        if (!to_stdout)
            print_json(j);
        else
            std::cerr << j.dump(2) << '\n';
    }
    else if (!g.quiet)
    {
        for (const auto& r : results)
        {
            if (r["status"] != "OK")
                std::cerr << std::format("error: {}: {}\n", r["name"].get<std::string>(), r.value("message", ""));
        }
        if (status == ZP_OK || status == ZP_SOURCE_CHANGED)
        {
            const auto saved = in_total > 0 ? 100.0 * (1.0 - static_cast<double>(out_total) / static_cast<double>(in_total)) : 0.0;
            const auto change = saved >= 0 ? std::format("{:.0f}% smaller", saved) : std::format("{:.0f}% larger", -saved);
            std::cerr << std::format("{} entries, {} -> {} ({})\n", results.size(), human_size(in_total), human_size(out_total), change);
        }
    }
    if (status == ZP_CANCELLED)
        return exit_cancelled;
    if (status == ZP_SOURCE_CHANGED)
        return exit_partial;
    if (status != ZP_OK)
    {
        print_error(message);
        return exit_error;
    }
    return exit_ok;
}

}
