
#pragma once

#include "../../../compatibility.h"
#include "../types.h"
#include "extensions.h"
#include "skin.h"

namespace whiteout {
namespace m2 {

/// @brief The MD20 header's `globalFlags`, named for what WoW 12.1 does with
///        each bit (`WOW_M2_FLAGS.md`, corrected where noted).
enum class GlobalFlag : u32 {
    None = 0,
    /// The model leans to follow the ground normal, about X and about Y.
    TiltX = 0x00000001,
    TiltY = 0x00000002,
    /// The world transform is taken as is: no attachment parent's scale or
    /// translation is composed in, and emitters do not inherit it.
    WorldAbsoluteTransform = 0x00000004,
    /// The header carries `textureCombinerCombos` after its fixed part. Read by
    /// the parser; the 12.1 client never tests it.
    UseTextureCombinerCombos = 0x00000008,
    /// Batch bounds and sort distance come from the bone-transformed geometry.
    AnimatedBounds = 0x00000010,
    /// Load physics when the model attaches to a scene. CM2Shared::FinishLoadingM2Data
    /// tests it before LegacyLoadPhysData.
    LoadPhysicsData = 0x00000020,
    /// Enters the visible-geometry optimiser and the shadow-map gather.
    VisibleGeometryOptimise = 0x00000080,
    /// Emitters of record type 4 are relinked when the model attaches to a parent.
    ParentLinkedParticles = 0x00000100,
    /// Files of version 271 and below: the particle record carries the 16-byte
    /// multi-texture scroll tail. 12.1 reads the bit on helmets instead, as
    /// "has per-race rows in HelmetAnimScaling" (bone flag HelmetAnimScaled).
    NewParticleRecord = 0x00000200,
    Unk_0x400 = 0x00000400,
    /// Texture transforms are driven by bone sequences through textureTransformBoneMap.
    TextureTransformsUsesBoneSequences = 0x00000800,
    Unk_0x1000 = 0x00001000,
    /// Each skin profile owns a slice of the vertex array starting at its
    /// SkinProfile::lodVertexBase; clear, every profile indexes from 0.
    PerSkinVertexBlocks = 0x00002000,
    /// A skinned attachment posed by its parent model: the client rebinds its
    /// bones to the parent skeleton by Bone::boneNameCRC.
    ParentSkeletonBound = 0x00004000,
    /// Point lights take their attenuation start and end from their tracks;
    /// clear, the client uses 1.6666 and 5.2666 times the model scale.
    /// (WOW_M2_FLAGS.md reads this as a ribbon bit: the 156-byte record it
    /// describes is the light.)
    LightAttenuationTracks = 0x00008000,
    /// Ribbons resolve their textureTransformIndex through
    /// textureTransformCombos; clear, the index is ignored. (WOW_M2_FLAGS.md
    /// reads this as a particle bit: the 176-byte record is the ribbon.)
    RibbonTextureTransforms = 0x00020000,
    /// In the bone-wind system: the palette entries of every bone but the root
    /// carry wind amplitude and phase for a wind vertex-shader permutation.
    BoneWind = 0x00040000,
    /// Sequences and bones come from the SKID `.skel`, not the header.
    ExternalSkeleton = 0x00100000,
    /// External `.anim` files are a chunk stream whose AFM2 chunk holds the
    /// sequence data; clear, the whole file is sequence data.
    ChunkedAnimAfm2 = 0x00200000,
    /// Sets a texture-creation flag nothing in the 12.1 client reads.
    NamedTextureRequestInert = 0x00800000,
    /// Ignore the PFID physics file, whatever the model carries.
    SuppressPhysicsFile = 0x01000000,
    /// Skip the HiZ occlusion test.
    SkipOcclusionQuery = 0x02000000,
    /// Treat the model as visible without querying occlusion.
    ForceUnoccluded = 0x04000000,
    /// Patches two bytes of the M2 render-state word; the bytes' meaning is
    /// not resolved.
    PipelineStateOverride = 0x08000000,
    /// Past a LOD threshold the model swaps its link record and releases its textures.
    FarLodLinkSubstitute = 0x10000000,
    /// Picks which of two render-state words a secondary pass draws the model with.
    SecondaryPassRenderState = 0x20000000,
};

inline GlobalFlag operator|(GlobalFlag lhs, GlobalFlag rhs) {
    return static_cast<GlobalFlag>(static_cast<u32>(lhs) | static_cast<u32>(rhs));
}

inline GlobalFlag operator&(GlobalFlag lhs, GlobalFlag rhs) {
    return static_cast<GlobalFlag>(static_cast<u32>(lhs) & static_cast<u32>(rhs));
}

inline GlobalFlag operator|=(GlobalFlag& lhs, GlobalFlag rhs) {
    lhs = lhs | rhs;
    return lhs;
}

inline GlobalFlag operator&=(GlobalFlag& lhs, GlobalFlag rhs) {
    lhs = lhs & rhs;
    return lhs;
}

inline GlobalFlag operator~(GlobalFlag flag) {
    return static_cast<GlobalFlag>(~static_cast<u32>(flag));
}

inline bool hasFlag(GlobalFlag flags, GlobalFlag flag) {
    return (static_cast<u32>(flags) & static_cast<u32>(flag)) != 0;
}

struct GlobalFlags {
    GlobalFlag value = GlobalFlag::None;
};

struct GlobalSequence {
    u32 timestamp = 0;
};

enum class SequenceFlag : u32 {
    None = 0,
    TiltIn = 0x00000001,
    TiltOut = 0x00000002,
    TiltFixed = 0x00000004,
    Looping = 0x00000020,
    IsAlias = 0x00000040,
    AnimatedSetup = 0x00000080,
    StoredAnimated = 0x00000100,
    EnableComposite = 0x00000200,
};

inline SequenceFlag operator|(SequenceFlag lhs, SequenceFlag rhs) {
    return static_cast<SequenceFlag>(static_cast<u32>(lhs) | static_cast<u32>(rhs));
}

inline SequenceFlag operator&(SequenceFlag lhs, SequenceFlag rhs) {
    return static_cast<SequenceFlag>(static_cast<u32>(lhs) & static_cast<u32>(rhs));
}

inline SequenceFlag operator|=(SequenceFlag& lhs, SequenceFlag rhs) {
    lhs = lhs | rhs;
    return lhs;
}

inline SequenceFlag operator&=(SequenceFlag& lhs, SequenceFlag rhs) {
    lhs = lhs & rhs;
    return lhs;
}

inline SequenceFlag operator~(SequenceFlag flag) {
    return static_cast<SequenceFlag>(~static_cast<u32>(flag));
}

inline bool hasFlag(SequenceFlag flags, SequenceFlag flag) {
    return (static_cast<u32>(flags) & static_cast<u32>(flag)) != 0;
}

struct Sequence {
    u16 id = 0;
    u16 variationIndex = 0;
    u32 duration = 0;
    f32 movespeed = 0.0f;
    SequenceFlag flags = SequenceFlag::None;
    i16 frequency = 0;
    u16 padding = 0;

