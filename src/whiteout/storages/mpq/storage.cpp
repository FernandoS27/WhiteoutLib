// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/storages/mpq/storage.h>

#include "../../common/unicode_path.h"
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

// ============================================================================
// Shared archive-parsing helper
// ============================================================================

namespace {

/// A name's two hash table checks as one key; @p keepSlashes hashes '/' as
/// itself, as Storm and StormLib do.
u64 nameKey(std::string_view name, bool keepSlashes = false) {
    return hashName(name, keepSlashes).key();
}

/// A name as the overlay keys it: upper case, '/' as '\\', the way it is
/// hashed. Short names stay on the stack: a lookup builds no string.
class OverlayKey {
public:
    explicit OverlayKey(std::string_view name) {
        char* out = m_inline;
        if (name.size() > sizeof(m_inline)) {
            m_long.resize(name.size());
            out = m_long.data();
        }
        for (size_t i = 0; i < name.size(); ++i)
            out[i] = static_cast<char>(kNameFold.folded[static_cast<u8>(name[i])]);
        m_view = std::string_view(out, name.size());
    }
    OverlayKey(const OverlayKey&) = delete;
    OverlayKey& operator=(const OverlayKey&) = delete;

    [[nodiscard]] std::string_view view() const {
        return m_view;
    }
    [[nodiscard]] std::string str() const {
        return std::string(m_view);
    }

private:
    char m_inline[160];
    std::string m_long;
    std::string_view m_view;
};

/// Hash and equality that let the overlay's maps be asked with a view.
struct NameViewHash {
    using is_transparent = void;
    size_t operator()(std::string_view name) const noexcept {
        return std::hash<std::string_view>{}(name);
    }
};
struct NameViewEqual {
    using is_transparent = void;
    bool operator()(std::string_view a, std::string_view b) const noexcept {
        return a == b;
    }
};

/// The source archive's (listfile), matched against its hash table.
struct NameIndex {
    struct Line {
        std::string_view name;      ///< As the (listfile) spells it; points into `text`.
        NameHash folded;            ///< Hashed with '/' as '\\'.
        NameHash slashKept;         ///< Hashed with '/' as itself; `folded` again when it has none.
        bool held = false;          ///< The archive holds a file by this name, in some locale.
        bool heldSlashKept = false; ///< The spelling its hash slots are under.

        /// The key of the spelling the archive knows it by.
        [[nodiscard]] u64 heldKey() const {
            return (heldSlashKept ? slashKept : folded).key();
        }
    };
    std::vector<u8> text;     ///< The (listfile) as read.
    std::vector<Line> lines;  ///< In (listfile) order.
    std::vector<u32> listed;  ///< The lines listFiles gives: held, the first for each file.
};

/// Result of parsing an MPQ archive from a memory-mapped file.
struct ParsedArchive {
    storages::common::MappedFile mapping;
    MpqHeader header;
    size_t archiveOffset = 0;
    bool war3Map = false; ///< Read as Warcraft III reads a map (see isWar3Map).
    HashTable hashTable;
    BlockTable blockTable;
};

/// Map, parse header and decrypt tables from an archive on disk; its
/// (listfile) is read when something first asks for names.
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

    ParsedArchive pa;
    pa.war3Map = isWar3Map(path, mappedFile->data());
    auto parseResult = findAndParseHeader(mappedFile->data(), pa.war3Map);
    if (!parseResult) {
        setError("no valid MPQ header found (missing MPQ\\x1A signature or header too small)");
        return std::nullopt;
    }

    pa.header = parseResult->header;
    pa.archiveOffset = parseResult->archiveOffset;
    auto archiveSpan = mappedFile->data();

    // As StormLib does: no reader can split files into zero-byte sectors.
    if (pa.header.sectorSize() == 0) {
        setError("sector size shift " + std::to_string(pa.header.sectorSizeShift) + " leaves no sector size");
        return std::nullopt;
    }

    // Storm keeps 28 bits of a table size, as of a block index: it scales
    // them by 16, and the top four bits shift out.
    pa.header.hashTableEntries &= kBlockIndexMask;
    pa.header.blockTableEntries &= kBlockIndexMask;
    // A version 1 archive's offsets are 32 bits wide and wrap.
    auto tableAt = [&](u64 offset) {
        u64 const at = pa.archiveOffset + offset;
        return pa.header.formatVersion == 0 ? at & 0xFFFFFFFF : at;
    };

