
#pragma once

#include "../../../compatibility.h"
#include "../types.h"

namespace whiteout {
namespace m2 {

struct ParticleEmitterExtension {
    f32 zSource = 0.0f;
    f32 colorMult = 0.0f;
    f32 alphaMult = 0.0f;
    ParticleAnimationTrack<unorm16> alphaCutoff;
};

struct LodProfile {
    u16 flags = 0; // bit 1: alt particleBoneLod, bit 2: lodBatchCount > 0, bit 3: lodScaleRaw is live
    u16 numLodLevels = 0;                   // SFID_count / numSkinProfiles
    f32 lodDistance = 0.0f;                 // LOD switch distance: fmaxf(fminf(740/dist, 5), 0.5)
    /// Per-LOD particle bone mask index. Only four are stored, but the client
    /// drives up to eight LOD levels: it copies [0..3] into its first four
    /// slots and then replicates entry [3] into slots 4..7.
    std::array<u8, 4> particleBoneLod = {};
    /// Fixed-point LOD scale, applied as `lodScaleRaw / 2048.0` and only when
    /// `flags & 0x08` is set (otherwise the client uses 1.0). This is the pair
    /// of bytes previously read as `reserved0` + `lodFlags`; reading it as a
    /// u16 explains why the high byte looked like a mirror of flags bit 3,
    /// since 0x0800 / 2048 == 1.0.
    u16 lodScaleRaw = 0;
    u8 lodBatchCount = 0; // unknown count, nonzero iff flags & 0x04
    u8 reserved1 = 0;

    /// LOD scale as the client computes it for these @ref flags.
    f32 lodScale() const {
        return (flags & 0x08) != 0 ? static_cast<f32>(lodScaleRaw) / 2048.0f : 1.0f;
    }
    /// Number of LOD levels the client will actually use (it clamps to 8).
    u16 effectiveLodLevels() const { return numLodLevels > 8 ? u16(8) : numLodLevels; }
};

// ── Chunk data types (shared between Model and chunk wrappers) ──────────────

struct WaterfallData {
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

struct ParticleGeosetData {
    u16 geoset = 0;
};

struct EdgeFadeData {
    std::array<f32, 2> value0 = {0.0f, 0.0f};
    f32 value8 = 0.0f;
    std::array<u8, 0xC> valueC = {0};
};

struct DistanceFadeData {
    f32 squaredFarDist;          // squared distance at which alpha = 0 (fade-out complete)
    f32 squaredNearDist;         // squared distance at which alpha = 1 (fully visible)
    std::array<u32, 2> reserved; // always 0
};

struct DetailedLightData {
    u16 flags = 0;
    f16 scale = 0;
    f16 diffuseColorMultiplier = 0;
    u16 unknown0 = 0;
    u32 unknown1 = 0;
};

struct DebugOcclusionData {
    f32 unknown1_1 = 0.0f;
    f32 unknown1_2 = 0.0f;
    u32 unknown1_3 = 0;
    u32 unknown1_4 = 0;
};

struct TexturedLightData {
    f32 unknown0 = 0.0f;
    f32 unknown1 = 0.0f;
    i32 textureLookup = -1;
    i32 unknown2 = 0;
};

/// One DPIV (pivot displacement) record, 32 bytes.
///
/// The client keeps the payload pointer and a record count of `chunkSize / 32`,
/// so a chunk holds N of these — the corpus has both 1- and 2-record chunks.
struct PivotDisplacementData {
    Vector3f offset;                    ///< small displacement; Z varies most
    u32 flags = 0;                      ///< 0 or 1
    std::array<u32, 4> reserved = {};   ///< always 0
};

struct PhysicsCollision {
    std::vector<Vector3f> vertexPositions;
    std::vector<Vector3f> faceNormals;
    std::vector<i16> indices;
    std::vector<i16> flags;
};

} // namespace m2
} // namespace whiteout
