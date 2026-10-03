// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/storages/mpq/storage.h>

#include "../../common/unicode_path.h"
#include "../../storages/common/jenkins.h"
#include "../../storages/common/mapped_file.h"
#include "../../storages/mpq/crypto.h"
#include "../../storages/mpq/file_data.h"
#include "../../storages/mpq/special_files.h"
#include "../../storages/mpq/tables/block_table.h"
#include "../../storages/mpq/tables/hash_table.h"
#include "../../storages/mpq/tables/het_bet.h"
#include "../../storages/mpq/tables/header.h"
#include "../../storages/mpq/writer.h"

#include <whiteout/interfaces.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <shared_mutex>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace whiteout::storages::mpq {

using storages::common::normalizePath;

// ============================================================================
// Shared archive-parsing helper
// ============================================================================

namespace {

/// Result of parsing an MPQ archive from a memory-mapped file.
struct ParsedArchive {
    storages::common::MappedFile mapping;
    MpqHeader header;
    size_t archiveOffset = 0;
    HashTable hashTable;
    BlockTable blockTable;
    std::vector<std::string> listfileNames;
};

/// Map, parse header, decrypt tables, and read the listfile from an archive on disk.
/// Returns nullopt on any parse failure.  When @p error is non-null, stores a
/// diagnostic message explaining the first failure.
std::optional<ParsedArchive> parseMappedArchive(const std::string& path,
                                                std::string* error = nullptr) {
    auto setError = [&](const std::string& msg) {
        if (error)
            *error = msg;
    };

    auto mappedFile = storages::common::MappedFile::open(path);
    if (!mappedFile) {
        setError("failed to memory-map the file (file not found, empty, or permission denied)");
        return std::nullopt;
    }

    auto parseResult = findAndParseHeader(mappedFile->data());
    if (!parseResult) {
        setError("no valid MPQ header found (missing MPQ\\x1A signature or header too small)");
        return std::nullopt;
    }

    ParsedArchive pa;
    pa.header = parseResult->header;
    pa.archiveOffset = parseResult->archiveOffset;
    auto archiveSpan = mappedFile->data();

    // Parse hash table.
    u64 const htOffset = pa.archiveOffset + pa.header.hashTableByteOffset();
    u64 const htSize = static_cast<u64>(pa.header.hashTableEntries) * 16;
    if (htOffset + htSize > archiveSpan.size()) {
        setError("hash table extends past end of file (offset 0x" + std::to_string(htOffset) +
                 ", size " + std::to_string(htSize) + ", file size " +
                 std::to_string(archiveSpan.size()) + ")");
        return std::nullopt;
    }
    if (!pa.hashTable.parse(archiveSpan.subspan(htOffset, htSize), pa.header.hashTableEntries)) {
        setError("hash table decryption or validation failed");
        return std::nullopt;
    }

    // Parse block table.
    u64 const btOffset = pa.archiveOffset + pa.header.blockTableByteOffset();
    u64 const btSize = static_cast<u64>(pa.header.blockTableEntries) * 16;
    if (btOffset + btSize > archiveSpan.size()) {
        setError("block table extends past end of file (offset 0x" + std::to_string(btOffset) +
                 ", size " + std::to_string(btSize) + ", file size " +
                 std::to_string(archiveSpan.size()) + ")");
        return std::nullopt;
    }
    if (!pa.blockTable.parse(archiveSpan.subspan(btOffset, btSize), pa.header.blockTableEntries)) {
        setError("block table decryption or validation failed");
        return std::nullopt;
    }

    // Parse hi-block table (V2+).
    if (pa.header.formatVersion >= 1 && pa.header.hiBlockTableOffset != 0) {
        u64 const hiOffset = pa.archiveOffset + pa.header.hiBlockTableOffset;
        u64 const hiSize = static_cast<u64>(pa.header.blockTableEntries) * 2;
        if (hiOffset + hiSize <= archiveSpan.size()) {
            pa.blockTable.parseHiBlockTable(archiveSpan.subspan(hiOffset, hiSize),
                                            pa.header.blockTableEntries);
        }
    }

    // Read (listfile).
    auto lfIdx = pa.hashTable.lookup("(listfile)");
    if (lfIdx) {
        const auto& he = pa.hashTable.entry(*lfIdx);
        if (he.blockIndex < pa.blockTable.count()) {
            const auto& be = pa.blockTable.entry(he.blockIndex);
            u32 const fileKey = be.isEncrypted() ? deriveFileKey("(listfile)", be) : 0;
            auto lfData = extractFileData(mappedFile->data(), pa.archiveOffset, be,
                                          pa.header.sectorSize(), fileKey);
            if (!lfData.empty())
                pa.listfileNames = parseListfile(std::span<const u8>(lfData));
        }
    }

    pa.mapping = std::move(*mappedFile);
    return pa;
}

} // anonymous namespace

