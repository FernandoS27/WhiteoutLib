// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "index.h"
#include "../common/bit_reader.h"
#include "../common/md5.h"
#include "../common/mapped_file.h"

#include <whiteout/utils/job_group.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <mutex>

namespace whiteout::storages::casc {

// ---- Constants local to index file codec ----

/// Minimum file size to read the idx header (contains headerDataSize field).
static constexpr size_t kIdxMinHeaderSize = 36;

/// Byte offset where entry data begins in an idx file.
static constexpr size_t kIdxEntryDataOffset = 40;

/// Default field sizes for index entries.
static constexpr u8 kIdxDefaultEKeyLen = 9;
static constexpr u8 kIdxDefaultOffsetLen = 5;
static constexpr u8 kIdxDefaultSizeLen = 4;
static constexpr u8 kIdxDefaultOffsetHighBits = 30;

/// Number of index buckets (one per high nibble of EKey).
static constexpr int kIdxNumBuckets = 16;

/// Index file version written during serialization.
static constexpr u16 kIdxWriteVersion = 7;

// ============================================================================
// Internal Structures
// ============================================================================

struct IdxHeader {
    u16 version = 0;
    u8 bucketIndex = 0;
    u8 extraBytes = 0;
    u8 encodedSizeLen = kIdxDefaultSizeLen;
    u8 storageOffsetLen = kIdxDefaultOffsetLen;
    u8 eKeyLen = kIdxDefaultEKeyLen;
    u8 offsetHighBits = kIdxDefaultOffsetHighBits;
};

/// Parsed from the idx file header block (16 bytes at offset 4).
static bool parseIdxHeader(const u8* data, size_t fileSize, IdxHeader& hdr) {
    if (fileSize < kIdxMinHeaderSize)
        return false;

    // First 4 bytes at offset 0: header data size (always 16).
    u32 headerDataSize = 0;
    std::memcpy(&headerDataSize, data, 4);
    if (headerDataSize < 12 || headerDataSize > 256)
        return false;

    // Header fields within the 16-byte block at offset 4.
    // Layout (determined from real corpus files):
    //   +0: u32 inner_checksum (varies per bucket)
    //   +4: u16 version (LE, typically 7)
    //   +6: u8  bucketIndex
    //   +7: u8  extraBytes
    //   +8: u8  encodedSizeLen
    //   +9: u8  storageOffsetLen
    //  +10: u8  eKeyLen
    //  +11: u8  offsetHighBits
    //  +12: u32 flags/padding

    const u8* h = data + 4; // Skip the headerDataSize u32.
    // Skip the 4-byte inner checksum.
    std::memcpy(&hdr.version, h + 4, 2);
    hdr.bucketIndex = h[6];
    hdr.extraBytes = h[7];
    hdr.encodedSizeLen = h[8];
    hdr.storageOffsetLen = h[9];
    hdr.eKeyLen = h[10];
    hdr.offsetHighBits = h[11];

    if (hdr.version != 5 && hdr.version != 7 && hdr.version != 8)
        return false;
    if (hdr.eKeyLen == 0 || hdr.eKeyLen > 16)
        return false;
    if (hdr.encodedSizeLen == 0 || hdr.encodedSizeLen > 8)
        return false;
    if (hdr.storageOffsetLen == 0 || hdr.storageOffsetLen > 8)
        return false;

    return true;
}

static size_t entrySize(const IdxHeader& hdr) {
    return size_t(hdr.eKeyLen) + hdr.storageOffsetLen + hdr.encodedSizeLen + hdr.extraBytes;
}

/// Parse a single .idx file and collect entries.
static void parseIdxFile(const u8* data, size_t fileSize,
                         std::vector<IndexEntry>& entries) {
    IdxHeader hdr;
    if (!parseIdxHeader(data, fileSize, hdr))
        return;

    size_t eSz = entrySize(hdr);
    if (eSz == 0 || eSz > 64)
        return;

    // Index file layout using "guarded blocks" (8-byte headers: [BlockSize:4][BlockHash:4]):
    //   Offset 0:  GuardedBlock1 header (8 bytes: header data size + hash)
    //   Offset 8:  Header data (headerDataSize bytes, typically 16)
    //   Offset 24: Padding (8 bytes)
    //   Offset 32: GuardedBlock2 header (8 bytes: entry data size + hash)
    //   Offset 40: Entry data
    if (fileSize < kIdxEntryDataOffset)
        return;

    u32 segmentSize = 0;
    std::memcpy(&segmentSize, data + 32, 4);

    size_t entryDataStart = kIdxEntryDataOffset;
    if (entryDataStart + segmentSize > fileSize)
        segmentSize = u32(fileSize - entryDataStart);

    size_t numEntries = segmentSize / eSz;
    entries.reserve(entries.size() + numEntries);

    u32 offsetMask = (1u << hdr.offsetHighBits) - 1;

    for (size_t i = 0; i < numEntries; ++i) {
        const u8* entry = data + entryDataStart + i * eSz;

        IndexEntry ie;

        // EKey (first eKeyLen bytes).
        std::memcpy(ie.eKey.data(), entry, hdr.eKeyLen);

        // Storage offset (storageOffsetLen bytes, BIG-ENDIAN per CascLib).
        const u8* offBytes = entry + hdr.eKeyLen;
        u64 rawOffset = 0;
        for (u8 b = 0; b < hdr.storageOffsetLen; ++b)
            rawOffset = (rawOffset << 8) | offBytes[b];

        // Split: lower offsetHighBits = byte offset, upper bits = archive index.
        ie.archiveOffset = u32(rawOffset & offsetMask);
        ie.archiveIndex = u32(rawOffset >> hdr.offsetHighBits);

        // Encoded size (encodedSizeLen bytes, LE).
        u64 rawSize64 = 0;
        std::memcpy(&rawSize64, entry + hdr.eKeyLen + hdr.storageOffsetLen,
                     std::min<u8>(hdr.encodedSizeLen, 8));
        ie.encodedSize = u32(rawSize64);

        // Skip zero/empty entries.
        bool allZero = true;
        for (size_t b = 0; b < hdr.eKeyLen; ++b) {
            if (ie.eKey[b] != 0) { allZero = false; break; }
        }
        if (allZero)
            continue;

        entries.push_back(ie);
    }
}

// ============================================================================
// EKey Hash
// ============================================================================

u64 IndexTable::eKeyHash(std::span<const u8> eKey) {
    // Use first 8 bytes of EKey as a u64 hash key.
    u64 h = 0;
    size_t len = std::min(eKey.size(), size_t(8));
    std::memcpy(&h, eKey.data(), len);
    return h;
}

// ============================================================================
// IndexTable::load
// ============================================================================

IndexTable IndexTable::load(const std::string& dataDir,
                            interfaces::WorkerPool* pool) {
    IndexTable table;

    // Scan for .idx files in data/ or the root directory.
    std::vector<std::filesystem::path> idxPaths;
    for (auto& candidate : {dataDir + "/data", dataDir}) {
        if (!std::filesystem::exists(candidate))
            continue;
        for (auto& entry : std::filesystem::directory_iterator(candidate)) {
            if (entry.is_regular_file() && entry.path().extension() == ".idx")
                idxPaths.push_back(entry.path());
        }
        if (!idxPaths.empty())
            break;
    }

    // For each bucket, keep only the highest-version .idx file.
    // Filename: {bucket:02x}{version:08x}.idx → 10 hex chars + .idx
    std::unordered_map<u8, std::filesystem::path> bestPerBucket;
    for (auto& p : idxPaths) {
        auto stem = p.stem().string();
        if (stem.size() < 4)
            continue;

        // First 2 hex chars = bucket.
        u8 bucket = 0;
        for (int i = 0; i < 2; ++i) {
            char c = stem[i];
            u8 nibble = 0;
            if (c >= '0' && c <= '9') nibble = u8(c - '0');
            else if (c >= 'a' && c <= 'f') nibble = u8(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') nibble = u8(c - 'A' + 10);
            bucket = (bucket << 4) | nibble;
        }

        auto it = bestPerBucket.find(bucket);
        if (it == bestPerBucket.end() || stem > it->second.stem().string())
            bestPerBucket[bucket] = p;
    }

    // Parse each .idx file.
    std::vector<std::filesystem::path> filesToParse;
    for (auto& [_, path] : bestPerBucket)
        filesToParse.push_back(path);

    if (pool && filesToParse.size() > 1) {
        // Parallel parse.
        utils::JobGroup jobGroup;
        std::vector<std::vector<IndexEntry>> perFileEntries(filesToParse.size());
        jobGroup.add(filesToParse.size());
        for (size_t i = 0; i < filesToParse.size(); ++i) {
            interfaces::WorkerTask task;
            task.fn = [&, i]() {
                auto mf = common::MappedFile::open(filesToParse[i].string());
                if (mf)
                    parseIdxFile(mf->ptr(), mf->size(), perFileEntries[i]);
                jobGroup.done();
            };
            pool->submit(task);
        }
        jobGroup.wait();

        for (auto& entries : perFileEntries)
            for (auto& e : entries)
                table.m_entries[eKeyHash(std::span(e.eKey.data(), 9))] = e;
    } else {
        // Sequential parse.
        for (auto& path : filesToParse) {
            auto mf = common::MappedFile::open(path.string());
            if (!mf) continue;

            std::vector<IndexEntry> entries;
            parseIdxFile(mf->ptr(), mf->size(), entries);
            for (auto& e : entries)
                table.m_entries[eKeyHash(std::span(e.eKey.data(), 9))] = e;
        }
    }

    return table;
}

// ============================================================================
// IndexTable::find
// ============================================================================

const IndexEntry* IndexTable::find(std::span<const u8> eKeyPrefix) const {
    u64 h = eKeyHash(eKeyPrefix);
    auto it = m_entries.find(h);
    if (it == m_entries.end())
        return nullptr;

    // Verify that the prefix actually matches (collision check).
    size_t cmpLen = std::min(eKeyPrefix.size(), size_t(9));
    if (std::memcmp(it->second.eKey.data(), eKeyPrefix.data(), cmpLen) != 0)
        return nullptr;

    return &it->second;
}

// ============================================================================
// IndexTable::insert
// ============================================================================

void IndexTable::insert(const IndexEntry& entry) {
    u64 h = eKeyHash(std::span(entry.eKey.data(), 9));
    m_entries[h] = entry;
}

// ============================================================================
// IndexTable::serialize
// ============================================================================

std::vector<std::pair<std::string, std::vector<u8>>> IndexTable::serialize() const {
    // Group entries by bucket (first nibble of EKey).
    std::vector<std::vector<const IndexEntry*>> buckets(kIdxNumBuckets);

    for (auto& [_, entry] : m_entries) {
        u8 bucket = (entry.eKey[0] >> 4) & 0x0F;
        buckets[bucket].push_back(&entry);
    }

    std::vector<std::pair<std::string, std::vector<u8>>> result;

    for (int b = 0; b < kIdxNumBuckets; ++b) {
        if (buckets[b].empty())
            continue;

        // Sort entries by EKey for consistent output.
        std::sort(buckets[b].begin(), buckets[b].end(),
                  [](const IndexEntry* a, const IndexEntry* b) {
                      return std::memcmp(a->eKey.data(), b->eKey.data(), 9) < 0;
                  });

        // Default params
        constexpr u8 kEKeyLen = kIdxDefaultEKeyLen;
        constexpr u8 kOffsetLen = kIdxDefaultOffsetLen;
        constexpr u8 kSizeLen = kIdxDefaultSizeLen;
        constexpr u8 kHighBits = kIdxDefaultOffsetHighBits;
        constexpr size_t kEntrySize = kEKeyLen + kOffsetLen + kSizeLen;

        size_t numEntries = buckets[b].size();
        u32 dataSize = u32(numEntries * kEntrySize);

        // Build idx file: header area + entry data + padding to 4096
        // Layout: [headerDataSize:4][headerHash:4][headerFields:16][padding:8]
        //         [segmentSize:4][segmentHash:4][entryData...]
        size_t entryDataStart = kIdxEntryDataOffset;
        size_t fileSize = entryDataStart + dataSize;
        // Pad to 4096-byte boundary, plus 16 bytes for footer hash.
        size_t padded = ((fileSize + 4095) / 4096) * 4096;
        if (padded < fileSize + 16)
            padded += 4096;

        std::vector<u8> file(padded, 0);

        // Write header block (16 bytes at offset 4).
        u32 headerDataSize = 16;
        std::memcpy(file.data(), &headerDataSize, 4);

        // Inner checksum placeholder (offset 4, 4 bytes) — write zeros.
        u16 version = kIdxWriteVersion;
        std::memcpy(file.data() + 8, &version, 2);
        file[10] = u8(b); // bucket
        file[11] = 0;     // extraBytes
        file[12] = kSizeLen;
        file[13] = kOffsetLen;
        file[14] = kEKeyLen;
        file[15] = kHighBits;

        // Segment size at offset 32.
        std::memcpy(file.data() + 32, &dataSize, 4);

        // Write entries.
        for (size_t i = 0; i < numEntries; ++i) {
            u8* dst = file.data() + entryDataStart + i * kEntrySize;
            auto& e = *buckets[b][i];

            std::memcpy(dst, e.eKey.data(), kEKeyLen);

            u64 offsetField = (u64(e.archiveIndex) << kHighBits) | u64(e.archiveOffset);
            // Write storage offset in big-endian (parser reads BE byte-by-byte).
            for (int j = kOffsetLen - 1; j >= 0; --j) {
                dst[kEKeyLen + (kOffsetLen - 1 - j)] = u8(offsetField >> (j * 8));
            }

            std::memcpy(dst + kEKeyLen + kOffsetLen, &e.encodedSize, kSizeLen);
        }

        // Compute and write inner checksum (first 4 bytes of MD5 of header fields).
        // Hash the 12 bytes of header fields after the checksum slot (offset 8..19).
        auto headerMd5 = common::md5Hash(std::span(file.data() + 8, 12));
        std::memcpy(file.data() + 4, headerMd5.data(), 4);

        // Segment hash at offset 36 (first 4 bytes of MD5 of entry data).
        auto dataMd5 = common::md5Hash(std::span(file.data() + entryDataStart, dataSize));
        std::memcpy(file.data() + 36, dataMd5.data(), 4);

        // Footer hash (last 16 bytes): MD5 of entry data.
        std::memcpy(file.data() + padded - 16, dataMd5.data(), 16);

        // Filename: {bucket:02x}00000001.idx
        char name[32];
        std::snprintf(name, sizeof(name), "%02x00000001.idx", b);
        result.emplace_back(name, std::move(file));
    }

    return result;
}

} // namespace whiteout::storages::casc
