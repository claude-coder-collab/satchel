// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "io/zip/editor.hpp"

#include "io/zip/path_policy.hpp"

#include <format>

namespace zp
{

ArchiveEditor::ArchiveEditor(Context& context, PlannerOptions options) :
    context_(context),
    planner_(options),
    options_(options)
{
}

VoidResult ArchiveEditor::open(IChunkedStream& input)
{
    auto reader = ArchiveReader::open(input);
    if (!reader)
        return std::unexpected(reader.error());
    entries_.clear();
    warnings_.clear();
    const auto& list = (*reader)->entries();
    for (std::size_t i = 0; i < list.size(); ++i)
    {
        const auto& e = list[i];
        if (e.encrypted)
            return fail(Status::UnsupportedMethod, std::format("'{}' is encrypted; archives with encrypted entries cannot be edited", e.name));
        if (e.kind == ItemKind::File && e.method == EntryMethod::Unsupported)
            return fail(Status::UnsupportedMethod, std::format("'{}' uses compression method {}; such archives cannot be edited", e.name, e.raw_method));
        if (e.kind == ItemKind::Symlink)
        {
            warnings_.push_back({ WarningKind::SymlinkSkipped, e.name, "symbolic link entries are dropped when the archive is rewritten" });
            continue;
        }
        PlanEntry p;
        p.item.source_path = "archive:" + e.name;
        p.item.archive_path = e.name;
        p.item.kind = e.kind;
        p.item.size = e.uncompressed_size;
        p.item.mtime_ns = e.mtime * 1'000'000'000;
        p.item.unix_mode = e.unix_mode.value_or(e.kind == ItemKind::Directory ? 0755u : 0644u);
        auto n = PathPolicy::normalize(e.name);
        p.output_name = n ? *n : e.name;
        p.codec = PlanCodec::Kept;
        p.kept_index = i;
        p.snapshot = p.item.snapshot();
        entries_.push_back(std::move(p));
    }
    reader_ = std::move(*reader);
    return {};
}

VoidResult ArchiveEditor::add(InputSource& input)
{
    auto items = input.enumerate();
    if (!items)
        return std::unexpected(items.error());
    auto added = planner_.make_entries(std::move(*items), warnings_);
    if (!added)
        return std::unexpected(added.error());
    for (auto& e : *added)
        entries_.push_back(std::move(e));
    return {};
}

VoidResult ArchiveEditor::remove(std::size_t index)
{
    if (index >= entries_.size())
        return fail(Status::InvalidArgument, "entry index out of range");
    entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(index));
    return {};
}

VoidResult ArchiveEditor::rename(std::size_t index, const std::string& new_name)
{
    if (index >= entries_.size())
        return fail(Status::InvalidArgument, "entry index out of range");
    auto n = PathPolicy::normalize(new_name);
    if (!n)
        return std::unexpected(n.error());
    if (auto v = PathPolicy::validate_for_archive(*n); !v)
        return v;
    entries_[index].output_name = std::move(*n);
    return {};
}

VoidResult ArchiveEditor::replace(std::size_t index, InputItem item)
{
    if (index >= entries_.size())
        return fail(Status::InvalidArgument, "entry index out of range");
    if (item.kind != ItemKind::File)
        return fail(Status::InvalidArgument, "an entry can only be replaced by a file");
    auto& e = entries_[index];
    e.item = std::move(item);
    e.codec = PlanCodec::General;
    e.kept_index.reset();
    e.snapshot = e.item.snapshot();
    return {};
}

ArchivePlan ArchiveEditor::plan() const
{
    ArchivePlan p;
    p.options = options_;
    p.entries = entries_;
    p.warnings = warnings_;
    ArchivePlanner::check(p);
    return p;
}

BuildResult ArchiveEditor::commit(IChunkedStream& output, ProgressSink& progress)
{
    if (!reader_)
    {
        BuildResult r;
        r.status = Status::InvalidArgument;
        r.message = "no archive is open";
        return r;
    }
    ArchiveBuilder builder(output, context_);
    builder.set_kept_source(reader_.get());
    return builder.execute(plan(), progress);
}

}
