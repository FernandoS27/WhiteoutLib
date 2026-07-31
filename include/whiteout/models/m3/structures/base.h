// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file base.h
 * @brief Enumerations, flag types, and utility macros for M3 structures
 *
 * Defines all enumeration classes (MaterialType, LightType, PhysicsShapeType,
 * EmitterShape, etc.), bitfield flag enums with operator overloads
 * (ModelFlag, BoneFlag, MaterialFlag, ParticleFlag, RibbonFlag, etc.),
 * and the M3_DEFINE_VERSION_ACCESSORS() macro used by every versioned struct.
 */

#include "../types.h"

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

/** @brief Identifies which material array a MATM entry references */
enum class MaterialType : u32 {
    Standard = 1,         ///< MAT_ — Standard material
    Displacement = 2,     ///< DIS_ — Displacement material
    Composite = 3,        ///< CMP_ — Composite material
    Terrain = 4,          ///< TER_ — Terrain material
    Volume = 5,           ///< VOL_ — Volume material
    VolumeNoise = 6,      ///< VON_ — Volume noise material
    Creep = 7,            ///< CREP — Creep material
    Hair = 8,             ///< HAI_ — Hair material (defunct)
    SplatTerrainBake = 9, ///< STBM — Splat terrain bake material
    Reflection = 10,      ///< REF_ — Reflection material
    LensFlare = 11,       ///< LFLR — Lens flare material
    BufferMaterial = 12,  ///< MADD — Buffer / additional material data
};

/** @brief Light source type (LITE) */
enum class LightType : u16 {
    Omni = 0,        ///< Point light (omnidirectional)
    Spot = 1,        ///< Spot light (cone)
    Directional = 2, ///< Directional light (infinite distance)
};

/** @brief Physics collision shape type (PHSH) */
enum class PhysicsShapeType : u8 {
    Box = 0,        ///< Box (half-extents in shapeDimensions)
    Sphere = 1,     ///< Sphere (radius in shapeDimensions.x)
    Capsule = 2,    ///< Capsule (radius + height)
    Cylinder = 3,   ///< Cylinder (radius + height)
    ConvexHull = 4, ///< Convex hull (vertex/half-edge data)
    Mesh = 5,       ///< Triangle mesh (face/edge/normal data)
};

/** @brief Hit-test shape type (SSGS / ATVL), same semantics as PhysicsShapeType but u32 */
enum class HitTestShapeType : u32 {
    Box = 0,
    Sphere = 1,
    Capsule = 2,
    Cylinder = 3,
    Mesh = 4,
};

/** @brief Particle / ribbon emitter shape (PAR_ / RIB_) */
enum class EmitterShape : u32 {
    Point = 0,    ///< Emit from a single point
    Plane = 1,    ///< Emit from a rectangular plane
    Sphere = 2,   ///< Emit from a sphere surface/volume
    Box = 3,      ///< Emit from a box volume
    Cylinder = 4, ///< Emit from a cylinder
    Disc = 5,     ///< Emit from a disc
    Spline = 6,   ///< Emit from a spline path, splineLineData
    Mesh = 7,     ///< Emit from mesh surface, mesh region indices in shapeRegions
};

/** @brief Particle visual / billboard type (maps to b_iInstanceType in Particle.fx) */
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

/** @brief Force influence type (FOR_) */
enum class ForceType : u32 {
    Radial = 0,    ///< Radial force (outward from center)
    Wind = 1,      ///< Wind force (directional)
    Explosion = 2, ///< Explosion force (impulse)
};

/** @brief Influence volume shape for a force (FOR_) */
enum class ForceShape : u32 {
    Sphere = 0,     ///< Spherical influence volume
    Cylinder = 1,   ///< Cylindrical influence volume
    Box = 2,        ///< Box influence volume
    Hemisphere = 3, ///< Hemispherical influence volume
};

/** @brief Ribbon cross-section type (maps to b_iRibbonType in Ribbon.fx) */
enum class RibbonType : u32 {
    Billboard = 0, ///< Camera-facing ribbon strip
    Planar = 1,    ///< Flat/planar ribbon strip
    Cylinder = 2,  ///< Cylindrical cross-section
    Star = 3,      ///< Star-shaped cross-section
};

/** @brief Projector / decal projection type (PROJ) */
enum class ProjectionType : u32 {
    Orthographic = 0, ///< Orthographic projection
    Perspective = 1,  ///< Perspective projection
};

/** @brief Volume shape type (VOL_ / VON_) */
enum class VolumeType : u32 {
    Box = 0,
    Sphere = 1,
    Capsule = 2,
};

/**
 * @brief Interpolation mode for particle/ribbon smoothing curves
 *
 * Maps to RibbonParticleCommon.fx constants. Used by PAR_
 * colorSmoothing / sizeSmoothing / rotationSmoothing and RIB_
 * sizeSmoothing / colorSmoothing.
 */
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

