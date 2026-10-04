// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "updater.hpp"

std::unique_ptr<Updater> Updater::create(bool)
{
    return std::make_unique<Updater>();
}
