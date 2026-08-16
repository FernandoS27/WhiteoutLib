// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
/// @file progress_reporter.cpp

#include "progress_reporter.h"

#include <algorithm>

namespace whiteout::storages::casc {

namespace {
/// ~30 Hz. Fast enough that a bar never looks stuck, slow enough that a
/// hundreds-of-archives fan-out doesn't spend its time in the callback.
constexpr auto kMinUpdateInterval = std::chrono::milliseconds(33);
} // namespace

ProgressReporter::ProgressReporter(ProgressCallback callback, std::vector<ProgressStep> plan)
    : m_callback(std::move(callback)), m_plan(std::move(plan)) {}

void ProgressReporter::restart(std::vector<ProgressStep> plan) {
    m_plan = std::move(plan);
    m_sink = nullptr;
    m_cancelled.store(false, std::memory_order_relaxed);
    m_step = ProgressStep::Ready;
    m_stepIndex = 0;
    m_stepTotal = 0;
    m_lastCurrent = 0;
    m_lastTotal = 0;
    m_stepOpen = false;
    m_start = Clock::now();
    m_lastEmit = {};
}

void ProgressReporter::dropStep(ProgressStep step) {
    auto it = std::find(m_plan.begin(), m_plan.end(), step);
    if (it == m_plan.end() || u32(it - m_plan.begin()) <= m_stepIndex)
        return;
    m_plan.erase(it);
}

bool ProgressReporter::emit(ProgressState state, u64 current, u64 total, u64 bytesDone,
                            u64 bytesTotal, std::string_view object, bool throttled) {
    if (!m_callback)
        return true;
    if (m_cancelled.load(std::memory_order_relaxed))
        return false;

    std::unique_lock<std::mutex> lock(m_mutex, std::defer_lock);
    if (throttled) {
        // A sample that would cost a worker a wait is a sample worth dropping.
        if (!lock.try_lock())
            return true;
        auto const now = Clock::now();
        bool const final = (total != 0 && current >= total);
        if (!final && now - m_lastEmit < kMinUpdateInterval)
            return true;
        m_lastEmit = now;
    } else {
        lock.lock();
        m_lastEmit = Clock::now();
    }

    // An End that was given no tally inherits the last one a worker reported —
    // the loop counted, the caller shouldn't have to count again.
    if (state == ProgressState::End) {
        if (current == kInheritCount)
            current = m_lastCurrent;
        if (total == kInheritCount)
            total = m_lastTotal != 0 ? m_lastTotal : current;
    } else if (state == ProgressState::Update) {
        m_lastCurrent = current;
        m_lastTotal = total;
    }

    ProgressInfo info;
    info.step = m_step;
    info.state = state;
    info.object = object;
    info.current = current;
    info.total = total;
    info.bytesDone = bytesDone;
    info.bytesTotal = bytesTotal;
    info.stepIndex = m_stepIndex;
    info.stepCount = u32(m_plan.size());
    info.elapsedMs =
        std::chrono::duration<double, std::milli>(m_lastEmit - m_start).count();

    if (!m_callback(info)) {
        m_cancelled.store(true, std::memory_order_relaxed);
        return false;
    }
    return true;
}

bool ProgressReporter::begin(ProgressStep step, std::string_view object, u64 total) {
    if (!m_callback)
        return !cancelled();

    // A step can be re-entered — the root loader tries TVFS and falls back to
    // ROOT — so close the open one rather than nest, and never let the position
    // go backwards.
    if (m_stepOpen)
        end();

    m_sink = nullptr;
    m_stepOpen = true;
    m_step = step;
    m_stepTotal = total;
    m_lastCurrent = 0;
    m_lastTotal = total;
    auto it = std::find(m_plan.begin(), m_plan.end(), step);
    if (it != m_plan.end())
        m_stepIndex = std::max(m_stepIndex, u32(it - m_plan.begin()));

    return emit(ProgressState::Begin, 0, total, 0, 0, object, false);
}

/// Called from worker threads: it must not touch the step state that begin()
/// owns, only forward the counters the caller already holds.
bool ProgressReporter::update(u64 current, u64 total, std::string_view object) {
    return emit(ProgressState::Update, current, total, 0, 0, object, true);
}

bool ProgressReporter::updateBytes(u64 bytesDone, u64 bytesTotal, std::string_view object) {
    return emit(ProgressState::Update, 0, m_stepTotal, bytesDone, bytesTotal, object, true);
}

bool ProgressReporter::end(u64 current, std::string_view object) {
    if (!m_callback || !m_stepOpen)
        return !cancelled();
    m_stepOpen = false;
    u64 const total = (current == kInheritCount && m_stepTotal != 0) ? m_stepTotal : kInheritCount;
    return emit(ProgressState::End, current, total, 0, 0, object, false);
}

void ProgressReporter::ready() {
    if (!m_callback)
        return;
    if (m_stepOpen)
        end();
    m_sink = nullptr;
    m_step = ProgressStep::Ready;
    m_stepIndex = u32(m_plan.size());
    m_stepTotal = 0;
    // Terminal: a cancel here has nothing left to stop, so the result is ignored.
    emit(ProgressState::End, 1, 1, 0, 0, {}, false);
}

const ProgressSink* ProgressReporter::sink() {
    if (!m_callback)
        return nullptr;
    if (!m_sink) {
        m_sink = [this](u64 done, u64 total, std::string_view object) {
            return update(done, total, object);
        };
    }
    return &m_sink;
}

} // namespace whiteout::storages::casc
