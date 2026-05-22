// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
/// @file data_traits.h
/// @brief Policy types for local and online data access in StorageBackendImpl.
///
/// Each DataTraits wraps a concrete DataSource + IndexTable, exposing
/// non-virtual member functions that the template calls directly.
/// The compiler devirtualises/inlines these since the type is known.
///
/// Internal header — not part of the public include path.
#pragma once

#include "../cdn/online_data_source.h"
#include "../cdn/online_index.h"
#include "../tables/index.h"
#include "data_source.h"
#include "key_utils.h"
#include "local_data_source.h"
#include "storage_backend.h"

#include <whiteout/common_types.h>
#include <whiteout/interfaces.h>

#include <array>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace whiteout::storages::casc {

// ============================================================================
// LocalDataTraits
// ============================================================================

struct LocalDataTraits {
    const IndexTable* indexTable = nullptr;
    LocalDataSource* dataSource = nullptr;

    static constexpr bool isLocal() noexcept {
        return true;
    }
    static constexpr bool isOnline() noexcept {
        return false;
    }

    std::optional<IndexLocation> findInIndex(std::span<const u8> eKeyPrefix) const {
        return dataSource->findInIndex(eKeyPrefix);
    }

    std::vector<u8> fetchBlte(const IndexLocation& loc) const {
        return dataSource->fetchBlte(loc);
    }

    std::vector<u8> fetchBlte(const std::array<u8, 16>& eKey) const {
        return dataSource->fetchBlte(eKey);
    }

    /// Zero-copy read from memory-mapped archive.
    std::span<const u8> readBlteFromIndex(const IndexEntry& entry) const {
        return dataSource->readBlteFromIndex(entry);
    }

    /// Batch Phase 1: parallel mmap reads via WorkerPool.
    void resolveBatchPhase1(const EncodingTable& encoding, std::span<ResolveWork> work,
                            std::span<ResolvedBlob> blobs, interfaces::WorkerPool* pool) const;

    /// VFS prefetch dispatch (local path).
    std::unordered_map<u64, std::vector<u8>> prefetchVfs(
        const Storage::Impl& impl, const std::vector<std::array<u8, 16>>& vfsEKeys,
        const std::unordered_map<u64, std::array<u8, 16>>& vfsEKeyToCKey) const;
};

// ============================================================================
// OnlineDataTraits
// ============================================================================

struct OnlineDataTraits {
    OnlineDataSource* dataSource = nullptr;
    const OnlineIndexTable* onlineIndex = nullptr;

    static constexpr bool isLocal() noexcept {
        return false;
    }
    static constexpr bool isOnline() noexcept {
        return true;
    }

    std::optional<IndexLocation> findInIndex(std::span<const u8> eKeyPrefix) const {
        return dataSource->findInIndex(eKeyPrefix);
    }

    std::vector<u8> fetchBlte(const IndexLocation& loc) const {
        // Use base-class dispatch to avoid name hiding in OnlineDataSource.
        return static_cast<DataSource*>(dataSource)->fetchBlte(loc);
    }

    std::vector<u8> fetchBlte(const std::array<u8, 16>& eKey) const {
        return static_cast<DataSource*>(dataSource)->fetchBlte(eKey);
    }

    /// Batch Phase 1: async HTTP fetch + WaitState.
    void resolveBatchPhase1(const EncodingTable& encoding, std::span<ResolveWork> work,
                            std::span<ResolvedBlob> blobs, interfaces::WorkerPool* pool) const;

    /// VFS prefetch dispatch (online path).
    std::unordered_map<u64, std::vector<u8>> prefetchVfs(
        const Storage::Impl& impl, const std::vector<std::array<u8, 16>>& vfsEKeys,
        const std::unordered_map<u64, std::array<u8, 16>>& vfsEKeyToCKey) const;
};

} // namespace whiteout::storages::casc