    u32 replayMin = 0;
    u32 replayMax = 0;

    u16 blendTimeIn = 0;
    u16 blendTimeOut = 0;

    Extent bounding;

    i16 variationNext = -1;
    u16 aliasNext = 0;
};

struct Vertex {
    Vector3f position;
    std::array<u8, 4> boneWeights = {0};
    std::array<u8, 4> boneIndices = {0};
    Vector3f normal;
    std::array<Vector2f, 2> texCoords;
};

struct Bone {
    i32 keyBoneId = -1;
    u32 flags = 0;
    i16 parentBoneId = -1;
    u16 submeshId = 0;

    u32 boneNameCRC = 0;

    AnimationTrack<Vector3f> translation;
    AnimationTrack<CompatQuaternion> rotation;
    AnimationTrack<Vector3f> scale;
    Vector3f pivot;
};

/// @brief `M2CompBone.flags`, as WoW 12.1 reads it (`WOW_M2_FLAGS.md` §7).
///
/// The client ORs the file word with a runtime word, so the bits marked
/// runtime below are never meant to come from a file.
enum class BoneFlag : u32 {
    None = 0,
    IgnoreParentTranslate = 0x001,
    IgnoreParentScale = 0x002,
    IgnoreParentRotation = 0x004,
    /// @name Billboards
    /// One switch over `flags & BillboardMask`: exactly one of these, or none.
    /// Two or more set give no billboard at all.
    /// @{
    SphericalBillboard = 0x008,
    CylindricalBillboardX = 0x010,
    CylindricalBillboardY = 0x020,
    CylindricalBillboardZ = 0x040,
    /// Faces the camera position rather than lying flat against the view.
    BillboardAimAtCamera = 0x4000000,
    /// @}
    /// A procedural matrix is multiplied into the local transform.
    ProceduralTransform = 0x080,
    /// Has animation; with ProceduralTransform clear too, the bone skips animation.
    Transformed = 0x200,
    /// Eligible for physics: a live dynamic body on the bone replaces its animation.
    Kinematic = 0x400,
    /// The helmet-scaling pass writes its per-race scale into this bone.
    HelmetAnimScaled = 0x1000,
    PrimarySequenceAttached = 0x2000,   ///< runtime
    SecondarySequenceAttached = 0x4000, ///< runtime
    PhysicsInteractionOffset = 0x200000, ///< runtime
    PhysicsDriven = 0x400000,            ///< runtime: a dynamic body owns the bone
    /// With PrimarySequenceAttached, skip the per-sequence blend weight.
    SkipSequenceBlendWeight = 0x800000,
    /// With ProceduralTransform, apply the matrix after parenting, in world space.
    ProceduralInWorldSpace = 0x1000000,
};

/// @brief The bits the client's billboard switch reads (12.1 widened 6.0.1's 0x78).
constexpr u32 kBoneBillboardMask = 0x4000078;

inline BoneFlag operator|(BoneFlag lhs, BoneFlag rhs) {
    return static_cast<BoneFlag>(static_cast<u32>(lhs) | static_cast<u32>(rhs));
}

inline BoneFlag operator&(BoneFlag lhs, BoneFlag rhs) {
    return static_cast<BoneFlag>(static_cast<u32>(lhs) & static_cast<u32>(rhs));
}

inline BoneFlag operator|=(BoneFlag& lhs, BoneFlag rhs) {
    lhs = lhs | rhs;
    return lhs;
}

inline BoneFlag operator&=(BoneFlag& lhs, BoneFlag rhs) {
    lhs = lhs & rhs;
    return lhs;
}

inline BoneFlag operator~(BoneFlag flag) {
    return static_cast<BoneFlag>(~static_cast<u32>(flag));
}

inline bool hasFlag(BoneFlag flags, BoneFlag flag) {
    return (static_cast<u32>(flags) & static_cast<u32>(flag)) != 0;
}

struct Texture {
    u32 type = 0;
    u32 flags = 0;
    std::string filename;
};

enum class MaterialFlag : u16 {
    None = 0,
    Unlit = 0x01,
    Unfogged = 0x02,
    TwoSided = 0x04,
    DepthTest = 0x08,
    DepthWrite = 0x10,
    NoAlphaComposite = 0x800,
};

inline MaterialFlag operator|(MaterialFlag lhs, MaterialFlag rhs) {
    return static_cast<MaterialFlag>(static_cast<u16>(lhs) | static_cast<u16>(rhs));
}

inline MaterialFlag operator&(MaterialFlag lhs, MaterialFlag rhs) {
    return static_cast<MaterialFlag>(static_cast<u16>(lhs) & static_cast<u16>(rhs));
}

inline MaterialFlag operator|=(MaterialFlag& lhs, MaterialFlag rhs) {
    lhs = lhs | rhs;
    return lhs;
}

inline MaterialFlag operator&=(MaterialFlag& lhs, MaterialFlag rhs) {
    lhs = lhs & rhs;
    return lhs;
}

inline MaterialFlag operator~(MaterialFlag flag) {
    return static_cast<MaterialFlag>(~static_cast<u16>(flag));
}

inline bool hasFlag(MaterialFlag flags, MaterialFlag flag) {
    return (static_cast<u16>(flags) & static_cast<u16>(flag)) != 0;
}

/// @wem rename=M2RenderRecord
struct Material {
    u16 flags = 0;
    u16 blendingMode = 0;
};

struct TextureWeight {
    AnimationTrack<i16> weight;
};

struct TextureTransform {
    AnimationTrack<Vector3f> translation;
    // Stored as C4Quaternion (floats) in every M2 version, unlike bone
    // rotations, which are compressed. Verified against the client's
    // M2Init<M2TextureTransform>.
    AnimationTrack<Quaternion> rotation;
    AnimationTrack<Vector3f> scaling;
};

struct ColorAnimation {
    AnimationTrack<Vector3f> color;
    AnimationTrack<i16> alpha;
};

struct Light {
    u16 type = 0;
    i16 boneId = -1;
    Vector3f position;

