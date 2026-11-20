// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "codecs/compression.h"
#include "codecs/pkware.h"
#include "crypto.h"
#include "file_data.h"
#include "job_board.h"

#include "../../common/checksum.h"
#include "../../common/output_reserve.h"
#include "../common/md5.h"

#include <whiteout/interfaces.h>
#include <whiteout/utils/job_group.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <memory>

namespace whiteout::storages::mpq {

// ============================================================================
// File Key Derivation
// ============================================================================

namespace {

/// Extract the basename (filename after last '\\') for key derivation.
std::string_view getBaseName(std::string_view path) {
    auto pos = path.rfind('\\');
    if (pos == std::string_view::npos)
        pos = path.rfind('/');
    if (pos == std::string_view::npos)
        return path;
    return path.substr(pos + 1);
}

} // anonymous namespace

u32 deriveFileKey(std::string_view filename, const BlockEntry& block) {
    u32 key = hashString(getBaseName(filename), HashType::FileKey);
    if (block.hasFixKey()) {
        key = (key + block.fileOffset) ^ block.uncompressedSize;
    }
    return key;
}

// ============================================================================
// Re-keying
// ============================================================================

namespace {

/// Entries in a compressed file's sector offset table: one per sector, one
/// for the end, one more for a sector CRC block.
u32 sectorTableEntries(const BlockEntry& block, u32 sectorSize) {
    u32 const sectors = (block.uncompressedSize + sectorSize - 1) / sectorSize;
    return sectors + 1 + (block.hasSectorCrc() ? 1u : 0u);
}

/// The sector offset table of @p stored decrypted with @p tableKey, when it
/// reads as one: starting at its own size, never going back, ending inside.
std::optional<std::vector<u32>> openSectorTable(std::span<const u8> stored, u32 entries, u32 tableKey) {
    if (entries == 0 || stored.size() < size_t{entries} * 4)
        return std::nullopt;
    std::vector<u32> table(entries);
    std::memcpy(table.data(), stored.data(), size_t{entries} * 4);
    decryptBlock(table.data(), table.size(), tableKey);
    if (table[0] != entries * 4)
        return std::nullopt;
    for (u32 i = 1; i < entries; ++i)
        if (table[i] < table[i - 1] || table[i] > stored.size())
            return std::nullopt;
    return table;
}

/// Decrypt @p length bytes at @p at with @p from and encrypt them with @p to;
/// a tail past the last whole word is never encrypted.
void recrypt(std::vector<u8>& data, size_t at, size_t length, u32 from, u32 to) {
    size_t const words = length / 4;
    if (words == 0 || at + words * 4 > data.size())
        return;
    std::vector<u32> buffer(words);
    std::memcpy(buffer.data(), data.data() + at, words * 4);
    decryptBlock(buffer.data(), words, from);
    encryptBlock(buffer.data(), words, to);
    std::memcpy(data.data() + at, buffer.data(), words * 4);
}

} // anonymous namespace

std::optional<std::pair<u32, BlockEntry>> splitPatchFile(std::span<const u8> stored, const BlockEntry& block) {
    static constexpr u32 kPatchInfoValid = 0x80000000;
    static constexpr u32 kMaxPatchInfo = 0x400; // StormLib's bound.
    u32 info[3] = {}; // Length, flags, data size; an MD5 follows.
    if (stored.size() < sizeof(info))
        return std::nullopt;
    std::memcpy(info, stored.data(), sizeof(info));
    if ((info[1] & kPatchInfoValid) == 0 || info[0] > kMaxPatchInfo || info[0] > stored.size())
        return std::nullopt;
    BlockEntry body = block;
    body.flags &= ~FileFlag::kPatchFile;
    body.compressedSize = static_cast<u32>(stored.size()) - info[0];
    body.uncompressedSize = info[2];
    return std::pair{info[0], body};
}

std::optional<u32> detectStoredFileKey(std::span<const u8> stored, const BlockEntry& block, u32 sectorSize) {
    if (block.isSingleUnit() || !block.isCompressed() || block.uncompressedSize == 0 || stored.size() < 4 ||
        hasFlag(block.flags, FileFlag::kPatchFile))
        return std::nullopt;
    u32 const entries = sectorTableEntries(block, sectorSize);
    u32 encrypted0 = 0;
    std::memcpy(&encrypted0, stored.data(), 4);
    const u32* table = getEncryptionTable();
    // The first word decrypts to the table's size: one candidate per low key byte.
    for (u32 keyByte = 0; keyByte < 256; ++keyByte) {
        u32 const seed = 0xEEEEEEEE + table[0x400 + keyByte];
        u32 const tableKey = (encrypted0 ^ (entries * 4)) - seed;
        if ((tableKey & 0xFF) == keyByte && openSectorTable(stored, entries, tableKey))
            return tableKey + 1;
    }
    return std::nullopt;
}

std::optional<std::vector<u8>> rekeyStoredFile(std::span<const u8> stored, const BlockEntry& block, u32 sectorSize,
                                               u32 oldKey, u32 newKey) {
    if (hasFlag(block.flags, FileFlag::kPatchFile) && block.uncompressedSize != 0 && oldKey != newKey) {
        // The patch info is never encrypted; the file behind it is.
        auto const patch = splitPatchFile(stored, block);
        if (!patch)
            return std::nullopt;
        auto body = rekeyStoredFile(stored.subspan(patch->first), patch->second, sectorSize, oldKey, newKey);
        if (body)
            body->insert(body->begin(), stored.begin(), stored.begin() + patch->first);
        return body;
    }
    std::vector<u8> out(stored.begin(), stored.end());
    if (block.uncompressedSize == 0 || oldKey == newKey)
        return out;
    if (block.isSingleUnit()) {
        recrypt(out, 0, out.size(), oldKey, newKey);
        return out;
    }
    u32 const sectors = (block.uncompressedSize + sectorSize - 1) / sectorSize;
    if (!block.isCompressed()) {
        // Stored: no table, every sector its own key.
        for (u32 i = 0; i < sectors; ++i) {
            size_t const at = size_t{i} * sectorSize;
            if (at >= out.size())
                break;
            recrypt(out, at, std::min<size_t>(sectorSize, out.size() - at), oldKey + i, newKey + i);
        }
        return out;
    }
    u32 const entries = sectorTableEntries(block, sectorSize);
    std::optional<std::vector<u32>> table = openSectorTable(stored, entries, oldKey - 1);
    if (!table)
        return std::nullopt;
    for (u32 i = 0; i < sectors; ++i)
        recrypt(out, (*table)[i], (*table)[i + 1] - (*table)[i], oldKey + i, newKey + i);
    encryptBlock(table->data(), table->size(), newKey - 1);
    std::memcpy(out.data(), table->data(), table->size() * 4);
    return out;
}

// ============================================================================
// Extraction (Read)
// ============================================================================

namespace {

/// Sectors decoded through a pool go in runs of at least this much output:
/// a task costs more than a 4 KiB sector takes to decode.
constexpr size_t kMinRunBytes = size_t{256} << 10;
/// Under this much output a file is decoded on the thread that asked.
constexpr size_t kMinPooledBytes = size_t{1} << 20;
/// A file whose claim is beyond what its stored bytes plausibly hold is
/// given its output this much at a time, as its sectors really decode.
constexpr size_t kGrowWindowBytes = size_t{4} << 20;
/// In a batch, a file from this size on is split into runs of sectors;
/// a smaller one is one thread's work.
constexpr size_t kSplitFileBytes = size_t{4} << 20;

/// What decoding a unit borrows; one thread keeps one across its units.
struct UnitScratch {
    std::vector<u8> plain; ///< An encrypted unit, decrypted.
    DecodeScratch stages;
};

/// One unit of a file as the archive stores it — a sector, or the whole of
/// a single-unit file — decrypted with @p key and expanded to exactly
/// @p size bytes. A unit that did not shrink is stored as it is. The output
/// grows as it decodes: this is for a unit whose stored bytes cannot
/// plausibly hold what it claims.
std::optional<std::vector<u8>> decodeUnitGrowing(std::span<const u8> stored, const BlockEntry& block,
                                                 u32 size, u32 key, std::string& error) {
    std::vector<u8> buf(stored.begin(), stored.end());
    if (block.isEncrypted() && key != 0)
        decryptBytes(buf, key);
    if (buf.size() >= size) {
        buf.resize(size);
        return buf;
    }

    std::vector<u8> out;
    if (hasFlag(block.flags, FileFlag::kCompress))
        out = mpqDecompress(buf, size, &error);
    else if (hasFlag(block.flags, FileFlag::kImplode))
        out = pkwareExplode(buf, size); // PKware DCL alone: no compression-mask byte.
    else
        error = "stored data is shorter than the file";
    if (out.size() != size) {
        if (error.empty())
            error = "decompressed to " + std::to_string(out.size()) + " bytes, expected " +
                    std::to_string(size);
        return std::nullopt;
    }
    return out;
}

/// The same unit into @p dst, which is exactly the size it comes to and
/// which nothing is written outside of: from the mapping when it is not
/// encrypted, from @p scratch when it is.
bool decodeUnitInto(std::span<const u8> stored, const BlockEntry& block, std::span<u8> dst, u32 key,
                    UnitScratch& scratch, std::string* error) {
    bool const encrypted = block.isEncrypted() && key != 0;
    if (stored.size() == dst.size()) {
        std::memcpy(dst.data(), stored.data(), dst.size());
        if (encrypted)
            decryptBytes(dst, key);
        return true;
    }
    if (encrypted) {
        scratch.plain.assign(stored.begin(), stored.end());
        decryptBytes(scratch.plain, key);
        stored = scratch.plain;
    }
    if (stored.size() > dst.size()) { // Stored with more than it holds: its head is the unit.
        std::memcpy(dst.data(), stored.data(), dst.size());
        return true;
    }

    if (hasFlag(block.flags, FileFlag::kCompress))
        return mpqDecompressInto(stored, dst, scratch.stages, error);
    if (hasFlag(block.flags, FileFlag::kImplode)) {
        // PKware DCL alone: no compression-mask byte.
        size_t const got = pkwareExplodeInto(stored, dst);
        if (got != dst.size() && error)
            *error = "decompressed to " + std::to_string(got) + " bytes, expected " + std::to_string(dst.size());
        return got == dst.size();
    }
    if (error)
        *error = "stored data is shorter than the file";
    return false;
}

/// A unit into a place already set aside for it. One with an implausible
/// claim is still decoded growing first, then copied in.
bool decodeUnit(std::span<const u8> stored, const BlockEntry& block, std::span<u8> dst, u32 key,
                UnitScratch& scratch, std::string* error) {
    if (whiteout::common::plausibleOutput(dst.size(), stored.size())) [[likely]]
        return decodeUnitInto(stored, block, dst, key, scratch, error);
    std::string why;
    auto const data = decodeUnitGrowing(stored, block, static_cast<u32>(dst.size()), key, why);
    if (!data) {
        if (error)
            *error = std::move(why);
        return false;
    }
    std::memcpy(dst.data(), data->data(), dst.size());
    return true;
}

/// A compressed multi-sector file, its sector offset table read and checked.
class SectorFile {
public:
    SectorFile() = default;
    SectorFile(const SectorFile&) = delete;
    SectorFile& operator=(const SectorFile&) = delete;

