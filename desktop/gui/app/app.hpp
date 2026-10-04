// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#pragma once

#include "jobs.hpp"
#include "logic.hpp"

#include <QObject>
#include <QStringList>

class MainWindow;
class SimpleWindow;

// Owns the job queue, settings and both windows; both modes share one queue.
class App : public QObject
{
    Q_OBJECT

public:
    explicit App(QObject* parent = nullptr);
    ~App() override;

    JobQueue& jobs() { return jobs_; }
    satchel_gui::Settings& settings() { return settings_; }
    void save();

    void show_simple();
    void show_full(const QString& archive = {});
    void toggle_mode();
    // Files dropped on the app icon or passed on the command line: Simple-mode rule.
    void open_paths(const QStringList& paths);
    // Desktop notification when a job ends while no window is focused.
    void notify(const QString& title, const QString& text);

    MainWindow* full() { return full_; }
    SimpleWindow* simple() { return simple_; }

private:
    JobQueue jobs_;
    satchel_gui::Settings settings_;
    SimpleWindow* simple_ = nullptr;
    MainWindow* full_ = nullptr;
};
