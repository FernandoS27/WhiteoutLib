// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include "types.h"

#include <array>
#include <memory>
#include <string>
#include <vector>
#include <whiteout/compatibility.h>

namespace whiteout {
namespace m3 {

// ============================================================================
// Enumeration Classes
// ============================================================================

/// Material type enum - identifies which material array a MATM entry references
enum class MaterialType : u32 {
    Standard = 1,         // MAT_
    Displacement = 2,     // DIS_
    Composite = 3,        // CMP_
    Terrain = 4,          // TER_
    Volume = 5,           // VOL_
    VolumeNoise = 6,      // VON_
    Creep = 7,            // CREP
    Hair = 8,             // HAI_
    SplatTerrainBake = 9, // STBM
    Reflection = 10,      // REF_
    LensFlare = 11,       // LFLR
    BufferMaterial = 12,  // MADD
};

/// Light type enum
enum class LightType : u16 {
    Omni = 0,
    Spot = 1,
    Directional = 2,
};

/// Physics shape type enum - used in PHSH (RigidBodyShape)
enum class PhysicsShapeType : u8 {
    Box = 0,
    Sphere = 1,
    Capsule = 2,
    Cylinder = 3,
    ConvexHull = 4,
    Mesh = 5,
};

/// Hit test shape type enum - used in SSGS and ATVL (same values as PhysicsShapeType but u32)
enum class HitTestShapeType : u32 {
    Box = 0,
    Sphere = 1,
    Capsule = 2,
    Cylinder = 3,
    Mesh = 4,
};

/// Particle emitter shape enum
enum class EmitterShape : u32 {
    Point = 0,
    Plane = 1,
    Sphere = 2,
    Box = 3,
    Cylinder = 4,
    Disc = 5,
    Mesh = 6,
    Spline = 7,
};

/// Particle instance/visual type enum (maps 1:1 to b_iInstanceType in Particle.fx)
enum class ParticleInstanceType : u32 {
    Billboard = 0,          ///< Camera-facing billboard quad
    Tail = 1,               ///< Velocity-stretched quad
    FaceTravelDir = 2,      ///< Quad oriented along instantaneous velocity
    FaceWorldDir = 3,       ///< Quad oriented along a fixed world direction
    SingleAxis = 4,         ///< Billboard locked to a single rotation axis
    TerrainOriented = 5,    ///< Quad projected onto terrain normal
    TerrainDirOriented = 6, ///< Terrain-oriented + velocity-stretched
    EmitterOriented = 7,    ///< Quad uses the emitter bone's orientation
    PhysicsOriented = 8,    ///< Quad oriented by physics simulation
    Pinned = 9,             ///< Stretch between spawn origin and current position
    Trail = 10,             ///< Like Tail but offset by one tail-length
};

/// Force type enum
enum class ForceType : u32 {
    Radial = 0,
    Wind = 1,
    Explosion = 2,
    // Additional types may exist in extended versions
};

/// Force shape enum - influence shape of a force
enum class ForceShape : u32 {
    Sphere = 0,
    Cylinder = 1,
    Box = 2,
    Hemisphere = 3,
};

/// Ribbon cross-section type enum (maps 1:1 to b_iRibbonType in Ribbon.fx)
enum class RibbonType : u32 {
    Billboard = 0, ///< Camera-facing ribbon strip
    Planar = 1,    ///< Flat/planar ribbon strip
    Cylinder = 2,  ///< Cylindrical cross-section
    Star = 3,      ///< Star-shaped cross-section
};

/// Projection type enum
enum class ProjectionType : u32 {
    Orthographic = 0,
    Perspective = 1,
};

/// Volume type enum
enum class VolumeType : u32 {
    Box = 0,
    Sphere = 1,
    Capsule = 2,
};

/// Interpolation mode enum (maps to RibbonParticleCommon.fx constants)
/// Used by PAR_ colorSmoothing/sizeSmoothing/rotationSmoothing and RIB_
/// sizeSmoothing/colorSmoothing
enum class InterpolationMode : u32 {
    Linear = 0,         ///< ITERPOLATION_LINEAR
    LinearSmooth = 1,   ///< ITERPOLATION_LINEAR_SMOOTH
    Bezier = 2,         ///< ITERPOLATION_BEZIER
    LinearWithHold = 3, ///< ITERPOLATION_LINEAR_WITH_HOLD
    BezierWithHold = 4, ///< ITERPOLATION_BEZIER_WITH_HOLD
};

// ============================================================================
// Flag Enum Classes
// ============================================================================

// Helper macro to define bitwise operators for flag enum classes
#define M3_DEFINE_FLAG_OPS(FlagType, UnderlyingType)                                               \
    inline FlagType operator|(FlagType lhs, FlagType rhs) {                                        \
        return static_cast<FlagType>(static_cast<UnderlyingType>(lhs) |                            \
                                     static_cast<UnderlyingType>(rhs));                            \
    }                                                                                              \
    inline FlagType operator&(FlagType lhs, FlagType rhs) {                                        \
        return static_cast<FlagType>(static_cast<UnderlyingType>(lhs) &                            \
                                     static_cast<UnderlyingType>(rhs));                            \
    }                                                                                              \
    inline FlagType operator|=(FlagType& lhs, FlagType rhs) {                                      \
        lhs = lhs | rhs;                                                                           \
        return lhs;                                                                                \
    }                                                                                              \
    inline FlagType operator&=(FlagType& lhs, FlagType rhs) {                                      \
        lhs = lhs & rhs;                                                                           \
        return lhs;                                                                                \
    }                                                                                              \
    inline FlagType operator~(FlagType flag) {                                                     \
        return static_cast<FlagType>(~static_cast<UnderlyingType>(flag));                          \
    }                                                                                              \
    inline bool hasFlag(FlagType flags, FlagType flag) {                                           \
        return (static_cast<UnderlyingType>(flags) & static_cast<UnderlyingType>(flag)) != 0;      \
    }

/// Model flags (MODL)
enum class ModelFlag : u32 {
    None = 0x0,
    Tangents = 0x00001,                   ///< Tangents computed
    BonesFixed = 0x00002,                 ///< Bone transforms fixed
    UVDensitiesComputed = 0x00004,        ///< UV densities computed
    RelativeBounds = 0x00008,             ///< Uses relative bounds
    SectionBoundsFixed = 0x00010,         ///< Section bounds fixed
    TrackSetsComputed = 0x00020,          ///< Track sets computed
    TrackCollectionSorted = 0x00040,      ///< Track collection sorted
    AcceptsSplats = 0x00080,              ///< Model accepts splats
    TrackAnimatedBaseFlagValid = 0x00800, ///< Animated base flag valid
    FileDirty = 0x01000,                  ///< File marked dirty
    FowDoNotUseTint = 0x04000,            ///< FOW: do not tint
    InstancedVB = 0x08000,                ///< Uses instanced vertex buffer
    ForceSampledFOW = 0x10000,            ///< Force sampled FOW
    InstancedModel = 0x20000,             ///< Instanced model
    NeverUseFOW = 0x40000,                ///< Never use FOW
    BoneAnimatedFlagSolved = 0x80000,     ///< Bone animated flags solved
    AllowLocalLightShadows = 0x100000,    ///< Allow local light shadows
    AvoidSampledFOW = 0x200000,           ///< Avoid sampled FOW
};
M3_DEFINE_FLAG_OPS(ModelFlag, u32)

/// Sequence flags (SEQS)
enum class SequenceFlag : u32 {
    None = 0x0,
    NotLooping = 0x1,        ///< Sequence does not loop
    AlwaysGlobal = 0x2,      ///< Always plays globally
    Unknown0x4 = 0x4,        ///< Unknown
    GlobalInPreviewer = 0x8, ///< Global playback in editor
};
M3_DEFINE_FLAG_OPS(SequenceFlag, u32)

/// Bone flags (BONE)
enum class BoneFlag : u32 {
    None = 0x0,
    InheritTranslation = 0x0001, ///< Inherit parent translation
    InheritScale = 0x0002,       ///< Inherit parent scale
    InheritRotation = 0x0004,    ///< Inherit parent rotation
    Billboard1 = 0x0010,         ///< Billboard mode 1
    Billboard2 = 0x0040,         ///< Billboard mode 2
    Project2D = 0x0100,          ///< 2D projection mode
    Animated = 0x0200,           ///< Has animation data
    InverseKinematics = 0x0400,  ///< IK bone
    Skinned = 0x0800,            ///< Affects mesh skin
    Real = 0x2000,               ///< Real bone (not helper)
    Batch1 = 0x4000,             ///< Primary batch bone
    Batch2 = 0x8000,             ///< Descendant of batch1 bone
};
M3_DEFINE_FLAG_OPS(BoneFlag, u32)

/// Region flags (REGN v4+)
enum class RegionFlag : u32 {
    None = 0x0,
    Hidden = 0x1,          ///< Region is hidden
    Placeholder = 0x2,     ///< Placeholder region
    ClothSimulated = 0x4,  ///< Cloth-simulated
    ClothInfluenced = 0x8, ///< Cloth-influenced
};
M3_DEFINE_FLAG_OPS(RegionFlag, u32)

/// Standard material additional flags (MAT_)
enum class MaterialAdditionalFlag : u32 {
    None = 0x0,
    DepthBlendFalloff = 0x1, ///< Enable depth blend falloff
    VertexColor = 0x4,       ///< Uses vertex color
    VertexAlpha = 0x8,       ///< Uses vertex alpha
};
M3_DEFINE_FLAG_OPS(MaterialAdditionalFlag, u32)

/// Standard material flags (MAT_)
enum class MaterialFlag : u32 {
    None = 0x0,
    VertexColor = 0x00000001,             ///< Enable vertex color
    VertexAlpha = 0x00000002,             ///< Enable vertex alpha
    Unfogged = 0x00000004,                ///< Not affected by fog
    TwoSided = 0x00000008,                ///< Two-sided rendering
    Unshaded = 0x00000010,                ///< Unlit / unshaded
    NoShadowsCast = 0x00000020,           ///< Does not cast shadows
    NoHitTest = 0x00000040,               ///< Excluded from hit testing
    NoShadowsReceive = 0x00000080,        ///< Does not receive shadows
    DepthPrepass = 0x00000100,            ///< Z-fill pre-pass
    TerrainHDR = 0x00000200,              ///< Terrain HDR mode
    SimulateRoughness = 0x00000800,       ///< Simulate roughness
    PixelForwardLighting = 0x00001000,    ///< Pixel forward lighting
    DepthFog = 0x00002000,                ///< Depth-based fog
    TransparentShadows = 0x00004000,      ///< Transparent shadows
    DecalLighting = 0x00008000,           ///< Decal lighting mode
    TransparentDepthEffects = 0x00010000, ///< Transparent depth effects
    TransparentLocalLights = 0x00020000,  ///< Transparent local lights
    DisableSoft = 0x00040000,             ///< Disable soft blending
    DoubleLambert = 0x00080000,           ///< Double Lambert shading
    HairLayerSorting = 0x00100000,        ///< Hair layer sorting
    AcceptSplats = 0x00200000,            ///< Accept splat projections
    DecalLowRequired = 0x00400000,        ///< Decal low LOD required
    EmisLowRequired = 0x00800000,         ///< Emissive low LOD required
    SpecLowRequired = 0x01000000,         ///< Specular low LOD required
    AcceptSplatsOnly = 0x02000000,        ///< Accept splats only
    BackgroundObject = 0x04000000,        ///< Background object
    DepthPrepassLowRequired = 0x10000000, ///< Depth prepass low LOD
    NoHighlighting = 0x20000000,          ///< Disable highlighting
    ClampOutput = 0x40000000,             ///< Clamp output
    GeometryVisible = 0x80000000,         ///< Geometry visible (v17+)
};
M3_DEFINE_FLAG_OPS(MaterialFlag, u32)

/// Texture layer flags (LAYR)
enum class TextureLayerFlag : u32 {
    None = 0x0,
    UVWrapX = 0x0004,              ///< Wrap texture in U
    UVWrapY = 0x0008,              ///< Wrap texture in V
    ColorInvert = 0x0010,          ///< Invert color
    ColorClamp = 0x0020,           ///< Clamp to [0,1]
    ColorAdd = 0x0040,             ///< Additive blending
    ColorMultiply = 0x0080,        ///< Multiplicative blending
    ParticleUVFlipbook = 0x0100,   ///< Flipbook UVs for particles
    Video = 0x0200,                ///< Video texture
    Color = 0x0400,                ///< Solid color (no texture)
    ReplaceTextureSource = 0x0800, ///< Override texture source
    FresnelTransform = 0x4000,     ///< Fresnel-based UV transform
    FresnelNormalize = 0x8000,     ///< Normalize fresnel values
};
M3_DEFINE_FLAG_OPS(TextureLayerFlag, u32)

/// Blend mode for materials (MAT_.blendMode, VOL_.blendMode)
enum class BlendMode : u32 {
    Opaque = 0,     ///< Fully opaque
    AlphaBlend = 1, ///< Standard alpha blending
    Add = 2,        ///< Additive blending
    AlphaAdd = 3,   ///< Alpha-modulated additive
    Mod = 4,        ///< Multiplicative blending
    Mod2x = 5,      ///< Double multiplicative
};

/// Material class (MAT_.materialClass)
enum class MaterialClass : u32 {
    Unit = 0,      ///< Unit/character material
    Building = 1,  ///< Building/structure material
    Doodad = 2,    ///< Doodad/prop material
    SpecialFX = 3, ///< Special effect material
};

/// Layer blend operation (MAT_.layerBlendMode, emissiveBlendMode)
enum class LayerBlendOp : u32 {
    Mod = 0,                  ///< Multiply: base * layer
    Mod2x = 1,                ///< Double multiply: base * layer * 2
    Add = 2,                  ///< Add: base + layer
    Lerp = 3,                 ///< Linear interpolate by layer alpha
    TeamColorEmissiveAdd = 4, ///< Team color emissive add
    TeamColorDiffuseAdd = 5,  ///< Team color diffuse add
    AddNoAlpha = 6,           ///< Add ignoring alpha channel
};

/// UV mapping mode (LAYR.uvMapping)
enum class UVMappingMode : u32 {
    ExplicitUV0 = 0,           ///< UV coordinate set 0
    ExplicitUV1 = 1,           ///< UV coordinate set 1
    ReflectCubicEnvio = 2,     ///< Cubic environment reflection
    ReflectSphericalEnvio = 3, ///< Spherical environment reflection
    PlanarLocalZ = 4,          ///< Planar local UVs (Z plane)
    PlanarWorldZ = 5,          ///< Planar world UVs (Z plane)
    ParticleFlipbook = 6,      ///< Particle flipbook UVs
    CubicEnvio = 7,            ///< Cubic environment mapping
    SphericalEnvio = 8,        ///< Spherical environment mapping
    ExplicitUV2 = 9,           ///< UV coordinate set 2
    ExplicitUV3 = 10,          ///< UV coordinate set 3
    PlanarLocalX = 11,         ///< Planar local UVs (X plane)
    PlanarLocalY = 12,         ///< Planar local UVs (Y plane)
    PlanarWorldX = 13,         ///< Planar world UVs (X plane)
    PlanarWorldY = 14,         ///< Planar world UVs (Y plane)
    ScreenSpace = 15,          ///< Screen-space UVs
    TriPlanarLocal = 16,       ///< Tri-planar blending (local space)
    TriPlanarWorld = 17,       ///< Tri-planar blending (world space)
    TriPlanarWorldLocalZ = 18, ///< Tri-planar world with local Z
};

/// Color channel selection (LAYR.colorType)
enum class ColorChannelSelect : u32 {
    RGB = 0,   ///< Use RGB channels (alpha forced to 1)
    RGBA = 1,  ///< Use all RGBA channels
    Alpha = 2, ///< Use alpha channel only (splat to all)
    Red = 3,   ///< Use red channel only (splat to all)
    Green = 4, ///< Use green channel only (splat to all)
    Blue = 5,  ///< Use blue channel only (splat to all)
};

/// Specular mode (MAT_.specularMode)
enum class SpecularMode : u32 {
    RGB = 0,       ///< Use RGB channels for specularity
    AlphaOnly = 1, ///< Use alpha channel only
};

/// Fresnel mode (LAYR.fresnelMode)
enum class FresnelMode : u32 {
    None = 0,     ///< No fresnel effect
    Standard = 1, ///< Standard fresnel (edge glow)
    Inverted = 2, ///< Inverted fresnel (center glow)
};

enum class ReflectionMaterialFlag : u32 {
    None = 0x0,
    UseReflectionMap = 0x1,        ///< Use reflection map
    UseDisplacementMap = 0x2,      ///< Use displacement map
    RenderInTransparentPass = 0x4, ///< Render in transparent pass
    Blurring = 0x8,                ///< Enable blurring
    UseBlurMap = 0x10,             ///< Use blur map
};

enum class VolumeNoiseMaterialFlag : u32 {
    None = 0x0,
    DrawAfterTransparency = 0x1, ///< Draw in separate pass after transparency
};
M3_DEFINE_FLAG_OPS(VolumeNoiseMaterialFlag, u32)

/// Volume falloff type (VOL_.falloffType, VON_.falloffType)
enum class VolumeFalloffType : u32 {
    Linear = 0,      ///< Linear density falloff
    Exponential = 1, ///< Exponential density falloff
};

/// Volume noise camera position mode (VON_.drawTransparency)
enum class VolumeNoiseCameraMode : u32 {
    Outside = 0, ///< Camera is outside the volume
    Inside = 1,  ///< Camera is inside the volume
};

/// Light flags (LITE)
enum class LightFlag : u32 {
    None = 0x0,
    Shadows = 0x01,          ///< Casts shadows
    Specular = 0x02,         ///< Specular component
    AmbientOcclusion = 0x04, ///< AO influence
    LightOpaque = 0x08,      ///< Lights opaque objects
    LightTransparent = 0x10, ///< Lights transparent objects
    TeamColor = 0x20,        ///< Uses team color
};
M3_DEFINE_FLAG_OPS(LightFlag, u32)

/// Particle emitter main flags (PAR_ `flags` field)
enum class ParticleFlag : u32 {
    None = 0x0,
    Sort = 0x1,                         ///< Sort by distance
    CollideTerrain = 0x2,               ///< Collide with terrain
    CollideObjects = 0x4,               ///< Collide with objects
    CollideEmit = 0x8,                  ///< Emit on collision
    EmitShapeCutout = 0x10,             ///< Emit from shape cutout
    InheritEmitParams = 0x20,           ///< Inherit emission parameters
    InheritParentVelocity = 0x40,       ///< Inherit parent velocity
    SortHeight = 0x80,                  ///< Sort by height
    SortReverse = 0x100,                ///< Reverse sort order
    OldRotationSmooth = 0x200,          ///< Legacy rotation smoothing
    OldRotationBezier = 0x400,          ///< Legacy rotation bezier
    OldSizeSmooth = 0x800,              ///< Legacy size smoothing
    OldSizeBezier = 0x1000,             ///< Legacy size bezier
    OldColorSmooth = 0x2000,            ///< Legacy color smoothing
    OldColorBezier = 0x4000,            ///< Legacy color bezier
    LitParts = 0x8000,                  ///< Lit particles
    RandomFlipbookStart = 0x10000,      ///< Random flipbook start
    MultiplyGravityByMass = 0x20000,    ///< Multiply gravity by mass
    ClampTailLength = 0x40000,          ///< Clamp tail length
    SpawnTrailingParticles = 0x80000,   ///< Spawn trailing particles
    FixTailLengthOnCreation = 0x100000, ///< Fix tail length on creation
    UseVertexAlpha = 0x200000,          ///< Use vertex alpha
    ModelParticles = 0x400000,          ///< Use model particles
    SwapYZOnModelParticles = 0x800000,  ///< Swap Y/Z on model particles
    ScaleTimeByParent = 0x1000000,      ///< Scale time by parent
    UseLocalTime = 0x2000000,           ///< Use local time
    SimulateInit = 0x4000000,           ///< Simulate on initialization
    Copy = 0x8000000,                   ///< Copy emitter
};
M3_DEFINE_FLAG_OPS(ParticleFlag, u32)

/// Particle emitter additional flags (PAR_ `additionalFlags` field, since v17)
enum class ParticleAdditionalFlag : u32 {
    None = 0x0,
    EmitSpeedRandomize = 0x1, ///< Randomize emission speed
    LifespanRandomize = 0x2,  ///< Randomize lifespan
    MassRandomize = 0x4,      ///< Randomize mass
    WorldSpace = 0x8,         ///< World-space coordinates
};
M3_DEFINE_FLAG_OPS(ParticleAdditionalFlag, u32)

/// Particle rotation flags (PAR_ `rotationFlags` field, since v18)
enum class ParticleRotationFlag : u32 {
    None = 0x0,
    Relative = 0x2,  ///< Relative rotation
    AlwaysSet = 0x4, ///< Always set
};
M3_DEFINE_FLAG_OPS(ParticleRotationFlag, u32)

/// Ribbon emitter main flags (RIB_)
enum class RibbonFlag : u32 {
    None = 0x0,
    CollideTerrain = 0x02,        ///< Collide with terrain
    CollideObjects = 0x04,        ///< Collide with objects
    EdgeFalloff = 0x08,           ///< Fade edges
    InheritParentVelocity = 0x10, ///< Inherit parent velocity
    SmoothSize = 0x20,            ///< Smooth size
    BezierSmoothSize = 0x40,      ///< Bezier smooth size
    UseVertexAlpha = 0x80,        ///< Use vertex alpha
    ScaleTimeByParent = 0x100,    ///< Scale time by parent
    ForceCPUSim = 0x200,          ///< Force CPU simulation
    LocalTime = 0x400,            ///< Use local time
    SimulateInit = 0x800,         ///< Simulate on init
    UseLengthAndTime = 0x1000,    ///< Use length and time
    AccurateGPUTangents = 0x2000, ///< Accurate GPU tangents
    YawFromSpeed = 0x4000,        ///< Derive yaw from speed
    UseLocator = 0x8000,          ///< Use locator node
};
M3_DEFINE_FLAG_OPS(RibbonFlag, u32)

/// Ribbon emitter additional flags (RIB_ flags2)
enum class RibbonAdditionalFlag : u32 {
    None = 0x0,
    SpeedRandomize = 0x01,    ///< Randomize emission speed
    LifespanRandomize = 0x02, ///< Randomize lifespan
    MassRandomize = 0x04,     ///< Randomize mass
    WorldSpace = 0x08,        ///< World-space coordinates
};
M3_DEFINE_FLAG_OPS(RibbonAdditionalFlag, u32)

/// Projector flags (PROJ)
enum class ProjectorFlag : u32 {
    None = 0x0,
    Static = 0x1,         ///< Static position
    UnknownFlag0x2 = 0x2, ///< Unknown
    UnknownFlag0x4 = 0x4, ///< Unknown
    UnknownFlag0x8 = 0x8, ///< Unknown
};
M3_DEFINE_FLAG_OPS(ProjectorFlag, u32)

/// Force flags (FOR_)
enum class ForceFlag : u32 {
    None = 0x0,
    Falloff = 0x01,        ///< Distance falloff
    HeightGradient = 0x02, ///< Height gradient
    Unbounded = 0x04,      ///< Unbounded range
};
M3_DEFINE_FLAG_OPS(ForceFlag, u32)

/// Rigid body flags (PHRB)
enum class RigidBodyFlag : u32 {
    None = 0x0,
    Collidable = 0x0001,        ///< Can collide
    Walkable = 0x0002,          ///< Walkable surface
    Stackable = 0x0004,         ///< Can be stacked
    SimulateCollision = 0x0008, ///< Simulate collisions
    IgnoreLocalBodies = 0x0010, ///< Ignore local bodies
    AlwaysExists = 0x0020,      ///< Always present
    Unknown6 = 0x0040,          ///< Unknown
    NoSimulation = 0x0080,      ///< Disable simulation
    Unknown9 = 0x0200,          ///< Unknown
};
M3_DEFINE_FLAG_OPS(RigidBodyFlag, u32)

// Clean up the macro - it's only used in this header
#undef M3_DEFINE_FLAG_OPS

#define M3_DEFINE_VERSION_ACCESSORS()                                                              \
private:                                                                                           \
    i32 version = -1;                                                                              \
                                                                                                   \
public:                                                                                            \
    i32 getVersion() const {                                                                       \
        return version;                                                                            \
    }                                                                                              \
    bool setVersion(i32 newVersion) {                                                              \
        if (version != -1) {                                                                       \
            return false;                                                                          \
        }                                                                                          \
        version = newVersion;                                                                      \
        return true;                                                                               \
    }

// ============================================================================
// Struct Definitions
// ============================================================================

// ============================================================================
// Animation System
// ============================================================================

/// Event – Event (104 or 108 bytes)
struct Event {
    static constexpr u32 tag = TAG_EVNT;
    static constexpr u32 max_version = 2;
    std::string name;
    u32 unknown;
    u16 boneIndex;
    u16 padding;
    Matrix44f transform;
    u32 eventType;
    std::string optionString;
    u32 rttChannelIndex;
    u32 extraParameter;
    M3_DEFINE_VERSION_ACCESSORS()
};

/// Sequence – Animation Sequence (latest version, 92 bytes)
struct Sequence {
    static constexpr u32 tag = TAG_SEQS;
    static constexpr u32 max_version = 2;
    i32 id;
    i32 index;
    std::string name; // sequence name
    u32 startFrame;
    u32 endFrame;
    f32 moveSpeed;
    SequenceFlag flags = SequenceFlag::None; ///< Sequence flags
    u32 frequency;
    u32 replayStart;
    u32 replayEnd;
    u32 blendTime;
    Extent bounds;
    std::vector<u8> animationSets;

