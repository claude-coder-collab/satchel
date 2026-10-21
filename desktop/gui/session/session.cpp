// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "session.hpp"

#include <algorithm>
#include <array>
#include <ctime>
#include <map>
#include <ranges>
#include <set>
#include <string_view>
#include <utility>

namespace satchel_gui
{

namespace
{

constexpr std::array<std::uint8_t, 22> empty_zip{ 'P', 'K', 5, 6 };
constexpr std::string_view archive_prefix = "archive:";

std::string utf8(const std::filesystem::path& p)
{
    const auto s = p.u8string();
    return { reinterpret_cast<const char*>(s.data()), s.size() };
}

std::string trim_slashes(std::string s)
{
    while (!s.empty() && (s.back() == '/' || s.back() == '\\'))
        s.pop_back();
    while (!s.empty() && (s.front() == '/' || s.front() == '\\'))
        s.erase(s.begin());
    return s;
}

bool in_folder(std::string_view name, std::string_view folder)
{
    return name == folder || (name.size() > folder.size() && name.starts_with(folder) && name[folder.size()] == '/');
}

std::string parent_folder(std::string_view name)
{
    const auto slash = name.rfind('/');
    return slash == std::string_view::npos ? std::string{} : std::string(name.substr(0, slash));
}

zp_plan_options_t plan_options(const SessionOptions& o)
{
    zp_plan_options_t p;
    zp_plan_options_init(&p);
    p.flac_enabled = o.flac ? 1 : 0;
    p.flac_level = o.flac_level;
    p.deflate_level = o.deflate_level;
    return p;
}

}

Session::Session(zp_context_t* context, std::filesystem::path target, bool is_new, SessionOptions options) :
    context_(context),
    target_(std::move(target)),
    is_new_(is_new),
    options_(std::move(options))
{
}

Session Session::create(zp_context_t* context, std::filesystem::path target, SessionOptions options)
{
    Session s(context, std::move(target), true, std::move(options));
    s.load_base();
    return s;
}

Session Session::open(zp_context_t* context, std::filesystem::path archive, SessionOptions options)
{
    Session s(context, std::move(archive), false, std::move(options));
    s.load_base();
    return s;
}

void Session::set_target(std::filesystem::path target)
{
    target_ = std::move(target);
}

void Session::load_base()
{
    base_entries_.clear();
    if (!is_new_)
    {
        zpp::Stream stream(zpp::not_null(zp_stream_open_file(utf8(target_).c_str())));
        zpp::Reader reader(zpp::not_null(zp_reader_open(context_, stream.get())));
        base_entries_ = zpp::reader_entries(reader.get());
    }
    reset_editor();
    baseline_.clear();
    for (const auto& e : entries())
    {
        if (e.codec != ZP_CODEC_GENERATED)
            baseline_.push_back({ e.source_path, e.output_name });
    }
    refresh_view();
}

void Session::reset_editor()
{
    editor_.reset();
    base_stream_.reset();
    inputs_.clear();
    base_stream_ = zpp::Stream(zpp::not_null(is_new_ ? zp_stream_memory_from(empty_zip.data(), empty_zip.size()) : zp_stream_open_file(utf8(target_).c_str())));
    const auto o = plan_options(options_);
    editor_ = zpp::Editor(zpp::not_null(zp_editor_open(context_, base_stream_.get(), &o)));
}

std::vector<zpp::PlanEntry> Session::entries() const
{
    zpp::Plan plan(zpp::not_null(zp_editor_plan(editor_.get())));
    return zpp::plan_entries(plan.get());
}

std::size_t Session::index_of(const EntryKey& key) const
{
    for (const auto& e : entries())
    {
        if (e.source_path == key.source && e.output_name == key.name)
            return e.index;
    }
    throw zpp::Error(ZP_INVALID_ARGUMENT, "entry not found: " + key.name);
}

void Session::apply_op(const Op& o, std::vector<EntryKey>& renamed)
{
    std::visit([&](const auto& x) { run(x, renamed); }, o);
}

void Session::run(const op::Add& a, std::vector<EntryKey>&)
{
    std::vector<std::string> paths;
    paths.reserve(a.sources.size());
    for (const auto& p : a.sources)
        paths.push_back(utf8(p));
    auto input = zpp::input_from_paths(paths);
    zpp::set_excludes(input, options_.excludes);
    zpp::set_prefix(input, trim_slashes(a.folder));
    const auto first_added = static_cast<std::size_t>(std::ranges::count_if(entries(), [](const auto& e) { return e.codec != ZP_CODEC_GENERATED; }));
    zpp::check(zp_editor_add(editor_.get(), input.get()));
    inputs_.push_back(std::move(input));
    apply_existing_policy(first_added);
}

void Session::run(const op::NewFolder& f, std::vector<EntryKey>&)
{
    zpp::Input input(zpp::not_null(zp_input_memory()));
    zpp::check(zp_input_memory_add_directory(input.get(), trim_slashes(f.path).c_str(), std::time(nullptr), 0755));
    zpp::check(zp_editor_add(editor_.get(), input.get()));
    inputs_.push_back(std::move(input));
}

void Session::run(const op::Rename& r, std::vector<EntryKey>& renamed)
{
    const auto i = index_of(r.entry);
    zpp::check(zp_editor_rename(editor_.get(), i, r.to.c_str()));
    renamed.push_back({ r.entry.source, entries()[i].output_name });
}

void Session::run(const op::RenameFolder& r, std::vector<EntryKey>& renamed)
{
    const auto from = trim_slashes(r.from);
    const auto to = trim_slashes(r.to);
    std::vector<std::pair<std::size_t, std::string>> moves;
    for (const auto& e : entries())
    {
        if (e.codec != ZP_CODEC_GENERATED && in_folder(e.output_name, from))
            moves.emplace_back(e.index, to + e.output_name.substr(from.size()));
    }
    if (moves.empty())
        throw zpp::Error(ZP_INVALID_ARGUMENT, "folder not found: " + from);
    for (const auto& [i, name] : moves)
        zpp::check(zp_editor_rename(editor_.get(), i, name.c_str()));
    const auto now = entries();
    for (const auto& [i, name] : moves)
        renamed.push_back({ now[i].source_path, now[i].output_name });
}

void Session::run(const op::Remove& r, std::vector<EntryKey>&)
{
    std::map<std::pair<std::string_view, std::string_view>, std::size_t> index;
    const auto list = entries();
    for (const auto& e : list)
        index.emplace(std::pair<std::string_view, std::string_view>{ e.source_path, e.output_name }, e.index);
    std::vector<std::size_t> doomed;
    doomed.reserve(r.entries.size());
    for (const auto& k : r.entries)
    {
        const auto it = index.find({ k.source, k.name });
        if (it == index.end())
            throw zpp::Error(ZP_INVALID_ARGUMENT, "entry not found: " + k.name);
        doomed.push_back(it->second);
    }
    std::ranges::sort(doomed);
    const auto [first, last] = std::ranges::unique(doomed);
    doomed.erase(first, last);
    for (const auto i : doomed | std::views::reverse)
        zpp::check(zp_editor_remove(editor_.get(), i));
}

void Session::run(const op::RemoveFolder& r, std::vector<EntryKey>&)
{
    const auto folder = trim_slashes(r.path);
    std::vector<std::size_t> doomed;
    for (const auto& e : entries())
    {
        if (e.codec != ZP_CODEC_GENERATED && in_folder(e.output_name, folder))
            doomed.push_back(e.index);
    }
    if (doomed.empty())
        throw zpp::Error(ZP_INVALID_ARGUMENT, "folder not found: " + folder);
    for (const auto i : doomed | std::views::reverse)
        zpp::check(zp_editor_remove(editor_.get(), i));
}

void Session::run(const op::Replace& r, std::vector<EntryKey>&)
{
    const auto i = index_of(r.entry);
    const auto folder = parent_folder(entries()[i].output_name);
    zpp::check(zp_editor_remove(editor_.get(), i));
    auto input = zpp::input_from_paths({ utf8(r.source) });
    zpp::set_prefix(input, folder);
    zpp::check(zp_editor_add(editor_.get(), input.get()));
    inputs_.push_back(std::move(input));
}

void Session::run(const op::Resolve& r, std::vector<EntryKey>&)
{
    const zp_resolution_t res{ index_of(r.entry), r.action, r.action == ZP_RESOLVE_RENAME ? r.new_name.c_str() : nullptr };
    zpp::check(zp_editor_resolve(editor_.get(), &res, 1));
}

void Session::apply_existing_policy(std::size_t first_added)
{
    if (options_.existing == ExistingEntries::Ask)
        return;
    zpp::Plan plan(zpp::not_null(zp_editor_plan(editor_.get())));
    const auto list = zpp::plan_entries(plan.get());
    std::vector<zp_resolution_t> skips;
    for (const auto& c : zpp::plan_conflicts(plan.get()))
    {
        if (c.kind != ZP_CONFLICT_COLLISION || c.entries.size() != 2)
            continue;
        auto older = c.entries[0];
        auto newer = c.entries[1];
        if (older > newer)
            std::swap(older, newer);
        if (older >= first_added || newer < first_added)
            continue;
        bool replace = options_.existing == ExistingEntries::Replace;
        if (options_.existing == ExistingEntries::ReplaceIfNewer)
            replace = list[newer].mtime > list[older].mtime;
        skips.push_back({ replace ? older : newer, ZP_RESOLVE_SKIP, nullptr });
    }
    if (!skips.empty())
        zpp::check(zp_editor_resolve(editor_.get(), skips.data(), skips.size()));
}

void Session::rebuild()
{
    reset_editor();
    view_.problems.clear();
    std::vector<EntryKey> ignored;
    for (std::size_t i = 0; i < applied_;)
    {
        try
        {
            for (const auto& o : history_[i])
                apply_op(o, ignored);
            ++i;
        } catch (const zpp::Error& e)
        {
            view_.problems.emplace_back(e.what());
            history_.erase(history_.begin() + static_cast<std::ptrdiff_t>(i));
            --applied_;
            reset_editor();
            for (std::size_t j = 0; j < i; ++j)
            {
                for (const auto& o : history_[j])
                    apply_op(o, ignored);
            }
        }
    }
    refresh_view();
}

void Session::apply(Change change)
{
    std::vector<EntryKey> renamed;
    try
    {
        for (const auto& o : change)
            apply_op(o, renamed);
        if (!renamed.empty())
        {
            zpp::Plan plan(zpp::not_null(zp_editor_plan(editor_.get())));
            const auto list = zpp::plan_entries(plan.get());
            for (const auto& c : zpp::plan_conflicts(plan.get()))
            {
                for (const auto i : c.entries)
                {
                    const EntryKey k{ list[i].source_path, list[i].output_name };
                    if (std::ranges::find(renamed, k) != renamed.end())
                        throw zpp::Error(ZP_NAME_COLLISION, "'" + k.name + "' already exists");
                }
            }
        }
    } catch (...)
    {
        rebuild();
        throw;
    }
    history_.resize(applied_);
    history_.push_back(std::move(change));
    ++applied_;
    view_.problems.clear();
    refresh_view();
}

void Session::undo()
{
    if (!can_undo())
        return;
    --applied_;
    rebuild();
}

void Session::redo()
{
    if (!can_redo())
        return;
    std::vector<EntryKey> ignored;
    try
    {
        for (const auto& o : history_[applied_])
            apply_op(o, ignored);
    } catch (...)
    {
        history_.resize(applied_);
        rebuild();
        throw;
    }
    ++applied_;
    refresh_view();
}

void Session::discard()
{
    history_.clear();
    applied_ = 0;
    rebuild();
}

void Session::set_options(SessionOptions options)
{
    if (options == options_)
        return;
    options_ = std::move(options);
    rebuild();
}

namespace
{

std::vector<std::string> split_path(std::string_view path)
{
    std::vector<std::string> parts;
    for (const auto part : std::views::split(path, '/'))
    {
        if (!part.empty())
            parts.emplace_back(part.begin(), part.end());
    }
    return parts;
}

std::string join_parts(const std::vector<std::string>& parts, std::size_t n)
{
    std::string out;
    for (std::size_t i = 0; i < n; ++i)
        out += (i ? "/" : "") + parts[i];
    return out;
}

std::set<std::string, std::less<>> folders_of(const std::vector<std::string>& names)
{
    std::set<std::string, std::less<>> out;
    for (const auto& n : names)
    {
        const auto parts = split_path(n);
        const auto own = n.ends_with('/') ? parts.size() : parts.size() - std::min<std::size_t>(parts.size(), 1);
        for (std::size_t i = 1; i <= own; ++i)
            out.insert(join_parts(parts, i));
    }
    return out;
}

}

MoveCount count_moves(const std::vector<std::pair<std::string, std::string>>& changes, const std::vector<std::string>& before, const std::vector<std::string>& after)
{
    const auto folders_before = folders_of(before);
    const auto folders_after = folders_of(after);
    std::set<std::pair<std::string, std::string>> renames;
    std::set<std::pair<std::string, std::string>> moves;
    for (const auto& [from, to] : changes)
    {
        const auto p = split_path(from);
        const auto n = split_path(to);
        if (p.empty() || n.empty() || p == n)
            continue;
        std::size_t k = 0;
        while (k < p.size() && k < n.size() && p[p.size() - 1 - k] == n[n.size() - 1 - k])
            ++k;
        if (k == 0)
        {
            const bool same_folder = p.size() == n.size() && join_parts(p, p.size() - 1) == join_parts(n, n.size() - 1);
            renames.emplace(join_parts(p, p.size()), join_parts(n, n.size()));
            if (!same_folder)
                moves.emplace(join_parts(p, p.size()), join_parts(n, n.size()));
            continue;
        }
        const auto a = p.size() - k;
        const auto b = n.size() - k;
        const auto old_folder = join_parts(p, a);
        const auto new_folder = join_parts(n, b);
        const bool folder_renamed = a == b && a >= 1 && join_parts(p, a - 1) == join_parts(n, b - 1) && !folders_after.contains(old_folder) && !folders_before.contains(new_folder);
        if (folder_renamed)
            renames.emplace(old_folder, new_folder);
        else
            moves.emplace(join_parts(p, a + 1), join_parts(n, b + 1));
    }
    return { renames.size(), moves.size() };
}

void Session::refresh_view()
{
    zpp::Plan plan(zpp::not_null(zp_editor_plan(editor_.get())));
    const auto list = zpp::plan_entries(plan.get());
    auto problems = std::move(view_.problems);
    view_ = {};
    view_.problems = std::move(problems);
    view_.warnings = zpp::plan_warnings(plan.get());
    view_.excluded = zpp::plan_excluded(plan.get());
    view_.executable = zp_plan_executable(plan.get()) != 0;

    std::map<std::string, std::size_t, std::less<>> base_by_name;
    for (const auto& b : base_entries_)
        base_by_name.emplace(b.name, b.index);
    const auto base_index = [&](std::string_view name) -> std::optional<std::size_t> {
        if (const auto it = base_by_name.find(name); it != base_by_name.end())
            return it->second;
        return std::nullopt;
    };
    std::map<std::string, std::string, std::less<>> baseline_name;
    for (const auto& k : baseline_)
        baseline_name.emplace(k.source, k.name);

    std::set<std::string, std::less<>> present;
    std::vector<SessionRow> added;
    for (const auto& e : list)
    {
        SessionRow row;
        row.key = { e.source_path, e.output_name };
        row.plan = e;
        if (e.codec == ZP_CODEC_GENERATED)
        {
            row.generated = true;
            row.base_index = base_index(e.output_name);
            row.state = row.base_index ? RowState::Unchanged : RowState::Added;
        }
        else if (e.source_path.starts_with(archive_prefix))
        {
            present.insert(e.source_path);
            const auto original = std::string_view(e.source_path).substr(archive_prefix.size());
            row.base_index = base_index(original);
            const auto it = baseline_name.find(e.source_path);
            if (it != baseline_name.end() && it->second != e.output_name)
            {
                row.state = RowState::Renamed;
                row.previous_name = it->second;
            }
        }
        else
        {
            row.state = RowState::Added;
            if (e.kind == ZP_KIND_FILE && (e.codec == ZP_CODEC_GENERAL || e.fallback_reason >= 0))
            {
                const auto key = std::tuple{ e.source_path, e.size, e.mtime, options_.deflate_level };
                auto it = stored_.find(key);
                if (it == stored_.end())
                {
                    int store = 0;
                    if (zp_plan_predict_store(plan.get(), e.index, &store) != ZP_OK)
                        store = 0;
                    it = stored_.emplace(key, store != 0).first;
                }
                row.stored = it->second;
            }
        }
        view_.rows.push_back(std::move(row));
    }

    std::map<std::string, SessionRow, std::less<>> deleted;
    for (const auto& k : baseline_)
    {
        if (present.contains(k.source))
            continue;
        SessionRow row;
        row.state = RowState::Deleted;
        row.key = k;
        row.base_index = base_index(std::string_view(k.source).substr(archive_prefix.size()));
        deleted.emplace(k.name, std::move(row));
    }
    for (auto& row : view_.rows)
    {
        if (row.state != RowState::Added || row.generated)
            continue;
        if (const auto it = deleted.find(row.key.name); it != deleted.end())
        {
            row.state = RowState::Replaced;
            row.base_index = it->second.base_index;
            deleted.erase(it);
            ++view_.summary.replaced;
        }
        else if (row.plan && row.plan->kind == ZP_KIND_FILE)
        {
            ++view_.summary.added;
            view_.summary.added_bytes += row.plan->size;
        }
    }
    for (const auto& k : baseline_)
    {
        if (const auto it = deleted.find(k.name); it != deleted.end())
        {
            const auto& gone = it->second;
            if (gone.base_index && *gone.base_index < base_entries_.size() && base_entries_[*gone.base_index].kind == ZP_KIND_FILE)
                ++view_.summary.removed;
            view_.rows.push_back(std::move(it->second));
            deleted.erase(it);
        }
    }

    for (const auto& c : zpp::plan_conflicts(plan.get()))
    {
        SessionConflict sc{ c.kind, c.key, c.detail, {} };
        for (const auto i : c.entries)
        {
            sc.entries.push_back({ list[i].source_path, list[i].output_name });
            view_.rows[i].conflicted = true;
        }
        view_.conflicts.push_back(std::move(sc));
    }
    std::vector<std::pair<std::string, std::string>> changes;
    std::vector<std::string> before;
    std::vector<std::string> after;
    for (const auto& k : baseline_)
        before.push_back(k.name);
    for (const auto& r : view_.rows)
    {
        if (r.state != RowState::Deleted)
            after.push_back(r.key.name);
        if (r.state == RowState::Renamed)
            changes.emplace_back(r.previous_name, r.key.name);
    }
    const auto moves = count_moves(changes, before, after);
    view_.summary.renamed = moves.renamed;
    view_.summary.moved = moves.moved;
    view_.summary.entries = list.size();
    view_.summary.input_bytes = zp_plan_total_bytes(plan.get());
}

std::vector<std::string> Session::commit(const std::filesystem::path& output, const zp_build_options_t* build_options, const CommitProgress& progress)
{
    if (!view_.executable)
        throw zpp::Error(ZP_CONFLICTS_UNRESOLVED, "resolve the conflicts first");
    zpp::Stream out(zpp::not_null(zp_stream_create_file(utf8(output).c_str())));
    zpp::ProgressAdapter adapter{ progress };
    zp_build_result_t* raw = nullptr;
    const auto status = zp_editor_commit(editor_.get(), out.get(), build_options, zpp::ProgressAdapter::call, &adapter, &raw);
    const zpp::BuildResult result(raw);
    if (status != ZP_SOURCE_CHANGED)
        zpp::check(status);
    std::vector<std::string> left_out;
    for (std::size_t i = 0; result && i < zp_build_result_entry_count(result.get()); ++i)
    {
        zp_entry_result_t r{};
        zpp::check(zp_build_result_get_entry(result.get(), i, &r));
        if (r.status != ZP_OK)
            left_out.push_back(std::string(r.name ? r.name : "") + ": " + (r.message ? r.message : ""));
    }
    editor_.reset();
    base_stream_.reset();
    inputs_.clear();
    if (const auto committed = zp_stream_commit(out.get()); committed != ZP_OK)
    {
        const auto error = zpp::last_error(committed);
        rebuild();
        throw zpp::Error(error.status(), error.what());
    }
    out.reset();
    target_ = output;
    is_new_ = false;
    history_.clear();
    applied_ = 0;
    view_.problems.clear();
    load_base();
    return left_out;
}

}
