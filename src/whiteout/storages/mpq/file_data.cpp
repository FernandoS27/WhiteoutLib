// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "codecs/compression.h"
#include "codecs/pkware.h"
#include "crypto.h"
#include "file_data.h"

#include "../../common/output_reserve.h"

#include <whiteout/interfaces.h>
#include <whiteout/utils/job_group.h>

#include <algorithm>
#include <atomic>
#include <cstring>

namespace whiteout::storages::mpq {

// ============================================================================
// File Key Derivation
// ============================================================================

namespace {

/// Extract the basename (filename after last '\\') for key derivation.
std::string getBaseName(const std::string& path) {
    auto pos = path.rfind('\\');
    if (pos == std::string::npos)
        pos = path.rfind('/');
    if (pos == std::string::npos)
        return path;
    return path.substr(pos + 1);
}

} // anonymous namespace

u32 deriveFileKey(const std::string& filename, const BlockEntry& block) {
    std::string const baseName = getBaseName(filename);
    u32 key = hashString(baseName, HashType::FileKey);
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

std::optional<u32> detectStoredFileKey(std::span<const u8> stored, const BlockEntry& block, u32 sectorSize) {
    if (block.isSingleUnit() || !block.isCompressed() || block.uncompressedSize == 0 || stored.size() < 4)
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

/// One unit of a file as the archive stores it — a sector, or the whole of
/// a single-unit file — decrypted with @p key and expanded to exactly
/// @p size bytes. A unit that did not shrink is stored as it is.
std::optional<std::vector<u8>> decodeUnit(std::span<const u8> stored, const BlockEntry& block,
                                          u32 size, u32 key, std::string& error) {
    std::vector<u8> buf(stored.begin(), stored.end());
    if (block.isEncrypted() && key != 0 && buf.size() >= 4)
        decryptBlock(reinterpret_cast<u32*>(buf.data()), buf.size() / 4, key);
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

    u64 const dataStart = archiveOffset + (u64{offsetHi} << 32) + block.fileOffset;
    if (dataStart + block.compressedSize > archiveData.size()) {
        setErr("data out of bounds");
        return std::nullopt;
    }

    auto fileSpan = archiveData.subspan(dataStart, block.compressedSize);

    // -- Single-unit file --
    if (block.isSingleUnit()) {
        std::string err;
        auto data = decodeUnit(fileSpan, block, block.uncompressedSize, fileKey, err);
        if (!data)
            setErr("single-unit file: " + err);
        return data;
    }

    // -- Multi-sector file --
    if (sectorSize == 0) {
        setErr("the archive has no sector size");
        return std::nullopt;
    }
    u32 const numSectors = static_cast<u32>((u64{block.uncompressedSize} + sectorSize - 1) / sectorSize);

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
            for (u32 i = 0; i < numSectors; ++i) {
                u32 const off = i * sectorSize;
                u32 const len = std::min(sectorSize, block.uncompressedSize - off);
                size_t const alignedCount = len / 4;
                if (alignedCount > 0) {
                    decryptBlock(reinterpret_cast<u32*>(buf.data() + off), alignedCount,
                                 fileKey + i);
                }
            }
        }
        return buf;
    }

    bool const hasCrc = block.hasSectorCrc();
    // When sector CRC is present the offset table has one extra entry pointing
    // to the CRC block at the end of the file data, making it numSectors + 2
    // entries total instead of numSectors + 1.
    u32 const numOffsetEntries = numSectors + 1 + (hasCrc ? 1u : 0u);

    // Read and decrypt the sector offset table.
    u32 const offsetTableSize = numOffsetEntries * sizeof(u32);
    if (fileSpan.size() < offsetTableSize) {
        setErr("sector offset table truncated: have=" + std::to_string(fileSpan.size()) +
               " need=" + std::to_string(offsetTableSize));
        return std::nullopt;
    }

    std::vector<u32> sectorOffsets(numOffsetEntries);
    std::memcpy(sectorOffsets.data(), fileSpan.data(), offsetTableSize);

    if (block.isEncrypted() && fileKey != 0) {
        decryptBlock(sectorOffsets.data(), sectorOffsets.size(), fileKey - 1);
    }

    // Validate offsets.
    if (sectorOffsets[0] != offsetTableSize) {
        setErr("sector offset table validation failed: offsets[0]=" +
               std::to_string(sectorOffsets[0]) + " expected=" + std::to_string(offsetTableSize) +
               " numSectors=" + std::to_string(numSectors));
        return std::nullopt;
    }

    // Validate all sector offsets up-front (serial — required before parallel submission).
    for (u32 i = 0; i < numSectors; ++i) {
        u32 const sectorStart = sectorOffsets[i];
        u32 const sectorEnd = sectorOffsets[i + 1];
        if (sectorEnd < sectorStart || sectorEnd > block.compressedSize) {
            setErr("sector " + std::to_string(i) + " bounds invalid: start=" +
                   std::to_string(sectorStart) + " end=" + std::to_string(sectorEnd) +
                   " compressedSize=" + std::to_string(block.compressedSize));
            return std::nullopt;
        }
    }

    // Every sector decodes to exactly its share. They decode a batch at a
    // time and the output grows by what really decoded: a block cannot make
    // a reader set aside memory its sectors do not fill.
    static constexpr u32 kSectorBatch = 64;
    std::vector<u8> output;
    output.reserve(whiteout::common::outputReserve(block.uncompressedSize, block.compressedSize));
    std::vector<std::vector<u8>> decoded(std::min(numSectors, kSectorBatch));
    std::vector<std::string> sectorErrors(decoded.size());
    bool const parallel = pool && pool->threadCount() > 0 && numSectors >= 4;
    for (u32 base = 0; base < numSectors; base += kSectorBatch) {
        u32 const count = std::min(kSectorBatch, numSectors - base);
        for (auto& sectorError : sectorErrors)
            sectorError.clear();
        auto decodeSector = [&](u32 k) {
            u32 const i = base + k;
            u32 const size = std::min(sectorSize, block.uncompressedSize - i * sectorSize);
            u32 const key = fileKey != 0 ? fileKey + i : 0;
            auto data = decodeUnit(fileSpan.subspan(sectorOffsets[i], sectorOffsets[i + 1] - sectorOffsets[i]),
                                   block, size, key, sectorErrors[k]);
            if (!data) {
                sectorErrors[k] = "sector " + std::to_string(i) + ": " + sectorErrors[k];
                return false;
            }
            decoded[k] = std::move(*data);
            return true;
        };

        bool ok = true;
        if (parallel) {
            std::atomic<bool> failed{false};
            utils::JobGroup jobGroup;
            jobGroup.add(count);
            for (u32 k = 0; k < count; ++k) {
                interfaces::WorkerTask task;
                task.fn = [k, &decodeSector, &failed, &jobGroup]() {
                    if (!failed.load(std::memory_order_acquire) && !decodeSector(k))
                        failed.store(true, std::memory_order_release);
                    jobGroup.done();
                };
                pool->submit(task);
            }
            jobGroup.wait();
            ok = !failed.load(std::memory_order_acquire);
        } else {
            for (u32 k = 0; k < count && ok; ++k)
                ok = decodeSector(k);
        }
        if (!ok) {
            for (auto& sectorError : sectorErrors) {
                if (!sectorError.empty()) {
                    setErr(std::move(sectorError));
                    break;
                }
            }
            return std::nullopt;
        }
        for (u32 k = 0; k < count; ++k)
            output.insert(output.end(), decoded[k].begin(), decoded[k].end());
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
        for (size_t at = 0, i = 0; at < rawData.size(); at += sectorSize, ++i) {
            size_t const alignedCount = std::min<size_t>(sectorSize, rawData.size() - at) / 4;
            if (alignedCount > 0)
                encryptBlock(reinterpret_cast<u32*>(result.data.data() + at), alignedCount,
                             fileKey + static_cast<u32>(i));
        }
    }
    result.compressedSize = static_cast<u32>(result.data.size());
    result.flags = flags;
    return result;
}

} // anonymous namespace

