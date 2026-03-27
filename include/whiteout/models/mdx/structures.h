// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file structures.h
 * @brief Data structures for MDX model components
 *
 * This file defines all the structure types used in MDX files, including:
 * - Animation sequences
 * - Textures and materials
 * - Geometry (geosets)
 * - Bones and hierarchy nodes
 * - Lights and cameras
 * - Particle systems and effects
 *
 * Each structure corresponds to a specific chunk type in the MDX format.
 */

#include <array>
#include "types.h"

namespace whiteout {
namespace mdx {

// ============================================================================
// Sequence
// ============================================================================

/**
 * @brief Animation sequence definition
 *
 * Sequences define named animation clips with specific frame ranges.
 * Examples: "Stand", "Walk", "Attack", "Death"
 */
struct Sequence {
    std::string name;      ///< Sequence name (e.g., "Stand", "Walk")
    u32 intervalStart = 0; ///< Starting frame number
    u32 intervalEnd = 0;   ///< Ending frame number (exclusive)
    f32 moveSpeed = 0.0f;  ///< Movement speed during this animation
    u32 flags = 0;         ///< Flags (0x1 = non-looping)
    f32 rarity = 0.0f;     ///< Rarity factor for variation sequences
    u32 syncPoint = 0;     ///< Sync point for blending
    Extent extent;         ///< Bounding volume for this sequence
};

// ============================================================================
// Texture
// ============================================================================

/**
 * @brief Texture definition
 *
 * Defines a texture file path and its properties.
 */
struct Texture {
    u32 replaceableId =
        0; ///< Replaceable texture ID (0 = not replaceable, 1 = team color, 2 = team glow, etc.)
    std::string fileName; ///< Path to texture file (BLP, DDS or TGA)
    u32 flags = 0;        ///< Texture flags (0x1 = wrap width, 0x2 = wrap height)
};

// ============================================================================
// Sound (Deprecated)
// ============================================================================

/**
 * @brief Sound definition (deprecated, not used in shipped models)
 *
 * Sounds define audio file paths and playback properties.
 * While readers for this chunk exist in the internal game code,
 * no known shipped model contains SNDS data.
 *
 * Binary layout: 56 bytes per entry
 */
struct Sound {
    std::string soundFile;      ///< Path to sound file (44 bytes, null-terminated)
    f32 maximumDistance = 0.0f; ///< Maximum audible distance
    f32 minimumDistance = 0.0f; ///< Minimum distance (full volume)
    u32 soundChannel = 0;       ///< Sound channel identifier
};

// ============================================================================
// Node (Base for hierarchy)
// ============================================================================

/**
 * @brief Base node in the model hierarchy
 *
 * Nodes form the skeleton and attachment system of the model. Each node
 * can have a parent creating a transformation hierarchy. Nodes can be:
 * - Bones (for skeletal animation)
 * - Helpers (attachment points)
 * - Lights
 * - Particle emitters
 * - And other special objects
 *
 * Nodes contain transformation animation tracks (translation, rotation, scaling).
 */
struct Node {
    /**
     * @brief Type of node in the hierarchy
     */
    enum class NodeType : u32 {
        Bone,             ///< Skeleton bone
        Light,            ///< Light source
        Helper,           ///< Helper/attachment point
        Attachment,       ///< Equipment attachment
        ParticleEmitter,  ///< Particle emitter v1
        ParticleEmitter2, ///< Particle emitter v2
        RibbonEmitter,    ///< Ribbon trail emitter
        EventObject,      ///< Event trigger
        Camera,           ///< Camera
        CollisionShape,   ///< Collision volume
        FaceEffect,       ///< Face effect (Reforged)
        CornEmitter       ///< PopcornFX emitter (Reforged)
    };

