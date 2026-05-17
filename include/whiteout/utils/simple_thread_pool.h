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
///
/// @bind methods, extends=whiteout::interfaces::WorkerPool
class SimpleThreadPool : public interfaces::WorkerPool {
public:
    /// Create a pool with @p nThreads worker threads.
    ///
    /// @param nThreads Number of worker threads to spawn.
    explicit SimpleThreadPool(size_t nThreads);

    /// Shut down workers and release resources.
    ~SimpleThreadPool() override;

    /// @bind skip — std::function in WorkerTask isn't bindable
    void submit(const interfaces::WorkerTask& task) override;

    /// @bind — Block until the pool has no pending or running tasks.
    void waitIdle() override;

    /// @bind — Number of worker threads in this pool.
    size_t threadCount() const noexcept override;

    /// @bind skip — unique_ptr return not bindable
    std::unique_ptr<interfaces::TimelineSemaphore> createTimelineSemaphore() override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace whiteout::utils
