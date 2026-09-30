
#pragma once

namespace whiteout {
namespace m2 {

constexpr u32 SKIN_TAG = makeTag("SKIN");

struct SkinSection {
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

/// @brief Bits of the batch's `flags2` word (see Batch::flags2).
enum class BatchFlags2 : u16 {
    /// Drawn as a deferred box decal projected onto the scene, not as geometry.
    Decal = 0x0002,
};

struct Batch {
    u8 flags = 0;
    i8 priorityPlane = 0;
    u16 shaderId = 0;
    u16 skinSectionIndex = 0;
    /// The u16 at +6: a geoset index before version 0x112, which the 12.1 client
    /// zeroes on load, and `flags2` from 0x112 on (Batch::flags2).
    u16 geosetIndex = 0;
    i16 colorIndex = -1;
    u16 materialIndex = 0;
    u16 materialLayer = 0;
    u16 textureCount = 0;
    u16 textureComboIndex = 0;
    u16 textureCoordComboIndex = 0;
    u16 textureWeightComboIndex = 0;
    u16 textureTransformComboIndex = 0;

    /// The +6 word as 12.1 reads it for a file of @p version: `flags2`
    /// (BatchFlags2) from 0x112 on, and 0 before.
    u16 flags2(u32 version) const {
        return version >= 0x112 ? geosetIndex : u16{0};
    }
};

struct ShadowBatch {
    u8 flags = 0;
    u8 flags2 = 0;
    u16 unknown0 = 0;
    u16 submeshId = 0;
    u16 textureId = 0;
    u16 colorId = 0;
    u16 transparencyId = 0;
};

struct SkinProfile {
    std::vector<u16> vertices;
    std::vector<u16> indices;
    std::vector<std::array<u8, 4>> bones;
    std::vector<SkinSection> submeshes;
    std::vector<Batch> batches;

    /// Where this profile's vertex indices start in the model's vertex array.
    /// The client applies it only when GlobalFlag::PerSkinVertexBlocks is set.
    u32 lodVertexBase = 0;

    std::vector<ShadowBatch> shadowBatches;
};

} // namespace m2
} // namespace whiteout
