// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
/// @file progress_reporter.h
/// @brief Serialising, throttling front-end for ProgressCallback.
///
/// Internal header — used by the open paths and the deferred loader.
#pragma once

#include "../progress_sink.h"

#include <whiteout/storages/casc/types.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <vector>

namespace whiteout::storages::casc {

/**
 * @brief Turns scattered load events into an ordered progress stream.
 *
 * Construction takes the *plan* — the steps this operation expects to run — so
 * every event carries an honest `stepIndex`/`stepCount` and a UI can draw one
 * bar for the whole open. Steps that turn out not to run are simply never
 * begun.
 *
 * Delivery rules, in priority order:
 *  - A reporter with no callback is inert: `enabled()` is false and `sink()`
 *    hands back nullptr, so instrumented loops fall back to a null check.
 *  - Begin/End/Ready are never dropped.
 *  - Update is throttled to ~30 Hz and delivered on a try-lock, so a worker
 *    thread reports only if it can do so without waiting on another thread.
 *  - The callback is never entered concurrently.
 */
class ProgressReporter {
public:
    /// Passed as `current`/`total` to reuse whatever the step last reported.
    static constexpr u64 kInheritCount = ~u64(0);

    ProgressReporter() = default;
    ProgressReporter(ProgressCallback callback, std::vector<ProgressStep> plan);

    ProgressReporter(const ProgressReporter&) = delete;
    ProgressReporter& operator=(const ProgressReporter&) = delete;

    /// Begin a new operation on the same callback (the deferred load, a
    /// prefetch): fresh plan, fresh elapsed clock, cancellation cleared.
    void restart(std::vector<ProgressStep> plan);

    /// Drop a planned step that turned out not to apply, so the percentage
    /// stops reserving room for it. Ignored once the step has begun.
    void dropStep(ProgressStep step);

    bool enabled() const noexcept {
        return static_cast<bool>(m_callback);
    }

    bool cancelled() const noexcept {
        return m_cancelled.load(std::memory_order_relaxed);
    }

    /// Open a step. @p total is 0 when the item count isn't known yet.
    /// @return false if cancellation has been requested.
    bool begin(ProgressStep step, std::string_view object = {}, u64 total = 0);

    /// Throttled item counter update for the open step.
    bool update(u64 current, u64 total, std::string_view object = {});

    /// Throttled byte counter update for the open step.
    bool updateBytes(u64 bytesDone, u64 bytesTotal, std::string_view object = {});

    /// Close the open step. Defaults to the last tally its sink reported.
    bool end(u64 current = kInheritCount, std::string_view object = {});

    /// Terminal event. Reports the operation as complete.
    void ready();

    /// Sink for the open step, or nullptr when disabled. Valid until the next
    /// begin() call on this reporter.
    const ProgressSink* sink();

private:
    using Clock = std::chrono::steady_clock;

    bool emit(ProgressState state, u64 current, u64 total, u64 bytesDone, u64 bytesTotal,
              std::string_view object, bool throttled);

    ProgressCallback m_callback;
    std::vector<ProgressStep> m_plan;
    ProgressSink m_sink;

    std::mutex m_mutex;
    std::atomic<bool> m_cancelled{false};

    ProgressStep m_step = ProgressStep::Ready;
    u32 m_stepIndex = 0;
    u64 m_stepTotal = 0;
    bool m_stepOpen = false;

    /// Last counters seen by emit(). Written and read under m_mutex.
    u64 m_lastCurrent = 0;
    u64 m_lastTotal = 0;

    Clock::time_point m_start = Clock::now();
    Clock::time_point m_lastEmit{};
};

} // namespace whiteout::storages::casc
