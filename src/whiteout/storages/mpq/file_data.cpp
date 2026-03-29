// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "codecs/compression.h"
#include "crypto.h"
#include "file_data.h"

#include <whiteout/interfaces.h>

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
    std::string baseName = getBaseName(filename);
    u32 key = hashString(baseName, HashType::FileKey);
    if (block.hasFixKey()) {
        key = (key + block.fileOffset) ^ block.uncompressedSize;
    }
    return key;
}

// ============================================================================
// Extraction (Read)
// ============================================================================

std::vector<u8> extractFileData(std::span<const u8> archiveData, size_t archiveOffset,
                                const BlockEntry& block, u32 sectorSize, u32 fileKey,
                                std::string* error, interfaces::WorkerPool* pool) {
    auto setErr = [&](std::string msg) {
        if (error)
            *error = std::move(msg);
    };
    if (!block.exists()) {
        setErr("block does not exist");
        return {};
    }
    if (block.uncompressedSize == 0)
        return {};

    u64 dataStart = archiveOffset + block.fileOffset;
    if (dataStart + block.compressedSize > archiveData.size()) {
        setErr("data out of bounds");
        return {};
    }

    auto fileSpan = archiveData.subspan(dataStart, block.compressedSize);

    // -- Single-unit file --
    if (block.isSingleUnit()) {
        std::vector<u8> buf(fileSpan.begin(), fileSpan.end());

        // Decrypt if needed.
        if (block.isEncrypted() && fileKey != 0) {
            size_t alignedCount = buf.size() / 4;
            if (alignedCount > 0) {
                decryptBlock(reinterpret_cast<u32*>(buf.data()), alignedCount, fileKey);
            }
        }

        // Decompress if needed.
        if (block.isCompressed() && block.compressedSize < block.uncompressedSize) {
            std::string decompErr;
            auto decompressed =
                mpqDecompress(std::span<const u8>(buf), block.uncompressedSize, &decompErr);
            if (decompressed.empty()) {
                setErr(decompErr.empty() ? "single-unit decompression failed"
                                         : std::move(decompErr));
                return {};
            }
            return decompressed;
        }

        buf.resize(block.uncompressedSize);
        return buf;
    }

    // -- Multi-sector file --
    u32 numSectors = (block.uncompressedSize + sectorSize - 1) / sectorSize;
    bool hasCrc = block.hasSectorCrc();
    // When sector CRC is present the offset table has one extra entry pointing
    // to the CRC block at the end of the file data, making it numSectors + 2
    // entries total instead of numSectors + 1.
    u32 numOffsetEntries = numSectors + 1 + (hasCrc ? 1u : 0u);

    // Read and decrypt the sector offset table.
    u32 offsetTableSize = numOffsetEntries * sizeof(u32);
    if (fileSpan.size() < offsetTableSize) {
        setErr("sector offset table truncated: have=" + std::to_string(fileSpan.size()) +
               " need=" + std::to_string(offsetTableSize));
        return {};
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
        return {};
    }

    // Validate all sector offsets up-front (serial — required before parallel submission).
    for (u32 i = 0; i < numSectors; ++i) {
        u32 sectorStart = sectorOffsets[i];
        u32 sectorEnd = sectorOffsets[i + 1];
        if (sectorEnd < sectorStart || sectorEnd > block.compressedSize) {
            setErr("sector " + std::to_string(i) + " bounds invalid: start=" +
                   std::to_string(sectorStart) + " end=" + std::to_string(sectorEnd) +
                   " compressedSize=" + std::to_string(block.compressedSize));
            return {};
        }
    }

    // --- Sector decode: decrypt + decompress a single sector ---
    auto decodeSector = [&](u32 sectorIdx, u32 sectorStart, u32 sectorEnd,
                            u32 expectedUncompressed) -> std::pair<std::vector<u8>, std::string> {
        std::vector<u8> sectorBuf(fileSpan.data() + sectorStart, fileSpan.data() + sectorEnd);
        u32 sectorLen = sectorEnd - sectorStart;

        if (block.isEncrypted() && fileKey != 0) {
            size_t alignedCount = sectorBuf.size() / 4;
            if (alignedCount > 0) {
                decryptBlock(reinterpret_cast<u32*>(sectorBuf.data()), alignedCount,
                             fileKey + sectorIdx);
            }
        }

        if (block.isCompressed() && sectorLen < expectedUncompressed) {
            std::string decompErr;
            auto decompressed =
                mpqDecompress(std::span<const u8>(sectorBuf), expectedUncompressed, &decompErr);
            if (decompressed.empty()) {
                return {{},
                        "sector " + std::to_string(sectorIdx) + " decomp failed: " +
                            (decompErr.empty() ? std::string("unknown") : std::move(decompErr))};
            }
            return {std::move(decompressed), {}};
        }

        sectorBuf.resize(expectedUncompressed);
        return {std::move(sectorBuf), {}};
    };

    auto expectedSectorSize = [&](u32 i) -> u32 {
        return (i < numSectors - 1) ? sectorSize : (block.uncompressedSize - i * sectorSize);
    };

    // --- Parallel sector extraction (when pool available and >= 4 sectors) ---
    if (pool && numSectors >= 4) {
        std::vector<std::vector<u8>> sectorResults(numSectors);
        std::vector<std::string> sectorErrors(numSectors);
        std::atomic<bool> failed{false};

        for (u32 i = 0; i < numSectors; ++i) {
            interfaces::WorkerTask task;
            task.fn = [i, &sectorOffsets, &expectedSectorSize, &decodeSector, &sectorResults,
                       &sectorErrors, &failed]() {
                auto [data, err] =
                    decodeSector(i, sectorOffsets[i], sectorOffsets[i + 1], expectedSectorSize(i));
                if (!err.empty()) {
                    failed.store(true, std::memory_order_relaxed);
                    sectorErrors[i] = std::move(err);
                } else {
                    sectorResults[i] = std::move(data);
                }
            };
            pool->submit(task);
        }
        pool->waitIdle();

        if (failed.load(std::memory_order_relaxed)) {
            for (u32 i = 0; i < numSectors; ++i) {
                if (!sectorErrors[i].empty()) {
                    setErr(std::move(sectorErrors[i]));
                    break;
                }
            }
            return {};
        }

        std::vector<u8> output(block.uncompressedSize);
        size_t writePos = 0;
        for (u32 i = 0; i < numSectors; ++i) {
            std::memcpy(output.data() + writePos, sectorResults[i].data(), sectorResults[i].size());
            writePos += sectorResults[i].size();
        }
        return output;
    }

    // --- Serial sector extraction ---
    std::vector<u8> output;
    output.reserve(block.uncompressedSize);

    for (u32 i = 0; i < numSectors; ++i) {
        auto [data, err] =
            decodeSector(i, sectorOffsets[i], sectorOffsets[i + 1], expectedSectorSize(i));
        if (!err.empty()) {
            setErr(std::move(err));
            return {};
        }
        output.insert(output.end(), data.begin(), data.end());
    }

    return output;
}

