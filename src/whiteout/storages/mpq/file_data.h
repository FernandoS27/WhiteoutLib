// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file file_data.h
/// @brief MPQ file data extraction and encoding (sector-based read/write).

#pragma once

#include <whiteout/common_types.h>

#include "block_table.h"

#include <span>
#include <string>
#include <vector>

namespace whiteout::interfaces { class WorkerPool; }

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
[[nodiscard]] std::vector<u8> extractFileData(
    std::span<const u8> archiveData,
    size_t archiveOffset,
    const BlockEntry& block,
    u32 sectorSize,
    u32 fileKey,
    std::string* error = nullptr,
    interfaces::WorkerPool* pool = nullptr);

// ============================================================================
// Encoding (Write)
// ============================================================================

/// Result of encoding file data for writing.
struct EncodedFile {
    std::vector<u8> data;     ///< Encoded file data (sector offset table + sectors).
    u32 compressedSize = 0;   ///< Total compressed size.
    u32 flags = 0;            ///< Block entry flags to use.
};

/// Options for encoding file data.
struct EncodeOptions {
    u8 compression = 0x02;    ///< CompressionFlag (default: zlib).
    bool encrypt = false;
    bool singleUnit = false;
    u32 sectorSize = 4096;
    std::string filename;     ///< Needed for encryption key derivation.
};

/// Encode raw file data for writing to an archive.
/// Splits into sectors, compresses, optionally encrypts.
[[nodiscard]] EncodedFile encodeFileData(
    std::span<const u8> rawData,
    const EncodeOptions& opts,
    interfaces::WorkerPool* pool = nullptr);

} // namespace whiteout::storages::mpq
