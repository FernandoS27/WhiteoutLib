// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
/// @file local_data_source.h
/// @brief DataSource implementation for local memory-mapped CASC archives.
///
/// Internal header — not part of the public include path.
#pragma once

#include "../../common/mapped_file.h"
#include "../tables/config.h"
#include "../tables/index.h"
#include "constants.h"
#include "data_source.h"
#include "key_utils.h"

#include <cstring>

namespace whiteout::storages::casc {

/// Fetches BLTE blobs from local memory-mapped data archives.
///
/// Two flavours of local install go through here. The usual one resolves an
/// EKey through the `.idx` buckets. A static-build-config install (the shape
/// Steam ships) has no index files at all: StaticKeyLayout says which trailing
/// EKey bytes name the archive and offset, so the lookup is arithmetic and the
/// encoded size is read back from the BLTE header found there.
class LocalDataSource : public DataSource {
public:
    LocalDataSource(const IndexTable* index,
                    const std::vector<storages::common::MappedFile>* archives,
                    StaticKeyLayout staticLayout = {})
        : m_index(index), m_archives(archives), m_static(staticLayout) {}

    std::optional<IndexLocation> findInIndex(std::span<const u8, 16> eKey) const override {
        if (m_static.valid)
            return decodeStaticKey(eKey);

        auto entry = m_index->find(eKeyTrunc(eKey));
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
        auto loc = findInIndex(std::span<const u8, 16>(eKey));
        if (!loc)
            return {};
        auto span = viewBlte(*loc);
        return {span.begin(), span.end()};
    }

    std::vector<u8> fetchBlte(u32 archiveIndex, u64 offset, u32 encodedSize) override {
        // Auto-detect header mode: try with 30-byte header first, then direct.
        auto span = readRawBlte(archiveIndex, offset, encodedSize);
        if (!span.empty() && span.size() >= 4 && std::memcmp(span.data(), kBlteMagic, 4) == 0)
            return {span.begin(), span.end()};

        auto direct = readRawBlteDirect(archiveIndex, offset, encodedSize);
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
        auto span = viewBlte(loc);
        return {span.begin(), span.end()};
    }

    // ── Zero-copy access (used by readBatch local path) ──────────

    /// Read raw BLTE data at @p loc, with directBLTE auto-fallback.
    /// Returns a span into the memory-mapped archive (zero-copy).
    std::span<const u8> viewBlte(const IndexLocation& loc) const {
        auto blteData = loc.directBLTE
                            ? readRawBlteDirect(loc.archiveIndex, loc.offset, loc.encodedSize)
                            : readRawBlte(loc.archiveIndex, loc.offset, loc.encodedSize);
        if (!blteData.empty() && blteData.size() >= 4 &&
            std::memcmp(blteData.data(), kBlteMagic, 4) != 0) {
            // BLTE magic wrong — retry with opposite mode.
            auto retry = loc.directBLTE
                             ? readRawBlte(loc.archiveIndex, loc.offset, loc.encodedSize)
                             : readRawBlteDirect(loc.archiveIndex, loc.offset, loc.encodedSize);
            if (!retry.empty())
                blteData = retry;
        }
        return blteData;
    }

    /// Read pre-encoded BLTE data for the writer's raw-copy path.
    /// @p encodedSize 0 means "as long as the BLTE header at @p offset says".
    std::span<const u8> readRawBlteDirect(u32 archiveIndex, u64 offset, u32 encodedSize) const {
        const storages::common::MappedFile* archive = archiveAt(archiveIndex);
        if (!archive)
            return {};
        if (encodedSize == 0) {
            encodedSize = blteFrameSize(*archive, offset);
            if (encodedSize == 0)
                return {};
        }
        if (offset + encodedSize > archive->size())
            return {};
        return std::span<const u8>(archive->ptr() + offset, encodedSize);
    }

private:
    static constexpr u8 kBlteMagic[4] = {'B', 'L', 'T', 'E'};

    const storages::common::MappedFile* archiveAt(u32 archiveIndex) const {
        if (archiveIndex >= m_archives->size())
            return nullptr;
        auto& archive = (*m_archives)[archiveIndex];
        return archive ? &archive : nullptr;
    }

    static u32 readBE32(const u8* p) {
        return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | u32(p[3]);
    }

    static u64 readBE(const u8* p, u32 bytes) {
        u64 v = 0;
        for (u32 i = 0; i < bytes; ++i)
            v = (v << 8) | p[i];
        return v;
    }

    /// Total encoded length of the BLTE frame at @p offset: its header plus
    /// every chunk the chunk table declares. 0 when there is no chunk table to
    /// read it from — such a frame carries no length, and handing back the rest
    /// of a multi-gigabyte archive instead would be worse than failing.
    static u32 blteFrameSize(const storages::common::MappedFile& archive, u64 offset) {
        if (offset + 12 > archive.size())
            return 0;
        const u8* p = archive.ptr() + offset;
        if (std::memcmp(p, kBlteMagic, 4) != 0)
            return 0;

        u32 const headerSize = readBE32(p + 4);
        u32 const chunkCount = readBE32(p + 8) & 0x00FFFFFFu;
        if (chunkCount == 0 || headerSize < 12 + chunkCount * 24u)
            return 0;
        if (offset + headerSize > archive.size())
            return 0;

        u64 total = headerSize;
        for (u32 i = 0; i < chunkCount; ++i)
            total += readBE32(p + 12 + i * 24u);
        if (total > archive.size() - offset || total > 0xFFFFFFFFu)
            return 0;
        return static_cast<u32>(total);
    }

    /// EKey → archive slot + offset, for static-build-config installs. Misses
    /// when the archive it names was never downloaded, which is the same "not
    /// here" answer the `.idx` path gives for a file this install skipped.
    std::optional<IndexLocation> decodeStaticKey(std::span<const u8, 16> eKey) const {
        const u8* p = eKey.data() + m_static.hashBytes;
        u64 const chunk = readBE(p, m_static.chunkBytes);
        u64 const uid = readBE(p + m_static.chunkBytes, m_static.uidBytes);
        u64 const offset =
            readBE(p + m_static.chunkBytes + m_static.uidBytes, m_static.offsetBytes);
        if (uid >= kStaticArchiveUidSpan || chunk > 0xFFFFFFFFu / kStaticArchiveUidSpan)
            return std::nullopt;

        IndexLocation loc{};
        loc.archiveIndex = staticArchiveSlot(static_cast<u32>(chunk), static_cast<u32>(uid));
        loc.offset = offset;
        loc.encodedSize = 0; // read back from the BLTE header
        loc.directBLTE = true;

        const storages::common::MappedFile* archive = archiveAt(loc.archiveIndex);
        if (!archive || offset + 12 > archive->size())
            return std::nullopt;
        return loc;
    }

    std::span<const u8> readRawBlte(u32 archiveIndex, u64 offset, u32 encodedSize) const {
        const storages::common::MappedFile* archive = archiveAt(archiveIndex);
        if (!archive)
            return {};
        u64 const dataOffset = offset + kArchiveEntryHeaderSize;
        if (encodedSize <= kArchiveEntryHeaderSize)
            return {};
        u32 const dataSize = encodedSize - kArchiveEntryHeaderSize;
        if (dataOffset + dataSize > archive->size())
            return {};
        return std::span<const u8>(archive->ptr() + dataOffset, dataSize);
    }

    const IndexTable* m_index;
    const std::vector<storages::common::MappedFile>* m_archives;
    StaticKeyLayout m_static;
};

} // namespace whiteout::storages::casc
