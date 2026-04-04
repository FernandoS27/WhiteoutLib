// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
/// @file entry_index.h
/// @brief Multimap index for RootEntry lookups, reducing per-root boilerplate.
///
/// Internal header — not part of the public include path.
#pragma once

#include "../root.h"

#include <unordered_map>
#include <vector>

namespace whiteout::storages::casc {

/// A multimap index that maps keys to indices into a flat RootEntry vector.
/// Wraps std::unordered_multimap to eliminate repeated lookup boilerplate
/// across root implementations.
template<typename Key>
class EntryIndex {
public:
    void reserve(size_t n) { m_map.reserve(n); }
    void clear() { m_map.clear(); }
    void emplace(const Key& key, size_t index) { m_map.emplace(key, index); }
    void emplace(Key&& key, size_t index) { m_map.emplace(std::move(key), index); }

    /// Look up all entries matching a key.
    std::vector<const RootEntry*> findAll(const std::vector<RootEntry>& entries,
                                          const Key& key) const {
        std::vector<const RootEntry*> results;
        auto range = m_map.equal_range(key);
        for (auto it = range.first; it != range.second; ++it)
            results.push_back(&entries[it->second]);
        return results;
    }

    /// Check if any entry matches a key.
    bool contains(const Key& key) const {
        return m_map.find(key) != m_map.end();
    }

    /// Check if the index is empty.
    bool empty() const { return m_map.empty(); }

    /// Number of entries in the index.
    size_t size() const { return m_map.size(); }

private:
    std::unordered_multimap<Key, size_t> m_map;
};

} // namespace whiteout::storages::casc