    struct {
        u32 unknown; // v2=4 bytes, v1=8 bytes
    } deprecated;    // v1 only - used for unknown data that was removed in v2
    M3_DEFINE_VERSION_ACCESSORS()
};

/// SubTrackContainer – Animation Sub-Track Container (204 bytes)
struct SubTrackContainer {
    static constexpr u32 tag = TAG_STC;
    static constexpr u32 max_version = 4;
    std::string name;
    u16 runsConcurrent;
    u16 animPriority;
    u16 animationStateIndex;
    u16 padding;
    std::vector<u32> animIds;
    std::vector<u32> animRefs;
    u32 unknown;
    // 13 animation data block arrays (each contains AnimBlocks with typed keys)
    std::vector<AnimBlock<Event>> sdev;      // slot 0: SDEV (Event keys)
    std::vector<AnimBlock<Vector2f>> sd2v;   // slot 1: SD2V (Vector2f keys)
    std::vector<AnimBlock<Vector3f>> sd3v;   // slot 2: SD3V (Vector3f keys)
    std::vector<AnimBlock<Quaternion>> sd4q; // slot 3: SD4Q (Quat keys)
    std::vector<AnimBlock<ColorBGRA>> sdcc;  // slot 4: SDCC (ColorBGRA keys)
    std::vector<AnimBlock<f32>> sdr3;        // slot 5: SDR3 (f32 keys)
    std::vector<AnimBlock<u8>> sdu8;         // slot 6: SDU8 (u8 keys)
    std::vector<AnimBlock<i16>> sds6;        // slot 7: SDS6 (i16 keys)
    std::vector<AnimBlock<u16>> sdu6;        // slot 8: SDU6 (u16 keys)
    std::vector<AnimBlock<i32>> sds3;        // slot 9: SDS3 (i32 keys)
    std::vector<AnimBlock<u32>> sdu3;        // slot 10: SDU3 (u32 keys)
    std::vector<AnimBlock<Flag>> sdfg;       // slot 11: SDFG (Flag keys)
    std::vector<AnimBlock<Extent>> sdmb;     // slot 12: SDMB (Extent keys)
    M3_DEFINE_VERSION_ACCESSORS()
};

/// AnimationGroup – Animation Group (24 bytes)
struct AnimationGroup {
    static constexpr u32 tag = TAG_STG;
    static constexpr u32 max_version = 0;
    std::string name;
    std::vector<u32> subtrackIndices;
    M3_DEFINE_VERSION_ACCESSORS()
};

/// AnimationState – Animation State (28 bytes)
struct AnimationState {
    static constexpr u32 tag = TAG_STS;
    static constexpr u32 max_version = 0;
    std::vector<u32> animIds;
    std::array<u8, 16> unknown;
    M3_DEFINE_VERSION_ACCESSORS()
};

/// BoneAnimationSet – Bone Animation Set (32 bytes)
struct BoneAnimationSet {
    static constexpr u32 tag = TAG_BSET;
    static constexpr u32 max_version = 0;
    Flag flags;
    u16 animationSequenceIndex;
    u16 fallbackSequenceIndex;
    std::string name;
    std::vector<u16> splitItems;
    M3_DEFINE_VERSION_ACCESSORS()
};

// ============================================================================
// Skeleton & Mesh
// ============================================================================

/// Bone – Skeleton Bone (160 bytes)
struct Bone {
    static constexpr u32 tag = TAG_BONE;
    static constexpr u32 max_version = 1;
    u32 unknown;
    std::string name;
    BoneFlag flags = BoneFlag::None; ///< Bone flags
    u16 parentIndex;
    u16 padding;
    AnimRef<Vector3f> position;   // 36 bytes
    AnimRef<Quaternion> rotation; // 44 bytes
    AnimRef<Vector3f> scale;      // 36 bytes
    AnimRef<u32> visibility;      // 20 bytes
    M3_DEFINE_VERSION_ACCESSORS()
};

/// Region – Region / Submesh (v5, 48 bytes)
struct Region {
    static constexpr u32 tag = TAG_REGN;
    static constexpr u32 max_version = 5;
    u32 index;
    u32 unknown;
    u32 firstVertex;
    u32 vertexCount;
    u32 firstIndex;
    u32 indexCount;
    u16 unknown2;
    u16 firstBoneLookup;
    u16 boneLookupCount;
    u16 padding;
    u8 boneWeightPairs;
    u8 boneIndexPairs;
    u16 rootBone;
    RegionFlag flags = RegionFlag::None; ///< Region flags
    f32 uvScale;
    f32 uvOffset;
    M3_DEFINE_VERSION_ACCESSORS()
};

/// Batch – Batch / Draw Call (14 bytes)
struct Batch {
    static constexpr u32 tag = TAG_BAT;
    static constexpr u32 max_version = 1;
    u32 unknown;
    u16 regionIndex;
    u32 unknown2;
    u16 materialIndex;
    u16 boneCount;
    M3_DEFINE_VERSION_ACCESSORS()
};

/// MeshSection – Mesh Section Bounds (80 bytes)
struct MeshSection {
    static constexpr u32 tag = TAG_MSEC;
    static constexpr u32 max_version = 1;
    u32 nodeIndex;
    AnimRef<Extent> bounds;
    M3_DEFINE_VERSION_ACCESSORS()
};

/// MeshDivision – Mesh Divisions (52 bytes)
struct MeshDivision {
    static constexpr u32 tag = TAG_DIV;
    static constexpr u32 max_version = 2;
    std::vector<u16> faces;
    std::vector<Region> regions;
    std::vector<Batch> batches;
    std::vector<MeshSection> msec;
    u32 instances;
    M3_DEFINE_VERSION_ACCESSORS()
};

/// InitialReference – Initial Reference / Bind Pose (64 bytes)
struct InitialReference {
    static constexpr u32 tag = TAG_IREF;
    static constexpr u32 max_version = 0;
    Matrix44f matrix;
    M3_DEFINE_VERSION_ACCESSORS()
};

/// AttachmentPoint – Attachment Point (20 bytes)
struct AttachmentPoint {
    static constexpr u32 tag = TAG_ATT;
    static constexpr u32 max_version = 1;
    u32 unknown;
    std::string name;
    u32 boneIndex;
    M3_DEFINE_VERSION_ACCESSORS()
};

// ============================================================================
// Materials
// ============================================================================

/// MaterialMap – Material Map (8 bytes)
struct MaterialMap {
    static constexpr u32 tag = TAG_MATM;
    static constexpr u32 max_version = 0;
    MaterialType materialType; // enum: 1=standard, 2=displacement, etc.
    u32 materialIndex;
    M3_DEFINE_VERSION_ACCESSORS()
};

/// TextureLayer – Texture Layer (356-468 bytes depending on version)
struct TextureLayer {
    static constexpr u32 tag = TAG_LAYR;
    static constexpr u32 max_version = 26;
    u32 id;
    std::string texturePath;
    AnimRef<ColorBGRA> color;
    TextureLayerFlag flags = TextureLayerFlag::None; ///< Texture layer flags
    UVMappingMode uvMapping = UVMappingMode::ExplicitUV0;
    ColorChannelSelect colorType = ColorChannelSelect::RGB;
    AnimRef<f32> rgbMultiply;
    AnimRef<f32> rgbAdd;
    u32 pocTexture;
    f32 noiseAmplitude; // v24+
    f32 noiseFrequency; // v24+
    u32 textureSource;
    u32 aviFrameRate;
    u32 aviStart;
    u32 aviStop;
    u32 aviLoop;
    u32 aviSync;
    AnimRef<u32> aviPlay;
    AnimRef<u32> aviRestart;
    u32 flipbookRows;
    u32 flipbookColumns;
    AnimRef<u16> currentFrame;
    AnimRef<Vector2f> uvOffset;
    AnimRef<Vector3f> uvAngle;
    AnimRef<Vector2f> uvTiling;
    AnimRef<f32> wOffset;
    AnimRef<f32> wTiling;
    AnimRef<f32> mapAlpha;
    AnimRef<Vector3f> triplanarOffset; // v23+
    AnimRef<Vector3f> triplanarScale;  // v23+
    u32 uvSourceRelated;
    FresnelMode fresnelMode = FresnelMode::None;
    f32 fresnelExponent;
    f32 fresnelMin;
    f32 fresnelMax;
    Vector3f fresnelTranslation; // v25+
    Vector3f fresnelMask;        // v25+
    Vector2f fresnelRotation;    // v25+
    u32 uvDensity;               // v25+
    M3_DEFINE_VERSION_ACCESSORS()
};

/// StandardMaterial – Standard Material (268-352 bytes depending on version)
struct StandardMaterial {
    static constexpr u32 tag = TAG_MAT;
    static constexpr u32 max_version = 20;
    std::string name;
    MaterialAdditionalFlag additionalFlags = MaterialAdditionalFlag::None; ///< Additional flags
    MaterialFlag flags = MaterialFlag::None;                               ///< Material flags
    BlendMode blendMode = BlendMode::Opaque;
    i32 priority;
    u32 rttChannels;
    f32 specularExponent;
    f32 depthBlendFalloff;
    u32 alphaTestThreshold;
    f32 hdrSpecularMultiplier;
    f32 hdrEmissiveMultiplier;
    f32 hdrEnvironmentConstant; // v20 only
    f32 hdrEnvironmentDiffuse;  // v20 only
    f32 hdrEnvironmentSpecular; // v20 only
    // Texture layers (13-18 depending on version)
    std::optional<TextureLayer> diffuseLayer;
    std::optional<TextureLayer> decalLayer;
    std::optional<TextureLayer> specularLayer;
    std::optional<TextureLayer> glossLayer; // v16+
    std::optional<TextureLayer> emissiveLayer1;
    std::optional<TextureLayer> emissiveLayer2;
    std::optional<TextureLayer> environmentLayer;
    std::optional<TextureLayer> environmentMaskLayer;
    std::optional<TextureLayer> alphaLayer1;
    std::optional<TextureLayer> alphaLayer2;
    std::optional<TextureLayer> normalLayer;
    std::optional<TextureLayer> heightLayer;
    std::optional<TextureLayer> lightMapLayer;
    std::optional<TextureLayer> ambientOcclusionLayer;
    std::optional<TextureLayer> normalBlend1MaskLayer; // v19+
    std::optional<TextureLayer> normalBlend2MaskLayer; // v19+
    std::optional<TextureLayer> normalBlend1Layer;     // v19+
    std::optional<TextureLayer> normalBlend2Layer;     // v19+
    MaterialClass materialClass = MaterialClass::Unit;
    LayerBlendOp layerBlendMode = LayerBlendOp::Mod;
    LayerBlendOp emissiveBlendMode1 = LayerBlendOp::Mod;
    LayerBlendOp emissiveBlendMode2 = LayerBlendOp::Mod;
    SpecularMode specularMode = SpecularMode::RGB;
    AnimRef<f32> parallaxHeight;
    AnimRef<f32> motionBlurAmount;
    std::vector<AnimRef<f32>> normalBlendFactors; // v19+
    M3_DEFINE_VERSION_ACCESSORS()
};

/// DisplacementMaterial – Displacement Material (68 bytes)
struct DisplacementMaterial {
    static constexpr u32 tag = TAG_DIS;
    static constexpr u32 max_version = 4;
    std::string name;
    u32 unknown;
    AnimRef<f32> strength;
    std::optional<TextureLayer> normalMap;
    std::optional<TextureLayer> strengthMap;
    Flag flags; ///< Displacement material flags
    u32 priority;
    M3_DEFINE_VERSION_ACCESSORS()
};

/// CompositeSection – Composite Material Section (24 bytes)
struct CompositeSection {
    static constexpr u32 tag = TAG_CMS;
    static constexpr u32 max_version = 0;
    u32 materialIndex;
    AnimRef<f32> mapMultiplier;
    M3_DEFINE_VERSION_ACCESSORS()
};

/// CompositeMaterial – Composite Material (28 bytes)
struct CompositeMaterial {
    static constexpr u32 tag = TAG_CMP;
    static constexpr u32 max_version = 2;
    std::string name;
    u32 priority;
    std::vector<CompositeSection> sections;
    M3_DEFINE_VERSION_ACCESSORS()
};

/// TerrainMaterial – Terrain Material (v1, 28 bytes)
struct TerrainMaterial {
    static constexpr u32 tag = TAG_TER;
    static constexpr u32 max_version = 1;
    std::string name;
    std::optional<TextureLayer> terrainMap;
    u32 unknown;
    M3_DEFINE_VERSION_ACCESSORS()
};

/// VolumeMaterial – Volume Material (84 bytes)
struct VolumeMaterial {
    static constexpr u32 tag = TAG_VOL;
    static constexpr u32 max_version = 0;
    std::string name;
    u32 blendMode;
    VolumeFalloffType falloffType;
    AnimRef<f32> density;
    std::optional<TextureLayer> colorMap;
    std::optional<TextureLayer> noiseMap1;
    std::optional<TextureLayer> noiseMap2;
    u32 alphaThreshold;
    Flag flags; ///< Volume material flags
    M3_DEFINE_VERSION_ACCESSORS()
};

/// HairMaterial – Hair Material (defunct, 116 bytes)
struct HairMaterial {
    static constexpr u32 tag = TAG_HAI;
    static constexpr u32 max_version = 0;
    std::string name;
    std::optional<TextureLayer> layerBase;      // -> LAYR (base color / diffuse)
    std::optional<TextureLayer> layerSpecShift; // -> LAYR (anisotropic specular shift)
    std::optional<TextureLayer> layerSpecNoise; // -> LAYR (specular noise / break-up)
    std::optional<TextureLayer> layerAO;        // -> LAYR (ambient occlusion)
    f32 shiftPrimary;
    f32 shiftSecondary;
    AnimRef<ColorBGRA> colorDiffuse; // Animated diffuse tint
    AnimRef<ColorBGRA> colorSpec;    // Animated specular tint
    f32 specExponent0;
    f32 specExponent1;
    M3_DEFINE_VERSION_ACCESSORS()
};

/// VolumeNoiseMaterial – Volume Noise Material (268 bytes)
struct VolumeNoiseMaterial {
    static constexpr u32 tag = TAG_VON;
    static constexpr u32 max_version = 0;
    std::string name;
    VolumeFalloffType falloffType;
    VolumeNoiseCameraMode drawTransparency;
    AnimRef<f32> density;
    AnimRef<f32> nearPlane;
    AnimRef<f32> falloff;
    std::optional<TextureLayer> colorMap;
    std::optional<TextureLayer> noiseMap1;
    std::optional<TextureLayer> noiseMap2;
    AnimRef<Vector3f> scrollRate;
    AnimRef<Vector3f> position;
    AnimRef<Vector3f> scale;
    AnimRef<Vector3f> rotation;
    u32 alphaThreshold;
    VolumeNoiseMaterialFlag flags; ///< Volume noise material flags
    M3_DEFINE_VERSION_ACCESSORS()
};

/// CreepMaterial – Creep Material (v1, 28 bytes)
struct CreepMaterial {
    static constexpr u32 tag = TAG_CREP;
    static constexpr u32 max_version = 1;
    std::string name;
    std::optional<TextureLayer> maskMap;
    u32 creepLow;
    M3_DEFINE_VERSION_ACCESSORS()
};

/// STBMaterial – STB Material (48 bytes)
struct STBMaterial {
    static constexpr u32 tag = TAG_STBM;
    static constexpr u32 max_version = 0;
    std::string name;
    std::optional<TextureLayer> diffuseMap;
    std::optional<TextureLayer> normalMap;
    std::optional<TextureLayer> specularMap;
    M3_DEFINE_VERSION_ACCESSORS()
};

/// ReflectionMaterial – Reflection Material (84-160 bytes depending on version)
struct ReflectionMaterial {
    static constexpr u32 tag = TAG_REF;
    static constexpr u32 max_version = 3;
    std::string name;
    u32 unknown;
    AnimRef<f32> reflectionStrength;   // v2+
    AnimRef<f32> displacementStrength; // v2+
    AnimRef<f32> reflectionOffset;     // v2+
    AnimRef<f32> blurAngle;            // v2+
    AnimRef<f32> blurDistanceMax;      // v2+
    std::optional<TextureLayer> reflectionMap;
    std::optional<TextureLayer> displacementMap;
    std::optional<TextureLayer> blurMap;
    ReflectionMaterialFlag flags; ///< Reflection material flags (v2+)
    u32 unknown2;
    M3_DEFINE_VERSION_ACCESSORS()
};

/// SubFlare – Sub-Flare (56 bytes)
struct SubFlare {
    static constexpr u32 tag = TAG_LFSB;
    static constexpr u32 max_version = 2;
    u32 index;
    f32 position;
    Vector2f sizeXY;
    Vector2f scaleXY;
    Vector2f fadeIn;
    Vector2f fadeOut;
    ColorBGRA colorAlpha;
    u32 faceCenter;
    Vector2f offset;
    M3_DEFINE_VERSION_ACCESSORS()
};

/// LensFlare – Lens Flare Material (v3, 152 bytes)
struct LensFlare {
    static constexpr u32 tag = TAG_LFLR;
    static constexpr u32 max_version = 3;
    std::string name;
    std::optional<TextureLayer> flareMap;
    std::optional<TextureLayer> maskMap;
    std::vector<SubFlare> subFlares;
    u32 columns;
    u32 rows;
    f32 distanceFade;
    std::string libName;
    AnimRef<f32> intensity;
    AnimRef<ColorBGRA> color;
    AnimRef<f32> hdr;
    AnimRef<f32> size;
    M3_DEFINE_VERSION_ACCESSORS()
};

/// MaterialAddData – Material Additional Data (140-160 bytes)
struct MaterialAddData {
    static constexpr u32 tag = TAG_MADD;
    static constexpr u32 max_version = 3;
    std::string keyName;
    std::vector<u32> keyHash;
    std::vector<u32> extraHash; // v2+
    std::string valuePath;
    std::vector<std::string> valueData;
    std::array<Reference, 4> reserved; // reserved
    f32 frequency;
    f32 intensity;
    f32 holdTime;
    u32 randomHash;
    u32 animationType;
    u32 padding0;
    i32 loopCount;
    u32 flags;
    u32 subType;
    u32 configA;
    u32 configB;
    u32 extraId0; // v3+
    u32 extraId1; // v3+
    M3_DEFINE_VERSION_ACCESSORS()
};

// ============================================================================
// Lights, Cameras, Events
// ============================================================================

/// Light – Light (212 bytes)
struct Light {
    static constexpr u32 tag = TAG_LITE;
    static constexpr u32 max_version = 7;
    LightType lightType; // 0=omni, 1=spot, 2=directional
    u16 boneIndex;
    LightFlag flags = LightFlag::None; ///< Light flags
    u32 lodCut;
    u32 shadowLodCut;
    AnimRef<Vector3f> diffuseColor;
    AnimRef<f32> intensityMultiplier;
    AnimRef<Vector3f> specularColor;
    AnimRef<f32> specularMultiplier;
    AnimRef<f32> decay;
    f32 attenuationEnd;
    AnimRef<f32> attenuationStart;
    AnimRef<f32> hotSpot;
    AnimRef<f32> falloff;
    M3_DEFINE_VERSION_ACCESSORS()
};

/// Camera – Camera (v2=144† β, v3=180, v4=220, v5=264 bytes)
struct Camera {
    static constexpr u32 tag = TAG_CAM;
    static constexpr u32 max_version = 5;
    u32 boneIndex;
    std::string name;
    AnimRef<f32> fieldOfView;         // v2+: radians (e.g. π/4 = 45°)
    u32 useVerticalFOV;               // v2+: boolean (0 or 1)
    u32 dofType;                      // v5 only (default 3 when absent)
    AnimRef<f32> farClip;             // v3+: animatable; v2: promoted from plain f32
    AnimRef<f32> nearClip;            // v3+: animatable; v2: promoted from plain f32
    AnimRef<f32> shadowClipDistance;  // v2+
    AnimRef<f32> focusDistance;       // v2+: DOF focal point distance
    AnimRef<f32> farFocusRange;       // v2+: DOF far focus range
    AnimRef<f32> nearFocusRange;      // v2+: DOF near focus range
    AnimRef<f32> nearFalloffStart;    // v4+
    AnimRef<f32> nearFalloffEnd;      // v4+
    AnimRef<f32> dofAmount;           // v2+: DOF strength (0 = disabled)
    AnimRef<f32> bokehFStop;          // v5+
    AnimRef<f32> bokehMaxCoCDiameter; // v5+
    M3_DEFINE_VERSION_ACCESSORS()
};

// ============================================================================
// Particle Systems
// ============================================================================

/// ParticleEmitter – Particle Emitter (v10=1300 .. v24=1496 bytes depending on version)
struct ParticleEmitter {
    static constexpr u32 tag = TAG_PAR;
    static constexpr u32 max_version = 24;

