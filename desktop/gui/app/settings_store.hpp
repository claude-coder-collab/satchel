// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Satchel contributors
#pragma once

#include "logic.hpp"

#include <QString>

satchel_gui::Settings load_settings();
void save_settings(const satchel_gui::Settings& s);

// "simple" or "full"
QString last_mode();
void set_last_mode(const QString& mode);
