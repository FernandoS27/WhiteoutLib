// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file base.h
 * @brief Core M2 data structures from the MD20 header
 *
 * This file defines all structures that appear within the MD20 header payload:
 * - Global flags and sequences
 * - Animation sequences with timing and blending data
 * - Vertex, bone, and skeleton hierarchy
 * - Textures, materials, and render flags
 * - Color animations and texture transforms
 * - Lights, cameras, and attachments
 * - Ribbon and particle emitters
 * - Events (sound triggers, camera shakes, etc.)
 * - The MD20Header itself (complete model header)
 *
 * Each structure corresponds to an M2Array in the MD20 header. On disk,
 * these are referenced by count/offset pairs; WhiteoutLib resolves them
 * into std::vector at parse time.
 *
 * @see M2_FILE_FORMAT_SPECIFICATION.md for binary layout details
 */

#include "../types.h"

namespace whiteout {
namespace m2 {

// ============================================================================
// Global Flags
// ============================================================================

/**
 * @brief Bit flags from the MD20 header's globalFlags field (offset 0x010)
 *
 * These flags control various model behaviors including physics loading,
 * particle record format, animation file format, and multitexturing.
 */
enum class GlobalFlag : u32 {
    None = 0,
    TiltX = 0x00000001, ///< Model tilts along X axis
    TiltY = 0x00000002, ///< Model tilts along Y axis
    UseTextureCombinerCombos =
        0x00000008,                 ///< Appends textureCombinerCombos array; enables multitexturing
    LoadPhysicsData = 0x00000020,   ///< Model has associated .phys or PFDC physics data
    Unk_0x80 = 0x00000080,          ///< Universal in modern files; purpose unclear
    CameraRelated = 0x00000100,     ///< Camera-related; only on models with camera definitions
    NewParticleRecord = 0x00000200, ///< Extended 492-byte particle record (instead of 476)
    Unk_0x400 = 0x00000400,         ///< Unknown
    TextureTransformsUsesBoneSequences =
        0x00000800,                ///< Texture transforms use bone's playing sequence
    Unk_0x1000 = 0x00001000,       ///< Unknown
    ChunkedAnimFiles = 0x00002000, ///< .anim files use chunked AFM2/AFSA/AFSB format
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

/**
 * @brief Wrapper for the global flags u32 field in the MD20 header
 */
struct GlobalFlags {
    GlobalFlag value = GlobalFlag::None; ///< Combined GlobalFlag bit values
};

// ============================================================================
// Global Sequences
// ============================================================================

/**
 * @brief Global sequence loop definition
 *
 * Global sequences provide animation timing independent of which model animation
 * is playing. They are used for perpetually looping effects (weapon glows, ambient
 * light pulses, etc.). Tracks referencing a global sequence loop within
 * [0, timestamp] milliseconds.
 */
struct GlobalSequence {
    u32 timestamp = 0; ///< Maximum loop time in milliseconds
};

enum class SequenceFlag : u32 {
    None = 0,
    TiltIn = 0x00000001,             ///< Apply tilt from global flags during animation
    TiltOut = 0x00000002,            ///< Apply tilt from global flags when not animating
    TiltFixed = 0x00000004,          ///< Apply tilt from global flags at all times
    Looping = 0x00000020,         ///< Primary bone sequence (data embedded in .m2)
    IsAlias = 0x00000040,           ///< Is alias; follow aliasNext to find actual data
    AnimatedSetup = 0x00000080,   ///< Blended transitions between states
    StoredAnimated = 0x00000100,  ///< Sequence data stored within the model
    EnableComposite = 0x00000200, ///< Dual blend time (blendTimeIn + blendTimeOut)
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

/**
 * @brief Animation sequence definition (68 bytes on disk)
 *
 * Sequences define animation clips such as Stand, Walk, Attack, Death, etc.
 * The client loads external .anim data when (flags & 0x130) == 0.
 * Multiple sequences can share an animation ID with different variationIndex
 * values; `frequency` weights random selection among them.
 */
struct Sequence {
    u16 id = 0;             ///< Animation ID (references AnimationData.dbc)
    u16 variationIndex = 0; ///< Sub-animation index within the same ID
    u32 duration = 0;       ///< Length in milliseconds (>= WotLK)
    f32 movespeed = 0.0f;   ///< Character movement speed during this animation
    SequenceFlag flags = SequenceFlag::None; ///< SequenceFlag bits (0x20=primary, 0x40=alias, 0x100=stored)
    i16 frequency = 0;      ///< Playback probability weight; all variations sum to 0x7FFF
    u16 padding = 0;        ///< Padding (unused)

