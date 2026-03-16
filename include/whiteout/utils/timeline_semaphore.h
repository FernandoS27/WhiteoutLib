// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include <memory>

#include "../interfaces.h"

namespace whiteout::utils {

/// CPU-side implementation of interfaces::TimelineSemaphore.
///
/// Maintains a monotonically increasing 64-bit counter.  Callers may signal
/// the semaphore to advance the counter and block waiting for it to reach a
/// target value.  next() reserves strictly increasing values so multiple
/// producers never collide.
class TimelineSemaphore : public interfaces::TimelineSemaphore
{
public:
    /// Create a semaphore with an initial completed timeline value.
    ///
    /// @param initialValue Initial value returned by value().
    explicit TimelineSemaphore(Value initialValue = 0);

    /// Destroy the semaphore and release implementation resources.
    ~TimelineSemaphore() override;

    /// Get the currently completed timeline value.
    ///
    /// @return The latest signaled value.
    Value value() const noexcept override;

    /// Advance the timeline to at least @p v.
    ///
    /// If the current value is already greater than or equal to @p v, this is
    /// a no-op. Otherwise the timeline is advanced and waiting threads are
    /// notified.
    ///
    /// @param v Target value to signal.
    void  signal(Value v) noexcept override;

    /// Block until the completed value reaches at least @p v.
    ///
    /// @param v Target value to wait for.
    void  wait(Value v) override;

    /// Reserve and return the next timeline value for future signaling.
    ///
    /// @return A unique monotonically increasing value.
    Value next() noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace whiteout::utils
