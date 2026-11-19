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
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
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

    // As StormLib does: no reader can split files into zero-byte sectors.
    if (pa.header.sectorSize() == 0) {
        setError("sector size shift " + std::to_string(pa.header.sectorSizeShift) + " leaves no sector size");
        return std::nullopt;
    }

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
    if (auto lfIdx = pa.hashTable.find("(listfile)", 0, pa.blockTable.count())) {
        const auto& he = pa.hashTable.entry(*lfIdx);
        const auto& be = pa.blockTable.entry(he.blockIndex);
        u32 const fileKey = be.isEncrypted() ? deriveFileKey("(listfile)", be) : 0;
        auto lfData = extractFileData(mappedFile->data(), pa.archiveOffset, be, pa.header.sectorSize(),
                                      fileKey, nullptr, nullptr, pa.blockTable.fileOffsetHi(he.blockIndex));
        if (lfData)
            pa.listfileNames = parseListfile(std::span<const u8>(*lfData));
    }

    pa.mapping = std::move(*mappedFile);
    return pa;
}

/// Holds a saved archive in memory until it passes kSpillBytes, then streams
/// it to the file: a small save is one write, a large one never holds the
/// whole archive.
class SpillFileSink final : public ArchiveSink {
public:
    static constexpr size_t kSpillBytes = size_t{128} << 20;

    /// @p base: what the file holds before the archive (the preamble).
    SpillFileSink(std::ofstream& out, u64 base) : m_out(out), m_base(base) {}

    [[nodiscard]] u64 size() const override {
        return m_flushed + m_buffer.size();
    }

    void append(std::span<const u8> bytes) override {
        if (m_buffer.size() + bytes.size() < kSpillBytes) {
            m_buffer.insert(m_buffer.end(), bytes.begin(), bytes.end());
            return;
        }
        flush();
        write(bytes);
        m_flushed += bytes.size();
    }

    void patch(u64 offset, std::span<const u8> bytes) override {
        if (offset < m_flushed) {
            size_t const onDisk = static_cast<size_t>(std::min<u64>(bytes.size(), m_flushed - offset));
            m_out.seekp(static_cast<std::streamoff>(m_base + offset));
            write(bytes.first(onDisk));
            m_out.seekp(0, std::ios::end);
            bytes = bytes.subspan(onDisk);
            offset += onDisk;
        }
        std::copy(bytes.begin(), bytes.end(), m_buffer.begin() + static_cast<std::ptrdiff_t>(offset - m_flushed));
    }

