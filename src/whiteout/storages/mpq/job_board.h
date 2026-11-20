// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file job_board.h
/// @brief Numbered jobs shared between the caller's thread and a worker pool.
///
/// Internal header — not part of the public include path.

#pragma once

#include <whiteout/common_types.h>
#include <whiteout/interfaces.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>

namespace whiteout::storages::mpq {

namespace detail {

/// The jobs still to take, and how many are finished. Held by shared_ptr: a
/// helper the pool gets to only after the caller has returned finds nothing
/// left to take and touches nothing else.
struct JobBoard {
    std::atomic<size_t> next{0};
    std::atomic<size_t> finished{0};
    size_t count = 0;
    std::function<void(size_t)> job; ///< Called only for an index below count.
    std::mutex mutex;
    std::condition_variable allFinished;

    void drain() {
        for (;;) {
            size_t const index = next.fetch_add(1, std::memory_order_relaxed);
            if (index >= count)
                return;
            job(index);
            if (finished.fetch_add(1, std::memory_order_acq_rel) + 1 == count) {
                std::lock_guard const lock(mutex);
                allFinished.notify_all();
            }
        }
    }
};

} // namespace detail

/// Runs @p job(index) for every index below @p count and returns when all
/// have finished. The jobs are taken by the caller's thread and by up to as
/// many pool threads as there are jobs to share; because the caller takes
/// jobs too, the work finishes even when every pool thread is busy, or when
/// the caller is one of them. Without a pool the caller does them all.
///
/// A job touches only its own data: two run at once.
template <class Job>
void runJobs(interfaces::WorkerPool* pool, size_t count, Job&& job) {
    size_t const threads = pool ? pool->threadCount() : 0;
    if (threads == 0 || count < 2) {
        for (size_t index = 0; index < count; ++index)
            job(index);
        return;
    }

    auto board = std::make_shared<detail::JobBoard>();
    board->count = count;
    board->job = [&job](size_t index) { job(index); };
    size_t const helpers = std::min(threads, count - 1);
    for (size_t helper = 0; helper < helpers; ++helper) {
        interfaces::WorkerTask task;
        task.fn = [board]() { board->drain(); };
        pool->submit(task);
    }
    board->drain();

    std::unique_lock lock(board->mutex);
    board->allFinished.wait(lock, [&] { return board->finished.load(std::memory_order_acquire) == count; });
}

} // namespace whiteout::storages::mpq
