// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#pragma once

#include "common/status.hpp"
#include "io/zip/output_sink.hpp"
#include "io/zip/reader.hpp"
#include "pipeline/context.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace zp
{

enum class OverwritePolicy : std::uint8_t {
    Ask,
    Skip,
    Replace,
};

struct ExtractOptions
{
    bool restore_wav = true;
    bool include_readme = false;
    OverwritePolicy overwrite = OverwritePolicy::Ask;
};

enum class ExtractIssueKind : std::uint8_t {
    SymlinkSkipped, // warning
    UnsupportedMethod, // warning, entry skipped
    UnsafePath, // error, entry refused
    NameCollision, // error, entries refused
    IncompleteGroup, // error, group refused
    ExistsAtDestination, // needs a decision when overwrite = Ask
};

struct ExtractIssue
{
    ExtractIssueKind kind = ExtractIssueKind::SymlinkSkipped;
    std::size_t entry = 0;
    std::string name;
    std::string detail;

    [[nodiscard]] bool is_error() const
    {
        return kind == ExtractIssueKind::UnsafePath || kind == ExtractIssueKind::NameCollision || kind == ExtractIssueKind::IncompleteGroup;
    }
};

enum class ItemDecision : std::uint8_t {
    Write,
    Skip,
    Replace,
    Undecided,
};

struct ExtractItem
{
    std::size_t entry = 0;
    std::string target;
    ItemKind kind = ItemKind::File;
    ItemDecision decision = ItemDecision::Write;
    // Restored from FLAC: the member entries in channel order (one for a single FLAC file).
    std::vector<std::size_t> members;
};

struct ExtractionPlan
{
    std::vector<ExtractItem> items;
    std::vector<ExtractIssue> issues;

    [[nodiscard]] bool needs_decisions() const;
    // Decision for an item that exists at the destination (Skip or Replace).
    VoidResult decide(std::size_t item, ItemDecision decision);
    [[nodiscard]] std::size_t error_count() const;
};

struct EntryOutcome
{
    std::size_t entry = 0;
    std::string target;
    Status status = Status::Ok;
    std::string message;
};

struct ExtractResult
{
    std::vector<EntryOutcome> outcomes;
    std::size_t files_written = 0;
    std::size_t directories_created = 0;
    std::size_t skipped = 0;
    Status status = Status::Ok;
    std::string message;
};

class ArchiveExtractor
{
public:
    ArchiveExtractor(ArchiveReader& reader, OutputSink& sink, Context& context, ExtractOptions options = {});

    // Works out targets, warnings and blocking errors before anything is written. An empty
    // selection means every entry.
    Result<ExtractionPlan> plan_extraction(const std::vector<std::size_t>& selection) const;
    // Writes every item that is not refused or skipped. Refuses a plan with undecided items.
    ExtractResult execute(const ExtractionPlan& plan, ProgressSink& progress);

private:
    ArchiveReader& reader_;
    OutputSink& sink_;
    Context& context_;
    ExtractOptions options_;
};

}
