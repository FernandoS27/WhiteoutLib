// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "whiteout/utils/timeline_semaphore.h"

#include <atomic>
#include <condition_variable>
#include <mutex>

namespace whiteout::utils {

struct TimelineSemaphore::Impl {
    std::atomic<Value> value; ///< Current completed value
    std::atomic<Value> next;  ///< Next value to hand out via next()
    std::mutex mutex;         ///< Guards cv waits
    std::condition_variable cv;

    explicit Impl(Value initialValue) noexcept : value(initialValue), next(initialValue + 1) {}
};

TimelineSemaphore::TimelineSemaphore(Value initialValue)
    : m_impl(std::make_unique<Impl>(initialValue)) {}

TimelineSemaphore::~TimelineSemaphore() = default;

TimelineSemaphore::Value TimelineSemaphore::value() const noexcept {
    return m_impl->value.load(std::memory_order_acquire);
}

void TimelineSemaphore::signal(Value v) noexcept {
    // Advance and notify under the SAME mutex acquisition.
    {
        std::lock_guard<std::mutex> const lock(m_impl->mutex);
        Value cur = m_impl->value.load(std::memory_order_relaxed);
        while (cur < v && !m_impl->value.compare_exchange_weak(cur, v, std::memory_order_release,
                                                               std::memory_order_relaxed)) {
            /* retry with updated cur */
        }
        m_impl->cv.notify_all();
    }
}

void TimelineSemaphore::wait(Value v) {
    std::unique_lock<std::mutex> lock(m_impl->mutex);
    m_impl->cv.wait(lock, [this, v] { return m_impl->value.load(std::memory_order_acquire) >= v; });
}

TimelineSemaphore::Value TimelineSemaphore::next() noexcept {
    return m_impl->next.fetch_add(1, std::memory_order_relaxed);
}

} // namespace whiteout::utils