    /// False, with @p error set, when the table is cut short or does not hold.
    bool open(std::span<const u8> stored, const BlockEntry& block, u32 sectorSize, u32 fileKey, std::string* error) {
        auto fail = [&](auto describe) {
            if (error)
                *error = describe();
            return false;
        };
        m_stored = stored;
        m_block = block;
        m_sectorSize = sectorSize;
        m_key = block.isEncrypted() ? fileKey : 0;
        m_sectors = static_cast<u32>((u64{block.uncompressedSize} + sectorSize - 1) / sectorSize);

        // With a sector CRC the table has one more entry, for the CRC block
        // after the last sector.
        u64 const entries = u64{m_sectors} + 1 + (block.hasSectorCrc() ? 1u : 0u);
        u64 const tableSize = entries * sizeof(u32);
        if (stored.size() < tableSize)
            return fail([&] {
                return "sector offset table truncated: have=" + std::to_string(stored.size()) +
                       " need=" + std::to_string(tableSize);
            });
        u32* offsets = m_inline.data();
        if (entries > m_inline.size()) {
            m_large.resize(static_cast<size_t>(entries));
            offsets = m_large.data();
        }
        std::memcpy(offsets, stored.data(), static_cast<size_t>(tableSize));
        if (m_key != 0)
            decryptBlock(offsets, static_cast<size_t>(entries), m_key - 1);
        m_offsets = offsets;

        if (offsets[0] != tableSize)
            return fail([&] {
                return "sector offset table validation failed: offsets[0]=" + std::to_string(offsets[0]) +
                       " expected=" + std::to_string(tableSize) + " numSectors=" + std::to_string(m_sectors);
            });
        for (u32 i = 0; i < m_sectors; ++i) {
            if (offsets[i + 1] < offsets[i] || offsets[i + 1] > block.compressedSize)
                return fail([&] {
                    return "sector " + std::to_string(i) + " bounds invalid: start=" + std::to_string(offsets[i]) +
                           " end=" + std::to_string(offsets[i + 1]) +
                           " compressedSize=" + std::to_string(block.compressedSize);
                });
        }
        return true;
    }

