// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/storages/mpq/storage.h>

#include "../../storages/common/mapped_file.h"
#include "../../storages/common/jenkins.h"
#include "../../storages/mpq/block_table.h"
#include "../../storages/mpq/crypto.h"
#include "../../storages/mpq/file_data.h"
#include "../../storages/mpq/hash_table.h"
#include "../../storages/mpq/header.h"
#include "../../storages/mpq/special_files.h"
#include "../../storages/mpq/writer.h"

#include <whiteout/interfaces.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>

namespace whiteout::storages::mpq {

using storages::common::normalizePath;

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

    /// Extract a file from the source archive by name.
    std::optional<std::vector<u8>> extractFromSource(const std::string& name,
                                                      std::string* error = nullptr) const {
        if (!sourceArchive) { if (error) *error = "no source archive"; return std::nullopt; }

        auto idx = hashTable.lookup(name);
        if (!idx) { if (error) *error = "not found in hash table"; return std::nullopt; }

        const auto& he = hashTable.entry(*idx);
        if (he.blockIndex >= blockTable.count()) {
            if (error) *error = "block index out of range";
            return std::nullopt;
        }
        const auto& be = blockTable.entry(he.blockIndex);

        u32 fileKey = 0;
        if (be.isEncrypted()) {
            // Derive file key from the filename.
            fileKey = deriveFileKey(name, be);
        }

        return extractFileData(
            sourceArchive->data(),
            archiveOffset,
            be,
            header.sectorSize(),
            fileKey,
            error,
            pool);
    }

    /// Extract a file by name + locale.
    std::optional<std::vector<u8>> extractFromSource(const std::string& name, u16 locale) const {
        if (!sourceArchive) return std::nullopt;

        auto idx = hashTable.lookup(name, locale);
        if (!idx) return std::nullopt;

        const auto& he = hashTable.entry(*idx);
        if (he.blockIndex >= blockTable.count()) return std::nullopt;
        const auto& be = blockTable.entry(he.blockIndex);

        u32 fileKey = 0;
        if (be.isEncrypted()) {
            fileKey = deriveFileKey(name, be);
        }

        return extractFileData(
            sourceArchive->data(),
            archiveOffset,
            be,
            header.sectorSize(),
            fileKey,
            nullptr,
            pool);
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
            if (deleteSet.count(norm)) continue; // Deleted in overlay.

            if (writeNameSet.count(norm)) {
                // Overwritten in overlay — will be added in step 2.
                continue;
            }

            // Raw copy from source.
            auto idx = hashTable.lookup(name);
            if (!idx) continue;
            const auto& he = hashTable.entry(*idx);
            if (he.blockIndex >= blockTable.count()) continue;
            const auto& be = blockTable.entry(he.blockIndex);

            if (!be.exists()) continue;

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
            we.compression = static_cast<u8>(val.opts.compression);
            we.encrypt = val.opts.encrypt;
            we.singleUnit = val.opts.singleUnit;
            entries.push_back(std::move(we));
        }

