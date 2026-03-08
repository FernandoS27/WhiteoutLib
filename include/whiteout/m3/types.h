// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

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

/// Create a FourCC tag from a 4-character string literal (little-endian).
template <std::size_t N>
constexpr u32 makeTag(const char (&str)[N]) {
    static_assert(N == 5, "Tag must be exactly 4 characters (plus null terminator)");
    return static_cast<u32>(static_cast<u8>(str[0]) << 24) |
           (static_cast<u32>(static_cast<u8>(str[1])) << 16) |
           (static_cast<u32>(static_cast<u8>(str[2])) << 8) |
           (static_cast<u32>(static_cast<u8>(str[3])) << 0);
}

constexpr std::string tagToString(u32 tag) {
    char chars[5] = {static_cast<char>((tag >> 24) & 0xFF), static_cast<char>((tag >> 16) & 0xFF),
                     static_cast<char>((tag >> 8) & 0xFF), static_cast<char>((tag >> 0) & 0xFF),
                     '\0'};
    return std::string(chars, 4);
}

// ============================================================================
// Known Tags (stored in reversed/canonical form for comparison)
// ============================================================================

// Header
constexpr u32 TAG_MD34 = makeTag("MD34"); // -> MD34
constexpr u32 TAG_MD33 = makeTag("MD33"); // -> MD33

// Primitive data chunks
constexpr u32 TAG_CHAR = makeTag("CHAR"); // -> CHAR
constexpr u32 TAG_U8 = makeTag("U8__");   // -> U8__
constexpr u32 TAG_U16 = makeTag("U16_");  // -> U16_
constexpr u32 TAG_U32 = makeTag("U32_");  // -> U32_
constexpr u32 TAG_U64 = makeTag("U64_");  // -> U64_
constexpr u32 TAG_I16 = makeTag("I16_");  // -> I16_
constexpr u32 TAG_I32 = makeTag("I32_");  // -> I32_
constexpr u32 TAG_VEC2 = makeTag("VEC2"); // -> VEC2
constexpr u32 TAG_VEC3 = makeTag("VEC3"); // -> VEC3
constexpr u32 TAG_VEC4 = makeTag("VEC4"); // -> VEC4
constexpr u32 TAG_QUAT = makeTag("QUAT"); // -> QUAT
constexpr u32 TAG_REAL = makeTag("REAL"); // -> REAL
constexpr u32 TAG_FLAG = makeTag("FLAG"); // -> FLAG
constexpr u32 TAG_BNDS = makeTag("BNDS"); // -> BNDS
constexpr u32 TAG_COL = makeTag("\0COL"); // -> COL\0
constexpr u32 TAG_MT32 = makeTag("MT32"); // -> MT32
constexpr u32 TAG_MT16 = makeTag("MT16"); // -> MT16

// Model root
constexpr u32 TAG_MODL = makeTag("MODL"); // -> MODL

// Animation system
constexpr u32 TAG_SEQS = makeTag("SEQS"); // -> SEQS
constexpr u32 TAG_STC = makeTag("STC_");  // -> STC_
constexpr u32 TAG_STG = makeTag("STG_");  // -> STG_
constexpr u32 TAG_STS = makeTag("STS_");  // -> STS_
constexpr u32 TAG_BSET = makeTag("BSET"); // -> BSET

// Animation data blocks
constexpr u32 TAG_SDEV = makeTag("SDEV"); // -> SDEV
constexpr u32 TAG_SD2V = makeTag("SD2V"); // -> SD2V
constexpr u32 TAG_SD3V = makeTag("SD3V"); // -> SD3V
constexpr u32 TAG_SD4Q = makeTag("SD4Q"); // -> SD4Q
constexpr u32 TAG_SDCC = makeTag("SDCC"); // -> SDCC
constexpr u32 TAG_SDR3 = makeTag("SDR3"); // -> SDR3
constexpr u32 TAG_SDU8 = makeTag("SDU8"); // -> SDU8
constexpr u32 TAG_SDS6 = makeTag("SDS6"); // -> SDS6
constexpr u32 TAG_SDU6 = makeTag("SDU6"); // -> SDU6
constexpr u32 TAG_SDS3 = makeTag("SDS3"); // -> SDS3
constexpr u32 TAG_SDU3 = makeTag("SDU3"); // -> SDU3
constexpr u32 TAG_SDFG = makeTag("SDFG"); // -> SDFG
constexpr u32 TAG_SDMB = makeTag("SDMB"); // -> SDMB

// Skeleton & mesh
constexpr u32 TAG_BONE = makeTag("BONE"); // -> BONE
constexpr u32 TAG_DIV = makeTag("DIV_");  // -> DIV_
constexpr u32 TAG_REGN = makeTag("REGN"); // -> REGN
constexpr u32 TAG_BAT = makeTag("BAT_");  // -> BAT_
constexpr u32 TAG_MSEC = makeTag("MSEC"); // -> MSEC
constexpr u32 TAG_IREF = makeTag("IREF"); // -> IREF
constexpr u32 TAG_ATT = makeTag("ATT_");  // -> ATT_

