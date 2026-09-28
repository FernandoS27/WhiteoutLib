// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

// G-V (WEM_PHYSICS_DESIGN.md §11): the chunk table of the StarCraft II 5.0
// client, every tag it loads at its current version and element size.
// Versions from running `M3_ValidateChunkVersions` (0x102c64a80) under
// Unicorn over versions 0-63; sizes and names from `M3_EnumChunkDescriptors`
// (0x102c658f0). An older version is accepted and upgraded at load; a newer
// one refuses the file.

#include <whiteout/common_types.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace m3client {

struct ChunkEntry {
    whiteout::u32 tag;     ///< As the index stores it: the first character in the high byte
    whiteout::u32 version; ///< Current
    whiteout::u32 size;    ///< Element size at the current version
};

inline constexpr std::array<ChunkEntry, 111> kChunks{{
    {0x00434F4C,  0,    4}, // \0COL CColor
    {0x4154545F,  1,   20}, // ATT_ SAttachmentData
    {0x4154564C,  0,  116}, // ATVL SAttachmentVolumeData
    {0x4241545F,  1,   14}, // BAT_ SModelBatchData
    {0x42425343,  0,   48}, // BBSC SBBSolverData
    {0x424E4453,  0,   28}, // BNDS SModelBoundsData
    {0x424F4E45,  1,  160}, // BONE SBoneData
    {0x42534554,  0,   32}, // BSET SAnimSetData
    {0x43414D5F,  5,  264}, // CAM_ SCameraData
    {0x43484152,  0,    1}, // CHAR char
    {0x434C4344,  0,  112}, // CLCD SCollisionCacheData
    {0x434C5344,  0,   56}, // CLSD SCollisionSharedData
    {0x434D505F,  2,   28}, // CMP_ SCompositeMaterialData
    {0x434D535F,  0,   24}, // CMS_ SCompositeSubMaterialData
    {0x43524550,  1,   28}, // CREP SCreepMaterialData
    {0x4449535F,  4,   68}, // DIS_ SDisplacementMaterialData
    {0x4449565F,  2,   52}, // DIV_ SModelSkinProfileData
    {0x444D4D45,  0,   20}, // DMME SMeshEdge
    {0x444D4D4E,  1,    8}, // DMMN SMeshNode
    {0x444D4D54,  0,   28}, // DMMT SMeshTriangle_ver2
    {0x444D5345,  0,    4}, // DMSE SSubEdge
    {0x45564E54,  2,  108}, // EVNT SEventData
    {0x464C4147,  0,    4}, // FLAG bool32
    {0x464F525F,  2,  104}, // FOR_ SForceFieldData
    {0x4841495F,  0,  116}, // HAI_ SHairMaterialData
    {0x48554C4C,  0,   80}, // HULL SPolytopeData
    {0x4931365F,  0,    2}, // I16_ int16
    {0x4933325F,  0,    4}, // I32_ int32
    {0x4936345F,  0,    8}, // I64_ int64
    {0x494B324A,  0,   48}, // IK2J STwoJointsIKSolverData
    {0x494B4343,  0,   24}, // IKCC SCCDIKSolverData
    {0x494B4A54,  0,   32}, // IKJT SJTIKSolverData
    {0x49524546,  0,   64}, // IREF SRefPoseData
    {0x4C415952, 26,  464}, // LAYR SMaterialLayerData
    {0x4C464C52,  3,  152}, // LFLR SLensFlareMaterialData
    {0x4C465342,  2,   56}, // LFSB SLensFlareSubData
    {0x4C495445,  7,  212}, // LITE SLightData
    {0x4C4F4F50,  0,    4}, // LOOP SAnimLoopData
    {0x4D41544D,  0,    8}, // MATM SMaterialMappingData
    {0x4D41545F, 20,  352}, // MAT_ SMaterialData
    {0x4D443334, 11,   24}, // MD34 SModelRootData
    {0x4D455348,  1,  116}, // MESH SMeshData
    {0x4D4F444C, 29,  856}, // MODL SModelData
    {0x4D534543,  1,   72}, // MSEC SModelSectionData
    {0x4D543136,  0,   14}, // MT16 SMeshTriangle16
    {0x4D543332,  0,   28}, // MT32 SMeshTriangle32
    {0x50414F42,  0,   24}, // PAOB SOneBoneSolverData
    {0x50415243,  0,   40}, // PARC SParticleCopiedSysData
    {0x5041525F, 24, 1496}, // PAR_ SParticleSysData
    {0x50415455,  4,  152}, // PATU STurretSolverData
    {0x50484143,  0,   32}, // PHAC SPhysicsAttachedClothData
    {0x50484343,  0,   76}, // PHCC SPhysicsClothCollider
    {0x5048434C,  4,  192}, // PHCL SPhysicsClothData
    {0x50484353,  0,   68}, // PHCS SPhysicsConstraintSpace
    {0x50484354,  0,  180}, // PHCT SPhysicsConstraintData
    {0x50485242,  4,   80}, // PHRB SPhysicsBodyData
    {0x50485348,  3,  300}, // PHSH SPhysicsShapeData
    {0x5048594A,  0,  180}, // PHYJ SPhysicsJointData
    {0x50524F4A,  5,  388}, // PROJ SMaterialProjectorData
    {0x51554154,  0,   16}, // QUAT C4Quaternion
    {0x5245414C,  0,    4}, // REAL real32
    {0x5245465F,  2,  156}, // REF_ SReflectionMaterialData
    {0x5245474E,  5,   48}, // REGN SModelSkinSectionData
    {0x5249425F,  9,  760}, // RIB_ SRibbonData
    {0x524E4749,  0,    8}, // RNGI CiRange
    {0x5343445F,  0,    8}, // SCD_ SStartCountData
    {0x53434852,  0,   12}, // SCHR SM2Array<char>
    {0x53443256,  0,   32}, // SD2V SSequenceData<C2Vector>
    {0x53443356,  0,   32}, // SD3V SSequenceData<C3Vector>
    {0x53443451,  0,   32}, // SD4Q SSequenceData<C4Quaternion>
    {0x53444343,  0,   32}, // SDCC SSequenceData<CColor>
    {0x53444556,  0,   32}, // SDEV SSequenceData<SEventData>
    {0x53444647,  0,   32}, // SDFG SSequenceData<bool32>
    {0x53444D42,  0,   32}, // SDMB SSequenceData<SModelBoundsData>
    {0x53445233,  0,   32}, // SDR3 SSequenceData<real32>
    {0x53445333,  0,   32}, // SDS3 SSequenceData<int32>
    {0x53445336,  0,   32}, // SDS6 SSequenceData<int16>
    {0x53445533,  0,   32}, // SDU3 SSequenceData<uint32>
    {0x53445536,  0,   32}, // SDU6 SSequenceData<uint16>
    {0x53445538,  0,   32}, // SDU8 SSequenceData<uint8>
    {0x53455153,  2,   92}, // SEQS SAnimSequenceData
    {0x53484258,  0,   64}, // SHBX SShadowBoxData
    {0x5350524C,  0,   12}, // SPRL SSplineKey<real32>
    {0x53505633,  0,   36}, // SPV3 SSplineKey<C3Vector>
    {0x53523332,  0,   20}, // SR32 STrack<real32>
    {0x53524942,  0,  272}, // SRIB SSplineRibbonData
    {0x53534753,  1,  108}, // SSGS SGeometryShapeData
    {0x5354424D,  0,   48}, // STBM SSplatTerrainBakeMaterialData
    {0x5354435F,  4,  204}, // STC_ STrackCollection
    {0x5354475F,  0,   24}, // STG_ SAnimSequenceGroup
    {0x5354535F,  0,   28}, // STS_ SAnimTrackSet
    {0x53564333,  0,   36}, // SVC3 STrack<C3Vector>
    {0x5445525F,  1,   28}, // TER_ STerrainMaterialData
    {0x54455841,  0,   20}, // TEXA STextureAlphaData
    {0x54455846,  0,   24}, // TEXF STextureFlipbookData
    {0x54455856,  1,   60}, // TEXV STextureAVIData
    {0x5445585F,  0,   16}, // TEX_ STextureData
    {0x544D445F,  1,   68}, // TMD_ STreadMarkData
    {0x54524744,  0,   24}, // TRGD STurretGroupData
    {0x5531365F,  0,    2}, // U16_ uint16
    {0x5533325F,  0,    4}, // U32_ uint32
    {0x5536345F,  0,    8}, // U64_ uint64
    {0x55385F5F,  0,    1}, // U8__ uint8
    {0x5542345F,  0,    4}, // UB4_ ubyte4
    {0x56454332,  0,    8}, // VEC2 C2Vector
    {0x56454333,  0,   12}, // VEC3 C3Vector
    {0x56454334,  0,   16}, // VEC4 C4Vector
    {0x564F4C5F,  0,   84}, // VOL_ SVolumeMaterialUniformData
    {0x564F4E5F,  0,  268}, // VON_ SVolumeMaterialNoisyData
    {0x56564F4C,  0,   40}, // VVOL SViewVolumeData
    {0x5752505F,  1,  132}, // WRP_ SVertexWarpData
}};

