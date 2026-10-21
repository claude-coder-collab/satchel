// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#pragma once

// A Full-mode session: a base archive (none for a new archive) plus staged changes, written in
// one step by commit() (desktop UI spec, "Building and editing archives"). No widgets; not
// thread-safe, so the caller serializes access (the GUI runs it on its job thread).

#include "zp_cpp.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

namespace satchel_gui
{

enum class ExistingEntries {
    Ask,
    Replace,
    ReplaceIfNewer,
    Keep,
};

struct SessionOptions
{
    bool flac = true;
    int flac_level = 5;
    int deflate_level = 6;
    std::vector<std::string> excludes;
    ExistingEntries existing = ExistingEntries::Ask;

    bool operator==(const SessionOptions&) const = default;
};

// Identifies an entry of the merged plan: where it comes from ("archive:<original name>" for
// entries of the base archive, the file path for additions) and its current name.
struct EntryKey
{
    std::string source;
    std::string name;

    bool operator==(const EntryKey&) const = default;
};

namespace op
{

// Files and folders from disk, placed under `folder` ("" = archive root).
struct Add
{
    std::vector<std::filesystem::path> sources;
    std::string folder;
};

struct NewFolder
{
    std::string path;
};

struct Rename
{
    EntryKey entry;
    std::string to;
};

// Renames a folder and everything below it; the folder need not have its own entry.
struct RenameFolder
{
    std::string from;
    std::string to;
};

struct Remove
{
    std::vector<EntryKey> entries;
};

struct RemoveFolder
{
    std::string path;
};

// Removes `entry` and adds `source` to the same folder, planned like any addition.
struct Replace
{
    EntryKey entry;
    std::filesystem::path source;
};

// A conflict decision (Rename / Skip / Store unconverted) for one entry.
struct Resolve
{
    EntryKey entry;
    int action = ZP_RESOLVE_SKIP;
    std::string new_name;
};

}

using Op = std::variant<op::Add, op::NewFolder, op::Rename, op::RenameFolder, op::Remove, op::RemoveFolder, op::Replace, op::Resolve>;
// One undo step.
using Change = std::vector<Op>;

enum class RowState {
    Unchanged,
    Added,
    Renamed,
    Replaced,
    Deleted,
};

struct SessionRow
{
    RowState state = RowState::Unchanged;
    EntryKey key; // Deleted: the base entry's key
    std::string previous_name; // Renamed: the name in the base archive
    std::optional<zpp::PlanEntry> plan; // empty for Deleted
    std::optional<std::size_t> base_index; // index into base_entries() (kept and deleted entries)
    bool generated = false; // the readme the library writes for converted audio
    bool stored = false; // added file the build will store, not deflate (sampled like the build)
    bool conflicted = false;
};

struct SessionConflict
{
    int kind = ZP_CONFLICT_COLLISION;
    std::string key;
    std::string detail;
    std::vector<EntryKey> entries;
};

struct SessionSummary
{
    // Files only; folders show as rows but are not counted.
    std::size_t added = 0;
    std::uint64_t added_bytes = 0;
    std::size_t removed = 0;
    // Name changes, counted where they happened (see count_moves): renaming a folder is one
    // rename, moving a folder one move. A change of both name and folder counts in both.
    std::size_t renamed = 0;
    std::size_t moved = 0;
    std::size_t replaced = 0;
    std::size_t entries = 0;
    std::uint64_t input_bytes = 0;
};

struct SessionView
{
    std::vector<SessionRow> rows; // merged plan order, then deleted base entries in archive order
    std::vector<SessionConflict> conflicts;
    std::vector<zpp::Warning> warnings;
    std::vector<zpp::Excluded> excluded;
    SessionSummary summary;
    bool executable = true;
    // Changes dropped because they no longer apply (e.g. an added file was deleted from disk).
    std::vector<std::string> problems;
};

using CommitProgress = std::function<bool(std::uint64_t done, std::uint64_t total)>;

struct MoveCount
{
    std::size_t renamed = 0;
    std::size_t moved = 0;
};

// Classifies path changes ("old" -> "new" archive paths, folders with or without a trailing
// slash) as renames (same folder, new name) or moves (same name, other folder). Changes are counted
// once per item that changed: entries whose folder was renamed count as that folder's rename, which
// is recognised by the old folder being gone (`after` has no entry under it) and the new one being
// new (`before` had none).
MoveCount count_moves(const std::vector<std::pair<std::string, std::string>>& changes, const std::vector<std::string>& before, const std::vector<std::string>& after);

class Session
{
public:
    // A new archive to be written to `target`; nothing is written before commit().
    static Session create(zp_context_t* context, std::filesystem::path target, SessionOptions options = {});
    // An existing archive. Throws zpp::Error if it cannot be read or edited.
    static Session open(zp_context_t* context, std::filesystem::path archive, SessionOptions options = {});