    /**
     * @brief Flags controlling node behavior and rendering
     */
    enum class NodeFlag : u32 {
        None = 0x0,
        DontInheritTranslation = 0x1, ///< Don't inherit parent translation
        DontInheritRotation = 0x2,    ///< Don't inherit parent rotation
        DontInheritScaling = 0x4,     ///< Don't inherit parent scaling
        Billboarded = 0x8,            ///< Always face camera
        BillboardedLockX = 0x10,      ///< Billboard but lock X axis
        BillboardedLockY = 0x20,      ///< Billboard but lock Y axis
        BillboardedLockZ = 0x40,      ///< Billboard but lock Z axis
        CameraAnchored = 0x80,        ///< Anchored to camera
        Bone = 0x100,                 ///< This is a bone
        Light = 0x200,                ///< This is a light
        EventObject = 0x400,          ///< This is an event object
        Attachment = 0x800,           ///< This is an attachment
        ParticleEmitter = 0x1000,     ///< This is a particle emitter
        CollisionShape = 0x2000,      ///< This is a collision shape
        RibbonEmitter = 0x4000,       ///< This is a ribbon emitter
        Unshaded = 0x8000,            ///< Unshaded (PE2) / EmitterUsesMdl (PE)
        EmitterUsesMdl = 0x8000,      ///< Particle emitter uses MDL model
        SortPrimitives = 0x10000,     ///< Sort primitives (PE2 only)
        EmitterUsesTga = 0x10000,     ///< Particle emitter uses TGA (PE only)
        LineEmitter = 0x20000,        ///< Line-shaped emitter
        Unfogged = 0x40000,           ///< Not affected by fog
        ModelSpace = 0x80000,         ///< Use model space coordinates
        XYQuad = 0x100000             ///< XY quad billboarding
    };

    static constexpr u32 NO_PARENT = 0xFFFFFFFF; ///< Value indicating no parent node

    std::string name;                 ///< Node name (for debugging/reference)
    u32 objectId = 0;                 ///< Unique ID for this node
    u32 parentId = NO_PARENT;         ///< Parent node ID or NO_PARENT
    NodeFlag flags = NodeFlag::None;  ///< Combination of NodeFlag values
    NodeType type = NodeType::Helper; ///< Type of this node
    u32 nodeFamilyId = 0;             ///< Used to link related nodes of the same type

    // Animation tracks
    Track<Vector3f> translationTracks; ///< Position animation
    Track<Quaternion> rotationTracks;    ///< Rotation animation (quaternion XYZW)
    Track<Vector3f> scalingTracks;     ///< Scale animation
};

inline Node::NodeFlag operator|(Node::NodeFlag lhs, Node::NodeFlag rhs) {
    return static_cast<Node::NodeFlag>(static_cast<u32>(lhs) | static_cast<u32>(rhs));
}

inline Node::NodeFlag operator&(Node::NodeFlag lhs, Node::NodeFlag rhs) {
    return static_cast<Node::NodeFlag>(static_cast<u32>(lhs) & static_cast<u32>(rhs));
}

inline Node::NodeFlag operator|=(Node::NodeFlag& lhs, Node::NodeFlag rhs) {
    lhs = lhs | rhs;
    return lhs;
}

inline Node::NodeFlag operator&=(Node::NodeFlag& lhs, Node::NodeFlag rhs) {
    lhs = lhs & rhs;
    return lhs;
}

inline Node::NodeFlag operator~(Node::NodeFlag flag) {
    return static_cast<Node::NodeFlag>(~static_cast<u32>(flag));
}

// Helper to check if a flag is set
inline bool hasFlag(Node::NodeFlag flags, Node::NodeFlag flag) {
    return (static_cast<u32>(flags) & static_cast<u32>(flag)) != 0;
}

// ============================================================================
// Sound Emitter (Deprecated)
// ============================================================================

/**
 * @brief Sound emitter attached to the model hierarchy (deprecated)
 *
 * Sound emitters are positioned nodes that can emit sounds at specific
 * animation frames via KSEK tracks. While readers exist in the internal
 * game code, no known shipped model contains SNEM data.
 */
struct SoundEmitter {
    Node node;             ///< Base node data with transform