    [[nodiscard]] u32 sectors() const {
        return m_sectors;
    }
    [[nodiscard]] u32 sectorSize() const {
        return m_sectorSize;
    }
    /// The bytes sector @p i comes to: a whole sector, but for the last.
    [[nodiscard]] u32 sizeOf(u32 i) const {
        return std::min(m_sectorSize, m_block.uncompressedSize - i * m_sectorSize);
    }
    [[nodiscard]] std::span<const u8> storedOf(u32 i) const {
        return m_stored.subspan(m_offsets[i], m_offsets[i + 1] - m_offsets[i]);
    }
    [[nodiscard]] u32 keyOf(u32 i) const {
        return m_key != 0 ? m_key + i : 0;
    }
    [[nodiscard]] const BlockEntry& block() const {
        return m_block;
    }
    /// Whether sector @p i may be given its place before it decodes.
    [[nodiscard]] bool plausible(u32 i) const {
        return whiteout::common::plausibleOutput(sizeOf(i), m_offsets[i + 1] - m_offsets[i]);
    }

    /// Sectors [@p first, @p last) decoded to their places in @p file, the
    /// start of the file's whole output.
    bool decode(u32 first, u32 last, u8* file, UnitScratch& scratch, std::string* error) const {
        for (u32 i = first; i < last; ++i) {
            std::span<u8> const dst(file + size_t{i} * m_sectorSize, sizeOf(i));
            if (!decodeUnit(storedOf(i), m_block, dst, keyOf(i), scratch, error)) {
                if (error)
                    *error = "sector " + std::to_string(i) + ": " + *error;
                return false;
            }
        }
        return true;
    }

private:
    std::span<const u8> m_stored;
    BlockEntry m_block{};
    u32 m_sectorSize = 0;
    u32 m_sectors = 0;
    u32 m_key = 0;
    /// The table of a file of up to 62 sectors needs no allocation.
    std::array<u32, 64> m_inline{};
    std::vector<u32> m_large;
    const u32* m_offsets = nullptr;
};

/// How many sectors of @p sectorSize make a run of sectors for one task,
/// for @p bytes of output shared between @p threads.
u32 sectorsPerRun(size_t bytes, u32 sectorSize, size_t threads) {
    size_t const runBytes = std::max(kMinRunBytes, bytes / (threads * 2));
    return static_cast<u32>(std::max<size_t>(1, (runBytes + sectorSize - 1) / sectorSize));
}

/// Sectors [@p first, @p last) of @p f to their places in @p file. With a
/// pool and enough of them they go in runs, each into its own part of the
/// output, shared between the pool's threads and the caller's.
bool decodeSectors(const SectorFile& f, u32 first, u32 last, u8* file, interfaces::WorkerPool* pool,
                   std::string* error) {
    size_t const bytes = size_t{last - first} * f.sectorSize();
    size_t const threads = pool ? pool->threadCount() : 0;
    u32 const perRun = threads != 0 ? sectorsPerRun(bytes, f.sectorSize(), threads) : last - first;
    if (threads == 0 || bytes < kMinPooledBytes || perRun >= last - first) {
        UnitScratch scratch;
        return f.decode(first, last, file, scratch, error);
    }

    u32 const runs = (last - first + perRun - 1) / perRun;
    std::vector<std::string> errors(error ? runs : 0);
    std::atomic<bool> failed{false};
    runJobs(pool, runs, [&](size_t run) {
        if (failed.load(std::memory_order_acquire))
            return;
        u32 const from = first + static_cast<u32>(run) * perRun;
        UnitScratch scratch;
        if (!f.decode(from, std::min(last, from + perRun), file, scratch, error ? &errors[run] : nullptr))
            failed.store(true, std::memory_order_release);
    });

    if (!failed.load(std::memory_order_acquire))
        return true;
    for (auto& runError : errors) {
        if (!runError.empty()) {
            *error = std::move(runError);
            break;
        }
    }
    return false;
}

/// Where a file's stored bytes lie in the archive, when they lie inside it.
std::optional<std::span<const u8>> storedBytes(std::span<const u8> archiveData, size_t archiveOffset,
                                               const BlockEntry& block, u16 offsetHi) {
    u64 const dataStart = archiveOffset + (u64{offsetHi} << 32) + block.fileOffset;
    if (dataStart + block.compressedSize > archiveData.size())
        return std::nullopt;
    return archiveData.subspan(dataStart, block.compressedSize);
}

} // anonymous namespace

std::optional<std::vector<u8>> extractFileData(std::span<const u8> archiveData, size_t archiveOffset,
                                               const BlockEntry& block, u32 sectorSize, u32 fileKey,
                                               std::string* error, interfaces::WorkerPool* pool,
                                               u16 offsetHi) {
    auto setErr = [&](std::string msg) {
        if (error)
            *error = std::move(msg);
    };
    if (!block.exists()) {
        setErr("block does not exist");
        return std::nullopt;
    }
    if (block.uncompressedSize == 0)
        return std::vector<u8>{};

    auto const stored = storedBytes(archiveData, archiveOffset, block, offsetHi);
    if (!stored) {
        setErr("data out of bounds");
        return std::nullopt;
    }
    auto const fileSpan = *stored;

    // -- Patch file --
    // Read alone, as StormLib reads one outside a patch chain: the patch
    // data behind its patch info (length, flags, data size, MD5).
    if (hasFlag(block.flags, FileFlag::kPatchFile)) {
        auto const patch = splitPatchFile(fileSpan, block);
        if (!patch) {
            setErr("patch file without valid patch info");
            return std::nullopt;
        }
        return extractFileData(archiveData, archiveOffset + patch->first, patch->second, sectorSize, fileKey, error,
                               pool, offsetHi);
    }

    // -- Single-unit file --
    if (block.isSingleUnit()) {
        // One that did not shrink is copied as it is; one whose claim is
        // beyond what its stored bytes plausibly hold grows as it decodes.
        if (fileSpan.size() >= block.uncompressedSize ||
            !whiteout::common::plausibleOutput(block.uncompressedSize, fileSpan.size())) {
            std::string err;
            auto data = decodeUnitGrowing(fileSpan, block, block.uncompressedSize, fileKey, err);
            if (!data)
                setErr("single-unit file: " + err);
            return data;
        }
        std::vector<u8> output(block.uncompressedSize);
        UnitScratch scratch;
        std::string err;
        if (!decodeUnitInto(fileSpan, block, output, fileKey, scratch, error ? &err : nullptr)) {
            setErr("single-unit file: " + err);
            return std::nullopt;
        }
        return output;
    }

    // -- Multi-sector file --
    if (sectorSize == 0) {
        setErr("the archive has no sector size");
        return std::nullopt;
    }

    // -- Uncompressed (stored) file --
    // A file with no compression flag has no sector offset table — its data
    // is stored verbatim. Only encryption is applied, still per sector.
    if (!block.isCompressed()) {
        if (fileSpan.size() < block.uncompressedSize) {
            setErr("stored data is shorter than the file");
            return std::nullopt;
        }
        std::vector<u8> buf(fileSpan.begin(), fileSpan.begin() + block.uncompressedSize);
        if (block.isEncrypted() && fileKey != 0) {
            u32 const numSectors = static_cast<u32>((u64{block.uncompressedSize} + sectorSize - 1) / sectorSize);
            for (u32 i = 0; i < numSectors; ++i) {
                u32 const off = i * sectorSize;
                u32 const len = std::min(sectorSize, block.uncompressedSize - off);
                decryptBytes(std::span<u8>(buf).subspan(off, len), fileKey + i);
            }
        }
        return buf;
    }

    SectorFile file;
    if (!file.open(fileSpan, block, sectorSize, fileKey, error))
        return std::nullopt;

    // Every sector decodes to exactly its share, straight into its place.
    std::vector<u8> output;
    if (whiteout::common::plausibleOutput(block.uncompressedSize, block.compressedSize)) {
        output.resize(block.uncompressedSize);
        if (!decodeSectors(file, 0, file.sectors(), output.data(), pool, error))
            return std::nullopt;
        return output;
    }

    // The claim is beyond what the stored bytes plausibly hold: the output
    // grows a window at a time, by what really decoded, so a block cannot
    // make a reader set aside memory its sectors do not fill.
    output.reserve(whiteout::common::outputReserve(block.uncompressedSize, block.compressedSize));
    for (u32 i = 0; i < file.sectors();) {
        if (!file.plausible(i)) {
            std::string err;
            auto const data = decodeUnitGrowing(file.storedOf(i), block, file.sizeOf(i), file.keyOf(i), err);
            if (!data) {
                setErr("sector " + std::to_string(i) + ": " + err);
                return std::nullopt;
            }
            output.insert(output.end(), data->begin(), data->end());
            ++i;
            continue;
        }
        u32 end = i;
        size_t bytes = 0;
        while (end < file.sectors() && bytes < kGrowWindowBytes && file.plausible(end))
            bytes += file.sizeOf(end++);
        output.resize(output.size() + bytes);
        if (!decodeSectors(file, i, end, output.data(), pool, error))
            return std::nullopt;
        i = end;
    }
    return output;
}

// ============================================================================
// Encoding (Write)
// ============================================================================

namespace {

/// The codec for unit @p index of a file. ADPCM, being lossy, never takes
/// the first, which holds a wave's header (StormLib compresses it as data),
/// and goes with Huffman after it, as Blizzard stores waves. A version 2+
/// archive's readers take Huffman only in that pair: alone it becomes zlib.
CompressionFlag unitCodec(const EncodeOptions& opts, u32 index) {
    CompressionFlag const adpcm = opts.compression & (CompressionFlag::kAdpcmMono | CompressionFlag::kAdpcmStereo);
    if (adpcm != CompressionFlag::None)
        return index == 0 ? CompressionFlag::kPKware : adpcm | CompressionFlag::kHuffman;
    if (opts.modernCodecs && opts.compression == CompressionFlag::kHuffman)
        return CompressionFlag::kZlib;
    return opts.compression;
}

/// The checksums of @p rawData that @p opts asks for, into @p result. An
/// empty file's MD5 is left zero, as the StarCraft II editor leaves it.
void takeChecksums(std::span<const u8> rawData, const EncodeOptions& opts, EncodedFile& result) {
    if (opts.crc32)
        result.crc32 = crc32(rawData.data(), rawData.size());
    if (opts.md5 && !rawData.empty())
        result.md5 = storages::common::md5Hash(rawData);
}

/// @p rawData as a stored (uncompressed) multi-sector file. A stored file has
/// no sector offset table: readers take its bytes verbatim, sector by sector,
/// so one that compressed nowhere must not keep the table it was built with.
EncodedFile encodeStored(std::span<const u8> rawData, const EncodeOptions& opts, u32 sectorSize) {
    EncodedFile result;
    FileFlag flags = FileFlag::kExists;
    result.data.assign(rawData.begin(), rawData.end());
    if (opts.encrypt && !opts.filename.empty()) {
        flags |= FileFlag::kEncrypted;
        u32 const size = static_cast<u32>(rawData.size());
        u32 const fileKey = deriveFileKey(opts.filename, BlockEntry{0, size, size, flags});
        for (size_t at = 0, i = 0; at < rawData.size(); at += sectorSize, ++i)
            encryptBytes(std::span<u8>(result.data).subspan(at, std::min<size_t>(sectorSize, rawData.size() - at)),
                         fileKey + static_cast<u32>(i));
    }
    result.compressedSize = static_cast<u32>(result.data.size());
    result.flags = flags;
    return result;
}

/// Sectors [@p first, @p last) of @p rawData appended to @p out, each one
/// compressed when that makes it smaller and as it is otherwise; @p sizes
/// receives what each came to. Returns whether any was compressed.
bool encodeSectorRun(std::span<const u8> rawData, const EncodeOptions& opts, u32 first, u32 last, std::vector<u8>& out,
                     u32* sizes) {
    bool anyCompressed = false;
    for (u32 i = first; i < last; ++i) {
        size_t const at = size_t{i} * opts.sectorSize;
        auto const sector = rawData.subspan(at, std::min<size_t>(opts.sectorSize, rawData.size() - at));
        size_t const before = out.size();
        if (opts.compression != CompressionFlag::None && mpqCompressAppend(sector, unitCodec(opts, i), out))
            anyCompressed = true;
        else
            out.insert(out.end(), sector.begin(), sector.end());
        sizes[i] = static_cast<u32>(out.size() - before);
    }
    return anyCompressed;
}

/// The last step of a multi-sector file: @p result.data holds room for the
/// sector offset table and then every sector as encodeSectorRun left it.
/// The table is filled in and, when asked, table and sectors are encrypted.
void sealSectors(EncodedFile& result, std::span<const u8> rawData, const EncodeOptions& opts,
                 const std::vector<u32>& sizes) {
    u32 const numSectors = static_cast<u32>(sizes.size());
    std::vector<u32> offsets(numSectors + 1);
    offsets[0] = (numSectors + 1) * static_cast<u32>(sizeof(u32));
    for (u32 i = 0; i < numSectors; ++i)
        offsets[i + 1] = offsets[i] + sizes[i];

    u32 const total = offsets[numSectors];
    FileFlag flags = FileFlag::kExists | FileFlag::kCompress;
    if (opts.encrypt && !opts.filename.empty()) {
        flags |= FileFlag::kEncrypted;
        // The block's offset is the writer's to set: a key fixed to it is
        // the writer's to encrypt with again.
        u32 const fileKey =
            deriveFileKey(opts.filename, BlockEntry{0, total, static_cast<u32>(rawData.size()), flags});
        for (u32 i = 0; i < numSectors; ++i)
            encryptBytes(std::span<u8>(result.data).subspan(offsets[i], sizes[i]), fileKey + i);
        encryptBlock(offsets.data(), offsets.size(), fileKey - 1);
    }
    std::memcpy(result.data.data(), offsets.data(), offsets.size() * sizeof(u32));
    result.compressedSize = total;
    result.flags = flags;
}

/// A file stored with no sector smaller than it was: stored plainly, with
/// the checksums already taken.
EncodedFile storedInstead(const EncodedFile& checksums, std::span<const u8> rawData, const EncodeOptions& opts) {
    EncodedFile stored = encodeStored(rawData, opts, opts.sectorSize);
    stored.crc32 = checksums.crc32;
    stored.md5 = checksums.md5;
    return stored;
}

/// A large multi-sector file encoded in runs of sectors, each run into its
/// own buffer, the checksums beside them: jobs for a pool, then joined.
class SplitEncode {
public:
    SplitEncode(std::span<const u8> rawData, const EncodeOptions& opts, size_t threads)
        : m_rawData(rawData), m_opts(opts),
          m_sectors(static_cast<u32>((rawData.size() + opts.sectorSize - 1) / opts.sectorSize)),
          m_perRun(sectorsPerRun(rawData.size(), opts.sectorSize, threads)),
          m_runs((m_sectors + m_perRun - 1) / m_perRun), m_sizes(m_sectors), m_compressed(m_runs.size(), 0) {}

