// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
/// @file online_index.h
/// @brief CDN-format .index parser — maps EKey prefixes to archive offsets.
///
/// CDN index files use a footer-based format (TACT specification):
///   [4 KB data blocks of index entries]
///   [Table of contents: last EKey per block + block checksums]
///   [Footer: format info + total entry count + checksum]
///
/// Internal header — not part of the public include path.
#pragma once

#include <whiteout/common_types.h>
#include <whiteout/interfaces.h>

#include "cdn_fetcher.h"

#include <array>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace whiteout::storages::casc {

/// Maps EKey prefixes to (archive, offset, size). Eager via parse/loadAll/
/// loadLoose, or lazy via makeLazy (faults in one .index file per missed
/// archive). Lazy form is thread-safe.
class OnlineIndexTable {
public:
    struct Entry {
        u32 archiveIndex;
        u64 archiveOffset;
        u32 encodedSize;
    };

    OnlineIndexTable();
    ~OnlineIndexTable();
    OnlineIndexTable(OnlineIndexTable&&) noexcept;
    OnlineIndexTable& operator=(OnlineIndexTable&&) noexcept;
    OnlineIndexTable(const OnlineIndexTable&) = delete;
    OnlineIndexTable& operator=(const OnlineIndexTable&) = delete;

    static OnlineIndexTable parse(std::span<const u8> data, u32 archiveIndex);
    static OnlineIndexTable loadAll(CdnFetcher& fetcher,
                                    const std::vector<std::array<u8, 16>>& archiveEKeys,
                                    interfaces::WorkerPool* pool = nullptr);
    static OnlineIndexTable loadLoose(CdnFetcher& fetcher,
                                      const std::string& fileIndexKey);

    /// Lazy variant — pointer args must outlive the table.
    static OnlineIndexTable makeLazy(CdnFetcher* fetcher,
                                     const std::vector<std::array<u8, 16>>* archiveEKeys,
                                     interfaces::WorkerPool* pool = nullptr);

    /// First 9 bytes of eKeyPrefix are matched. Lazy mode may block on CDN.
    /// Returned pointer is stable across subsequent inserts.
    const Entry* find(std::span<const u8> eKeyPrefix) const;

    void merge(const OnlineIndexTable& other);
    size_t entryCount() const;

    /// Force every pending archive index to load. No-op for eager tables.
    void ensureAllLoaded() const;

private:
    static u64 eKeyHash(std::span<const u8> eKey);
    void loadArchive(u32 archiveIndex) const;

    mutable std::unordered_map<u64, Entry> m_entries;

    struct LazyState;
    mutable std::unique_ptr<LazyState> m_lazy;
};

} // namespace whiteout::storages::casc
