// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
/// @file cache_traits.h
/// @brief Cache policy traits for StorageBackendImpl.
///
/// MemCacheEnabled wraps MemoryCache; NoCachePolicy compiles to no-ops.
/// Internal header — not part of the public include path.
#pragma once

#include "../cdn/memory_cache.h"

#include <array>
#include <memory>
#include <optional>
#include <vector>

namespace whiteout::storages::casc {

/// Cache trait that wraps a MemoryCache instance.
/// All methods forward to the underlying cache.
struct MemCacheEnabled {
    mutable std::unique_ptr<MemoryCache> cache;

    explicit MemCacheEnabled(std::unique_ptr<MemoryCache> c)
        : cache(std::move(c)) {}

    MemCacheEnabled(MemCacheEnabled&&) = default;
    MemCacheEnabled& operator=(MemCacheEnabled&&) = default;

    static constexpr bool hasCache() noexcept { return true; }

    std::optional<std::vector<u8>> get(const std::array<u8, 16>& key) const {
        return cache->get(key);
    }

    std::optional<MemoryCache::CacheView> view(const std::array<u8, 16>& key) const {
        return cache->view(key);
    }

    void put(const std::array<u8, 16>& key, const std::vector<u8>& data) const {
        cache->put(key, data);
    }

    void flush() const { cache->clear(); }
};

/// No-op cache trait.  All methods compile to nothing.
struct NoCachePolicy {
    static constexpr bool hasCache() noexcept { return false; }

    std::optional<std::vector<u8>> get(const std::array<u8, 16>&) const {
        return std::nullopt;
    }

    std::optional<MemoryCache::CacheView> view(const std::array<u8, 16>&) const {
        return std::nullopt;
    }

    void put(const std::array<u8, 16>&, const std::vector<u8>&) const {}
    void flush() const {}
};

} // namespace whiteout::storages::casc