// ============================================================================
// Overlay key
// ============================================================================

struct OverlayKey {
    std::string normalizedName;
    u16 locale = 0;

    bool operator==(const OverlayKey& o) const {
        return normalizedName == o.normalizedName && locale == o.locale;
    }
};

struct OverlayKeyHash {
    size_t operator()(const OverlayKey& k) const {
        size_t const h1 = std::hash<std::string>{}(k.normalizedName);
        size_t const h2 = std::hash<u16>{}(k.locale);
        return h1 ^ (h2 * 0x9E3779B97F4A7C15ULL + 0x9E3779B9 + (h1 << 6) + (h1 >> 2));
    }
};

// ============================================================================
// Impl
// ============================================================================

struct Storage::Impl {
    // Source archive (read-only, may be empty for create()-d archives).
    std::optional<storages::common::MappedFile> sourceArchive;
    std::string sourcePath;

    // Parsed tables (read-only snapshot of source archive).
    MpqHeader header{};
    size_t archiveOffset = 0;
    HashTable hashTable;
    BlockTable blockTable;

    // Overlay — pending modifications.
    struct OverlayEntry {
        std::string originalName; // Preserves user-provided casing for the listfile.
        std::vector<u8> data;
        WriteOptions opts;
    };
    std::unordered_map<OverlayKey, OverlayEntry, OverlayKeyHash> pendingWrites;
    std::unordered_set<OverlayKey, OverlayKeyHash> pendingDeletes;

    // Parsed (listfile) from source archive.
    std::vector<std::string> sourceListfileNames;

    // Thread safety.
    mutable std::shared_mutex mutex;

    bool isValid = false;

    /// Why the last save() failed; empty after one that did not.
    std::string lastError;

    // Worker pool for parallel compression/decompression (non-owning, may be null).
    interfaces::WorkerPool* pool = nullptr;

    // -- Helpers --

    /// Extract a file from the source archive by name, with optional locale filter.
    std::optional<std::vector<u8>> extractFromSource(const std::string& name,
                                                     std::optional<u16> locale = std::nullopt,
                                                     std::string* error = nullptr) const {
        if (!sourceArchive) {
            if (error)
                *error = "no source archive";
            return std::nullopt;
        }

        auto idx = locale ? hashTable.lookup(name, *locale) : hashTable.lookup(name);
        if (!idx) {
            if (error)
                *error = "not found in hash table";
            return std::nullopt;
        }

        const auto& he = hashTable.entry(*idx);
        if (he.blockIndex >= blockTable.count()) {
            if (error)
                *error = "block index out of range";
            return std::nullopt;
        }
        const auto& be = blockTable.entry(he.blockIndex);

        u32 fileKey = 0;
        if (be.isEncrypted()) {
            fileKey = deriveFileKey(name, be);
        }

        return extractFileData(sourceArchive->data(), archiveOffset, be, header.sectorSize(),
                               fileKey, error, pool);
    }

