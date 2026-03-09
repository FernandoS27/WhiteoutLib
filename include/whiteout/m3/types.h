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

#include "../common_types.h"
#include "../compatibility.h"
#include "../vector_types.h"

namespace whiteout {
namespace m3 {

// ============================================================================
// Tag Utilities
// ============================================================================

/**
 * @brief Create a FourCC tag from a 4-character string literal (big-endian storage)
 *
 * M3 tags are stored as 4 ASCII bytes in reverse order when read as a little-endian
 * uint32. This function creates the canonical tag value for comparison.
 *
 * @tparam N Size of the string literal (must be 5 including null terminator)
 * @param str 4-character string literal
 * @return u32 Tag value
 */
template <std::size_t N>
constexpr u32 makeTag(const char (&str)[N]) {
    static_assert(N == 5, "Tag must be exactly 4 characters (plus null terminator)");
    return static_cast<u32>(static_cast<u8>(str[0]) << 24) |
           (static_cast<u32>(static_cast<u8>(str[1])) << 16) |
           (static_cast<u32>(static_cast<u8>(str[2])) << 8) |
           (static_cast<u32>(static_cast<u8>(str[3])) << 0);
}

/// Convert a FourCC tag value back to a human-readable 4-character string.
constexpr std::string tagToString(u32 tag) {
    std::array<char, 5> chars = {
        static_cast<char>((tag >> 24) & 0xFF), static_cast<char>((tag >> 16) & 0xFF),
        static_cast<char>((tag >> 8) & 0xFF), static_cast<char>((tag >> 0) & 0xFF), '\0'};
    return std::string(chars.data(), 4);
}

// ============================================================================
// Known Tags (stored in reversed/canonical form for comparison)
// ============================================================================

// Header
constexpr u32 TAG_MD34 = makeTag("MD34"); ///< Release file magic (MD34)
constexpr u32 TAG_MD33 = makeTag("MD33"); ///< Beta file magic (MD33)

// Primitive data chunks
constexpr u32 TAG_CHAR = makeTag("CHAR"); ///< Null-terminated ASCII string (1 byte per element)
constexpr u32 TAG_U8 = makeTag("U8__");   ///< Unsigned 8-bit (vertex blobs, weights)
constexpr u32 TAG_U16 = makeTag("U16_");  ///< Unsigned 16-bit (indices, bone lookups)
constexpr u32 TAG_U32 = makeTag("U32_");  ///< Unsigned 32-bit (animation IDs, flags)
constexpr u32 TAG_U64 = makeTag("U64_");  ///< Unsigned 64-bit
constexpr u32 TAG_I16 = makeTag("I16_");  ///< Signed 16-bit
constexpr u32 TAG_I32 = makeTag("I32_");  ///< Signed 32-bit (frame timestamps)
constexpr u32 TAG_VEC2 = makeTag("VEC2"); ///< Vector2f (8 bytes)
constexpr u32 TAG_VEC3 = makeTag("VEC3"); ///< Vector3f (12 bytes)
constexpr u32 TAG_VEC4 = makeTag("VEC4"); ///< Vector4f (16 bytes)
constexpr u32 TAG_QUAT = makeTag("QUAT"); ///< Quaternion (16 bytes)
constexpr u32 TAG_REAL = makeTag("REAL"); ///< f32 scalar (4 bytes)
constexpr u32 TAG_FLAG = makeTag("FLAG"); ///< Flag bitfield (4 bytes)
constexpr u32 TAG_BNDS = makeTag("BNDS"); ///< Extent / bounding volume (28 bytes)
constexpr u32 TAG_COL = makeTag("\0COL"); ///< ColorBGRA (4 bytes)
constexpr u32 TAG_MT32 = makeTag("MT32"); ///< Physics mesh triangle indices (7 x u32, 28 bytes)
constexpr u32 TAG_MT16 = makeTag("MT16"); ///< Physics mesh triangle indices (7 x u16, 14 bytes)

// Model root
constexpr u32 TAG_MODL = makeTag("MODL"); ///< Model root chunk (all Ref<T> fields)

// Animation system
constexpr u32 TAG_SEQS = makeTag("SEQS"); ///< Animation sequences (frame ranges, bounds)
constexpr u32 TAG_STC = makeTag("STC_");  ///< Sub-track containers (keyframe data refs)
constexpr u32 TAG_STG = makeTag("STG_");  ///< Animation groups (name + STC indices)
constexpr u32 TAG_STS = makeTag("STS_");  ///< Animation states (animId lookup)
constexpr u32 TAG_BSET = makeTag("BSET"); ///< Bone animation sets

// Animation data blocks (keyframe storage — one tag per value type)
constexpr u32 TAG_SDEV = makeTag("SDEV"); ///< Slot 0: f32 keyframes
constexpr u32 TAG_SD2V = makeTag("SD2V"); ///< Slot 1: Vector2f keyframes
constexpr u32 TAG_SD3V = makeTag("SD3V"); ///< Slot 2: Vector3f keyframes
constexpr u32 TAG_SD4Q = makeTag("SD4Q"); ///< Slot 3: Quaternion keyframes
constexpr u32 TAG_SDCC = makeTag("SDCC"); ///< Slot 4: ColorBGRA keyframes
constexpr u32 TAG_SDR3 = makeTag("SDR3"); ///< Slot 5: f32 keyframes (rotation)
constexpr u32 TAG_SDU8 = makeTag("SDU8"); ///< Slot 6: u8 keyframes
constexpr u32 TAG_SDS6 = makeTag("SDS6"); ///< Slot 7: i16 keyframes
constexpr u32 TAG_SDU6 = makeTag("SDU6"); ///< Slot 8: u16 keyframes
constexpr u32 TAG_SDS3 = makeTag("SDS3"); ///< Slot 9: i32 keyframes
constexpr u32 TAG_SDU3 = makeTag("SDU3"); ///< Slot 10: u32 keyframes
constexpr u32 TAG_SDFG = makeTag("SDFG"); ///< Slot 11: Flag keyframes
constexpr u32 TAG_SDMB = makeTag("SDMB"); ///< Slot 12: Extent keyframes

// Skeleton & mesh
constexpr u32 TAG_BONE = makeTag("BONE"); ///< Skeleton bones (v1, 160 bytes)
constexpr u32 TAG_DIV = makeTag("DIV_");  ///< Mesh divisions (indices, regions, batches)
constexpr u32 TAG_REGN = makeTag("REGN"); ///< Submesh regions (vertex/index ranges)
constexpr u32 TAG_BAT = makeTag("BAT_");  ///< Draw call batches (region → material)
constexpr u32 TAG_MSEC = makeTag("MSEC"); ///< Mesh section bounding shapes
constexpr u32 TAG_IREF = makeTag("IREF"); ///< Inverse bind-pose matrices (one per bone)
constexpr u32 TAG_ATT = makeTag("ATT_");  ///< Attachment points (named bone locations)

// Materials
constexpr u32 TAG_MATM = makeTag("MATM"); ///< Material map (type + index into material arrays)
constexpr u32 TAG_MAT = makeTag("MAT_"); ///< Standard material (diffuse, specular, emissive layers)
constexpr u32 TAG_LAYR = makeTag("LAYR"); ///< Texture layer (UV animation, tint, fresnel)
constexpr u32 TAG_DIS = makeTag("DIS_");  ///< Displacement material
constexpr u32 TAG_CMP = makeTag("CMP_");  ///< Composite material (blended sub-materials)
constexpr u32 TAG_CMS = makeTag("CMS_");  ///< Composite material section
constexpr u32 TAG_TER = makeTag("TER_");  ///< Terrain material
constexpr u32 TAG_VOL = makeTag("VOL_");  ///< Volume material (uniform density fog)
constexpr u32 TAG_HAI = makeTag("HAI_");  ///< Hair material (defunct — always null)
constexpr u32 TAG_VON = makeTag("VON_");  ///< Volume noise material (noisy density fog)
constexpr u32 TAG_CREP = makeTag("CREP"); ///< Creep material (SC2 Zerg creep)
constexpr u32 TAG_STBM = makeTag("STBM"); ///< Splat terrain bake material
constexpr u32 TAG_REF = makeTag("REF_");  ///< Reflection material
constexpr u32 TAG_LFLR = makeTag("LFLR"); ///< Lens flare material
constexpr u32 TAG_MADD = makeTag("MADD"); ///< Buffer material / material add data (v30+)
constexpr u32 TAG_SCHR = makeTag("SCHR"); ///< String reference array (Ref<CHAR>, 12 bytes)

// Lights, cameras, events
constexpr u32 TAG_LITE = makeTag("LITE"); ///< Light (omni, spot, or directional)
constexpr u32 TAG_CAM = makeTag("CAM_");  ///< Camera (FOV, DOF, clip planes)
constexpr u32 TAG_EVNT = makeTag("EVNT"); ///< Event (sound triggers, effects)

// Particle systems
constexpr u32 TAG_PAR = makeTag("PAR_");  ///< Particle emitter (emission, physics, collision)
constexpr u32 TAG_PARC = makeTag("PARC"); ///< Particle emitter copy (overrides emission rate)
constexpr u32 TAG_RIB = makeTag("RIB_");  ///< Ribbon emitter (connected trail strips)
constexpr u32 TAG_SRIB = makeTag("SRIB"); ///< Spline ribbon sub-structure
constexpr u32 TAG_PROJ = makeTag("PROJ"); ///< Projector (projected textures / decals)
constexpr u32 TAG_FOR = makeTag("FOR_");  ///< Force (radial, wind, explosion)
constexpr u32 TAG_WRP = makeTag("WRP_");  ///< Warp (spatial distortion)

// Physics
constexpr u32 TAG_PHRB = makeTag("PHRB"); ///< Rigid body (collision shape + dynamics)
constexpr u32 TAG_PHSH = makeTag("PHSH"); ///< Physics shape (box, sphere, capsule, hull, mesh)
constexpr u32 TAG_PHYJ = makeTag("PHYJ"); ///< Physics joint (hinge, cone, etc.)
constexpr u32 TAG_PHCL = makeTag("PHCL"); ///< Cloth physics simulation (v28+)
constexpr u32 TAG_PHCC = makeTag("PHCC"); ///< Cloth collider (capsule shape)
constexpr u32 TAG_PHAC = makeTag("PHAC"); ///< Cloth proxy (vertex mapping)
constexpr u32 TAG_PHCT = makeTag("PHCT"); ///< Physics constraint (rigid body linkage)

// Miscellaneous
constexpr u32 TAG_SSGS = makeTag("SSGS"); ///< Hit test shape (box, sphere, capsule, mesh)
constexpr u32 TAG_ATVL = makeTag("ATVL"); ///< Attachment volume (hit test + dual bone refs)
constexpr u32 TAG_TRGD = makeTag("TRGD"); ///< Trigger data
constexpr u32 TAG_PATU = makeTag("PATU"); ///< Turret behavior (yaw/pitch limits)
constexpr u32 TAG_BBSC = makeTag("BBSC"); ///< Billboard behavior
constexpr u32 TAG_IKJT = makeTag("IKJT"); ///< IK joint (raycast-based foot planting)
constexpr u32 TAG_IK2J = makeTag("IK2J"); ///< Two-joint IK solver
constexpr u32 TAG_IKCC = makeTag("IKCC"); ///< CCD IK solver (v24+)
constexpr u32 TAG_PAOB = makeTag("PAOB"); ///< One-bone IK solver
constexpr u32 TAG_SHBX = makeTag("SHBX"); ///< Shadow box (64-byte matrix)
constexpr u32 TAG_DMMT = makeTag("DMMT"); ///< Physics mesh triangle (winged-edge, 28 bytes)
constexpr u32 TAG_DMME = makeTag("DMME"); ///< Physics mesh edge (winged-edge, 20 bytes)
constexpr u32 TAG_DMMN = makeTag("DMMN"); ///< Physics mesh normal (8 or 12 bytes)
constexpr u32 TAG_DMSE = makeTag("DMSE"); ///< Convex hull half-edge (4 bytes)
constexpr u32 TAG_VVOL = makeTag("VVOL"); ///< View volume (visibility culling)
constexpr u32 TAG_LFSB = makeTag("LFSB"); ///< Sub-flare entry (lens flare element)
constexpr u32 TAG_SVC2 = makeTag("SVC2"); ///< Static Vector2f channel (AnimRef<Vector2f>)
constexpr u32 TAG_SVC3 = makeTag("SVC3"); ///< Static Vector3f channel (AnimRef<Vector3f>)
constexpr u32 TAG_SS32 = makeTag("SS32"); ///< Static i32 channel
constexpr u32 TAG_SU32 = makeTag("SU32"); ///< Static u32 channel
constexpr u32 TAG_SR32 = makeTag("SR32"); ///< Static f32 channel (AnimRef<f32>)
constexpr u32 TAG_TMD = makeTag("TMD_");  ///< Trailing model (defunct)

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

} // namespace m3
} // namespace whiteout