    /// The runs, and one more job for the checksums.
    [[nodiscard]] size_t jobs() const {
        return m_runs.size() + 1;
    }

    void run(size_t job) {
        if (job == m_runs.size()) {
            takeChecksums(m_rawData, m_opts, m_result);
            return;
        }
        u32 const first = static_cast<u32>(job) * m_perRun;
        u32 const last = std::min(m_sectors, first + m_perRun);
        m_runs[job].reserve(std::min<size_t>(size_t{last - first} * m_opts.sectorSize, m_rawData.size()));
        // A byte per run: vector<bool> packs neighbours into one word, which
        // the workers would race to write.
        m_compressed[job] = encodeSectorRun(m_rawData, m_opts, first, last, m_runs[job], m_sizes.data());
    }

    [[nodiscard]] EncodedFile finish() {
        if (std::find(m_compressed.begin(), m_compressed.end(), u8{1}) == m_compressed.end())
            return storedInstead(m_result, m_rawData, m_opts);
        size_t total = (size_t{m_sectors} + 1) * sizeof(u32);
        for (const auto& run : m_runs)
            total += run.size();
        m_result.data.reserve(total);
        m_result.data.resize((size_t{m_sectors} + 1) * sizeof(u32));
        for (auto& run : m_runs) {
            m_result.data.insert(m_result.data.end(), run.begin(), run.end());
            std::vector<u8>().swap(run);
        }
        sealSectors(m_result, m_rawData, m_opts, m_sizes);
        return std::move(m_result);
    }

private:
    std::span<const u8> m_rawData;
    const EncodeOptions& m_opts;
    u32 m_sectors;
    u32 m_perRun;
    std::vector<std::vector<u8>> m_runs;
    std::vector<u32> m_sizes;
    std::vector<u8> m_compressed;
    EncodedFile m_result;
};

/// Whether a file is worth encoding in runs across a pool's threads.
bool splitsForEncode(std::span<const u8> rawData, const EncodeOptions& opts) {
    return !opts.singleUnit && opts.compression != CompressionFlag::None && rawData.size() >= kSplitFileBytes;
}

} // anonymous namespace

EncodedFile encodeFileData(std::span<const u8> rawData, const EncodeOptions& opts,
                           interfaces::WorkerPool* pool) {
    EncodedFile result;
    if (rawData.empty())
        return result;

    size_t const threads = pool ? pool->threadCount() : 0;
    if (threads != 0 && splitsForEncode(rawData, opts)) {
        SplitEncode split(rawData, opts, threads);
        runJobs(pool, split.jobs(), [&](size_t job) { split.run(job); });
        return split.finish();
    }

    takeChecksums(rawData, opts, result);

    // -- Single-unit mode --
    if (opts.singleUnit) {
        FileFlag flags = FileFlag::kExists | FileFlag::kSingleUnit;
        result.data.reserve(rawData.size());
        if (opts.compression != CompressionFlag::None && mpqCompressAppend(rawData, unitCodec(opts, 0), result.data))
            flags |= FileFlag::kCompress;
        else
            result.data.assign(rawData.begin(), rawData.end()); // Store uncompressed.

        if (opts.encrypt && !opts.filename.empty()) {
            flags |= FileFlag::kEncrypted;
            u32 const fileKey =
                deriveFileKey(opts.filename, BlockEntry{0, static_cast<u32>(result.data.size()),
                                                        static_cast<u32>(rawData.size()), flags});
            encryptBytes(result.data, fileKey);
        }

        result.compressedSize = static_cast<u32>(result.data.size());
        result.flags = flags;
        return result;
    }

    // -- Multi-sector mode --
    // One buffer for the file: room for the sector offset table, then each
    // sector as it is encoded. No sector comes out larger than it went in.
    u32 const numSectors = static_cast<u32>((rawData.size() + opts.sectorSize - 1) / opts.sectorSize);
    size_t const tableBytes = (size_t{numSectors} + 1) * sizeof(u32);
    std::vector<u32> sizes(numSectors);
    result.data.reserve(tableBytes + rawData.size());
    result.data.resize(tableBytes);
    if (!encodeSectorRun(rawData, opts, 0, numSectors, result.data, sizes.data()))
        return storedInstead(result, rawData, opts);
    sealSectors(result, rawData, opts, sizes);
    return result;
}

// ============================================================================
// Batch Encoding
// ============================================================================

BatchEncodeResult encodeBatch(std::span<const std::pair<std::span<const u8>, EncodeOptions>> items,
                              interfaces::WorkerPool* pool) {
    BatchEncodeResult result;
    result.files.resize(items.size());

    size_t const threads = pool ? pool->threadCount() : 0;
    if (threads == 0) {
        for (size_t i = 0; i < items.size(); ++i)
            result.files[i] = encodeFileData(items[i].first, items[i].second, nullptr);
        return result;
    }

    // A job is a run of whole files, small ones grouped so that each job is
    // worth a thread's while, or one piece of a large file.
    struct Job {
        size_t firstFile = 0;
        size_t lastFile = 0;
        SplitEncode* split = nullptr;
        size_t piece = 0;
    };
    std::vector<std::pair<size_t, std::unique_ptr<SplitEncode>>> splits;
    std::vector<Job> jobs;
    size_t groupStart = 0;
    size_t groupBytes = 0;
    auto closeGroup = [&](size_t end) {
        if (end > groupStart)
            jobs.push_back({groupStart, end});
        groupStart = end;
        groupBytes = 0;
    };
    for (size_t i = 0; i < items.size(); ++i) {
        const auto& [rawData, opts] = items[i];
        if (!splitsForEncode(rawData, opts)) {
            groupBytes += rawData.size();
            if (groupBytes >= kMinRunBytes)
                closeGroup(i + 1);
            continue;
        }
        closeGroup(i);
        groupStart = i + 1;
        auto split = std::make_unique<SplitEncode>(rawData, opts, threads);
        for (size_t piece = 0; piece < split->jobs(); ++piece)
            jobs.push_back({0, 0, split.get(), piece});
        splits.emplace_back(i, std::move(split));
    }
    closeGroup(items.size());

    runJobs(pool, jobs.size(), [&](size_t index) {
        const Job& job = jobs[index];
        if (job.split) {
            job.split->run(job.piece);
            return;
        }
        for (size_t i = job.firstFile; i < job.lastFile; ++i)
            result.files[i] = encodeFileData(items[i].first, items[i].second, nullptr);
    });
    for (auto& [index, split] : splits)
        result.files[index] = split->finish();
    return result;
}

// ============================================================================
// Batch Extraction
// ============================================================================

std::vector<std::optional<std::vector<u8>>> extractBatch(std::span<const u8> archiveData,
                                                         size_t archiveOffset,
                                                         std::span<const ExtractFileInfo> files,
                                                         u32 sectorSize,
                                                         interfaces::WorkerPool* pool) {

    std::vector<std::optional<std::vector<u8>>> results(files.size());

    if (files.empty() || sectorSize == 0)
        return results;

    auto extractWhole = [&](size_t i) {
        results[i] = extractFileData(archiveData, archiveOffset, files[i].block, sectorSize, files[i].fileKey,
                                     nullptr, nullptr, files[i].offsetHi);
    };
    size_t const threads = pool ? pool->threadCount() : 0;
    if (threads == 0) {
        for (size_t i = 0; i < files.size(); ++i)
            extractWhole(i);
        return results;
    }

    // A job is a run of whole files, small ones grouped so that each job is
    // worth a thread's while, or a run of the sectors of one large file.
    struct Split {
        size_t file = 0;
        SectorFile sectors;
        std::atomic<bool> failed{false};
    };
    struct Job {
        size_t firstFile = 0;
        size_t lastFile = 0;
        Split* split = nullptr;
        u32 firstSector = 0;
        u32 lastSector = 0;
    };
    std::vector<std::unique_ptr<Split>> splits;
    std::vector<Job> jobs;
    size_t groupStart = 0;
    size_t groupBytes = 0;
    auto closeGroup = [&](size_t end) {
        if (end > groupStart)
            jobs.push_back({groupStart, end});
        groupStart = end;
        groupBytes = 0;
    };

    for (size_t i = 0; i < files.size(); ++i) {
        const BlockEntry& block = files[i].block;
        bool const splits_ = block.exists() && block.uncompressedSize >= kSplitFileBytes && block.isCompressed() &&
                             !block.isSingleUnit() && !hasFlag(block.flags, FileFlag::kPatchFile) &&
                             whiteout::common::plausibleOutput(block.uncompressedSize, block.compressedSize);
        if (!splits_) {
            groupBytes += block.uncompressedSize;
            if (groupBytes >= kMinRunBytes)
                closeGroup(i + 1);
            continue;
        }
        closeGroup(i);
        groupStart = i + 1;
        auto split = std::make_unique<Split>();
        split->file = i;
        auto const stored = storedBytes(archiveData, archiveOffset, block, files[i].offsetHi);
        if (!stored || !split->sectors.open(*stored, block, sectorSize, files[i].fileKey, nullptr))
            continue; // Its result stays empty.
        results[i].emplace(block.uncompressedSize);
        u32 const sectors = split->sectors.sectors();
        u32 const perRun = sectorsPerRun(block.uncompressedSize, sectorSize, threads);
        for (u32 from = 0; from < sectors; from += perRun)
            jobs.push_back({0, 0, split.get(), from, std::min(sectors, from + perRun)});
        splits.push_back(std::move(split));
    }
    closeGroup(files.size());

    runJobs(pool, jobs.size(), [&](size_t index) {
        const Job& job = jobs[index];
        if (!job.split) {
            for (size_t i = job.firstFile; i < job.lastFile; ++i)
                extractWhole(i);
            return;
        }
        Split& split = *job.split;
        if (split.failed.load(std::memory_order_acquire))
            return;
        UnitScratch scratch;
        if (!split.sectors.decode(job.firstSector, job.lastSector, results[split.file]->data(), scratch, nullptr))
            split.failed.store(true, std::memory_order_release);
    });

    for (const auto& split : splits) {
        if (split->failed.load(std::memory_order_acquire))
            results[split->file].reset();
    }
    return results;
}

} // namespace whiteout::storages::mpq