    // Identification
    u32 boneIndex;
    u32 materialIndex;
    ParticleAdditionalFlag additionalFlags = ParticleAdditionalFlag::None; ///< since v17

    // Initial Velocity
    AnimRef<f32> initialSpeed;
    AnimRef<f32> initialSpeedRandom;
    AnimRef<f32> initialYaw;
    AnimRef<f32> initialPitch;
    AnimRef<f32> initialHorizontal;
    AnimRef<f32> initialVertical;

    // Lifetime
    AnimRef<f32> lifetime;
    AnimRef<f32> lifetimeRandom;

    // Distance & Gravity
    f32 killRadius = 0.0f;
    u32 gravityX = 0; ///< Expected 0
    u32 gravityY = 0; ///< Expected 0
    f32 gravity = 0.0f;

    // Midpoint Timing (since v12; before v12 these appear after their animation fields)
    f32 sizeMidTime = 0.5f;     ///< since v12
    f32 colorMidTime = 0.5f;    ///< since v12
    f32 alphaMidTime = 0.5f;    ///< since v12
    f32 rotationMidTime = 0.5f; ///< since v12

    // Hold Times (since v14)
    f32 sizeMidHoldTime = 0.0f;     ///< since v14
    f32 colorMidHoldTime = 0.0f;    ///< since v14
    f32 alphaMidHoldTime = 0.0f;    ///< since v14
    f32 rotationMidHoldTime = 0.0f; ///< since v14

