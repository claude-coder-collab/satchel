// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Satchel contributors
#pragma once

#include "common/status.hpp"
#include "io/input_source.hpp"
#include "io/zip/plan.hpp"
#include "pipeline/context.hpp"

#include <vector>

namespace zp
{

class ArchivePlanner
{
public:
    explicit ArchivePlanner(PlannerOptions options) :
        options_(options)
    {
    }

    // `progress` is polled with (0, 0) while the input is listed, then with (files read, files);
    // returning false cancels (Status::Cancelled).
    Result<ArchivePlan> plan(InputSource& input, ProgressSink* progress = nullptr) const;
    // Applies user decisions (indices refer to `plan`) and re-checks. Rejects a rename to an
    // invalid name or one that still collides.
    static Result<ArchivePlan> replan(const ArchivePlan& plan, const std::vector<Resolution>& resolutions);
    // Recomputes conflicts from scratch.
    static void check(ArchivePlan& plan);
    // Adds the readme entry when the plan has converted audio, removes it otherwise.
    static void ensure_readme(ArchivePlan& plan);

    // Turns enumerated items into plan entries (shared with the editor).
    Result<std::vector<PlanEntry>> make_entries(std::vector<InputItem> items, std::vector<Warning>& warnings, ProgressSink* progress = nullptr) const;

private:
    PlannerOptions options_;
};

}
