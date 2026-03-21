// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include <whiteout/models/wem/structures.h>

#include <array>

namespace whiteout {
namespace models {
namespace wem {

constexpr u32 kWoemMagic = 0x4D454F57; // "WOEM" in little-endian
constexpr u32 kCurrentVersion = 2;     // v2: index-table format

constexpr u32 kTag(const char (&s)[5]) {
    return static_cast<u32>(static_cast<u8>(s[0])) |
           (static_cast<u32>(static_cast<u8>(s[1])) << 8) |
           (static_cast<u32>(static_cast<u8>(s[2])) << 16) |
           (static_cast<u32>(static_cast<u8>(s[3])) << 24);
}

// ============================================================================
// Index table structures (on disk)
// ============================================================================

/// Index table entry (16 bytes). Located at header.indexOffset.
struct IndexEntry {
    u32 tag = 0;     ///< FourCC tag identifying the data type
    u32 offset = 0;  ///< Byte offset to data in file
    u32 count = 0;   ///< Number of items
    u32 version = 0; ///< Structure version
};

/// Reference to data via index table (12 bytes). Written inline within structures.
struct Reference {
    u32 entries = 0; ///< Number of elements (0 = null reference)
    u32 index = 0;   ///< Index into the index table
    u32 flags = 0;   ///< Reserved
};

/// File header (32 bytes, always at offset 0).
struct WEMHeader {
    u32 magic = 0;       ///< kWoemMagic
    u32 version = 0;     ///< kCurrentVersion
    u32 indexOffset = 0;  ///< Byte offset to index table
    u32 indexCount = 0;   ///< Number of IndexEntry records
    Reference modelRef{}; ///< Reference to MODL data
    u32 padding = 0;      ///< Pad to 32 bytes
};

static_assert(sizeof(IndexEntry) == 16, "IndexEntry must be 16 bytes");
static_assert(sizeof(Reference) == 12, "Reference must be 12 bytes");
static_assert(sizeof(WEMHeader) == 32, "WEMHeader must be 32 bytes");

// ============================================================================
// Chunk tag traits — associates each type with its FourCC tag and properties
// ============================================================================

template <typename T>
struct ChunkTagTraits; // Primary template — specialize below

// --- Basic types ---

template <>
struct ChunkTagTraits<char> {
    static constexpr u32 value = kTag("CHAR");
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = true;
};

template <>
struct ChunkTagTraits<u8> {
    static constexpr u32 value = kTag("U8__");
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = true;
};

template <>
struct ChunkTagTraits<u16> {
    static constexpr u32 value = kTag("U16_");
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = true;
};

template <>
struct ChunkTagTraits<u32> {
    static constexpr u32 value = kTag("U32_");
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = true;
};

template <>
struct ChunkTagTraits<u64> {
    static constexpr u32 value = kTag("U64_");
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = true;
};

template <>
struct ChunkTagTraits<i16> {
    static constexpr u32 value = kTag("I16_");
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = true;
};

template <>
struct ChunkTagTraits<i32> {
    static constexpr u32 value = kTag("I32_");
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = true;
};

template <>
struct ChunkTagTraits<f32> {
    static constexpr u32 value = kTag("REAL");
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = true;
};

template <>
struct ChunkTagTraits<Vector2f> {
    static constexpr u32 value = kTag("VEC2");
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = true;
};

template <>
struct ChunkTagTraits<Vector3f> {
    static constexpr u32 value = kTag("VEC3");
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = true;
};

template <>
struct ChunkTagTraits<Vector4f> {
    static constexpr u32 value = kTag("VEC4");
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = true;
};

template <>
struct ChunkTagTraits<Quaternion> {
    static constexpr u32 value = kTag("QUAT");
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = true;
};

template <>
struct ChunkTagTraits<Extent> {
    static constexpr u32 value = kTag("BNDS");
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = true;
};

template <>
struct ChunkTagTraits<std::array<u8, 4>> {
    static constexpr u32 value = kTag("BYT4");
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = true;
};

// --- Structure types ---

template <>
struct ChunkTagTraits<Model> {
    static constexpr u32 value = kTag("MODL");
    static constexpr u32 max_version = 1;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<TextureRef> {
    static constexpr u32 value = kTag("TEXR");
    static constexpr u32 max_version = 1;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<Material> {
    static constexpr u32 value = kTag("MATL");
    static constexpr u32 max_version = 1;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<Mesh> {
    static constexpr u32 value = kTag("MESH");
    static constexpr u32 max_version = 1;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<Submesh> {
    static constexpr u32 value = kTag("SUBM");
    static constexpr u32 max_version = 1;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<TextureSlot> {
    static constexpr u32 value = kTag("TXSL");
    static constexpr u32 max_version = 1;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<CompositeSection> {
    static constexpr u32 value = kTag("CSEC");
    static constexpr u32 max_version = 1;
    static constexpr bool is_trivial = false;
};

// Convenience: retrieve tag for a type
template <typename T>
inline constexpr u32 chunkTag = ChunkTagTraits<T>::value;

} // namespace wem
} // namespace models
} // namespace whiteout