    /// Writes what is still held; false when any write failed.
    bool finish() {
        flush();
        m_out.flush();
        return static_cast<bool>(m_out);
    }

private:
    void write(std::span<const u8> bytes) {
        m_out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    void flush() {
        write(m_buffer);
        m_flushed += m_buffer.size();
        m_buffer.clear();
    }

    std::ofstream& m_out;
    u64 m_base;
    u64 m_flushed = 0;
    std::vector<u8> m_buffer;
};

/// A name beside @p path that no file has yet: a save never writes over a
/// file it does not own.
std::string unusedTempPath(const std::string& path) {
    for (u32 n = 0;; ++n) {
        std::string candidate = path + ".tmp" + (n == 0 ? std::string() : std::to_string(n));
        std::error_code ec;
        if (!std::filesystem::exists(whiteout::common::utf8_to_path(candidate), ec) || ec)
            return candidate;
    }
}

/// Whether @p a and @p b name one file, however each is spelled.
bool sameFile(const std::string& a, const std::string& b) {
    std::error_code ec;
    bool const same =
        std::filesystem::equivalent(whiteout::common::utf8_to_path(a), whiteout::common::utf8_to_path(b), ec);
    return ec ? a == b : same;
}

} // anonymous namespace

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

    // Overlay — pending modifications, by normalized name. A name the
    // overlay writes or deletes loses every source copy at save, in every
    // locale, so the source no longer answers for it.
    struct OverlayEntry {
        std::string originalName; // Preserves user-provided casing for the listfile.
        std::vector<u8> data;
        WriteOptions opts;
    };
    std::unordered_map<std::string, std::map<u16, OverlayEntry>> pendingWrites;
    std::unordered_set<std::string> pendingDeletes;

    // Parsed (listfile) from source archive.
    std::vector<std::string> sourceListfileNames;

    // Thread safety.
    mutable std::shared_mutex mutex;

    std::atomic<bool> isValid{false}; ///< Read unlocked by operator bool.

    /// Why the last save() failed; empty after one that did not.
    std::string lastError;

    // Worker pool for parallel compression/decompression (non-owning, may be null).
    interfaces::WorkerPool* pool = nullptr;

    // -- Helpers --

    /// What a read of a name in a locale lands on: a pending write, a
    /// source hash slot, or nothing.
    struct Found {
        const OverlayEntry* pending = nullptr;
        std::optional<u32> slot;
        bool shadowed = false; ///< The overlay owns the name, so the source is not asked.
    };

    /// Resolve @p name as Storm does: the entry of exactly @p locale, else the
    /// neutral one. Caller must hold at least a shared lock on mutex.
    Found resolve(const std::string& name, u16 locale) const {
        std::string const norm = normalizePath(name);
        auto const it = pendingWrites.find(norm);
        if (it == pendingWrites.end() && !pendingDeletes.contains(norm))
            return {nullptr, hashTable.find(name, locale, blockTable.count())};
        if (it == pendingWrites.end())
            return {nullptr, std::nullopt, true};
        const auto& byLocale = it->second;
        if (locale != Locale::Neutral)
            if (auto e = byLocale.find(locale); e != byLocale.end())
                return {&e->second, std::nullopt, true};
        if (auto e = byLocale.find(Locale::Neutral); e != byLocale.end())
            return {&e->second, std::nullopt, true};
        return {nullptr, std::nullopt, true};
    }

    /// Extract the source file at hash @p slot, whose name is @p name.
    std::optional<std::vector<u8>> extractSlot(u32 slot, const std::string& name,
                                               std::string* error = nullptr) const {
        const auto& he = hashTable.entry(slot);
        const auto& be = blockTable.entry(he.blockIndex);
        u32 const fileKey = be.isEncrypted() ? deriveFileKey(name, be) : 0;
        return extractFileData(sourceArchive->data(), archiveOffset, be, header.sectorSize(),
                               fileKey, error, pool, blockTable.fileOffsetHi(he.blockIndex));
    }

    /// Shared readFile logic. Caller must hold at least a shared lock on mutex.
    std::optional<std::vector<u8>> readFileCore(const std::string& name, u16 locale,
                                                std::string* error) const {
        Found const found = resolve(name, locale);
        if (found.pending)
            return found.pending->data;
        if (!found.slot || !sourceArchive) {
            if (error)
                *error = found.shadowed ? "file deleted in overlay" : "not found in hash table";
            return std::nullopt;
        }
        return extractSlot(*found.slot, name, error);
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
        for (const auto& [norm, byLocale] : pendingWrites)
            replaced.insert(nameKey(norm));
        for (const auto& norm : pendingDeletes)
            replaced.insert(nameKey(norm));

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
            if (auto slot = hashTable.find("(attributes)", 0, blockTable.count())) {
                auto data = extractSlot(*slot, "(attributes)");
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
        std::vector<const std::pair<const std::string, std::map<u16, OverlayEntry>>*> overlay;
        for (const auto& item : pendingWrites)
            overlay.push_back(&item);
        std::sort(overlay.begin(), overlay.end(),
                  [](const auto* a, const auto* b) { return a->first < b->first; });
        for (const auto* item : overlay) {
            for (const auto& [locale, pending] : item->second) {
                WriteEntry we;
                we.filename = pending.originalName;
                we.locale = locale;
                we.listRank = rankOf(nameKey(pending.originalName));
                we.rawData = pending.data;
                we.compression = static_cast<CompressionFlag>(pending.opts.compression);
                we.encrypt = pending.opts.encrypt;
                we.singleUnit = pending.opts.singleUnit;
                entries.push_back(std::move(we));
            }
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

    /// Write the archive to @p path and make it the source. Caller holds
    /// the exclusive lock.
    bool saveTo(const std::string& path);

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
    if (!m_impl)
        return std::nullopt;
    std::shared_lock const lock(m_impl->mutex);
    if (!m_impl->isValid)
        return std::nullopt;
    return m_impl->readFileCore(name, Locale::Neutral, nullptr);
}

std::optional<std::vector<u8>> Storage::readFile(const std::string& name,
                                                 std::string* error) const {
    if (!m_impl)
        return std::nullopt;
    std::shared_lock const lock(m_impl->mutex);
    if (!m_impl->isValid) {
        if (error)
            *error = "storage not open";
        return std::nullopt;
    }
    return m_impl->readFileCore(name, Locale::Neutral, error);
}

std::optional<std::vector<u8>> Storage::readFile(const std::string& name, u16 locale) const {
    if (!m_impl)
        return std::nullopt;
    std::shared_lock const lock(m_impl->mutex);
    if (!m_impl->isValid)
        return std::nullopt;
    return m_impl->readFileCore(name, locale, nullptr);
}

bool Storage::fileExists(const std::string& name) const {
    if (!m_impl)
        return false;
    std::shared_lock const lock(m_impl->mutex);
    if (!m_impl->isValid)
        return false;

    auto const found = m_impl->resolve(name, Locale::Neutral);
    if (found.pending)
        return true;
    return found.slot && m_impl->blockTable.entry(m_impl->hashTable.entry(*found.slot).blockIndex).exists();
}

std::optional<FileInfo> Storage::fileInfo(const std::string& name) const {
    if (!m_impl)
        return std::nullopt;
    std::shared_lock const lock(m_impl->mutex);
    if (!m_impl->isValid)
        return std::nullopt;

    auto const found = m_impl->resolve(name, Locale::Neutral);
    if (found.pending) {
        FileInfo info;
        info.name = name;
        info.uncompressedSize = static_cast<u32>(found.pending->data.size());
        info.compressedSize = info.uncompressedSize; // Not compressed yet.
        info.locale = found.pending->opts.locale;
        info.flags = FileFlags::Exists;
        return info;
    }
    if (!found.slot)
        return std::nullopt;

    const auto& he = m_impl->hashTable.entry(*found.slot);
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
    if (!m_impl)
        return {};
    std::shared_lock const lock(m_impl->mutex);
    if (!m_impl->isValid)
        return {};

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
    if (!m_impl)
        return {};
    std::shared_lock const lock(m_impl->mutex);
    if (!m_impl->isValid)
        return {};

    const auto& deleteSet = m_impl->pendingDeletes;
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
    for (const auto& [norm, byLocale] : m_impl->pendingWrites) {
        if (seen.insert(norm).second && !byLocale.empty()) {
            result.push_back(byLocale.begin()->second.originalName);
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
    // An MPQ file's size is 32 bits wide.
    if (!m_impl || data.size() > 0xFFFFFFFFull)
        return false;
    std::unique_lock const lock(m_impl->mutex);
    if (!m_impl->isValid)
        return false;

    // A pending delete stays: it still takes every source copy of the name.
    m_impl->pendingWrites[normalizePath(name)][opts.locale] = {name, std::vector<u8>(data.begin(), data.end()), opts};
    return true;
}

bool Storage::deleteFile(const std::string& name) {
    if (!m_impl)
        return false;
    std::unique_lock const lock(m_impl->mutex);
    if (!m_impl->isValid)
        return false;

    // Every locale of the name goes, pending writes included.
    std::string norm = normalizePath(name);
    bool const pending = m_impl->pendingWrites.erase(norm) > 0;
    bool const inSource = !m_impl->pendingDeletes.contains(norm) && m_impl->hashTable.lookup(name).has_value();
    if (!pending && !inSource)
        return false;
    m_impl->pendingDeletes.insert(std::move(norm));
    return true;
}

// ============================================================================
// Save
// ============================================================================

bool Storage::save() {
    if (!m_impl)
        return false;
    std::unique_lock const lock(m_impl->mutex);
    if (!m_impl->isValid)
        return false;
    if (m_impl->sourcePath.empty()) {
        m_impl->lastError = "the archive has no path yet: save it to one";
        return false;
    }
    return m_impl->saveTo(std::string(m_impl->sourcePath));
}

bool Storage::save(const std::string& path) {
    if (!m_impl)
        return false;
    std::unique_lock const lock(m_impl->mutex);
    if (!m_impl->isValid)
        return false;
    return m_impl->saveTo(path);
}

bool Storage::Impl::saveTo(const std::string& path) {
    lastError.clear();
    auto fail = [&](std::string why) {
        lastError = std::move(why);
        return false;
    };

    // Build the writer's input from source + overlay.
    WritePlan plan;
    std::vector<WriteEntry> entries;
    if (std::string why = buildWriteJob(plan, entries); !why.empty())
        return fail(std::move(why));
    if (sourceArchive)
        plan.sourceHashTable = &hashTable;

    // Whatever precedes the MPQ header -- a Warcraft III map's 512-byte HM3W
    // block, a user data header -- is kept byte for byte, so every offset in
    // it still lands on the header. A trailing signature is not: it signed
    // the old bytes.
    std::span<const u8> preamble;
    if (sourceArchive)
        preamble = sourceArchive->data().subspan(0, archiveOffset);

    // The archive goes to a temp file beside the target, then over it in one
    // rename, so a failed save leaves the target as it was.
    std::string const tempPath = unusedTempPath(path);
    auto const temp = whiteout::common::utf8_to_path(tempPath);
    {
        auto out = whiteout::common::open_ofstream(tempPath, std::ios::binary | std::ios::trunc);
        if (!out)
            return fail("could not open '" + tempPath + "' for writing");
        out.write(reinterpret_cast<const char*>(preamble.data()), static_cast<std::streamsize>(preamble.size()));
        SpillFileSink sink(out, preamble.size());
        std::string why = writeArchive(plan, entries, sink, pool);
        if (why.empty() && !sink.finish())
            why = "could not write '" + tempPath + "'";
        out.close();
        if (!why.empty()) {
            std::error_code ec;
            std::filesystem::remove(temp, ec);
            return fail(std::move(why));
        }
    }

    // The source stays mapped until here, and Windows renames nothing over a
    // mapped file. Its path may be spelled another way than the target's.
    bool const replacesSource = sourceArchive && sameFile(path, sourcePath);
    if (replacesSource)
        sourceArchive.reset();
    std::error_code ec;
    std::filesystem::rename(temp, whiteout::common::utf8_to_path(path), ec);
    if (ec) {
        if (replacesSource)
            sourceArchive = storages::common::MappedFile::open(sourcePath);
        std::error_code removeEc;
        std::filesystem::remove(temp, removeEc);
        return fail("could not replace '" + path + "': " + ec.message());
    }

    // Re-open the saved archive as the new source.
    if (!reloadFromDisk(path)) {
        if (replacesSource)
            invalidate();
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