    Track<u32> soundTrack; ///< KSEK - sound event track (u32 values)
};

// ============================================================================
// Layer
// ============================================================================

/**
 * @brief Material rendering layer
 *
 * Materials can have multiple layers, each with its own texture and blending mode.
 * Layers control how textures are combined and rendered.
 */
struct Layer {
    /**
     * @brief Blending/filter mode for the layer
     */
    enum class FilterMode : u32 {
        None = 0,        ///< No blending
        Transparent = 1, ///< Alpha test transparency
        Blend = 2,       ///< Alpha blending
        Additive = 3,    ///< Additive blending
        AddAlpha = 4,    ///< Additive alpha blending
        Modulate = 5,    ///< Modulate (multiply) blending
        Modulate2x = 6,  ///< Modulate 2x (brighten)
        Count,
    };

    /**
     * @brief Shader and rendering flags
     */
    enum class ShadingFlag : u32 {
        None = 0,
        Unshaded = 0x1,     ///< Not affected by lighting
        SphereEnvMap = 0x2, ///< Spherical environment mapping
        Unknown1 = 0x4,     ///< Unknown flag
        Unknown2 = 0x8,     ///< Unknown flag
        TwoSided = 0x10,    ///< Render both sides of polygons
        Unfogged = 0x20,    ///< Not affected by fog
        NoDepthTest = 0x40, ///< Disable depth testing
        NoDepthSet = 0x80,  ///< Don't write to depth buffer
    };

    enum class SlotType : u32 {
        DiffuseMap = 0,
        NormalMap = 1,
        ORMMap = 2,
        EmissiveMap = 3,
        TeamColor = 4,
        EnvironmentMap = 5,
        Unknown
    };

    /**
     * @brief Sub-texture definition (Reforged multi-texture)
     */
    struct SubTexture {
        u32 textureId = 0;                    ///< Index into texture array
        SlotType slot = SlotType::DiffuseMap; ///< Texture slot number
        Track<u32> tracks;                    ///< Texture ID animation
    };

    FilterMode filterMode = FilterMode::None;     ///< Blending mode
    ShadingFlag shadingFlags = ShadingFlag::None; ///< Rendering flags
    u32 textureId = 0;                            ///< Texture index (versions 800-1100)
    u32 textureAnimationId = 0;                   ///< Texture animation index
    u32 coordId = 0;                              ///< Texture coordinate set index
    f32 alpha = 1.0f;                             ///< Layer opacity

    // Reforged PBR properties (version > 800)
    f32 emissiveGain = 0.0f;                   ///< Emissive light intensity
    Vector3f fresnelColor = Vector3f(1, 1, 1); ///< Fresnel effect color
    f32 fresnelOpacity = 0.0f;                 ///< Fresnel effect opacity
    f32 fresnelTeamColor = 0.0f;               ///< Fresnel team color factor

    bool is_hd = false;                  ///< True if using Reforged HD shading
    std::vector<SubTexture> subTextures; ///< Multi-texture support (version 1200+)

