// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#pragma once

#include <QString>
#include <QStringList>

class QMenu;
class QWidget;

namespace satchel_gui
{

struct CleanupReport
{
    QStringList removed;
    QStringList failed;
};

// Folders `satchel-<ms>` (files extracted for opening or dragging) and `satchel-spill-*.flac`
// (build spill files left by a crash) in `temp_dir`.
QStringList temp_leftovers(const QString& temp_dir);

// Deletes the application's QSettings (the registry key on Windows, the config file elsewhere),
// WinSparkle's registry key on Windows, and with `include_temp` the temp leftovers.
CleanupReport clean_user_data(const QString& temp_dir, bool include_temp);

}

// "Delete Settings and Temporary Files…" for platforms whose uninstaller leaves user data behind
// (everything but macOS, where "Uninstall Satchel…" covers it).
[[nodiscard]] bool user_data_cleanup_available();

// Asks, then quits; the deletion runs in `~PendingCleanup` after the windows and App are gone, so
// nothing rewrites the settings on the way out.
void request_user_data_cleanup(QWidget* parent);

void add_user_data_cleanup_action(QMenu* menu, QWidget* parent);

// Declare before App in main(): reports the result of a requested cleanup when main() returns.
class PendingCleanup
{
public:
    PendingCleanup() = default;
    PendingCleanup(const PendingCleanup&) = delete;
    PendingCleanup& operator=(const PendingCleanup&) = delete;
    PendingCleanup(PendingCleanup&&) = delete;
    PendingCleanup& operator=(PendingCleanup&&) = delete;
    ~PendingCleanup();
};
