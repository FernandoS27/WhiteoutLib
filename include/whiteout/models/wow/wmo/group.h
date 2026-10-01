// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include <optional>
#include <vector>

#include "root.h"
#include "types.h"

namespace whiteout {
namespace models {
namespace wow {
namespace wmo {

/// @brief `MOGP.flags` (and `MOGI.flags`), as 12.1 reads them.
enum class GroupFlag : u32 {
    Bsp = 0x00000001,
    VertexColors = 0x00000004, ///< The first MOCV is colour set 0.
    Exterior = 0x00000008,
    ExteriorLit = 0x00000040,
    Unreachable = 0x00000080,
    ShowExteriorSky = 0x00000100,
    Lights = 0x00000200,
    /// With the root's Lod flag: the group has LOD files. Also makes every
    /// vertex a transition vertex.
    Lod = 0x00000400,
    Doodads = 0x00000800,
    LiquidSurface = 0x00001000,
    Interior = 0x00002000,
    AlwaysDraw = 0x00010000,
    ShowSkybox = 0x00040000,
    Ocean = 0x00080000, ///< Legacy basic water reads as ocean.
    MountAllowed = 0x00200000,
    Antiportal = 0x04000000,
    NoRender = 0x08000000,
    /// Client-owned: cleared at load.
    ClientOwned = 0x30000000,
};

inline constexpr bool hasFlag(u32 flags, GroupFlag flag) {
    return (flags & static_cast<u32>(flag)) != 0;
}

/// `MOGP.flags2`.
enum class GroupFlag2 : u32 {
    CanCutTerrain = 0x001,
    SplitParent = 0x040,
    SplitChild = 0x080,     ///< Takes its parent's fog and never has liquid.
    AttachmentMesh = 0x100, ///< Draws no batches.
    DetailDoodads = 0x200,  ///< MOGX's key selects an MDDL group.
};

inline constexpr bool hasFlag(u32 flags, GroupFlag2 flag) {
    return (flags & static_cast<u32>(flag)) != 0;
}

/// The 68-byte header of a group file's MOGP chunk.
struct GroupHeader {
    u32 nameOffset = 0;            ///< Into MOGN.
    u32 descriptiveNameOffset = 0; ///< Unread by 12.1.
    u32 flags = 0;                 ///< GroupFlag
    Box bounds;
    u16 portalStart = 0; ///< Into Root::portalRefs.
    u16 portalCount = 0;
    /// The first this-many batches are transition batches. The two counts
    /// after it are unread: interior versus exterior is GroupFlag 0x48.
    u16 transBatchCount = 0;
    u16 intBatchCount = 0;
    u16 extBatchCount = 0;
    u16 pad = 0;
    std::array<u8, 4> fogIds{}; ///< Into Root::fogs; MFVR replaces them.
    u32 groupLiquid = 0;
    u32 uniqueId = 0; ///< WMOAreaTable.WMOGroupID
    u32 flags2 = 0;   ///< GroupFlag2
    /// A split parent's first child, or a split child's parent.
    i16 splitParentOrFirstChild = -1;
    i16 nextSplitChild = -1;
};
static_assert(sizeof(GroupHeader) == 68);

/// One triangle's MPY2 record. A file with MOPY carries the two-byte form; the
/// parser widens it the way the client does (material 0xFF → 0xFFFF).
struct Poly {
    u16 flags = 0;    ///< 0x100: ground type from MOQG
    u16 material = 0; ///< 0xFFFF for a collision-only triangle
};
static_assert(sizeof(Poly) == 4);

/// `MOBA` (and the MOBS / MOPB lists, which share it), 24 bytes.
struct Batch {
    /// Bytes 0 and 1 are the high bytes of minIndex / maxIndex when
    /// `flags & 4`; the rest is an old bounding box nothing reads.
    std::array<u8, 10> head{};
    u16 materialLarge = 0; ///< The material when `flags & 2`.
    u32 startIndex = 0;
    u16 indexCount = 0;
    u16 minIndex = 0;
    u16 maxIndex = 0;
    u8 flags = 0;
    u8 materialSmall = 0;

