// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#pragma once

#include "logic.hpp"
#include "zp_cpp.hpp"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QVector>

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>

// One unit of background work: compress, extract, verify or edit. Runs on the queue's thread.
struct JobOutcome
{
    int status = ZP_OK;
    QString message;
    QString summary; // one line for the result card / jobs panel
    QString output; // archive or folder written
    QStringList details; // warnings and per-entry problems
    std::uint64_t input_bytes = 0;
    std::uint64_t output_bytes = 0;
    QVector<QPair<int, QString>> entry_status; // verify: entry index -> OK / error name
};

struct JobContext
{
    std::function<bool(std::uint64_t, std::uint64_t)> progress; // false = cancelled
    const std::atomic<bool>* cancelled = nullptr;
};

struct Job
{
    int id = 0;
    QString title;
    std::function<JobOutcome(JobContext&)> run;
};

class JobQueue : public QObject
{
    Q_OBJECT

public:
    explicit JobQueue(QObject* parent = nullptr);
    ~JobQueue() override;

    int enqueue(QString title, std::function<JobOutcome(JobContext&)> run);
    void cancel(int id);
    [[nodiscard]] bool busy() const;

signals:
    void started(int id, const QString& title);
    void progress(int id, quint64 done, quint64 total);
    void finished(int id, const QString& title, const JobOutcome& outcome);

private:
    void loop();

    QThread* thread_ = nullptr;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Job> queue_;
    int next_id_ = 1;
    int running_ = 0;
    std::atomic<bool> cancel_running_{ false };
    bool stop_ = false;
};

Q_DECLARE_METATYPE(JobOutcome)

// Shared library context for the whole app.
zp_context_t* app_context();

// Job bodies.
JobOutcome compress_job(JobContext& jc, zp_plan_t* plan, const QString& output, const satchel_gui::Settings& settings);
JobOutcome extract_job(JobContext& jc, const QString& archive, const QString& destination, const satchel_gui::Settings& settings, const std::vector<std::size_t>& selection, bool replace_existing);
JobOutcome verify_job(JobContext& jc, const QString& archive);