    // Parse hash table.
    u64 const htOffset = tableAt(pa.header.hashTableByteOffset());
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

    // Parse block table. A version 1 table running past the end of the file
    // ends there, as StormLib cuts it: map protectors inflate the count.
    u64 const btOffset = tableAt(pa.header.blockTableByteOffset());
    if (pa.header.formatVersion == 0 && btOffset <= archiveSpan.size() &&
        btOffset + u64{pa.header.blockTableEntries} * 16 > archiveSpan.size())
        pa.header.blockTableEntries = static_cast<u32>((archiveSpan.size() - btOffset) / 16);
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
    // Warcraft III's Storm knows fewer flags than later ones: a map's file
    // is never single-unit, nor a patch.
    if (pa.war3Map)
        for (u32 i = 0; i < pa.blockTable.count(); ++i)
            pa.blockTable.entry(i).flags &= kWar3MapFileFlags;

    // Parse hi-block table (V2+).
    if (pa.header.formatVersion >= 1 && pa.header.hiBlockTableOffset != 0) {
        u64 const hiOffset = pa.archiveOffset + pa.header.hiBlockTableOffset;
        u64 const hiSize = static_cast<u64>(pa.header.blockTableEntries) * 2;
        if (hiOffset + hiSize <= archiveSpan.size()) {
            pa.blockTable.parseHiBlockTable(archiveSpan.subspan(hiOffset, hiSize),
                                            pa.header.blockTableEntries);
        }
    }

    pa.mapping = std::move(*mappedFile);
    return pa;
}

/// Writes a saved archive to its file. Small pieces gather in one fixed
/// buffer. Bytes that last (files copied whole from the source's mapping)
/// are not copied at all while they follow one another in memory: the
/// stretch they make is written from where it lies. A save holds no more of
/// the archive than that buffer.
class FileSink final : public ArchiveSink {
public:
    static constexpr size_t kBufferBytes = size_t{4} << 20;
    /// A stretch this long is written from where it lies; a shorter one is
    /// cheaper to gather with its neighbours.
    static constexpr size_t kDirectBytes = size_t{256} << 10;
    static constexpr size_t kWriteBytes = size_t{16} << 20; ///< The most given to one write.

    /// @p base: what the file holds before the archive (the preamble).
    FileSink(std::ofstream& out, u64 base) : m_out(out), m_base(base) {
        m_buffer.reserve(kBufferBytes);
    }

    [[nodiscard]] u64 size() const override {
        return m_flushed + m_buffer.size() + m_stretch.size();
    }

    void append(std::span<const u8> bytes) override {
        settleStretch();
        gather(bytes);
    }

    void appendLasting(std::span<const u8> bytes) override {
        if (!m_stretch.empty() && m_stretch.data() + m_stretch.size() == bytes.data()) {
            m_stretch = std::span<const u8>(m_stretch.data(), m_stretch.size() + bytes.size());
            return;
        }
        settleStretch();
        m_stretch = bytes;
    }

    void patch(u64 offset, std::span<const u8> bytes) override {
        settleStretch();
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
        settleStretch();
        flush();
        m_out.flush();
        return static_cast<bool>(m_out);
    }

private:
    void write(std::span<const u8> bytes) {
        for (size_t at = 0; at < bytes.size(); at += kWriteBytes) {
            auto const piece = bytes.subspan(at, std::min(kWriteBytes, bytes.size() - at));
            m_out.write(reinterpret_cast<const char*>(piece.data()), static_cast<std::streamsize>(piece.size()));
        }
    }
    void flush() {
        write(m_buffer);
        m_flushed += m_buffer.size();
        m_buffer.clear();
    }
    /// Bytes into the buffer, which is written out when they do not fit.
    void gather(std::span<const u8> bytes) {
        if (m_buffer.size() + bytes.size() > kBufferBytes)
            flush();
        if (bytes.size() > kBufferBytes) {
            write(bytes);
            m_flushed += bytes.size();
            return;
        }
        m_buffer.insert(m_buffer.end(), bytes.begin(), bytes.end());
    }
    /// The stretch of lasting bytes still pending goes out: from where it
    /// lies when it is long, through the buffer when it is not.
    void settleStretch() {
        if (m_stretch.empty())
            return;
        std::span<const u8> const stretch = m_stretch;
        m_stretch = {};
        if (stretch.size() < kDirectBytes) {
            gather(stretch);
            return;
        }
        flush();
        write(stretch);
        m_flushed += stretch.size();
    }

