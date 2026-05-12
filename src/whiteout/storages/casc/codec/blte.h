// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
/// @file blte.h
/// @brief BLTE container decode/encode.
///
/// Internal header — not part of the public include path.
#pragma once

#include <whiteout/common_types.h>
#include <whiteout/interfaces.h>

#include <span>
#include <string>
#include <vector>

namespace whiteout::storages::casc {

class KeyRing; // forward declaration

// ============================================================================
// BLTE Decode
// ============================================================================

struct BlteDecodeResult {
    std::vector<u8> data;   ///< Uncompressed file content.
    bool success = false;
    std::string error;
};

/// Decode a BLTE container.
/// @param blteData Raw BLTE-encoded data (starts with 'BLTE' magic).
/// @param keys     Optional key ring for decrypting 'E' frames.
/// @param pool     Optional worker pool for parallel frame decoding.
BlteDecodeResult blteDecode(std::span<const u8> blteData,
                            const KeyRing* keys = nullptr,
                            interfaces::WorkerPool* pool = nullptr);

// ============================================================================
// BLTE Frame-Level Access
// ============================================================================

/// Frame metadata extracted from the BLTE header without decoding bodies.
struct BlteFrameLayout {
    struct Frame {
        u32 compressedSize;
        u32 uncompressedSize; ///< 0 for single-frame BLTEs (headerSize==0).
    };
    std::vector<Frame> frames;
    std::vector<size_t> offsets;
    bool valid = false;
    std::string error;
};

BlteFrameLayout blteParseFrameLayout(std::span<const u8> blteData);

struct BlteBatchResult;
BlteBatchResult blteDecodeFrame(std::span<const u8> blteData,
                                const BlteFrameLayout& layout,
                                size_t frameIdx,
                                const KeyRing* keys = nullptr);

// ============================================================================
// BLTE Batch Decode
// ============================================================================

/// Per-file decode request for the DAG batch pipeline.
struct BlteBatchEntry {
    std::span<const u8> blteData; ///< Raw BLTE blob for one file.
};

/// Per-file decode result.
struct BlteBatchResult {
    std::vector<u8> data;
    bool success = false;
    std::string error;
};

/// Decode multiple BLTE blobs using DAG scheduling.
///
/// Each multi-frame file gets its own timeline semaphore with two phases:
///   Phase 1 (framesDone): parallel frame decode, signalled via
///     JobGroup::signalOnComplete.
///   Phase 2 (assemblyDone): assembly task concatenates frames,
///     waits on framesDone, signals assemblyDone.
///
/// Falls back to serial per-file decode when pool lacks timeline
/// semaphore support.
///
/// @param entries   Per-file BLTE blobs.
/// @param keys      Encryption key ring (shared by all entries; may be nullptr).
/// @param pool      Worker pool (may be nullptr → serial).
/// @return Per-file results in the same order as entries.
std::vector<BlteBatchResult> blteDecodeBatch(
    std::span<const BlteBatchEntry> entries,
    const KeyRing* keys = nullptr,
    interfaces::WorkerPool* pool = nullptr);

// ============================================================================
// BLTE Encode
// ============================================================================

struct BlteEncodeOptions {
    u32 frameSize = 0x10000; ///< Frame size in bytes (default 64 KB).
    bool compress = true;    ///< If true, use zlib compression; otherwise raw.
};

/// Encode data into a BLTE container.
/// @param rawData  Uncompressed file content.
/// @param opts     Encoding options.
/// @param pool     Optional worker pool for parallel frame encoding.
/// @return The BLTE-encoded blob.
std::vector<u8> blteEncode(std::span<const u8> rawData,
                           const BlteEncodeOptions& opts = {},
                           interfaces::WorkerPool* pool = nullptr);

} // namespace whiteout::storages::casc
