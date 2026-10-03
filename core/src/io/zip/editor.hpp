// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#pragma once

#include "common/status.hpp"
#include "io/input_source.hpp"
#include "io/zip/builder.hpp"
#include "io/zip/plan.hpp"
#include "io/zip/planner.hpp"
#include "io/zip/reader.hpp"
#include "pipeline/context.hpp"

#include <memory>
#include <string>
#include <vector>

namespace zp
{

// Edits an archive by rewriting it: kept entries are copied raw (bytes and CRC unchanged),
// additions go through the planner. Indices refer to plan().entries.
class ArchiveEditor
{
public:
    explicit ArchiveEditor(Context& context, PlannerOptions options = {});

    VoidResult open(IChunkedStream& input);
    VoidResult add(InputSource& input);
    VoidResult remove(std::size_t index);
    VoidResult rename(std::size_t index, const std::string& new_name);
    VoidResult replace(std::size_t index, InputItem item);
    [[nodiscard]] ArchivePlan plan() const;
    // Writes the edited archive to `output` (native in-place edits pass an AtomicFileStream for
    // the original path). Refuses while the merged plan has conflicts.
    BuildResult commit(IChunkedStream& output, ProgressSink& progress);

    [[nodiscard]] ArchiveReader* reader() { return reader_.get(); }

private:
    Context& context_;
    ArchivePlanner planner_;
    PlannerOptions options_;
    std::unique_ptr<ArchiveReader> reader_;
    std::vector<PlanEntry> entries_;
    std::vector<Warning> warnings_;
};

}