    // Animation tracks
    Track<u32> textureIdTracks;         ///< Texture ID animation (versions 800-1100)
    Track<f32> alphaTracks;             ///< Alpha animation
    Track<f32> emissiveGainTracks;      ///< Emissive gain animation
    Track<Vector3f> fresnelColorTracks; ///< Fresnel color animation
    Track<f32> fresnelAlphaTracks;      ///< Fresnel alpha animation
    Track<f32> fresnelTeamColorTracks;  ///< Fresnel team color animation
};

inline Layer::ShadingFlag operator|(Layer::ShadingFlag lhs, Layer::ShadingFlag rhs) {
    return static_cast<Layer::ShadingFlag>(static_cast<u32>(lhs) | static_cast<u32>(rhs));
}

inline Layer::ShadingFlag operator&(Layer::ShadingFlag lhs, Layer::ShadingFlag rhs) {
    return static_cast<Layer::ShadingFlag>(static_cast<u32>(lhs) & static_cast<u32>(rhs));
}

inline Layer::ShadingFlag operator|=(Layer::ShadingFlag& lhs, Layer::ShadingFlag rhs) {
    lhs = lhs | rhs;
    return lhs;
}

inline Layer::ShadingFlag operator&=(Layer::ShadingFlag& lhs, Layer::ShadingFlag rhs) {
    lhs = lhs & rhs;
    return lhs;
}

inline Layer::ShadingFlag operator~(Layer::ShadingFlag flag) {
    return static_cast<Layer::ShadingFlag>(~static_cast<u32>(flag));
}

// Helper to check if a flag is set
inline bool hasFlag(Layer::ShadingFlag flags, Layer::ShadingFlag flag) {
    return (static_cast<u32>(flags) & static_cast<u32>(flag)) != 0;
}

// ============================================================================
// Material
// ============================================================================

/**
 * @brief Material definition with rendering properties
 *
 * Materials define how surfaces are rendered. Each material contains one or more
 * layers that specify textures and blending modes.
 */
struct Material {
    u32 priorityPlane = 0;     ///< Rendering priority (higher = render last)
    u32 flags = 0;             ///< Material flags
    std::string shader;        ///< Shader name (Reforged)
    std::vector<Layer> layers; ///< Rendering layers
};

// ============================================================================
// Texture Animation
// ============================================================================

/**
 * @brief UV coordinate animation
 *
 * Texture animations transform UV coordinates over time, creating effects
 * like scrolling water, rotating symbols, etc.
 */
struct TextureAnimation {
    Track<Vector3f> translationTracks; ///< UV translation animation
    Track<Quaternion> rotationTracks;  ///< UV rotation animation (quaternion XYZW)
    Track<Vector3f> scalingTracks;     ///< UV scaling animation
};

// ============================================================================
// Geoset
// ============================================================================

/**
 * @brief Mesh geometry
 *
 * A geoset is a complete mesh with vertices, normals, faces, and skinning data.
 * Models can have multiple geosets with different materials or LOD levels.
 */
struct Geoset {
    std::vector<Vector3f> vertexPositions; ///< Vertex positions
    std::vector<Vector3f> vertexNormals;   ///< Vertex normals
    std::vector<u32> faceTypeGroups;       ///< Face type groups (4 = triangles)
    std::vector<u32> faceGroups;           ///< Number of indices per group
    std::vector<u16> faces;                ///< Vertex indices (triangles)
    std::vector<u8> vertexGroups;          ///< Bone groups per vertex
    std::vector<u32> matrixGroups;         ///< Number of matrices per group
    std::vector<u32> matrixIndices;        ///< Bone indices

    u32 materialId = 0;     ///< Material index
    u32 selectionGroup = 0; ///< Selection group (for editor)
    u32 selectionFlags = 0; ///< Selection flags

    u32 lod = 0;         ///< Level of detail index
    std::string lodName; ///< LOD name

    Extent extent;                       ///< Bounding volume
    std::vector<Extent> sequenceExtents; ///< Per-sequence bounding volumes

    std::vector<Vector4f> tangents; ///< Tangent vectors (for normal mapping)
    std::vector<u8> skinData;       ///< Bone indices and weights

    std::vector<std::vector<Vector2f>> textureCoordinateSets; ///< UV coordinates (multiple sets)
};

// ============================================================================
// Geoset Animation
// ============================================================================

/**
 * @brief Geoset visibility and color animation
 *
 * Geoset animations control the visibility and color tinting of meshes.
 */
struct GeosetAnimation {
    f32 alpha = 1.0f;                   ///< Base alpha value
    u32 flags = 0;                      ///< Animation flags
    Vector3f color = Vector3f(1, 1, 1); ///< Base color tint
    u32 geosetId = 0;                   ///< Target geoset index