    /// Shared readFile logic. Caller must hold at least a shared lock on mutex.
    std::optional<std::vector<u8>> readFileCore(const std::string& name, std::optional<u16> locale,
                                                std::string* error) const {
        std::string const norm = normalizePath(name);
        OverlayKey const key{norm, locale.value_or(Locale::Neutral)};

        if (pendingDeletes.contains(key)) {
            if (error)
                *error = "file deleted in overlay";
            return std::nullopt;
        }

        auto it = pendingWrites.find(key);
        if (it != pendingWrites.end()) {
            return it->second.data;
        }

        return extractFromSource(name, locale, error);
    }

    /// The writer's input for a save: every source file still standing, in
    /// block order and at its hash slot, then the overlay. Returns why there
    /// is none, or an empty string.
    ///
    /// A source file is found through its hash table slot rather than its
    /// listfile line, so a file no listfile names survives the save too; it
    /// just cannot move slots. (listfile), (attributes) and (signature) are
    /// written again rather than copied, the last not at all: it signs bytes
    /// that no longer exist.
    std::string buildWriteJob(WritePlan& plan, std::vector<WriteEntry>& entries) const {
        plan.header = header;
        plan.hashTableCapacity = header.hashTableEntries;
        plan.singleUnitSpecials = header.formatVersion >= 2;

        auto nameKey = [](const std::string& name) {
            return (static_cast<u64>(hashString(name, HashType::NameA)) << 32) |
                   hashString(name, HashType::NameB);
        };
        // A name keeps its (listfile) line; a new one goes after them all.
        std::unordered_map<u64, u32> ranks;
        for (const auto& name : sourceListfileNames)
            ranks.try_emplace(nameKey(name), static_cast<u32>(ranks.size()));
        u32 const appended = static_cast<u32>(ranks.size());
        auto rankOf = [&](u64 key) {
            auto it = ranks.find(key);
            return it != ranks.end() ? it->second : appended;
        };
        // Names the overlay writes or deletes: every locale of one goes, so a
        // replaced file is not shadowed by a localized copy of the old one.
        std::unordered_set<u64> replaced;
        for (const auto& [key, val] : pendingWrites)
            replaced.insert(nameKey(key.normalizedName));
        for (const auto& key : pendingDeletes)
            replaced.insert(nameKey(key.normalizedName));

        if (sourceArchive) {
            std::unordered_map<u64, std::string> names;
            for (const auto& name : sourceListfileNames)
                names.try_emplace(nameKey(name), name);
            std::unordered_set<u64> const specials{nameKey("(listfile)"), nameKey("(attributes)"),
                                                   nameKey("(signature)")};

            // The source's (attributes), carried per block; written again
            // with the same arrays, or not at all when it had none.
            FileAttributes carried;
            plan.attributes = false;
            if (hashTable.lookup("(attributes)")) {
                auto data = extractFromSource("(attributes)");
                if (data && data->size() >= 8) {
                    u32 flags = 0;
                    std::memcpy(&flags, data->data() + 4, 4);
                    plan.attributes = true;
                    plan.attributeFlags = static_cast<AttributeFlag>(flags) &
                                          (AttributeFlag::kCrc32 | AttributeFlag::kFiletime | AttributeFlag::kMd5);
                    carried = parseAttributes(*data, blockTable.count());
                }
            }

            // A v3+ archive's HET/BET tables are rebuilt; a file no listfile
            // names keeps the name hash they gave it.
            std::vector<u64> blockHashes;
            if (header.formatVersion >= 2 && header.hetTableOffset != 0 && header.betTableOffset != 0) {
                plan.hetBet = true;
                auto const bytes = sourceArchive->data();
                u64 const hetAt = archiveOffset + header.hetTableOffset;
                u64 const betAt = archiveOffset + header.betTableOffset;
                if (hetAt < bytes.size() && betAt < bytes.size()) {
                    auto het = parseHetTable(bytes.subspan(hetAt));
                    auto bet = parseBetTable(bytes.subspan(betAt));
                    if (het && bet)
                        blockHashes = blockNameHashes(*het, *bet);
                }
            }

            std::vector<std::pair<u32, u32>> order; // (block, slot)
            for (u32 slot = 0; slot < hashTable.capacity(); ++slot) {
                const HashEntry& he = hashTable.entry(slot);
                if (he.isOccupied() && he.blockIndex < blockTable.count() &&
                    blockTable.entry(he.blockIndex).exists())
                    order.emplace_back(he.blockIndex, slot);
            }
            std::sort(order.begin(), order.end());

            for (const auto& [block, slot] : order) {
                const HashEntry& he = hashTable.entry(slot);
                u64 const key = (static_cast<u64>(he.hashA) << 32) | he.hashB;
                if (specials.contains(key) || replaced.contains(key))
                    continue;
                const BlockEntry& be = blockTable.entry(block);

                WriteEntry we;
                if (auto it = names.find(key); it != names.end())
                    we.filename = it->second;
                we.listRank = rankOf(key);
                we.locale = he.locale;
                we.platform = he.platform;
                we.hashA = he.hashA;
                we.hashB = he.hashB;
                we.sourceSlot = slot;
                if (we.filename.empty() && block < blockHashes.size())
                    we.nameHash = blockHashes[block];
                if (block < carried.crc32s.size())
                    we.crc32 = carried.crc32s[block];
                if (block < carried.filetimes.size())
                    we.filetime = carried.filetimes[block];
                if (block < carried.md5s.size())
                    we.md5 = carried.md5s[block];

                u64 const start = archiveOffset + blockTable.fileOffset48(block);
                if (start + be.compressedSize > sourceArchive->data().size())
                    return "'" + (we.filename.empty() ? std::string("a file") : we.filename) +
                           "' lies past the end of the archive";
                we.rawCopy = true;
                we.rawSectors = sourceArchive->data().subspan(start, be.compressedSize);
                we.sourceBlock = be;
                if (be.isEncrypted() && be.hasFixKey()) {
                    // Its key moves with its offset: the name gives the key,
                    // and without one a sector table still does.
                    std::optional<u32> fileKey;
                    if (!we.filename.empty())
                        fileKey = deriveFileKey(we.filename, be);
                    else
                        fileKey = detectStoredFileKey(we.rawSectors, be, header.sectorSize());
                    if (!fileKey)
                        return "an encrypted file no (listfile) names is keyed to its position and cannot be moved";
                    we.rekeyBase = (*fileKey ^ be.uncompressedSize) - be.fileOffset;
                }
                entries.push_back(std::move(we));
            }
        }

        // The overlay, in name order so a save is reproducible.
        std::vector<const std::pair<const OverlayKey, OverlayEntry>*> overlay;
        for (const auto& item : pendingWrites)
            overlay.push_back(&item);
        std::sort(overlay.begin(), overlay.end(), [](const auto* a, const auto* b) {
            return std::tie(a->first.normalizedName, a->first.locale) <
                   std::tie(b->first.normalizedName, b->first.locale);
        });
        for (const auto* item : overlay) {
            WriteEntry we;
            we.filename = item->second.originalName;
            we.locale = item->first.locale;
            we.listRank = rankOf(nameKey(item->second.originalName));
            we.rawData = item->second.data;
            we.compression = static_cast<CompressionFlag>(item->second.opts.compression);
            we.encrypt = item->second.opts.encrypt;
            we.singleUnit = item->second.opts.singleUnit;
            entries.push_back(std::move(we));
        }
        return {};
    }

