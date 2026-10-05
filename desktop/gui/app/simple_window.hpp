// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#pragma once

#include "jobs.hpp"

#include <QElapsedTimer>
#include <QSet>
#include <QStringList>
#include <QWidget>

class App;
class QLabel;
class QProgressBar;
class QPushButton;
class QStackedWidget;

// Drop and go: zips are extracted, everything else is compressed into one archive.
class SimpleWindow : public QWidget
{
    Q_OBJECT

public:
    explicit SimpleWindow(App& app);
    void handle(const QStringList& paths, satchel_gui::Intent intent = satchel_gui::Intent::Auto);

protected:
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dropEvent(QDropEvent* e) override;
    void closeEvent(QCloseEvent* e) override;

private:
    void start_compress(const QStringList& paths);
    void start_extract(const QString& archive);
    void on_started(int id, const QString& title);
    void on_progress(int id, quint64 done, quint64 total);
    void on_finished(int id, const QString& title, const JobOutcome& outcome);

    App& app_;
    QStackedWidget* pages_ = nullptr;
    QLabel* hint_ = nullptr;
    QLabel* running_ = nullptr;
    QLabel* rate_ = nullptr;
    QProgressBar* bar_ = nullptr;
    QLabel* result_ = nullptr;
    QLabel* details_ = nullptr;
    QPushButton* reveal_ = nullptr;
    QPushButton* retry_ = nullptr;
    QList<int> mine_;
    QSet<int> quit_when_clean_;
    int current_ = 0;
    QElapsedTimer timer_;
    QString last_output_;
    QStringList last_input_;
};

void reveal_in_file_manager(const QString& path);
