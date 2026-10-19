// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#pragma once

#include <QStringList>

#include <chrono>
#include <optional>
#include <string>

class QWidget;
class QMenu;

// "Uninstall Satchel…": removes the app bundle, its Quick Look extension, Finder services and
// settings (the same code as `satchel uninstall`). macOS only. The app cannot remove itself
// while it runs (it saves settings as it quits), so it starts a copy of itself as a helper that
// waits for it to exit, then uninstalls.
[[nodiscard]] bool uninstall_available();

void run_uninstall_dialog(QWidget* parent);

// Adds "Uninstall Satchel…" to the application menu (through `menu`) once per app, when
// available; the dialog opens over the active window.
void add_uninstall_action(QMenu* menu, QWidget* parent);

struct UninstallRequest
{
    long long pid = 0; // the app to wait for
    bool keep_settings = false;
    std::string app; // bundle path
};

// Command-line arguments (after the program) that make the executable an uninstall helper.
[[nodiscard]] QStringList uninstall_helper_arguments(const UninstallRequest& request);
// The request when argv asks for the helper (checked before any Qt application exists).
[[nodiscard]] std::optional<UninstallRequest> parse_uninstall_helper(int argc, char** argv);
// Waits up to `wait` for the app to exit, then uninstalls; reports failures in an alert.
// Returns the process exit code.
int run_uninstall_helper(const UninstallRequest& request, std::chrono::milliseconds wait = std::chrono::minutes(2));
