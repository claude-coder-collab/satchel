// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#pragma once

#include "io/zip/plan.hpp"

#include <cstddef>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace zp
{

// The default readme template (main spec 6.5). Placeholders: {APP_NAME}, {APP_VERSION},
// {DEARCHIVER_URL}, {FILE_LIST}. Lines end with "\n" here; rendering writes CRLF.
std::string_view default_readme_template();

// Renders the readme for the FLAC entries of `plan`, leaving out entries in `excluded`.
std::string render_readme(const ArchivePlan& plan, const std::set<std::size_t>& excluded, std::string_view template_text = {});

}
