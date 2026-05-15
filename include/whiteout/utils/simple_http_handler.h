// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include <cstddef>
#include <memory>

#include "../interfaces.h"

namespace whiteout::utils {

/// Basic HTTP handler with two platform backends:
///   * Windows  — WinHTTP
///   * Other    — libcurl (linked at build time)
///
/// Requests are dispatched asynchronously onto an internal thread pool;
/// each worker owns one backend handle and keeps its connection cache warm.
/// Thread-safe: multiple threads may call getAsync / getRangeAsync
/// concurrently.
class SimpleHttpHandler : public interfaces::HttpHandler {
public:
    /// Create a handler with @p nThreads I/O worker threads (default: 4).
    explicit SimpleHttpHandler(size_t nThreads = 4);

    ~SimpleHttpHandler() override;

    // Non-copyable, non-movable (PImpl).
    SimpleHttpHandler(const SimpleHttpHandler&) = delete;
    SimpleHttpHandler& operator=(const SimpleHttpHandler&) = delete;

    u32 capabilities() const noexcept override;

    void getAsync(const std::string& url,
                  interfaces::HttpCallback callback) override;

    void getRangeAsync(const std::string& url, u64 start, u64 end,
                       interfaces::HttpCallback callback) override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace whiteout::utils