    u32 replayMin = 0; ///< Minimum repetition count (0 = no repeat)
    u32 replayMax = 0; ///< Maximum repetition count

    u16 blendTimeIn = 0;  ///< Transition blend-in duration (ms)
    u16 blendTimeOut = 0; ///< Transition blend-out duration (ms)

    Extent bounding; ///< Per-sequence bounding volume

    i16 variationNext = -1; ///< Index of next variation of this animation ID, or -1
    u16 aliasNext = 0;      ///< If alias (flag 0x40), index of target sequence
};

// ============================================================================
// Vertex
// ============================================================================

/**
 * @brief Model vertex (48 bytes on disk)
 *
 * Vertices are stored globally in the .m2 file. Skin profiles reference subsets
 * via index buffers. Two UV sets support multi-texturing; which set is used
 * depends on the vertex shader selected by the rendering pipeline.
 */
struct Vertex {
    Vector3f position;                   ///< Vertex position in model space (12B)
    std::array<u8, 4> boneWeights = {0}; ///< Bone weights (sum should be 255) (4B)
    std::array<u8, 4> boneIndices = {0}; ///< Indices into bones[] (4B)
    Vector3f normal;                     ///< Vertex normal (12B)
    std::array<Vector2f, 2> texCoords;   ///< Two UV coordinate sets (16B)
};

// ============================================================================
// Bone
// ============================================================================

/**
 * @brief Skeleton bone with animation tracks
 *
 * Bones form the skeleton hierarchy. Each bone's final transform is computed as:
 * `parent_transform * translate(pivot) * T * R * S * translate(-pivot)`
 * where T/R/S come from the animation tracks.
 *
 * WoW uses a Z-up coordinate system. To convert to Y-up: (x, y, z) -> (x, -z, y).
 */
struct Bone {
    i32 keyBoneId = -1;    ///< Index in key_bone_lookup, or -1 if not a key bone
    u32 flags = 0;         ///< BoneFlag bits (billboard, ignore parent, etc.)
    i16 parentBoneId = -1; ///< Parent bone index, or -1 for root
    u16 submeshId = 0;     ///< Mesh part ID

    u32 boneNameCRC = 0; ///< CRC of bone name (debug only)

