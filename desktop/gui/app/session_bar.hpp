// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#pragma once

#include "session_controller.hpp"

#include <QFrame>

#include <memory>

class QLabel;
class QProgressBar;
class QPushButton;

// Full mode's action bar below the browser: what the staged changes add up to, the Issues and
// Options popovers, Discard and the Go button, and the result of the last write.
class SessionBar : public QFrame
{
    Q_OBJECT

public:
    explicit SessionBar(QWidget* parent = nullptr);

    void show_snapshot(std::shared_ptr<const SessionSnapshot> snapshot, bool busy, bool committing);
    void show_result(const QString& text, const QString& output);
    void set_progress(std::uint64_t done, std::uint64_t total);
    void clear_result();
    // Session options as Settings (compression and Leave out) plus Existing entries.
    void set_options(const satchel_gui::Settings& settings, satchel_gui::ExistingEntries existing);
    [[nodiscard]] QPushButton* go_button() const { return go_; }

signals:
    void go();
    void discard();
    // A new archive: leave it without creating it.
    void cancel_new();
    void cancel_write();
    void resolve(const satchel_gui::op::Resolve& resolution);
    void options_changed(const satchel_gui::Settings& settings, satchel_gui::ExistingEntries existing);
    void reveal(const QString& path);

private:
    void open_issues();
    void open_options();
    void update_visibility();

    std::shared_ptr<const SessionSnapshot> snapshot_;
    satchel_gui::Settings settings_;
    satchel_gui::ExistingEntries existing_ = satchel_gui::ExistingEntries::Ask;
    QLabel* summary_ = nullptr;
    QPushButton* issues_ = nullptr;
    QPushButton* options_ = nullptr;
    QPushButton* discard_ = nullptr;
    QPushButton* go_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QPushButton* cancel_write_ = nullptr;
    bool committing_ = false;
    QLabel* result_ = nullptr;
    QPushButton* reveal_ = nullptr;
    QString result_output_;
};
