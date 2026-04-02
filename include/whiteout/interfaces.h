// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "common_types.h"
#include "compatibility.h"

namespace whiteout::interfaces {

class TimelineSemaphore {
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

struct WorkerTask {
    std::function<void()> fn;

    TimelineSemaphore* waitSemaphore = nullptr;
    TimelineSemaphore::Value waitValue = 0;

    TimelineSemaphore* signalSemaphore = nullptr;
    TimelineSemaphore::Value signalValue = 0;
};

class WorkerPool {
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
    virtual std::unique_ptr<TimelineSemaphore> createTimelineSemaphore() {
        return nullptr;
    }
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

// ============================================================================
// HTTP Handler (for online CASC storage)
// ============================================================================

/// Result of an HTTP GET request.
struct HttpResponse {
    i32 statusCode = 0;          ///< HTTP status code (200, 206, 404, …).
    std::vector<u8> body;        ///< Response body.
    std::string error;           ///< Transport-level error (empty on success).
};

/// Completion callback for async HTTP requests.
using HttpCallback = std::function<void(HttpResponse)>;

/// Capability flags reported by the handler.
namespace HttpCapability {
    static constexpr u32 None              = 0;
    static constexpr u32 Http2Multiplexing = 0x1; ///< Connection multiplexing (HTTP/2).
}

/// Abstract HTTP handler.  The library calls these methods to fetch CDN data.
/// Users must provide a concrete implementation.
///
/// Both methods are pure virtual — async support is mandatory.
/// The handler invokes @p callback exactly once when the request completes
/// (or fails). The callback may be invoked from any thread.
///
/// Thread safety: methods may be called concurrently from multiple
/// WorkerPool threads.  The implementation must be thread-safe.
class HttpHandler {
public:
    virtual ~HttpHandler() = default;

    /// Report supported optional capabilities.  The library inspects this
    /// once at open() time to tune its dispatch strategy.
    /// Default: no optional capabilities.
    virtual u32 capabilities() const noexcept { return HttpCapability::None; }

    /// Start a non-blocking HTTP GET for the full resource.
    /// The handler must invoke @p callback exactly once when the request
    /// completes (success or failure).  The callback may be invoked on
    /// any thread (e.g. an I/O thread, the calling thread, a thread pool).
    /// @param url      Fully-qualified URL.
    /// @param callback Invoked with the response when the request completes.
    virtual void getAsync(const std::string& url,
                          HttpCallback callback) = 0;

    /// Start a non-blocking HTTP range-GET (inclusive byte range).
    /// Implementations should send `Range: bytes=start-end`.
    /// The handler must invoke @p callback exactly once when the request
    /// completes.
    /// @param url      Fully-qualified URL.
    /// @param start    First byte offset (inclusive).
    /// @param end      Last byte offset (inclusive).
    /// @param callback Invoked with the response when the request completes.
    virtual void getRangeAsync(const std::string& url, u64 start, u64 end,
                               HttpCallback callback) = 0;
};

} // namespace whiteout::interfaces