    AnimationTrack<Vector3f> translation;      ///< Position animation
    AnimationTrack<CompatQuaternion> rotation; ///< Rotation animation (compressed quaternion)
    AnimationTrack<Vector3f> scale;            ///< Scale animation
    Vector3f pivot;                            ///< Pivot point in model space
};

/**
 * @brief Bone behavior flags
 *
 * Spherical and cylindrical billboard bits are mutually exclusive.
 * Billboards are used for light halos, view-facing geometry, etc.
 */
enum class BoneFlag : u32 {
    None = 0,
    IgnoreParentTranslate = 0x001, ///< Don't inherit parent's translation
    IgnoreParentScale = 0x002,     ///< Don't inherit parent's scale
    IgnoreParentRotation = 0x004,  ///< Don't inherit parent's rotation
    SphericalBillboard = 0x008,    ///< Always face camera (all axes)
    CylindricalBillboardX = 0x010, ///< Billboard locked to X axis
    CylindricalBillboardY = 0x020, ///< Billboard locked to Y axis
    CylindricalBillboardZ = 0x040, ///< Billboard locked to Z axis
    Transformed = 0x200,           ///< Bone has animation transforms
    Kinematic = 0x400,             ///< Physics system can influence this bone (MoP+)
    HelmetAnimScaled = 0x1000,     ///< Helmet animation scaling via HelmetAnimScaling.dbc
};

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

// ============================================================================
// Texture
// ============================================================================

/**
 * @brief Texture definition
 *
 * Type 0 textures have an in-file filename (BLP path). All other types are
 * "hardcoded" — their actual texture is determined at runtime from database
 * lookups (CharSections.dbc, CreatureDisplayInfo.dbc, etc.). In chunked files
 * (>= BfA), the TXID chunk provides fileDataIDs, replacing filenames.
 */
struct Texture {
    u32 type = 0;         ///< Texture type (0=filename, 1=skin, 2=object skin, 6=hair, etc.)
    u32 flags = 0;        ///< 0x1 = wrap X, 0x2 = wrap Y
    std::string filename; ///< BLP file path (type 0 only; empty for hardcoded types)
};

// ============================================================================
// Material (Render Flags)
// ============================================================================

/**
 * @brief Material rendering properties
 *
 * Materials define how surfaces are rendered, combining flags for lighting,
 * fog, culling, and depth behavior with a blending mode.
 *
 * Binary layout: 4 bytes (u16 flags + u16 blendingMode).
 */
struct Material {
    u16 flags = 0; ///< MaterialFlag bits (unlit, two-sided, depth, etc.)
    u16 blendingMode =
        0; ///< Blend mode (0=opaque, 1=srcColor+one, 2=alpha, 3=alphaTest, 4=additive)
};

// ============================================================================
// Material Flags
// ============================================================================

/**
 * @brief Bit flags for material rendering behavior
 */
enum class MaterialFlag : u16 {
    None = 0,
    Unlit = 0x01,             ///< Not affected by lighting
    Unfogged = 0x02,          ///< Not affected by fog
    TwoSided = 0x04,          ///< Disable backface culling
    DepthTest = 0x08,         ///< Enable depth testing
    DepthWrite = 0x10,        ///< Enable depth writing
    NoAlphaComposite = 0x800, ///< Force opaque or fully transparent (MoP+)
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

// ============================================================================
// Texture Weight
// ============================================================================

/**
 * @brief Animated texture transparency
 *
 * Provides global transparency modulation in addition to color alpha.
 * Values are fixed16: 0 = transparent, 0x7FFF = opaque.
 */
struct TextureWeight {
    AnimationTrack<i16> weight; ///< Transparency animation (fixed16: 0=transparent, 0x7FFF=opaque)
};

// ============================================================================
// Texture Transform
// ============================================================================

/**
 * @brief Animated UV coordinate transformation
 *
 * Transforms UV coordinates over time, creating effects like flowing water,
 * lava, pulsing runes, etc. Rotation is applied around texture center (0.5, 0.5).
 */
struct TextureTransform {
    AnimationTrack<Vector3f> translation;      ///< UV translation animation
    AnimationTrack<CompatQuaternion> rotation; ///< UV rotation animation (center = 0.5, 0.5)
    AnimationTrack<Vector3f> scaling;          ///< UV scaling animation
};

// ============================================================================
// Color Animation
// ============================================================================

/**
 * @brief Per-vertex color and alpha animation
 *
 * Color animations provide per-vertex color modulation and alpha control
 * for meshes. Referenced by skin batches via combo/lookup tables.
 */
struct ColorAnimation {
    AnimationTrack<Vector3f> color; ///< RGB color [0,1] per component
    AnimationTrack<i16> alpha;      ///< Alpha (fixed16: 0=transparent, 0x7FFF=opaque)
};

// ============================================================================
// Light
// ============================================================================

/**
 * @brief Light source attached to the model
 *
 * Lights can be attached to bones to move with animations. Type 0 = directional
 * (login screens only), type 1 = point light (wands, doodads, creature effects).
 */
struct Light {
    u16 type = 0;      ///< 0 = Directional, 1 = Point
    i16 boneId = -1;   ///< Bone index (-1 if not bone-attached)
    Vector3f position; ///< Position relative to bone

