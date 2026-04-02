// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
/// @file cdn_fetcher.h
/// @brief Centralized HTTP fetch logic with CDN failover, caching, and async dispatch.
///
/// Internal header — not part of the public include path.
#pragma once

#include <whiteout/common_types.h>
#include <whiteout/interfaces.h>
#include <whiteout/storages/casc/types.h>

#include "cdn_cache.h"

#include <atomic>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace whiteout::storages::casc {

/// Centralized CDN fetch logic with failover, caching, and async dispatch.
class CdnFetcher {
public:
    using FetchCallback = std::function<void(std::optional<std::vector<u8>>)>;

    CdnFetcher(interfaces::HttpHandler* http,
               std::vector<CdnServer> servers,
               CdnCache* cache);

    /// Fetch a full CDN resource by path type and key (synchronous).
    /// pathType: "config", "data", or "patch"
    /// key: hex-encoded hash (32 chars)
    /// Tries each CDN server in order; caches result on success.
    std::optional<std::vector<u8>> fetch(const std::string& pathType,
                                         const std::string& keyHex);

    /// Fetch a byte range from an archive on CDN (synchronous).
    std::optional<std::vector<u8>> fetchRange(const std::string& archiveKeyHex,
                                              u64 offset, u32 size);

    /// Fetch a raw URL (synchronous, for versions/cdns endpoints).
    std::optional<std::vector<u8>> fetchUrl(const std::string& url);

    // ── Callback-based async API ─────────────────────────────────

    /// Start an async full-resource fetch.
    void fetchAsync(const std::string& pathType,
                    const std::string& keyHex,
                    FetchCallback callback);

    /// Start an async range fetch.
    void fetchRangeAsync(const std::string& archiveKeyHex,
                         u64 offset, u32 size,
                         FetchCallback callback);

    /// Whether the handler supports HTTP/2 multiplexing.
    bool supportsHttp2() const;

private:
    interfaces::HttpHandler* m_http;
    std::vector<CdnServer> m_servers;
    CdnCache* m_cache;     // May be null (no disk cache).
    std::atomic<size_t> m_currentCdn{0};

    /// Build CDN URL: https://<host>/<path>/<pathType>/XX/YY/<keyHex>
    std::string buildUrl(const CdnServer& server,
                         const std::string& pathType,
                         const std::string& keyHex) const;

    /// Synchronous GET with CDN failover. Returns response body or nullopt.
    std::optional<std::vector<u8>> fetchWithFailover(
        const std::string& pathType, const std::string& keyHex);

    /// Async GET for a single URL (no failover).
    void getUrlAsync(const std::string& url,
                     std::function<void(interfaces::HttpResponse)> callback);
};

} // namespace whiteout::storages::casc
