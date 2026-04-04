// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
/// @file flat_hash_map.h
/// @brief Minimal open-addressing flat hash map optimized for u64 keys.
///
/// Designed for CASC key lookups where keys are derived from MD5 hashes
/// (first 8–9 bytes), providing good distribution without additional hashing.
/// Uses power-of-2 bucket counts with linear probing for cache efficiency.
///
/// Internal header — not part of the public include path.
#pragma once

#include <whiteout/common_types.h>

#include <cstring>
#include <utility>
#include <vector>

namespace whiteout::storages::casc {

/// Open-addressing hash map with u64 keys and fixed-type values.
///
/// Uses identity masking (key & mask) instead of std::hash, exploiting the
/// uniform distribution of MD5-derived keys. Power-of-2 bucket count with
/// linear probing gives excellent cache locality.
template<typename Value>
class FlatHashMap {
public:
    FlatHashMap() = default;

    /// Pre-allocate buckets for at least @p capacity entries at ~75% load.
    void reserve(size_t capacity) {
        if (capacity == 0) return;
        size_t needed = nextPow2(capacity + capacity / 3 + 16);
        if (needed <= m_buckets.size()) return;
        rehash(needed);
    }

    /// Insert a key-value pair. No-op if key already exists.
    void emplace(u64 key, const Value& value) {
        if (key == kEmpty) {
            if (!m_hasZero) {
                m_hasZero = true;
                m_zeroValue = value;
                ++m_size;
            }
            return;
        }
        maybeGrow();
        size_t idx = key & m_mask;
        while (true) {
            auto& b = m_buckets[idx];
            if (b.key == kEmpty) {
                b.key = key;
                b.value = value;
                ++m_size;
                return;
            }
            if (b.key == key)
                return; // Already present.
            idx = (idx + 1) & m_mask;
        }
    }

    /// Insert or overwrite a key-value pair.
    void insertOrAssign(u64 key, const Value& value) {
        if (key == kEmpty) {
            if (!m_hasZero)
                ++m_size;
            m_hasZero = true;
            m_zeroValue = value;
            return;
        }
        maybeGrow();
        size_t idx = key & m_mask;
        while (true) {
            auto& b = m_buckets[idx];
            if (b.key == kEmpty) {
                b.key = key;
                b.value = value;
                ++m_size;
                return;
            }
            if (b.key == key) {
                b.value = value;
                return;
            }
            idx = (idx + 1) & m_mask;
        }
    }

    /// Look up a key. Returns pointer to value, or nullptr if not found.
    const Value* find(u64 key) const {
        if (key == kEmpty)
            return m_hasZero ? &m_zeroValue : nullptr;
        size_t idx = key & m_mask;
        while (true) {
            auto& b = m_buckets[idx];
            if (b.key == key)
                return &b.value;
            if (b.key == kEmpty)
                return nullptr;
            idx = (idx + 1) & m_mask;
        }
    }

    size_t size() const { return m_size; }

    /// Iterate over all entries. Callback signature: void(u64 key, const Value& value).
    template<typename Fn>
    void forEach(Fn&& fn) const {
        if (m_hasZero)
            fn(u64(0), m_zeroValue);
        for (auto& b : m_buckets) {
            if (b.key != kEmpty)
                fn(b.key, b.value);
        }
    }

private:
    static constexpr u64 kEmpty = 0;
    static constexpr size_t kInitialCapacity = 16;

    struct Bucket {
        u64 key = kEmpty;
        Value value{};
    };

    static size_t nextPow2(size_t v) {
        if (v == 0) return 1;
        v--;
        v |= v >> 1;
        v |= v >> 2;
        v |= v >> 4;
        v |= v >> 8;
        v |= v >> 16;
        v |= v >> 32;
        return v + 1;
    }

    void maybeGrow() {
        // Auto-init if empty, or grow at ~75% load.
        if (m_buckets.empty()) {
            m_buckets.assign(kInitialCapacity, Bucket{});
            m_mask = kInitialCapacity - 1;
        } else if (m_size * 4 >= m_buckets.size() * 3) {
            rehash(m_buckets.size() * 2);
        }
    }

    void rehash(size_t newBucketCount) {
        std::vector<Bucket> oldBuckets = std::move(m_buckets);
        m_buckets.assign(newBucketCount, Bucket{});
        m_mask = newBucketCount - 1;
        for (auto& ob : oldBuckets) {
            if (ob.key != kEmpty) {
                size_t idx = ob.key & m_mask;
                while (m_buckets[idx].key != kEmpty)
                    idx = (idx + 1) & m_mask;
                m_buckets[idx] = std::move(ob);
            }
        }
    }

    std::vector<Bucket> m_buckets;
    size_t m_mask = 0;
    size_t m_size = 0;
    bool m_hasZero = false;
    Value m_zeroValue{};
};

} // namespace whiteout::storages::casc