    AnimationTrack<Vector3f> ambientColor; ///< Ambient color animation (RGB)
    AnimationTrack<f32> ambientIntensity;  ///< Ambient intensity (default 1.0)
    AnimationTrack<Vector3f> diffuseColor; ///< Diffuse color animation (RGB)
    AnimationTrack<f32> diffuseIntensity;  ///< Diffuse intensity (default 1.0)
    AnimationTrack<f32> attenuationStart;  ///< Distance where attenuation begins
    AnimationTrack<f32> attenuationEnd;    ///< Distance where light reaches zero
    AnimationTrack<u8> visibility;         ///< Visibility toggle (0/1)
};

// ============================================================================
// Camera
// ============================================================================

/**
 * @brief Camera spline keyframe value with tangent data
 *
 * Camera positions and target positions use spline interpolation with
 * explicit in/out tangents for smooth camera movement.
 */
struct CameraSpline {
    Vector3f value;      ///< Position at this keyframe
    Vector3f inTangent;  ///< Incoming tangent for curve interpolation
    Vector3f outTangent; ///< Outgoing tangent for curve interpolation
};

/**
 * @brief Camera definition
 *
 * Cameras define viewpoints for character portraits (type 0), character info
 * tab (type 1), or flyby camera paths (type -1). Most models have cameras
 * for portrait and character info tab.
 *
 * FOV is stored as diagonal FOV in radians. Convert to vertical:
 * v_fov = d_fov / sqrt(1 + aspect^2)
 */
struct Camera {
    u32 type = 0;           ///< 0=portrait, 1=characterinfo, -1=flyby
    f32 fieldOfView = 0.0f; ///< Diagonal FOV in radians (< Cata only; see fieldOfViewTrack)
    f32 farClip = 0.0f;     ///< Far clipping distance
    f32 nearClip = 0.0f;    ///< Near clipping distance

    AnimationTrack<CameraSpline> positions; ///< Camera position animation (spline)
    Vector3f positionBase;                  ///< Base camera position

    AnimationTrack<CameraSpline> targetPositions; ///< Look-at target animation (spline)
    Vector3f targetPositionBase;                  ///< Base target position

    AnimationTrack<f32> roll;             ///< Camera roll animation (0 to 2π)
    AnimationTrack<f32> fieldOfViewTrack; ///< Animated FOV (>= Cata only)
};

// ============================================================================
// Attachment
// ============================================================================

/**
 * @brief Attachment point for external models and effects
 *
 * Attachments define mount points where weapons, shields, effects, and UI
 * elements (like nameplates) can be attached. The id determines the semantic
 * role (0=Shield/MountMain, 1=HandRight, 2=HandLeft, 11=Helm, 18=PlayerName, etc.).
 */
struct Attachment {
    u32 id = 0;                 ///< Attachment type/slot ID
    u16 boneId = 0;             ///< Anchor bone index
    u16 unknown = 0;            ///< Unknown field
    Vector3f position;          ///< Offset from bone (often same as bone pivot)
    AnimationTrack<u8> animate; ///< Enable/disable toggle (bool; default = true)
};

// ============================================================================
// Ribbon Emitter
// ============================================================================

/**
 * @brief Ribbon/trail emitter
 *
 * Creates ribbon trails that follow the emitter's movement (sword trails,
 * missile contrails, wisps, light-trails, etc.). heightAbove/heightBelow
 * define ribbon width; edgesPerSecond controls smoothness; edgeLifetime
 * controls trail length.
 */
struct RibbonEmitter {
    u32 ribbonId = UINT32_MAX; ///< Ribbon ID (usually -1)
    u32 boneId = 0;            ///< Attached bone index
    Vector3f position;         ///< Position relative to bone

    std::vector<u16> textureIndices;  ///< Indices into textures[]
    std::vector<u16> materialIndices; ///< Indices into materials[]