    AnimationTrack<Vector3f> ambientColor;
    AnimationTrack<f32> ambientIntensity;
    AnimationTrack<Vector3f> diffuseColor;
    AnimationTrack<f32> diffuseIntensity;
    AnimationTrack<f32> attenuationStart;
    AnimationTrack<f32> attenuationEnd;
    AnimationTrack<u8> visibility;
};

struct CameraSpline {
    Vector3f value;
    Vector3f inTangent;
    Vector3f outTangent;
};

struct Camera {
    u32 type = 0;
    f32 fieldOfView = 0.0f;
    f32 farClip = 0.0f;
    f32 nearClip = 0.0f;

    AnimationTrack<CameraSpline> positions;
    Vector3f positionBase;

    AnimationTrack<CameraSpline> targetPositions;
    Vector3f targetPositionBase;

    AnimationTrack<f32> roll;
    AnimationTrack<f32> fieldOfViewTrack;
};

struct Attachment {
    u32 id = 0;
    u16 boneId = 0;
    u16 unknown = 0;
    Vector3f position;
    AnimationTrack<u8> animate;
};

struct RibbonEmitter {
    u32 ribbonId = UINT32_MAX;
    u32 boneId = 0;
    Vector3f position;

    std::vector<u16> textureIndices;
    std::vector<u16> materialIndices;

