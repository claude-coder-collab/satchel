// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#pragma once

#include "common/status.hpp"
#include "io/input_source.hpp"
#include "io/zip/plan.hpp"

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

    Result<ArchivePlan> plan(InputSource& input) const;
    // Applies user decisions (indices refer to `plan`) and re-checks. Rejects a rename to an
    // invalid name or one that still collides.
    static Result<ArchivePlan> replan(const ArchivePlan& plan, const std::vector<Resolution>& resolutions);
    // Recomputes conflicts from scratch.
    static void check(ArchivePlan& plan);

    // Turns enumerated items into plan entries (shared with the editor).
    Result<std::vector<PlanEntry>> make_entries(std::vector<InputItem> items, std::vector<Warning>& warnings) const;

private:
    PlannerOptions options_;
};

}
