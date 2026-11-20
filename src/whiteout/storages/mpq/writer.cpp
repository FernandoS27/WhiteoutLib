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
#include <cctype>
#include <chrono>
#include <cstring>
#include <unordered_set>

namespace whiteout::storages::mpq {

namespace {

/// Helper: the block entry of a file that was freshly encoded (its offset is
/// set where it is placed).
BlockEntry makeBlockEntry(const EncodedFile& encoded, u32 uncompressedSize) {
    BlockEntry be;
    be.compressedSize = encoded.compressedSize;
    be.uncompressedSize = uncompressedSize;
    be.flags = encoded.flags;
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

/// The whole archive in memory.
class MemorySink final : public ArchiveSink {
public:
    std::vector<u8> bytes;

    [[nodiscard]] u64 size() const override {
        return bytes.size();
    }
    void append(std::span<const u8> data) override {
        bytes.insert(bytes.end(), data.begin(), data.end());
    }
    void patch(u64 offset, std::span<const u8> data) override {
        std::copy(data.begin(), data.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset));
    }
};

} // anonymous namespace

std::string writeArchive(const WritePlan& plan, const std::vector<WriteEntry>& entries, ArchiveSink& sink,
                         interfaces::WorkerPool* pool) {
    // Every offset, size and MD5 is the writer's to state; the template only
    // says which version, sector size and chunk size.
    MpqHeader hdr{};
    hdr.formatVersion = plan.header.formatVersion;
    hdr.sectorSizeShift = plan.header.sectorSizeShift;
    hdr.headerSize = std::max(plan.header.headerSize, headerSizeFor(hdr.formatVersion));
    hdr.rawChunkSize = hdr.formatVersion >= 3 ? plan.header.rawChunkSize : 0;
    u32 const rawChunk = hdr.rawChunkSize;
    if (hdr.sectorSize() == 0)
        return "sector size shift " + std::to_string(hdr.sectorSizeShift) + " leaves no sector size";

    u32 const specials = plan.attributes ? 2u : 1u;
    if (entries.size() + specials > kBlockIndexMask / 2)
        return "too many files for one archive";
    u32 const needed = static_cast<u32>(entries.size()) + specials;
    u32 capacity = nextPowerOf2(std::max(plan.hashTableCapacity, 1u));
    bool const allNamed =
        std::all_of(entries.begin(), entries.end(), [](const WriteEntry& entry) { return !entry.filename.empty(); });
    if (allNamed) {
        // Every file can be probed for by name, so the table is built anew:
        // no deleted markers, and at most three quarters full.
        u32 const roomy = (needed * 4 + 2) / 3;
        if (capacity < roomy)
            capacity = nextPowerOf2(roomy);
    } else if (capacity < needed) {
        capacity = nextPowerOf2(needed + needed / 3);
    }
    bool const keepSlots =
        !allNamed && plan.sourceHashTable != nullptr && plan.sourceHashTable->capacity() == capacity;

    // A nameless file can neither be hashed into a resized table nor given a
    // HET entry: refuse rather than write an archive that loses it.
    for (const auto& entry : entries) {
        if (!entry.filename.empty())
            continue;
        if (!keepSlots || !entry.sourceSlot)
            return "the hash table has to grow to " + std::to_string(capacity) +
                   " slots, and the archive holds files no (listfile) names";
        if (plan.hetBet && entry.nameHash == 0)
            return "the archive holds files neither its (listfile) nor its HET table names";
    }

    sink.append(std::vector<u8>(hdr.headerSize, 0)); // Patched in last.

    std::vector<BlockEntry> blocks;
    std::vector<u16> blockOffsetHi; // The high bits of each block's 48-bit offset.
    blocks.reserve(needed);
    blockOffsetHi.reserve(needed);
    auto placeBlock = [&](BlockEntry be, u64 offset) {
        be.fileOffset = static_cast<u32>(offset);
        blocks.push_back(be);
        blockOffsetHi.push_back(static_cast<u16>(offset >> 32));
    };
    FileAttributes attrs;
    u64 const now = currentFiletime();

    // Appends one file's stored bytes and, in a V4 archive, its chunk MD5s;
    // returns where they start. @p lasting: the bytes are the source's own
    // and outlive the write.
    auto appendStored = [&](std::span<const u8> stored, bool lasting = false) {
        u64 const offset = sink.size();
        if (lasting)
            sink.appendLasting(stored);
        else
            sink.append(stored);
        if (rawChunk != 0)
            sink.append(rawChunkMd5s(stored, rawChunk));
        return offset;
    };
    // Only the arrays (attributes) will hold are worth a pass over a file,
    // and that pass is made where the file is encoded.
    bool const wantCrc32 = plan.attributes && hasFlag(plan.attributeFlags, AttributeFlag::kCrc32);
    bool const wantMd5 = plan.attributes && hasFlag(plan.attributeFlags, AttributeFlag::kMd5);
    auto recordAttributes = [&](std::optional<u32> crc, std::optional<u64> time,
                                std::optional<std::array<u8, 16>> md5) {
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
        opts.modernCodecs = hdr.formatVersion >= 1;
        opts.crc32 = wantCrc32 && !entry.crc32;
        opts.md5 = wantMd5 && !entry.md5;
        return opts;
    };

    // New files are encoded a window at a time (in parallel when a pool is
    // available), just before they are written: a save holds one window's
    // encoded bytes, not every new file's.
    static constexpr size_t kEncodeWindowBytes = size_t{64} << 20;
    std::vector<EncodedFile> encodedFiles(entries.size());
    size_t windowEnd = 0;
    auto encodeWindow = [&](size_t from) {
        std::vector<size_t> fresh;
        size_t bytes = 0;
        for (windowEnd = from; windowEnd < entries.size(); ++windowEnd) {
            const WriteEntry& entry = entries[windowEnd];
            if (entry.rawCopy)
                continue;
            if (!fresh.empty() && bytes + entry.rawData.size() > kEncodeWindowBytes)
                break;
            bytes += entry.rawData.size();
            fresh.push_back(windowEnd);
        }
        if (pool && pool->threadCount() > 0 && fresh.size() >= 2) {
            std::vector<std::pair<std::span<const u8>, EncodeOptions>> batchItems;
            batchItems.reserve(fresh.size());
            for (size_t const idx : fresh)
                batchItems.emplace_back(entries[idx].rawData, makeEncodeOpts(entries[idx]));
            auto batchResult = encodeBatch(batchItems, pool);
            for (size_t b = 0; b < batchResult.files.size(); ++b)
                encodedFiles[fresh[b]] = std::move(batchResult.files[b]);
        } else {
            for (size_t const idx : fresh)
                encodedFiles[idx] = encodeFileData(entries[idx].rawData, makeEncodeOpts(entries[idx]), pool);
        }
    };

    // File data, in entry order.
    for (size_t idx = 0; idx < entries.size(); ++idx) {
        if (idx == windowEnd)
            encodeWindow(idx);
        const WriteEntry& entry = entries[idx];
        // Keyed to its offset: encrypted again when this write moves it, a
        // plain copy when it lands where it was. The key takes the low 32 bits.
        if (entry.rawCopy && entry.rekeyBase && static_cast<u32>(sink.size()) != entry.sourceBlock.fileOffset) {
            const BlockEntry& source = entry.sourceBlock;
            u32 const size = source.uncompressedSize;
            u32 const newOffset = static_cast<u32>(sink.size());
            auto const rekeyed =
                rekeyStoredFile(entry.rawSectors, source, hdr.sectorSize(), (*entry.rekeyBase + source.fileOffset) ^ size,
                                (*entry.rekeyBase + newOffset) ^ size);
            if (!rekeyed)
                return "the sector table of '" + (entry.filename.empty() ? std::string("a file") : entry.filename) +
                       "' does not decrypt, so it cannot be moved";
            placeBlock(source, appendStored(*rekeyed));
            recordAttributes(entry.crc32, entry.filetime, entry.md5);
            continue;
        }
        if (entry.rawCopy) {
            placeBlock(entry.sourceBlock, appendStored(entry.rawSectors, true));
            recordAttributes(entry.crc32, entry.filetime, entry.md5);
            continue;
        }
        EncodedFile encoded = std::move(encodedFiles[idx]);
        if (encoded.data.empty() && !entry.rawData.empty()) {
            encoded.data.assign(entry.rawData.begin(), entry.rawData.end());
            encoded.compressedSize = static_cast<u32>(entry.rawData.size());
            encoded.flags = FileFlag::kExists;
        }
        if (entry.rawData.empty())
            encoded.flags = FileFlag::kExists;
        placeBlock(makeBlockEntry(encoded, static_cast<u32>(entry.rawData.size())), appendStored(encoded.data));
        recordAttributes(entry.crc32.value_or(encoded.crc32), entry.filetime, entry.md5.value_or(encoded.md5));
    }

    // (listfile) names every named file once, and not the special files. A
    // name is listed as it was hashed: Storm and StormLib hash a slash as
    // itself, so a name hashed with backslashes is listed with them.
    std::vector<size_t> listed(entries.size());
    for (size_t idx = 0; idx < entries.size(); ++idx)
        listed[idx] = idx;
    std::stable_sort(listed.begin(), listed.end(),
                     [&](size_t a, size_t b) { return entries[a].listRank < entries[b].listRank; });
    std::vector<std::string> listfileNames;
    {
        std::unordered_set<std::string> seen;
        for (size_t const idx : listed) {
            const WriteEntry& entry = entries[idx];
            if (entry.filename.empty())
                continue;
            std::string name = entry.filename;
            if (!entry.slashHashed)
                std::replace(name.begin(), name.end(), '/', '\\');
            std::string upper = name;
            std::transform(upper.begin(), upper.end(), upper.begin(),
                           [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            if (seen.insert(std::move(upper)).second)
                listfileNames.push_back(std::move(name));
        }
    }
    auto appendSpecialFile = [&](const std::string& name, const std::vector<u8>& rawData) {
        EncodeOptions opts;
        opts.compression = CompressionFlag::kZlib;
        opts.sectorSize = hdr.sectorSize();
        opts.filename = name;
        opts.singleUnit = plan.singleUnitSpecials;
        opts.crc32 = wantCrc32;
        opts.md5 = wantMd5;
        auto encoded = encodeFileData(std::span<const u8>(rawData), opts);
        if (encoded.data.empty() && !rawData.empty()) {
            encoded.data = rawData;
            encoded.compressedSize = static_cast<u32>(encoded.data.size());
            encoded.flags = FileFlag::kExists;
        }
        if (rawData.empty())
            encoded.flags = FileFlag::kExists;
        placeBlock(makeBlockEntry(encoded, static_cast<u32>(rawData.size())), appendStored(encoded.data));
        return encoded;
    };
    auto const listfile = buildListfile(listfileNames);
    {
        auto const encoded = appendSpecialFile("(listfile)", listfile);
        recordAttributes(encoded.crc32, now, encoded.md5);
    }
    if (plan.attributes) {
        // Its own entry is zero; the arrays cover every block, itself included.
        recordAttributes(0u, now, std::array<u8, 16>{});
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
        auto const bet = buildBetTable(blocks, nameHashes, blockOffsetHi);
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
        if (!hashTable.insert(entry.filename, entry.locale, static_cast<u32>(idx), entry.platform, entry.slashHashed))
            return "the hash table is full";
    }
    u32 const listfileBlock = static_cast<u32>(entries.size());
    if (!hashTable.insert("(listfile)", 0, listfileBlock) ||
        (plan.attributes && !hashTable.insert("(attributes)", 0, listfileBlock + 1)))
        return "the hash table is full";

    BlockTable blockTable;
    blockTable.createEmpty();
    for (size_t i = 0; i < blocks.size(); ++i)
        (void)blockTable.append(blocks[i], blockOffsetHi[i]);

    auto const hashTableData = hashTable.serialize();
    hdr.hashTableOffset = static_cast<u32>(sink.size());
    hdr.hashTableOffsetHi = static_cast<u16>(sink.size() >> 32);
    hdr.hashTableEntries = capacity;
    hdr.hashTableSize64 = hashTableData.size();
    hdr.hashTableMd5 = common::md5Hash(hashTableData);
    sink.append(hashTableData);

    auto const blockTableData = blockTable.serialize();
    hdr.blockTableOffset = static_cast<u32>(sink.size());
    hdr.blockTableOffsetHi = static_cast<u16>(sink.size() >> 32);
    hdr.blockTableEntries = blockTable.count();
    hdr.blockTableSize64 = blockTableData.size();
    hdr.blockTableMd5 = common::md5Hash(blockTableData);
    sink.append(blockTableData);

    if (hdr.formatVersion >= 1 && blockTable.needsHiBlockTable()) {
        auto const hiData = blockTable.serializeHiBlockTable();
        hdr.hiBlockTableOffset = sink.size();
        hdr.hiBlockTableSize64 = hiData.size();
        hdr.hiBlockTableMd5 = common::md5Hash(hiData);
        sink.append(hiData);
    }

    // A version 1 header has no high offset bits to reach past 4 GiB with.
    if (hdr.formatVersion == 0 && sink.size() > 0xFFFFFFFFull)
        return "a version 1 archive cannot exceed 4 GiB";
    hdr.archiveSize = static_cast<u32>(sink.size());
    if (hdr.formatVersion >= 2)
        hdr.archiveSize64 = sink.size();
    std::vector<u8> header(hdr.headerSize, 0);
    serializeHeader(header, hdr);
    // V4: the header's own MD5 covers everything in it before that field.
    if (hdr.headerSize >= 208) {
        static constexpr size_t kMpqHeaderMd5 = 192;
        auto const digest = common::md5Hash(std::span<const u8>(header.data(), kMpqHeaderMd5));
        std::memcpy(header.data() + kMpqHeaderMd5, digest.data(), digest.size());
    }
    sink.patch(0, header);
    return {};
}

WriteResult writeArchive(const WritePlan& plan, const std::vector<WriteEntry>& entries,
                         interfaces::WorkerPool* pool) {
    MemorySink sink;
    sink.bytes.reserve(1024 * 1024);
    WriteResult result;
    result.error = writeArchive(plan, entries, sink, pool);
    if (result.error.empty())
        result.archive = std::move(sink.bytes);
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