    // Per-Particle Curves
    AnimRef<Vector3f> sizeAnimation;
    AnimRef<Vector3f> rotationAnimation;
    AnimRef<ColorBGRA> colorStart;
    AnimRef<ColorBGRA> colorMid;
    AnimRef<ColorBGRA> colorEnd;

    // Physics
    f32 drag = 0.0f;
    f32 mass = 0.001f;
    f32 massRandom = 1.0f;
    f32 massSizeMultiplier = 0.0f; ///< since v12

    // Forces
    u16 localForces = 0;
    u16 worldForces = 0;
    u16 localForcesFallback = 0;
    u16 worldForcesFallback = 0;
    f32 worldForcesMassMultiplier = 1.0f; ///< since v24

    // Noise
    f32 noiseAmplitude = 0.0f;
    f32 noiseFrequency = 0.0f;
    f32 noiseCoherence = 0.0f;
    f32 noiseEdge = 0.0f;
    u32 indexPlusLength = 0; ///< since v11

    // Emission
    u32 maxParticles = 0;
    AnimRef<f32> emissionRate;
    EmitterShape emitterShape = EmitterShape::Point;
    AnimRef<Vector3f> shapeOuter;
    AnimRef<Vector3f> shapeInner;
    AnimRef<f32> outerRadius;
    AnimRef<f32> innerRadius;
    std::vector<u32> shapeRegions; ///< since v14: Reference -> U32_