    std::ofstream& m_out;
    u64 m_base;
    u64 m_flushed = 0;
    std::vector<u8> m_buffer;
    std::span<const u8> m_stretch; ///< Lasting bytes appended and not yet written.
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
    bool war3Map = false;
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
    std::unordered_map<std::string, std::map<u16, OverlayEntry>, NameViewHash, NameViewEqual> pendingWrites;
    std::unordered_set<std::string, NameViewHash, NameViewEqual> pendingDeletes;
    /// The slash-kept keys of the names with a slash the overlay was given:
    /// a source file hashed that way goes at save too.
    std::unordered_set<u64> overlaySlashKeys;

    // The source archive's (listfile) and what it holds of it, built by
    // names() the first time it is asked for.
    mutable NameIndex nameIndex;
    mutable std::mutex nameIndexMutex;
    mutable std::atomic<bool> nameIndexBuilt{false};

    // Thread safety.
    mutable std::shared_mutex mutex;

    std::atomic<bool> isValid{false}; ///< Read unlocked by operator bool.

    /// Why the last save() failed; empty after one that did not.
    std::string lastError;

    // Worker pool for parallel compression/decompression (non-owning, may be null).
    interfaces::WorkerPool* pool = nullptr;

    /// The (attributes) arrays an archive made by create() is first saved
    /// with; one that was opened keeps its own.
    AttributeFlag createdAttributes = AttributeFlag::kCrc32 | AttributeFlag::kFiletime;

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
    Found resolve(std::string_view name, u16 locale) const {
        if (pendingWrites.empty() && pendingDeletes.empty())
            return {nullptr, hashTable.find(name, locale, blockTable.count())};
        OverlayKey const key(name);
        std::string_view const norm = key.view();
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
    std::optional<std::vector<u8>> extractSlot(u32 slot, std::string_view name, std::string* error = nullptr,
                                               bool pooled = true) const {
        const auto& he = hashTable.entry(slot);
        const auto& be = blockTable.entry(he.blockIndex);
        u32 const fileKey = be.isEncrypted() ? deriveFileKey(name, be) : 0;
        return extractFileData(sourceArchive->data(), archiveOffset, be, header.sectorSize(), fileKey, error,
                               pooled ? pool : nullptr, blockTable.fileOffsetHi(he.blockIndex));
    }

    /// The source's (listfile) matched against its hash table: read and
    /// built by the first caller, then shared. Caller must hold at least a
    /// shared lock on mutex.
    const NameIndex& names() const {
        if (!nameIndexBuilt.load(std::memory_order_acquire)) {
            std::lock_guard const lock(nameIndexMutex);
            if (!nameIndexBuilt.load(std::memory_order_relaxed)) {
                buildNameIndex();
                nameIndexBuilt.store(true, std::memory_order_release);
            }
        }
        return nameIndex;
    }

    void buildNameIndex() const {
        NameIndex index;
        if (sourceArchive) {
            if (auto slot = hashTable.find("(listfile)", 0, blockTable.count()))
                if (auto data = extractSlot(*slot, "(listfile)", nullptr, false))
                    index.text = std::move(*data);
        }
        forEachListfileLine(index.text, [&](std::string_view name) {
            NameIndex::Line line;
            line.name = name;
            line.folded = hashName(name);
            line.slashKept = line.folded.hasSlash ? hashName(name, true) : line.folded;
            index.lines.push_back(line);
        });

        // A line names a file when the archive holds one by that name, in
        // some locale, under the first spelling with any hash slot at all.
        // Two lines for one file share its slots: the first is the one listed.
        u32 const blockCount = blockTable.count();
        std::vector<u8> claimed(hashTable.capacity(), 0);
        for (u32 i = 0; i < index.lines.size(); ++i) {
            NameIndex::Line& line = index.lines[i];
            bool any = false;
            std::optional<u32> heldSlot;
            auto scan = [&](const NameHash& spelling) {
                hashTable.eachSlot(spelling, [&](u32 slot) {
                    any = true;
                    const HashEntry& he = hashTable.entry(slot);
                    if (!heldSlot && he.blockIndex < blockCount && blockTable.entry(he.blockIndex).exists() &&
                        (he.platform & 0xFF) == 0)
                        heldSlot = slot;
                });
            };
            if (line.folded.hasSlash) {
                scan(line.slashKept);
                line.heldSlashKept = any;
            }
            if (!line.heldSlashKept)
                scan(line.folded);
            line.held = heldSlot.has_value();
            if (heldSlot && !claimed[*heldSlot]) {
                claimed[*heldSlot] = 1;
                index.listed.push_back(i);
            }
        }
        nameIndex = std::move(index);
    }

    /// Forget the name index: the source it was built from is gone.
    void resetNameIndex() {
        nameIndex = NameIndex{};
        nameIndexBuilt.store(false, std::memory_order_release);
    }

    /// Shared readFile logic. Caller must hold at least a shared lock on mutex.
    std::optional<std::vector<u8>> readFileCore(std::string_view name, u16 locale, std::string* error) const {
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
        plan.attributeFlags = createdAttributes;

        // A name keeps its (listfile) line, in either spelling; a new one
        // goes after them all. Each spelling's key gives its first line and
        // which spelling it is.
        const NameIndex& index = names();
        struct Named {
            u32 line;
            bool slashHashed;
        };
        std::unordered_map<u64, Named> named;
        named.reserve(index.lines.size());
        for (u32 i = 0; i < index.lines.size(); ++i) {
            const NameIndex::Line& line = index.lines[i];
            if (line.folded.hasSlash)
                named.try_emplace(line.slashKept.key(), Named{i, true});
            named.try_emplace(line.folded.key(), Named{i, false});
        }
        u32 const lineCount = static_cast<u32>(index.lines.size());
        auto rankOf = [&](u64 key) {
            auto it = named.find(key);
            return it != named.end() ? it->second.line : lineCount;
        };
        // Names the overlay writes or deletes: every locale of one goes, so a
        // replaced file is not shadowed by a localized copy of the old one.
        std::unordered_set<u64> replaced = overlaySlashKeys;
        for (const auto& [norm, byLocale] : pendingWrites)
            replaced.insert(nameKey(norm));
        for (const auto& norm : pendingDeletes)
            replaced.insert(nameKey(norm));
        bool const overlaid = !pendingWrites.empty() || !pendingDeletes.empty();
        auto overlayOwns = [&](std::string_view name) {
            if (!overlaid)
                return false;
            OverlayKey const key(name);
            return pendingWrites.contains(key.view()) || pendingDeletes.contains(key.view());
        };

        if (sourceArchive) {
            std::unordered_set<u64> const specials{nameKey("(listfile)"), nameKey("(attributes)"), nameKey("(signature)")};

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
                if (auto it = named.find(key); it != named.end()) {
                    const NameIndex::Line& line = index.lines[it->second.line];
                    bool const slashHashed = it->second.slashHashed;
                    if (overlayOwns(line.name))
                        continue;
                    // Only the entry a lookup lands on is ever read: a decoy
                    // ahead of it, or a second copy for its locale, goes.
                    if ((he.platform & 0xFF) == 0 &&
                        hashTable.findHashed(slashHashed ? line.slashKept : line.folded, he.locale,
                                             blockTable.count()) != slot)
                        continue;
                    we.filename = std::string(line.name);
                    we.slashHashed = slashHashed;
                }
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
                we.singleUnit = pending.opts.singleUnit && !war3Map; // Warcraft III reads no single unit.
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
        resetNameIndex();
        pendingWrites.clear();
        pendingDeletes.clear();
        overlaySlashKeys.clear();
        isValid = false;
    }

    /// Note a name the overlay now owns, in the spelling it was given.
    void noteSpelling(std::string_view name) {
        if (name.find('/') != std::string_view::npos)
            overlaySlashKeys.insert(nameKey(name, true));
    }

    /// Apply the results of parseMappedArchive() into this Impl, setting it as
    /// the new source archive.  Clears any pending overlay.
    void applyParsedArchive(ParsedArchive& pa, const std::string& path) {
        header = pa.header;
        archiveOffset = pa.archiveOffset;
        war3Map = pa.war3Map;
        sourcePath = path;
        hashTable = std::move(pa.hashTable);
        blockTable = std::move(pa.blockTable);
        sourceArchive = std::move(pa.mapping);
        resetNameIndex();
        isValid = true;
        pendingWrites.clear();
        pendingDeletes.clear();
        overlaySlashKeys.clear();
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

    impl.header = buildHeader(static_cast<u16>(opts.version), opts.hashTableSize,
                              std::max<u16>(opts.sectorSizeShift, 1));
    if (opts.fileHashes)
        impl.createdAttributes |= AttributeFlag::kMd5;
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

std::vector<std::optional<std::vector<u8>>> Storage::readFiles(const std::vector<std::string>& names) const {
    std::vector<std::optional<std::vector<u8>>> results(names.size());
    if (!m_impl)
        return results;
    std::shared_lock const lock(m_impl->mutex);
    const Impl& impl = *m_impl;
    if (!impl.isValid)
        return results;

    // A pending write answers at once; the rest go to the source together.
    std::vector<ExtractFileInfo> wanted;
    std::vector<size_t> wantedIndex;
    for (size_t i = 0; i < names.size(); ++i) {
        Impl::Found const found = impl.resolve(names[i], Locale::Neutral);
        if (found.pending) {
            results[i] = found.pending->data;
            continue;
        }
        if (!found.slot || !impl.sourceArchive)
            continue;
        const HashEntry& he = impl.hashTable.entry(*found.slot);
        const BlockEntry& be = impl.blockTable.entry(he.blockIndex);
        wanted.push_back({be, be.isEncrypted() ? deriveFileKey(names[i], be) : 0,
                          impl.blockTable.fileOffsetHi(he.blockIndex)});
        wantedIndex.push_back(i);
    }
    if (wanted.empty())
        return results;
    auto extracted = extractBatch(impl.sourceArchive->data(), impl.archiveOffset, wanted, impl.header.sectorSize(),
                                  impl.pool);
    for (size_t k = 0; k < extracted.size(); ++k)
        results[wantedIndex[k]] = std::move(extracted[k]);
    return results;
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

    // A (listfile) line is listed when the archive holds a file by that name,
    // in some locale: a line can name nothing.
    const Impl& impl = *m_impl;
    const NameIndex& index = impl.names();
    std::vector<std::string> result;
    if (impl.pendingWrites.empty() && impl.pendingDeletes.empty()) {
        result.reserve(index.listed.size());
        for (u32 const line : index.listed)
            result.emplace_back(index.lines[line].name);
        return result;
    }

    std::unordered_set<u64> seen; // The files listed, by name key.
    for (const NameIndex::Line& line : index.lines) {
        OverlayKey const key(line.name);
        if (impl.pendingWrites.contains(key.view())) {
            // Written over: listed once, as the (listfile) spells it.
            if (seen.insert(line.folded.key()).second)
                result.emplace_back(line.name);
            continue;
        }
        if (line.held && !impl.pendingDeletes.contains(key.view()) && seen.insert(line.heldKey()).second)
            result.emplace_back(line.name);
    }

    // Overlay additions.
    for (const auto& [norm, byLocale] : impl.pendingWrites) {
        if (!byLocale.empty() && seen.insert(nameKey(norm)).second)
            result.push_back(byLocale.begin()->second.originalName);
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
    m_impl->pendingWrites[OverlayKey(name).str()][opts.locale] = {name, std::vector<u8>(data.begin(), data.end()), opts};
    m_impl->noteSpelling(name);
    return true;
}

bool Storage::writeFileOwned(const std::string& name, std::vector<u8>&& data, WriteOptions opts) {
    if (!m_impl || data.size() > 0xFFFFFFFFull)
        return false;
    std::unique_lock const lock(m_impl->mutex);
    if (!m_impl->isValid)
        return false;

    m_impl->pendingWrites[OverlayKey(name).str()][opts.locale] = {name, std::move(data), opts};
    m_impl->noteSpelling(name);
    return true;
}

bool Storage::deleteFile(const std::string& name) {
    if (!m_impl)
        return false;
    std::unique_lock const lock(m_impl->mutex);
    if (!m_impl->isValid)
        return false;

    // Every locale of the name goes, pending writes included.
    std::string norm = OverlayKey(name).str();
    bool const pending = m_impl->pendingWrites.erase(norm) > 0;
    bool const gone = m_impl->pendingDeletes.contains(norm);
    bool const inSource = !gone && m_impl->hashTable.lookup(name).has_value();
    if (pending || gone || inSource)
        m_impl->noteSpelling(name); // A source copy under this spelling goes as well.
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
        FileSink sink(out, preamble.size());
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
