// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/storages/mpq/storage.h>

#include "../../storages/common/jenkins.h"
#include "../../storages/common/mapped_file.h"
#include "../../storages/mpq/tables/block_table.h"
#include "../../storages/mpq/crypto.h"
#include "../../storages/mpq/file_data.h"
#include "../../storages/mpq/tables/hash_table.h"
#include "../../storages/mpq/tables/header.h"
#include "../../storages/mpq/special_files.h"
#include "../../storages/mpq/writer.h"

#include <whiteout/interfaces.h>

#include <filesystem>
#include <fstream>
#include <shared_mutex>
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
    u64 htOffset = pa.archiveOffset + pa.header.hashTableByteOffset();
    u64 htSize = static_cast<u64>(pa.header.hashTableEntries) * 16;
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
    u64 btOffset = pa.archiveOffset + pa.header.blockTableByteOffset();
    u64 btSize = static_cast<u64>(pa.header.blockTableEntries) * 16;
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
        u64 hiOffset = pa.archiveOffset + pa.header.hiBlockTableOffset;
        u64 hiSize = static_cast<u64>(pa.header.blockTableEntries) * 2;
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
            auto lfData =
                extractFileData(mappedFile->data(), pa.archiveOffset,
                                pa.blockTable.entry(he.blockIndex), pa.header.sectorSize(), 0);
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
        size_t h1 = std::hash<std::string>{}(k.normalizedName);
        size_t h2 = std::hash<u16>{}(k.locale);
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
        std::string norm = normalizePath(name);
        OverlayKey key{norm, locale.value_or(Locale::Neutral)};

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

    /// Build WriteEntry list for the writer — merges source files + overlay.
    std::vector<WriteEntry> buildWriteEntries() const {
        std::vector<WriteEntry> entries;

        // Normalized pending-delete set for fast lookup.
        std::unordered_set<std::string> deleteSet;
        for (const auto& dk : pendingDeletes) {
            deleteSet.insert(dk.normalizedName);
        }

        // Normalized pending-write keys for checking "already in overlay".
        std::unordered_set<std::string> writeNameSet;
        for (const auto& [key, val] : pendingWrites) {
            writeNameSet.insert(key.normalizedName);
        }

        // 1. Source files (from listfile).
        for (const auto& name : sourceListfileNames) {
            std::string norm = normalizePath(name);
            if (deleteSet.contains(norm))
                continue; // Deleted in overlay.

            if (writeNameSet.contains(norm)) {
                // Overwritten in overlay — will be added in step 2.
                continue;
            }

            // Raw copy from source.
            auto idx = hashTable.lookup(name);
            if (!idx)
                continue;
            const auto& he = hashTable.entry(*idx);
            if (he.blockIndex >= blockTable.count())
                continue;
            const auto& be = blockTable.entry(he.blockIndex);

            if (!be.exists())
                continue;

            // Get raw sector data span from source archive.
            u64 dataStart = archiveOffset + be.fileOffset;
            if (dataStart + be.compressedSize <= sourceArchive->data().size()) {
                WriteEntry we;
                we.filename = name;
                we.locale = he.locale;
                we.rawSectors = sourceArchive->data().subspan(dataStart, be.compressedSize);
                we.sourceBlock = be;
                entries.push_back(std::move(we));
            }
        }

        // 2. Overlay writes (new + modified files).
        for (const auto& [key, val] : pendingWrites) {
            WriteEntry we;
            we.filename = val.originalName;
            we.locale = key.locale;
            we.rawData = val.data;
            we.compression = static_cast<CompressionFlag>(val.opts.compression);
            we.encrypt = val.opts.encrypt;
            we.singleUnit = val.opts.singleUnit;
            entries.push_back(std::move(we));
        }

        return entries;
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
        std::unique_lock lock(m_impl->mutex);
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
    std::shared_lock lock(m_impl->mutex);
    return m_impl->readFileCore(name, std::nullopt, nullptr);
}

std::optional<std::vector<u8>> Storage::readFile(const std::string& name,
                                                 std::string* error) const {
    if (!m_impl || !m_impl->isValid) {
        if (error)
            *error = "storage not open";
        return std::nullopt;
    }
    std::shared_lock lock(m_impl->mutex);
    return m_impl->readFileCore(name, std::nullopt, error);
}