    AnimationTrack<Vector3f> colorTrack;
    AnimationTrack<i16> alphaTrack;
    AnimationTrack<f32> heightAbove;
    AnimationTrack<f32> heightBelow;

    f32 edgesPerSecond = 0.0f;
    f32 edgeLifetime = 0.0f;
    f32 gravity = 0.0f;
    u16 textureRows = 1;
    u16 textureCols = 1;
    AnimationTrack<u16> texSlot;
    AnimationTrack<u8> visibility;
    i16 priorityPlane = 0;
    i8 ribbonColorIndex = -1;
    i8 textureTransformIndex = -1;
};

struct M2Box {
    Vector3f minimum;
    Vector3f maximum;
};

enum class ParticleEmitterType : u8 {
    Plane = 1,
    Sphere = 2,
    Spline = 3,
    Bone = 4,
};

enum class ParticleBlending : u8 {
    Opaque = 0,
    AlphaBlend = 1,
    Additive = 2,
    AlphaTest = 3,
    AdditiveAlphaTest = 4,
};

enum class ParticleFlag : u32 {
    None = 0,
    Unlit = 0x1,          // Sets the material unlit bit; particles are lit by default
    SortParticles = 0x2,  // Depth-sorted rendering via s_pq priority queue
    VelocityOrient = 0x4, // Billboard aligns along velocity vector
    Unfogged = 0x8,       // Sets the material unfogged bit (CParticleMat bit 1 = !(flags & 0x8))
    WorldSpace = 0x10,    // Particles operate in world space; skips bone matrix transform in
                          // CreateParticle/UpdateXform.
    InheritBoneScale = 0x20, // In IBuildVertices: multiplies particle size by m_scaleFactor, which
                             // is sqrt(length(boneMatrix column 0)) extracted in UpdateXform.
                             // Particles scale proportionally to attached bone
    InheritVelocity = 0x40,  // Child emitter inherits parent velocity
    ImplosionFilter = 0x80,  // Particles going away from the center (world space or bone matrix if
                             // local) are killed.
    HemisphereUpDirection = 0x100, // Sets velocity direction to be Z-up in SphereEmitters.
    NegateSpinRandom = 0x200,      // Negate spin angle for particles with random bit & 1
    ClampTailToAge = 0x400,        // Clamp tail length to min(tailLength, age) in IBuildVertices
    InheritPosition = 0x800,       // Child inherits parent position; random emission spacing
    XYQuad = 0x1000,               // Use s_quadToView matrix instead of screen-aligned billboard
    ProjectParticle = 0x2000,      // Snap particle to terrain via s_projectCallback
    FollowPosition = 0x4000,       // Add emitter delta-position to particle when 2*dt < age
    Squirt =
        0x8000, // Particle Emitter only emits as bursts particles when emissionRate is animated.
    ChooseRandomTexture = 0x10000,   // Random flipbook frame via CRandom::dice_
    HeadStyle = 0x20000,             // Controls head particle style bit pattern in SetParticleStyle
    TailStyle = 0x40000,             // Controls tail particle style bit pattern in SetParticleStyle
    UnscaledSizeVariation = 0x80000, // Independent X/Y scale variation (2 random floats)
    Refraction = 0x100000,           // ParticleType 3: drawn with the Particle_Refraction shader,
                                     // forced unlit, skipped while the camera is submerged.
                                     // MultiTexture takes precedence when both are set.
    RandFlipbookStart = 0x200000,    // Random starting frame via SetRandFlipBookStart
    Unk_0x400000 = 0x400000,         // Present in data but never read by the client
    CompressedGravity = 0x800000,    // Gravity keys are compressed direction vectors
                                     // (int8 x,y + int16 z) instead of z-axis floats
    BoneGeneratorBone =
        0x1000000, // Select CBoneGeneratorBone (1) vs CBoneGeneratorJoint (0); only consulted
                   // when emitterType == Bone — exporters set it on other emitter types too
    NoGlobalViewScale = 0x2000000, // Skip s_globalViewScale multiplication on emission rate
    LodIgnoreDistance =
        0x4000000, // Skip distance-based LOD emissionrate scaling in EmitNewParticles
    OffsetHeadBySpin = 0x8000000, // Offset head particle position along spin rotation axis
    MultiTexture = 0x10000000,    // Route through multi-texture particle creation path
    MultitexUseModx4 =
        0x20000000, // CParticleMat bit 3; uses Modx4 instead of Modx2 requires MultiTexture
    MultitexUse3Colors =
        0x40000000, // CParticleMat bit 4; uses 3 colors instead of 2 requires MultiTexture
    DynamicWind =
        0x80000000, // Enables dynamic wind; SET=dynamic callback, CLEAR=static from M2 data
};

struct ParticleEmitter {
    u32 particleId = UINT32_MAX;
    ParticleFlag flags = ParticleFlag::None;
    Vector3f position;
    u16 boneId = 0;
    WHITEOUT_ANON union {
        u16 textureId;
        struct {
            u16 textureId1 : 5;
            u16 textureId2 : 5;
            u16 textureId3 : 5;
            u16 padding : 1;
        };
    };