    AnimationTrack<Vector3f> colorTrack; ///< RGB color multiplier animation
    AnimationTrack<i16> alphaTrack;  ///< Alpha animation (fixed16: 0=transparent, 0x7FFF=opaque)
    AnimationTrack<f32> heightAbove; ///< Height above attachment point animation
    AnimationTrack<f32> heightBelow; ///< Height below attachment point animation

    f32 edgesPerSecond = 0.0f;     ///< Smoothness; quads generated per second
    f32 edgeLifetime = 0.0f;       ///< Seconds each quad persists (trail length)
    f32 gravity = 0.0f;            ///< Gravity effect; arcsin(val) = emission angle in degrees
    u16 textureRows = 1;           ///< Texture atlas rows
    u16 textureCols = 1;           ///< Texture atlas columns
    AnimationTrack<u16> texSlot;   ///< Texture slot animation
    AnimationTrack<u8> visibility; ///< Visibility toggle animation
    i16 priorityPlane = 0;         ///< Rendering priority (>= WotLK)
    i8 ribbonColorIndex = -1;      ///< Ribbon color index
    i8 textureTransformIndex = -1; ///< Index into textureTransformCombos (if globalFlags & 0x20000)
};

// ============================================================================
// Particle Emitter
// ============================================================================

/**
 * @brief Tumble rotation speed bounds for particle emitters
 *
 * Defines minimum and maximum rotation speeds per axis for
 * particle tumbling (random rotation during flight).
 */
struct M2Box {
    Vector3f minimum; ///< Minimum rotation speed per axis (radians/sec)
    Vector3f maximum; ///< Maximum rotation speed per axis (radians/sec)
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
    Lighting = 0x1,       ///< Particles are affected by lighting
    Unk_0x2 = 0x2,        ///< Unknown; used in some spell effects
    InheritOrientation = 0x4,      ///< On emission, particle orientation is affected by player orientation
    WorldSpace = 0x8,         ///< 	Particles travel "up" in world space, rather than model.
    DoNotTrail = 0x10,       ///< Do not Trail
    Unshaded = 0x20,       ///< Unlightning
    BurstMultiplier = 0x40,///< Use Burst Multiplier
    ModelSpace = 0x80,       ///< Particles in Model Space (Causes animation of the particle emitter to be carried over to the particles.)
    Unk_0x100 = 0x100,     ///< Unknown; used in some spell effects
    Unk_0x200 = 0x200,     ///< spawn position randomized in some way?
    Pinned = 0x400,     ///< Particles are pinned to emitter position. their quad enlarges from their creation
                        // position to where they expand
    Unk_0x800 = 0x800,     ///< Unknown; used in some spell effects
    XYQuad = 0x1000,   ///< XYQuad Particles. They align to XY axis facing Z axis direction.
    Project = 0x2000,   ///< clamp to ground; call CParticleEmitter2::ProjectParticle
    Unk_0x4000 = 0x4000,   ///< Unknown; used in some spell effects
    Unk_0x8000 = 0x8000,   ///< Unknown; used in some spell effects
    ChooseRandomTexture = 0x10000,   ///< Choose random texture
    Unk_0x20000 = 0x20000,   ///< Unknown; used in some spell effects
    Unk_0x40000 = 0x40000,   ///< Unknown; used in some spell effects
    Unk_0x80000 = 0x80000,   ///< Unknown; used in some spell effects
    Unk_0x100000 = 0x100000, ///< Unknown; used in some spell effects
    Outwards = 0x200000, ///< 	STYLE: "Outward" particles, most emitters have this and their particles move away from the origin, 
                         ///< when they don't the particles start at origin+(speed*life) and move towards the origin.
    Unk_0x400000 = 0x400000, ///< STYLE: unknown. In a large proportion of particles this seems to be simply the opposite 
                             ///< of the above flag, but in some (e.g. voidgod.m2 or wingedlionmount.m2) both flags are true.
    ScaleVary = 0x800000, ///< If set, ScaleVary affects x and y independently; if not set, ScaleVary.x affects x and y uniformly, and ScaleVary.y is not used.
    Unk_0x1000000 = 0x1000000, ///< Unknown; used in some spell effects
    RandomFlipbookStart = 0x2000000, ///< Random FlipBookStart
    IgnoresDistance = 0x4000000, ///< Ignores Distance (or 0x4000000?!, CMapObjDef::SetDoodadEmittersIgnoresDistance has this one)
    CompressedGravity = 0x8000000, ///< gravity values are compressed vectors instead of z-axis values (see Compressed Particle Gravity below)
    BoneGenerator = 0x10000000, ///< bone generator = bone, not joint
    Unk_0x20000000 = 0x20000000, ///< Unknown; used in some spell effects
    Unk_0x40000000 = 0x40000000, ///< do not throttle emission rate based on distance
    UsesMultitexturing = 0x80000000, ///< Particle uses multi-texturing
};

struct ParticleEmitterExtension {
    f32 zSource = 0.0f;              ///< Z-axis source offset
    f32 colorMult = 0.0f;            ///< Applied against particle's diffuse color
    f32 alphaMult = 0.0f;            ///< Applied against particle's opacity
    AnimationTrack<i16> alphaCutoff; ///< Per-particle alpha test over lifetime (fixed16)
};

/**
 * @brief Particle emitter (sprite-based)
 *
 * The most complex M2 structure. Particles use sprite textures and support
 * various shapes (plane, sphere, spline, bone), blending modes, and animation
 * over the particle lifetime. Used for fire, smoke, magic effects, etc.
 *
 * Emitter types: 1=Plane, 2=Sphere, 3=Spline, 4=Bone.
 * Blending: 0=opaque, 1=srcColor+one, 2=alpha, 3=alphaTest, 4=additive.
 */
struct ParticleEmitter {
    u32 particleId = UINT32_MAX; ///< Particle ID (usually -1)
    u32 flags = 0;               ///< Particle flags (lighting, billboard, model-space, etc.)
    Vector3f position;           ///< Position relative to bone
    u16 boneId = 0;              ///< Attached bone index
    union {
        u16 textureId;          ///< Texture index (references textures[]; ignored if usesMultitexturing)
        struct {
            u16 textureId1 : 5; ///< Texture index for first texture (if usesMultitexturing)
            u16 textureId2 : 5; ///< Texture index for second texture (if usesMultitexturing)
            u16 textureId3 : 5; ///< Texture index for third texture (if usesMultitexturing)
            u16 padding : 1;    ///< Padding bit (unused)
        };
    };

