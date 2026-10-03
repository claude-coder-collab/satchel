// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace zp
{

class ThreadPool
{
public:
    explicit ThreadPool(std::size_t threads);
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;
    ~ThreadPool();

    void submit(std::function<void()> task);
    [[nodiscard]] std::size_t size() const { return threads_.size(); }

private:
    void run();

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> queue_;
    std::vector<std::thread> threads_;
    bool stop_ = false;
};

// Bounds the bytes held by in-flight segments. A request larger than the whole budget is
// granted once nothing else is held, so oversized segments still make progress.
class ByteBudget
{
public:
    explicit ByteBudget(std::uint64_t limit) :
        limit_(limit)
    {
    }

    // Returns false when cancelled.
    bool acquire(std::uint64_t n);
    void release(std::uint64_t n);
    void cancel();
    [[nodiscard]] std::uint64_t peak() const;

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::uint64_t limit_;
    std::uint64_t used_ = 0;
    std::uint64_t peak_ = 0;
    bool cancelled_ = false;
};

// Counts outstanding tasks so a job can wait for all of them before returning.
class TaskCounter
{
public:
    void add();
    void done();
    void wait();
    // Returns true when no tasks are pending.
    bool wait_for(std::chrono::milliseconds timeout);

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::size_t pending_ = 0;
};

class Context
{
public:
    static constexpr std::uint64_t default_memory_budget = 256ull << 20;

    // threads = 0: hardware concurrency; memory_budget = 0: default.
    Context(int threads, std::uint64_t memory_budget);

    ThreadPool& workers() { return workers_; }
    // Reader and hasher threads.
    ThreadPool& services() { return services_; }
    [[nodiscard]] std::uint64_t memory_budget() const { return memory_budget_; }
    [[nodiscard]] std::size_t thread_count() const { return workers_.size(); }
    // Jobs in one context run one at a time.
    std::mutex& job_mutex() { return job_mutex_; }

    static std::size_t resolve_threads(int threads);

private:
    std::uint64_t memory_budget_;
    ThreadPool workers_;
    ThreadPool services_;
    std::mutex job_mutex_;
};

struct ProgressSink
{
    ProgressSink() = default;
    ProgressSink(const ProgressSink&) = delete;
    ProgressSink& operator=(const ProgressSink&) = delete;
    ProgressSink(ProgressSink&&) = delete;
    ProgressSink& operator=(ProgressSink&&) = delete;
    virtual ~ProgressSink() = default;

    // Return false to cancel. Called from the job's writer thread.
    virtual bool on_progress(std::uint64_t done, std::uint64_t total)
    {
        (void) done;
        (void) total;
        return true;
    }
};

}
