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
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace whiteout::storages::casc {

struct IndexEntry {
    std::array<u8, 16> eKey{};  ///< Full or prefix EKey (only first EntrySizeEKey bytes valid).
    u32 archiveIndex = 0;       ///< Archive number (from shmem/CDN config).
    u32 archiveOffset = 0;      ///< Byte offset within data.XXX.
    u32 encodedSize = 0;        ///< BLTE-encoded size.
    bool directBLTE = false;    ///< True for .index entries: offset points directly to BLTE data.
};

class IndexTable {
public:
    /// Load from scanning .idx files in a data directory.
    /// @param dataDir Path to the CASC Data directory containing .idx files.
    /// @param pool    Optional worker pool for parallel .idx parsing.
    static IndexTable load(const std::string& dataDir,
                           interfaces::WorkerPool* pool = nullptr);

    /// Load per-archive .index files from the indices/ subdirectory.
    /// @param dataDir      Path to the CASC Data directory.
    /// @param archiveEKeys Ordered list of archive EKeys from the CDN config.
    /// @param pool         Optional worker pool for parallel parsing.
    void loadArchiveIndices(const std::string& dataDir,
                            const std::vector<std::array<u8, 16>>& archiveEKeys,
                            interfaces::WorkerPool* pool = nullptr);

    /// Lookup by EKey (prefix match using first 9 bytes).
    const IndexEntry* find(std::span<const u8> eKeyPrefix) const;

    /// For write path: add an entry.
    void insert(const IndexEntry& entry);

    /// Serialize all entries back to .idx file contents.
    /// Returns pairs of (filename, data).
    std::vector<std::pair<std::string, std::vector<u8>>> serialize() const;

    size_t entryCount() const { return m_entries.size(); }

private:
    /// Hash key derived from first 9 bytes of EKey.
    static u64 eKeyHash(std::span<const u8> eKey);

    std::unordered_map<u64, IndexEntry> m_entries;
};

} // namespace whiteout::storages::casc
