// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "whiteout/utils/simple_thread_pool.h"
#include "whiteout/utils/timeline_semaphore.h"

#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>
#include <queue>

namespace whiteout::utils {

struct SimpleThreadPool::Impl {
    std::vector<std::thread> workers;
    std::queue<interfaces::WorkerTask> jobs;
    std::mutex mutex;
    std::condition_variable cv;
    std::condition_variable doneCv;
    int pendingCount = 0;
    bool stop = false;

    explicit Impl(size_t nThreads) {
        workers.reserve(nThreads);
        for (size_t i = 0; i < nThreads; ++i) {
            workers.emplace_back([this] {
                for (;;) {
                    interfaces::WorkerTask task;
                    {
                        std::unique_lock<std::mutex> lock(mutex);
                        cv.wait(lock, [this] { return stop || !jobs.empty(); });
                        if (stop && jobs.empty())
                            return;
                        task = std::move(jobs.front());
                        jobs.pop();
                    }

                    if (task.waitSemaphore && task.waitSemaphore->value() < task.waitValue) {
                        {
                            std::unique_lock<std::mutex> const lock(mutex);
                            jobs.push(std::move(task));
                        }
                        cv.notify_one();
                        std::this_thread::yield();
                        continue;
                    }

                    task.fn();

                    if (task.signalSemaphore)
                        task.signalSemaphore->signal(task.signalValue);

                    {
                        std::unique_lock<std::mutex> const lock(mutex);
                        if (--pendingCount == 0)
                            doneCv.notify_all();
                    }
                    // Wake threads that may now have runnable tasks
                    // (dependencies satisfied by this task's signal).
                    cv.notify_all();
                }
            });
        }
    }

    ~Impl() {
        {
            std::unique_lock<std::mutex> const lock(mutex);
            stop = true;
        }
        cv.notify_all();
        for (auto& w : workers)
            w.join();
    }
};

SimpleThreadPool::SimpleThreadPool(size_t nThreads) : m_impl(std::make_unique<Impl>(nThreads)) {}

SimpleThreadPool::~SimpleThreadPool() = default;

void SimpleThreadPool::submit(const interfaces::WorkerTask& task) {
    {
        std::unique_lock<std::mutex> const lock(m_impl->mutex);
        ++m_impl->pendingCount;
        m_impl->jobs.push(task);
    }
    m_impl->cv.notify_one();
}

void SimpleThreadPool::waitIdle() {
    std::unique_lock<std::mutex> lock(m_impl->mutex);
    m_impl->doneCv.wait(lock, [this] { return m_impl->pendingCount == 0; });
}

size_t SimpleThreadPool::threadCount() const noexcept {
    return m_impl->workers.size();
}

std::unique_ptr<interfaces::TimelineSemaphore> SimpleThreadPool::createTimelineSemaphore() {
    return std::make_unique<TimelineSemaphore>();
}

} // namespace whiteout::utils
