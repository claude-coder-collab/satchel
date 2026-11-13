// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Satchel contributors
#pragma once

#include "jobs.hpp"
#include "rate_label.hpp"

#include <QHash>
#include <QSet>
#include <QStringList>
#include <QWidget>

class Workspace;
class QLabel;
class QProgressBar;
class QPushButton;
class QStackedWidget;

// Drop and go: zips are extracted, everything else is compressed into one archive.
class SimpleWindow : public QWidget
{
    Q_OBJECT

public:
    explicit SimpleWindow(Workspace& workspace);
    void handle(const QStringList& paths, satchel_gui::Intent intent = satchel_gui::Intent::Auto);
    // No job of this window is running or queued.
    [[nodiscard]] bool idle() const { return mine_.isEmpty(); }
    // On the drop page with no job: closing the window loses nothing.
    [[nodiscard]] bool pristine() const;

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

    Workspace& ws_;
    QStackedWidget* pages_ = nullptr;
    QLabel* hint_ = nullptr;
    QLabel* running_ = nullptr;
    RateLabel* rate_ = nullptr;
    QProgressBar* bar_ = nullptr;
    QLabel* result_ = nullptr;
    QLabel* details_ = nullptr;
    QPushButton* reveal_ = nullptr;
    QPushButton* retry_ = nullptr;
    QPushButton* copy_ = nullptr;
    QPushButton* view_contents_ = nullptr;
    QList<int> mine_;
    QHash<int, QString> cancelled_text_;
    QSet<int> quit_when_clean_;
    int current_ = 0;
    QString last_output_;
    QStringList last_input_;
    // The result line, then every warning and problem of the last job (the page shows the first few).
    QStringList last_report_;
};

void reveal_in_file_manager(const QString& path);
