// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
/// @file index.h
/// @brief CASC .idx file parser — maps EKey prefixes to archive offsets.
///
/// Internal header — not part of the public include path.
#pragma once

#include <whiteout/common_types.h>
#include <whiteout/interfaces.h>

#include <array>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "../progress_sink.h"
#include "flat_hash_map.h"

namespace whiteout::storages::casc {

struct IndexEntry {
    std::array<u8, 16> eKey{}; ///< Full or prefix EKey (only first EntrySizeEKey bytes valid).
    u32 archiveIndex = 0;      ///< Archive number (from shmem/CDN config).
    u32 archiveOffset = 0;     ///< Byte offset within data.XXX.
    u32 encodedSize = 0;       ///< BLTE-encoded size.
    bool directBLTE = false;   ///< True for .index entries: offset points directly to BLTE data.
};

class IndexTable {
public:
    IndexTable();
    ~IndexTable();
    IndexTable(IndexTable&&) noexcept;
    IndexTable& operator=(IndexTable&&) noexcept;
    IndexTable(const IndexTable&) = delete;
    IndexTable& operator=(const IndexTable&) = delete;

    /// Eagerly parse every .idx in the Data dir.
    /// @param sink Optional per-file progress; null for no reporting.
    static IndexTable load(const std::string& dataDir, interfaces::WorkerPool* pool = nullptr,
                           const ProgressSink* sink = nullptr);

    /// Discover .idx files but defer parsing each bucket until find() needs it.
    static IndexTable loadLazyBuckets(const std::string& dataDir,
                                      interfaces::WorkerPool* pool = nullptr,
                                      const ProgressSink* sink = nullptr);

    void ensureAllBucketsLoaded() const;

    /// True if header parsed (eager populated entries or lazy state ready).
    /// Distinct from entryCount()==0 — lazy tables start at 0.
    bool isValid() const;

    void loadArchiveIndices(const std::string& dataDir,
                            const std::vector<std::array<u8, 16>>& archiveEKeys,
                            interfaces::WorkerPool* pool = nullptr);

    /// Lazy variant — archives parsed on first find() miss.
    void loadArchiveIndicesLazy(const std::string& dataDir,
                                const std::vector<std::array<u8, 16>>& archiveEKeys,
                                interfaces::WorkerPool* pool = nullptr);

    void ensureAllArchivesLoaded() const;

    /// Matches first 9 bytes of eKeyPrefix.
    const IndexEntry* find(std::span<const u8> eKeyPrefix) const;

    /// Warm the bucket a later find of @p eKeyPrefix will probe.
    void prefetch(std::span<const u8> eKeyPrefix) const;

    void insert(const IndexEntry& entry);
    std::vector<std::pair<std::string, std::vector<u8>>> serialize() const;

    size_t entryCount() const;

private:
    static u64 eKeyHash(std::span<const u8> eKey);

    void loadArchive(u32 archiveIdx) const;
    void loadBucket(u8 bucket) const;

    FlatHashMap<IndexEntry> m_entries;

    struct LazyArchives;
    mutable std::unique_ptr<LazyArchives> m_lazyArchives;

    struct LazyBuckets;
    mutable std::unique_ptr<LazyBuckets> m_lazyBuckets;
};

} // namespace whiteout::storages::casc