    Track<f32> alphaTracks;      ///< Alpha animation
    Track<Vector3f> colorTracks; ///< Color animation
};

// ============================================================================
// Bone
// ============================================================================

/**
 * @brief Skeleton bone for skinned animation
 *
 * Bones  form the skeleton that deforms mesh geometry. Each bone is a node
 * in the hierarchy and can affect one or more geosets.
 */
struct Bone {
    static constexpr u32 MULTIPLE_GEOSETS = 0xFFFFFFFF; ///< Bone affects all geosets

    Node node;                                ///< Base node data with transform
    u32 geosetId = MULTIPLE_GEOSETS;          ///< Geoset this bone affects
    u32 geosetAnimationId = MULTIPLE_GEOSETS; ///< Geoset animation index
};

// ============================================================================
// Light
// ============================================================================

/**
 * @brief Light source
 *
 * Lights can be attached to bones to move with animations.
 * They affect how the model is rendered in the game engine.
 */
struct Light {
    /**
     * @brief Type of light source
     */
    enum class LightType : u32 {
        Omni = 0,        ///< Point light (radiates in all directions)
        Directional = 1, ///< Directional light (like sunlight)
        Ambient = 2      ///< Ambient light (affects everything equally)
    };

    Node node;                                 ///< Base node data
    LightType type = LightType::Omni;          ///< Type of light
    f32 attenuationStart = 0.0f;               ///< Distance where attenuation begins
    f32 attenuationEnd = 100.0f;               ///< Distance where light reaches zero
    Vector3f color = Vector3f(1, 1, 1);        ///< Light color (RGB)
    f32 intensity = 1.0f;                      ///< Light intensity
    Vector3f ambientColor = Vector3f(0, 0, 0); ///< Ambient light color
    f32 ambientIntensity = 0.0f;               ///< Ambient intensity
    f32 shadowIntensity = 0.4f;                ///< Shadow darkness (Reforged)

    // Animation tracks
    Track<f32> attenuationStartTracks;  ///< Attenuation start animation
    Track<f32> attenuationEndTracks;    ///< Attenuation end animation
    Track<Vector3f> colorTracks;        ///< Color animation
    Track<f32> intensityTracks;         ///< Intensity animation
    Track<f32> ambientIntensityTracks;  ///< Ambient intensity animation
    Track<Vector3f> ambientColorTracks; ///< Ambient color animation
    Track<f32> visibilityTracks;        ///< Visibility animation
    Track<f32> shadowIntensityTracks;   ///< Shadow intensity animation (Reforged)
};

// ============================================================================
// Helper
// ============================================================================

/**
 * @brief Helper node (attachment point)
 *
 * Helpers are simple nodes used as attachment points for effects,
 * weapons, or other objects. They don't render anything themselves.
 */
struct Helper {
    Node node; ///< Base node data with transform
};

// ============================================================================
// Attachment
// ============================================================================

/**
 * @brief Attachment point for external models
 *
 * Attachments define points where other models (like weapons or shields)
 * can be attached to this model.
 */
struct Attachment {
    Node node;             ///< Base node data
    std::string path;      ///< Path to attached model
    u32 attachmentId = 0;  ///< Attachment slot ID

    Track<f32> visibilityTracks; ///< Visibility animation
};

// ============================================================================
// Particle Emitter
// ============================================================================

/**
 * @brief Particle emitter version 1 (uses external model)
 *
 * Legacy particle emitter that spawns copies of an external model file.
 * Used for effects like footprints, blood splatter, etc.
 */
struct ParticleEmitter {
    Node node;                      ///< Base node data
    f32 emissionRate = 0.0f;        ///< Particles per second
    f32 gravity = 0.0f;             ///< Gravity force
    f32 longitude = 0.0f;           ///< Emission longitude angle
    f32 latitude = 0.0f;            ///< Emission latitude angle
    std::string spawnModelFileName; ///< Model to spawn as particles
    f32 lifespan = 0.0f;            ///< Particle lifetime in seconds
    f32 initialVelocity = 0.0f;     ///< Initial particle speed

