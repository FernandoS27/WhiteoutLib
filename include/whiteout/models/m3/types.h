// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file types.h
 * @brief Core type definitions and constants for the M3 format
 *
 * This file defines:
 * - FourCC tag constants identifying chunk types in the M3 index table
 * - Core structures: Reference, AnimRef<T>, AnimBlock<T>, Flag, Extent
 * - Vertex buffer handling (VertexBuffer with PImpl)
 * - Index table entry and file header (MD3Header)
 *
 * M3 files use a chunk-based layout. A central Index Table maps FourCC tags
 * to byte offsets. The MODL root chunk contains Ref<T> fields pointing to
 * every sub-chunk. All data is little-endian and 16-byte aligned.
 */

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../../common_types.h"
#include "../../compatibility.h"
#include "../../vector_types.h"

namespace whiteout {
namespace m3 {

// ============================================================================
// Core Structures
// ============================================================================

/**
 * @brief Typed reference into the M3 index table (12 bytes)
 *
 * The primary mechanism for linking chunks together. When entries > 0,
 * seek to indexTable[index].offset and read entries items. A Reference
 * with entries == 0 is null. The flags field is tool metadata (safe to ignore).
 */
struct Reference {
    u32 entries = 0; ///< Number of elements at target (0 = null reference)
    u32 index = 0;   ///< 0-based index into the Index Table
    u32 flags = 0;   ///< Tool metadata (93.5% are 0 in corpus; safe to ignore)
};

/**
 * @brief Animatable reference holding a default value and animation link
 *
 * Holds both a constant default value and a link to keyframed animation data.
 * If animId == 0, the property is not animated — use initValue as a constant.
 * Otherwise, resolve through STC_.animIds to locate keyframe data.
 * Total size depends on sizeof(T): 12 + 2*sizeof(T) + 4 bytes.
 *
 * @tparam T The value type (f32, Vector3f, Quaternion, ColorBGRA, Extent, etc.)
 */
template <typename T>
struct AnimRef {
    u16 interpType = 0; ///< Interpolation: 0=none/step, 1=linear, 2=hermite, 3=bezier
    u16 flags = 0;      ///< Animation flags
    u32 animId = 0;     ///< Animation identifier (links to STC animation data; 0=not animated)
    T initValue{};      ///< Initial/default value (used when not animated)
    T nullValue{};      ///< Null/reset value
    i32 unused = -1;    ///< Typically -1

    /// Check if this property has animation data linked.
    bool isAnimated() const {
        return animId != 0;
    }
};

/**
 * @brief Animation block containing keyframe timestamps and values
 *
 * Each AnimBlock stores a set of time-value keyframe pairs for one animated
 * property within a sub-track container (STC). Timestamps are in animation ticks.
 *
 * @tparam T The keyframe value type
 */
template <typename T>
struct AnimBlock {
    std::vector<int32_t> timestamps; ///< Frame timestamps (animation ticks)
    u32 flags;                       ///< Animation block flags
    u32 endFrame;                    ///< Last frame number
    std::vector<T> keys;             ///< Keyframe values (same count as timestamps)
};

/**
 * @brief Bitfield flags stored as a 4-byte unsigned integer
 *
 * Generic flag container used throughout M3 structures where
 * the flag bits are not mapped to a typed enum.
 */
struct Flag {
    u32 value = 0; ///< Raw flag bits

    /// Test whether a specific bit is set (0-based bit index).
    bool get(int bit) const {
        return (value >> bit) & 1;
    }