    // If the path is valid, the particle system uses model particles
    std::string particleModelFilename;
    // Child emitters are obtained from it.
    std::string childEmittersModelFilename; // Emitted as trail per particle.

    ParticleBlending blendingType = ParticleBlending::Opaque;
    ParticleEmitterType emitterType = ParticleEmitterType::Plane;
    u16 particleColorIndex = 0;

    // Pre-Cataclysm records carry these two bytes where multiTexScale now
    // lives; kept so old files round-trip. headOrTail: 0 head, 1 tail, 2 both.
    u8 particleType = 0;
    u8 headOrTail = 0;

    std::array<fixed8_5, 2> multiTexScale; // Scale per layer
    i16 textureTilerotation;
    u16 rows;
    u16 columns;

    AnimationTrack<f32> emissionSpeed;
    AnimationTrack<f32> speedVariation;
    AnimationTrack<f32> verticalRange;
    AnimationTrack<f32> horizontalRange;
    AnimationTrack<f32> gravity;
    AnimationTrack<f32> lifespan;
    f32 lifespanVariation = 0.0f;
    AnimationTrack<f32> emissionRate;
    f32 emissionRateVariation = 0.0f;
    AnimationTrack<f32> emissionAreaWidth;
    AnimationTrack<f32> emissionAreaLength;
    AnimationTrack<f32> zSource;
    ParticleAnimationTrack<Vector3f> colorTrack;
    ParticleAnimationTrack<unorm16> alphaTrack;
    ParticleAnimationTrack<Vector2f> scaleTrack;
    Vector2f scaleVary = Vector2f(0.0f, 0.0f);
    ParticleAnimationTrack<unorm16> headUVScroll;
    ParticleAnimationTrack<unorm16> tailUVScroll;
    f32 tailLength = 0.0f;