        return entries;
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

std::optional<Storage> Storage::open(const std::string& path,
                                      interfaces::WorkerPool* pool) {
    return open(path, nullptr, pool);
}

std::optional<Storage> Storage::open(const std::string& path, std::string* error,
                                      interfaces::WorkerPool* pool) {
    auto setError = [&](const std::string& msg) {
        if (error) *error = msg;
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

    Storage storage;
    auto& impl = *storage.m_impl;
    impl.header = parseResult->header;
    impl.archiveOffset = parseResult->archiveOffset;
    impl.sourcePath = path;

    auto archiveSpan = mappedFile->data();

    // Parse hash table.
    u64 htOffset = impl.archiveOffset + impl.header.hashTableByteOffset();
    u64 htSize = static_cast<u64>(impl.header.hashTableEntries) * 16;
    if (htOffset + htSize > archiveSpan.size()) {
        setError("hash table extends past end of file (offset 0x"
                 + std::to_string(htOffset) + ", size " + std::to_string(htSize)
                 + ", file size " + std::to_string(archiveSpan.size()) + ")");
        return std::nullopt;
    }
    if (!impl.hashTable.parse(archiveSpan.subspan(htOffset, htSize), impl.header.hashTableEntries)) {
        setError("hash table decryption or validation failed");
        return std::nullopt;
    }

    // Parse block table.
    u64 btOffset = impl.archiveOffset + impl.header.blockTableByteOffset();
    u64 btSize = static_cast<u64>(impl.header.blockTableEntries) * 16;
    if (btOffset + btSize > archiveSpan.size()) {
        setError("block table extends past end of file (offset 0x"
                 + std::to_string(btOffset) + ", size " + std::to_string(btSize)
                 + ", file size " + std::to_string(archiveSpan.size()) + ")");
        return std::nullopt;
    }
    if (!impl.blockTable.parse(archiveSpan.subspan(btOffset, btSize), impl.header.blockTableEntries)) {
        setError("block table decryption or validation failed");
        return std::nullopt;
    }

    // Parse hi-block table (V2+).
    if (impl.header.formatVersion >= 1 && impl.header.hiBlockTableOffset != 0) {
        u64 hiOffset = impl.archiveOffset + impl.header.hiBlockTableOffset;
        u64 hiSize = static_cast<u64>(impl.header.blockTableEntries) * 2;
        if (hiOffset + hiSize <= archiveSpan.size()) {
            impl.blockTable.parseHiBlockTable(
                archiveSpan.subspan(hiOffset, hiSize),
                impl.header.blockTableEntries);
        }
    }

    impl.sourceArchive = std::move(mappedFile);

    // Try to read (listfile).
    auto listfileData = impl.extractFromSource("(listfile)");
    if (listfileData) {
        impl.sourceListfileNames = parseListfile(std::span<const u8>(*listfileData));
    }

    impl.isValid = true;
    impl.pool = pool;
    return storage;
}

Storage Storage::create(CreateOptions opts, interfaces::WorkerPool* pool) {
    Storage storage;
    auto& impl = *storage.m_impl;

    impl.header = buildHeader(
        static_cast<u16>(opts.version),
        opts.hashTableSize,
        opts.sectorSizeShift);
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
        m_impl->sourceArchive.reset();
        m_impl->pendingWrites.clear();
        m_impl->pendingDeletes.clear();
        m_impl->sourceListfileNames.clear();
        m_impl->isValid = false;
    }
}

Storage::operator bool() const noexcept {
    return m_impl && m_impl->isValid;
}

// ============================================================================
// Read Operations
// ============================================================================

std::optional<std::vector<u8>> Storage::readFile(const std::string& name) const {
    if (!m_impl || !m_impl->isValid) return std::nullopt;
    std::shared_lock lock(m_impl->mutex);

    std::string norm = normalizePath(name);
    OverlayKey key{norm, Locale::Neutral};

    // Check deletions.
    if (m_impl->pendingDeletes.count(key)) return std::nullopt;

    // Check overlay writes.
    auto it = m_impl->pendingWrites.find(key);
    if (it != m_impl->pendingWrites.end()) {
        return it->second.data; // Return overlay data.
    }

    // Fall through to source archive.
    return m_impl->extractFromSource(name);
}

std::optional<std::vector<u8>> Storage::readFile(const std::string& name, std::string* error) const {
    if (!m_impl || !m_impl->isValid) {
        if (error) *error = "storage not open";
        return std::nullopt;
    }
    std::shared_lock lock(m_impl->mutex);

    std::string norm = normalizePath(name);
    OverlayKey key{norm, Locale::Neutral};

    if (m_impl->pendingDeletes.count(key)) {
        if (error) *error = "file deleted in overlay";
        return std::nullopt;
    }

    auto it = m_impl->pendingWrites.find(key);
    if (it != m_impl->pendingWrites.end()) {
        return it->second.data;
    }

    return m_impl->extractFromSource(name, error);
}

std::optional<std::vector<u8>> Storage::readFile(const std::string& name, u16 locale) const {
    if (!m_impl || !m_impl->isValid) return std::nullopt;
    std::shared_lock lock(m_impl->mutex);

    std::string norm = normalizePath(name);
    OverlayKey key{norm, locale};

    if (m_impl->pendingDeletes.count(key)) return std::nullopt;

    auto it = m_impl->pendingWrites.find(key);
    if (it != m_impl->pendingWrites.end()) {
        return it->second.data;
    }

    return m_impl->extractFromSource(name, locale);
}

bool Storage::fileExists(const std::string& name) const {
    if (!m_impl || !m_impl->isValid) return false;
    std::shared_lock lock(m_impl->mutex);

    std::string norm = normalizePath(name);
    OverlayKey key{norm, Locale::Neutral};

    if (m_impl->pendingDeletes.count(key)) return false;
    if (m_impl->pendingWrites.count(key)) return true;

    return m_impl->hashTable.lookup(name).has_value();
}

std::optional<FileInfo> Storage::fileInfo(const std::string& name) const {
    if (!m_impl || !m_impl->isValid) return std::nullopt;
    std::shared_lock lock(m_impl->mutex);

    std::string norm = normalizePath(name);
    OverlayKey key{norm, Locale::Neutral};

    if (m_impl->pendingDeletes.count(key)) return std::nullopt;

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
    if (!idx) return std::nullopt;

    const auto& he = m_impl->hashTable.entry(*idx);
    if (he.blockIndex >= m_impl->blockTable.count()) return std::nullopt;
    const auto& be = m_impl->blockTable.entry(he.blockIndex);

    FileInfo info;
    info.name = name;
    info.compressedSize = be.compressedSize;
    info.uncompressedSize = be.uncompressedSize;
    info.flags = be.flags;
    info.locale = he.locale;
    return info;
}

ArchiveInfo Storage::archiveInfo() const {
    if (!m_impl || !m_impl->isValid) return {};
    std::shared_lock lock(m_impl->mutex);

    ArchiveInfo info;
    info.formatVersion = m_impl->header.formatVersion;
    info.hashTableEntries = m_impl->header.hashTableEntries;
    info.blockTableEntries = m_impl->header.blockTableEntries;
    info.sectorSize = m_impl->header.sectorSize();
    info.archiveSize = (m_impl->header.formatVersion >= 2)
        ? m_impl->header.archiveSize64
        : m_impl->header.archiveSize;
    return info;
}

std::vector<std::string> Storage::listFiles() const {
    if (!m_impl || !m_impl->isValid) return {};
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
        if (deleteSet.count(norm)) continue;
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
        if (!callback(name)) break;
    }
}

