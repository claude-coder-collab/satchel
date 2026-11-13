// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Satchel contributors
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
    zp_context_t* context = nullptr;
    std::function<bool(std::uint64_t, std::uint64_t)> progress; // false = cancelled
    const std::atomic<bool>* cancelled = nullptr;
};

struct Job
{
    int id = 0;
    QString title;
    std::function<JobOutcome(JobContext&)> run;
};

// A background thread running jobs in order on its own library context, so queues never wait
// for one another.
class JobQueue : public QObject
{
    Q_OBJECT

public:
    // threads: worker threads of the context (0 = all cores).
    explicit JobQueue(int threads = 0, QObject* parent = nullptr);
    ~JobQueue() override;
    JobQueue(const JobQueue&) = delete;
    JobQueue& operator=(const JobQueue&) = delete;
    JobQueue(JobQueue&&) = delete;
    JobQueue& operator=(JobQueue&&) = delete;

    [[nodiscard]] zp_context_t* context() const { return context_.get(); }

    int enqueue(QString title, std::function<JobOutcome(JobContext&)> run);
    // A queued job is dropped and reported as finished with ZP_CANCELLED; a running one is asked to stop.
    void cancel(int id);
    [[nodiscard]] bool busy() const;

signals:
    void started(int id, const QString& title);
    void progress(int id, quint64 done, quint64 total);
    void finished(int id, const QString& title, const JobOutcome& outcome);

private:
    void loop();

    zpp::Context context_;
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

// The exclude patterns the settings ask for: enabled presets, then the custom patterns.
std::vector<std::string> exclude_patterns(const satchel_gui::Settings& settings);
// Empty if every pattern is valid, else the library's message for the first invalid one.
QString exclude_pattern_error(const std::vector<std::string>& patterns);

// Plans `inputs` with the settings' compression and Leave out options (Simple mode).
std::shared_ptr<zpp::Plan> make_plan(zp_context_t* context, const QStringList& inputs, const satchel_gui::Settings& settings);

// Job bodies; they run on `jc.context`.
JobOutcome compress_job(JobContext& jc, zp_plan_t* plan, const QString& output, const satchel_gui::Settings& settings);
// base_folder: the selection is extracted relative to this archive folder ("" = the root).
JobOutcome extract_job(JobContext& jc, const QString& archive, const QString& destination, const satchel_gui::Settings& settings, const std::vector<std::size_t>& selection, bool replace_existing, const QString& base_folder = {});
JobOutcome verify_job(JobContext& jc, const QString& archive);
