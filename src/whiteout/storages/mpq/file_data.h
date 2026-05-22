// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file file_data.h
/// @brief MPQ file data extraction and encoding (sector-based read/write).

#pragma once

#include <whiteout/common_types.h>

#include "codecs/compression.h"
#include "tables/block_table.h"

#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace whiteout::interfaces {
class WorkerPool;
}

namespace whiteout::storages::mpq {

// ============================================================================
// File Key Derivation
// ============================================================================

/// Derive the encryption key for a file given its name and block entry.
/// If the file has the FIX_KEY flag, the key is adjusted by the block offset.
[[nodiscard]] u32 deriveFileKey(const std::string& filename, const BlockEntry& block);

// ============================================================================
// Extraction (Read)
// ============================================================================

/// Extract (decompress + decrypt) file data from an archive.
/// @param archiveData   Full archive data (memory-mapped).
/// @param archiveOffset Byte offset of the archive start within the mapped data.
/// @param block         Block table entry for this file.
/// @param sectorSize    Sector size in bytes (from header).
/// @param fileKey       Encryption key (from deriveFileKey), or 0 if not encrypted.
/// @return Decompressed file contents, or empty vector on failure.
[[nodiscard]] std::vector<u8> extractFileData(std::span<const u8> archiveData, size_t archiveOffset,
                                              const BlockEntry& block, u32 sectorSize, u32 fileKey,
                                              std::string* error = nullptr,
                                              interfaces::WorkerPool* pool = nullptr);

// ============================================================================
// Encoding (Write)
// ============================================================================

/// Result of encoding file data for writing.
struct EncodedFile {
    std::vector<u8> data;            ///< Encoded file data (sector offset table + sectors).
    u32 compressedSize = 0;          ///< Total compressed size.
    FileFlag flags = FileFlag::None; ///< Block entry flags to use.
};

/// Options for encoding file data.
struct EncodeOptions {
    CompressionFlag compression = CompressionFlag::kZlib; ///< Compression codec (default: zlib).
    bool encrypt = false;
    bool singleUnit = false;
    u32 sectorSize = 4096;
    std::string filename; ///< Needed for encryption key derivation.
};

/// Encode raw file data for writing to an archive.
/// Splits into sectors, compresses, optionally encrypts.
[[nodiscard]] EncodedFile encodeFileData(std::span<const u8> rawData, const EncodeOptions& opts,
                                         interfaces::WorkerPool* pool = nullptr);

// ============================================================================
// Batch Operations (Flattened Pipeline)
// ============================================================================

/// Per-sector compression result for the flattened write pipeline.
struct SectorResult {
    std::vector<u8> data;       ///< Compressed (or stored) sector data.
    bool wasCompressed = false; ///< True if compression was effective.
};

/// Result of batch encoding for the flattened write pipeline.
struct BatchEncodeResult {
    std::vector<EncodedFile> files; ///< One per input item.
};

/// Encode multiple files using a flattened pipeline.
/// All sector compression tasks are submitted as top-level pool tasks.
/// Uses timeline semaphores when available, otherwise falls back to the
/// current parallel-files XOR parallel-sectors strategy.
[[nodiscard]] BatchEncodeResult encodeBatch(
    std::span<const std::pair<std::span<const u8>, EncodeOptions>> items,
    interfaces::WorkerPool* pool);

/// Descriptor for a file to extract in a batch operation.
struct ExtractFileInfo {
    BlockEntry block;
    u32 fileKey = 0;
};

/// Extract multiple files using a flattened pipeline.
/// All sector decompression tasks are submitted as top-level pool tasks.
/// Returns one entry per input file: the decompressed data, or std::nullopt on failure.
[[nodiscard]] std::vector<std::optional<std::vector<u8>>> extractBatch(
    std::span<const u8> archiveData, size_t archiveOffset, std::span<const ExtractFileInfo> files,
    u32 sectorSize, interfaces::WorkerPool* pool);

} // namespace whiteout::storages::mpq
