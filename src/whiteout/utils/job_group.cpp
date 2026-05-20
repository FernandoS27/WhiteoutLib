// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "whiteout/utils/job_group.h"

#include <atomic>
#include <condition_variable>
#include <mutex>

namespace whiteout::utils {

struct JobGroup::Impl {
    std::atomic<size_t> remaining{0};
    std::mutex mutex;
    std::condition_variable cv;
    interfaces::TimelineSemaphore* signalSemaphore = nullptr;
    interfaces::TimelineSemaphore::Value signalValue = 0;
};

JobGroup::JobGroup() : m_impl(std::make_unique<Impl>()) {}

JobGroup::~JobGroup() = default;

void JobGroup::add(size_t n) {
    m_impl->remaining.fetch_add(n, std::memory_order_relaxed);
}

void JobGroup::done() {
    if (m_impl->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        std::lock_guard<std::mutex> const lock(m_impl->mutex);
        if (m_impl->signalSemaphore)
            m_impl->signalSemaphore->signal(m_impl->signalValue);
        m_impl->cv.notify_all();
    }
}

void JobGroup::wait() {
    std::unique_lock<std::mutex> lock(m_impl->mutex);
    m_impl->cv.wait(lock, [this] { return m_impl->remaining.load() == 0; });
}

bool JobGroup::isReady() const {
    return m_impl->remaining.load(std::memory_order_acquire) == 0;
}

void JobGroup::signalOnComplete(interfaces::TimelineSemaphore* sem,
                                interfaces::TimelineSemaphore::Value value) {
    std::lock_guard<std::mutex> const lock(m_impl->mutex);
    m_impl->signalSemaphore = sem;
    m_impl->signalValue = value;
    if (m_impl->remaining.load(std::memory_order_acquire) == 0)
        sem->signal(value);
}

} // namespace whiteout::utils
