// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include <functional>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "common_types.h"
#include "compatibility.h"

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
    virtual void waitIdle() = 0;

    // Worker thread count
    virtual size_t threadCount() const noexcept = 0;

    /// Create a timeline semaphore for task dependency management.
    /// Returns nullptr by default; override in concrete pool implementations.
    virtual std::unique_ptr<TimelineSemaphore> createTimelineSemaphore() { return nullptr; }
};

/// Abstract file system that resolves files by numeric data ID (e.g. CASC).
class CascFileSystem {
public:
    virtual ~CascFileSystem() = default;

    /// Read the entire contents of a file by its numeric data ID.
    virtual std::vector<u8> readFile(u32 fileId) const = 0;

    virtual std::optional<u32> reserveFileId(const std::string& path) = 0;

    virtual bool writeFile(u32 fileId, const std::vector<u8>& data) = 0;

    /// Check if a file with the given data ID exists.
    virtual bool fileExists(u32 fileId) const = 0;
};

/// Entry returned by VirtualPathFileSystem::listDirectory().
struct DirectoryEntry {
    std::string name;
    bool isDirectory;
};

/// Abstract path-based file system (similar to std::filesystem).
class VirtualPathFileSystem {
public:
    virtual ~VirtualPathFileSystem() = default;

    /// Read the entire contents of a file into a byte vector.
    virtual std::vector<u8> readFile(const std::string& path) const = 0;

    virtual bool writeFile(const std::string& path, const std::vector<u8>& data) = 0;

    /// Check if a file exists at the given path.
    virtual bool fileExists(const std::string& path) const = 0;

    /// List all entries (files and subdirectories) in the given directory.
    virtual std::vector<DirectoryEntry> listDirectory(const std::string& path) const = 0;
};

} // namespace whiteout::interfaces
