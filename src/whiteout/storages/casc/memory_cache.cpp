// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "memory_cache.h"

#include <cstring>

namespace whiteout::storages::casc {

MemoryCache::MemoryCache(size_t maxBytes) : m_maxBytes(maxBytes) {}

std::optional<std::vector<u8>> MemoryCache::get(const std::array<u8, 16>& eKey) const {
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_map.find(eKey);
    if (it == m_map.end()) return std::nullopt;

    // Promote to front (MRU).
    m_lru.splice(m_lru.begin(), m_lru, it->second);
    return it->second->data;
}

void MemoryCache::put(const std::array<u8, 16>& eKey, const std::vector<u8>& data) {
    std::lock_guard<std::mutex> lk(m_mutex);

    // If already cached, update and promote.
    auto it = m_map.find(eKey);
    if (it != m_map.end()) {
        m_currentBytes -= it->second->data.size();
        it->second->data = data;
        m_currentBytes += data.size();
        m_lru.splice(m_lru.begin(), m_lru, it->second);
        evict();
        return;
    }

    // Don't cache entries larger than the entire budget.
    if (data.size() > m_maxBytes) return;

    // Insert at front.
    m_lru.push_front(CacheEntry{eKey, data});
    m_map[eKey] = m_lru.begin();
    m_currentBytes += data.size();

    evict();
}

void MemoryCache::clear() {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_lru.clear();
    m_map.clear();
    m_currentBytes = 0;
}

void MemoryCache::evict() {
    // Caller must hold m_mutex.
    while (m_currentBytes > m_maxBytes && !m_lru.empty()) {
        auto& back = m_lru.back();
        m_currentBytes -= back.data.size();
        m_map.erase(back.eKey);
        m_lru.pop_back();
    }
}

} // namespace whiteout::storages::casc