inline const ChunkEntry* Find(whiteout::u32 tag) {
    for (const ChunkEntry& entry : kChunks) {
        if (entry.tag == tag) {
            return &entry;
        }
    }
    return nullptr;
}

constexpr whiteout::u32 Tag(const char (&name)[5]) {
    return static_cast<whiteout::u32>(static_cast<whiteout::u8>(name[0])) << 24 |
           static_cast<whiteout::u32>(static_cast<whiteout::u8>(name[1])) << 16 |
           static_cast<whiteout::u32>(static_cast<whiteout::u8>(name[2])) << 8 |
           static_cast<whiteout::u32>(static_cast<whiteout::u8>(name[3]));
}

/// The chunks a Heroes of the Storm file shares with StarCraft II's physics.
inline bool IsPhysics(whiteout::u32 tag) {
    constexpr std::array<whiteout::u32, 12> kPhysics{Tag("PHRB"), Tag("PHSH"), Tag("PHCL"), Tag("FOR_"),
                                                     Tag("WRP_"), Tag("PHYJ"), Tag("PHCC"), Tag("PHAC"),
                                                     Tag("DMSE"), Tag("DMMN"), Tag("MT16"), Tag("MT32")};
    return std::find(kPhysics.begin(), kPhysics.end(), tag) != kPhysics.end();
}

