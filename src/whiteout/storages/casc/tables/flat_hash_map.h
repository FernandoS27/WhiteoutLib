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

#if defined(_MSC_VER) && !defined(__clang__) && (defined(_M_X64) || defined(_M_IX86))
#include <xmmintrin.h>
#endif

namespace whiteout::storages::casc {

/// Warm the cache line at @p p. Table lookups that chain through an index into
/// a second array need this for the second hop, which no key alone can locate.
inline void prefetchAddress(const void* p) {
#if defined(__GNUC__) || defined(__clang__)
    __builtin_prefetch(p, 0, 1);
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    _mm_prefetch(static_cast<const char*>(p), _MM_HINT_T0);
#else
    (void)p;
#endif
}

/// Open-addressing hash map with u64 keys and fixed-type values.
///
/// Uses identity masking (key & mask) instead of std::hash, exploiting the
/// uniform distribution of MD5-derived keys. Power-of-2 bucket count with
/// linear probing gives excellent cache locality.
template <typename Value>
class FlatHashMap {
public:
    FlatHashMap() = default;

    /// Pre-allocate buckets for at least @p capacity entries at ~75% load.
    void reserve(size_t capacity) {
        if (capacity == 0)
            return;
        size_t needed = nextPow2(capacity + capacity / 3 + 16);
        if (needed <= m_buckets.size())
            return;
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

    /// Reference to the value for @p key, inserting @p initial if it is absent.
    /// A caller that needs the current value before overwriting it — chaining a
    /// collision list, say — otherwise probes the table twice.
    Value& findOrInsert(u64 key, const Value& initial) {
        if (key == kEmpty) {
            if (!m_hasZero) {
                m_hasZero = true;
                m_zeroValue = initial;
                ++m_size;
            }
            return m_zeroValue;
        }
        maybeGrow();
        size_t idx = key & m_mask;
        while (true) {
            auto& b = m_buckets[idx];
            if (b.key == kEmpty) {
                b.key = key;
                b.value = initial;
                ++m_size;
                return b.value;
            }
            if (b.key == key)
                return b.value;
            idx = (idx + 1) & m_mask;
        }
    }

    /// Look up a key. Returns pointer to value, or nullptr if not found.
    const Value* find(u64 key) const {
        if (key == kEmpty)
            return m_hasZero ? &m_zeroValue : nullptr;
        // Buckets are allocated lazily on first insert; a map that never took
        // one (e.g. an IndexTable with both LazyIdxBuckets and LazyArchiveIndex)
        // would otherwise index into an empty vector.
        if (m_buckets.empty())
            return nullptr;
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

    size_t size() const {
        return m_size;
    }

    /// Warm the cache line that a later find/emplace of @p key will touch.
    /// Bulk builds are DRAM-latency bound — the archive index alone inserts
    /// 4.7 M keys into a ~320 MB table — so issuing this a few iterations ahead
    /// of the insert hides most of the miss.
    void prefetch(u64 key) const {
        if (m_buckets.empty())
            return;
        const void* p = &m_buckets[key & m_mask];
#if defined(__GNUC__) || defined(__clang__)
        __builtin_prefetch(p, 1, 1);
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
        _mm_prefetch(static_cast<const char*>(p), _MM_HINT_T0);
#else
        (void)p;
#endif
    }

    /// Iterate over all entries. Callback signature: void(u64 key, const Value& value).
    template <typename Fn>
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
        if (v == 0)
            return 1;
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
