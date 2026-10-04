// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#pragma once

#include "logic.hpp"

#include <QString>

satchel_gui::Settings load_settings();
void save_settings(const satchel_gui::Settings& s);

// "simple" or "full"
QString last_mode();
void set_last_mode(const QString& mode);
