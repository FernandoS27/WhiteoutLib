// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "codecs/compression.h"
#include "crypto.h"
#include "writer.h"

#include <whiteout/interfaces.h>
#include <whiteout/utils/job_group.h>

#include <algorithm>
#include <chrono>
#include <cstring>

namespace whiteout::storages::mpq {

namespace {

/// Helper: recompute a block entry for a file that was freshly encoded.
BlockEntry makeBlockEntry(u32 offset, const EncodedFile& encoded, u32 uncompressedSize) {
    BlockEntry be;
    be.fileOffset = offset;
    be.compressedSize = encoded.compressedSize;
    be.uncompressedSize = uncompressedSize;
    be.flags = encoded.flags;
    return be;
}

/// Helper: recompute a block entry for a raw-copied file.
BlockEntry makeRawCopyBlockEntry(u32 newOffset, const BlockEntry& source) {
    BlockEntry be = source;
    be.fileOffset = newOffset;
    return be;
}

/// Get current FILETIME (Windows epoch: Jan 1, 1601).
u64 currentFiletime() {
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    auto hundred_ns =
        std::chrono::duration_cast<std::chrono::duration<u64, std::ratio<1, 10000000>>>(duration);
    constexpr u64 kWindowsEpochOffset = 116444736000000000ULL;
    return hundred_ns.count() + kWindowsEpochOffset;
}

/// Serialize the MpqHeader struct into the archive buffer starting at offset 0.
/// Supports V1 (32 bytes), V2 (44 bytes), V3 (68 bytes), and V4 (208 bytes).
void serializeHeader(std::vector<u8>& archive, const MpqHeader& hdr) {
    if (archive.size() < 32) {
        archive.resize(32, 0);
    }

    static constexpr size_t kMagic = 0;
    static constexpr size_t kHeaderSize = 4;
    static constexpr size_t kArchiveSize = 8;
    static constexpr size_t kFormatVersion = 12;
    static constexpr size_t kSectorSizeShift = 14;
    static constexpr size_t kHashTableOffset = 16;
    static constexpr size_t kBlockTableOffset = 20;
    static constexpr size_t kHashTableEntries = 24;
    static constexpr size_t kBlockTableEntries = 28;

    static constexpr size_t kHiBlockTableOffset = 32;
    static constexpr size_t kHashTableOffsetHi = 40;
    static constexpr size_t kBlockTableOffsetHi = 42;

    static constexpr size_t kArchiveSize64 = 44;
    static constexpr size_t kBetTableOffset = 52;
    static constexpr size_t kHetTableOffset = 60;

    static constexpr size_t kHashTableSize64 = 68;
    static constexpr size_t kBlockTableSize64 = 76;
    static constexpr size_t kHiBlockTableSize64 = 84;
    static constexpr size_t kHetTableSize64 = 92;
    static constexpr size_t kBetTableSize64 = 100;
    static constexpr size_t kRawChunkSize = 108;

    static constexpr size_t kBlockTableMd5 = 112;
    static constexpr size_t kHashTableMd5 = 128;
    static constexpr size_t kHiBlockTableMd5 = 144;
    static constexpr size_t kBetTableMd5 = 160;
    static constexpr size_t kHetTableMd5 = 176;
    static constexpr size_t kMpqHeaderMd5 = 192;

    auto w = [&](size_t off, const void* src, size_t len) {
        std::memcpy(archive.data() + off, src, len);
    };

    // V1 base — 32 bytes.
    w(kMagic, &hdr.magic, 4);
    w(kHeaderSize, &hdr.headerSize, 4);
    w(kArchiveSize, &hdr.archiveSize, 4);
    w(kFormatVersion, &hdr.formatVersion, 2);
    w(kSectorSizeShift, &hdr.sectorSizeShift, 2);
    w(kHashTableOffset, &hdr.hashTableOffset, 4);
    w(kBlockTableOffset, &hdr.blockTableOffset, 4);
    w(kHashTableEntries, &hdr.hashTableEntries, 4);
    w(kBlockTableEntries, &hdr.blockTableEntries, 4);

    // V2 extension — 44 bytes.
    if (hdr.headerSize >= 44) {
        w(kHiBlockTableOffset, &hdr.hiBlockTableOffset, 8);
        w(kHashTableOffsetHi, &hdr.hashTableOffsetHi, 2);
        w(kBlockTableOffsetHi, &hdr.blockTableOffsetHi, 2);
    }

    // V3 extension — 68 bytes.
    if (hdr.headerSize >= 68) {
        w(kArchiveSize64, &hdr.archiveSize64, 8);
        w(kBetTableOffset, &hdr.betTableOffset, 8);
        w(kHetTableOffset, &hdr.hetTableOffset, 8);
    }

    // V4 extension — 208 bytes.
    if (hdr.headerSize >= 208) {
        w(kHashTableSize64, &hdr.hashTableSize64, 8);
        w(kBlockTableSize64, &hdr.blockTableSize64, 8);
        w(kHiBlockTableSize64, &hdr.hiBlockTableSize64, 8);
        w(kHetTableSize64, &hdr.hetTableSize64, 8);
        w(kBetTableSize64, &hdr.betTableSize64, 8);
        w(kRawChunkSize, &hdr.rawChunkSize, 4);

        w(kBlockTableMd5, hdr.blockTableMd5.data(), hdr.blockTableMd5.size());
        w(kHashTableMd5, hdr.hashTableMd5.data(), hdr.hashTableMd5.size());
        w(kHiBlockTableMd5, hdr.hiBlockTableMd5.data(), hdr.hiBlockTableMd5.size());
        w(kBetTableMd5, hdr.betTableMd5.data(), hdr.betTableMd5.size());
        w(kHetTableMd5, hdr.hetTableMd5.data(), hdr.hetTableMd5.size());
        w(kMpqHeaderMd5, hdr.mpqHeaderMd5.data(), hdr.mpqHeaderMd5.size());
    }
}

} // anonymous namespace

std::vector<u8> writeArchive(const MpqHeader& header, const std::vector<WriteEntry>& entries,
                             u32 hashTableCapacity, interfaces::WorkerPool* pool) {
    // Ensure capacity is power of 2.
    u32 const htCapacity = nextPowerOf2(hashTableCapacity);

    // Build the archive in memory.
    std::vector<u8> archive;
    archive.reserve(1024 * 1024); // Initial reservation.

    // Step 1: Write header placeholder (will be updated at end).
    MpqHeader hdr = header;
    hdr.hashTableEntries = htCapacity;
    hdr.blockTableEntries = 0;
    if (hdr.headerSize < 32) {
        hdr.headerSize = 32;
    }

    size_t const headerSize = hdr.headerSize;
    archive.resize(headerSize, 0);

    // Step 2: Write file data.
    HashTable hashTable;
    hashTable.createEmpty(htCapacity);

    BlockTable blockTable;
    blockTable.createEmpty();

    // Also build (listfile) content and attributes.
    std::vector<std::string> listfileNames;
    FileAttributes attrs;

    // Count overlay entries (files that need encoding, not raw-copies).
    size_t overlayCount = 0;
    for (const auto& entry : entries) {
        if (entry.rawSectors.empty())
            ++overlayCount;
    }

    // --- Helper: append a single entry (raw-copy or pre-encoded) to the archive ---
    auto appendEntry = [&](const WriteEntry& entry, EncodedFile* encoded) {
        u32 const fileOffset = static_cast<u32>(archive.size());
        BlockEntry be;

        if (!entry.rawSectors.empty()) {
            archive.insert(archive.end(), entry.rawSectors.begin(), entry.rawSectors.end());
            be = makeRawCopyBlockEntry(fileOffset, entry.sourceBlock);
        } else {
            if (encoded->data.empty() && !entry.rawData.empty()) {
                encoded->data = entry.rawData;
                encoded->compressedSize = static_cast<u32>(entry.rawData.size());
                encoded->flags = FileFlag::kExists;
            }
            archive.insert(archive.end(), encoded->data.begin(), encoded->data.end());
            be = makeBlockEntry(fileOffset, *encoded, static_cast<u32>(entry.rawData.size()));
        }

        u32 const blockIndex = blockTable.append(be);
        (void)hashTable.insert(entry.filename, entry.locale, blockIndex);
        listfileNames.push_back(entry.filename);
    };

    // Pre-encode overlay entries (parallel when pool available).
    std::vector<EncodedFile> encodedFiles(entries.size());

    auto makeEncodeOpts = [&](const WriteEntry& entry) {
        EncodeOptions opts;
        opts.compression = entry.compression;
        opts.encrypt = entry.encrypt;
        opts.singleUnit = entry.singleUnit;
        opts.sectorSize = hdr.sectorSize();
        opts.filename = entry.filename;
        return opts;
    };

    if (pool && pool->threadCount() > 0 && overlayCount >= 2) {
        // Build batch items for all overlay entries.
        std::vector<std::pair<std::span<const u8>, EncodeOptions>> batchItems;
        std::vector<size_t> batchToEntry;
        batchItems.reserve(overlayCount);
        batchToEntry.reserve(overlayCount);

        for (size_t idx = 0; idx < entries.size(); ++idx) {
            if (!entries[idx].rawSectors.empty())
                continue;
            batchItems.emplace_back(std::span<const u8>(entries[idx].rawData),
                                    makeEncodeOpts(entries[idx]));
            batchToEntry.push_back(idx);
        }

        auto batchResult = encodeBatch(batchItems, pool);
        for (size_t b = 0; b < batchResult.files.size(); ++b) {
            encodedFiles[batchToEntry[b]] = std::move(batchResult.files[b]);
        }
    } else {
        for (size_t idx = 0; idx < entries.size(); ++idx) {
            const auto& entry = entries[idx];
            if (!entry.rawSectors.empty())
                continue;
            encodedFiles[idx] =
                encodeFileData(std::span<const u8>(entry.rawData), makeEncodeOpts(entry), pool);
        }
    }

    // Assemble archive entries serially.
    for (size_t idx = 0; idx < entries.size(); ++idx) {
        appendEntry(entries[idx], &encodedFiles[idx]);
    }

    // --- Helper: encode and append a special file (listfile, attributes) ---
    auto appendSpecialFile = [&](const std::string& name, std::vector<u8>& rawData) {
        u32 const uncompressedSize = static_cast<u32>(rawData.size());
        u32 const fileOffset = static_cast<u32>(archive.size());

        EncodeOptions opts;
        opts.compression = CompressionFlag::kZlib;
        opts.sectorSize = hdr.sectorSize();
        opts.filename = name;

        auto encoded = encodeFileData(std::span<const u8>(rawData), opts);
        if (encoded.data.empty() && !rawData.empty()) {
            encoded.data = std::move(rawData);
            encoded.compressedSize = static_cast<u32>(encoded.data.size());
            encoded.flags = FileFlag::kExists;
        }

        archive.insert(archive.end(), encoded.data.begin(), encoded.data.end());
        BlockEntry const be = makeBlockEntry(fileOffset, encoded, uncompressedSize);
        u32 const blockIndex = blockTable.append(be);
        (void)hashTable.insert(name, 0, blockIndex);
    };

    // Step 3: Write (listfile).
    listfileNames.push_back("(listfile)");
    listfileNames.push_back("(attributes)");
    auto listfileData = buildListfile(listfileNames);
    appendSpecialFile("(listfile)", listfileData);

    // Step 4: Write (attributes).
    attrs.crc32s.resize(blockTable.count(), 0);
    attrs.filetimes.resize(blockTable.count(), currentFiletime());
    attrs.md5s.resize(blockTable.count(), std::array<u8, 16>{});
    auto attrData = buildAttributes(attrs);
    appendSpecialFile("(attributes)", attrData);

    // Step 5: Write hash table.
    u32 const hashTableOffset = static_cast<u32>(archive.size());
    auto hashTableData = hashTable.serialize();
    archive.insert(archive.end(), hashTableData.begin(), hashTableData.end());

    // Step 6: Write block table.
    u32 const blockTableOffset = static_cast<u32>(archive.size());
    auto blockTableData = blockTable.serialize();
    archive.insert(archive.end(), blockTableData.begin(), blockTableData.end());

    // Hi-block table (V2+ if needed).
    if (hdr.formatVersion >= 1 && blockTable.needsHiBlockTable()) {
        auto hiData = blockTable.serializeHiBlockTable();
        hdr.hiBlockTableOffset = archive.size();
        archive.insert(archive.end(), hiData.begin(), hiData.end());
    }

    // Step 7: Update header with final values.
    hdr.hashTableOffset = hashTableOffset;
    hdr.blockTableOffset = blockTableOffset;
    hdr.blockTableEntries = blockTable.count();
    hdr.archiveSize = static_cast<u32>(archive.size());
    if (hdr.formatVersion >= 2) {
        hdr.archiveSize64 = archive.size();
    }

    // Write header at the start of the archive.
    serializeHeader(archive, hdr);

    return archive;
}

} // namespace whiteout::storages::mpq