    /// Reset state to invalid (used when save fails after overwriting).
    void invalidate() {
        sourceArchive.reset();
        hashTable = HashTable{};
        blockTable = BlockTable{};
        sourceListfileNames.clear();
        pendingWrites.clear();
        pendingDeletes.clear();
        isValid = false;
    }

    /// Apply the results of parseMappedArchive() into this Impl, setting it as
    /// the new source archive.  Clears any pending overlay.
    void applyParsedArchive(ParsedArchive& pa, const std::string& path) {
        header = pa.header;
        archiveOffset = pa.archiveOffset;
        sourcePath = path;
        hashTable = std::move(pa.hashTable);
        blockTable = std::move(pa.blockTable);
        sourceArchive = std::move(pa.mapping);
        sourceListfileNames = std::move(pa.listfileNames);
        isValid = true;
        pendingWrites.clear();
        pendingDeletes.clear();
    }

    /// Re-map and re-parse an archive from disk, replacing current state.
    /// Clears overlay on success. Returns false on any parse failure.
    bool reloadFromDisk(const std::string& path) {
        auto pa = parseMappedArchive(path);
        if (!pa)
            return false;
        applyParsedArchive(*pa, path);
        return true;
    }
};

// ============================================================================
// Storage — Construction / Destruction
// ============================================================================

