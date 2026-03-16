// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "whiteout/utils/simple_thread_pool.h"

#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>

namespace whiteout::utils {

struct SimpleThreadPool::Impl
{
    std::vector<std::thread> workers;
    std::queue<interfaces::WorkerTask> jobs;
    std::mutex mutex;
    std::condition_variable cv;
    std::condition_variable doneCv;
    int pendingCount = 0;
    bool stop = false;

    explicit Impl(size_t nThreads)
    {
        workers.reserve(nThreads);
        for (size_t i = 0; i < nThreads; ++i)
        {
            workers.emplace_back([this]
            {
                for (;;)
                {
                    interfaces::WorkerTask task;
                    {
                        std::unique_lock<std::mutex> lock(mutex);
                        cv.wait(lock, [this] { return stop || !jobs.empty(); });
                        if (stop && jobs.empty())
                            return;
                        task = std::move(jobs.front());
                        jobs.pop();
                    }

                    if (task.waitSemaphore)
                        task.waitSemaphore->wait(task.waitValue);

                    task.fn();

                    if (task.signalSemaphore)
                        task.signalSemaphore->signal(task.signalValue);

                    {
                        std::unique_lock<std::mutex> lock(mutex);
                        if (--pendingCount == 0)
                            doneCv.notify_all();
                    }
                }
            });
        }
    }

    ~Impl()
    {
        {
            std::unique_lock<std::mutex> lock(mutex);
            stop = true;
        }
        cv.notify_all();
        for (auto& w : workers)
            w.join();
    }
};

SimpleThreadPool::SimpleThreadPool(size_t nThreads)
    : m_impl(std::make_unique<Impl>(nThreads))
{}

SimpleThreadPool::~SimpleThreadPool() = default;

void SimpleThreadPool::submit(const interfaces::WorkerTask& task)
{
    {
        std::unique_lock<std::mutex> lock(m_impl->mutex);
        ++m_impl->pendingCount;
        m_impl->jobs.push(task);
    }
    m_impl->cv.notify_one();
}

void SimpleThreadPool::wait_idle()
{
    std::unique_lock<std::mutex> lock(m_impl->mutex);
    m_impl->doneCv.wait(lock, [this] { return m_impl->pendingCount == 0; });
}

size_t SimpleThreadPool::thread_count() const noexcept
{
    return m_impl->workers.size();
}

} // namespace whiteout::utils