    // Randomization
    u32 velocityType = 0;
    u32 sizeRandomEnable = 0;
    AnimRef<Vector3f> sizeRandomAnimation;
    u32 rotationRandomEnable = 0;
    AnimRef<Vector3f> rotationRandomAnimation;
    u32 colorRandomEnable = 0;
    AnimRef<ColorBGRA> colorStartRandom;
    AnimRef<ColorBGRA> colorMidRandom;
    AnimRef<ColorBGRA> colorEndRandom;
    u32 alphaRandomEnable = 0;

    // Squirt & Flipbook
    AnimRef<u16> squirtAmount;
    u8 flipbookStartInitIndex = 0;
    u8 flipbookStartStopIndex = 0;
    u8 flipbookEndInitIndex = 0;
    u8 flipbookEndStopIndex = 0;
    f32 flipbookMidTime = 0.0f;
    u16 flipbookColumns = 0;
    u16 flipbookRows = 0;
    f32 flipbookColumnFraction = 0.0f; ///< since v12
    f32 flipbookRowFraction = 0.0f;    ///< since v12

    // Collision
    f32 bounce = 0.0f;
    f32 friction = 1.0f;
    i32 collisionSpawnIndex = -1;
    u32 collisionSpawnMin = 0;
    u32 collisionSpawnMax = 0;
    f32 collisionSpawnChance = 0.0f;
    f32 collisionSpawnEnergy = 0.0f;
    u32 collisionDieBounce = 0;

    // Instance
    ParticleInstanceType instanceType = ParticleInstanceType::Billboard;
    f32 tailLength = 1.0f;
    Vector3f instanceAngle;
    f32 instanceDistance = 1.0f; ///< since v17