Storage::Storage() : m_impl(std::make_unique<Impl>()) {}
Storage::~Storage() = default;
Storage::Storage(Storage&& other) noexcept = default;
Storage& Storage::operator=(Storage&& other) noexcept = default;

// ============================================================================
// Static Factories
// ============================================================================

std::optional<Storage> Storage::open(const std::string& path, interfaces::WorkerPool* pool) {
    return open(path, nullptr, pool);
}

std::optional<Storage> Storage::open(const std::string& path, std::string* error,
                                     interfaces::WorkerPool* pool) {
    auto pa = parseMappedArchive(path, error);
    if (!pa)
        return std::nullopt;

    Storage storage;
    storage.m_impl->applyParsedArchive(*pa, path);
    storage.m_impl->pool = pool;
    return storage;
}

Storage Storage::create(CreateOptions opts, interfaces::WorkerPool* pool) {
    Storage storage;
    auto& impl = *storage.m_impl;

    impl.header =
        buildHeader(static_cast<u16>(opts.version), opts.hashTableSize, opts.sectorSizeShift);
    impl.isValid = true;
    impl.pool = pool;
    return storage;
}

// ============================================================================
// Lifetime
// ============================================================================

void Storage::close() {
    if (m_impl) {
        std::unique_lock const lock(m_impl->mutex);
        m_impl->invalidate();
    }
}

Storage::operator bool() const noexcept {
    return m_impl && m_impl->isValid;
}

// ============================================================================
// Read Operations
// ============================================================================

std::optional<std::vector<u8>> Storage::readFile(const std::string& name) const {
    if (!m_impl || !m_impl->isValid)
        return std::nullopt;
    std::shared_lock const lock(m_impl->mutex);
    return m_impl->readFileCore(name, std::nullopt, nullptr);
}

std::optional<std::vector<u8>> Storage::readFile(const std::string& name,
                                                 std::string* error) const {
    if (!m_impl || !m_impl->isValid) {
        if (error)
            *error = "storage not open";
        return std::nullopt;
    }
    std::shared_lock const lock(m_impl->mutex);
    return m_impl->readFileCore(name, std::nullopt, error);
}

std::optional<std::vector<u8>> Storage::readFile(const std::string& name, u16 locale) const {
    if (!m_impl || !m_impl->isValid)
        return std::nullopt;
    std::shared_lock const lock(m_impl->mutex);
    return m_impl->readFileCore(name, std::optional<u16>(locale), nullptr);
}

bool Storage::fileExists(const std::string& name) const {
    if (!m_impl || !m_impl->isValid)
        return false;
    std::shared_lock const lock(m_impl->mutex);

    std::string const norm = normalizePath(name);
    OverlayKey const key{norm, Locale::Neutral};

    if (m_impl->pendingDeletes.contains(key))
        return false;
    if (m_impl->pendingWrites.contains(key))
        return true;

    return m_impl->hashTable.lookup(name).has_value();
}