EncodedFile encodeFileData(std::span<const u8> rawData, const EncodeOptions& opts,
                           interfaces::WorkerPool* pool) {
    EncodedFile result;
    if (rawData.empty())
        return result;

    FileFlag flags = FileFlag::kExists;

    // -- Single-unit mode --
    if (opts.singleUnit) {
        flags |= FileFlag::kSingleUnit;
        std::vector<u8> encoded;

        if (opts.compression != CompressionFlag::None) {
            flags |= FileFlag::kCompress;
            auto compressed = mpqCompress(rawData, unitCodec(opts, 0));
            if (!compressed.empty()) {
                encoded = std::move(compressed);
            }
        }

        if (encoded.empty()) {
            // Store uncompressed.
            flags &= ~FileFlag::kCompress;
            encoded.assign(rawData.begin(), rawData.end());
        }

        // Encrypt if needed.
        u32 fileKey = 0;
        if (opts.encrypt && !opts.filename.empty()) {
            flags |= FileFlag::kEncrypted;
            fileKey =
                deriveFileKey(opts.filename, BlockEntry{0, static_cast<u32>(encoded.size()),
                                                        static_cast<u32>(rawData.size()), flags});
            size_t const alignedCount = encoded.size() / 4;
            if (alignedCount > 0) {
                encryptBlock(reinterpret_cast<u32*>(encoded.data()), alignedCount, fileKey);
            }
        }

        result.data = std::move(encoded);
        result.compressedSize = static_cast<u32>(result.data.size());
        result.flags = flags;
        return result;
    }

    // -- Multi-sector mode --
    u32 sectorSize = opts.sectorSize;
    u32 const numSectors = (static_cast<u32>(rawData.size()) + sectorSize - 1) / sectorSize;

    std::vector<std::vector<u8>> sectorData(numSectors);
    bool anyCompressed = false;

    // --- Sector compress: compress a single raw sector ---
    auto compressSector = [&](u32 i) -> bool {
        size_t const srcStart = static_cast<size_t>(i) * sectorSize;
        size_t const srcLen = std::min<size_t>(sectorSize, rawData.size() - srcStart);
        auto sectorRaw = rawData.subspan(srcStart, srcLen);

        if (opts.compression != CompressionFlag::None) {
            auto compressed = mpqCompress(sectorRaw, unitCodec(opts, i));
            if (!compressed.empty()) {
                sectorData[i] = std::move(compressed);
                return true; // was compressed
            }
        }
        sectorData[i].assign(sectorRaw.begin(), sectorRaw.end());
        return false; // stored uncompressed
    };

    if (pool && pool->threadCount() > 0 && numSectors >= 2) {
        // A byte per sector: vector<bool> packs neighbours into one word,
        // which the workers would race to write.
        std::vector<u8> sectorCompressed(numSectors, 0);
        std::atomic<bool> encodeFailed{false};
        utils::JobGroup jobGroup;
        jobGroup.add(numSectors);

        for (u32 i = 0; i < numSectors; ++i) {
            interfaces::WorkerTask task;
            task.fn = [i, &compressSector, &sectorCompressed, &encodeFailed, &jobGroup]() {
                if (!encodeFailed.load(std::memory_order_acquire))
                    sectorCompressed[i] = compressSector(i);
                jobGroup.done();
            };
            pool->submit(task);
        }
        jobGroup.wait();

        for (u32 i = 0; i < numSectors; ++i) {
            if (sectorCompressed[i]) {
                anyCompressed = true;
                break;
            }
        }
    } else {
        for (u32 i = 0; i < numSectors; ++i) {
            if (compressSector(i))
                anyCompressed = true;
        }
    }

    if (!anyCompressed)
        return encodeStored(rawData, opts, sectorSize);

    // Build sector offset table from compressed sizes.
    std::vector<u32> sectorOffsets;
    sectorOffsets.reserve(numSectors + 1);
    u32 currentOffset = (numSectors + 1) * sizeof(u32);
    sectorOffsets.push_back(currentOffset);
    for (u32 i = 0; i < numSectors; ++i) {
        currentOffset += static_cast<u32>(sectorData[i].size());
        sectorOffsets.push_back(currentOffset);
    }

    if (anyCompressed) {
        flags |= FileFlag::kCompress;
    }

    // Derive encryption key if needed.
    u32 fileKey = 0;
    if (opts.encrypt && !opts.filename.empty()) {
        flags |= FileFlag::kEncrypted;
        // For key derivation, we need the block entry. The offset will be
        // set by the writer, so we use 0 here. If FIX_KEY is needed, the
        // writer must re-encrypt with the correct offset.
        fileKey = deriveFileKey(
            opts.filename, BlockEntry{0, currentOffset, static_cast<u32>(rawData.size()), flags});
    }

    // Encrypt sector offsets.
    std::vector<u32> encSectorOffsets = sectorOffsets;
    if (fileKey != 0) {
        encryptBlock(encSectorOffsets.data(), encSectorOffsets.size(), fileKey - 1);
    }

    // Encrypt each sector.
    if (fileKey != 0) {
        for (u32 i = 0; i < numSectors; ++i) {
            size_t const alignedCount = sectorData[i].size() / 4;
            if (alignedCount > 0) {
                encryptBlock(reinterpret_cast<u32*>(sectorData[i].data()), alignedCount,
                             fileKey + i);
            }
        }
    }

    // Assemble final data.
    result.data.resize(currentOffset);
    std::memcpy(result.data.data(), encSectorOffsets.data(), encSectorOffsets.size() * sizeof(u32));

    size_t writePos = encSectorOffsets.size() * sizeof(u32);
    for (const auto& sector : sectorData) {
        std::memcpy(result.data.data() + writePos, sector.data(), sector.size());
        writePos += sector.size();
    }

    result.compressedSize = currentOffset;
    result.flags = flags;
    return result;
}