    std::string geometryModelFilename;  ///< Spawned model geometry filename
    std::string recursionModelFilename; ///< Recursion model filename (alias for up to 4 emitters)

    ParticleBlending blendingType = ParticleBlending::Opaque; ///< Blend mode
    ParticleEmitterType emitterType = ParticleEmitterType::Plane; ///< Emitter shape
    u16 particleColorIndex = 0; ///< ParticleColor.dbc row reference (11/12/13)

    std::array<fixed8_5, 2> multiTextureParamX;
    i16 textureTilerotation; ///< Texture tiling/rotation
    u16 rows;    ///< Texture atlas rows (for flipbook animation)
    u16 columns; ///< Texture atlas columns (for flipbook animation)

    AnimationTrack<f32> emissionSpeed;   ///< Particle initial speed
    AnimationTrack<f32> speedVariation;  ///< Speed variation [0, 1]
    AnimationTrack<f32> verticalRange;   ///< Vertical emission angle [0, π]
    AnimationTrack<f32> horizontalRange; ///< Horizontal emission angle [0, 2π]
    AnimationTrack<f32> gravity;         ///< Gravity acceleration
    AnimationTrack<f32> lifespan;        ///< Particle lifetime in seconds
    f32 lifespanVary = 0.0f;             ///< Lifespan variation (>= WotLK)
    AnimationTrack<f32> emissionRate;    ///< Particles emitted per second
    f32 emissionRateVary = 0.0f;         ///< Emission rate variation (>= WotLK)
    AnimationTrack<f32> emissionAreaWidth; ///< Width of emission area (for plane emitters, min radious for sphere emitters)
    AnimationTrack<f32> emissionAreaLength; ///< Length of emission area (for plane emitters, max radious for sphere emitters)
    AnimationTrack<f32> zSource;       ///< // When greater than 0, the initial velocity of the particle is (particle.position - C3Vector(0, 0, zSource)).Normalize()
    AnimationTrackSimple<Vector3f> colorTrack; ///< Particle color animation (RGB)
    AnimationTrackSimple<i16> alphaTrack;      ///< Particle alpha animation (fixed16: 0=none, 0x7FFF=full)
    AnimationTrackSimple<Vector2f> scaleTrack;      ///< Particle scale animation 
    Vector2f scaleVary = Vector2f(0.0f, 0.0f); ///< Particle scale variation
    AnimationTrackSimple<u16> headIntensity;
    AnimationTrackSimple<u16> tailIntensity;
    f32 tailLength = 0.0f; ///< For ribbon-like particles, the length of the tail behind the head
    f32 twinkleSpeed = 0.0f; ///< Speed of twinkling effect (if lighting flag is set)
    f32 twinklePercent = 0.0f; ///< same mechanic as MDL twinkleOnOff but non-binary in 0.11.0
    Vector2f twinkleScale = Vector2f(0.0f, 0.0f); ///< Scale variation for twinkling particles (if lighting flag is set)
    f32 burstMultiplier = 1.0f; ///< Multiplier for burst emission (if BurstMultiplier flag is set)
    f32 drag = 0.0f;             ///< Speed damping factor: speed *= exp(-drag * t)
    f32 baseSpin = 0.0f;         ///< Initial rotation of particle quad (radians) (>= WotLK)
    f32 baseSpinVary = 0.0f;     ///< Variation on base spin (>= WotLK)
    f32 spin = 0.0f;             ///< Rotation speed of particle quad (radians/sec)
    f32 spinVary = 0.0f;         ///< Spin variation (>= WotLK)