std::optional<FileInfo> Storage::fileInfo(const std::string& name) const {
    if (!m_impl || !m_impl->isValid)
        return std::nullopt;
    std::shared_lock const lock(m_impl->mutex);

    std::string const norm = normalizePath(name);
    OverlayKey const key{norm, Locale::Neutral};

    if (m_impl->pendingDeletes.contains(key))
        return std::nullopt;

    // Check overlay.
    auto it = m_impl->pendingWrites.find(key);
    if (it != m_impl->pendingWrites.end()) {
        FileInfo info;
        info.name = name;
        info.uncompressedSize = static_cast<u32>(it->second.data.size());
        info.compressedSize = info.uncompressedSize; // Not compressed yet.
        info.locale = it->first.locale;
        info.flags = FileFlags::Exists;
        return info;
    }

    // Source archive.
    auto idx = m_impl->hashTable.lookup(name);
    if (!idx)
        return std::nullopt;

    const auto& he = m_impl->hashTable.entry(*idx);
    if (he.blockIndex >= m_impl->blockTable.count())
        return std::nullopt;
    const auto& be = m_impl->blockTable.entry(he.blockIndex);

    FileInfo info;
    info.name = name;
    info.compressedSize = be.compressedSize;
    info.uncompressedSize = be.uncompressedSize;
    info.flags = static_cast<FileFlags>(static_cast<u32>(be.flags));
    info.locale = he.locale;
    return info;
}

ArchiveInfo Storage::archiveInfo() const {
    if (!m_impl || !m_impl->isValid)
        return {};
    std::shared_lock const lock(m_impl->mutex);

    ArchiveInfo info;
    info.formatVersion = m_impl->header.formatVersion;
    info.hashTableEntries = m_impl->header.hashTableEntries;
    info.blockTableEntries = m_impl->header.blockTableEntries;
    info.sectorSize = m_impl->header.sectorSize();
    info.archiveSize = (m_impl->header.formatVersion >= 2) ? m_impl->header.archiveSize64
                                                           : m_impl->header.archiveSize;
    return info;
}

std::vector<std::string> Storage::listFiles() const {
    if (!m_impl || !m_impl->isValid)
        return {};
    std::shared_lock const lock(m_impl->mutex);

    // Build normalized delete set.
    std::unordered_set<std::string> deleteSet;
    for (const auto& dk : m_impl->pendingDeletes) {
        deleteSet.insert(dk.normalizedName);
    }

    std::unordered_set<std::string> seen;
    std::vector<std::string> result;

    // Source listfile names.
    for (const auto& name : m_impl->sourceListfileNames) {
        std::string const norm = normalizePath(name);
        if (deleteSet.contains(norm))
            continue;
        if (seen.insert(norm).second) {
            result.push_back(name);
        }
    }

    // Overlay additions.
    for (const auto& [key, val] : m_impl->pendingWrites) {
        if (seen.insert(key.normalizedName).second) {
            result.push_back(val.originalName);
        }
    }

    return result;
}

void Storage::enumerate(std::function<bool(const std::string&)> callback) const {
    auto files = listFiles();
    for (const auto& name : files) {
        if (!callback(name))
            break;
    }
}

// ============================================================================
// Write Operations
// ============================================================================

bool Storage::writeFile(const std::string& name, std::span<const u8> data, WriteOptions opts) {
    if (!m_impl || !m_impl->isValid)
        return false;
    std::unique_lock const lock(m_impl->mutex);

    std::string const norm = normalizePath(name);
    OverlayKey const key{norm, opts.locale};

    // Remove from deletes if present.
    m_impl->pendingDeletes.erase(key);

    // Store in overlay.
    m_impl->pendingWrites[key] = {name, std::vector<u8>(data.begin(), data.end()), opts};

    return true;
}