    /// Set or clear a specific bit (0-based bit index).
    void set(int bit, bool v) {
        if (v)
            value |= (1u << bit);
        else
            value &= ~(1u << bit);
    }
};

/**
 * @brief Color stored as BGRA (4 bytes)
 *
 * Blue-green-red-alpha byte order, matching the M3 on-disk format.
 */
struct ColorBGRA {
    u8 b = 0; ///< Blue channel
    u8 g = 0; ///< Green channel
    u8 r = 0; ///< Red channel
    u8 a = 0; ///< Alpha channel
};

/**
 * @brief Color stored as BGR (3 bytes, packed)
 *
 * Blue-green-red byte order without alpha. Packed to prevent padding.
 */
#pragma pack(push, 1)
struct ColorBGR {
    u8 b = 0; ///< Blue channel
    u8 g = 0; ///< Green channel
    u8 r = 0; ///< Red channel
};
#pragma pack(pop)

/**
 * @brief Axis-aligned bounding box with bounding sphere radius (28 bytes)
 *
 * Used throughout M3 for model bounds, collision bounds, and per-region extents.
 */
struct Extent {
    Vector3f min{};    ///< AABB minimum corner
    Vector3f max{};    ///< AABB maximum corner
    f32 radius = 0.0f; ///< Bounding sphere radius
};

/**
 * @brief Vertex format flags determining vertex buffer layout
 *
 * These bitmask flags control the per-vertex data layout in the U8__ vertex blob.
 * The vertex stride is: 24 + (hasColor ? 4 : 0) + (numUVs * 4) + 4 bytes.
 */
enum class VertexFormatFlag : u32 {
    None = 0x0,
    VertexColor = 0x0200, ///< Bit 10 (1-based): Has vertex color (adds 4 bytes)
    UV1 = 0x20000,        ///< Bit 18 (1-based): Has UV layer 1 (adds 4 bytes)
    UV2 = 0x40000,        ///< Bit 19 (1-based): Has UV layer 2 (adds 4 bytes)
    UV3 = 0x80000,        ///< Bit 20 (1-based): Has UV layer 3 (adds 4 bytes)
    UV4 = 0x100000,       ///< Bit 21 (1-based): Has UV layer 4 (adds 4 bytes)
    UV5 = 0x20000000,     ///< Bit 30 (1-based): Has UV layer 5 (adds 4 bytes)
};

// Bitwise operators for VertexFormatFlag
inline VertexFormatFlag operator|(VertexFormatFlag lhs, VertexFormatFlag rhs) {
    return static_cast<VertexFormatFlag>(static_cast<u32>(lhs) | static_cast<u32>(rhs));
}
inline VertexFormatFlag operator&(VertexFormatFlag lhs, VertexFormatFlag rhs) {
    return static_cast<VertexFormatFlag>(static_cast<u32>(lhs) & static_cast<u32>(rhs));
}

// Helper to check if a flag is set
inline bool hasFlag(VertexFormatFlag flags, VertexFormatFlag flag) {
    return (static_cast<u32>(flags) & static_cast<u32>(flag)) != 0;
}

/**
 * @brief Decoded vertex buffer with accessor methods
 *
 * Wraps the raw U8__ vertex blob and provides typed accessors for positions,
 * normals, tangents, UVs, bone indices/weights, and vertex colors. The vertex
 * layout is determined by VertexFormatFlag bits stored in MODL.vertexFlags.
 *
 * Uses the PImpl (Pointer to Implementation) idiom to hide layout details.
 */
struct VertexBuffer {
    VertexFormatFlag flags; ///< Vertex format flags from MODL.vertexFlags
    std::vector<u8> data;   ///< Raw vertex data blob

    /// @brief Construct a new VertexBuffer
    VertexBuffer();
    /// @brief Destructor (defined in .cpp for incomplete type)
    ~VertexBuffer();
    VertexBuffer(const VertexBuffer& other);
    VertexBuffer& operator=(const VertexBuffer& other);
    VertexBuffer(VertexBuffer&& other) noexcept;
    VertexBuffer& operator=(VertexBuffer&& other) noexcept;

    /// Initialize internal layout from flags and data.
    void initialize();

    /// @brief Get total number of vertices in the buffer
    size_t vertexCount() const;
    /// @brief Get per-vertex byte stride
    size_t vertexSize() const;
    /// @brief Get number of UV layers present
    size_t UVsNum() const;
    /// @brief Check if vertex color data is present
    bool hasVertexColors() const;

    /// @brief Extract vertex positions (Vector3f, 12 bytes at offset 0)
    std::vector<Vector3f> getPositions() const;
    /// @brief Extract vertex normals (3 x i8 at offset 20, divided by 127.0)
    std::vector<Vector3f> getNormals() const;
    /// @brief Extract tangent vectors (3 x i8 + sign byte at end of vertex)
    std::vector<Vector4f> getTangents() const;
    /// @brief Extract UV coordinates for a layer (i16 pairs, divided by 2048.0)
    std::vector<Vector2f> getUVs(size_t which) const;
    /**
     * @brief Extract UV coordinates using region-level scale/offset (REGN v5+)
     * @param which UV layer index (0-based)
     * @param uvMultiply UV scale factor (default 16.0 in REGN v5+)
     * @param uvOffset UV offset (default 0.0 in REGN v5+)
     * @return Vector of UV coordinates: float_uv = i16_uv * uvMultiply + uvOffset
     */
    std::vector<Vector2f> getUVs(size_t which, f32 uvMultiply, f32 uvOffset) const;
    /// @brief Extract vertex colors (BGRA, only if VertexColor flag is set)
    std::vector<ColorBGRA> getColors() const;
    /// @brief Extract bone indices (4 x u8 at offset 16, region-local)
    std::vector<std::array<u8, 4>> getBoneIndices() const;
    /// @brief Extract bone weights (4 x u8 at offset 12, divide by 255.0)
    std::vector<std::array<u8, 4>> getBoneWeights() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl; // Pimpl to hide implementation details
};

} // namespace m3
} // namespace whiteout
