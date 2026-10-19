// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#pragma once

#include "jobs.hpp"
#include "logic.hpp"

#include <QObject>
#include <QStringList>

#include <memory>

class MainWindow;
class SimpleWindow;
class Updater;

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
    // Starts Sparkle/WinSparkle (once, from main; tests never start it).
    void start_updates();
    Updater* updater() { return updater_.get(); }

    // keep_full: alongside a visible Full window (files from the file manager while planning).
    void show_simple(bool keep_full = false);
    void show_full(const QString& archive = {});
    void toggle_mode();
    // Files dropped on the app icon or passed on the command line: Simple-mode rule. A visible
    // Full window and its session stay as they are.
    void open_paths(const QStringList& paths, satchel_gui::Intent intent = satchel_gui::Intent::Auto);
    // Desktop notification when a job ends while no window is focused.
    void notify(const QString& title, const QString& text);

    MainWindow* full() { return full_; }
    SimpleWindow* simple() { return simple_; }

private:
    JobQueue jobs_;
    satchel_gui::Settings settings_;
    SimpleWindow* simple_ = nullptr;
    MainWindow* full_ = nullptr;
    std::unique_ptr<Updater> updater_;
};
