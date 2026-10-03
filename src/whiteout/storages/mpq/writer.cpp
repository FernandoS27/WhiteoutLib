// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "codecs/compression.h"
#include "crypto.h"
#include "tables/het_bet.h"
#include "writer.h"

#include "../../common/checksum.h"
#include "../common/jenkins.h"
#include "../common/md5.h"

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

/// Header size for @p formatVersion (0 = V1 ... 3 = V4).
u32 headerSizeFor(u16 formatVersion) {
    switch (formatVersion) {
    case 1:
        return 44;
    case 2:
        return 68;
    case 3:
        return 208;
    default:
        return 32;
    }
}

/// MD5 of each @p chunkSize piece of @p data, concatenated: what a V4 archive
/// stores after every file and extended table it holds.
std::vector<u8> rawChunkMd5s(std::span<const u8> data, u32 chunkSize) {
    std::vector<u8> out;
    for (size_t at = 0; at < data.size(); at += chunkSize) {
        auto const digest = common::md5Hash(data.subspan(at, std::min<size_t>(chunkSize, data.size() - at)));
        out.insert(out.end(), digest.begin(), digest.end());
    }
    return out;
}

} // anonymous namespace

WriteResult writeArchive(const WritePlan& plan, const std::vector<WriteEntry>& entries,
                         interfaces::WorkerPool* pool) {
    WriteResult result;

    // Every offset, size and MD5 is the writer's to state; the template only
    // says which version, sector size and chunk size.
    MpqHeader hdr{};
    hdr.formatVersion = plan.header.formatVersion;
    hdr.sectorSizeShift = plan.header.sectorSizeShift;
    hdr.headerSize = std::max(plan.header.headerSize, headerSizeFor(hdr.formatVersion));
    hdr.rawChunkSize = hdr.formatVersion >= 3 ? plan.header.rawChunkSize : 0;
    u32 const rawChunk = hdr.rawChunkSize;

    u32 const specials = plan.attributes ? 2u : 1u;
    u32 const needed = static_cast<u32>(entries.size()) + specials;
    u32 capacity = nextPowerOf2(std::max(plan.hashTableCapacity, 1u));
    if (capacity < needed)
        capacity = nextPowerOf2(needed + needed / 3);
    bool const keepSlots =
        plan.sourceHashTable != nullptr && plan.sourceHashTable->capacity() == capacity;

    // A nameless file can neither be hashed into a resized table nor given a
    // HET entry: refuse rather than write an archive that loses it.
    for (const auto& entry : entries) {
        if (!entry.filename.empty())
            continue;
        if (!keepSlots || !entry.sourceSlot) {
            result.error = "the hash table has to grow to " + std::to_string(capacity) +
                           " slots, and the archive holds files no (listfile) names";
            return result;
        }
        if (plan.hetBet && entry.nameHash == 0) {
            result.error = "the archive holds files neither its (listfile) nor its HET table names";
            return result;
        }
    }

    std::vector<u8> archive;
    archive.reserve(1024 * 1024);
    archive.resize(hdr.headerSize, 0);

    std::vector<BlockEntry> blocks;
    blocks.reserve(needed);
    FileAttributes attrs;
    u64 const now = currentFiletime();

    // Appends one file's stored bytes and, in a V4 archive, its chunk MD5s;
    // returns where they start.
    auto appendStored = [&](std::span<const u8> stored) {
        u32 const offset = static_cast<u32>(archive.size());
        archive.insert(archive.end(), stored.begin(), stored.end());
        if (rawChunk != 0) {
            auto const md5s = rawChunkMd5s(stored, rawChunk);
            archive.insert(archive.end(), md5s.begin(), md5s.end());
        }
        return offset;
    };
    auto recordAttributes = [&](std::optional<u32> crc, std::optional<u64> time,
                                std::optional<std::array<u8, 16>> md5, std::span<const u8> plain,
                                bool hasPlain) {
        if (hasPlain && !crc)
            crc = crc32(plain.data(), plain.size());
        // An empty file's MD5 is left zero, as the StarCraft II editor leaves it.
        if (hasPlain && !md5 && !plain.empty())
            md5 = common::md5Hash(plain);
        attrs.crc32s.push_back(crc.value_or(0));
        attrs.filetimes.push_back(time.value_or(now));
        attrs.md5s.push_back(md5.value_or(std::array<u8, 16>{}));
    };

    auto makeEncodeOpts = [&](const WriteEntry& entry) {
        EncodeOptions opts;
        opts.compression = entry.compression;
        opts.encrypt = entry.encrypt;
        opts.singleUnit = entry.singleUnit;
        opts.sectorSize = hdr.sectorSize();
        opts.filename = entry.filename;
        return opts;
    };

    // Pre-encode the new files (parallel when a pool is available).
    std::vector<EncodedFile> encodedFiles(entries.size());
    size_t overlayCount = 0;
    for (const auto& entry : entries)
        overlayCount += entry.rawCopy ? 0 : 1;
    if (pool && pool->threadCount() > 0 && overlayCount >= 2) {
        std::vector<std::pair<std::span<const u8>, EncodeOptions>> batchItems;
        std::vector<size_t> batchToEntry;
        batchItems.reserve(overlayCount);
        batchToEntry.reserve(overlayCount);
        for (size_t idx = 0; idx < entries.size(); ++idx) {
            if (entries[idx].rawCopy)
                continue;
            batchItems.emplace_back(std::span<const u8>(entries[idx].rawData), makeEncodeOpts(entries[idx]));
            batchToEntry.push_back(idx);
        }
        auto batchResult = encodeBatch(batchItems, pool);
        for (size_t b = 0; b < batchResult.files.size(); ++b)
            encodedFiles[batchToEntry[b]] = std::move(batchResult.files[b]);
    } else {
        for (size_t idx = 0; idx < entries.size(); ++idx) {
            if (!entries[idx].rawCopy)
                encodedFiles[idx] = encodeFileData(std::span<const u8>(entries[idx].rawData),
                                                   makeEncodeOpts(entries[idx]), pool);
        }
    }

    // File data, in entry order.
    for (size_t idx = 0; idx < entries.size(); ++idx) {
        const WriteEntry& entry = entries[idx];
        if (entry.rawCopy && entry.rekeyBase) {
            // Keyed to its offset, which this write changes.
            const BlockEntry& source = entry.sourceBlock;
            u32 const size = source.uncompressedSize;
            u32 const newOffset = static_cast<u32>(archive.size());
            auto const rekeyed =
                rekeyStoredFile(entry.rawSectors, source, hdr.sectorSize(), (*entry.rekeyBase + source.fileOffset) ^ size,
                                (*entry.rekeyBase + newOffset) ^ size);
            if (!rekeyed) {
                result.error = "the sector table of '" + (entry.filename.empty() ? std::string("a file") : entry.filename) +
                               "' does not decrypt, so it cannot be moved";
                return result;
            }
            blocks.push_back(makeRawCopyBlockEntry(appendStored(*rekeyed), source));
            recordAttributes(entry.crc32, entry.filetime, entry.md5, {}, false);
            continue;
        }
        if (entry.rawCopy) {
            blocks.push_back(makeRawCopyBlockEntry(appendStored(entry.rawSectors), entry.sourceBlock));
            recordAttributes(entry.crc32, entry.filetime, entry.md5, {}, false);
            continue;
        }
        EncodedFile& encoded = encodedFiles[idx];
        if (encoded.data.empty() && !entry.rawData.empty()) {
            encoded.data = entry.rawData;
            encoded.compressedSize = static_cast<u32>(entry.rawData.size());
            encoded.flags = FileFlag::kExists;
        }
        if (entry.rawData.empty())
            encoded.flags = FileFlag::kExists;
        blocks.push_back(makeBlockEntry(appendStored(encoded.data), encoded,
                                        static_cast<u32>(entry.rawData.size())));
        recordAttributes(entry.crc32, entry.filetime, entry.md5, entry.rawData, true);
    }

    // (listfile) names every named file once, and not the special files.
    std::vector<size_t> listed(entries.size());
    for (size_t idx = 0; idx < entries.size(); ++idx)
        listed[idx] = idx;
    std::stable_sort(listed.begin(), listed.end(),
                     [&](size_t a, size_t b) { return entries[a].listRank < entries[b].listRank; });
    std::vector<std::string> listfileNames;
    {
        std::vector<std::string> seen;
        for (size_t const idx : listed) {
            const WriteEntry& entry = entries[idx];
            if (entry.filename.empty())
                continue;
            std::string norm = common::normalizePath(entry.filename);
            if (std::find(seen.begin(), seen.end(), norm) != seen.end())
                continue;
            seen.push_back(std::move(norm));
            listfileNames.push_back(entry.filename);
        }
    }
    auto appendSpecialFile = [&](const std::string& name, const std::vector<u8>& rawData) {
        EncodeOptions opts;
        opts.compression = CompressionFlag::kZlib;
        opts.sectorSize = hdr.sectorSize();
        opts.filename = name;
        opts.singleUnit = plan.singleUnitSpecials;
        auto encoded = encodeFileData(std::span<const u8>(rawData), opts);
        if (encoded.data.empty() && !rawData.empty()) {
            encoded.data = rawData;
            encoded.compressedSize = static_cast<u32>(encoded.data.size());
            encoded.flags = FileFlag::kExists;
        }
        if (rawData.empty())
            encoded.flags = FileFlag::kExists;
        blocks.push_back(makeBlockEntry(appendStored(encoded.data), encoded, static_cast<u32>(rawData.size())));
    };
    auto const listfile = buildListfile(listfileNames);
    appendSpecialFile("(listfile)", listfile);
    recordAttributes(std::nullopt, now, std::nullopt, listfile, true);
    if (plan.attributes) {
        // Its own entry is zero; the arrays cover every block, itself included.
        recordAttributes(0u, now, std::array<u8, 16>{}, {}, false);
        FileAttributes planned;
        if (hasFlag(plan.attributeFlags, AttributeFlag::kCrc32))
            planned.crc32s = attrs.crc32s;
        if (hasFlag(plan.attributeFlags, AttributeFlag::kFiletime))
            planned.filetimes = attrs.filetimes;
        if (hasFlag(plan.attributeFlags, AttributeFlag::kMd5))
            planned.md5s = attrs.md5s;
        appendSpecialFile("(attributes)", buildAttributes(planned));
    }

    // HET and BET, each followed by its chunk MD5s like a file.
    if (plan.hetBet) {
        std::vector<u64> nameHashes;
        nameHashes.reserve(blocks.size());
        for (const auto& entry : entries)
            nameHashes.push_back(entry.filename.empty() ? entry.nameHash : hetNameHash(entry.filename));
        nameHashes.push_back(hetNameHash("(listfile)"));
        if (plan.attributes)
            nameHashes.push_back(hetNameHash("(attributes)"));
        std::vector<u32> fillOrder;
        fillOrder.reserve(nameHashes.size());
        for (size_t const idx : listed)
            fillOrder.push_back(static_cast<u32>(idx));
        for (u32 block = static_cast<u32>(entries.size()); block < nameHashes.size(); ++block)
            fillOrder.push_back(block);

        auto const het = buildHetTable(nameHashes, fillOrder);
        hdr.hetTableOffset = appendStored(het);
        hdr.hetTableSize64 = het.size();
        hdr.hetTableMd5 = common::md5Hash(het);
        auto const bet = buildBetTable(blocks, nameHashes);
        hdr.betTableOffset = appendStored(bet);
        hdr.betTableSize64 = bet.size();
        hdr.betTableMd5 = common::md5Hash(bet);
    }

    // Hash table: the source's slots where it keeps its size, then every
    // other file probed in by name.
    HashTable hashTable;
    hashTable.createEmpty(capacity);
    if (keepSlots) {
        for (u32 slot = 0; slot < capacity; ++slot) {
            if (plan.sourceHashTable->entry(slot).isEmpty())
                continue;
            HashEntry deleted;
            deleted.blockIndex = kHashEntryDeleted;
            hashTable.entry(slot) = deleted;
        }
        for (size_t idx = 0; idx < entries.size(); ++idx) {
            const WriteEntry& entry = entries[idx];
            if (!entry.sourceSlot)
                continue;
            HashEntry& e = hashTable.entry(*entry.sourceSlot);
            e.hashA = entry.hashA;
            e.hashB = entry.hashB;
            e.locale = entry.locale;
            e.platform = entry.platform;
            e.blockIndex = static_cast<u32>(idx);
        }
    }
    for (size_t idx = 0; idx < entries.size(); ++idx) {
        const WriteEntry& entry = entries[idx];
        if (keepSlots && entry.sourceSlot)
            continue;
        if (!hashTable.insert(entry.filename, entry.locale, static_cast<u32>(idx))) {
            result.error = "the hash table is full";
            return result;
        }
    }
    u32 const listfileBlock = static_cast<u32>(entries.size());
    if (!hashTable.insert("(listfile)", 0, listfileBlock) ||
        (plan.attributes && !hashTable.insert("(attributes)", 0, listfileBlock + 1))) {
        result.error = "the hash table is full";
        return result;
    }

    BlockTable blockTable;
    blockTable.createEmpty();
    for (const BlockEntry& be : blocks)
        (void)blockTable.append(be);

    auto const hashTableData = hashTable.serialize();
    hdr.hashTableOffset = static_cast<u32>(archive.size());
    hdr.hashTableEntries = capacity;
    hdr.hashTableSize64 = hashTableData.size();
    hdr.hashTableMd5 = common::md5Hash(hashTableData);
    archive.insert(archive.end(), hashTableData.begin(), hashTableData.end());

    auto const blockTableData = blockTable.serialize();
    hdr.blockTableOffset = static_cast<u32>(archive.size());
    hdr.blockTableEntries = blockTable.count();
    hdr.blockTableSize64 = blockTableData.size();
    hdr.blockTableMd5 = common::md5Hash(blockTableData);
    archive.insert(archive.end(), blockTableData.begin(), blockTableData.end());

    if (hdr.formatVersion >= 1 && blockTable.needsHiBlockTable()) {
        auto const hiData = blockTable.serializeHiBlockTable();
        hdr.hiBlockTableOffset = archive.size();
        hdr.hiBlockTableSize64 = hiData.size();
        hdr.hiBlockTableMd5 = common::md5Hash(hiData);
        archive.insert(archive.end(), hiData.begin(), hiData.end());
    }

    hdr.archiveSize = static_cast<u32>(archive.size());
    if (hdr.formatVersion >= 2)
        hdr.archiveSize64 = archive.size();
    serializeHeader(archive, hdr);
    // V4: the header's own MD5 covers everything in it before that field.
    if (hdr.headerSize >= 208) {
        static constexpr size_t kMpqHeaderMd5 = 192;
        auto const digest = common::md5Hash(std::span<const u8>(archive.data(), kMpqHeaderMd5));
        std::memcpy(archive.data() + kMpqHeaderMd5, digest.data(), digest.size());
    }

    result.archive = std::move(archive);
    return result;
}

std::vector<u8> writeArchive(const MpqHeader& header, const std::vector<WriteEntry>& entries,
                             u32 hashTableCapacity, interfaces::WorkerPool* pool) {
    WritePlan plan;
    plan.header = header;
    plan.hashTableCapacity = hashTableCapacity;
    return writeArchive(plan, entries, pool).archive;
}

} // namespace whiteout::storages::mpq