// ============================================================================
// Batch Encoding (Flattened Pipeline)
// ============================================================================

BatchEncodeResult encodeBatch(std::span<const std::pair<std::span<const u8>, EncodeOptions>> items,
                              interfaces::WorkerPool* pool) {
    BatchEncodeResult result;
    result.files.resize(items.size());

    if (items.empty())
        return result;

    const bool usePool = pool && pool->threadCount() > 0;

    // Check semaphore support.
    const bool useSemaphores = usePool && [&]() {
        auto test = pool->createTimelineSemaphore();
        return test != nullptr;
    }();

    if (!useSemaphores) {
        // Fallback: current strategy (parallel files XOR parallel sectors).
        if (usePool && items.size() >= 2) {
            std::atomic<bool> failed{false};
            utils::JobGroup jobGroup;
            jobGroup.add(items.size());

            for (size_t i = 0; i < items.size(); ++i) {
                interfaces::WorkerTask task;
                task.fn = [i, &items, &result, &failed, &jobGroup]() {
                    if (!failed.load(std::memory_order_acquire))
                        result.files[i] = encodeFileData(items[i].first, items[i].second, nullptr);
                    jobGroup.done();
                };
                pool->submit(task);
            }
            jobGroup.wait();
        } else {
            for (size_t i = 0; i < items.size(); ++i) {
                result.files[i] =
                    encodeFileData(items[i].first, items[i].second, usePool ? pool : nullptr);
            }
        }
        return result;
    }

    // ── Flattened pipeline with timeline semaphores ──

    struct FileState {
        std::unique_ptr<interfaces::TimelineSemaphore> sem;
        interfaces::TimelineSemaphore::Value completeDone = 0;
        std::vector<SectorResult> sectorResults;
    };

    std::vector<FileState> states(items.size());
    std::atomic<bool> failed{false};

    for (size_t i = 0; i < items.size(); ++i) {
        const auto& [rawData, opts] = items[i];
        auto& state = states[i];
        state.sem = pool->createTimelineSemaphore();

        // Fast path: empty, single-unit, or 1-sector files — single task, no JobGroup.
        u32 numSectors = 0;
        if (!rawData.empty() && !opts.singleUnit) {
            numSectors = (static_cast<u32>(rawData.size()) + opts.sectorSize - 1) / opts.sectorSize;
        }

        if (rawData.empty() || opts.singleUnit || numSectors <= 1) {
            state.completeDone = state.sem->next();
            interfaces::WorkerTask task;
            task.fn = [i, &items, &result, &failed]() {
                if (!failed.load(std::memory_order_acquire))
                    result.files[i] = encodeFileData(items[i].first, items[i].second, nullptr);
            };
            task.signalSemaphore = state.sem.get();
            task.signalValue = state.completeDone;
            pool->submit(task);
            continue;
        }

        // Multi-sector: flatten into Phase 1 (compress) + Phase 2 (assemble).
        state.sectorResults.resize(numSectors);

        // Phase 1: submit sector compression tasks.
        auto compressDone = state.sem->next();
        auto compressGroup = std::make_shared<utils::JobGroup>();
        compressGroup->add(numSectors);
        compressGroup->signalOnComplete(state.sem.get(), compressDone);

        for (u32 j = 0; j < numSectors; ++j) {
            size_t const srcStart = static_cast<size_t>(j) * opts.sectorSize;
            size_t const srcLen = std::min<size_t>(opts.sectorSize, rawData.size() - srcStart);

            interfaces::WorkerTask task;
            task.fn = [j, srcStart, srcLen, i, &items, &states, &failed, compressGroup]() {
                if (!failed.load(std::memory_order_acquire)) {
                    const auto& [rawData, opts] = items[i];
                    auto sectorRaw = rawData.subspan(srcStart, srcLen);
                    auto& sr = states[i].sectorResults[j];

                    if (opts.compression != CompressionFlag::None) {
                        auto compressed = mpqCompress(sectorRaw, unitCodec(opts, j));
                        if (!compressed.empty()) {
                            sr.data = std::move(compressed);
                            sr.wasCompressed = true;
                        } else {
                            sr.data.assign(sectorRaw.begin(), sectorRaw.end());
                        }
                    } else {
                        sr.data.assign(sectorRaw.begin(), sectorRaw.end());
                    }
                }
                compressGroup->done();
            };
            pool->submit(task);
        }

        // Phase 2: assembly task (waits for all sectors of this file).
        state.completeDone = state.sem->next();
        interfaces::WorkerTask assembleTask;
        assembleTask.fn = [i, &items, &result, &states, &failed, numSectors]() {
            if (failed.load(std::memory_order_acquire))
                return;

            const auto& [rawData, opts] = items[i];
            auto& state = states[i];

            FileFlag flags = FileFlag::kExists;
            bool anyCompressed = false;
            for (u32 j = 0; j < numSectors; ++j) {
                if (state.sectorResults[j].wasCompressed) {
                    anyCompressed = true;
                    break;
                }
            }
            if (!anyCompressed) {
                result.files[i] = encodeStored(rawData, opts, opts.sectorSize);
                return;
            }
            flags |= FileFlag::kCompress;

            // Build sector offset table.
            std::vector<u32> sectorOffsets;
            sectorOffsets.reserve(numSectors + 1);
            u32 currentOffset = (numSectors + 1) * sizeof(u32);
            sectorOffsets.push_back(currentOffset);
            for (u32 j = 0; j < numSectors; ++j) {
                currentOffset += static_cast<u32>(state.sectorResults[j].data.size());
                sectorOffsets.push_back(currentOffset);
            }

            // Derive encryption key if needed.
            u32 fileKey = 0;
            if (opts.encrypt && !opts.filename.empty()) {
                flags |= FileFlag::kEncrypted;
                fileKey = deriveFileKey(
                    opts.filename,
                    BlockEntry{0, currentOffset, static_cast<u32>(rawData.size()), flags});
            }

            // Encrypt sector offsets.
            auto encOffsets = sectorOffsets;
            if (fileKey != 0)
                encryptBlock(encOffsets.data(), encOffsets.size(), fileKey - 1);

            // Encrypt each sector.
            if (fileKey != 0) {
                for (u32 j = 0; j < numSectors; ++j) {
                    size_t const aligned = state.sectorResults[j].data.size() / 4;
                    if (aligned > 0)
                        encryptBlock(reinterpret_cast<u32*>(state.sectorResults[j].data.data()),
                                     aligned, fileKey + j);
                }
            }

            // Assemble final data.
            EncodedFile& ef = result.files[i];
            ef.data.resize(currentOffset);
            std::memcpy(ef.data.data(), encOffsets.data(), encOffsets.size() * sizeof(u32));
            size_t writePos = encOffsets.size() * sizeof(u32);
            for (u32 j = 0; j < numSectors; ++j) {
                std::memcpy(ef.data.data() + writePos, state.sectorResults[j].data.data(),
                            state.sectorResults[j].data.size());
                writePos += state.sectorResults[j].data.size();
            }
            ef.compressedSize = currentOffset;
            ef.flags = flags;
        };
        assembleTask.waitSemaphore = state.sem.get();
        assembleTask.waitValue = compressDone;
        assembleTask.signalSemaphore = state.sem.get();
        assembleTask.signalValue = state.completeDone;
        pool->submit(assembleTask);
    }

    // Wait for all files to complete.
    for (size_t i = 0; i < items.size(); ++i) {
        states[i].sem->wait(states[i].completeDone);
    }

    return result;
}

