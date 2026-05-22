// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
/// @file storage_backend_impl.h
/// @brief Template implementation of StorageBackend for {Local,Online} × {Cache,NoCache}.
///
/// Internal header — not part of the public include path.
#pragma once

#include "../codec/blte.h"
#include "../codec/crypto.h"
#include "../tables/encoding.h"
#include "cache_traits.h"
#include "data_traits.h"
#include "storage_backend.h"

#include <whiteout/interfaces.h>

namespace whiteout::storages::casc {

/// Concrete backend that combines a DataTraits policy (LocalDataTraits or
/// OnlineDataTraits) with a CacheTraits policy (MemCacheEnabled or
/// NoCachePolicy).  All DataTraits/CacheTraits calls are devirtualised
/// because the concrete types are known at compile time.
template <typename DataTraits, typename CacheTraits>
class StorageBackendImpl final : public StorageBackend {
public:
    StorageBackendImpl(DataTraits data, CacheTraits cache, const EncodingTable& encoding,
                       const KeyRing& keyRing, interfaces::WorkerPool* pool)
        : m_data(std::move(data)), m_cache(std::move(cache)), m_encoding(encoding),
          m_keyRing(keyRing), m_pool(pool) {}

    // ── Resolution ───────────────────────────────────────────────

    std::vector<u8> resolveCKey(std::span<const u8, 16> cKey,
                                interfaces::WorkerPool* pool = nullptr) const override;

    std::vector<u8> resolveEKey(std::span<const u8, 16> eKey,
                                interfaces::WorkerPool* pool = nullptr) const override;

    std::optional<std::vector<u8>> resolveRootEntry(const std::vector<const RootEntry*>& entries,
                                                    u32 localeFlags) const override;

    // ── Batch ────────────────────────────────────────────────────

    void resolveBatch(std::span<ResolveWork> work, std::span<ResolvedBlob> blobs,
                      interfaces::WorkerPool* pool) const override;

    // ── Data access ──────────────────────────────────────────────

    std::optional<IndexLocation> findInIndex(std::span<const u8> eKeyPrefix) const override {
        return m_data.findInIndex(eKeyPrefix);
    }

    std::vector<u8> fetchBlte(const IndexLocation& loc) const override {
        return m_data.fetchBlte(loc);
    }

    std::vector<u8> fetchBlte(const std::array<u8, 16>& eKey) const override {
        return m_data.fetchBlte(eKey);
    }

    // ── VFS prefetch ─────────────────────────────────────────────

    std::unordered_map<u64, std::vector<u8>> prefetchVfs(
        const Storage::Impl& impl, const std::vector<std::array<u8, 16>>& vfsEKeys,
        const std::unordered_map<u64, std::array<u8, 16>>& vfsEKeyToCKey) const override {
        return m_data.prefetchVfs(impl, vfsEKeys, vfsEKeyToCKey);
    }

    // ── Cache management ─────────────────────────────────────────

    void flushCache() override {
        m_cache.flush();
    }
    bool hasCache() const noexcept override {
        return CacheTraits::hasCache();
    }

    // ── Queries ──────────────────────────────────────────────────

    bool isLocal() const noexcept override {
        return DataTraits::isLocal();
    }
    bool isOnline() const noexcept override {
        return DataTraits::isOnline();
    }

private:
    DataTraits m_data;
    mutable CacheTraits m_cache;
    const EncodingTable& m_encoding;
    const KeyRing& m_keyRing;
    interfaces::WorkerPool* m_pool;
};

} // namespace whiteout::storages::casc
