
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

/// @brief The SKPD parent of a child `.skel`: a second sequence table.
///
/// The 12.1 client evaluates the child's own bones, attachments and global
/// loops, and plays from this table an animation the child's sequences lack
/// (and the model's PABC list does not block). Such a play samples these bones'
/// tracks by the CHILD's bone index; their pivots and parents are never read.
struct ParentSkeleton {
    u32 fileId = 0;      ///< SKPD's FileDataID.
    bool loaded = false; ///< False until loadParentSkeleton() has read it.
    std::vector<Sequence> sequences;
    std::vector<u16> sequenceIdxHashById;
    std::vector<Bone> bones;
    /// @brief Set only by a lazy load; see sequence_loader.h.
    std::shared_ptr<SequenceLoader> sequenceLoader;
};

struct Model {
    /// The MD20 version the file carried; 0 for a model built in memory. Some
    /// fields change meaning by version (Batch::flags2).
    u32 fileVersion = 0;
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
    /// Header +0x88. With GlobalFlag::TextureTransformsUsesBoneSequences,
    /// texture transform `i` runs on the clock of bone `[i]`; the 12.1 client
    /// reads it for nothing else (older tools called it `textureCoordCombos`).
    std::vector<u16> textureTransformBoneMap;
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

    /// Present only in files of version 271 and below with
    /// GlobalFlag::UseTextureCombinerCombos.
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
    std::vector<DepthBasedOpacityData> depthBasedOpacityEntries; ///< DBOC
    std::vector<u8> animFrameData;                         ///< AFRA
    std::optional<PhysicsCollision> physicsCollision;      ///< PCOL
    std::vector<PivotDisplacementData> dpivData;           ///< DPIV (32 B per record)
    std::vector<TexturedLightData> texturedLightEntries;   ///< TEXL

    /// @brief Set only by a lazy parse: what the deferred `.anim` siblings are
    ///        read through. See sequence_loader.h.
    ///
    /// @bind skip — an opaque handle, not model data.
    ///
    /// Shared, not owned: copying a Model shares the loader, and loading a
    /// sequence fills in whichever copy is passed to loadSequence().
    std::shared_ptr<SequenceLoader> sequenceLoader;

    /// @brief The `.skel`'s SKPD parent, when it names one.
    ///
    /// @bind skip — read through loadParentSkeleton() / loadParentSequence().
    std::optional<ParentSkeleton> parentSkeleton;
};

enum class Format : u32 {
    ClassicMD20 = 0,
    LegionMD21 = 1,

    Invalid = std::numeric_limits<u32>::max(),
};

} // namespace m2
} // namespace whiteout
