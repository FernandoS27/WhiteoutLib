#pragma once

#include "m2_base.h"

namespace whiteout {
namespace m2 {

constexpr u32 SKIN_TAG = makeTag("SKIN");

struct M2SkinSection {
    u16 skinSectionId = 0;
    u16 level = 0;
    u16 vertexStart = 0;
    u16 vertexCount = 0;
    u16 indexStart = 0;
    u16 indexCount = 0;
    u16 boneCount = 0;
    u16 boneComboIndex = 0;
    u16 boneInfluences = 0;
    u16 centerBoneIndex = 0;
    Vector3f centerPosition;
    Vector3f sortCenterPosition;
    f32 sortRadius = 0.0f;
};

struct M2Batch {
    u8 flags = 0;
    i8 priorityPlane = 0;
    u16 shaderId = 0;
    u16 skinSectionIndex = 0;
    u16 geosetIndex = 0;
    i16 colorIndex = -1;
    u16 materialIndex = 0;
    u16 materialLayer = 0;
    u16 textureCount = 0;
    u16 textureComboIndex = 0;
    u16 textureCoordComboIndex = 0;
    u16 textureWeightComboIndex = 0;
    u16 textureTransformComboIndex = 0;
};

struct M2ShadowBatch {
    u8 flags = 0;
    u8 flags2 = 0;
    u16 unknown0 = 0;
    u16 submeshId = 0;
    u16 textureId = 0;
    u16 colorId = 0;
    u16 transparencyId = 0;
};

struct M2SkinProfile {
    u32 magic = SKIN_TAG;

    std::vector<u16> vertices;
    std::vector<u16> indices;
    std::vector<std::array<u8, 4>> bones;
    std::vector<M2SkinSection> submeshes;
    std::vector<M2Batch> batches;

    u32 boneCountMax = 0;
    u32 lodVertexBase = 0;

    std::vector<M2ShadowBatch> shadowBatches;
};

} // namespace m2
} // namespace whiteout
