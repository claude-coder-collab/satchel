// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#pragma once

#include "jobs.hpp"
#include "session.hpp"

#include <QObject>
#include <QString>

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

// What the window shows of a session; immutable once published.
struct SessionSnapshot
{
    satchel_gui::SessionView view;
    satchel_gui::SessionOptions options;
    QString target;
    bool is_new = false;
    bool has_changes = false;
    bool can_undo = false;
    bool can_redo = false;
    // Changes whenever the base archive is (re)loaded: open, create, commit.
    int base_generation = 0;
};

// Runs a Full-mode session on its own thread so planning never blocks the window. Every call
// is queued; the window reads only snapshots. A commit runs on the app's job queue instead, and
// nothing else touches the session until it ends.
class SessionController : public QObject
{
    Q_OBJECT

public:
    explicit SessionController(JobQueue& jobs, QObject* parent = nullptr);
    ~SessionController() override;
    SessionController(const SessionController&) = delete;
    SessionController& operator=(const SessionController&) = delete;
    SessionController(SessionController&&) = delete;
    SessionController& operator=(SessionController&&) = delete;

    void open(const QString& archive, satchel_gui::SessionOptions options);
    // A new archive at `target`, optionally starting with `initial` (added at the top level).
    void create(const QString& target, satchel_gui::SessionOptions options, satchel_gui::Change initial = {});
    void close();

    void apply(satchel_gui::Change change);
    void undo();
    void redo();
    void discard();
    void set_options(satchel_gui::SessionOptions options);
    void set_target(const QString& target);

    // Writes the session to `output` as a job; returns the job id, or 0 if it cannot start now.
    int commit(const QString& output, const QString& title, const satchel_gui::Settings& settings);

    [[nodiscard]] bool active() const { return active_; }
    [[nodiscard]] bool busy() const { return pending_ > 0 || committing_ != 0; }
    [[nodiscard]] bool committing() const { return committing_ != 0; }
    [[nodiscard]] const std::shared_ptr<const SessionSnapshot>& snapshot() const { return snapshot_; }

signals:
    void updated();
    void busy_changed(bool busy);
    void failed(const QString& message);

private:
    using Task = std::function<void(std::unique_ptr<satchel_gui::Session>&)>;
    void post(Task task, bool reload_base = false);
    void worker();
    void publish(std::shared_ptr<const SessionSnapshot> snapshot, const QString& error);

    JobQueue& jobs_;
    std::shared_ptr<std::unique_ptr<satchel_gui::Session>> session_;
    std::shared_ptr<const SessionSnapshot> snapshot_;
    bool active_ = false;
    int pending_ = 0;
    int committing_ = 0;
    int generation_ = 0;

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::pair<Task, bool>> queue_;
    bool stop_ = false;
    std::thread thread_;
};
