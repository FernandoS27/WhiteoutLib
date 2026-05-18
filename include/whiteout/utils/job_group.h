// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include <cstddef>
#include <memory>

#include "../interfaces.h"

namespace whiteout::utils {

/// Thread-safe counter-based completion primitive for grouped jobs.
///
/// A JobGroup starts with zero pending jobs. Call add() before submitting work,
/// then call done() once per completed job. wait() blocks until the pending
/// count reaches zero.
///
/// Typical usage pattern:
/// 1. add(N)
/// 2. submit N tasks
/// 3. each task calls done() on completion
/// 4. wait() to join the group
/// @bind methods, java_package=whiteout.utils
class JobGroup {
public:
    /// Construct an empty job group (pending count = 0).
    JobGroup();

    /// Destroy the job group.
    ~JobGroup();

    JobGroup(const JobGroup&) = delete;
    JobGroup& operator=(const JobGroup&) = delete;
    JobGroup(JobGroup&&) = delete;
    JobGroup& operator=(JobGroup&&) = delete;

    /// Increment the number of pending jobs.
    ///
    /// @param n Number of jobs to add to the group.
    void add(size_t n = 1);

    /// Mark one pending job as completed.
    ///
    /// When the pending count reaches zero, all waiters are notified.
    ///
    /// @note done() calls must be balanced with prior add() calls.
    void done();

    /// @bind rename=await — Java's Object.wait() is final so the
    /// generated wrapper would fail to compile; expose as await().
    /// Block until all pending jobs in the group are completed.
    void wait();

    /// Check whether the group has no pending jobs.
    ///
    /// @return true if pending count is zero, false otherwise.
    bool isReady() const;

    /// Set a timeline semaphore to be signaled when the pending count reaches zero.
    ///
    /// @param sem Semaphore to signal on completion.
    /// @param value Value to signal on the semaphore.
    /// @bind skip — needs TimelineSemaphore dispatch helper that's not
    /// emitted yet (TimelineSemaphore is hand-written, not auto-bound
    /// as a subclassable interface).
    void signalOnComplete(interfaces::TimelineSemaphore* sem,
                          interfaces::TimelineSemaphore::Value value);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace whiteout::utils