    // Twinkle effect, causes twinkling like effects on particles.
    // they scale up and down randomly and are culled based on a thresshole.
    f32 twinkleSpeed = 0.0f;   // blinking speed
    f32 twinklePercent = 1.0f; // how visible is the particle. 1.0 - 100% time, 0.5 - 50% of time.
    Vector2f twinkleScale = Vector2f(0.0f, 0.0f); // min and max scale variation

    // Scales the velocity inherited from the parent particle.
    f32 inheritVelocityScale = 1.0f;

    // Drag effect, causes particles to slow down over time.
    f32 drag = 0.0f;

    // 2D Billboard spin rotation
    // Quad and Multitex Particles only
    f32 baseSpin = 0.0f;
    f32 baseSpinVariation = 0.0f;
    f32 spinSpeed = 0.0f;
    f32 spinSpeedVariation = 0.0f;

    // 3D Model particle rotation (ModelParticles only)
    M2Box tumble; // angularVelocity min and max

    // Static wind parameters, ignored if DynamicWind flag is set
    Vector3f windVector;
    f32 windTime = 0.0f;

    f32 followSpeed1 = 0.0f;
    f32 followScale1 = 0.0f;
    f32 followSpeed2 = 0.0f;
    f32 followScale2 = 0.0f;

    std::vector<Vector3f> splinePoints;
    AnimationTrack<u8> enabledIn;

    std::array<std::array<fixed16_9, 2>, 2> multiTexScrollMid{};
    std::array<std::array<fixed16_9, 2>, 2> multiTexScrollRange{};

    std::optional<ParticleEmitterExtension> extension;
};

struct Event {
    u32 identifier = 0;
    u32 data = 0;
    u32 boneId = 0;
    Vector3f position;
    AnimationTrackBase enabled;
};

} // namespace m2
} // namespace whiteout
