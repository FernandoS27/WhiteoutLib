// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
/// @file local_data_source.h
/// @brief DataSource implementation for local memory-mapped CASC archives.
///
/// Internal header — not part of the public include path.
#pragma once

#include "../../common/mapped_file.h"
#include "../tables/index.h"
#include "constants.h"
#include "data_source.h"
#include "key_utils.h"

#include <cstring>

namespace whiteout::storages::casc {

/// Fetches BLTE blobs from local memory-mapped data archives.
class LocalDataSource : public DataSource {
public:
    LocalDataSource(const IndexTable* index,
                    const std::vector<storages::common::MappedFile>* archives)
        : m_index(index), m_archives(archives) {}

    std::optional<IndexLocation> findInIndex(std::span<const u8> eKeyPrefix) const override {
        auto entry = m_index->find(eKeyPrefix);
        if (!entry)
            return std::nullopt;
        return IndexLocation{
            entry->archiveIndex,
            entry->archiveOffset,
            entry->encodedSize,
            entry->directBLTE,
        };
    }

    std::vector<u8> fetchBlte(const std::array<u8, 16>& eKey) override {
        auto entry = m_index->find(std::span<const u8>(eKey.data(), kEKeyTruncSize));
        if (!entry)
            return {};
        auto span = readBlteFromIndex(*entry);
        return {span.begin(), span.end()};
    }

    std::vector<u8> fetchBlte(u32 archiveIndex, u64 offset, u32 encodedSize) override {
        // Auto-detect header mode: try with 30-byte header first, then direct.
        auto span = readRawBlte(archiveIndex, static_cast<u32>(offset), encodedSize);
        if (!span.empty() && span.size() >= 4 && std::memcmp(span.data(), kBlteMagic, 4) == 0)
            return {span.begin(), span.end()};

        auto direct = readRawBlteDirect(archiveIndex, static_cast<u32>(offset), encodedSize);
        if (!direct.empty() && direct.size() >= 4 && std::memcmp(direct.data(), kBlteMagic, 4) == 0)
            return {direct.begin(), direct.end()};

        // Return whichever was non-empty (may be encrypted / non-BLTE).
        if (!direct.empty())
            return {direct.begin(), direct.end()};
        if (!span.empty())
            return {span.begin(), span.end()};
        return {};
    }

    std::vector<u8> fetchBlte(const IndexLocation& loc) override {
        // Reconstruct IndexEntry to use the directBLTE hint.
        IndexEntry idx{};
        idx.archiveIndex = loc.archiveIndex;
        idx.archiveOffset = static_cast<u32>(loc.offset);
        idx.encodedSize = loc.encodedSize;
        idx.directBLTE = loc.directBLTE;
        auto span = readBlteFromIndex(idx);
        return {span.begin(), span.end()};
    }

    // ── Zero-copy access (used by readBatch local path) ──────────

    /// Read raw BLTE data from an IndexEntry, with directBLTE auto-fallback.
    /// Returns a span into the memory-mapped archive (zero-copy).
    std::span<const u8> readBlteFromIndex(const IndexEntry& idx) const {
        auto blteData =
            idx.directBLTE ? readRawBlteDirect(idx.archiveIndex, idx.archiveOffset, idx.encodedSize)
                           : readRawBlte(idx.archiveIndex, idx.archiveOffset, idx.encodedSize);
        if (!blteData.empty() && blteData.size() >= 4 &&
            std::memcmp(blteData.data(), kBlteMagic, 4) != 0) {
            // BLTE magic wrong — retry with opposite mode.
            auto retry =
                idx.directBLTE
                    ? readRawBlte(idx.archiveIndex, idx.archiveOffset, idx.encodedSize)
                    : readRawBlteDirect(idx.archiveIndex, idx.archiveOffset, idx.encodedSize);
            if (!retry.empty())
                blteData = retry;
        }
        return blteData;
    }

    /// Read pre-encoded BLTE data for writer raw-copy path.
    std::span<const u8> readRawBlteDirect(u32 archiveIndex, u32 offset, u32 encodedSize) const {
        if (archiveIndex >= m_archives->size())
            return {};
        auto& archive = (*m_archives)[archiveIndex];
        if (!archive)
            return {};
        if (u64(offset) + encodedSize > archive.size())
            return {};
        return std::span<const u8>(archive.ptr() + offset, encodedSize);
    }

private:
    static constexpr u8 kBlteMagic[4] = {'B', 'L', 'T', 'E'};

    std::span<const u8> readRawBlte(u32 archiveIndex, u32 offset, u32 encodedSize) const {
        if (archiveIndex >= m_archives->size())
            return {};
        auto& archive = (*m_archives)[archiveIndex];
        if (!archive)
            return {};
        u64 dataOffset = u64(offset) + kArchiveEntryHeaderSize;
        if (encodedSize <= kArchiveEntryHeaderSize)
            return {};
        u32 dataSize = encodedSize - kArchiveEntryHeaderSize;
        if (dataOffset + dataSize > archive.size())
            return {};
        return std::span<const u8>(archive.ptr() + dataOffset, dataSize);
    }

    const IndexTable* m_index;
    const std::vector<storages::common::MappedFile>* m_archives;
};

} // namespace whiteout::storages::casc
