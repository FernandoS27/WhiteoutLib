// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "cdn_fetcher.h"

#include <condition_variable>
#include <memory>
#include <mutex>

namespace whiteout::storages::casc {

CdnFetcher::CdnFetcher(interfaces::HttpHandler* http, std::vector<CdnServer> servers,
                       CdnCache* cache)
    : m_http(http), m_servers(std::move(servers)), m_cache(cache) {}

std::string CdnFetcher::buildUrl(const CdnServer& server, const std::string& pathType,
                                 const std::string& keyHex) const {
    // https://<host>/<path>/<pathType>/XX/YY/<keyHex>
    if (keyHex.size() < 4)
        return {};
    return "https://" + server.host + "/" + server.path + "/" + pathType + "/" +
           keyHex.substr(0, 2) + "/" + keyHex.substr(2, 2) + "/" + keyHex;
}

bool CdnFetcher::supportsHttp2() const {
    return (m_http->capabilities() & interfaces::HttpCapability::Http2Multiplexing) != 0;
}

// ── Synchronous helpers (block via condition_variable) ─────────────
//
// NOTE: The synchronization state is heap-allocated via shared_ptr so that
// the callback always holds a valid reference even if the calling function
// times out and its stack frame is reused before the HTTP worker fires.
// Capturing local variables by reference ([&]) is unsafe here because the
// SimpleHttpHandler queue may be backed up (e.g. 300+ archive-index fetches
// for WoW are in-flight) and a 60-second timeout can expire while the request
// is still pending — after which the loop variable storage is destroyed and
// potentially reused.

namespace {

struct SyncState {
    std::mutex mtx;
    std::condition_variable cv;
    bool done = false;
    interfaces::HttpResponse result;
};

} // anonymous namespace

std::optional<std::vector<u8>> CdnFetcher::fetchUrl(const std::string& url) {
    auto sync = std::make_shared<SyncState>();

    m_http->getAsync(url, [sync](interfaces::HttpResponse resp) {
        std::lock_guard<std::mutex> const lk(sync->mtx);
        sync->result = std::move(resp);
        sync->done = true;
        sync->cv.notify_one();
    });

    std::unique_lock<std::mutex> lk(sync->mtx);
    if (!sync->cv.wait_for(lk, std::chrono::seconds(60), [&] { return sync->done; }))
        return std::nullopt; // Timeout — HttpHandler did not invoke callback.

    if (sync->result.statusCode == 200)
        return std::move(sync->result.body);
    return std::nullopt;
}

std::optional<std::vector<u8>> CdnFetcher::fetchWithFailover(const std::string& pathType,
                                                             const std::string& keyHex) {
    if (m_servers.empty())
        return std::nullopt;

    size_t const startIdx = m_currentCdn.load(std::memory_order_relaxed);
    for (size_t attempt = 0; attempt < m_servers.size(); ++attempt) {
        size_t const idx = (startIdx + attempt) % m_servers.size();
        auto url = buildUrl(m_servers[idx], pathType, keyHex);
        if (url.empty())
            continue;

        auto sync = std::make_shared<SyncState>();

        m_http->getAsync(url, [sync](interfaces::HttpResponse resp) {
            std::lock_guard<std::mutex> const lk(sync->mtx);
            sync->result = std::move(resp);
            sync->done = true;
            sync->cv.notify_one();
        });

        std::unique_lock<std::mutex> lk(sync->mtx);
        if (!sync->cv.wait_for(lk, std::chrono::seconds(60), [&] { return sync->done; }))
            continue; // Timeout — try next server.

        if (sync->result.statusCode == 200) {
            // Remember this server as the last-good one.
            m_currentCdn.store(idx, std::memory_order_relaxed);
            return std::move(sync->result.body);
        }
        // On 404, don't failover — the file doesn't exist.
        if (sync->result.statusCode == 404)
            return std::nullopt;
        // On other errors (5xx, transport failure), try next server.
    }
    return std::nullopt;
}

std::optional<std::vector<u8>> CdnFetcher::fetch(const std::string& pathType,
                                                 const std::string& keyHex) {
    // Check disk cache first.
    if (m_cache) {
        if (auto cached = m_cache->read(pathType, keyHex))
            return cached;
    }

    auto result = fetchWithFailover(pathType, keyHex);
    if (result && m_cache) {
        m_cache->write(pathType, keyHex, *result);
    }
    return result;
}

std::optional<std::vector<u8>> CdnFetcher::fetchRange(const std::string& archiveKeyHex, u64 offset,
                                                      u32 size) {
    // Check disk cache first.
    if (m_cache) {
        if (auto cached = m_cache->readRange(archiveKeyHex, offset, size))
            return cached;
    }

    if (m_servers.empty())
        return std::nullopt;

    size_t const startIdx = m_currentCdn.load(std::memory_order_relaxed);
    for (size_t attempt = 0; attempt < m_servers.size(); ++attempt) {
        size_t const idx = (startIdx + attempt) % m_servers.size();
        auto url = buildUrl(m_servers[idx], "data", archiveKeyHex);
        if (url.empty())
            continue;

        auto sync = std::make_shared<SyncState>();

        m_http->getRangeAsync(url, offset, offset + size - 1,
                              [sync](interfaces::HttpResponse resp) {
                                  std::lock_guard<std::mutex> const lk(sync->mtx);
                                  sync->result = std::move(resp);
                                  sync->done = true;
                                  sync->cv.notify_one();
                              });

        std::unique_lock<std::mutex> lk(sync->mtx);
        if (!sync->cv.wait_for(lk, std::chrono::seconds(60), [&] { return sync->done; }))
            continue; // Timeout — try next server.

        if (sync->result.statusCode == 200 || sync->result.statusCode == 206) {
            m_currentCdn.store(idx, std::memory_order_relaxed);
            if (m_cache) {
                m_cache->writeRange(archiveKeyHex, offset, size, sync->result.body);
            }
            return std::move(sync->result.body);
        }
        if (sync->result.statusCode == 404)
            return std::nullopt;
    }
    return std::nullopt;
}

// ── Async API ─────────────────────────────────────────────────────

void CdnFetcher::getUrlAsync(const std::string& url,
                             std::function<void(interfaces::HttpResponse)> callback) {
    m_http->getAsync(url, std::move(callback));
}

void CdnFetcher::fetchAsync(const std::string& pathType, const std::string& keyHex,
                            FetchCallback callback) {
    // Check disk cache first.
    if (m_cache) {
        if (auto cached = m_cache->read(pathType, keyHex)) {
            callback(std::move(cached));
            return;
        }
    }

    if (m_servers.empty()) {
        callback(std::nullopt);
        return;
    }

    // Fire request against current best CDN server. On success, cache + callback.
    // On failure, callback with nullopt (simplified — no async failover chain).
    size_t const idx = m_currentCdn.load(std::memory_order_relaxed);
    auto url = buildUrl(m_servers[idx], pathType, keyHex);
    if (url.empty()) {
        callback(std::nullopt);
        return;
    }

    auto* cache = m_cache;
    auto pt = pathType;
    auto kh = keyHex;
    auto* currentCdnPtr = &m_currentCdn;
    auto serverIdx = idx;

    m_http->getAsync(url, [cache, pt, kh, callback = std::move(callback), currentCdnPtr,
                           serverIdx](interfaces::HttpResponse resp) {
        if (resp.statusCode == 200) {
            currentCdnPtr->store(serverIdx, std::memory_order_relaxed);
            if (cache)
                cache->write(pt, kh, resp.body);
            callback(std::move(resp.body));
        } else {
            callback(std::nullopt);
        }
    });
}

void CdnFetcher::fetchRangeAsync(const std::string& archiveKeyHex, u64 offset, u32 size,
                                 FetchCallback callback) {
    // Check disk cache first.
    if (m_cache) {
        if (auto cached = m_cache->readRange(archiveKeyHex, offset, size)) {
            callback(std::move(cached));
            return;
        }
    }

    if (m_servers.empty()) {
        callback(std::nullopt);
        return;
    }

    size_t const idx = m_currentCdn.load(std::memory_order_relaxed);
    auto url = buildUrl(m_servers[idx], "data", archiveKeyHex);
    if (url.empty()) {
        callback(std::nullopt);
        return;
    }

    auto* cache = m_cache;
    auto akh = archiveKeyHex;
    auto off = offset;
    auto sz = size;
    auto* currentCdnPtr = &m_currentCdn;
    auto serverIdx = idx;

    m_http->getRangeAsync(url, offset, offset + size - 1,
                          [cache, akh, off, sz, callback = std::move(callback), currentCdnPtr,
                           serverIdx](interfaces::HttpResponse resp) {
                              if (resp.statusCode == 200 || resp.statusCode == 206) {
                                  currentCdnPtr->store(serverIdx, std::memory_order_relaxed);
                                  if (cache)
                                      cache->writeRange(akh, off, sz, resp.body);
                                  callback(std::move(resp.body));
                              } else {
                                  callback(std::nullopt);
                              }
                          });
}

} // namespace whiteout::storages::casc