    u32 material() const {
        return (flags & 2) ? materialLarge : materialSmall;
    }
    u32 firstVertex() const {
        return (flags & 4) ? (static_cast<u32>(head[0]) << 16) | minIndex : minIndex;
    }
    u32 lastVertex() const {
        return (flags & 4) ? (static_cast<u32>(head[1]) << 16) | maxIndex : maxIndex;
    }
};
static_assert(sizeof(Batch) == 24);

/// `MOBN`, 16 bytes.
struct BspNode {
    u16 flags = 0; ///< 0–2 the split axis, 4 a leaf
    i16 negativeChild = -1;
    i16 positiveChild = -1;
    u16 faceCount = 0;
    u32 firstFace = 0; ///< Into Group::bspFaces.
    f32 planeDistance = 0.0f;
};
static_assert(sizeof(BspNode) == 16);

/// One MLIQ vertex: water carries depth and flow bytes, magma a UV.
struct LiquidVertex {
    std::array<u8, 4> data{};
    f32 height = 0.0f;
};
static_assert(sizeof(LiquidVertex) == 8);

/// `MLIQ`: a height grid over one liquid material.
struct Liquid {
    i32 xVertices = 0;
    i32 yVertices = 0;
    i32 xTiles = 0;
    i32 yTiles = 0;
    Vector3f corner{0.0f, 0.0f, 0.0f};
    u16 material = 0;                   ///< Into Root::materials.
    std::vector<LiquidVertex> vertices; ///< `xVertices * yVertices`, rows along x
    std::vector<u8> tiles;              ///< `xTiles * yTiles`; 0x0F bits a legacy type
};

/// `MOTA`: tangents for the window batches (shaders 10 and 14).
struct Tangents {
    /// Per MOBA batch: the batch's first tangent, or 0xFFFF for none.
    std::vector<u16> firstTangent;
    std::vector<Vector4f> tangents;
};

using PointLight = RawRecord<44>;     ///< `MOLP`
using SpotLight = RawRecord<56>;      ///< `MOLS`
using PointLightAnim = RawRecord<96>; ///< `MOP2`
using SpotLightAnim = RawRecord<108>; ///< `MOS2`

/// `MLSP` / `MLSS` / `MLSK` / `MLSO`: a run of the matching light list.
struct LightSetRange {
    u32 offset = 0;
    u32 count = 0;
};
static_assert(sizeof(LightSetRange) == 8);

/// One group file.
struct Group {
    u32 version = 0; ///< MVER; 12.1 never checks it.
    GroupHeader header;

    /// MOGX: the first triangle MOQG covers, and the MDDL group key.
    std::optional<u32> queryFaceStart;
    u8 detailDoodadKey = 0;
    /// MPY2, or MOPY widened to it.
    std::vector<Poly> polys;
    bool polysFromMopy = false;
    std::vector<u32> groundTypes; ///< MOQG

    /// MOVI, or MOVX when the file carries 32-bit indices.
    std::vector<u32> indices;
    bool wideIndices = false;
    std::vector<Vector3f> positions; ///< MOVT
    std::vector<Vector3f> normals;   ///< MONR
    /// Every MOTV in file order. The client keeps the first four.
    std::vector<std::vector<Vector2f>> uvSets;
    /// Every MOCV in file order. Which of them the client puts in which colour
    /// slot depends on the flags; see colorSet0() / colorSet1().
    std::vector<std::vector<Color>> vertexColors;
    std::optional<std::vector<Color>> vertexColors2; ///< MOC2
    std::optional<Tangents> tangents;                ///< MOTA

    std::vector<Batch> batches;        ///< MOBA
    std::vector<Batch> shadowBatches;  ///< MOBS (the client rebuilds it)
    std::vector<Batch> prepassBatches; ///< MOPB

    std::vector<u16> lightRefs;         ///< MOLR, into Root::lights
    std::vector<u16> newLightRefs;      ///< MNLR, into Root::newLights
    std::vector<u16> doodadRefs;        ///< MODR, into Root::doodadDefs
    std::vector<u16> particulateRefs;   ///< MPVR
    std::vector<u16> ambientVolumeRefs; ///< MAVR
    std::vector<u16> ambientBoxRefs;    ///< MBVR
    std::vector<u16> fogRefs;           ///< MFVR; replaces header.fogIds
    std::vector<u16> fogBoxRefs;        ///< MFBR, into Root::fogBoxes

    std::vector<PointLight> pointLights;         ///< MOLP
    std::vector<SpotLight> spotLights;           ///< MOLS
    std::vector<PointLightAnim> pointLightAnims; ///< MOP2
    std::vector<SpotLightAnim> spotLightAnims;   ///< MOS2
    std::vector<LightSetRange> pointLightSets;   ///< MLSP
    std::vector<LightSetRange> spotLightSets;    ///< MLSS
    std::vector<LightSetRange> pointAnimSets;    ///< MLSK
    std::vector<LightSetRange> spotAnimSets;     ///< MLSO

    std::optional<Color> ambientOverride; ///< MDAL
    std::vector<Plane> terrainCutPlanes;  ///< MOPL

    std::optional<Liquid> liquid;  ///< MLIQ
    std::vector<BspNode> bspNodes; ///< MOBN
    std::vector<u16> bspFaces;     ///< MOBR, into polys

    /// MOCV set 0: the first MOCV, when GroupFlag::VertexColors says it is.
    const std::vector<Color>* colorSet0() const;
    /// MOCV set 1: the last MOCV that is not set 0.
    const std::vector<Color>* colorSet1() const;
};

} // namespace wmo
} // namespace wow
} // namespace models
} // namespace whiteout
