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
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace whiteout::storages::casc {

/// Online (CDN) index table — maps EKey prefixes to archive locations.
class OnlineIndexTable {
public:
    struct Entry {
        u32 archiveIndex;   ///< Index into CdnConfig.archiveEKeys.
        u64 archiveOffset;  ///< Byte offset within the CDN archive.
        u32 encodedSize;    ///< BLTE-encoded size.
    };

    /// Parse a single CDN-format .index file.
    /// @param data          Raw .index file contents.
    /// @param archiveIndex  Index into CdnConfig.archiveEKeys for this archive.
    /// @return Parsed table (may be empty on parse error).
    static OnlineIndexTable parse(std::span<const u8> data, u32 archiveIndex);

    /// Load all archive indices from CDN via fetcher (synchronous).
    /// Uses WorkerPool for parallel fetching if available.
    static OnlineIndexTable loadAll(CdnFetcher& fetcher,
                                    const std::vector<std::array<u8, 16>>& archiveEKeys,
                                    interfaces::WorkerPool* pool = nullptr);

    /// Load loose-file index from CDN.
    static OnlineIndexTable loadLoose(CdnFetcher& fetcher,
                                      const std::string& fileIndexKey);

    /// Lookup by EKey prefix (first 9 bytes).
    const Entry* find(std::span<const u8> eKeyPrefix) const;

    /// Merge entries from another table into this one.
    void merge(const OnlineIndexTable& other);

    size_t entryCount() const { return m_entries.size(); }

private:
    static u64 eKeyHash(std::span<const u8> eKey);

    std::unordered_map<u64, Entry> m_entries;
};

} // namespace whiteout::storages::casc
