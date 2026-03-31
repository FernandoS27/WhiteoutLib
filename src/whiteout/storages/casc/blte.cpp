// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "blte.h"
#include "crypto.h"
#include "../common/byte_order.h"
#include "../common/md5.h"
#include "../common/zlib.h"

#include <whiteout/interfaces.h>
#include <whiteout/utils/job_group.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>

namespace whiteout::storages::casc {

using storages::common::readBE32;
using storages::common::writeBE32;

// ---- Constants local to BLTE codec ----

/// BLTE container magic: 'BLTE' as big-endian u32.
static constexpr u32 kBlteMagic = 0x424C5445;

/// Default BLTE frame size (64 KB).
static constexpr u32 kDefaultBlteFrameSize = 0x10000;

/// Minimum BLTE header size: magic(4) + headerSize(4).
static constexpr size_t kBlteMinHeaderSize = 8;

/// Size of one entry in the BLTE frame table:
/// compressedSize(4) + uncompressedSize(4) + MD5(16) = 24 bytes.
static constexpr size_t kBlteFrameTableEntrySize = 24;

/// Flags byte written at the start of a multi-frame BLTE header.
static constexpr u8 kBlteMultiFrameFlags = 0x0F;

/// BLTE frame encoding mode bytes.
namespace BlteFrameMode {
    static constexpr u8 kRaw       = 'N'; ///< Uncompressed.
    static constexpr u8 kZlib      = 'Z'; ///< zlib-compressed.
    static constexpr u8 kEncrypted = 'E'; ///< Encrypted (Salsa20).
    static constexpr u8 kRecursive = 'F'; ///< Recursive BLTE container.
} // namespace BlteFrameMode

// ============================================================================
// Frame Decode (single frame payload)
// ============================================================================

struct FrameDecodeResult {
    std::vector<u8> data;
    bool success = false;
    std::string error;
};

static FrameDecodeResult decodeFramePayload(std::span<const u8> payload,
                                            u32 expectedUncompressedSize,
                                            const KeyRing* keys) {
    FrameDecodeResult result;

    if (payload.empty()) {
        result.error = "empty frame payload";
        return result;
    }

    u8 mode = payload[0];
    auto inner = payload.subspan(1);

    switch (mode) {
    case BlteFrameMode::kRaw: {
        result.data.assign(inner.begin(), inner.end());
        result.success = true;
        break;
    }
    case BlteFrameMode::kZlib: {
        result.data = storages::common::zlibDecompress(inner, expectedUncompressedSize);
        if (result.data.empty() && expectedUncompressedSize > 0) {
            result.error = "zlib decompression failed";
        } else {
            result.success = true;
        }
        break;
    }
    case BlteFrameMode::kEncrypted: {
        // Layout: keyNameLE(8) + IVSize(1) + IV(IVSize) + encrypted payload
        if (inner.size() < 9) {
            result.error = "encrypted frame too small for key header";
            return result;
        }

        u64 keyName = 0;
        std::memcpy(&keyName, inner.data(), 8);
        u8 ivSize = inner[8];

        if (inner.size() < 9u + ivSize) {
            result.error = "encrypted frame truncated at IV";
            return result;
        }

        if (!keys) {
            result.error = "encrypted frame but no KeyRing provided";
            return result;
        }

        const auto* key = keys->findKey(keyName);
        if (!key) {
            result.error = "missing encryption key 0x" +
                           std::to_string(keyName); // simplified
            return result;
        }

        // Copy encrypted payload to mutable buffer
        auto encStart = inner.subspan(9 + ivSize);
        std::vector<u8> decrypted(encStart.begin(), encStart.end());

        // Decrypt with Salsa20
        if (ivSize >= 8) {
            std::array<u8, 8> iv{};
            std::memcpy(iv.data(), inner.data() + 9, 8);
            salsa20Decrypt(decrypted, *key,
                           std::span<const u8, 8>(iv));
        }

        // Recursively dispatch on decrypted inner mode byte
        auto innerResult = decodeFramePayload(decrypted, expectedUncompressedSize, keys);
        result = std::move(innerResult);
        break;
    }
    case BlteFrameMode::kRecursive: {
        auto innerResult = blteDecode(inner, keys, nullptr);
        result.data = std::move(innerResult.data);
        result.success = innerResult.success;
        result.error = std::move(innerResult.error);
        break;
    }
    default:
        result.error = "unknown BLTE encoding mode: 0x";
        result.error += "0123456789ABCDEF"[(mode >> 4) & 0xF];
        result.error += "0123456789ABCDEF"[mode & 0xF];
        break;
    }

    return result;
}

// ============================================================================
// BLTE Decode
// ============================================================================

BlteDecodeResult blteDecode(std::span<const u8> blteData,
                            const KeyRing* keys,
                            interfaces::WorkerPool* pool) {
    BlteDecodeResult result;

    if (blteData.size() < kBlteMinHeaderSize) {
        result.error = "data too small for BLTE header";
        return result;
    }

    // Verify magic
    u32 magic = readBE32(blteData.data());
    if (magic != kBlteMagic) {
        result.error = "invalid BLTE magic";
        return result;
    }

    u32 headerSize = readBE32(blteData.data() + 4);

    struct FrameInfo {
        u32 compressedSize;
        u32 uncompressedSize;
        // MD5 hash (16 bytes) — not checked in decode, just skipped.
    };

    std::vector<FrameInfo> frames;
    size_t dataOffset = 0;

    if (headerSize == 0) {
        // Single-frame: entire remainder after 8-byte header is one frame.
        FrameInfo fi;
        fi.compressedSize = u32(blteData.size() - kBlteMinHeaderSize);
        fi.uncompressedSize = 0; // Unknown
        frames.push_back(fi);
        dataOffset = kBlteMinHeaderSize;
    } else {
        // Multi-frame header.
        // Header starts at offset 8:
        //   flags (u8) + frameCount (u24 BE) = 4 bytes
        //   then frameCount * kBlteFrameTableEntrySize bytes each
        size_t hdrStart = kBlteMinHeaderSize;
        if (blteData.size() < hdrStart + 4) {
            result.error = "truncated BLTE frame table header";
            return result;
        }

        // Flags (1 byte) + frame count (3 bytes, big-endian)
        // u8 flags = blteData[hdrStart]; // unused for now
        u32 frameCount = (u32(blteData[hdrStart + 1]) << 16) |
                         (u32(blteData[hdrStart + 2]) << 8) |
                         u32(blteData[hdrStart + 3]);

        size_t tableStart = hdrStart + 4;
        size_t tableSize = frameCount * kBlteFrameTableEntrySize;

        if (blteData.size() < tableStart + tableSize) {
            result.error = "truncated BLTE frame table";
            return result;
        }

        frames.reserve(frameCount);
        for (u32 i = 0; i < frameCount; ++i) {
            const u8* entry = blteData.data() + tableStart + i * kBlteFrameTableEntrySize;
            FrameInfo fi;
            fi.compressedSize = readBE32(entry);
            fi.uncompressedSize = readBE32(entry + 4);
            // entry+8 .. entry+23 = MD5 hash, skip.
            frames.push_back(fi);
        }

        dataOffset = tableStart + tableSize;
    }

    // Pre-compute frame data offsets (shared by both paths).
    std::vector<size_t> offsets(frames.size());
    {
        size_t off = dataOffset;
        for (size_t i = 0; i < frames.size(); ++i) {
            offsets[i] = off;
            off += frames[i].compressedSize;
        }
        if (off > blteData.size()) {
            result.error = "frame data extends past end of BLTE blob";
            return result;
        }
    }

    // Decode frames
    if (pool && frames.size() > 1) {
        // Parallel decode
        utils::JobGroup jobGroup;
        std::vector<FrameDecodeResult> frameResults(frames.size());
        std::mutex errMutex;
        std::string firstError;

        jobGroup.add(frames.size());
        for (size_t i = 0; i < frames.size(); ++i) {
            interfaces::WorkerTask task;
            task.fn = [&, i]() {
                auto payload = blteData.subspan(offsets[i], frames[i].compressedSize);
                frameResults[i] = decodeFramePayload(payload, frames[i].uncompressedSize, keys);
                if (!frameResults[i].success) {
                    std::lock_guard<std::mutex> lock(errMutex);
                    if (firstError.empty())
                        firstError = frameResults[i].error;
                }
                jobGroup.done();
            };
            pool->submit(task);
        }
        jobGroup.wait();

        if (!firstError.empty()) {
            result.error = std::move(firstError);
            return result;
        }

        // Concatenate
        size_t totalSize = 0;
        for (auto& fr : frameResults)
            totalSize += fr.data.size();
        result.data.reserve(totalSize);
        for (auto& fr : frameResults) {
            result.data.insert(result.data.end(),
                               std::make_move_iterator(fr.data.begin()),
                               std::make_move_iterator(fr.data.end()));
            fr.data.clear();
            fr.data.shrink_to_fit();
        }

    } else {
        // Sequential decode
        for (size_t i = 0; i < frames.size(); ++i) {
            auto payload = blteData.subspan(offsets[i], frames[i].compressedSize);
            auto fr = decodeFramePayload(payload, frames[i].uncompressedSize, keys);
            if (!fr.success) {
                result.error = "frame " + std::to_string(i) + ": " + fr.error;
                return result;
            }
            result.data.insert(result.data.end(), fr.data.begin(), fr.data.end());
        }
    }

    result.success = true;
    return result;
}

// ============================================================================
// BLTE Batch Decode (DAG pipeline)
// ============================================================================

std::vector<BlteBatchResult> blteDecodeBatch(
    std::span<const BlteBatchEntry> entries,
    const KeyRing* keys,
    interfaces::WorkerPool* pool) {

    std::vector<BlteBatchResult> results(entries.size());

    if (entries.empty())
        return results;

    // Serial fallback: no pool or pool has no threads.
    if (!pool || pool->threadCount() == 0) {
        for (size_t i = 0; i < entries.size(); ++i) {
            auto decoded = blteDecode(entries[i].blteData, keys, nullptr);
            results[i].data = std::move(decoded.data);
            results[i].success = decoded.success;
            results[i].error = std::move(decoded.error);
        }
        return results;
    }

    // Check semaphore support.
    auto testSem = pool->createTimelineSemaphore();
    if (!testSem) {
        // Fallback: per-file blteDecode with intra-file frame parallelism.
        for (size_t i = 0; i < entries.size(); ++i) {
            auto decoded = blteDecode(entries[i].blteData, keys, pool);
            results[i].data = std::move(decoded.data);
            results[i].success = decoded.success;
            results[i].error = std::move(decoded.error);
        }
        return results;
    }
    testSem.reset(); // Release test semaphore.

    // ---- Full DAG path: per-file timeline semaphore ----

    // Pre-parse all BLTE headers to determine frame layouts.
    struct FileFrameInfo {
        struct Frame {
            u32 compressedSize;
            u32 uncompressedSize;
        };
        std::vector<Frame> frames;
        std::vector<size_t> offsets; // byte offset of each frame's data
        bool headerValid = false;
        std::string headerError;
    };

    std::vector<FileFrameInfo> fileInfos(entries.size());
    // Per-file semaphores (only for multi-frame files).
    std::vector<std::unique_ptr<interfaces::TimelineSemaphore>> sems(entries.size());
    // Per-file frame results (only for multi-frame files).
    std::vector<std::vector<FrameDecodeResult>> perFileFrameResults(entries.size());
    // Per-file error flag.
    std::vector<std::atomic<bool>> fileFailed(entries.size());
    for (auto& f : fileFailed) f.store(false, std::memory_order_relaxed);
    // Per-file job groups (only for multi-frame files).
    std::vector<std::shared_ptr<utils::JobGroup>> decodeGroups(entries.size());

    // Phase 0: Parse headers and dispatch.
    for (size_t i = 0; i < entries.size(); ++i) {
        auto& blob = entries[i].blteData;
        auto& fi = fileInfos[i];

        if (blob.size() < kBlteMinHeaderSize) {
            results[i].error = "data too small for BLTE header";
            continue;
        }

        u32 magic = readBE32(blob.data());
        if (magic != kBlteMagic) {
            results[i].error = "invalid BLTE magic";
            continue;
        }

        u32 headerSize = readBE32(blob.data() + 4);

        if (headerSize == 0) {
            // Single-frame: decode inline immediately.
            FileFrameInfo::Frame f;
            f.compressedSize = u32(blob.size() - kBlteMinHeaderSize);
            f.uncompressedSize = 0;
            fi.frames.push_back(f);
            fi.offsets.push_back(kBlteMinHeaderSize);
            fi.headerValid = true;

            // Decode inline — no DAG overhead for single frames.
            auto payload = blob.subspan(kBlteMinHeaderSize, f.compressedSize);
            auto fr = decodeFramePayload(payload, f.uncompressedSize, keys);
            results[i].data = std::move(fr.data);
            results[i].success = fr.success;
            results[i].error = std::move(fr.error);
            continue;
        }

        // Multi-frame header.
        size_t hdrStart = kBlteMinHeaderSize;
        if (blob.size() < hdrStart + 4) {
            results[i].error = "truncated BLTE frame table header";
            continue;
        }

        u32 frameCount = (u32(blob[hdrStart + 1]) << 16) |
                         (u32(blob[hdrStart + 2]) << 8) |
                         u32(blob[hdrStart + 3]);

        size_t tableStart = hdrStart + 4;
        size_t tableSize = frameCount * kBlteFrameTableEntrySize;

        if (blob.size() < tableStart + tableSize) {
            results[i].error = "truncated BLTE frame table";
            continue;
        }

        fi.frames.reserve(frameCount);
        fi.offsets.reserve(frameCount);
        size_t off = tableStart + tableSize;
        for (u32 j = 0; j < frameCount; ++j) {
            const u8* entry = blob.data() + tableStart + j * kBlteFrameTableEntrySize;
            FileFrameInfo::Frame f;
            f.compressedSize = readBE32(entry);
            f.uncompressedSize = readBE32(entry + 4);
            fi.frames.push_back(f);
            fi.offsets.push_back(off);
            off += f.compressedSize;
        }

        if (off > blob.size()) {
            results[i].error = "frame data extends past end of BLTE blob";
            continue;
        }

        fi.headerValid = true;

        if (frameCount <= 1) {
            // Single frame — decode inline.
            auto payload = blob.subspan(fi.offsets[0], fi.frames[0].compressedSize);
            auto fr = decodeFramePayload(payload, fi.frames[0].uncompressedSize, keys);
            results[i].data = std::move(fr.data);
            results[i].success = fr.success;
            results[i].error = std::move(fr.error);
            continue;
        }

        // Multi-frame: set up DAG.
        sems[i] = pool->createTimelineSemaphore();
        perFileFrameResults[i].resize(frameCount);
        decodeGroups[i] = std::make_shared<utils::JobGroup>();
        decodeGroups[i]->add(frameCount);

        auto framesDone = sems[i]->next();
        auto assemblyDone = sems[i]->next();

        decodeGroups[i]->signalOnComplete(sems[i].get(), framesDone);

        // Submit frame decode tasks.
        for (u32 j = 0; j < frameCount; ++j) {
            interfaces::WorkerTask task;
            task.fn = [&entries, &perFileFrameResults, &fileFailed, &fileInfos,
                       &decodeGroups, keys, i, j]() {
                if (fileFailed[i].load(std::memory_order_relaxed)) {
                    decodeGroups[i]->done();
                    return;
                }
                auto& blob = entries[i].blteData;
                auto& fi = fileInfos[i];
                auto payload = blob.subspan(fi.offsets[j], fi.frames[j].compressedSize);
                perFileFrameResults[i][j] = decodeFramePayload(
                    payload, fi.frames[j].uncompressedSize, keys);
                if (!perFileFrameResults[i][j].success)
                    fileFailed[i].store(true, std::memory_order_relaxed);
                decodeGroups[i]->done();
            };
            // No wait — raw data already available in blteData span.
            pool->submit(task);
        }

        // Submit assembly task — waits for all frames.
        interfaces::WorkerTask assemblyTask;
        assemblyTask.fn = [&results, &perFileFrameResults, &fileFailed, i]() {
            if (fileFailed[i].load(std::memory_order_relaxed)) {
                // Find first error.
                for (auto& fr : perFileFrameResults[i]) {
                    if (!fr.error.empty()) {
                        results[i].error = std::move(fr.error);
                        break;
                    }
                }
                if (results[i].error.empty())
                    results[i].error = "frame decode failed";
                return;
            }

            // Concatenate frame results.
            size_t totalSize = 0;
            for (auto& fr : perFileFrameResults[i])
                totalSize += fr.data.size();
            results[i].data.reserve(totalSize);
            for (auto& fr : perFileFrameResults[i]) {
                results[i].data.insert(results[i].data.end(),
                                       std::make_move_iterator(fr.data.begin()),
                                       std::make_move_iterator(fr.data.end()));
                fr.data.clear();
                fr.data.shrink_to_fit();
            }
            results[i].success = true;
        };
        assemblyTask.waitSemaphore = sems[i].get();
        assemblyTask.waitValue = framesDone;
        assemblyTask.signalSemaphore = sems[i].get();
        assemblyTask.signalValue = assemblyDone;
        pool->submit(assemblyTask);
    }

    // Join: wait for all multi-frame files.
    for (size_t i = 0; i < entries.size(); ++i) {
        if (sems[i]) {
            // assemblyDone is the second value allocated (next() was called twice).
            // Value 1 = framesDone, value 2 = assemblyDone.
            sems[i]->wait(2);
        }
    }

    return results;
}

// ============================================================================
// BLTE Encode
// ============================================================================

std::vector<u8> blteEncode(std::span<const u8> rawData,
                           const BlteEncodeOptions& opts,
                           interfaces::WorkerPool* pool) {
    u32 frameSize = opts.frameSize;
    if (frameSize == 0)
        frameSize = kDefaultBlteFrameSize;

    // Split into frames
    size_t frameCount = rawData.empty() ? 1 : (rawData.size() + frameSize - 1) / frameSize;

    struct EncodedFrame {
        std::vector<u8> data; // mode byte + compressed/raw payload
        u32 uncompressedSize;
        std::array<u8, 16> hash;
    };

    std::vector<EncodedFrame> encodedFrames(frameCount);

    auto encodeOneFrame = [&](size_t i) {
        size_t start = i * frameSize;
        size_t end = std::min(start + (size_t)frameSize, rawData.size());
        auto chunk = rawData.subspan(start, end - start);

        EncodedFrame& ef = encodedFrames[i];
        ef.uncompressedSize = u32(chunk.size());

        if (opts.compress && !chunk.empty()) {
            auto compressed = storages::common::zlibCompress(chunk);
            // Use compressed only if actually smaller (plus 1 for mode byte)
            if (!compressed.empty() && compressed.size() + 1 < chunk.size() + 1) {
                ef.data.resize(1 + compressed.size());
                ef.data[0] = BlteFrameMode::kZlib;
                std::memcpy(ef.data.data() + 1, compressed.data(), compressed.size());
            } else {
                // Compression didn't help — store raw
                ef.data.resize(1 + chunk.size());
                ef.data[0] = BlteFrameMode::kRaw;
                std::memcpy(ef.data.data() + 1, chunk.data(), chunk.size());
            }
        } else {
            ef.data.resize(1 + chunk.size());
            ef.data[0] = BlteFrameMode::kRaw;
            if (!chunk.empty())
                std::memcpy(ef.data.data() + 1, chunk.data(), chunk.size());
        }

        ef.hash = storages::common::md5Hash(ef.data);
    };

    if (rawData.empty()) {
        // Single empty frame
        encodedFrames[0].uncompressedSize = 0;
        encodedFrames[0].data = {BlteFrameMode::kRaw}; // mode byte, no payload
        encodedFrames[0].hash = storages::common::md5Hash(encodedFrames[0].data);
    } else if (pool && frameCount > 1) {
        // Parallel encode
        utils::JobGroup jobGroup;
        jobGroup.add(frameCount);
        for (size_t i = 0; i < frameCount; ++i) {
            interfaces::WorkerTask task;
            task.fn = [&, i]() {
                encodeOneFrame(i);
                jobGroup.done();
            };
            pool->submit(task);
        }
        jobGroup.wait();
    } else {
        // Sequential encode
        for (size_t i = 0; i < frameCount; ++i)
            encodeOneFrame(i);
    }

    // Build output
    std::vector<u8> output;

    if (frameCount <= 1) {
        // Single-frame format: magic(4) + headerSize=0(4) + frame data
        output.resize(kBlteMinHeaderSize + encodedFrames[0].data.size());
        writeBE32(output.data(), kBlteMagic);
        writeBE32(output.data() + 4, 0); // headerSize = 0
        std::memcpy(output.data() + kBlteMinHeaderSize, encodedFrames[0].data.data(),
                     encodedFrames[0].data.size());
    } else {
        // Multi-frame format:
        //   magic(4) + headerSize(4) +
        //   flags(1) + frameCount(3) +
        //   frameCount * kBlteFrameTableEntrySize
        //   + frame data
        u32 tableSize = u32(frameCount) * kBlteFrameTableEntrySize;
        u32 headerSize = 4 + tableSize; // flags+count(4) + table

        size_t totalFrameData = 0;
        for (auto& ef : encodedFrames)
            totalFrameData += ef.data.size();

        output.resize(kBlteMinHeaderSize + headerSize + totalFrameData);

        // Magic + header size
        writeBE32(output.data(), kBlteMagic);
        writeBE32(output.data() + 4, headerSize);

        // Flags + frame count (24-bit BE)
        u8* hdr = output.data() + kBlteMinHeaderSize;
        hdr[0] = kBlteMultiFrameFlags;
        hdr[1] = u8((frameCount >> 16) & 0xFF);
        hdr[2] = u8((frameCount >> 8) & 0xFF);
        hdr[3] = u8(frameCount & 0xFF);

        // Frame table
        u8* table = hdr + 4;
        for (size_t i = 0; i < frameCount; ++i) {
            u8* entry = table + i * kBlteFrameTableEntrySize;
            writeBE32(entry, u32(encodedFrames[i].data.size()));
            writeBE32(entry + 4, encodedFrames[i].uncompressedSize);
            std::memcpy(entry + 8, encodedFrames[i].hash.data(), 16);
        }

        // Frame data
        u8* dest = output.data() + kBlteMinHeaderSize + headerSize;
        for (auto& ef : encodedFrames) {
            std::memcpy(dest, ef.data.data(), ef.data.size());
            dest += ef.data.size();
        }
    }

    return output;
}

} // namespace whiteout::storages::casc