// ============================================================================
// Encoding (Write)
// ============================================================================

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
            auto compressed = mpqCompress(rawData, opts.compression);
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
            size_t alignedCount = encoded.size() / 4;
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
    u32 numSectors = (static_cast<u32>(rawData.size()) + sectorSize - 1) / sectorSize;

    std::vector<std::vector<u8>> sectorData(numSectors);
    bool anyCompressed = false;

    // --- Sector compress: compress a single raw sector ---
    auto compressSector = [&](u32 i) -> bool {
        size_t srcStart = static_cast<size_t>(i) * sectorSize;
        size_t srcLen = std::min<size_t>(sectorSize, rawData.size() - srcStart);
        auto sectorRaw = rawData.subspan(srcStart, srcLen);

        if (opts.compression != CompressionFlag::None) {
            auto compressed = mpqCompress(sectorRaw, opts.compression);
            if (!compressed.empty()) {
                sectorData[i] = std::move(compressed);
                return true; // was compressed
            }
        }
        sectorData[i].assign(sectorRaw.begin(), sectorRaw.end());
        return false; // stored uncompressed
    };

    if (pool && numSectors >= 2) {
        std::vector<bool> sectorCompressed(numSectors, false);

        for (u32 i = 0; i < numSectors; ++i) {
            interfaces::WorkerTask task;
            task.fn = [i, &compressSector, &sectorCompressed]() {
                sectorCompressed[i] = compressSector(i);
            };
            pool->submit(task);
        }
        pool->waitIdle();

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
            size_t alignedCount = sectorData[i].size() / 4;
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

} // namespace whiteout::storages::mpq
