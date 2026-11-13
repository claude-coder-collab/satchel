// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Satchel contributors
#pragma once

#include "logic.hpp"
#include "workspace.hpp"

#include <QList>
#include <QObject>
#include <QStringList>

#include <memory>

class Updater;

// Owns the settings and the workspaces (windows). The app runs until every workspace is closed
// and idle: a window closed with its job kept running finishes it first.
class App : public QObject
{
    Q_OBJECT

public:
    explicit App(QObject* parent = nullptr);
    ~App() override;

    satchel_gui::Settings& settings() { return settings_; }
    void save();
    // Starts Sparkle/WinSparkle (once, from main; tests never start it).
    void start_updates();
    Updater* updater() { return updater_.get(); }

    // The workspace used last; null while there is none.
    [[nodiscard]] Workspace* active() const { return active_; }
    void set_active(Workspace& workspace);
    [[nodiscard]] const QList<Workspace*>& workspaces() const { return workspaces_; }
    // A new Simple or Full window, shown offset from the active one.
    Workspace& new_window(bool full);
    // Switch mode: a new window of the other mode. A pristine `from` is closed and the new window
    // takes its place; otherwise it stays, since its state has no place in the other mode.
    Workspace& switch_mode(Workspace& from);
    // Shows `archive` in Full mode: the window already showing it, else the active window if it
    // is a pristine Full one, else a new Full window.
    MainWindow& show_archive(const QString& archive);
    // The visible Full window showing `archive`, if any.
    [[nodiscard]] MainWindow* window_showing(const QString& archive) const;

    // Files dropped on the app icon or passed on the command line. One zip with no explicit
    // intent while a Full window is active: show_archive. Otherwise the Simple-mode rule, in a
    // Simple window with no job of its own (the active one first), else in a new one.
    void open_paths(const QStringList& paths, satchel_gui::Intent intent = satchel_gui::Intent::Auto);
    // Desktop notification when a job ends while its window is not focused.
    void notify(const QString& title, const QString& text);
    // Drops workspaces that are closed and idle (showing one again if it holds unwritten changes)
    // and quits once none is left. Queued by windows when they close and by every finished job.
    void settle();
    void settle_later();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    Workspace& add_workspace(bool full);
    // Closes every window for a quit; true if the quit must not go on now: a window stayed open,
    // or a job was kept running (the app then quits when it ends).
    bool hold_quit();

    satchel_gui::Settings settings_;
    QList<Workspace*> workspaces_;
    Workspace* active_ = nullptr;
    std::unique_ptr<Updater> updater_;
};
