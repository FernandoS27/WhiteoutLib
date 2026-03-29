// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file header.h
/// @brief MPQ archive header structures and parsing.

#pragma once

#include <whiteout/common_types.h>

#include <array>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace whiteout::storages::mpq {

// ============================================================================
// Constants
// ============================================================================

/// MPQ archive signature "MPQ\x1A".
static constexpr u32 kMpqMagic = 0x1A51504D;

/// MPQ user-data signature "MPQ\x1B".
static constexpr u32 kMpqUserDataMagic = 0x1B51504D;

/// Archive alignment boundary when scanning for the header.
static constexpr u32 kHeaderSearchAlignment = 0x200;

// ============================================================================
// Header Structures
// ============================================================================

/// MPQ archive header (all versions, V1–V4 fields).
struct MpqHeader {
    // -- V1 fields (32 bytes) --
    u32 magic = kMpqMagic;
    u32 headerSize = 32;
    u32 archiveSize = 0;      ///< Archive size (V1: 32-bit)
    u16 formatVersion = 0;    ///< 0 = V1, 1 = V2, 2 = V3, 3 = V4
    u16 sectorSizeShift = 3;  ///< Sector size = 512 << sectorSizeShift
    u32 hashTableOffset = 0;
    u32 blockTableOffset = 0;
    u32 hashTableEntries = 0;
    u32 blockTableEntries = 0;

    // -- V2 fields (44 bytes total) --
    u64 hiBlockTableOffset = 0;
    u16 hashTableOffsetHi = 0;
    u16 blockTableOffsetHi = 0;

    // -- V3 fields (68 bytes total) --
    u64 archiveSize64 = 0;
    u64 betTableOffset = 0;
    u64 hetTableOffset = 0;

    // -- V4 fields (208 bytes total) --
    u64 hashTableSize64 = 0;
    u64 blockTableSize64 = 0;
    u64 hiBlockTableSize64 = 0;
    u64 hetTableSize64 = 0;
    u64 betTableSize64 = 0;
    u32 rawChunkSize = 0;
    std::array<u8, 16> blockTableMd5{};
    std::array<u8, 16> hashTableMd5{};
    std::array<u8, 16> hiBlockTableMd5{};
    std::array<u8, 16> betTableMd5{};
    std::array<u8, 16> hetTableMd5{};
    std::array<u8, 16> mpqHeaderMd5{};

    /// Compute actual hash table byte offset within the archive.
    [[nodiscard]] u64 hashTableByteOffset() const {
        return hashTableOffset | (static_cast<u64>(hashTableOffsetHi) << 32);
    }
    /// Compute actual block table byte offset within the archive.
    [[nodiscard]] u64 blockTableByteOffset() const {
        return blockTableOffset | (static_cast<u64>(blockTableOffsetHi) << 32);
    }
    /// Compute sector size in bytes.
    [[nodiscard]] u32 sectorSize() const {
        return 512u << sectorSizeShift;
    }
};

/// User data block (preceding the main header in some archives).
struct UserData {
    u32 magic = kMpqUserDataMagic;
    u32 userDataSize = 0;      ///< Size of the user data block.
    u32 headerOffset = 0;      ///< Offset to MPQ header from start of user data.
    u32 userDataHeaderSize = 0;
    std::vector<u8> data;      ///< Raw user data content.
};



/// Result of parsing the archive header.
struct HeaderParseResult {
    MpqHeader header;
    size_t archiveOffset = 0;     ///< Byte offset of MPQ header within the file.
    std::optional<UserData> userData;
};

// ============================================================================
// Functions
// ============================================================================

/// Scan a memory-mapped archive for the MPQ header.
/// Searches at 0x200-byte boundaries. Returns the parsed header and the byte
/// offset where the archive begins, or nullopt if no valid header is found.
[[nodiscard]] std::optional<HeaderParseResult>
findAndParseHeader(std::span<const u8> fileData);

/// Build a new MPQ header from creation parameters.
[[nodiscard]] MpqHeader buildHeader(u16 formatVersion, u32 hashTableSize, u16 sectorSizeShift);

/// Round a value up to the next power of 2 (if not already).
[[nodiscard]] u32 nextPowerOf2(u32 v);

} // namespace whiteout::storages::mpq