// Materials
constexpr u32 TAG_MATM = makeTag("MATM"); // -> MATM
constexpr u32 TAG_MAT = makeTag("MAT_");  // -> MAT_
constexpr u32 TAG_LAYR = makeTag("LAYR"); // -> LAYR
constexpr u32 TAG_DIS = makeTag("DIS_");  // -> DIS_
constexpr u32 TAG_CMP = makeTag("CMP_");  // -> CMP_
constexpr u32 TAG_CMS = makeTag("CMS_");  // -> CMS_
constexpr u32 TAG_TER = makeTag("TER_");  // -> TER_
constexpr u32 TAG_VOL = makeTag("VOL_");  // -> VOL_
constexpr u32 TAG_HAI = makeTag("HAI_");  // -> HAI_ (defunct)
constexpr u32 TAG_VON = makeTag("VON_");  // -> VON_
constexpr u32 TAG_CREP = makeTag("CREP"); // -> CREP
constexpr u32 TAG_STBM = makeTag("STBM"); // -> STBM
constexpr u32 TAG_REF = makeTag("REF_");  // -> REF_
constexpr u32 TAG_LFLR = makeTag("LFLR"); // -> LFLR
constexpr u32 TAG_MADD = makeTag("MADD"); // -> MADD
constexpr u32 TAG_SCHR = makeTag("SCHR"); // -> SCHR

// Lights, cameras, events
constexpr u32 TAG_LITE = makeTag("LITE"); // -> LITE
constexpr u32 TAG_CAM = makeTag("CAM_");  // -> CAM_
constexpr u32 TAG_EVNT = makeTag("EVNT"); // -> EVNT

// Particle systems
constexpr u32 TAG_PAR = makeTag("PAR_");  // -> PAR_
constexpr u32 TAG_PARC = makeTag("PARC"); // -> PARC
constexpr u32 TAG_RIB = makeTag("RIB_");  // -> RIB_
constexpr u32 TAG_SRIB = makeTag("SRIB"); // -> SRIB
constexpr u32 TAG_PROJ = makeTag("PROJ"); // -> PROJ
constexpr u32 TAG_FOR = makeTag("FOR_");  // -> FOR_
constexpr u32 TAG_WRP = makeTag("WRP_");  // -> WRP_

// Physics
constexpr u32 TAG_PHRB = makeTag("PHRB"); // -> PHRB
constexpr u32 TAG_PHSH = makeTag("PHSH"); // -> PHSH
constexpr u32 TAG_PHYJ = makeTag("PHYJ"); // -> PHYJ
constexpr u32 TAG_PHCL = makeTag("PHCL"); // -> PHCL
constexpr u32 TAG_PHCC = makeTag("PHCC"); // -> PHCC
constexpr u32 TAG_PHAC = makeTag("PHAC"); // -> PHAC
constexpr u32 TAG_PHCT = makeTag("PHCT"); // -> PHCT

// Miscellaneous
constexpr u32 TAG_SSGS = makeTag("SSGS"); // -> SSGS
constexpr u32 TAG_ATVL = makeTag("ATVL"); // -> ATVL
constexpr u32 TAG_TRGD = makeTag("TRGD"); // -> TRGD
constexpr u32 TAG_PATU = makeTag("PATU"); // -> PATU
constexpr u32 TAG_BBSC = makeTag("BBSC"); // -> BBSC
constexpr u32 TAG_IKJT = makeTag("IKJT"); // -> IKJT
constexpr u32 TAG_IK2J = makeTag("IK2J"); // -> IK2J
constexpr u32 TAG_IKCC = makeTag("IKCC"); // -> IKCC
constexpr u32 TAG_PAOB = makeTag("PAOB"); // -> PAOB
constexpr u32 TAG_SHBX = makeTag("SHBX"); // -> SHBX
constexpr u32 TAG_DMMT = makeTag("DMMT"); // -> DMMT
constexpr u32 TAG_DMME = makeTag("DMME"); // -> DMME
constexpr u32 TAG_DMMN = makeTag("DMMN"); // -> DMMN
constexpr u32 TAG_DMSE = makeTag("DMSE"); // -> DMSE
constexpr u32 TAG_VVOL = makeTag("VVOL"); // -> VVOL
constexpr u32 TAG_LFSB = makeTag("LFSB"); // -> LFSB
constexpr u32 TAG_SVC2 = makeTag("SVC2"); // -> SVC2
constexpr u32 TAG_SVC3 = makeTag("SVC3"); // -> SVC3
constexpr u32 TAG_SS32 = makeTag("SS32"); // -> SS32
constexpr u32 TAG_SU32 = makeTag("SU32"); // -> SU32
constexpr u32 TAG_SR32 = makeTag("SR32"); // -> SR32
constexpr u32 TAG_TMD = makeTag("TMD_");  // -> TMD_

// ============================================================================
// Core Structures
// ============================================================================

/// Reference into the index table (12 bytes)
struct Reference {
    u32 entries = 0; // Number of elements at target
    u32 index = 0;   // 0-based index into index table
    u32 flags = 0;   // Usually 0
};

