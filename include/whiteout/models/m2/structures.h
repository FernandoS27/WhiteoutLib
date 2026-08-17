
#pragma once

#include "structures/base.h"
#include "structures/bone_overrides.h"
#include "structures/extensions.h"
#include "structures/phys.h"
#include "structures/skin.h"
#include "types.h"

#include <limits>
#include <memory>
#include "../../compatibility.h"

namespace whiteout {
namespace m2 {

class SequenceLoader;

struct Model {
    std::string modelName;
    GlobalFlags globalFlags;

    std::vector<GlobalSequence> globalLoops;
    std::vector<Sequence> sequences;
    std::vector<u16> sequenceIdxHashById;

    std::vector<Bone> bones;
    std::vector<u16> keyBoneIds;

    std::vector<Vertex> vertices;
    std::vector<SkinProfile> skinProfiles;
    std::vector<SkinProfile> lodProfiles;
    u32 numSkinProfiles = 0;

    std::vector<ColorAnimation> colors;
    std::vector<Texture> textures;
    std::vector<TextureWeight> textureWeights;
    std::vector<TextureTransform> textureTransforms;
    std::vector<u16> textureIndicesById;
    std::vector<Material> materials;

    std::vector<u16> boneCombos;
    std::vector<u16> textureCombos;
    std::vector<u16> textureCoordCombos;
    std::vector<u16> textureWeightCombos;
    std::vector<u16> textureTransformCombos;

    Extent bounding;
    Extent collision;
    std::vector<u16> collisionTriangleIndices;
    std::vector<Vector3f> collisionVertices;
    std::vector<Vector3f> collisionFaceNormals;

    std::vector<Attachment> attachments;
    std::vector<u16> attachmentIndicesById;
    std::vector<Event> events;
    std::vector<Light> lights;
    std::vector<Camera> cameras;
    std::vector<u16> cameraIndicesById;

    std::vector<RibbonEmitter> ribbonEmitters;
    std::vector<ParticleEmitter> particleEmitters;

    std::vector<u16> textureCombinerCombos;

    // ── Pre-WotLK (≤263) header arrays, absent in later versions ──────────

    /// One 4-byte record per animation id; vanilla/BC clients use it to pick
    /// fallback animations. Preserved verbatim so old files round-trip.
    std::vector<u32> playableAnimationLookup;
    /// "Texture flipbooks" — unused even by old clients, preserved verbatim.
    std::vector<u16> textureFlipbooks;

    // ── Chunk extension data ──────────

    std::vector<u32> texture_ids;                        ///< TXID
    std::optional<LodProfile> lodProfile;                ///< LDV1
    std::vector<std::array<u8, 2>> textureCombinerHints; ///< TXAC
    std::vector<u16> parentSequenceReplacements;         ///< PABC
    std::vector<TextureWeight> parentTextureWeights;     ///< PADC
    std::vector<Extent> parentSequenceBounds;            ///< PSBC
    std::vector<AnimationTrackBase> parentEventData;     ///< PEDC
    std::vector<u32> recursiveParticleModelIds;          ///< RPID
    std::vector<u32> geometryParticleModelIds;           ///< GPID
    std::optional<WaterfallData> waterData;              ///< WFV3
    std::vector<ParticleGeosetData> particleGeosets;     ///< PGD1
    std::optional<PhysicsData> physics;                  ///< PFDC, or a `.phys` sibling
    std::optional<u32> physicsFileId;
    /// One entry per customization choice, in BFID order — see bone_file.h.
    std::vector<BoneOverrideSet> boneOverrides;
    /// BFID, kept so a model that named its `.bone` files by id keeps doing so.
    /// Parallel to @ref boneOverrides when both are present.
    std::vector<u32> boneFileIds;
    std::vector<EdgeFadeData> edgeFadeEntries;             ///< EDGF
    std::vector<DistanceFadeData> nerfEntries;             ///< NERF
    std::vector<DetailedLightData> detailedLightEntries;   ///< DETL
    std::vector<DebugOcclusionData> debugOcclusionEntries; ///< DBOC
    std::vector<u8> animFrameData;                         ///< AFRA
    std::optional<PhysicsCollision> physicsCollision;      ///< PCOL
    std::optional<std::array<u8, 32>> dpivData;            ///< DPIV
    std::vector<TexturedLightData> texturedLightEntries;   ///< TEXL

    /// @brief Set only by a lazy parse: what the deferred `.anim` siblings are
    ///        read through. See sequence_loader.h.
    ///
    /// @bind skip — an opaque handle, not model data.
    ///
    /// Shared, not owned: copying a Model shares the loader, and loading a
    /// sequence fills in whichever copy is passed to loadSequence().
    std::shared_ptr<SequenceLoader> sequenceLoader;
};

enum class Format : u32 {
    ClassicMD20 = 0,
    LegionMD21 = 1,

    Invalid = std::numeric_limits<u32>::max(),
};

} // namespace m2
} // namespace whiteout