    Session(Session&&) noexcept = default;
    Session& operator=(Session&&) noexcept = default;
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    ~Session() = default;

    [[nodiscard]] bool is_new() const { return is_new_; }
    [[nodiscard]] const std::filesystem::path& target() const { return target_; }
    // Save As for a new archive: changes where commit() writes, nothing else.
    void set_target(std::filesystem::path target);
    [[nodiscard]] bool has_changes() const { return applied_ > 0; }
    [[nodiscard]] const std::vector<zpp::EntryInfo>& base_entries() const { return base_entries_; }

    // Applies one undo step and clears redo. Throws zpp::Error and leaves the session unchanged
    // if any operation fails, or if a rename would collide with another entry (ZP_NAME_COLLISION).
    void apply(Change change);
    [[nodiscard]] bool can_undo() const { return applied_ > 0; }
    [[nodiscard]] bool can_redo() const { return applied_ < history_.size(); }
    void undo();
    void redo();
    void discard();

    [[nodiscard]] const SessionOptions& options() const { return options_; }
    // Re-plans every staged change with the new options.
    void set_options(SessionOptions options);

    [[nodiscard]] const SessionView& view() const { return view_; }

    // Writes the merged plan to `output` (the archive itself for Apply: temp file, then atomic
    // swap) and reopens the session on `output` with no changes. Throws zpp::Error; on failure
    // or cancel the session keeps its staged changes and nothing at `output` changes. Added files
    // that changed since they were staged are left out of the written archive; returns them as
    // "name: reason".
    std::vector<std::string> commit(const std::filesystem::path& output, const zp_build_options_t* build_options = nullptr, const CommitProgress& progress = {});

private:
    Session(zp_context_t* context, std::filesystem::path target, bool is_new, SessionOptions options);

    void load_base();
    void reset_editor();
    void rebuild();
    void apply_op(const Op& o, std::vector<EntryKey>& renamed);
    void run(const op::Add&, std::vector<EntryKey>&);
    void run(const op::NewFolder&, std::vector<EntryKey>&);
    void run(const op::Rename&, std::vector<EntryKey>&);
    void run(const op::RenameFolder&, std::vector<EntryKey>&);
    void run(const op::Remove&, std::vector<EntryKey>&);
    void run(const op::RemoveFolder&, std::vector<EntryKey>&);
    void run(const op::Replace&, std::vector<EntryKey>&);
    void run(const op::Resolve&, std::vector<EntryKey>&);
    void apply_existing_policy(std::size_t first_added);
    void refresh_view();
    [[nodiscard]] std::size_t index_of(const EntryKey& key) const;
    [[nodiscard]] std::vector<zpp::PlanEntry> entries() const;

    zp_context_t* context_ = nullptr;
    std::filesystem::path target_;
    bool is_new_ = false;
    SessionOptions options_;
    std::vector<zpp::EntryInfo> base_entries_;
    std::vector<EntryKey> baseline_; // the editor's entries before any change

    std::vector<Change> history_;
    std::size_t applied_ = 0;

    zpp::Stream base_stream_;
    zpp::Editor editor_;
    std::vector<zpp::Input> inputs_;
    SessionView view_;
    // Store predictions by source, size, mtime and deflate level, so replays do not resample.
    std::map<std::tuple<std::string, std::uint64_t, std::int64_t, int>, bool> stored_;
};

}
