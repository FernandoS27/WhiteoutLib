// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/**
 * @file types.h
 * @brief Public types for the MPQ storage module
 *
 * This file defines:
 * - FormatVersion and Compression enumerations
 * - Locale constants (Windows locale IDs)
 * - FileFlags constants (public subset of block-entry flags)
 * - FileInfo and ArchiveInfo query structs
 * - WriteOptions and CreateOptions for write and create operations
 */

#pragma once

#include <whiteout/common_types.h>

#include <cstdint>
#include <string>

namespace whiteout {
namespace storages {
namespace mpq {

// ============================================================================
// Enumerations
// ============================================================================

/// MPQ format version.
enum class FormatVersion : u16 {
    V1 = 0, ///< Original format (up to 4 GB archives).
    V2 = 1, ///< Extended format (>4 GB archives, hi-block table).
};

/// Compression algorithm for writing files.
enum class Compression : u8 {
    None   = 0,    ///< No compression; data stored verbatim.
    Zlib   = 0x02, ///< zlib / DEFLATE compression (most common MPQ codec).
    PKware = 0x08, ///< PKware DCL (implode) compression.
};

// ============================================================================
// Locale Constants
// ============================================================================

namespace Locale {
    static constexpr u16 Neutral = 0x0000;
    static constexpr u16 Chinese = 0x0404;
    static constexpr u16 Czech = 0x0405;
    static constexpr u16 German = 0x0407;
    static constexpr u16 English = 0x0409;
    static constexpr u16 Spanish = 0x040A;
    static constexpr u16 French = 0x040C;
    static constexpr u16 Italian = 0x0410;
    static constexpr u16 Japanese = 0x0411;
    static constexpr u16 Korean = 0x0412;
    static constexpr u16 Polish = 0x0415;
    static constexpr u16 Portuguese = 0x0416;
    static constexpr u16 Russian = 0x0419;
    static constexpr u16 EnglishUK = 0x0809;
} // namespace Locale

// ============================================================================
// File Flags (public subset)
// ============================================================================

namespace FileFlags {
    static constexpr u32 Compressed = 0x00000200; ///< File uses sector compression.
    static constexpr u32 Encrypted  = 0x00010000; ///< File data is encrypted.
    static constexpr u32 SingleUnit = 0x01000000; ///< File stored as a single unit (no sector splitting).
    static constexpr u32 Exists     = 0x80000000; ///< Slot is occupied by a real file.
} // namespace FileFlags

// ============================================================================
// Info Structs
// ============================================================================

/// Information about a single file in the archive.
struct FileInfo {
    std::string name;              ///< Filename (from listfile or hash table lookup).
    u32 compressedSize = 0;        ///< Compressed storage size in bytes.
    u32 uncompressedSize = 0;      ///< Uncompressed (original) file size in bytes.
    u32 flags = 0;                 ///< Block entry flags (see FileFlags namespace).
    u16 locale = 0;                ///< Locale ID (typically Locale::Neutral).
};

/// Summary information about the archive.
struct ArchiveInfo {
    u16 formatVersion = 0;     ///< 0 = V1, 1 = V2.
    u32 hashTableEntries = 0;  ///< Hash table capacity (always a power of 2).
    u32 blockTableEntries = 0; ///< Number of occupied block table entries.
    u32 sectorSize = 0;        ///< Sector size in bytes (512 << sectorSizeShift).
    u64 archiveSize = 0;       ///< Total archive size in bytes.
};

// ============================================================================
// Write / Create Options
// ============================================================================

/// Options for writing a file into the archive.
struct WriteOptions {
    Compression compression = Compression::Zlib; ///< Compression algorithm to apply.
    u16 locale = Locale::Neutral;                ///< Locale ID for the hash table slot.
    bool encrypt = false;                        ///< Encrypt file data with a derived key.
    bool singleUnit = false;                     ///< Store the file as a single unpartitioned unit.
};

/// Options for creating a new archive.
struct CreateOptions {
    FormatVersion version = FormatVersion::V1; ///< Archive format version (V1 or V2).
    u32 hashTableSize = 1024;                  ///< Initial hash table capacity; rounded up to the next power of 2.
    u16 sectorSizeShift = 3;                   ///< Sector size = 512 << shift (default 3 → 4096 bytes).
};

} // namespace mpq
} // namespace storages
} // namespace whiteout