    // Variation Channels (order: pitch, yaw, speed, size, alpha, color, rotation, horizontal,
    // vertical)
    u32 pitchType = 0;
    AnimRef<f32> pitchAmplitude;
    AnimRef<f32> pitchFrequency;
    u32 yawType = 0;
    AnimRef<f32> yawAmplitude;
    AnimRef<f32> yawFrequency;
    u32 speedType = 0;
    AnimRef<f32> speedAmplitude;
    AnimRef<f32> speedFrequency;
    u32 sizeType = 0;
    AnimRef<f32> sizeAmplitude;
    AnimRef<f32> sizeFrequency;
    u32 alphaType = 0;
    AnimRef<f32> alphaAmplitude;
    AnimRef<f32> alphaFrequency;
    u32 colorType = 0;
    AnimRef<f32> colorAmplitude;
    AnimRef<f32> colorFrequency;
    u32 rotationType = 0;
    AnimRef<f32> rotationAmplitude;
    AnimRef<f32> rotationFrequency;
    u32 horizontalType = 0;
    AnimRef<f32> horizontalAmplitude;
    AnimRef<f32> horizontalFrequency;
    u32 verticalType = 0;
    AnimRef<f32> verticalAmplitude;
    AnimRef<f32> verticalFrequency;

    // Parent Velocity & Phase
    AnimRef<f32> particleVelocity;
    AnimRef<f32> phaseShift; ///< since v22

    // Flags
    ParticleFlag flags = ParticleFlag::None;                         ///< Main particle flags
    ParticleRotationFlag rotationFlags = ParticleRotationFlag::None; ///< since v18

    // Smoothing (since v14)
    InterpolationMode colorSmoothing = InterpolationMode::Linear;
    InterpolationMode sizeSmoothing = InterpolationMode::Linear;
    InterpolationMode rotationSmoothing = InterpolationMode::Linear;

    // UV Screen Space (since v17)
    AnimRef<f32> alphaThreshold;
    AnimRef<Vector2f> uvOffset;
    AnimRef<Vector3f> uvAngle;
    AnimRef<Vector2f> uvTiling;

    // Spline (always present)
    std::vector<AnimRef<Vector3f>> splatLineData; ///< Reference -> SVC3

    // Wind & LOD
    f32 windMultiplier = 0.0f;
    u32 lodReduce = 2;
    u32 lodCut = 0;

    // Bounds
    AnimRef<f32> lowerBound;
    AnimRef<f32> upperBound;

    // Trails
    i32 trailLinkIndex = -1;
    f32 trailChance = 0.0f;
    AnimRef<f32> trailEmissionRate;

    // Splat
    i32 splatProjectionIndex = -1;
    f32 splatChance = 0.0f;

    // References
    std::vector<std::string> modelPaths; ///< Reference -> SCHR
    std::vector<u32> copyIndices;        ///< Reference -> U32_

    // Ribbon-on-bounce (since v23)
    f32 spawnRibbonOnBounceChance = 0.0f; ///< since v23; probability of spawning a ribbon on bounce
    i32 ribbonLinkIndex = -1;             ///< since v23; index into RIB_ array (-1 = none)

    /// Deprecated fields (cannot be migrated to canonical fields)
    struct {
        f32 noiseSmoothness = 0.0f; ///< till v11; deprecated fifth noise parameter
    } deprecated;
    M3_DEFINE_VERSION_ACCESSORS()
};

/// ParticleEmitterCopy – Particle Emitter Copy (40 bytes)
struct ParticleEmitterCopy {
    static constexpr u32 tag = TAG_PARC;
    static constexpr u32 max_version = 0;
    AnimRef<f32> emissionRate;
    AnimRef<u16> squirtAmount;
    u32 boneIndex;
    M3_DEFINE_VERSION_ACCESSORS()
};

/// SplineRibbon – Spline Ribbon (272 bytes)
struct SplineRibbon {
    static constexpr u32 tag = TAG_SRIB;
    static constexpr u32 max_version = 0;
    Vector3f emissionOffset;
    Vector3f emissionVector;
    AnimRef<f32> velocity;
    u32 reserved = 0; ///< Always 0 in all observed corpus data
    u32 boneIndex = 0;
    AnimRef<f32> velocityBaseFactor;
    AnimRef<f32> velocityEndFactor;
    u32 yawType;
    AnimRef<f32> yawAmplitude;
    AnimRef<f32> yawFrequency;
    u32 pitchType;
    AnimRef<f32> pitchAmplitude;
    AnimRef<f32> pitchFrequency;
    u32 velocityType;
    AnimRef<f32> velocityAmplitude;
    AnimRef<f32> velocityFrequency;
    AnimRef<f32> yaw;
    AnimRef<f32> pitch;
    f32 emissionVectorNormFactor = 0.0f; ///< Precomputed ≈ 0.01 / |emissionVector|
    f32 velocityNormFactor = 0.0f;       ///< Precomputed ≈ 0.01 / velocity.initValue
    M3_DEFINE_VERSION_ACCESSORS()
};

/// RibbonEmitter – Ribbon Emitter (v4=744, v5/v6=748, v8=756, v9=760 bytes)
struct RibbonEmitter {
    static constexpr u32 tag = TAG_RIB;
    static constexpr u32 max_version = 9;

    // Identification
    u16 boneIndex = 0;
    u16 boneIndexFallback = 0;
    u32 materialIndex;
    RibbonAdditionalFlag additionalFlags = RibbonAdditionalFlag::None; ///< since v8

    // Initial Velocity
    AnimRef<f32> initialSpeed;
    AnimRef<f32> initialSpeedRandom;
    AnimRef<f32> initialYaw;
    AnimRef<f32> initialPitch;
    AnimRef<f32> initialHorizontal;
    AnimRef<f32> initialVertical;

    // Lifetime
    AnimRef<f32> lifetime;
    AnimRef<f32> lifetimeRandom;
    u32 killRadius = 0;

    // Gravity
    f32 gravityX = 0.0f;
    f32 gravityY = 0.0f;
    f32 gravity;

    // Midpoint timing
    f32 sizeMidTime;
    f32 colorMidTime;
    f32 alphaMidTime;
    f32 rotationMidTime;

    // Hold timing
    f32 sizeMidHoldTime;
    f32 colorMidHoldTime;
    f32 alphaMidHoldTime;
    f32 rotationMidHoldTime;

    // Curves
    AnimRef<Vector3f> sizeAnimation;
    AnimRef<Vector3f> rotationAnimation;
    AnimRef<ColorBGRA> colorStart;
    AnimRef<ColorBGRA> colorMid;
    AnimRef<ColorBGRA> colorEnd;

    // Physics
    f32 drag;
    f32 mass;
    f32 massRandom;
    f32 massSizeMultiplier;
    u16 localForces = 0;
    u16 worldForces = 0;
    u16 localForcesFallback = 0;
    u16 worldForcesFallback = 0;
    f32 worldForcesMassMultiplier = 0.0f;

    // Noise
    f32 noiseAmplitude;
    f32 noiseFrequency;
    f32 noiseCoherence;
    f32 noiseEdge;
    u32 indexPlusLength = 1;

    // Ribbon shape
    u32 emitterShape;
    RibbonType ribbonType = RibbonType::Billboard;
    f32 divisions = 0.0f;
    u32 edges;
    f32 innerRadius;
    AnimRef<f32> maxLength;

    // References
    std::vector<SplineRibbon> splineRibbons;
    AnimRef<u32> active;

    // Flags
    RibbonFlag flags = RibbonFlag::None; ///< Ribbon emitter flags

    // Smoothing
    InterpolationMode sizeSmoothing = InterpolationMode::Linear;
    InterpolationMode colorSmoothing = InterpolationMode::Linear;

    // Collision & LOD
    f32 friction;
    f32 bounce;
    u32 lodReduce;
    u32 lodCut;

    // Variation channels
    u32 yawType;
    AnimRef<f32> yawAmplitude;
    AnimRef<f32> yawFrequency;
    u32 pitchType;
    AnimRef<f32> pitchAmplitude;
    AnimRef<f32> pitchFrequency;
    u32 speedType;
    AnimRef<f32> speedAmplitude;
    AnimRef<f32> speedFrequency;
    u32 sizeType;
    AnimRef<f32> sizeAmplitude;
    AnimRef<f32> sizeFrequency;
    u32 alphaType;
    AnimRef<f32> alphaAmplitude;
    AnimRef<f32> alphaFrequency;

    // Parent velocity & phase
    AnimRef<f32> particleVelocity;
    AnimRef<f32> overlay;

