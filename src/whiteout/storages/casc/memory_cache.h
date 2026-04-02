// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
/// @file memory_cache.h
/// @brief Thread-safe in-memory LRU cache for decoded BLTE blobs, keyed by EKey.
///
/// Internal header — not part of the public include path.
#pragma once

#include <whiteout/common_types.h>

#include <array>
#include <list>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace whiteout::storages::casc {

/// Simple thread-safe LRU cache for decoded file data, keyed by 16-byte EKey.
class MemoryCache {
public:
    explicit MemoryCache(size_t maxBytes = 256 * 1024 * 1024);

    /// Retrieve a cached blob (returns nullopt on miss). Promotes to MRU on hit.
    std::optional<std::vector<u8>> get(const std::array<u8, 16>& eKey) const;

    /// Insert a blob into the cache. Evicts LRU entries if over budget.
    void put(const std::array<u8, 16>& eKey, const std::vector<u8>& data);

    /// Clear all entries.
    void clear();

private:
    struct CacheEntry {
        std::array<u8, 16> eKey;
        std::vector<u8> data;
    };

    struct KeyHash {
        size_t operator()(const std::array<u8, 16>& k) const noexcept {
            u64 h = 0;
            std::memcpy(&h, k.data(), 8);
            return static_cast<size_t>(h);
        }
    };

    size_t m_maxBytes;
    mutable size_t m_currentBytes = 0;

    // LRU list: front = most recently used, back = least recently used.
    using LruList = std::list<CacheEntry>;
    mutable LruList m_lru;

    using LruIt = LruList::iterator;
    mutable std::unordered_map<std::array<u8, 16>, LruIt, KeyHash> m_map;

    mutable std::mutex m_mutex;

    void evict();
};

} // namespace whiteout::storages::casc