// ============================================================================
// Write Operations
// ============================================================================

bool Storage::writeFile(const std::string& name, std::span<const u8> data, WriteOptions opts) {
    if (!m_impl || !m_impl->isValid) return false;
    std::unique_lock lock(m_impl->mutex);

    std::string norm = normalizePath(name);
    OverlayKey key{norm, opts.locale};

    // Remove from deletes if present.
    m_impl->pendingDeletes.erase(key);

    // Store in overlay.
    m_impl->pendingWrites[key] = {
        name,
        std::vector<u8>(data.begin(), data.end()),
        opts
    };

    return true;
}

bool Storage::deleteFile(const std::string& name) {
    if (!m_impl || !m_impl->isValid) return false;
    std::unique_lock lock(m_impl->mutex);

    std::string norm = normalizePath(name);
    OverlayKey key{norm, Locale::Neutral};

    // Check if it exists in overlay or source.
    bool exists = m_impl->pendingWrites.count(key) ||
                  m_impl->hashTable.lookup(name).has_value();
    if (!exists) return false;

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
    if (!m_impl || !m_impl->isValid) return false;
    if (m_impl->sourcePath.empty()) return false; // Created via create(), no path.
    return save(m_impl->sourcePath);
}

bool Storage::save(const std::string& path) {
    if (!m_impl || !m_impl->isValid) return false;
    std::unique_lock lock(m_impl->mutex);

    bool isSamePath = (path == m_impl->sourcePath);

    // Build write entries from source + overlay.
    auto entries = m_impl->buildWriteEntries();

    // Write the archive.
    auto archiveData = writeArchive(
        m_impl->header,
        entries,
        m_impl->header.hashTableEntries,
        m_impl->pool);

    if (archiveData.empty()) return false;

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
        if (!out) return false;
        out.write(reinterpret_cast<const char*>(archiveData.data()),
                  static_cast<std::streamsize>(archiveData.size()));
        if (!out) {
            // Write failed — clean up partial temp file if created.
            if (useTempFile) {
                std::error_code ec;
                std::filesystem::remove(tempPath, ec);
            }
            return false;
        }
    }

    if (useTempFile) {
        // Unmap the source archive so we can replace the file.
        m_impl->sourceArchive.reset();

        std::error_code ec;
        std::filesystem::rename(tempPath, path, ec);
        if (ec) {
            // Rename failed — try to restore the original mapping.
            m_impl->sourceArchive = storages::common::MappedFile::open(m_impl->sourcePath);
            std::filesystem::remove(tempPath, ec);
            return false;
        }
    }

    auto invalidateAfterOverwriteFailure = [&]() {
        m_impl->sourceArchive.reset();
        m_impl->hashTable = HashTable{};
        m_impl->blockTable = BlockTable{};
        m_impl->sourceListfileNames.clear();
        m_impl->isValid = false;
    };

    // Re-map the new archive.
    auto newMapping = storages::common::MappedFile::open(path);
    if (!newMapping) {
        if (useTempFile) invalidateAfterOverwriteFailure();
        return false;
    }

    // Re-parse header + tables from new archive.
    auto parseResult = findAndParseHeader(newMapping->data());
    if (!parseResult) {
        if (useTempFile) invalidateAfterOverwriteFailure();
        return false;
    }

    MpqHeader newHeader = parseResult->header;
    size_t newArchiveOffset = parseResult->archiveOffset;
    HashTable newHashTable;
    BlockTable newBlockTable;
    std::vector<std::string> newListfileNames;

    auto archiveSpan = newMapping->data();

    // Re-parse tables.
    u64 htOffset = newArchiveOffset + newHeader.hashTableByteOffset();
    u64 htSize = static_cast<u64>(newHeader.hashTableEntries) * 16;
    if (htOffset + htSize > archiveSpan.size()) {
        if (useTempFile) invalidateAfterOverwriteFailure();
        return false;
    }
    if (!newHashTable.parse(archiveSpan.subspan(htOffset, htSize), newHeader.hashTableEntries)) {
        if (useTempFile) invalidateAfterOverwriteFailure();
        return false;
    }

    u64 btOffset = newArchiveOffset + newHeader.blockTableByteOffset();
    u64 btSize = static_cast<u64>(newHeader.blockTableEntries) * 16;
    if (btOffset + btSize > archiveSpan.size()) {
        if (useTempFile) invalidateAfterOverwriteFailure();
        return false;
    }
    if (!newBlockTable.parse(archiveSpan.subspan(btOffset, btSize), newHeader.blockTableEntries)) {
        if (useTempFile) invalidateAfterOverwriteFailure();
        return false;
    }

    // Re-read (listfile).
    auto listfileData = [&]() -> std::optional<std::vector<u8>> {
        auto idx = newHashTable.lookup("(listfile)");
        if (!idx) return std::nullopt;

        const auto& he = newHashTable.entry(*idx);
        if (he.blockIndex >= newBlockTable.count()) return std::nullopt;

        return extractFileData(
            newMapping->data(),
            newArchiveOffset,
            newBlockTable.entry(he.blockIndex),
            newHeader.sectorSize(),
            0);
    }();
    if (listfileData) {
        newListfileNames = parseListfile(std::span<const u8>(*listfileData));
    }

    m_impl->header = newHeader;
    m_impl->archiveOffset = newArchiveOffset;
    m_impl->sourcePath = path;
    m_impl->hashTable = std::move(newHashTable);
    m_impl->blockTable = std::move(newBlockTable);
    m_impl->sourceArchive = std::move(newMapping);
    m_impl->sourceListfileNames = std::move(newListfileNames);
    m_impl->isValid = true;

    // Clear overlay.
    m_impl->pendingWrites.clear();
    m_impl->pendingDeletes.clear();

    return true;
}

} // namespace whiteout::storages::mpq