std::optional<std::vector<u8>> Storage::readFile(const std::string& name, u16 locale) const {
    if (!m_impl || !m_impl->isValid)
        return std::nullopt;
    std::shared_lock lock(m_impl->mutex);
    return m_impl->readFileCore(name, std::optional<u16>(locale), nullptr);
}

bool Storage::fileExists(const std::string& name) const {
    if (!m_impl || !m_impl->isValid)
        return false;
    std::shared_lock lock(m_impl->mutex);

    std::string norm = normalizePath(name);
    OverlayKey key{norm, Locale::Neutral};

    if (m_impl->pendingDeletes.contains(key))
        return false;
    if (m_impl->pendingWrites.contains(key))
        return true;

    return m_impl->hashTable.lookup(name).has_value();
}

std::optional<FileInfo> Storage::fileInfo(const std::string& name) const {
    if (!m_impl || !m_impl->isValid)
        return std::nullopt;
    std::shared_lock lock(m_impl->mutex);

    std::string norm = normalizePath(name);
    OverlayKey key{norm, Locale::Neutral};

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
    std::shared_lock lock(m_impl->mutex);

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
    std::shared_lock lock(m_impl->mutex);

    // Build normalized delete set.
    std::unordered_set<std::string> deleteSet;
    for (const auto& dk : m_impl->pendingDeletes) {
        deleteSet.insert(dk.normalizedName);
    }

    std::unordered_set<std::string> seen;
    std::vector<std::string> result;

    // Source listfile names.
    for (const auto& name : m_impl->sourceListfileNames) {
        std::string norm = normalizePath(name);
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
    std::unique_lock lock(m_impl->mutex);

    std::string norm = normalizePath(name);
    OverlayKey key{norm, opts.locale};

    // Remove from deletes if present.
    m_impl->pendingDeletes.erase(key);

    // Store in overlay.
    m_impl->pendingWrites[key] = {name, std::vector<u8>(data.begin(), data.end()), opts};

    return true;
}

bool Storage::deleteFile(const std::string& name) {
    if (!m_impl || !m_impl->isValid)
        return false;
    std::unique_lock lock(m_impl->mutex);

    std::string norm = normalizePath(name);
    OverlayKey key{norm, Locale::Neutral};

    // Check if it exists in overlay or source.
    bool exists = m_impl->pendingWrites.contains(key) || m_impl->hashTable.lookup(name).has_value();
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
    std::unique_lock lock(m_impl->mutex);

    bool isSamePath = (path == m_impl->sourcePath);

    // Build write entries from source + overlay.
    auto entries = m_impl->buildWriteEntries();

    // Write the archive.
    auto archiveData =
        writeArchive(m_impl->header, entries, m_impl->header.hashTableEntries, m_impl->pool);

    if (archiveData.empty())
        return false;

    // Determine output path.  When overwriting the mapped source, write to a
    // temp file first, then atomically rename after unmapping.
    std::string outputPath = path;
    std::string tempPath;
    bool useTempFile = isSamePath && m_impl->sourceArchive;

    if (useTempFile) {
        tempPath = path + ".tmp";
        outputPath = tempPath;
    }

    // Write archive data to disk.
    {
        std::ofstream out(outputPath, std::ios::binary | std::ios::trunc);
        if (!out)
            return false;
        out.write(reinterpret_cast<const char*>(archiveData.data()),
                  static_cast<std::streamsize>(archiveData.size()));
        if (!out) {
            if (useTempFile) {
                std::error_code ec;
                std::filesystem::remove(tempPath, ec);
            }
            return false;
        }
    }

    if (useTempFile) {
        m_impl->sourceArchive.reset();

        std::error_code ec;
        std::filesystem::rename(tempPath, path, ec);
        if (ec) {
            m_impl->sourceArchive = storages::common::MappedFile::open(m_impl->sourcePath);
            std::filesystem::remove(tempPath, ec);
            return false;
        }
    }

    // Re-open the saved archive as the new source.
    if (!m_impl->reloadFromDisk(path)) {
        if (useTempFile)
            m_impl->invalidate();
        return false;
    }

    return true;
}

} // namespace whiteout::storages::mpq
