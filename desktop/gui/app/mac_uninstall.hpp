// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#pragma once

class QWidget;
class QMenu;

// "Uninstall Satchel…": removes the app bundle, its Quick Look extension, Finder services and
// settings (the same code as `satchel uninstall`), then quits. macOS only.
[[nodiscard]] bool uninstall_available();

void run_uninstall_dialog(QWidget* parent);

// Adds "Uninstall Satchel…" to `menu` (in the application menu on macOS) when available.
void add_uninstall_action(QMenu* menu, QWidget* parent);
