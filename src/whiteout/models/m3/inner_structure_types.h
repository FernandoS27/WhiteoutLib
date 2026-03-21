// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include <string>

#include <whiteout/common_types.h>
#include <whiteout/models/m3/m3.h>

namespace whiteout::m3 {

// ============================================================================
// Index Table Entry
// ============================================================================

/**
 * @brief Index table entry (16 bytes on disk)
 *
 * Located at header.indexOffset. Each entry maps a FourCC tag to chunk data.
 * Multiple entries can share the same tag (e.g. many CHAR or LAYR entries).
 * Chunk data size = count × chunkSize(tag, version).
 */
struct IndexEntry {
    u32 tag = 0;     ///< FourCC tag (raw bytes, need reversal for display)
    u32 offset = 0;  ///< Byte offset to chunk data in file
    u32 count = 0;   ///< Number of items in this chunk
    u32 version = 0; ///< Struct version for this chunk type
};

// ============================================================================
// MD34 Header
// ============================================================================

/**
 * @brief M3 file header (32 bytes, always at offset 0)
 *
 * Reading order:
 * 1. Read 32-byte header (magic must be MD34 or MD33)
 * 2. Seek to indexOffset and read indexCount IndexEntry structs
 * 3. Locate the MODL chunk via modelRef.index into the index table
 * 4. MODL contains Ref<T> fields pointing to every sub-chunk
 */
struct MD3Header {
    u32 magic = 0;                  ///< File signature: "MD34" (release) or "MD33" (beta)
    u32 indexOffset = 0;            ///< Byte offset to the Index Table
    u32 indexCount = 0;             ///< Number of IndexEntry structs
    Reference modelRef{};           ///< Reference to MODL chunk (typically index 1)
    std::array<u8, 8> padding = {}; ///< Padding to 32 bytes
};

static_assert(sizeof(MD3Header) == 32, "MD3Header must be 32 bytes");

} // namespace whiteout::m3