    /// Deprecated fields (cannot be migrated to canonical fields)
    struct {
        i32 unknown3fbae7d6 = 0; ///< till v6
    } deprecated;
    M3_DEFINE_VERSION_ACCESSORS()
};

/// Projector – Projection (388 bytes)
struct Projector {
    static constexpr u32 tag = TAG_PROJ;
    static constexpr u32 max_version = 5;
    ProjectionType projectionType;
    u32 bone;
    u32 materialReferenceIndex;
    AnimRef<Vector3f> offset;
    AnimRef<f32> pitch;
    AnimRef<f32> yaw;
    AnimRef<f32> roll;
    AnimRef<f32> fieldOfView;
    AnimRef<f32> aspectRatio;
    AnimRef<f32> near;
    AnimRef<f32> far;
    AnimRef<f32> boxOffsetZBottom;
    AnimRef<f32> boxOffsetZTop;
    AnimRef<f32> boxOffsetXLeft;
    AnimRef<f32> boxOffsetXRight;
    AnimRef<f32> boxOffsetYFront;
    AnimRef<f32> boxOffsetYBack;
    f32 falloff;
    f32 alphaInit;
    f32 alphaMid;
    f32 alphaEnd;
    f32 lifetimeAttack;
    f32 lifetimeAttackTo;
    f32 lifetimeHold;
    f32 lifetimeHoldTo;
    f32 lifetimeDecay;
    f32 lifetimeDecayTo;
    f32 attenuationDistance;
    AnimRef<u32> active;
    u32 layer;
    u32 lodReduce;
    u32 lodCut;
    ProjectorFlag flags = ProjectorFlag::None;
    M3_DEFINE_VERSION_ACCESSORS()
};

// ============================================================================
// Physics
// ============================================================================

/// Force – Force (104 bytes)
struct Force {
    static constexpr u32 tag = TAG_FOR;
    static constexpr u32 max_version = 2;
    ForceType forceType;
    ForceShape forceShape;
    u32 unknown;
    u32 boneIndex;
    ForceFlag flags = ForceFlag::None; ///< Force flags
    u32 localChannels;
    AnimRef<f32> strength;
    AnimRef<f32> width;
    AnimRef<f32> height;
    AnimRef<f32> length;
    M3_DEFINE_VERSION_ACCESSORS()
};

/// Warp – Warp (132 bytes)
struct Warp {
    static constexpr u32 tag = TAG_WRP;
    static constexpr u32 max_version = 1;
    u32 warpType;
    u32 boneIndex;
    u32 unknown;
    AnimRef<f32> radius;
    AnimRef<f32> height;
    AnimRef<f32> strength;
    AnimRef<f32> angular;
    AnimRef<f32> axial;
    AnimRef<f32> radial;
    M3_DEFINE_VERSION_ACCESSORS()
};

/// ConvexHullHalfEdge – Convex Hull Half-Edge (4 bytes)
/// Half-edge connectivity for PHSH convex hull shapes (shapeType = 4).
/// Entries are stored in consecutive twin pairs (forward 0x01 / reverse 0xFF).
/// The nextAroundVertex field chains half-edges into closed per-vertex rings.
struct ConvexHullHalfEdge {
    static constexpr u32 tag = TAG_DMSE;
    static constexpr u32 max_version = 0;
    u8 type;             ///< 0x01 = forward, 0xFF = reverse (twin)
    u8 faceIndex;        ///< Face this half-edge borders
    u8 vertexIndex;      ///< Target vertex of this half-edge
    u8 nextAroundVertex; ///< Next half-edge around the same vertex
    M3_DEFINE_VERSION_ACCESSORS()
};

/// PhysicsMeshNormal – Physics Mesh Normal (8 or 12 bytes)
struct PhysicsMeshNormal {
    static constexpr u32 tag = TAG_DMMN;
    static constexpr u32 max_version = 1;
    Vector3f normal;
    M3_DEFINE_VERSION_ACCESSORS()
};

/// PhysicsMeshTriangle – Physics Mesh Triangle (28 bytes)
struct PhysicsMeshTriangle {
    static constexpr u32 tag = TAG_DMMT;
    static constexpr u32 max_version = 0;
    u32 vertexIndex0;
    u32 vertexIndex1;
    u32 vertexIndex2;
    u32 edgeIndex0;
    u32 edgeIndex1;
    u32 edgeIndex2;
    u16 reserved;
    u16 flags;
    M3_DEFINE_VERSION_ACCESSORS()
};

/// PhysicsMeshEdge – Physics Mesh Edge (20 bytes)
struct PhysicsMeshEdge {
    static constexpr u32 tag = TAG_DMME;
    static constexpr u32 max_version = 0;
    u32 edgeType;
    u32 vertexA;
    u32 vertexB;
    u32 faceA;
    u32 faceB;
    M3_DEFINE_VERSION_ACCESSORS()
};

/// PhysicsShape – Physics Shape (132-300 bytes depending on version)
///
/// The 300-byte v3 layout is a three-part union.  Bytes 0–79 are the common
/// header.  Bytes 80–103 hold shape dimensions for simple shapes (0–3) or are
/// zero for complex shapes.  Bytes 80–183 form the convex hull section
/// (shapeType 4); bytes 184–299 form the mesh section (shapeType 5).
struct PhysicsShape {
    static constexpr u32 tag = TAG_PHSH;
    static constexpr u32 max_version = 3;
    Matrix44f transform;

    // v1: collisionMargin + shapeType at offsets 64-71
    // v2+: shapeType at offset 64
    f32 collisionMargin; ///< v1 only: Havok convex radius (≈ 0.019685)
    PhysicsShapeType shapeType;
    // Note: 3 bytes alignment padding follow shapeType in the binary layout
    Vector3f oldSizes; // v1 only, zero for shapeType 4–5
    Reference reserved0;
    Vector3f shapeDimensions; // v2+: shape dimensions for simple shapes (0–3), zero for complex
                              // shapes (4–5)

    // --- Convex Hull section (binary offsets 80–183, shapeType = 4 only) ---
    // hullReserved0 [80] always empty (not stored)
    // hullReserved1 [92] always empty (not stored)
    std::vector<Vector3f> hullFaceNormals;         // [104] -> VEC3  per-face unit normals
    std::vector<Vector4f> hullVertexPositions;     // [116] -> VEC4  vertex positions (w=0)
    std::vector<ConvexHullHalfEdge> hullHalfEdges; // [128] -> DMSE  half-edge table
    std::vector<u8> hullVertexFaceIndices;         // [140] -> U8__  one face index per vertex
    Vector3f hullCenter;                           // [152]
    u32 hullFaceNormalCount;                       // [164]
    u32 hullVertexCount;                           // [168]
    u32 hullHalfEdgeCount;                         // [172]
    f32 hullUnknown0;                              // [176]
    f32 hullUnknown1;                              // [180]

    // --- Mesh section (binary offsets 184–299, shapeType = 5 only) ---
    std::vector<PhysicsMeshNormal> meshFaceNormals;    // [184] -> DMMN
    std::vector<Vector4f> meshVertexPositions;         // [196] -> VEC4  vertex positions (w=0)
    std::vector<std::array<u16, 7>> meshFaceIndices16; // [208] -> MT16  (or empty)
    std::vector<std::array<u32, 7>> meshFaceIndices32; // [220] -> MT32  (or empty)
    // MT32/MT16 per-entry layout: {v0, v1, v2, adj0, adj1, adj2, flags}
    Vector3f meshBoundsCenter; // [232]
    Vector3f meshBoundsExtent; // [244]
    Vector3f meshTolerance;    // [256]
    u32 meshNormalCount;       // [268]
    u32 meshVertexCount;       // [272]
    u32 meshFaceIndex16Count;  // [276] Mirrors meshFaceIndices16.count (0 when MT32)
    u32 meshFaceIndex32Count;  // [280] Mirrors meshFaceIndices32.count (0 when MT16)
    u32 meshUnknown1;          // [284]
    u32 meshReserved;          // [288] Always 0
    u32 meshTreeDepth;         // [292] Correlates with mesh complexity (1–12)
    f32 meshCollisionMargin;   // [296] MT16: small float; MT32: 0.0

    struct {
        struct {
            std::vector<PhysicsMeshNormal> meshFaceNormals; // -> only v2
            std::vector<Vector3f> meshVertexPositions;      // -> only v2
            std::vector<PhysicsMeshTriangle> unknown;       // -> only v2
            std::vector<PhysicsMeshEdge> unknown2;          // -> only v2
        } v2;
        struct {
            std::vector<Vector3f> legacyVertices; ///< till v1: convex hull / mesh vertices
            std::vector<u8> unknown0;             ///< till v1: convex hull unknown data
            std::vector<u16> faceIndices;         ///< till v1: mesh face indices
            std::vector<Vector4f> planeEquations; ///< till v1: convex hull plane equations
            Vector3f halfExtents;                 ///< till v1: shape bounding half-extents
        } v1;
    } deprecated;
    M3_DEFINE_VERSION_ACCESSORS()
};

/// RigidBody – Rigid Body (v2=104, v3=56, v4=80 bytes)
struct RigidBody {
    static constexpr u32 tag = TAG_PHRB;
    static constexpr u32 max_version = 4;
    u16 simulationType; ///< v3+: simulation mode
    u16 parentBoneIndex;
    u32 physicsType; ///< v3+: engine-specific rigid body type
    f32 density;
    f32 friction;
    f32 restitution;
    f32 linearDamping;
    f32 angularDamping;
    f32 gravityScale;
    AnimRef<u32> dynamicState;                 ///< since v4
    f32 dynamicBlendOut;                       ///< since v4
    std::vector<PhysicsShape> rigidBodyShape;  ///< all versions (v2+ confirmed)
    RigidBodyFlag flags = RigidBodyFlag::None; ///< Rigid body flags
    u16 localForces;                           ///< Bitmask referencing local force channels
    u16 worldForces;                           ///< Bitmask referencing world force channels
    u32 priority;