    // Animation tracks
    Track<f32> emissionRateTracks; ///< Emission rate animation
    Track<f32> gravityTracks;      ///< Gravity animation
    Track<f32> longitudeTracks;    ///< Longitude animation
    Track<f32> latitudeTracks;     ///< Latitude animation
    Track<f32> lifespanTracks;     ///< Lifespan animation
    Track<f32> speedTracks;        ///< Speed animation
    Track<f32> visibilityTracks;   ///< Visibility animation
};

// ============================================================================
// Particle Emitter 2
// ============================================================================

/**
 * @brief Particle emitter version 2 (sprite-based)
 *
 * More advanced particle system that uses sprite textures. Supports various
 * particle shapes, blending modes, and animation over the particle lifetime.
 * Used for fire, smoke, magic effects, etc.
 */
struct ParticleEmitter2 {
    Node node;               ///< Base node data
    f32 speed = 0.0f;        ///< Particle speed
    f32 variation = 0.0f;    ///< Speed variation (randomness)
    f32 latitude = 0.0f;     ///< Emission cone latitude
    f32 gravity = 0.0f;      ///< Gravity acceleration
    f32 lifespan = 0.0f;     ///< Particle lifetime in seconds
    f32 emissionRate = 0.0f; ///< Particles per second
    f32 length = 0.0f;       ///< Particle length (for tail effect)
    f32 width = 0.0f;        ///< Particle width

    u32 filterMode = 0; ///< Blending mode
    u32 rows = 1;       ///< Texture atlas rows
    u32 columns = 1;    ///< Texture atlas columns
    u32 headOrTail = 0; ///< Head/tail flags

    f32 tailLength = 0.0f; ///< Tail particle length
    f32 time = 0.0f;       ///< Middle time for segment animation

    std::array<Vector3f, 3> segmentColor; ///< Color at start/middle/end
    std::array<u8, 3> segmentAlpha;       ///< Alpha at start/middle/end
    std::array<f32, 3> segmentScaling;    ///< Scale at start/middle/end

    std::array<u32, 3> headInterval;      ///< Head lifetime intervals
    std::array<u32, 3> headDecayInterval; ///< Head decay intervals
    std::array<u32, 3> tailInterval;      ///< Tail lifetime intervals
    std::array<u32, 3> tailDecayInterval; ///< Tail decay intervals

    u32 textureId = 0;     ///< Texture index
    u32 squirt = 0;        ///< Squirt flag (burst mode)
    u32 priorityPlane = 0; ///< Rendering priority
    u32 replaceableId = 0; ///< Replaceable texture ID

    // Animation tracks
    Track<f32> speedTracks;        ///< Speed animation
    Track<f32> variationTracks;    ///< Variation animation
    Track<f32> latitudeTracks;     ///< Latitude animation
    Track<f32> gravityTracks;      ///< Gravity animation
    Track<f32> emissionRateTracks; ///< Emission rate animation
    Track<f32> lengthTracks;       ///< Length animation
    Track<f32> widthTracks;        ///< Width animation
    Track<f32> visibilityTracks;   ///< Visibility animation
};

// ============================================================================
// Ribbon Emitter
// ============================================================================

/**
 * @brief Ribbon/trail emitter
 *
 * Creates ribbon trails that follow the emitter's movement, like sword trails,
 * missile contrails, etc.
 */
struct RibbonEmitter {
    Node node;                          ///< Base node data
    f32 heightAbove = 0.0f;             ///< Height above attachment point
    f32 heightBelow = 0.0f;             ///< Height below attachment point
    f32 alpha = 1.0f;                   ///< Ribbon opacity
    Vector3f color = Vector3f(1, 1, 1); ///< Ribbon color
    f32 lifespan = 0.0f;                ///< Ribbon segment lifetime
    u32 textureSlot = 0;                ///< Texture slot in material
    u32 emissionRate = 0;               ///< Emission rate
    u32 rows = 1;                       ///< Texture atlas rows
    u32 columns = 1;                    ///< Texture atlas columns
    u32 materialId = 0;                 ///< Material index
    f32 gravity = 0.0f;                 ///< Gravity effect

