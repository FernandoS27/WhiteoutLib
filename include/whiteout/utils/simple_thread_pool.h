// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include <cstddef>
#include <functional>
#include <memory>

#include "../interfaces.h"

namespace whiteout::utils {

/// Basic CPU worker pool implementation of interfaces::WorkerPool.
///
/// Tasks are executed by a fixed number of worker threads created at
/// construction time.
class SimpleThreadPool : public interfaces::WorkerPool {
public:
    /// Create a pool with @p nThreads worker threads.
    ///
    /// @param nThreads Number of worker threads to spawn.
    explicit SimpleThreadPool(size_t nThreads);

    /// Shut down workers and release resources.
    ~SimpleThreadPool() override;

    /// Submit a task to be executed by the pool.
    ///
    /// @param task Task descriptor containing function and optional
    ///             semaphore dependencies/signals.
    void submit(const interfaces::WorkerTask& task) override;

    /// Block until the pool has no pending or running tasks.
    void waitIdle() override;

    /// Get the number of worker threads in this pool.
    ///
    /// @return Worker thread count.
    size_t threadCount() const noexcept override;

    /// Create a timeline semaphore backed by utils::TimelineSemaphore.
    std::unique_ptr<interfaces::TimelineSemaphore> createTimelineSemaphore() override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace whiteout::utils
