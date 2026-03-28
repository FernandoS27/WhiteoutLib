
#pragma once

#include <whiteout/models/m2/structures/base.h>
#include <whiteout/models/m2/structures/extensions.h>

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

struct TXACChunk {
    /// Per-material + per-particle texture animation combiner hints.
    /// entries[i] = {textureTransformLookup0, textureTransformLookup1}, values in [0,3].
    /// Count = materials.count + particleEmitters.count.
    std::vector<std::array<u8, 2>> entries;
};

struct PFIDChunk {
    u32 physFileDataId = 0;
};

struct SFIDChunk {
    std::vector<u32> skinFileDataIds;
    std::vector<u32> lodSkinFileDataIds;
};

struct AFIDEntry {
    u16 animId = 0;
    u16 subAnimId = 0;
    u32 fileDataId = 0;
};

struct AFIDChunk {
    std::vector<AFIDEntry> animFileIds;
};

struct BFIDChunk {
    std::vector<u32> boneFileDataIds;
};

struct EXPTEntry {
    f32 zSource = 0.0f;
    f32 colorMult = 0.0f;
    f32 alphaMult = 0.0f;
};

struct EXPTChunk {
    std::vector<EXPTEntry> extendedParticles;
};

struct EXP2Chunk {
    std::vector<ParticleEmitterExtension> emitterExtensions;
    u32 unknownSize = 0;
    u32 unknownOffset = 0;
};

struct PABCChunk {
    std::vector<u16> replacementParentSequenceLookups;
};

struct PADCChunk {
    std::vector<TextureWeight> textureWeights;
};

struct PSBCChunk {
    std::vector<Extent> parentSequenceBounds;
};

struct PEDCChunk {
    std::vector<AnimationTrackBase> parentEventData;
};

struct SKIDChunk {
    u32 skeletonFileDataId = 0;
};

struct TXIDChunk {
    std::vector<u32> textureIds;
};

struct M2RPIDEntry {
    u32 fileDataId = 0;
};

struct M2RPIDChunk {
    std::vector<M2RPIDEntry> recursiveParticleModels;
};

struct GPIDEntry {
    u32 fileDataId = 0;
};

struct GPIDChunk {
    std::vector<GPIDEntry> geometryParticleModels;
};

struct PGD1Chunk {
    std::vector<ParticleGeosetData> particleGeosetData;
};

struct WFV3Chunk {
    WaterfallData data;
};

struct PFDCChunk {
    std::vector<u8> physicsData;
    std::array<u8, 6> padding;
};

struct EDGFChunk {
    std::vector<EdgeFadeData> entries;
};

struct NERFChunk {
    std::vector<DistanceFadeData> entries;
};

struct DETLChunk {
    std::vector<DetailedLightData> records;
};

struct DBOCChunk {
    std::vector<DebugOcclusionData> entries;
};

struct AFRAChunk {
    std::vector<u8> data;
};

struct PCOLChunk : PhysicsCollision {};

struct DPIVChunk {
    std::array<u8, 32> data = {0};
};

struct TEXLChunk {
    std::vector<TexturedLightData> texturedLights;
};

}
}
