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

/// @bind methods, subclassable, no_default_ctor, jni_package=whiteout.interfaces,
/// java_package=whiteout.utils — abstract opaque base. Concrete impl: utils::SimpleThreadPool.
class WorkerPool {
public:
    virtual ~WorkerPool() = default;

    /// @bind submit_workertask — JNI bridge: Java sees `submit(WorkerTask task)` with the
    /// std::function exposed as a Runnable and the two TimelineSemaphore pointers wrapped in opaque
    /// Java handles. WorkerTask implements Runnable; its default `run()` honours wait → fn → signal
    /// so simple pools can just `exec.submit(task)`.
    virtual void submit(const WorkerTask& task) = 0;

    /// @bind — Block until every submitted task has completed.
    virtual void waitIdle() = 0;

    /// @bind — Number of worker threads in this pool.
    virtual size_t threadCount() const noexcept = 0;

    /// @bind skip — JNI bridge's stub override returns a real `utils::TimelineSemaphore`
    /// rather than nullptr so library callers always get a working semaphore even on
    /// Java-backed pools. (Skipped from codegen-driven binding because unique_ptr
    /// returns don't round-trip; the override is hard-coded in emit_jni.)
    virtual std::unique_ptr<TimelineSemaphore> createTimelineSemaphore() {
        return nullptr;
    }
};

/// @bind methods, subclassable, no_default_ctor, jni_package=whiteout.interfaces,
/// java_package=whiteout.utils — abstract file system that resolves files by numeric data ID (e.g.
/// CASC).
class CascFileSystem {
public:
    virtual ~CascFileSystem() = default;

    /// @bind — Read the entire contents of a file by its numeric data ID.
    virtual std::vector<u8> readFile(u32 fileId) const = 0;

    /// @bind — Resolve a path to a numeric file ID (nullable).
    virtual std::optional<u32> reserveFileId(const std::string& path) = 0;

    /// @bind — Write a file by its numeric data ID. Returns true on success.
    virtual bool writeFile(u32 fileId, const std::vector<u8>& data) = 0;

    /// @bind — Check if a file with the given data ID exists.
    virtual bool fileExists(u32 fileId) const = 0;
};

/// Entry returned by VirtualPathFileSystem::listDirectory().
struct DirectoryEntry {
    std::string name;
    bool isDirectory;
};

/// @bind methods, subclassable, no_default_ctor, jni_package=whiteout.interfaces,
/// java_package=whiteout.utils — abstract base. Concrete impl: utils::OsFileSystem.
class VirtualPathFileSystem {
public:
    virtual ~VirtualPathFileSystem() = default;

    /// @bind — Read the entire contents of a file into a byte vector.
    virtual std::vector<u8> readFile(const std::string& path) const = 0;

    /// @bind — Write a file. Returns true on success.
    virtual bool writeFile(const std::string& path, const std::vector<u8>& data) = 0;

    /// @bind — Check if a file exists at the given path.
    virtual bool fileExists(const std::string& path) const = 0;

    /// @bind skip — DirectoryEntry vector not exposed via codegen yet
    virtual std::vector<DirectoryEntry> listDirectory(const std::string& path) const = 0;
};

// ============================================================================
// HTTP Handler (for online CASC storage)
// ============================================================================

/// @bind value_object, java_package=whiteout.utils — HTTP GET result handed to user callbacks.
struct HttpResponse {
    i32 statusCode = 0;   ///< HTTP status code (200, 206, 404, …).
    std::vector<u8> body; ///< Response body.
    std::string error;    ///< Transport-level error (empty on success).
};

/// Completion callback for async HTTP requests.
using HttpCallback = std::function<void(HttpResponse)>;

/// Capability flags reported by the handler.
namespace HttpCapability {
static constexpr u32 None = 0;
static constexpr u32 Http2Multiplexing = 0x1; ///< Connection multiplexing (HTTP/2).
} // namespace HttpCapability

/// Abstract HTTP handler.  The library calls these methods to fetch CDN data.
/// Users must provide a concrete implementation.
///
/// Both methods are pure virtual — async support is mandatory.
/// The handler invokes @p callback exactly once when the request completes
/// (or fails). The callback may be invoked from any thread.
///
/// Thread safety: methods may be called concurrently from multiple
/// WorkerPool threads.  The implementation must be thread-safe.
/// @bind methods, subclassable, no_default_ctor, jni_package=whiteout.interfaces,
/// java_package=whiteout.utils — abstract opaque base. Concrete impl: utils::SimpleHttpHandler.
class HttpHandler {
public:
    virtual ~HttpHandler() = default;

    /// @bind — Reported handler capability flags.
    virtual u32 capabilities() const noexcept {
        return HttpCapability::None;
    }

    /// @bind — Issue an async GET. The callback receives the response and is fired exactly once.
    virtual void getAsync(const std::string& url, HttpCallback callback) = 0;

    /// @bind — Issue an async range-GET (inclusive byte range).
    virtual void getRangeAsync(const std::string& url, u64 start, u64 end,
                               HttpCallback callback) = 0;
};

} // namespace whiteout::interfaces