/**
 * @brief Defines bitwise operators (|, &, |=, &=, ~) and hasFlag() for a flag enum
 * @param FlagType The enum class type
 * @param UnderlyingType The underlying integer type (u8, u16, u32)
 */
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

/** @brief Model-wide flags (MODL.flags) — tangents, FOW, instancing, etc. */
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

/** @brief Sequence playback flags (SEQS.flags) */
enum class SequenceFlag : u32 {
    None = 0x0,
    NotLooping = 0x1,        ///< Sequence does not loop
    AlwaysGlobal = 0x2,      ///< Always plays globally
    Unknown0x4 = 0x4,        ///< Unknown
    GlobalInPreviewer = 0x8, ///< Global playback in editor
};
M3_DEFINE_FLAG_OPS(SequenceFlag, u32)

/** @brief Bone flags (BONE.flags) — inheritance, billboard, IK, skin */
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

/** @brief Region flags (REGN.flags, v4+) */
enum class RegionFlag : u32 {
    None = 0x0,
    Hidden = 0x1,          ///< Region is hidden
    Placeholder = 0x2,     ///< Placeholder region
    ClothSimulated = 0x4,  ///< Cloth-simulated
    ClothInfluenced = 0x8, ///< Cloth-influenced
};
M3_DEFINE_FLAG_OPS(RegionFlag, u32)

/** @brief Additional standard-material flags (MAT_.additionalFlags) */
enum class MaterialAdditionalFlag : u32 {
    None = 0x0,
    DepthBlendFalloff = 0x1, ///< Enable depth blend falloff
    VertexColor = 0x4,       ///< Uses vertex color
    VertexAlpha = 0x8,       ///< Uses vertex alpha
};
M3_DEFINE_FLAG_OPS(MaterialAdditionalFlag, u32)

/** @brief Standard material rendering flags (MAT_.flags) */
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

/** @brief Texture layer flags (LAYR.flags) */
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

/** @brief Blend mode for materials (MAT_.blendMode, VOL_.blendMode) */
enum class BlendMode : u32 {
    Opaque = 0,     ///< Fully opaque
    AlphaBlend = 1, ///< Standard alpha blending
    Add = 2,        ///< Additive blending
    AlphaAdd = 3,   ///< Alpha-modulated additive
    Mod = 4,        ///< Multiplicative blending
    Mod2x = 5,      ///< Double multiplicative
};

/** @brief Material rendering class (MAT_.materialClass) */
enum class MaterialClass : u32 {
    Unit = 0,      ///< Unit/character material
    Building = 1,  ///< Building/structure material
    Doodad = 2,    ///< Doodad/prop material
    SpecialFX = 3, ///< Special effect material
};

/** @brief Layer blend operation (MAT_.layerBlendMode, emissiveBlendMode) */
enum class LayerBlendOp : u32 {
    Mod = 0,                  ///< Multiply: base * layer
    Mod2x = 1,                ///< Double multiply: base * layer * 2
    Add = 2,                  ///< Add: base + layer
    Lerp = 3,                 ///< Linear interpolate by layer alpha
    TeamColorEmissiveAdd = 4, ///< Team color emissive add
    TeamColorDiffuseAdd = 5,  ///< Team color diffuse add
    AddNoAlpha = 6,           ///< Add ignoring alpha channel
};

/** @brief UV mapping source / projection mode (LAYR.uvMapping) */
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

/** @brief Color channel selection (LAYR.colorType) */
enum class ColorChannelSelect : u32 {
    RGB = 0,   ///< Use RGB channels (alpha forced to 1)
    RGBA = 1,  ///< Use all RGBA channels
    Alpha = 2, ///< Use alpha channel only (splat to all)
    Red = 3,   ///< Use red channel only (splat to all)
    Green = 4, ///< Use green channel only (splat to all)
    Blue = 5,  ///< Use blue channel only (splat to all)
};

/** @brief Specular mode (MAT_.specularMode) */
enum class SpecularMode : u32 {
    RGB = 0,       ///< Use RGB channels for specularity
    AlphaOnly = 1, ///< Use alpha channel only
};

/** @brief Fresnel effect mode (LAYR.fresnelMode) */
enum class FresnelMode : u32 {
    None = 0,     ///< No fresnel effect
    Standard = 1, ///< Standard fresnel (edge glow)
    Inverted = 2, ///< Inverted fresnel (center glow)
};

/** @brief Reflection material flags (REF_.flags, v2+) */
enum class ReflectionMaterialFlag : u32 {
    None = 0x0,
    UseReflectionMap = 0x1,        ///< Use reflection map
    UseDisplacementMap = 0x2,      ///< Use displacement map
    RenderInTransparentPass = 0x4, ///< Render in transparent pass
    Blurring = 0x8,                ///< Enable blurring
    UseBlurMap = 0x10,             ///< Use blur map
};

/** @brief Volume noise material flags (VON_.flags) */
enum class VolumeNoiseMaterialFlag : u32 {
    None = 0x0,
    DrawAfterTransparency = 0x1, ///< Draw in separate pass after transparency
};
M3_DEFINE_FLAG_OPS(VolumeNoiseMaterialFlag, u32)

