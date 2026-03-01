// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include "m2_base.h"

namespace whiteout {
namespace m2 {

constexpr u32 PFID_TAG = makeTag("PFID");
constexpr u32 SFID_TAG = makeTag("SFID");
constexpr u32 AFID_TAG = makeTag("AFID");
constexpr u32 BFID_TAG = makeTag("BFID");
constexpr u32 TXAC_TAG = makeTag("TXAC");
constexpr u32 EXPT_TAG = makeTag("EXPT");
constexpr u32 EXP2_TAG = makeTag("EXP2");
constexpr u32 PABC_TAG = makeTag("PABC");
constexpr u32 PADC_TAG = makeTag("PADC");
constexpr u32 PSBC_TAG = makeTag("PSBC");
constexpr u32 PEDC_TAG = makeTag("PEDC");
constexpr u32 SKID_TAG = makeTag("SKID");
constexpr u32 TXID_TAG = makeTag("TXID");
constexpr u32 LDV1_TAG = makeTag("LDV1");
constexpr u32 RPID_TAG = makeTag("RPID");
constexpr u32 GPID_TAG = makeTag("GPID");
constexpr u32 WFV1_TAG = makeTag("WFV1");
constexpr u32 WFV2_TAG = makeTag("WFV2");
constexpr u32 WFV3_TAG = makeTag("WFV3");
constexpr u32 PFDC_TAG = makeTag("PFDC");
constexpr u32 EDGF_TAG = makeTag("EDGF");
constexpr u32 NERF_TAG = makeTag("NERF");
constexpr u32 DETL_TAG = makeTag("DETL");
constexpr u32 DBOC_TAG = makeTag("DBOC");
constexpr u32 AFRA_TAG = makeTag("AFRA");
constexpr u32 PCOL_TAG = makeTag("PCOL");
constexpr u32 DPIV_TAG = makeTag("DPIV");
constexpr u32 TEXL_TAG = makeTag("TEXL");
constexpr u32 PGD1_TAG = makeTag("PGD1");

struct M2TXACChunk {
    std::vector<std::array<u8, 2>> unknown;
};

struct M2PFIDChunk {
    u32 physFileDataId = 0;
};

struct M2SFIDChunk {
    std::vector<u32> skinFileDataIds;
    std::vector<u32> lodSkinFileDataIds;
};

struct M2AFIDEntry {
    u16 animId = 0;
    u16 subAnimId = 0;
    u32 fileDataId = 0;
};

struct M2AFIDChunk {
    std::vector<M2AFIDEntry> animFileIds;
};

struct M2BFIDChunk {
    std::vector<u32> boneFileDataIds;
};

struct M2EXPTEntry {
    f32 zSource = 0.0f;
    f32 colorMult = 0.0f;
    f32 alphaMult = 0.0f;
};

struct M2EXPTChunk {
    std::vector<M2EXPTEntry> extendedParticles;
};

struct M2EXP2Particle {
    f32 zSource = 0.0f;
    f32 colorMult = 0.0f;
    f32 alphaMult = 0.0f;
    AnimationTrack<i16> alphaCutoff;
};

struct M2EXP2Chunk {
    std::vector<M2EXP2Particle> content;
};

struct M2PABCChunk {
    std::vector<u16> replacementParentSequenceLookups;
};

struct M2PADCChunk {
    std::vector<TextureWeight> textureWeights;
};

struct M2SequenceBounds {
    Extent extent;
    f32 radius = 0.0f;
};

struct M2PSBCChunk {
    std::vector<M2SequenceBounds> parentSequenceBounds;
};

struct M2PEDCChunk {
    std::vector<AnimationTrackBase> parentEventData;
};

struct M2SKIDChunk {
    u32 skeletonFileDataId = 0;
};

struct M2TXIDEntry {
    u32 fileDataId = 0;
};

struct M2TXIDChunk {
    std::vector<M2TXIDEntry> textureIds;
};

struct M2LDV1Chunk {
    u16 unknown0 = 0;
    u16 lodCount = 0;
    f32 unknown2 = 0.0f;
    std::array<u8, 4> particleBoneLod = {0, 0, 0, 0};
    u32 unknown4 = 0;
};

struct M2RPIDEntry {
    u32 fileDataId = 0;
};

struct M2RPIDChunk {
    std::vector<M2RPIDEntry> recursiveParticleModels;
};

struct M2GPIDEntry {
    u32 fileDataId = 0;
};

struct M2GPIDChunk {
    std::vector<M2GPIDEntry> geometryParticleModels;
};

struct M2WFV1Chunk {
};

struct M2WFV2Chunk {
};

struct M2PGD1Entry {
    u16 geoset = 0;
};

struct M2PGD1Chunk {
    std::vector<M2PGD1Entry> particleGeosetData;
};

struct M2WFV3Data {
    f32 bumpScale = 0.0f;
    f32 value0_x = 0.0f;
    f32 value0_y = 0.0f;
    f32 value0_z = 0.0f;
    f32 value1_w = 0.0f;
    f32 value0_w = 0.0f;
    f32 value1_x = 0.0f;
    f32 value1_y = 0.0f;
    f32 value2_w = 0.0f;
    f32 value3_y = 0.0f;
    f32 value3_x = 0.0f;
    Vector4f baseColor;
    u16 flags = 0;
    u16 unknown0 = 0;
    f32 value3_w = 0.0f;
    f32 value3_z = 0.0f;
    f32 value4_y = 0.0f;
    f32 unknown1 = 0.0f;
    f32 unknown2 = 0.0f;
    f32 unknown3 = 0.0f;
    f32 unknown4 = 0.0f;
};

struct M2WFV3Chunk {
    M2WFV3Data data;
};

struct M2PFDCChunk {
    std::vector<u8> physicsData;
    std::array<u8, 6> padding;
};

struct M2EDGFEntry {
    std::array<f32, 2> value0 = {0.0f, 0.0f};
    f32 value8 = 0.0f;
    std::array<u8, 0xC> valueC = {0};
};

struct M2EDGFChunk {
    std::vector<M2EDGFEntry> entries;
};

struct M2NERFEntry {
    Vector2f coefs;
};

struct M2NERFChunk {
    std::vector<M2NERFEntry> entries;
};

struct M2DETLEntry {
    u16 flags = 0;
    f16 scale = 0;
    f16 diffuseColorMultiplier = 0;
    u16 unknown0 = 0;
    u32 unknown1 = 0;
};

struct M2DETLChunk {
    std::vector<M2DETLEntry> records;
};

struct M2DBOCEntry {
    f32 unknown1_1 = 0.0f;
    f32 unknown1_2 = 0.0f;
    u32 unknown1_3 = 0;
    u32 unknown1_4 = 0;
};

struct M2DBOCChunk {
    std::vector<M2DBOCEntry> entries;
};

struct M2AFRAChunk {
    std::vector<u8> data;
};

struct M2PCOLChunk {
    std::vector<Vector3f> vertexPositions;
    std::vector<Vector3f> faceNormals;
    std::vector<i16> indices;
    std::vector<i16> flags;
};

struct M2DPIVChunk {
    std::array<u8, 32> data = {0};
};

struct M2TEXLEntry {
    f32 unknown0 = 0.0f;
    f32 unknown1 = 0.0f;
    i32 textureLookup = -1;
    i32 unknown2 = 0;
};

struct M2TEXLChunk {
    std::vector<M2TEXLEntry> texturedLights;
};

} // namespace m2
} // namespace whiteout