    struct {
        /// v2: 3x3 symmetric inertia tensor (diagonal = moments, off-diagonal = products)
        std::array<std::array<f32, 3>, 3> inertiaTensor = {}; ///< till v2: 36 bytes
        u16 boneIndex;                    ///< till v2: typically same as parentBoneIndex
        std::array<u32, 4> reserved = {}; ///< till v2: always 0
    } deprecated;
    M3_DEFINE_VERSION_ACCESSORS()
};

/// PhysicsJoint – Physics Joint (180 bytes)
struct PhysicsJoint {
    static constexpr u32 tag = TAG_PHYJ;
    static constexpr u32 max_version = 0;
    u32 jointType;
    u32 boneIndex1;
    u32 boneIndex2;
    Matrix44f matrixBody1;
    Matrix44f matrixBody2;
    u32 enableLimits;
    f32 limitMin;
    f32 limitMax;
    f32 coneAngle;
    u32 enableFriction;
    f32 friction;
    f32 dampingRatio;
    f32 angularFrequency;
    f32 breakThreshold;
    u8 enableShape;
    M3_DEFINE_VERSION_ACCESSORS()
};

/// PhysicsConstraint – Physics Constraint (24 bytes)
struct PhysicsConstraint {
    static constexpr u32 tag = TAG_PHCT;
    static constexpr u32 max_version = 0;
    std::vector<u16> dependents;
    u16 rigidBody1;
    u16 rigidBody2;
    Flag flags;
    f32 breakForce;
    M3_DEFINE_VERSION_ACCESSORS()
};

/// ClothCollider – Cloth Collider (76 bytes)
struct ClothCollider {
    static constexpr u32 tag = TAG_PHCC;
    static constexpr u32 max_version = 0;
    Matrix44f transform;
    f32 radius;
    f32 height;
    u32 padding;
    M3_DEFINE_VERSION_ACCESSORS()
};

/// ClothProxy – Cloth Proxy (32 bytes)
struct ClothProxy {
    static constexpr u32 tag = TAG_PHAC;
    static constexpr u32 max_version = 0;
    u32 proxyIndex;
    u32 clothIndex;
    std::vector<u64> proxyVertices;
    std::vector<u32> proxyWeights;
    M3_DEFINE_VERSION_ACCESSORS()
};

/// ClothPhysics – Cloth Physics (v4, 192 bytes)
struct ClothPhysics {
    static constexpr u32 tag = TAG_PHCL;
    static constexpr u32 max_version = 4;
    u32 clothMeshCount;
    u32 skinBoneCount;
    std::vector<u16> skinBones;
    std::vector<u8> simEnabled;
    std::vector<u32> vertexBones;
    std::vector<u32> vertexWeights;
    std::vector<ClothCollider> colliders;
    std::vector<ClothProxy> proxies;
    f32 density;
    f32 tracking;
    f32 stretchStiffness;
    f32 horizontalStiffness;
    f32 bendingStiffness;
    f32 damping;
    f32 friction;
    f32 gravity;
    f32 explosionScale;
    f32 windScale;
    f32 shearStiffness;
    f32 dragFactor;
    f32 liftFactor;
    f32 sphereStiffness;
    u32 flatten;
    AnimRef<u32> active;
    u32 useSkinCollision;
    f32 skinOffset;
    f32 skinExponent;
    f32 skinStiffness;
    u32 localChannels;
    Vector3f localWind;
    M3_DEFINE_VERSION_ACCESSORS()
};

// ============================================================================
// Miscellaneous
// ============================================================================

/// HitTestShape – Hit Test Shape (108 bytes)
struct HitTestShape {
    static constexpr u32 tag = TAG_SSGS;
    static constexpr u32 max_version = 1;
    HitTestShapeType shapeType;
    u16 boneIndex;
    u16 padding;
    Matrix44f transform;
    std::vector<Vector3f> vertexPositions;
    std::vector<u16> faceIndices;
    f32 sizeX;
    f32 sizeY;
    f32 sizeZ;
    M3_DEFINE_VERSION_ACCESSORS()
};

/// AttachmentVolume – Attachment Volume (116 bytes)
struct AttachmentVolume {
    static constexpr u32 tag = TAG_ATVL;
    static constexpr u32 max_version = 0;
    u32 bone1;
    u32 bone2;
    HitTestShapeType shapeType;
    u16 boneIndex;
    u16 padding;
    Matrix44f transform;
    std::vector<Vector3f> vertexPositions;
    std::vector<u16> faceIndices;
    f32 sizeX;
    f32 sizeY;
    f32 sizeZ;
    M3_DEFINE_VERSION_ACCESSORS()
};

/// TriggerData – Trigger Data (24 bytes)
struct TriggerData {
    static constexpr u32 tag = TAG_TRGD;
    static constexpr u32 max_version = 0;
    std::vector<u32> dataIndices;
    std::string name;
    M3_DEFINE_VERSION_ACCESSORS()
};

/// TurretBehavior – Turret Behavior (152 bytes)
struct TurretBehavior {
    static constexpr u32 tag = TAG_PATU;
    static constexpr u32 max_version = 4;
    Matrix44f transform;
    Vector4f unknown1;
    Vector4f unknown2;
    u16 boneIndex;
    u8 useAsMainTurret;
    u8 turretGroupId;
    u32 yawLimited;
    f32 yawMin;
    f32 yawMax;
    f32 yawWeight;
    u32 pitchLimited;
    f32 pitchMin;
    f32 pitchMax;
    f32 pitchWeight;
    f32 unknown3;
    f32 unknown4;
    Vector3f mainBoneOffset;
    M3_DEFINE_VERSION_ACCESSORS()
};

/// BillboardBehavior – Billboard Behavior (48 bytes)
struct BillboardBehavior {
    static constexpr u32 tag = TAG_BBSC;
    static constexpr u32 max_version = 0;
    std::vector<u16> dependents;
    u16 boneIndex;
    u8 billboardType;
    u8 cameraLookAt = 1;
    Quaternion up;
    Quaternion forward;
    M3_DEFINE_VERSION_ACCESSORS()
};

/// IKJoint – IK Joint (32 bytes)
struct IKJoint {
    static constexpr u32 tag = TAG_IKJT;
    static constexpr u32 max_version = 0;
    std::vector<u16> dependents;
    u16 boneIndex1;
    u16 boneIndex2;
    f32 raycastUp;
    f32 raycastDown;
    f32 maxSpeed;
    f32 goalThreshold;
    M3_DEFINE_VERSION_ACCESSORS()
};

/// IKTwoJoint – Two-Joint IK Solver (48 bytes)
struct IKTwoJoint {
    static constexpr u32 tag = TAG_IK2J;
    static constexpr u32 max_version = 0;
    std::vector<u16> dependents;
    u16 boneBase;
    u16 boneTarget;
    u16 boneEnd;
    u16 padding;
    Vector3f hingeAxis;
    f32 maxAngleInner;
    f32 maxAngleOuter;
    f32 searchUp;
    f32 searchDown;
    M3_DEFINE_VERSION_ACCESSORS()
};

/// IKCCD – CCD IK Solver (24 bytes)
struct IKCCD {
    static constexpr u32 tag = TAG_IKCC;
    static constexpr u32 max_version = 0;
    std::vector<u16> dependents;
    u16 boneBase;
    u16 boneTarget;
    f32 searchUp;
    f32 searchDown;
    M3_DEFINE_VERSION_ACCESSORS()
};

/// OneBoneSolver – One-Bone IK Solver (24 bytes)
struct OneBoneSolver {
    static constexpr u32 tag = TAG_PAOB;
    static constexpr u32 max_version = 0;
    std::vector<u16> dependents;
    u16 bone;
    u16 boneFallback;
    Flag flags;
    f32 maxAngle;
    M3_DEFINE_VERSION_ACCESSORS()
};

/// ShadowBox – Shadow Box (64 bytes)
struct ShadowBox {
    static constexpr u32 tag = TAG_SHBX;
    static constexpr u32 max_version = 0;
    Matrix44f matrix;
    M3_DEFINE_VERSION_ACCESSORS()
};

/// ViewVolume – View Volume (40 bytes)
struct ViewVolume {
    static constexpr u32 tag = TAG_VVOL;
    static constexpr u32 max_version = 0;
    u32 nodeIndex;          // Index into BONE
    AnimRef<Vector3f> size; // Animated half-extents (36 bytes)
    M3_DEFINE_VERSION_ACCESSORS()
};

struct TrailingModel {
    static constexpr u32 tag = TAG_TMD;
    static constexpr u32 max_version = 1;
    std::vector<Vector3f> vectors; // -> VEC3
    f32 param0;                    // Observed 5.0
    f32 param1;                    // Observed 1.0
    AnimRef<f32> animFloat0;       // init 0.5
    AnimRef<f32> animFloat1;       // init 1.0
    u32 flag;                      // Observed 1
    u32 reserved0;
    u32 reserved1;
    M3_DEFINE_VERSION_ACCESSORS()
};

// ============================================================================
// Model Root
// ============================================================================

/// Model – Model Root (v30, 868 bytes)
struct Model {
    static constexpr u32 tag = TAG_MODL;
    static constexpr u32 max_version = 30;

    // Preamble (0x000–0x0E3, 228 bytes) - identical across all versions
    std::string name;
    ModelFlag flags = ModelFlag::None; ///< Model flags
    std::vector<Sequence> sequences;
    std::vector<SubTrackContainer> subTrackCollections;
    std::vector<AnimationGroup> animationGroups;
    std::vector<BoneAnimationSet> boneAnimationSets;
    u32 animationSplitCount;
    std::vector<AnimationState> animationStates;
    std::vector<Bone> bones;
    u32 skinBoneCount;
    VertexBuffer vertices;
    std::vector<MeshDivision> divisions;
    std::vector<u16> boneLookup;
    Extent bounds;
    Extent collisionBounds;
    std::vector<u16> collisionFaces;
    std::vector<Vector3f> collisionVerts;
    std::vector<Vector3f> collisionNormals;

    // Variable region (0x0E4+) - version-dependent
    // Scene objects
    std::vector<AttachmentPoint> attachmentPoints;
    std::vector<u16> attachmentPointAddons;
    std::vector<Light> lights;
    std::vector<ShadowBox> shadowBoxes;
    std::vector<Camera> cameras;
    std::vector<u16> camerasAddons;

    // Materials
    std::vector<MaterialMap> materialMaps;
    std::vector<StandardMaterial> standardMaterials;
    std::vector<DisplacementMaterial> displacementMaterials;
    std::vector<CompositeMaterial> compositeMaterials;
    std::vector<TerrainMaterial> terrainMaterials;
    std::vector<VolumeMaterial> volumeMaterials;
    std::vector<HairMaterial> hairMaterials; // -> HAI_ (defunct, always NULL)
    std::vector<CreepMaterial> creepMaterials;
    std::vector<VolumeNoiseMaterial> volumeNoiseMaterials; // v25+ -> VON_
    std::vector<STBMaterial> stbMaterials;                 // v26+
    std::vector<ReflectionMaterial> reflectionMaterials;   // v28+
    std::vector<LensFlare> lensFlareMaterials;             // v29+
    std::vector<MaterialAddData> materialAddData;          // v30+

    // Particle systems
    std::vector<ParticleEmitter> particleEmitters;
    std::vector<ParticleEmitterCopy> particleEmitterCopies;
    std::vector<RibbonEmitter> ribbonEmitters;
    std::vector<Projector> projections;
    std::vector<Force> forces;
    std::vector<Warp> warps;
    std::vector<ViewVolume> viewVolumes;

    // Physics
    std::vector<RigidBody> rigidBodies;
    std::vector<PhysicsConstraint> physicsConstraints;
    std::vector<PhysicsJoint> physicsJoints;
    std::vector<ClothPhysics> clothPhysics; // v28+
    std::vector<IKTwoJoint> ikTwoJoints;
    std::vector<IKCCD> ikCCD; // v24+
    std::vector<IKJoint> ikJoints;
    std::vector<OneBoneSolver> oneBoneSolvers;

    // Behaviors & data
    std::vector<TurretBehavior> turretBehaviors;
    std::vector<TriggerData> triggerData;
    std::vector<InitialReference> initialReference;

    // Inline hit-test structure (108 bytes)
    HitTestShape tightHitTestObject; // v28+ - extracted from inline hit-test data

    // Trailing references
    std::vector<HitTestShape> fuzzyHitTestObjects;
    std::vector<AttachmentVolume> attachmentVolumes;
    std::vector<u16> attachmentVolumesAddon0;
    std::vector<u16> attachmentVolumesAddon1;
    std::vector<BillboardBehavior> billboardBehaviors;
    std::vector<TrailingModel> trailingModels;
    u32 m3aAnimHash;
    std::vector<u32> m3aAnimHashes;
    M3_DEFINE_VERSION_ACCESSORS()
};

#undef M3_DEFINE_VERSION_ACCESSORS

} // namespace m3
} // namespace whiteout
