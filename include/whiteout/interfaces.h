// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include <functional>
#include <cstddef>
#include <string>
#include <vector>

#include "common_types.h"

namespace whiteout::interfaces {

class TimelineSemaphore
{
public:
    using Value = u64;

    virtual ~TimelineSemaphore() = default;

    // Current completed value
    virtual Value value() const noexcept = 0;

    // Signal that the timeline reached at least v
    virtual void signal(Value v) noexcept = 0;

    // Block until timeline >= v
    virtual void wait(Value v) = 0;

    // Allocate the next timeline value
    virtual Value next() noexcept = 0;
};

struct WorkerTask
{
    std::function<void()> fn;

    TimelineSemaphore* waitSemaphore = nullptr;
    TimelineSemaphore::Value waitValue = 0;

    TimelineSemaphore* signalSemaphore = nullptr;
    TimelineSemaphore::Value signalValue = 0;
};

class WorkerPool
{
public:
    virtual ~WorkerPool() = default;

    // Submit a task with optional dependencies
    virtual void submit(const WorkerTask& task) = 0;

    // Wait until pool idle
    virtual void wait_idle() = 0;

    // Worker thread count
    virtual size_t thread_count() const noexcept = 0;
};

class VirtualFileSystem {
public:
    virtual ~VirtualFileSystem() = default;

    virtual bool supportsFileIds() const = 0;

    /// Read the entire contents of a file into a byte vector.
    virtual std::vector<u8> readFile(const std::string& path) const = 0;

    virtual std::vector<u8> readFile(u32 fileId) const = 0;

    /// Check if a file exists at the given path.
    virtual bool fileExists(const std::string& path) const = 0;

    virtual bool fileExists(u32 fileId) const = 0;
};

} // namespace whiteout::interfaces
