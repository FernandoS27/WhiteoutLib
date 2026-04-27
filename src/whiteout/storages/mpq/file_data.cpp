// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "codecs/compression.h"
#include "crypto.h"
#include "file_data.h"

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
    if (pool && pool->threadCount() > 0 && numSectors >= 4) {
        std::vector<std::vector<u8>> sectorResults(numSectors);
        std::vector<std::string> sectorErrors(numSectors);
        std::atomic<bool> failed{false};
        utils::JobGroup jobGroup;
        jobGroup.add(numSectors);

        for (u32 i = 0; i < numSectors; ++i) {
            interfaces::WorkerTask task;
            task.fn = [i, &sectorOffsets, &expectedSectorSize, &decodeSector, &sectorResults,
                       &sectorErrors, &failed, &jobGroup]() {
                try {
                    if (!failed.load(std::memory_order_acquire)) {
                        auto [data, err] = decodeSector(i, sectorOffsets[i], sectorOffsets[i + 1],
                                                        expectedSectorSize(i));
                        if (!err.empty()) {
                            failed.store(true, std::memory_order_relaxed);
                            sectorErrors[i] = std::move(err);
                        } else {
                            sectorResults[i] = std::move(data);
                        }
                    }
                } catch (...) {
                    failed.store(true, std::memory_order_relaxed);
                    sectorErrors[i] = "sector " + std::to_string(i) + " threw exception";
                }
                jobGroup.done();
            };
            pool->submit(task);
        }
        jobGroup.wait();

        if (failed.load(std::memory_order_relaxed)) {
            for (u32 i = 0; i < numSectors; ++i) {
                if (!sectorErrors[i].empty()) {
                    setErr(std::move(sectorErrors[i]));
                    break;
                }
            }
            return {};
        }

        // Compute actual total decompressed size (may differ from block.uncompressedSize
        // for some archives where the block table stores a padded/approximate value).
        size_t totalSize = 0;
        for (u32 i = 0; i < numSectors; ++i)
            totalSize += sectorResults[i].size();

        std::vector<u8> output(totalSize);
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

    if (pool && pool->threadCount() > 0 && numSectors >= 2) {
        std::vector<bool> sectorCompressed(numSectors, false);
        std::atomic<bool> encodeFailed{false};
        utils::JobGroup jobGroup;
        jobGroup.add(numSectors);

        for (u32 i = 0; i < numSectors; ++i) {
            interfaces::WorkerTask task;
            task.fn = [i, &compressSector, &sectorCompressed, &encodeFailed, &jobGroup]() {
                try {
                    if (!encodeFailed.load(std::memory_order_acquire))
                        sectorCompressed[i] = compressSector(i);
                } catch (...) {
                    encodeFailed.store(true, std::memory_order_release);
                }
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
                    try {
                        if (!failed.load(std::memory_order_acquire))
                            result.files[i] =
                                encodeFileData(items[i].first, items[i].second, nullptr);
                    } catch (...) {
                        failed.store(true, std::memory_order_release);
                    }
                    jobGroup.done();
                };
                pool->submit(task);
            }
            jobGroup.wait();
        } else {
            for (size_t i = 0; i < items.size(); ++i) {
                result.files[i] = encodeFileData(items[i].first, items[i].second,
                                                 usePool ? pool : nullptr);
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
                try {
                    if (!failed.load(std::memory_order_acquire))
                        result.files[i] =
                            encodeFileData(items[i].first, items[i].second, nullptr);
                } catch (...) {
                    failed.store(true, std::memory_order_release);
                }
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
            size_t srcStart = static_cast<size_t>(j) * opts.sectorSize;
            size_t srcLen =
                std::min<size_t>(opts.sectorSize, rawData.size() - srcStart);

            interfaces::WorkerTask task;
            task.fn = [j, srcStart, srcLen, i, &items, &states, &failed, compressGroup]() {
                try {
                    if (!failed.load(std::memory_order_acquire)) {
                        const auto& [rawData, opts] = items[i];
                        auto sectorRaw = rawData.subspan(srcStart, srcLen);
                        auto& sr = states[i].sectorResults[j];

                        if (opts.compression != CompressionFlag::None) {
                            auto compressed = mpqCompress(sectorRaw, opts.compression);
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
                } catch (...) {
                    failed.store(true, std::memory_order_release);
                }
                compressGroup->done();
            };
            pool->submit(task);
        }

        // Phase 2: assembly task (waits for all sectors of this file).
        state.completeDone = state.sem->next();
        interfaces::WorkerTask assembleTask;
        assembleTask.fn = [i, &items, &result, &states, &failed, numSectors]() {
            try {
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
                if (anyCompressed)
                    flags |= FileFlag::kCompress;

                // Build sector offset table.
                std::vector<u32> sectorOffsets;
                sectorOffsets.reserve(numSectors + 1);
                u32 currentOffset = (numSectors + 1) * sizeof(u32);
                sectorOffsets.push_back(currentOffset);
                for (u32 j = 0; j < numSectors; ++j) {
                    currentOffset +=
                        static_cast<u32>(state.sectorResults[j].data.size());
                    sectorOffsets.push_back(currentOffset);
                }

                // Derive encryption key if needed.
                u32 fileKey = 0;
                if (opts.encrypt && !opts.filename.empty()) {
                    flags |= FileFlag::kEncrypted;
                    fileKey = deriveFileKey(
                        opts.filename,
                        BlockEntry{0, currentOffset,
                                   static_cast<u32>(rawData.size()), flags});
                }

                // Encrypt sector offsets.
                auto encOffsets = sectorOffsets;
                if (fileKey != 0)
                    encryptBlock(encOffsets.data(), encOffsets.size(), fileKey - 1);

                // Encrypt each sector.
                if (fileKey != 0) {
                    for (u32 j = 0; j < numSectors; ++j) {
                        size_t aligned = state.sectorResults[j].data.size() / 4;
                        if (aligned > 0)
                            encryptBlock(
                                reinterpret_cast<u32*>(
                                    state.sectorResults[j].data.data()),
                                aligned, fileKey + j);
                    }
                }

                // Assemble final data.
                EncodedFile& ef = result.files[i];
                ef.data.resize(currentOffset);
                std::memcpy(ef.data.data(), encOffsets.data(),
                            encOffsets.size() * sizeof(u32));
                size_t writePos = encOffsets.size() * sizeof(u32);
                for (u32 j = 0; j < numSectors; ++j) {
                    std::memcpy(ef.data.data() + writePos,
                                state.sectorResults[j].data.data(),
                                state.sectorResults[j].data.size());
                    writePos += state.sectorResults[j].data.size();
                }
                ef.compressedSize = currentOffset;
                ef.flags = flags;
            } catch (...) {
                failed.store(true, std::memory_order_release);
            }
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

std::vector<std::optional<std::vector<u8>>> extractBatch(
    std::span<const u8> archiveData, size_t archiveOffset,
    std::span<const ExtractFileInfo> files, u32 sectorSize,
    interfaces::WorkerPool* pool) {

    std::vector<std::optional<std::vector<u8>>> results(files.size());

    if (files.empty())
        return results;

    const bool usePool = pool && pool->threadCount() > 0;

    // Check semaphore support.
    const bool useSemaphores = usePool && [&]() {
        auto test = pool->createTimelineSemaphore();
        return test != nullptr;
    }();

    if (!useSemaphores) {
        // Fallback: serial per-file extraction with sector-level parallelism.
        for (size_t i = 0; i < files.size(); ++i) {
            std::string err;
            auto data = extractFileData(archiveData, archiveOffset, files[i].block, sectorSize,
                                        files[i].fileKey, &err, usePool ? pool : nullptr);
            if (err.empty() && !data.empty())
                results[i] = std::move(data);
            else if (files[i].block.uncompressedSize == 0)
                results[i] = std::vector<u8>{};
        }
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

        u64 dataStart = archiveOffset + block.fileOffset;
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
                try {
                    const auto& block = files[i].block;
                    u32 fileKey = files[i].fileKey;
                    std::vector<u8> buf(fileSpan.begin(), fileSpan.end());

                    if (block.isEncrypted() && fileKey != 0) {
                        size_t alignedCount = buf.size() / 4;
                        if (alignedCount > 0)
                            decryptBlock(reinterpret_cast<u32*>(buf.data()), alignedCount, fileKey);
                    }

                    if (block.isCompressed() &&
                        block.compressedSize < block.uncompressedSize) {
                        auto decompressed =
                            mpqDecompress(std::span<const u8>(buf), block.uncompressedSize);
                        if (!decompressed.empty()) {
                            results[i] = std::move(decompressed);
                        }
                    } else {
                        buf.resize(block.uncompressedSize);
                        results[i] = std::move(buf);
                    }
                } catch (...) {
                    // Leave results[i] as nullopt.
                }
            };
            task.signalSemaphore = state.sem.get();
            task.signalValue = state.completeDone;
            pool->submit(task);
            continue;
        }

        // Multi-sector file: validate + decrypt sector offset table (serial).
        u32 numSectors = (block.uncompressedSize + sectorSize - 1) / sectorSize;
        bool hasCrc = block.hasSectorCrc();
        u32 numOffsetEntries = numSectors + 1 + (hasCrc ? 1u : 0u);
        u32 offsetTableSize = numOffsetEntries * sizeof(u32);

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
                try {
                    if (states[i].fileFailed.load(std::memory_order_acquire)) {
                        decodeGroup->done();
                        return;
                    }

                    const auto& block = files[i].block;
                    u32 fileKey = files[i].fileKey;
                    u32 sectorStart = (*sectorOffsets)[j];
                    u32 sectorEnd = (*sectorOffsets)[j + 1];
                    u32 expectedSize = (j < numSectors - 1)
                                           ? sectorSize
                                           : (block.uncompressedSize - j * sectorSize);

                    std::vector<u8> sectorBuf(fileSpan.data() + sectorStart,
                                              fileSpan.data() + sectorEnd);
                    u32 sectorLen = sectorEnd - sectorStart;

                    if (block.isEncrypted() && fileKey != 0) {
                        size_t alignedCount = sectorBuf.size() / 4;
                        if (alignedCount > 0)
                            decryptBlock(reinterpret_cast<u32*>(sectorBuf.data()), alignedCount,
                                         fileKey + j);
                    }

                    if (block.isCompressed() && sectorLen < expectedSize) {
                        auto decompressed =
                            mpqDecompress(std::span<const u8>(sectorBuf), expectedSize);
                        if (decompressed.empty()) {
                            states[i].fileFailed.store(true, std::memory_order_release);
                        } else {
                            states[i].sectorResults[j] = std::move(decompressed);
                        }
                    } else {
                        sectorBuf.resize(expectedSize);
                        states[i].sectorResults[j] = std::move(sectorBuf);
                    }
                } catch (...) {
                    states[i].fileFailed.store(true, std::memory_order_release);
                }
                decodeGroup->done();
            };
            pool->submit(task);
        }

        // Phase 2: assembly task — concatenate sector results.
        state.completeDone = state.sem->next();
        interfaces::WorkerTask assembleTask;
        assembleTask.fn = [i, &files, &results, &states, numSectors]() {
            try {
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
            } catch (...) {
                // Leave results[i] as nullopt.
            }
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
