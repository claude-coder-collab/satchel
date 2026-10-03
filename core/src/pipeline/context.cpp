// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "pipeline/context.hpp"

#include <algorithm>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

namespace zp
{

ThreadPool::ThreadPool(std::size_t threads)
{
    threads_.reserve(threads);
    for (std::size_t i = 0; i < threads; ++i)
        threads_.emplace_back([this] { run(); });
}

ThreadPool::~ThreadPool()
{
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    for (auto& t : threads_)
        t.join();
}

void ThreadPool::submit(std::function<void()> task)
{
    {
        std::lock_guard lock(mutex_);
        queue_.push_back(std::move(task));
    }
    cv_.notify_one();
}

void ThreadPool::run()
{
    while (true)
    {
        std::function<void()> task;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
            if (queue_.empty())
                return;
            task = std::move(queue_.front());
            queue_.pop_front();
        }
        task();
    }
}

bool ByteBudget::acquire(std::uint64_t n)
{
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [&] { return cancelled_ || used_ == 0 || used_ + n <= limit_; });
    if (cancelled_)
        return false;
    used_ += n;
    peak_ = std::max(peak_, used_);
    return true;
}

void ByteBudget::release(std::uint64_t n)
{
    {
        std::lock_guard lock(mutex_);
        used_ -= std::min(n, used_);
    }
    cv_.notify_all();
}

void ByteBudget::cancel()
{
    {
        std::lock_guard lock(mutex_);
        cancelled_ = true;
    }
    cv_.notify_all();
}

std::uint64_t ByteBudget::peak() const
{
    std::lock_guard lock(mutex_);
    return peak_;
}

void TaskCounter::add()
{
    std::lock_guard lock(mutex_);
    ++pending_;
}

void TaskCounter::done()
{
    std::lock_guard lock(mutex_);
    --pending_;
    cv_.notify_all();
}

void TaskCounter::wait()
{
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [this] { return pending_ == 0; });
}

bool TaskCounter::wait_for(std::chrono::milliseconds timeout)
{
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, timeout, [this] { return pending_ == 0; });
}

std::size_t Context::resolve_threads(int threads)
{
    if (threads > 0)
        return static_cast<std::size_t>(threads);
    return std::max<std::size_t>(1, std::thread::hardware_concurrency());
}

Context::Context(int threads, std::uint64_t memory_budget) :
    memory_budget_(memory_budget == 0 ? default_memory_budget : memory_budget),
    workers_(resolve_threads(threads)),
    services_(2)
{
}

}
