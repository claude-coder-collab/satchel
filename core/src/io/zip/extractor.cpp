// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "io/zip/extractor.hpp"

#include "codecs/codec.hpp"
#include "io/zip/path_policy.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <map>
#include <mutex>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace zp
{

bool ExtractionPlan::needs_decisions() const
{
    return std::ranges::any_of(items, [](const ExtractItem& i) { return i.decision == ItemDecision::Undecided; });
}

VoidResult ExtractionPlan::decide(std::size_t item, ItemDecision decision)
{
    if (item >= items.size())
        return fail(Status::InvalidArgument, "item index out of range");
    if (decision != ItemDecision::Skip && decision != ItemDecision::Replace)
        return fail(Status::InvalidArgument, "decision must be skip or replace");
    items[item].decision = decision;
    return {};
}

std::size_t ExtractionPlan::error_count() const
{
    return static_cast<std::size_t>(std::ranges::count_if(issues, &ExtractIssue::is_error));
}

ArchiveExtractor::ArchiveExtractor(ArchiveReader& reader, OutputSink& sink, Context& context, ExtractOptions options) :
    reader_(reader),
    sink_(sink),
    context_(context),
    options_(options)
{
}

Result<ExtractionPlan> ArchiveExtractor::plan_extraction(const std::vector<std::size_t>& selection) const
{
    const auto& entries = reader_.entries();
    std::vector<std::size_t> chosen = selection;
    if (chosen.empty())
    {
        chosen.resize(entries.size());
        for (std::size_t i = 0; i < chosen.size(); ++i)
            chosen[i] = i;
    }
    std::ranges::sort(chosen);
    chosen.erase(std::ranges::unique(chosen).begin(), chosen.end());

    const auto& meta = reader_.metadata();
    const std::string readme = meta ? meta->readme_name : std::string{};

    ExtractionPlan plan;
    struct Candidate
    {
        std::size_t entry;
        std::string target;
        std::string key;
    };
    std::vector<Candidate> candidates;

    for (const auto idx : chosen)
    {
        if (idx >= entries.size())
            return fail(Status::InvalidArgument, std::format("entry {} does not exist", idx));
        const auto& e = entries[idx];
        if (!readme.empty() && !options_.include_readme && e.name == readme && selection.empty())
            continue;
        if (e.kind == ItemKind::Symlink)
        {
            plan.issues.push_back({ ExtractIssueKind::SymlinkSkipped, idx, e.name, "symbolic links are not extracted" });
            continue;
        }
        auto target = PathPolicy::sanitize_for_extraction(e.name);
        if (!target)
        {
            plan.issues.push_back({ ExtractIssueKind::UnsafePath, idx, e.name, target.error().message });
            continue;
        }
        if (e.kind == ItemKind::File && e.method == EntryMethod::Unsupported)
        {
            plan.issues.push_back({ ExtractIssueKind::UnsupportedMethod, idx, e.name, e.encrypted ? std::string("encrypted entries are not supported") : std::format("compression method {} is not supported", e.raw_method) });
            continue;
        }
        auto key = PathPolicy::collision_key(*target);
        candidates.push_back({ idx, std::move(*target), key ? std::move(*key) : std::string{} });
    }

    std::map<std::string, std::vector<std::size_t>> by_key;
    std::map<std::string, std::size_t> file_keys;
    for (std::size_t c = 0; c < candidates.size(); ++c)
    {
        by_key[candidates[c].key].push_back(c);
        if (entries[candidates[c].entry].kind == ItemKind::File)
            file_keys.emplace(candidates[c].key, c);
    }
    std::set<std::size_t> refused;
    for (const auto& [key, list] : by_key)
    {
        if (list.size() < 2)
            continue;
        const bool all_dirs = std::ranges::all_of(list, [&](std::size_t c) { return entries[candidates[c].entry].kind == ItemKind::Directory; });
        if (all_dirs)
        {
            for (std::size_t k = 1; k < list.size(); ++k)
                refused.insert(list[k]);
            continue;
        }
        for (const auto c : list)
        {
            refused.insert(c);
            plan.issues.push_back({ ExtractIssueKind::NameCollision, candidates[c].entry, entries[candidates[c].entry].name, std::format("{} entries map to the same name on a case-insensitive file system", list.size()) });
        }
    }
    for (std::size_t c = 0; c < candidates.size(); ++c)
    {
        const auto& key = candidates[c].key;
        for (auto pos = key.find('/'); pos != std::string::npos; pos = key.find('/', pos + 1))
        {
            if (auto it = file_keys.find(key.substr(0, pos)); it != file_keys.end() && !refused.contains(c))
            {
                refused.insert(c);
                plan.issues.push_back({ ExtractIssueKind::NameCollision, candidates[c].entry, entries[candidates[c].entry].name, std::format("'{}' is a file in this archive but is used as a folder here", entries[candidates[it->second].entry].name) });
                break;
            }
        }
    }

    for (std::size_t c = 0; c < candidates.size(); ++c)
    {
        if (refused.contains(c))
            continue;
        const auto& e = entries[candidates[c].entry];
        ExtractItem item{ candidates[c].entry, candidates[c].target, e.kind, ItemDecision::Write };
        if (e.kind == ItemKind::File && sink_.exists(item.target))
        {
            switch (options_.overwrite)
            {
                case OverwritePolicy::Skip:
                    item.decision = ItemDecision::Skip;
                    break;
                case OverwritePolicy::Replace:
                    item.decision = ItemDecision::Replace;
                    break;
                case OverwritePolicy::Ask:
                    item.decision = ItemDecision::Undecided;
                    break;
            }
            plan.issues.push_back({ ExtractIssueKind::ExistsAtDestination, item.entry, e.name, std::format("'{}' already exists", item.target) });
        }
        plan.items.push_back(std::move(item));
    }
    return plan;
}

ExtractResult ArchiveExtractor::execute(const ExtractionPlan& plan, ProgressSink& progress)
{
    ExtractResult result;
    if (plan.needs_decisions())
    {
        result.status = Status::DecisionRequired;
        result.message = "some files already exist at the destination";
        return result;
    }

    std::lock_guard job_lock(context_.job_mutex());
    const auto& entries = reader_.entries();

    std::uint64_t total = 0;
    std::vector<const ExtractItem*> files;
    std::vector<const ExtractItem*> dirs;
    for (const auto& item : plan.items)
    {
        if (item.decision == ItemDecision::Skip)
        {
            ++result.skipped;
            continue;
        }
        if (item.kind == ItemKind::Directory)
            dirs.push_back(&item);
        else
        {
            files.push_back(&item);
            total += entries[item.entry].uncompressed_size;
        }
    }

    for (const auto* d : dirs)
    {
        if (auto r = sink_.make_directory(d->target); !r)
        {
            result.outcomes.push_back({ d->entry, d->target, r.error().status, r.error().message });
            continue;
        }
        ++result.directories_created;
    }

    std::atomic<std::uint64_t> done{ 0 };
    std::atomic<bool> cancelled{ false };
    std::mutex outcome_mutex;
    TaskCounter tasks;
    std::size_t written = 0;

    for (const auto* item : files)
    {
        tasks.add();
        context_.workers().submit([&, item] {
            const auto& e = entries[item->entry];
            EntryOutcome outcome{ item->entry, item->target, Status::Ok, {} };
            const auto finish = [&] {
                std::lock_guard lock(outcome_mutex);
                if (outcome.status == Status::Ok)
                    ++written;
                result.outcomes.push_back(std::move(outcome));
            };
            if (cancelled.load())
            {
                outcome.status = Status::Cancelled;
                finish();
                tasks.done();
                return;
            }
            auto run = [&]() -> VoidResult {
                auto in = reader_.open_entry(item->entry);
                if (!in)
                    return std::unexpected(in.error());
                auto out = sink_.create_file(item->target, item->decision == ItemDecision::Replace);
                if (!out)
                    return std::unexpected(out.error());
                std::vector<std::uint8_t> buf(256u << 10);
                std::uint32_t crc = 0;
                std::uint64_t size = 0;
                while (true)
                {
                    if (cancelled.load())
                        return fail(Status::Cancelled, "cancelled");
                    auto n = (*in)->read(buf.data(), buf.size());
                    if (!n)
                        return std::unexpected(n.error());
                    if (*n == 0)
                        break;
                    const std::span<const std::uint8_t> chunk(buf.data(), *n);
                    crc = crc32_update(crc, chunk);
                    size += *n;
                    if (size > e.uncompressed_size)
                        return fail(Status::CorruptArchive, std::format("'{}' is larger than recorded", e.name));
                    if (auto r = (*out)->write(chunk); !r)
                        return r;
                    done.fetch_add(*n);
                }
                if (size != e.uncompressed_size)
                    return fail(Status::CorruptArchive, std::format("'{}' is shorter than recorded", e.name));
                if (crc != e.crc32)
                    return fail(Status::CrcMismatch, std::format("CRC-32 of '{}' does not match", e.name));
                return (*out)->commit(e.mtime, e.unix_mode);
            };
            if (auto r = run(); !r)
            {
                outcome.status = r.error().status;
                outcome.message = r.error().message;
            }
            finish();
            tasks.done();
        });
    }

    while (!tasks.wait_for(std::chrono::milliseconds(100)))
    {
        if (!cancelled.load() && !progress.on_progress(done.load(), total))
            cancelled.store(true);
    }
    progress.on_progress(done.load(), total);

    std::ranges::sort(result.outcomes, {}, &EntryOutcome::entry);
    result.files_written = written;

    std::vector<const ExtractItem*> by_depth = dirs;
    std::ranges::sort(by_depth, std::greater<>{}, [](const ExtractItem* d) { return std::ranges::count(d->target, '/'); });
    for (const auto* d : by_depth)
    {
        const auto& e = entries[d->entry];
        std::ignore = sink_.set_directory_attributes(d->target, e.mtime, e.unix_mode);
    }

    if (cancelled.load())
    {
        result.status = Status::Cancelled;
        result.message = "cancelled";
        return result;
    }
    for (const auto& o : result.outcomes)
    {
        if (o.status != Status::Ok)
        {
            result.status = o.status;
            result.message = o.message;
            break;
        }
    }
    if (result.status == Status::Ok && plan.error_count() > 0)
    {
        result.status = plan.issues[static_cast<std::size_t>(std::ranges::find_if(plan.issues, &ExtractIssue::is_error) - plan.issues.begin())].kind == ExtractIssueKind::UnsafePath
            ? Status::UnsafePath
            : Status::NameCollision;
        result.message = std::format("{} entr{} refused", plan.error_count(), plan.error_count() == 1 ? "y" : "ies");
    }
    return result;
}

}