// ============================================================================
// Batch Extraction (Flattened Pipeline)
// ============================================================================

std::vector<std::optional<std::vector<u8>>> extractBatch(std::span<const u8> archiveData,
                                                         size_t archiveOffset,
                                                         std::span<const ExtractFileInfo> files,
                                                         u32 sectorSize,
                                                         interfaces::WorkerPool* pool) {

    std::vector<std::optional<std::vector<u8>>> results(files.size());

    if (files.empty() || sectorSize == 0)
        return results;

    const bool usePool = pool && pool->threadCount() > 0;

    // Check semaphore support.
    const bool useSemaphores = usePool && [&]() {
        auto test = pool->createTimelineSemaphore();
        return test != nullptr;
    }();

    if (!useSemaphores) {
        // Fallback: serial per-file extraction with sector-level parallelism.
        for (size_t i = 0; i < files.size(); ++i)
            results[i] = extractFileData(archiveData, archiveOffset, files[i].block, sectorSize,
                                         files[i].fileKey, nullptr, usePool ? pool : nullptr,
                                         files[i].offsetHi);
        return results;
    }

    // ── Flattened pipeline with timeline semaphores ──

    struct FileState {
        std::unique_ptr<interfaces::TimelineSemaphore> sem;
        interfaces::TimelineSemaphore::Value completeDone = 0;
        std::vector<std::vector<u8>> sectorResults;
        std::atomic<bool> fileFailed{false};
        std::string errorMsg;
    };

    std::vector<FileState> states(files.size());

    for (size_t i = 0; i < files.size(); ++i) {
        const auto& fi = files[i];
        const auto& block = fi.block;
        auto& state = states[i];
        state.sem = pool->createTimelineSemaphore();

        // Skip non-existent or empty files.
        if (!block.exists() || block.uncompressedSize == 0) {
            if (block.exists() && block.uncompressedSize == 0)
                results[i] = std::vector<u8>{};
            state.completeDone = state.sem->next();
            // Signal immediately so the final wait doesn't block.
            state.sem->signal(state.completeDone);
            continue;
        }

        u64 const dataStart = archiveOffset + (u64{fi.offsetHi} << 32) + block.fileOffset;
        if (dataStart + block.compressedSize > archiveData.size()) {
            // Out of bounds — skip.
            state.completeDone = state.sem->next();
            state.sem->signal(state.completeDone);
            continue;
        }

        auto fileSpan = archiveData.subspan(dataStart, block.compressedSize);

        // Single-unit files: single task, no sector offset table.
        if (block.isSingleUnit()) {
            state.completeDone = state.sem->next();
            interfaces::WorkerTask task;
            task.fn = [i, &files, &results, fileSpan]() {
                const auto& block = files[i].block;
                std::string err;
                results[i] = decodeUnit(fileSpan, block, block.uncompressedSize, files[i].fileKey, err);
            };
            task.signalSemaphore = state.sem.get();
            task.signalValue = state.completeDone;
            pool->submit(task);
            continue;
        }

        // A stored multi-sector file has no sector table to split work by.
        if (!block.isCompressed()) {
            results[i] = extractFileData(archiveData, archiveOffset, block, sectorSize, fi.fileKey,
                                         nullptr, nullptr, fi.offsetHi);
            state.completeDone = state.sem->next();
            state.sem->signal(state.completeDone);
            continue;
        }

        // Multi-sector file: validate + decrypt sector offset table (serial).
        u32 const numSectors = static_cast<u32>((u64{block.uncompressedSize} + sectorSize - 1) / sectorSize);
        bool const hasCrc = block.hasSectorCrc();
        u32 const numOffsetEntries = numSectors + 1 + (hasCrc ? 1u : 0u);
        u32 const offsetTableSize = numOffsetEntries * sizeof(u32);

        if (fileSpan.size() < offsetTableSize) {
            state.completeDone = state.sem->next();
            state.sem->signal(state.completeDone);
            continue;
        }

        auto sectorOffsets = std::make_shared<std::vector<u32>>(numOffsetEntries);
        std::memcpy(sectorOffsets->data(), fileSpan.data(), offsetTableSize);

        if (block.isEncrypted() && fi.fileKey != 0) {
            decryptBlock(sectorOffsets->data(), sectorOffsets->size(), fi.fileKey - 1);
        }

        // Validate offsets.
        if ((*sectorOffsets)[0] != offsetTableSize) {
            state.completeDone = state.sem->next();
            state.sem->signal(state.completeDone);
            continue;
        }

        bool offsetsValid = true;
        for (u32 s = 0; s < numSectors; ++s) {
            if ((*sectorOffsets)[s + 1] < (*sectorOffsets)[s] ||
                (*sectorOffsets)[s + 1] > block.compressedSize) {
                offsetsValid = false;
                break;
            }
        }
        if (!offsetsValid) {
            state.completeDone = state.sem->next();
            state.sem->signal(state.completeDone);
            continue;
        }

        state.sectorResults.resize(numSectors);

        // Phase 1: submit sector decompression tasks.
        auto decodeDone = state.sem->next();
        auto decodeGroup = std::make_shared<utils::JobGroup>();
        decodeGroup->add(numSectors);
        decodeGroup->signalOnComplete(state.sem.get(), decodeDone);

        for (u32 j = 0; j < numSectors; ++j) {
            interfaces::WorkerTask task;
            task.fn = [i, j, &files, &states, fileSpan, sectorOffsets, numSectors, sectorSize,
                       decodeGroup]() {
                if (states[i].fileFailed.load(std::memory_order_acquire)) {
                    decodeGroup->done();
                    return;
                }

                const auto& block = files[i].block;
                u32 const fileKey = files[i].fileKey;
                u32 const sectorStart = (*sectorOffsets)[j];
                u32 const sectorEnd = (*sectorOffsets)[j + 1];
                u32 const expectedSize =
                    (j < numSectors - 1) ? sectorSize : (block.uncompressedSize - j * sectorSize);

                std::string err;
                auto sector = decodeUnit(fileSpan.subspan(sectorStart, sectorEnd - sectorStart), block,
                                         expectedSize, fileKey != 0 ? fileKey + j : 0, err);
                if (sector)
                    states[i].sectorResults[j] = std::move(*sector);
                else
                    states[i].fileFailed.store(true, std::memory_order_release);
                decodeGroup->done();
            };
            pool->submit(task);
        }

        // Phase 2: assembly task — concatenate sector results.
        state.completeDone = state.sem->next();
        interfaces::WorkerTask assembleTask;
        assembleTask.fn = [i, &files, &results, &states, numSectors]() {
            if (states[i].fileFailed.load(std::memory_order_acquire))
                return;

            const auto& block = files[i].block;
            std::vector<u8> output(block.uncompressedSize);
            size_t writePos = 0;
            for (u32 j = 0; j < numSectors; ++j) {
                std::memcpy(output.data() + writePos, states[i].sectorResults[j].data(),
                            states[i].sectorResults[j].size());
                writePos += states[i].sectorResults[j].size();
            }
            results[i] = std::move(output);
        };
        assembleTask.waitSemaphore = state.sem.get();
        assembleTask.waitValue = decodeDone;
        assembleTask.signalSemaphore = state.sem.get();
        assembleTask.signalValue = state.completeDone;
        pool->submit(assembleTask);
    }

    // Wait for all files to complete.
    for (size_t i = 0; i < files.size(); ++i) {
        states[i].sem->wait(states[i].completeDone);
    }

    return results;
}

} // namespace whiteout::storages::mpq