/** @brief Volume density falloff type (VOL_.falloffType, VON_.falloffType) */
enum class VolumeFalloffType : u32 {
    Linear = 0,      ///< Linear density falloff
    Exponential = 1, ///< Exponential density falloff
};

/** @brief Volume noise camera position mode (VON_.drawTransparency) */
enum class VolumeNoiseCameraMode : u32 {
    Outside = 0, ///< Camera is outside the volume
    Inside = 1,  ///< Camera is inside the volume
};

/** @brief Light flags (LITE.flags) */
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

/** @brief Particle emitter main flags (PAR_.flags) */
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
    LitParts = 0x8000,                  ///< Lit particles → lit pixel-shader variant
    RandomFlipbookStart = 0x10000,      ///< Random flipbook start → shader b_randomFlipBookStart
    MultiplyGravityByMass = 0x20000,    ///< Multiply gravity by mass
    ClampTailLength = 0x40000,          ///< Clamp tail length → shader b_clampedTailLength (Tail/Trail, not Pinned)
    SpawnTrailingParticles = 0x80000,   ///< Spawn trailing particles (also forces b_useProceduralPosition)
    FixTailLengthOnCreation = 0x100000, ///< Fix tail length on creation → shader b_fixedTailLength
    UseVertexAlpha = 0x200000,          ///< Use vertex alpha
    ModelParticles = 0x400000,          ///< Use model particles (also forces b_useProceduralPosition)
    SwapYZOnModelParticles = 0x800000,  ///< Swap Y/Z on model particles
    ScaleTimeByParent = 0x1000000,      ///< Scale time by parent
    UseLocalTime = 0x2000000,           ///< Use local time
    SimulateInit = 0x4000000,           ///< Simulate on initialization
    Copy = 0x8000000,                   ///< Copy emitter
    // Bits >= 0x10000000: consumed by CParticleSystem::SetupShaderFlags (SC2 71061),
    // not in the community flag set — provisional names.
    RequiresGpuSim = 0x10000000,          ///< Part of the b_useProceduralPosition trigger mask (0x10480003)
    ShaderPerm30 = 0x40000000,            ///< Toggles a particle shader permutation (role TBD)
    ForceProceduralPosition = 0x80000000, ///< Forces GPU procedural-position path (b_useProceduralPosition)
};
M3_DEFINE_FLAG_OPS(ParticleFlag, u32)

/** @brief Particle emitter additional flags (PAR_.additionalFlags, v17+) */
enum class ParticleAdditionalFlag : u32 {
    None = 0x0,
    EmitSpeedRandomize = 0x1, ///< Randomize emission speed
    LifespanRandomize = 0x2,  ///< Randomize lifespan
    MassRandomize = 0x4,      ///< Randomize mass
    WorldSpace = 0x8,         ///< World-space coordinates
};
M3_DEFINE_FLAG_OPS(ParticleAdditionalFlag, u32)

/** @brief Particle rotation flags (PAR_.rotationFlags, v18+) */
enum class ParticleRotationFlag : u32 {
    None = 0x0,
    Relative = 0x2,  ///< Relative rotation
    AlwaysSet = 0x4, ///< Always set
};
M3_DEFINE_FLAG_OPS(ParticleRotationFlag, u32)

/** @brief Ribbon emitter main flags (RIB_.flags) */
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

/** @brief Ribbon emitter additional flags (RIB_.flags2, v8+) */
enum class RibbonAdditionalFlag : u32 {
    None = 0x0,
    SpeedRandomize = 0x01,    ///< Randomize emission speed
    LifespanRandomize = 0x02, ///< Randomize lifespan
    MassRandomize = 0x04,     ///< Randomize mass
    WorldSpace = 0x08,        ///< World-space coordinates
};
M3_DEFINE_FLAG_OPS(RibbonAdditionalFlag, u32)

/** @brief Projector flags (PROJ.flags) */
enum class ProjectorFlag : u32 {
    None = 0x0,
    Static = 0x1,         ///< Static position
    UnknownFlag0x2 = 0x2, ///< Unknown
    UnknownFlag0x4 = 0x4, ///< Unknown
    UnknownFlag0x8 = 0x8, ///< Unknown
};
M3_DEFINE_FLAG_OPS(ProjectorFlag, u32)

/** @brief Force flags (FOR_.flags) */
enum class ForceFlag : u32 {
    None = 0x0,
    Falloff = 0x01,        ///< Distance falloff
    HeightGradient = 0x02, ///< Height gradient
    Unbounded = 0x04,      ///< Unbounded range
};
M3_DEFINE_FLAG_OPS(ForceFlag, u32)

/** @brief Rigid body flags (PHRB.flags) */
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

/**
 * @brief Injects private version storage with getVersion() / setVersion() accessors
 *
 * Every versioned M3 struct uses this macro to store a version field,
 * set once by the parser and readable thereafter. -1 indicates uninitialized.
 */
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

} // namespace m3
} // namespace whiteout