    M2Box tumble;                 ///< Tumble rotation speed bounds (min/max per axis)
    Vector3f windVector;          ///< Wind direction and strength
    f32 windTime = 0.0f;          ///< Wind time parameter

    f32 followSpeed1 = 0.0f;      ///< Follow parameter: speed 1
    f32 followScale1 = 0.0f;      ///< Follow parameter: scale 1
    f32 followSpeed2 = 0.0f;      ///< Follow parameter: speed 2
    f32 followScale2 = 0.0f;      ///< Follow parameter: scale 2

    std::vector<Vector3f> splinePoints; ///< Control points for spline emitter type
    AnimationTrack<u8> enabledIn;       ///< Visibility toggle per animation (boolean)

    // M2Particle extension (Cata+): multi-texture parameters
    // Present when version > 271 or globalFlags & NewParticleRecord
    std::array<std::array<fp_6_9, 2>, 2> multiTextureParam0{}; ///< [texture_idx][x/y]
    std::array<std::array<fp_6_9, 2>, 2> multiTextureParam1{}; ///< [texture_idx][x/y]
    std::optional<ParticleEmitterExtension> extension; ///< Additional parameters for extended particle behavior
};

// ============================================================================
// Event
// ============================================================================

/**
 * @brief Animation event trigger
 *
 * Events fire at specific animation timestamps to trigger sounds, spawn effects,
 * screen shakes, etc. Each timestamp in the enabled track is an implicit trigger.
 * The identifier is a 4-byte ASCII code (e.g., "$DTH"=death, "$FSD"=footstep,
 * "$CST"=cast spell), and data carries a context-dependent payload (e.g.,
 * soundEntryId for sound events).
 */
struct Event {
    u32 identifier = 0;         ///< 4-char ASCII ID stored as u32 (e.g., "$DTH", "$FSD", "$CST")
    u32 data = 0;               ///< Context payload (soundEntryId, cameraShakeId, etc.)
    u32 boneId = 0;             ///< Attachment bone index
    Vector3f position;          ///< Position relative to bone
    AnimationTrackBase enabled; ///< Timestamp-only track; each timestamp = "fire event"
};

// ============================================================================
// MD20 Header
// ============================================================================

struct Model {
    std::string modelName;   ///< Model name (from M2Array<char>)
    GlobalFlags globalFlags; ///< Global behavior flags

