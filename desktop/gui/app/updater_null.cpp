// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Satchel contributors
#include "updater.hpp"

std::unique_ptr<Updater> Updater::create(bool)
{
    return std::make_unique<Updater>();
}
