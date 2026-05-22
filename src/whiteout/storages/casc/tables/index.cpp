// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "../../common/bit_reader.h"
#include "../../common/jenkins.h"
#include "../../common/mapped_file.h"
#include "../../common/md5.h"
#include "index.h"

#include <whiteout/utils/job_group.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

namespace whiteout::storages::casc {

// pimpls so IndexTable stays moveable despite once_flags + mutex.

struct IndexTable::LazyArchives {
    interfaces::WorkerPool* pool = nullptr;
    std::vector<std::filesystem::path> archivePaths; // [i] empty if no file
    std::vector<std::once_flag> flags;
    std::unordered_map<u64, IndexEntry> entries;
    mutable std::shared_mutex mutex;
};

// Bucket from EKey is (eKey[0] >> 4) & 0x0F (matches serialize). find()
// faults that bucket first, then fans out across the rest as a safety net
// in case Blizzard's actual file assignment ever diverges from our formula.
struct IndexTable::LazyBuckets {
    interfaces::WorkerPool* pool = nullptr;
    std::array<std::vector<std::filesystem::path>, 16> bucketFiles;
    std::array<std::once_flag, 16> flags;
    std::unordered_map<u64, IndexEntry> entries;
    mutable std::shared_mutex mutex;
    bool nonEmpty = false;
};

IndexTable::IndexTable() = default;
IndexTable::~IndexTable() = default;
IndexTable::IndexTable(IndexTable&&) noexcept = default;
IndexTable& IndexTable::operator=(IndexTable&&) noexcept = default;

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
static void parseIdxFile(const u8* data, size_t fileSize, std::vector<IndexEntry>& entries) {
    IdxHeader hdr;
    if (!parseIdxHeader(data, fileSize, hdr))
        return;

    size_t const eSz = entrySize(hdr);
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

    size_t const entryDataStart = kIdxEntryDataOffset;
    if (entryDataStart + segmentSize > fileSize)
        segmentSize = u32(fileSize - entryDataStart);

    size_t const numEntries = segmentSize / eSz;
    entries.reserve(entries.size() + numEntries);

    u32 const offsetMask = (1u << hdr.offsetHighBits) - 1;

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
            if (ie.eKey[b] != 0) {
                allZero = false;
                break;
            }
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
    // XOR-fold all bytes of the truncated EKey (typically 9 bytes) into a u64.
    // Loading the first 8 bytes as the base and XOR-ing the remaining byte(s)
    // ensures byte 9 participates in the hash, preventing collisions between
    // entries that differ only in the last byte.
    u64 h = 0;
    size_t const len = std::min(eKey.size(), size_t(8));
    std::memcpy(&h, eKey.data(), len);
    for (size_t i = 8; i < eKey.size(); ++i)
        h ^= static_cast<u64>(eKey[i]) << ((i - 8) * 8);
    return h;
}

namespace {

/// Group .idx files by bucket (0..15), keeping best-per-(dir,bucket).
/// Multiple dirs (data/, darch/) can each contribute one file per bucket.
std::array<std::vector<std::filesystem::path>, 16> discoverIdxFilesByBucket(
    const std::string& dataDir) {
    namespace fs = std::filesystem;
    std::array<std::vector<fs::path>, 16> byBucket;

    std::vector<fs::path> idxPaths;

    // Try the known primary index directories first (matching CascLib).
    std::string primaryDir;
    for (auto& name : {"data", "darch"}) {
        std::string const candidate = dataDir + "/" + name;
        if (fs::exists(candidate) && fs::is_directory(candidate)) {
            primaryDir = candidate;
            break;
        }
    }

    if (!primaryDir.empty()) {
        for (auto& entry : fs::directory_iterator(primaryDir)) {
            if (entry.is_regular_file() && entry.path().extension() == ".idx")
                idxPaths.push_back(entry.path());
        }
    }

    // Fallback: scan subdirectories.
    if (idxPaths.empty() && fs::exists(dataDir)) {
        for (auto& dirEntry : fs::directory_iterator(dataDir)) {
            if (!dirEntry.is_directory())
                continue;
            for (auto& fileEntry : fs::directory_iterator(dirEntry.path())) {
                if (fileEntry.is_regular_file() && fileEntry.path().extension() == ".idx")
                    idxPaths.push_back(fileEntry.path());
            }
        }
    }
    // Fallback: flat layout.
    if (idxPaths.empty() && fs::exists(dataDir)) {
        for (auto& entry : fs::directory_iterator(dataDir)) {
            if (entry.is_regular_file() && entry.path().extension() == ".idx")
                idxPaths.push_back(entry.path());
        }
    }

    // Filename: {bucket:02x}{version:08x}.idx — first 2 chars give bucket.
    auto parseBucket = [](const std::string& stem) -> u8 {
        u8 bucket = 0;
        for (int i = 0; i < 2 && i < int(stem.size()); ++i) {
            char const c = stem[i];
            u8 nibble = 0;
            if (c >= '0' && c <= '9')
                nibble = u8(c - '0');
            else if (c >= 'a' && c <= 'f')
                nibble = u8(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                nibble = u8(c - 'A' + 10);
            bucket = (bucket << 4) | nibble;
        }
        return bucket;
    };

    // Pick best (highest stem string compare ≡ highest version) per
    // (directory, bucket). Same logic the original eager loader used.
    std::unordered_map<std::string, std::unordered_map<u8, fs::path>> bestByDirBucket;
    for (auto& p : idxPaths) {
        auto stem = p.stem().string();
        if (stem.size() < 4)
            continue;
        u8 const bucket = parseBucket(stem);
        auto& slot = bestByDirBucket[p.parent_path().string()][bucket];
        if (slot.empty() || stem > slot.stem().string())
            slot = p;
    }

    for (auto& [_, perBucket] : bestByDirBucket) {
        for (auto& [bucket, path] : perBucket) {
            if (bucket < 16) // skip out-of-range buckets — CascLib uses 16.
                byBucket[bucket].push_back(path);
        }
    }
    return byBucket;
}

void parseIdxFileIntoVector(const std::filesystem::path& path, std::vector<IndexEntry>& out) {
    auto mf = common::MappedFile::open(path.string());
    if (mf)
        parseIdxFile(mf->ptr(), mf->size(), out);
}

} // namespace

// ============================================================================
// IndexTable::load
// ============================================================================

IndexTable IndexTable::load(const std::string& dataDir, interfaces::WorkerPool* pool) {
    IndexTable table;

    // Scan for .idx files in the primary index subdirectory.
    // CascLib scans "data/" (or "darch/" for older HOTS builds) under the Data
    // directory.  Other subdirectories such as the game-specific shmem caches
    // (e.g. "d3/", "ecache/") use a different entry layout and must NOT be
    // mixed into the main index—their 8-byte storage-offsets encode archive
    // indices that don't correspond to local data.XXX archives.
    namespace fs = std::filesystem;
    std::vector<fs::path> idxPaths;

    // Try the known primary index directories first (matching CascLib).
    std::string primaryDir;
    for (auto& name : {"data", "darch"}) {
        std::string const candidate = dataDir + "/" + name;
        if (fs::exists(candidate) && fs::is_directory(candidate)) {
            primaryDir = candidate;
            break;
        }
    }

    if (!primaryDir.empty()) {
        for (auto& entry : fs::directory_iterator(primaryDir)) {
            if (entry.is_regular_file() && entry.path().extension() == ".idx")
                idxPaths.push_back(entry.path());
        }
    }

    // Fallback: if no primary directory found, scan all immediate subdirectories.
    if (idxPaths.empty() && fs::exists(dataDir)) {
        for (auto& dirEntry : fs::directory_iterator(dataDir)) {
            if (!dirEntry.is_directory())
                continue;
            for (auto& fileEntry : fs::directory_iterator(dirEntry.path())) {
                if (fileEntry.is_regular_file() && fileEntry.path().extension() == ".idx")
                    idxPaths.push_back(fileEntry.path());
            }
        }
    }
    // Fallback: check dataDir root (flat layout).
    if (idxPaths.empty() && fs::exists(dataDir)) {
        for (auto& entry : fs::directory_iterator(dataDir)) {
            if (entry.is_regular_file() && entry.path().extension() == ".idx")
                idxPaths.push_back(entry.path());
        }
    }

    // For each (parent directory, bucket) pair, keep only the highest-version .idx file.
    // Filename: {bucket:02x}{version:08x}.idx → 10 hex chars + .idx
    // Different subdirectories may have independent bucket spaces,
    // so we must resolve best-per-bucket within each directory separately.
    auto parseBucket = [](const std::string& stem) -> u8 {
        u8 bucket = 0;
        for (int i = 0; i < 2; ++i) {
            char const c = stem[i];
            u8 nibble = 0;
            if (c >= '0' && c <= '9')
                nibble = u8(c - '0');
            else if (c >= 'a' && c <= 'f')
                nibble = u8(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                nibble = u8(c - 'A' + 10);
            bucket = (bucket << 4) | nibble;
        }
        return bucket;
    };

    // Group by parent directory.
    std::unordered_map<std::string, std::vector<std::filesystem::path>> idxByDir;
    for (auto& p : idxPaths) {
        auto stem = p.stem().string();
        if (stem.size() < 4)
            continue;
        idxByDir[p.parent_path().string()].push_back(p);
    }

    // Within each directory, pick the best (highest-version) per bucket.
    std::vector<std::filesystem::path> filesToParse;
    for (auto& [dir, paths] : idxByDir) {
        std::unordered_map<u8, std::filesystem::path> bestPerBucket;
        for (auto& p : paths) {
            auto stem = p.stem().string();
            u8 const bucket = parseBucket(stem);
            auto it = bestPerBucket.find(bucket);
            if (it == bestPerBucket.end() || stem > it->second.stem().string())
                bestPerBucket[bucket] = p;
        }
        for (auto& [_, path] : bestPerBucket)
            filesToParse.push_back(path);
    }

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

        size_t totalEntries = 0;
        for (auto& entries : perFileEntries)
            totalEntries += entries.size();
        table.m_entries.reserve(totalEntries);

        for (auto& entries : perFileEntries)
            for (auto& e : entries)
                table.m_entries.insertOrAssign(eKeyHash(std::span(e.eKey.data(), 9)), e);
    } else {
        // Sequential parse.
        for (auto& path : filesToParse) {
            auto mf = common::MappedFile::open(path.string());
            if (!mf)
                continue;

            std::vector<IndexEntry> entries;
            parseIdxFile(mf->ptr(), mf->size(), entries);
            for (auto& e : entries)
                table.m_entries.insertOrAssign(eKeyHash(std::span(e.eKey.data(), 9)), e);
        }
    }

    return table;
}

// ============================================================================
// Archive Index (.index) file parser — per-archive footer-based format
// ============================================================================

/// Footer of an archive .index file (last 36 bytes for FooterHashBytes=8).
/// Layout matches CascLib's FILE_INDEX_FOOTER<0x08>:
///   TocHash[16], Version(1), Reserved[2], PageSizeKB(1),
///   OffsetBytes(1), SizeBytes(1), EKeyLength(1), FooterHashBytes(1),
///   ElementCount[4], FooterHash[8].
struct ArchiveIndexFooter {
    u8 pageSizeKB = 0;
    u8 offsetBytes = 0;
    u8 sizeBytes = 0;
    u8 eKeyLength = 0;
    u8 footerHashBytes = 0;
    u32 elementCount = 0;
    size_t pageLength = 0;
    size_t itemLength = 0;
    size_t footerLength = 0;
};

static constexpr size_t kArcIdxFooterSize8 = 36; // FooterHashBytes=8

static bool parseArchiveIndexFooter(const u8* data, size_t fileSize, ArchiveIndexFooter& footer) {
    if (fileSize < kArcIdxFooterSize8)
        return false;

    const u8* f = data + fileSize - kArcIdxFooterSize8;

    // Validate: Version == 1, Reserved == {0, 0}, FooterHashBytes == 8.
    u8 const version = f[16];
    if (version != 1)
        return false;
    if (f[17] != 0 || f[18] != 0)
        return false;

    footer.pageSizeKB = f[19];
    footer.offsetBytes = f[20];
    footer.sizeBytes = f[21];
    footer.eKeyLength = f[22];
    footer.footerHashBytes = f[23];

    if (footer.footerHashBytes != 8)
        return false;
    if (footer.eKeyLength == 0 || footer.eKeyLength > 16)
        return false;
    if (footer.offsetBytes == 0 || footer.offsetBytes > 8)
        return false;
    if (footer.sizeBytes == 0 || footer.sizeBytes > 8)
        return false;
    if (footer.pageSizeKB == 0)
        return false;

    // ElementCount is 4 bytes LE at offset 24.
    std::memcpy(&footer.elementCount, f + 24, 4);

    footer.pageLength = size_t(footer.pageSizeKB) << 10;
    footer.itemLength = footer.eKeyLength + footer.offsetBytes + footer.sizeBytes;
    footer.footerLength = kArcIdxFooterSize8;

    return true;
}

/// Read a variable-length big-endian integer (matches CascLib's ConvertBytesToInteger_X).
static u64 readBEVar(const u8* data, u8 numBytes) {
    u64 val = 0;
    for (u8 i = 0; i < numBytes; ++i)
        val = (val << 8) | data[i];
    return val;
}

/// Parse a single .index file and collect entries for the given archive index.
static void parseArchiveIndexFile(const u8* data, size_t fileSize, u32 archiveIndex,
                                  std::vector<IndexEntry>& entries) {
    ArchiveIndexFooter footer;
    if (!parseArchiveIndexFooter(data, fileSize, footer))
        return;

    if (footer.itemLength == 0 || footer.itemLength > 64)
        return;

    // File layout: contiguous pages of pageLength bytes, then page hashes,
    // then footer. Page count = (fileSize - footerLength) / (pageLength + 16).
    size_t const dataSize = fileSize - footer.footerLength;
    size_t const pageCount = dataSize / (footer.pageLength + 16); // 16 = MD5 hash per page
    size_t const entryDataEnd = pageCount * footer.pageLength;

    entries.reserve(entries.size() + footer.elementCount);

    u32 remaining = footer.elementCount;
    for (size_t page = 0; page < pageCount && remaining > 0; ++page) {
        size_t const pageStart = page * footer.pageLength;
        size_t const itemsOnPage = (footer.pageLength) / footer.itemLength;
        size_t const count = std::min<size_t>(itemsOnPage, remaining);

        for (size_t i = 0; i < count; ++i) {
            size_t const entryOffset = pageStart + i * footer.itemLength;
            if (entryOffset + footer.itemLength > entryDataEnd)
                break;

            const u8* entry = data + entryOffset;

            IndexEntry ie;

            // EKey (eKeyLength bytes).
            std::memcpy(ie.eKey.data(), entry, std::min<u8>(footer.eKeyLength, 16));

            // Skip zero EKeys.
            bool allZero = true;
            for (u8 b = 0; b < footer.eKeyLength && b < 16; ++b) {
                if (ie.eKey[b] != 0) {
                    allZero = false;
                    break;
                }
            }
            if (allZero)
                continue;

            // Encoded size (sizeBytes, BE) immediately after EKey.
            ie.encodedSize = u32(readBEVar(entry + footer.eKeyLength, footer.sizeBytes));

            // Archive offset (offsetBytes, BE) after size.
            u64 const offset =
                readBEVar(entry + footer.eKeyLength + footer.sizeBytes, footer.offsetBytes);
            ie.archiveOffset = u32(offset);
            ie.archiveIndex = archiveIndex;
            ie.directBLTE = true;

            entries.push_back(ie);
        }

        remaining -= u32(count);
    }
}

// ============================================================================
// IndexTable::loadArchiveIndices
// ============================================================================

void IndexTable::loadArchiveIndices(const std::string& dataDir,
                                    const std::vector<std::array<u8, 16>>& archiveEKeys,
                                    interfaces::WorkerPool* pool) {
    namespace fs = std::filesystem;

    if (archiveEKeys.empty())
        return;

    // Search for .index files in indices/ or data/ subdirectories.
    std::string indicesDir;
    for (auto& candidate : {dataDir + "/indices", dataDir + "/data"}) {
        if (fs::exists(candidate) && fs::is_directory(candidate)) {
            // Check if this directory has .index files.
            for (auto& entry : fs::directory_iterator(candidate)) {
                if (entry.is_regular_file() && entry.path().extension() == ".index") {
                    indicesDir = candidate;
                    break;
                }
            }
            if (!indicesDir.empty())
                break;
        }
    }

    if (indicesDir.empty())
        return;

    // Build archive EKey → index mapping.
    auto toHex = [](const std::array<u8, 16>& key) -> std::string {
        static constexpr char hex[] = "0123456789abcdef";
        std::string s;
        s.reserve(32);
        for (u8 const b : key) {
            s += hex[b >> 4];
            s += hex[b & 0xF];
        }
        return s;
    };

    // For each archive, try to find its .index file.
    struct IndexFileJob {
        fs::path path;
        u32 archiveIndex;
    };
    std::vector<IndexFileJob> jobs;

    for (size_t i = 0; i < archiveEKeys.size(); ++i) {
        std::string const hexName = toHex(archiveEKeys[i]);
        fs::path const indexPath = fs::path(indicesDir) / (hexName + ".index");
        if (fs::exists(indexPath))
            jobs.push_back({indexPath, u32(i)});
    }

    if (jobs.empty())
        return;

    [[maybe_unused]] size_t const prevEntries = m_entries.size();

    if (pool && jobs.size() > 1) {
        // Parallel parse.
        utils::JobGroup jobGroup;
        std::vector<std::vector<IndexEntry>> perFileEntries(jobs.size());
        jobGroup.add(jobs.size());
        for (size_t i = 0; i < jobs.size(); ++i) {
            interfaces::WorkerTask task;
            task.fn = [&, i]() {
                auto mf = common::MappedFile::open(jobs[i].path.string());
                if (mf)
                    parseArchiveIndexFile(mf->ptr(), mf->size(), jobs[i].archiveIndex,
                                          perFileEntries[i]);
                jobGroup.done();
            };
            pool->submit(task);
        }
        jobGroup.wait();

        size_t totalNew = 0;
        for (auto& entries : perFileEntries)
            totalNew += entries.size();
        m_entries.reserve(m_entries.size() + totalNew);

        for (auto& entries : perFileEntries)
            for (auto& e : entries)
                m_entries.emplace(eKeyHash(std::span(e.eKey.data(), 9)), e);
    } else {
        // Sequential parse.
        for (auto& job : jobs) {
            auto mf = common::MappedFile::open(job.path.string());
            if (!mf)
                continue;

            std::vector<IndexEntry> entries;
            parseArchiveIndexFile(mf->ptr(), mf->size(), job.archiveIndex, entries);
            for (auto& e : entries)
                m_entries.emplace(eKeyHash(std::span(e.eKey.data(), 9)), e);
        }
    }
}

// ============================================================================
// IndexTable::find
// ============================================================================

const IndexEntry* IndexTable::find(std::span<const u8> eKeyPrefix) const {
    u64 h = eKeyHash(eKeyPrefix);
    const size_t cmpLen = std::min(eKeyPrefix.size(), size_t(9));

    // Eager m_entries first — populated at open, frozen thereafter (no lock).
    // u64 hash collisions fall through to the lazy maps below.
    if (auto* ptr = m_entries.find(h)) {
        if (std::memcmp(ptr->eKey.data(), eKeyPrefix.data(), cmpLen) == 0)
            return ptr;
    }

    if (m_lazyBuckets) {
        auto checkLazyBuckets = [&]() -> const IndexEntry* {
            std::shared_lock<std::shared_mutex> const lk(m_lazyBuckets->mutex);
            auto it = m_lazyBuckets->entries.find(h);
            if (it != m_lazyBuckets->entries.end() &&
                std::memcmp(it->second.eKey.data(), eKeyPrefix.data(), cmpLen) == 0)
                return &it->second;
            return nullptr;
        };
        if (auto* hit = checkLazyBuckets())
            return hit;

        if (!eKeyPrefix.empty()) {
            u8 const firstBucket = u8((eKeyPrefix[0] >> 4) & 0x0F);
            loadBucket(firstBucket);
            if (auto* hit = checkLazyBuckets())
                return hit;

            for (u8 b = 0; b < 16; ++b) {
                if (b == firstBucket)
                    continue;
                if (m_lazyBuckets->bucketFiles[b].empty())
                    continue;
                loadBucket(b);
                if (auto* hit = checkLazyBuckets())
                    return hit;
            }
        }
    }

    if (!m_lazyArchives)
        return nullptr;

    {
        std::shared_lock<std::shared_mutex> const lk(m_lazyArchives->mutex);
        auto it = m_lazyArchives->entries.find(h);
        if (it != m_lazyArchives->entries.end() &&
            std::memcmp(it->second.eKey.data(), eKeyPrefix.data(), cmpLen) == 0) {
            return &it->second;
        }
    }

    const size_t N = m_lazyArchives->archivePaths.size();
    for (size_t i = 0; i < N; ++i) {
        if (m_lazyArchives->archivePaths[i].empty())
            continue;
        loadArchive(u32(i));
        std::shared_lock<std::shared_mutex> const lk(m_lazyArchives->mutex);
        auto it = m_lazyArchives->entries.find(h);
        if (it != m_lazyArchives->entries.end() &&
            std::memcmp(it->second.eKey.data(), eKeyPrefix.data(), cmpLen) == 0) {
            return &it->second;
        }
    }
    return nullptr;
}

size_t IndexTable::entryCount() const {
    size_t n = m_entries.size();
    if (m_lazyArchives) {
        std::shared_lock<std::shared_mutex> const lk(m_lazyArchives->mutex);
        n += m_lazyArchives->entries.size();
    }
    if (m_lazyBuckets) {
        std::shared_lock<std::shared_mutex> const lk(m_lazyBuckets->mutex);
        n += m_lazyBuckets->entries.size();
    }
    return n;
}

// ============================================================================
// IndexTable lazy archive loading
// ============================================================================

namespace {

/// Discover which .index files live on disk and which archives they correspond
/// to. Shared by eager and lazy paths. Returns the directory and a parallel
/// `(path, archiveIndex)` job list. Empty `outDir` ⇒ nothing found.
struct ArchiveDiscovery {
    std::string indicesDir;
    std::vector<std::filesystem::path> paths; // paths[i] = path for archive i, empty if missing
};

ArchiveDiscovery discoverArchiveIndices(const std::string& dataDir,
                                        const std::vector<std::array<u8, 16>>& archiveEKeys) {

    namespace fs = std::filesystem;
    ArchiveDiscovery out;
    out.paths.assign(archiveEKeys.size(), fs::path{});

    if (archiveEKeys.empty())
        return out;

    for (auto& candidate : {dataDir + "/indices", dataDir + "/data"}) {
        if (fs::exists(candidate) && fs::is_directory(candidate)) {
            for (auto& entry : fs::directory_iterator(candidate)) {
                if (entry.is_regular_file() && entry.path().extension() == ".index") {
                    out.indicesDir = candidate;
                    break;
                }
            }
            if (!out.indicesDir.empty())
                break;
        }
    }
    if (out.indicesDir.empty())
        return out;

    auto toHex = [](const std::array<u8, 16>& key) -> std::string {
        static constexpr char hex[] = "0123456789abcdef";
        std::string s;
        s.reserve(32);
        for (u8 const b : key) {
            s += hex[b >> 4];
            s += hex[b & 0xF];
        }
        return s;
    };

    for (size_t i = 0; i < archiveEKeys.size(); ++i) {
        fs::path indexPath = fs::path(out.indicesDir) / (toHex(archiveEKeys[i]) + ".index");
        if (fs::exists(indexPath))
            out.paths[i] = std::move(indexPath);
    }
    return out;
}

} // namespace

void IndexTable::loadArchiveIndicesLazy(const std::string& dataDir,
                                        const std::vector<std::array<u8, 16>>& archiveEKeys,
                                        interfaces::WorkerPool* pool) {

    auto disco = discoverArchiveIndices(dataDir, archiveEKeys);

    // Set up lazy state even if no .index files are on disk — keeps the
    // mode flag consistent and makes ensureAllArchivesLoaded a clean no-op.
    auto state = std::make_unique<LazyArchives>();
    state->pool = pool;
    state->archivePaths = std::move(disco.paths);
    state->flags = std::vector<std::once_flag>(state->archivePaths.size());

    m_lazyArchives = std::move(state);
}

void IndexTable::loadArchive(u32 archiveIdx) const {
    if (!m_lazyArchives)
        return;
    if (archiveIdx >= m_lazyArchives->archivePaths.size())
        return;
    auto& path = m_lazyArchives->archivePaths[archiveIdx];
    if (path.empty())
        return;

    std::call_once(m_lazyArchives->flags[archiveIdx], [&]() {
        auto mf = common::MappedFile::open(path.string());
        if (!mf)
            return;

        std::vector<IndexEntry> entries;
        parseArchiveIndexFile(mf->ptr(), mf->size(), archiveIdx, entries);
        if (entries.empty())
            return;

        std::unique_lock<std::shared_mutex> const lk(m_lazyArchives->mutex);
        for (auto& e : entries) {
            u64 const h = eKeyHash(std::span(e.eKey.data(), 9));
            m_lazyArchives->entries.emplace(h, e);
        }
    });
}

void IndexTable::ensureAllArchivesLoaded() const {
    if (!m_lazyArchives)
        return;
    const size_t N = m_lazyArchives->archivePaths.size();
    for (size_t i = 0; i < N; ++i)
        loadArchive(u32(i));
}

// ============================================================================
// IndexTable::loadLazyBuckets + loadBucket + ensureAllBucketsLoaded
// ============================================================================

IndexTable IndexTable::loadLazyBuckets(const std::string& dataDir, interfaces::WorkerPool* pool) {
    IndexTable table;
    auto byBucket = discoverIdxFilesByBucket(dataDir);

    auto state = std::make_unique<LazyBuckets>();
    state->pool = pool;
    state->bucketFiles = std::move(byBucket);
    for (auto& v : state->bucketFiles)
        if (!v.empty()) {
            state->nonEmpty = true;
            break;
        }

    table.m_lazyBuckets = std::move(state);
    return table;
}

void IndexTable::loadBucket(u8 bucket) const {
    if (!m_lazyBuckets)
        return;
    if (bucket >= 16)
        return;
    auto& paths = m_lazyBuckets->bucketFiles[bucket];
    if (paths.empty())
        return;

    std::call_once(m_lazyBuckets->flags[bucket], [&]() {
        std::vector<IndexEntry> entries;
        for (auto& path : paths)
            parseIdxFileIntoVector(path, entries);
        if (entries.empty())
            return;

        std::unique_lock<std::shared_mutex> const lk(m_lazyBuckets->mutex);
        for (auto& e : entries) {
            u64 const h = eKeyHash(std::span(e.eKey.data(), 9));
            m_lazyBuckets->entries.emplace(h, e);
        }
    });
}

void IndexTable::ensureAllBucketsLoaded() const {
    if (!m_lazyBuckets)
        return;
    for (u8 b = 0; b < 16; ++b)
        loadBucket(b);
}

bool IndexTable::isValid() const {
    if (m_entries.size() > 0)
        return true;
    if (m_lazyBuckets && m_lazyBuckets->nonEmpty)
        return true;
    if (m_lazyArchives && !m_lazyArchives->archivePaths.empty())
        return true;
    return false;
}

// ============================================================================
// IndexTable::insert
// ============================================================================

void IndexTable::insert(const IndexEntry& entry) {
    u64 const h = eKeyHash(std::span(entry.eKey.data(), 9));
    m_entries.insertOrAssign(h, entry);
}

// ============================================================================
// IndexTable::serialize
// ============================================================================

std::vector<std::pair<std::string, std::vector<u8>>> IndexTable::serialize() const {
    // Force any lazy state to fully materialise — serialize() promises a
    // complete on-disk representation.
    ensureAllBucketsLoaded();
    ensureAllArchivesLoaded();

    // Group entries by bucket (first nibble of EKey).
    std::vector<std::vector<const IndexEntry*>> buckets(kIdxNumBuckets);

    auto add = [&buckets](const IndexEntry& entry) {
        u8 const bucket = (entry.eKey[0] >> 4) & 0x0F;
        buckets[bucket].push_back(&entry);
    };

    m_entries.forEach([&add](u64 /*key*/, const IndexEntry& entry) { add(entry); });
    if (m_lazyBuckets) {
        std::shared_lock<std::shared_mutex> const lk(m_lazyBuckets->mutex);
        for (auto& [_, entry] : m_lazyBuckets->entries)
            add(entry);
    }
    if (m_lazyArchives) {
        std::shared_lock<std::shared_mutex> const lk(m_lazyArchives->mutex);
        for (auto& [_, entry] : m_lazyArchives->entries)
            add(entry);
    }

    std::vector<std::pair<std::string, std::vector<u8>>> result;

    for (int b = 0; b < kIdxNumBuckets; ++b) {
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

        size_t const numEntries = buckets[b].size();
        u32 const dataSize = u32(numEntries * kEntrySize);

        // CascLib's CaptureGuardedBlock2 rejects BlockSize=0.  For empty
        // buckets we write BlockSize=1 so the guarded-block is accepted, but
        // 1/18=0 entries are processed (hash stays 0, LoadIndexItems loops 0
        // times).  This lets CascLib load all 16 buckets without hitting a gap.
        u32 guardedBlockSize = (dataSize > 0) ? dataSize : 1;

        // Build idx file: header area + entry data + padding to 4096
        // Layout: [headerDataSize:4][headerHash:4][headerFields:16][padding:8]
        //         [segmentSize:4][segmentHash:4][entryData...]
        size_t const entryDataStart = kIdxEntryDataOffset;
        size_t const fileSize = entryDataStart + dataSize;
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

        // Segment size at offset 32 (GuardedBlock2.BlockSize).
        std::memcpy(file.data() + 32, &guardedBlockSize, 4);

        // Write entries.
        for (size_t i = 0; i < numEntries; ++i) {
            u8* dst = file.data() + entryDataStart + i * kEntrySize;
            auto& e = *buckets[b][i];

            std::memcpy(dst, e.eKey.data(), kEKeyLen);

            u64 const offsetField = (u64(e.archiveIndex) << kHighBits) | u64(e.archiveOffset);
            // Write storage offset in big-endian (parser reads BE byte-by-byte).
            for (int j = kOffsetLen - 1; j >= 0; --j) {
                dst[kEKeyLen + (kOffsetLen - 1 - j)] = u8(offsetField >> (j * 8));
            }

            std::memcpy(dst + kEKeyLen + kOffsetLen, &e.encodedSize, kSizeLen);
        }

        // Compute and write inner checksum (Jenkins hashlittle of header data block).
        // GuardedBlock1: hashlittle(offset 8, headerDataSize, initval=0).
        {
            u32 pc = 0, pb = 0;
            common::jenkinsHashlittle2(file.data() + 8, headerDataSize, pc, pb);
            std::memcpy(file.data() + 4, &pc, 4);
        }

        // Segment hash at offset 36 (Jenkins hashlittle2 accumulated per-entry).
        // GuardedBlock2: for each entry, hashlittle2(entry, entryLen, &pc, &pb); store pc.
        {
            u32 pc = 0, pb = 0;
            for (size_t i = 0; i < numEntries; ++i) {
                common::jenkinsHashlittle2(file.data() + entryDataStart + i * kEntrySize,
                                           kEntrySize, pc, pb);
            }
            std::memcpy(file.data() + 36, &pc, 4);
        }

        // Footer hash (last 16 bytes): MD5 of entry data (used by archive index validation).
        auto dataMd5 = common::md5Hash(std::span(file.data() + entryDataStart, dataSize));
        std::memcpy(file.data() + padded - 16, dataMd5.data(), 16);

        // Filename: {bucket:02x}00000001.idx
        char name[32];
        std::snprintf(name, sizeof(name), "%02x00000001.idx", b);
        result.emplace_back(name, std::move(file));
    }

    return result;
}

} // namespace whiteout::storages::casc
