// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Satchel contributors
#pragma once

#include "jobs.hpp"

#include <QKeySequence>
#include <QObject>
#include <QString>

class App;
class MainWindow;
class QWidget;
class SimpleWindow;

// One window of the app, Simple or Full for its whole life, over a job queue of its own.
// Nothing here is shared with other workspaces.
class Workspace : public QObject
{
    Q_OBJECT

public:
    Workspace(App& app, bool full);
    ~Workspace() override;
    Workspace(const Workspace&) = delete;
    Workspace& operator=(const Workspace&) = delete;
    Workspace(Workspace&&) = delete;
    Workspace& operator=(Workspace&&) = delete;

    App& app() { return app_; }
    JobQueue& jobs() { return jobs_; }
    [[nodiscard]] bool is_full() const { return full_ != nullptr; }
    // The window of the other mode is null.
    [[nodiscard]] MainWindow* full() const { return full_; }
    [[nodiscard]] SimpleWindow* simple() const { return simple_; }
    [[nodiscard]] QWidget* window() const;

    // Shows, raises and activates the window and makes this the active workspace.
    void show();
    [[nodiscard]] bool visible() const;
    // Running or queued jobs, or a session still planning or writing.
    [[nodiscard]] bool busy() const;
    // A Full window holding changes that have not been written.
    [[nodiscard]] bool dirty() const;
    // Nothing has been done in the window that closing it would lose: a Simple window on its
    // drop page with no job, a Full window with no archive, session or job.
    [[nodiscard]] bool pristine() const;

private:
    App& app_;
    JobQueue jobs_;
    SimpleWindow* simple_ = nullptr;
    MainWindow* full_ = nullptr;
};

enum class JobsOnClose {
    Cancel,
    Keep,
    StayOpen,
};
// Asks what a closing window does with its running and queued jobs.
[[nodiscard]] JobsOnClose ask_jobs_on_close(QWidget* parent);

// File ▸ New … Window for the mode of the window the menu is in.
[[nodiscard]] QKeySequence new_window_shortcut();