/// Animatable reference holding a default value and animation link
template <typename T>
struct AnimRef {
    u16 interpType = 0; // Interpolation type
    u16 flags = 0;      // Animation flags
    u32 animId = 0;     // Animation identifier
    T initValue{};      // Initial/default value
    T nullValue{};      // Null/reset value
    i32 unused = -1;    // Typically -1

    bool isAnimated() const {
        return animId != 0;
    }
};

/// Animation block for keyframe data
template <typename T>
struct AnimBlock {
    std::vector<int32_t> timestamps; // frame timestamps
    u32 flags;
    u32 endFrame;
    std::vector<T> keys; // -> typed array (polymorphic keyframe data)
};

/// Bitfield flags (4 bytes)
struct Flag {
    u32 value = 0;
    bool get(int bit) const {
        return (value >> bit) & 1;
    }
    void set(int bit, bool v) {
        if (v)
            value |= (1u << bit);
        else
            value &= ~(1u << bit);
    }
};

/// Color stored as BGRA (4 bytes)
struct ColorBGRA {
    u8 b = 0, g = 0, r = 0, a = 0;
};

/// Color stored as BGR (3 bytes, packed)
#pragma pack(push, 1)
struct ColorBGR {
    u8 b = 0, g = 0, r = 0;
};
#pragma pack(pop)

/// Bounding extents (28 bytes)
struct Extent {
    Vector3f min{};
    Vector3f max{};
    f32 radius = 0.0f;
};

/// Vertex format flags (determines vertex buffer layout)
enum class VertexFormatFlag : u32 {
    None = 0x0,
    VertexColor = 0x0200, ///< Bit 10 (1-based): Has vertex color (adds 4 bytes)
    UV1 = 0x20000,        ///< Bit 18 (1-based): Has UV layer 1 (adds 4 bytes)
    UV2 = 0x40000,        ///< Bit 19 (1-based): Has UV layer 2 (adds 4 bytes)
    UV3 = 0x80000,        ///< Bit 20 (1-based): Has UV layer 3 (adds 4 bytes)
    UV4 = 0x100000,       ///< Bit 21 (1-based): Has UV layer 4 (adds 4 bytes)
    UV5 = 0x20000000,     ///< Bit 30 (1-based): Has UV layer 5 (adds 4 bytes)
};

// Define bitwise operators for VertexFormatFlag
inline VertexFormatFlag operator|(VertexFormatFlag lhs, VertexFormatFlag rhs) {
    return static_cast<VertexFormatFlag>(static_cast<u32>(lhs) | static_cast<u32>(rhs));
}
inline VertexFormatFlag operator&(VertexFormatFlag lhs, VertexFormatFlag rhs) {
    return static_cast<VertexFormatFlag>(static_cast<u32>(lhs) & static_cast<u32>(rhs));
}
inline bool hasFlag(VertexFormatFlag flags, VertexFormatFlag flag) {
    return (static_cast<u32>(flags) & static_cast<u32>(flag)) != 0;
}

struct VertexBuffer {
    VertexFormatFlag flags;
    std::vector<u8> data;

    VertexBuffer();
    ~VertexBuffer();
    VertexBuffer(const VertexBuffer& other);
    VertexBuffer& operator=(const VertexBuffer& other);
    VertexBuffer(VertexBuffer&& other) noexcept;
    VertexBuffer& operator=(VertexBuffer&& other) noexcept;

    void initialize();

    size_t vertexCount() const;
    size_t vertexSize() const;
    size_t UVsNum() const;
    bool hasVertexColors() const;

    std::vector<Vector3f> getPositions() const;
    std::vector<Vector3f> getNormals() const;
    std::vector<Vector4f> getTangents() const;
    std::vector<Vector2f> getUVs(size_t which) const;
    /// Get UV coordinates for a specific layer, using region-level scale/offset (REGN v5+).
    /// For REGN v5+: float_uv = i16_uv * uvMultiply + uvOffset
    std::vector<Vector2f> getUVs(size_t which, f32 uvMultiply, f32 uvOffset) const;
    std::vector<ColorBGRA> getColors() const;
    std::vector<std::array<u8, 4>> getBoneIndices() const;
    std::vector<std::array<u8, 4>> getBoneWeights() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl; // Pimpl to hide implementation details
};

// ============================================================================
// Index Table Entry
// ============================================================================

/// Each entry in the M3 index table (16 bytes on disk)
struct IndexEntry {
    u32 tag = 0;     // FourCC tag (raw bytes, need reversal for display)
    u32 offset = 0;  // Byte offset in file
    u32 count = 0;   // Number of items
    u32 version = 0; // Version of the chunk structure
};

// ============================================================================
// MD3 Header
// ============================================================================

/// File header (32 bytes, always at offset 0)
struct MD3Header {
    u32 magic = 0;                  // "MD34" or "MD33" for beta models
    u32 indexOffset = 0;            // Byte offset to the Index Table
    u32 indexCount = 0;             // Number of IndexEntry structs
    Reference modelRef{};           // Reference to MODL chunk
    std::array<u8, 8> padding = {}; // Padding to 32 bytes
};

static_assert(sizeof(MD3Header) == 32, "MD3Header must be 32 bytes");

} // namespace m3
} // namespace whiteout