    // Animation and timing
    std::vector<GlobalSequence> globalLoops; ///< Global looping animation durations
    std::vector<Sequence> sequences;         ///< Named animation sequences
    std::vector<u16> sequenceIdxHashById;    ///< Hash table: animation ID -> sequence index

    // Skeleton
    std::vector<Bone> bones;     ///< Skeleton bones with animation tracks
    std::vector<u16> keyBoneIds; ///< Well-known bone role -> bone index lookup

    // Geometry
    std::vector<Vertex> vertices; ///< Global vertex array (48 bytes each)
    u32 numSkinProfiles = 0;      ///< Number of skin profiles (>= WotLK; replaces inline skins)

    // Textures and materials
    std::vector<ColorAnimation> colors;        ///< Per-vertex color/alpha animations
    std::vector<Texture> textures;             ///< Texture definitions (filename or hardcoded type)
    std::vector<TextureWeight> textureWeights; ///< Global transparency animations
    std::vector<TextureTransform> textureTransforms; ///< UV transformation animations
    std::vector<u16> textureIndicesById; ///< Replaceable texture type -> texture index lookup
    std::vector<Material> materials;     ///< Material render flags and blend modes

    // Lookup / combo tables (indirection for skin batches)
    std::vector<u16> boneCombos;    ///< Bone indices for skinning; skin sections reference slices
    std::vector<u16> textureCombos; ///< Indices into textures[]
    std::vector<u16> textureCoordCombos;     ///< UV mapping selection (-1=env, 0=UV0, 1=UV1)
    std::vector<u16> textureWeightCombos;    ///< Indices into textureWeights[]
    std::vector<u16> textureTransformCombos; ///< Indices into textureTransforms[]

    // Bounding and collision volumes
    Extent bounding;                           ///< Model bounding volume
    Extent collision;                          ///< Simplified collision volume
    std::vector<u16> collisionTriangleIndices; ///< Collision mesh triangle indices (3 per triangle)
    std::vector<Vector3f> collisionVertices;   ///< Collision mesh vertex positions
    std::vector<Vector3f> collisionFaceNormals; ///< Collision mesh face normals (1 per triangle)

    // Scene objects
    std::vector<Attachment> attachments;    ///< Equipment/effect attachment points
    std::vector<u16> attachmentIndicesById; ///< Attachment type -> attachment index lookup
    std::vector<Event> events;              ///< Animation event triggers
    std::vector<Light> lights;              ///< Light sources
    std::vector<Camera> cameras;            ///< Camera definitions
    std::vector<u16> cameraIndicesById;     ///< Camera type -> camera index lookup

    // Effects
    std::vector<RibbonEmitter> ribbonEmitters;     ///< Ribbon trail emitters
    std::vector<ParticleEmitter> particleEmitters; ///< Particle emitters

    // Multitexturing (conditional: requires UseTextureCombinerCombos flag)
    std::vector<u16> textureCombinerCombos; ///< Second-pass material override for multitexturing
};

/**
 * @brief Complete MD20 model header
 *
 * The central structure of an M2 file. Contains all model data stored as
 * count/offset pairs (M2Array) on disk, resolved to std::vector at parse time.
 * Includes geometry, skeleton, materials, animations, scene objects, emitters,
 * collision volumes, and lookup/combo tables for the rendering pipeline.
 *
 * Binary layout: Starts at offset 0 (MD20) or after the MD21 chunk header.
 * The header is a fixed-layout structure at ~0x130 bytes, followed by all
 * variable-length data blocks addressed through the M2Array offsets.
 */
struct MD20Header {
    u32 magic = MD20_TAG;            ///< File magic ("MD20" = 0x4D443230)
    u32 version = M2_VERSION_LEGION; ///< Format version (see M2_VERSION_* constants)

    Model model; 
};

} // namespace m2
} // namespace whiteout