inline std::string TagName(whiteout::u32 tag) {
    std::string out;
    for (int shift = 24; shift >= 0; shift -= 8) {
        const char c = static_cast<char>((tag >> shift) & 0xFFu);
        out += c == 0 ? '0' : c;
    }
    return out;
}

struct Report {
    std::vector<std::string> problems; ///< What the client would refuse or misread
    std::vector<std::string> unknown;  ///< Tags this table has no row for
};

/// @p file against the table: no chunk above the client's version, every
/// physics chunk at it, and a chunk at it spanning its count of elements plus
/// under 16 bytes of padding. @p all checks every tag; a Heroes file only the
/// physics ones, the rest of its set being its own game's.
inline Report Check(std::span<const whiteout::u8> file, bool all) {
    Report report;
    const auto u32At = [&](std::size_t at) {
        whiteout::u32 v = 0;
        if (at + 4 <= file.size()) {
            std::memcpy(&v, file.data() + at, 4);
        }
        return v;
    };
    const whiteout::u32 indexOffset = u32At(4);
    const whiteout::u32 indexCount = u32At(8);
    std::vector<whiteout::u32> starts;
    for (whiteout::u32 i = 0; i < indexCount; ++i) {
        starts.push_back(u32At(indexOffset + 16u * i + 4));
    }
    starts.push_back(indexOffset);
    starts.push_back(static_cast<whiteout::u32>(file.size()));
    std::sort(starts.begin(), starts.end());
    for (whiteout::u32 i = 0; i < indexCount; ++i) {
        const std::size_t at = indexOffset + 16u * i;
        const whiteout::u32 tag = u32At(at), offset = u32At(at + 4), count = u32At(at + 8), version = u32At(at + 12);
        if (!all && !IsPhysics(tag)) {
            continue;
        }
        const ChunkEntry* entry = Find(tag);
        const std::string name = TagName(tag) + " v" + std::to_string(version);
        if (entry == nullptr) {
            report.unknown.push_back(TagName(tag));
            continue;
        }
        if (version > entry->version) {
            report.problems.push_back(name + ": above the client's v" + std::to_string(entry->version));
        } else if (version < entry->version && IsPhysics(tag)) {
            report.problems.push_back(name + ": physics not written current");
        } else if (version == entry->version && count != 0) {
            const whiteout::u32 end = *std::upper_bound(starts.begin(), starts.end(), offset);
            const std::size_t used = static_cast<std::size_t>(count) * entry->size;
            if (end < offset || end - offset < used || end - offset - used >= 16) {
                report.problems.push_back(name + ": " + std::to_string(count) + " elements in " +
                                          std::to_string(end - offset) + " bytes, the client's are " +
                                          std::to_string(entry->size));
            }
        }
    }
    return report;
}

} // namespace m3client