    // Animation tracks
    Track<f32> heightAboveTracks; ///< Height above animation
    Track<f32> heightBelowTracks; ///< Height below animation
    Track<f32> alphaTracks;       ///< Alpha animation
    Track<Vector3f> colorTracks;  ///< Color animation
    Track<u32> textureSlotTracks; ///< Texture slot animation
    Track<f32> visibilityTracks;  ///< Visibility animation
};

// ============================================================================
// Event Object
// ============================================================================

/**
 * @brief Animation event trigger
 *
 * Event objects fire events at specific animation frames, used to trigger
 * sounds, spawn effects, etc. synchronized with animations.
 */
struct EventObject {
    Node node;                         ///< Base node data
    u32 globalSequenceId = 0xFFFFFFFF; ///< Global sequence if looping
    std::vector<u32> eventTrackTimes;  ///< Frame numbers when events fire
};

// ============================================================================
// Camera
// ============================================================================

/**
 * @brief Camera definition
 *
 * Cameras define viewpoints that can be used for portrait renders or
 * in-game cutscenes.
 */
struct Camera {
    std::string name;              ///< Camera name
    Vector3f position;             ///< Camera position
    f32 fieldOfView = 0.0f;        ///< Field of view angle in radians
    f32 farClippingPlane = 100.0f; ///< Far clipping distance
    f32 nearClippingPlane = 0.1f;  ///< Near clipping distance
    Vector3f targetPosition;       ///< Look-at target position

    // Animation tracks
    Track<Vector3f> positionTracks;       ///< Position animation
    Track<f32> targetRotationTracks;      ///< Target rotation animation
    Track<Vector3f> targetPositionTracks; ///< Target position animation
};

// ============================================================================
// Collision Shape
// ============================================================================

/**
 * @brief Collision volume for physics
 *
 * Collision shapes define simplified geometry for collision detection.
 */
struct CollisionShape {
    enum class ShapeType : u32 { Box = 0, Plane = 1, Sphere = 2, Cylinder = 3 };
    Node node;                       ///< Base node data
    ShapeType type = ShapeType::Box; ///< Shape type
    std::vector<Vector3f> vertices;  ///< Shape vertices (box only)
    f32 radius = 0.0f;               ///< Radius (sphere/cylinder only)
};

// ============================================================================
// Face Effect (Reforged)
// ============================================================================

/**
 * @brief Facial animation effect (Reforged)
 *
 * Face effects define facial animation paths for character portraits.
 */
struct FaceEffect {
    std::string target; ///< Target facial bone/node
    std::string path;   ///< Path to facial animation data
};

// ============================================================================
// Corn Emitter (PopcornFX - Reforged)
// ============================================================================

/**
 * @brief PopcornFX particle emitter (Reforged)
 *
 * Advanced particle system using PopcornFX technology in Warcraft III: Reforged.
 */
struct CornEmitter {
    Node node;                             ///< Base node data
    f32 lifeSpan = 0.0f;                   ///< Particle lifetime
    f32 emissionRate = 0.0f;               ///< Emission rate
    f32 speed = 0.0f;                      ///< Particle speed
    Vector4f color = Vector4f(1, 1, 1, 1); ///< Particle color (RGBA)
    u32 replaceableId = 0;                 ///< Replaceable texture ID
    std::string path;                      ///< Path to PopcornFX effect
    std::string animVisibilityGuide;       ///< Animation visibility guide

    // Animation tracks
    Track<f32> lifeSpanTracks;          ///< Lifespan animation
    Track<Vector4f> colorTracks;        ///< Color animation
    Track<f32> emissionRateTracks;      ///< Emission rate animation
    Track<f32> lifeSpanVariationTracks; ///< Lifespan variation animation
    Track<f32> speedTracks;             ///< Speed animation
    Track<f32> visibilityTracks;        ///< Visibility animation
};

} // namespace mdx
} // namespace whiteout