bool Storage::deleteFile(const std::string& name) {
    if (!m_impl || !m_impl->isValid)
        return false;
    std::unique_lock const lock(m_impl->mutex);

    std::string const norm = normalizePath(name);
    OverlayKey const key{norm, Locale::Neutral};

    // Check if it exists in overlay or source.
    bool const exists =
        m_impl->pendingWrites.contains(key) || m_impl->hashTable.lookup(name).has_value();
    if (!exists)
        return false;

    // Remove from writes if present.
    m_impl->pendingWrites.erase(key);

    // Add to deletes.
    m_impl->pendingDeletes.insert(key);
    return true;
}

// ============================================================================
// Save
// ============================================================================

bool Storage::save() {
    if (!m_impl || !m_impl->isValid)
        return false;
    if (m_impl->sourcePath.empty())
        return false; // Created via create(), no path.
    return save(m_impl->sourcePath);
}

bool Storage::save(const std::string& path) {
    if (!m_impl || !m_impl->isValid)
        return false;
    std::unique_lock const lock(m_impl->mutex);
    m_impl->lastError.clear();
    auto fail = [&](std::string why) {
        m_impl->lastError = std::move(why);
        return false;
    };

    bool const isSamePath = (path == m_impl->sourcePath);

    // Build the writer's input from source + overlay.
    WritePlan plan;
    std::vector<WriteEntry> entries;
    if (std::string why = m_impl->buildWriteJob(plan, entries); !why.empty())
        return fail(std::move(why));
    if (m_impl->sourceArchive)
        plan.sourceHashTable = &m_impl->hashTable;

    // Write the archive.
    auto written = writeArchive(plan, entries, m_impl->pool);
    if (!written.error.empty())
        return fail(std::move(written.error));
    if (written.archive.empty())
        return fail("the archive could not be assembled");

    // Whatever precedes the MPQ header -- a Warcraft III map's 512-byte HM3W
    // block, a user data header -- is kept byte for byte, so every offset in
    // it still lands on the header. A trailing signature is not: it signed
    // the old bytes.
    std::span<const u8> preamble;
    if (m_impl->sourceArchive)
        preamble = m_impl->sourceArchive->data().subspan(0, m_impl->archiveOffset);

    // Determine output path.  When overwriting the mapped source, write to a
    // temp file first, then atomically rename after unmapping.
    std::string outputPath = path;
    std::string tempPath;
    bool const useTempFile = isSamePath && m_impl->sourceArchive;

    if (useTempFile) {
        tempPath = path + ".tmp";
        outputPath = tempPath;
    }

    // Write archive data to disk.
    {
        auto out = whiteout::common::open_ofstream(outputPath, std::ios::binary | std::ios::trunc);
        if (!out)
            return fail("could not open '" + outputPath + "' for writing");
        out.write(reinterpret_cast<const char*>(preamble.data()), static_cast<std::streamsize>(preamble.size()));
        out.write(reinterpret_cast<const char*>(written.archive.data()),
                  static_cast<std::streamsize>(written.archive.size()));
        if (!out) {
            out.close();
            if (useTempFile) {
                std::error_code ec;
                std::filesystem::remove(whiteout::common::utf8_to_path(tempPath), ec);
            }
            return fail("could not write '" + outputPath + "'");
        }
    }

    if (useTempFile) {
        m_impl->sourceArchive.reset();

        std::error_code ec;
        std::filesystem::rename(whiteout::common::utf8_to_path(tempPath), whiteout::common::utf8_to_path(path), ec);
        if (ec) {
            m_impl->sourceArchive = storages::common::MappedFile::open(m_impl->sourcePath);
            std::error_code removeEc;
            std::filesystem::remove(whiteout::common::utf8_to_path(tempPath), removeEc);
            return fail("could not replace '" + path + "': " + ec.message());
        }
    }

    // Re-open the saved archive as the new source.
    if (!m_impl->reloadFromDisk(path)) {
        if (useTempFile)
            m_impl->invalidate();
        return fail("the saved archive could not be read back");
    }

    return true;
}

std::string Storage::lastError() const {
    if (!m_impl)
        return {};
    std::shared_lock const lock(m_impl->mutex);
    return m_impl->lastError;
}

} // namespace whiteout::storages::mpq
