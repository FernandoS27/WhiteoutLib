// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file types.h
 * @brief Core type definitions and constants for the MDX format
 * 
 * This file defines:
 * - Chunk tag constants (4-character codes identifying different data sections)
 * - Track types for animation data
 * - The main Model structure that represents a complete model
 * 
 * MDX files are organized as a series of tagged chunks. Each chunk contains
 * a specific type of data (vertices, materials, animations, etc.).
 */

#include <cstdint>
#include <vector>
#include <string>
#include <array>
#include <map>
#include <memory>
#include <span>

#include "../common_types.h"

namespace whiteout {
namespace mdx {

// ============================================================================
// Extent Type
// ============================================================================

struct Extent {
    f32 boundsRadius;
    Vector3f minimum;
    Vector3f maximum;
};

// ============================================================================
// Chunk Tags (as u32)
// ============================================================================

/**
 * @brief Helper function to create a chunk tag from a 4-character string at compile time
 * 
 * MDX files use 4-character tags stored as little-endian u32 values.
 * This function converts a 4-character string literal to the appropriate u32.
 * 
 * @tparam N Size of the string literal (must be 5 including null terminator)
 * @param str 4-character string literal
 * @return u32 Little-endian chunk tag value
 * 
 * @example
 * constexpr u32 MY_TAG = makeTag(\"TEST\");
 */
template<std::size_t N>
constexpr u32 makeTag(const char (&str)[N]) {
    static_assert(N == 5, "Tag must be exactly 4 characters (plus null terminator)");
    return static_cast<u32>(static_cast<u8>(str[0])) |
           (static_cast<u32>(static_cast<u8>(str[1])) << 8) |
           (static_cast<u32>(static_cast<u8>(str[2])) << 16) |
           (static_cast<u32>(static_cast<u8>(str[3])) << 24);
}

// Example usage: constexpr u32 MY_TAG = makeTag("TEST");
// Compile-time verification that makeTag produces correct little-endian values
static_assert(makeTag("MDLX") == 0x584C444D, "makeTag produces correct little-endian encoding");

// Main file chunks
constexpr u32 MDLX_TAG = makeTag("MDLX"); ///< File signature - must be first chunk
constexpr u32 VERS_TAG = makeTag("VERS"); ///< Version number (800, 900, 1000, 1100, 1200)
constexpr u32 MODL_TAG = makeTag("MODL"); ///< Model information (name, bounds, etc.)
constexpr u32 SEQS_TAG = makeTag("SEQS"); ///< Animation sequences
constexpr u32 GLBS_TAG = makeTag("GLBS"); ///< Global sequences (looping animations)
constexpr u32 TEXS_TAG = makeTag("TEXS"); ///< Texture paths
constexpr u32 SNDS_TAG = makeTag("SNDS"); ///< Sound tracks (deprecated)
constexpr u32 MTLS_TAG = makeTag("MTLS"); ///< Materials (shaders and rendering properties)
constexpr u32 TXAN_TAG = makeTag("TXAN"); ///< Texture animations
constexpr u32 GEOS_TAG = makeTag("GEOS"); ///< Geosets (mesh geometry)
constexpr u32 GEOA_TAG = makeTag("GEOA"); ///< Geoset animations (alpha, color)
constexpr u32 BONE_TAG = makeTag("BONE"); ///< Bones (skeleton hierarchy)
constexpr u32 LITE_TAG = makeTag("LITE"); ///< Lights
constexpr u32 HELP_TAG = makeTag("HELP"); ///< Helper nodes (attachment points, etc.)
constexpr u32 ATCH_TAG = makeTag("ATCH"); ///< Attachments (for equipping items)
constexpr u32 PIVT_TAG = makeTag("PIVT"); ///< Pivot points for bones
constexpr u32 PREM_TAG = makeTag("PREM"); ///< Particle emitters (version 1)
constexpr u32 PRE2_TAG = makeTag("PRE2"); ///< Particle emitters version 2
constexpr u32 RIBB_TAG = makeTag("RIBB"); ///< Ribbon emitters (trails)
constexpr u32 EVTS_TAG = makeTag("EVTS"); ///< Event objects (sound triggers, etc.)
constexpr u32 CAMS_TAG = makeTag("CAMS"); ///< Cameras
constexpr u32 CLID_TAG = makeTag("CLID"); ///< Collision shapes
constexpr u32 BPOS_TAG = makeTag("BPOS"); ///< Bind poses (Reforged skinning)
constexpr u32 FAFX_TAG = makeTag("FAFX"); ///< Face effects (Reforged facial animation)
constexpr u32 CORN_TAG = makeTag("CORN"); ///< Corn emitters (PopcornFX - Reforged)
constexpr u32 LAYS_TAG = makeTag("LAYS"); ///< Material layers
constexpr u32 KEVT_TAG = makeTag("KEVT"); ///< Event track keys

// Geoset sub-chunk tags
constexpr u32 VRTX_TAG = makeTag("VRTX"); ///< Vertex positions
constexpr u32 NRMS_TAG = makeTag("NRMS"); ///< Vertex normals
constexpr u32 PTYP_TAG = makeTag("PTYP"); ///< Face type groups
constexpr u32 PCNT_TAG = makeTag("PCNT"); ///< Face group counts
constexpr u32 PVTX_TAG = makeTag("PVTX"); ///< Face vertex indices
constexpr u32 GNDX_TAG = makeTag("GNDX"); ///< Vertex group assignments
constexpr u32 MTGC_TAG = makeTag("MTGC"); ///< Matrix group counts
constexpr u32 MATS_TAG = makeTag("MATS"); ///< Matrix indices
constexpr u32 UVAS_TAG = makeTag("UVAS"); ///< Texture coordinate set headers
constexpr u32 UVBS_TAG = makeTag("UVBS"); ///< Texture coordinate data
constexpr u32 TANG_TAG = makeTag("TANG"); ///< Tangent vectors (for normal mapping)
constexpr u32 SKIN_TAG = makeTag("SKIN"); ///< Skin weights and bone indices

// Track chunk tags (Node transformations)
constexpr u32 KGTR_TAG = makeTag("KGTR"); ///< Node translation animation
constexpr u32 KGRT_TAG = makeTag("KGRT"); ///< Node rotation animation (quaternion)
constexpr u32 KGSC_TAG = makeTag("KGSC"); ///< Node scaling animation

// Track chunk tags (Material/Layer animations)
constexpr u32 KMTF_TAG = makeTag("KMTF"); ///< Material texture ID animation
constexpr u32 KMTA_TAG = makeTag("KMTA"); ///< Material alpha animation
constexpr u32 KMTE_TAG = makeTag("KMTE"); ///< Emissive gain animation (Reforged)
constexpr u32 KFC3_TAG = makeTag("KFC3"); ///< Fresnel color animation (Reforged)
constexpr u32 KFCA_TAG = makeTag("KFCA"); ///< Fresnel alpha animation (Reforged)
constexpr u32 KFTC_TAG = makeTag("KFTC"); ///< Fresnel team color animation (Reforged)

// Track chunk tags (TextureAnimation)
constexpr u32 KTAT_TAG = makeTag("KTAT"); ///< Texture translation animation
constexpr u32 KTAR_TAG = makeTag("KTAR"); ///< Texture rotation animation
constexpr u32 KTAS_TAG = makeTag("KTAS"); ///< Texture scaling animation

// Track chunk tags (GeosetAnimation)
constexpr u32 KGAO_TAG = makeTag("KGAO"); ///< Geoset alpha animation
constexpr u32 KGAC_TAG = makeTag("KGAC"); ///< Geoset color animation

// Track chunk tags (RibbonEmitter)
constexpr u32 KRHA_TAG = makeTag("KRHA"); ///< Ribbon height above animation
constexpr u32 KRHB_TAG = makeTag("KRHB"); ///< Ribbon height below animation
constexpr u32 KRAL_TAG = makeTag("KRAL"); ///< Ribbon alpha animation
constexpr u32 KRCO_TAG = makeTag("KRCO"); ///< Ribbon color animation
constexpr u32 KRTX_TAG = makeTag("KRTX"); ///< Ribbon texture slot animation
constexpr u32 KRVS_TAG = makeTag("KRVS"); ///< Ribbon visibility animation

// Track chunk tags (ParticleEmitter & ParticleEmitter2)
constexpr u32 KPEE_TAG = makeTag("KPEE"); ///< Particle emission rate animation
constexpr u32 KPEG_TAG = makeTag("KPEG"); ///< Particle gravity animation
constexpr u32 KPLN_TAG = makeTag("KPLN"); ///< Particle longitude animation
constexpr u32 KPLT_TAG = makeTag("KPLT"); ///< Particle latitude animation
constexpr u32 KPEL_TAG = makeTag("KPEL"); ///< Particle lifespan animation
constexpr u32 KPES_TAG = makeTag("KPES"); ///< Particle speed animation
constexpr u32 KPEV_TAG = makeTag("KPEV"); ///< Particle visibility animation
constexpr u32 KP2S_TAG = makeTag("KP2S"); ///< Particle2 speed animation
constexpr u32 KP2R_TAG = makeTag("KP2R"); ///< Particle2 variation animation
constexpr u32 KP2L_TAG = makeTag("KP2L"); ///< Particle2 latitude animation
constexpr u32 KP2G_TAG = makeTag("KP2G"); ///< Particle2 gravity animation
constexpr u32 KP2E_TAG = makeTag("KP2E"); ///< Particle2 emission rate animation
constexpr u32 KP2N_TAG = makeTag("KP2N"); ///< Particle2 length animation
constexpr u32 KP2W_TAG = makeTag("KP2W"); ///< Particle2 width animation
constexpr u32 KP2V_TAG = makeTag("KP2V"); ///< Particle2 visibility animation

// Track chunk tags (Light)
constexpr u32 KLAS_TAG = makeTag("KLAS"); ///< Light attenuation start animation
constexpr u32 KLAE_TAG = makeTag("KLAE"); ///< Light attenuation end animation
constexpr u32 KLAC_TAG = makeTag("KLAC"); ///< Light color animation
constexpr u32 KLAI_TAG = makeTag("KLAI"); ///< Light intensity animation
constexpr u32 KLBI_TAG = makeTag("KLBI"); ///< Light ambient intensity animation
constexpr u32 KLBC_TAG = makeTag("KLBC"); ///< Light ambient color animation
constexpr u32 KLAV_TAG = makeTag("KLAV"); ///< Light visibility animation

// Track chunk tags (Camera)
constexpr u32 KCTR_TAG = makeTag("KCTR"); ///< Camera position animation
constexpr u32 KCRL_TAG = makeTag("KCRL"); ///< Camera target rotation animation
constexpr u32 KTTR_TAG = makeTag("KTTR"); ///< Camera target position animation

// Track chunk tags (Attachment)
constexpr u32 KATV_TAG = makeTag("KATV"); ///< Attachment visibility animation

// Track chunk tags (CornEmitter - PopcornFX)
constexpr u32 KPPA_TAG = makeTag("KPPA"); ///< Corn lifespan animation
constexpr u32 KPPC_TAG = makeTag("KPPC"); ///< Corn color animation
constexpr u32 KPPE_TAG = makeTag("KPPE"); ///< Corn emission rate animation
constexpr u32 KPPL_TAG = makeTag("KPPL"); ///< Corn lifespan variation animation
constexpr u32 KPPS_TAG = makeTag("KPPS"); ///< Corn speed animation
constexpr u32 KPPV_TAG = makeTag("KPPV"); ///< Corn visibility animation

// ============================================================================
// Data Structure Forward Declarations
// ============================================================================

// Forward declarations for all MDX structure types
struct Node;
struct Sequence;
struct Texture;
struct SoundTrack;
struct Material;
struct Layer;
struct TextureAnimation;
struct Geoset;
struct GeosetAnimation;
struct Bone;
struct Light;
struct Helper;
struct Attachment;
struct ParticleEmitter;
struct ParticleEmitter2;
struct RibbonEmitter;
struct EventObject;
struct Camera;
struct CollisionShape;
struct FaceEffect;
struct CornEmitter;

// ============================================================================
// Track Types
// ============================================================================

/**
 * @brief Header information for an animation track
 * 
 * Each animation track starts with this header which describes the type
 * of interpolation and how many keyframes follow.
 */
struct TrackHeader {
    u32 tag;                 ///< Chunk tag identifying the track type
    u32 count;               ///< Number of keyframes in this track
    u32 interpolationType;   ///< Interpolation: 0=none, 1=linear, 2=hermite, 3=bezier
    u32 globalSequenceId;    ///< Global sequence ID or 0xFFFFFFFF if not global
};

enum class InterpolationType : u32 {
    None = 0,
    Linear = 1,
    Hermite = 2,
    Bezier = 3
};

constexpr bool isSmoothInterpolation(InterpolationType t) {
    return t == InterpolationType::Hermite || t == InterpolationType::Bezier;
}

/**
 * @brief Generic animation track with keyframe data
 * 
 * Tracks store animation data as a series of keyframes. The interpolation type
 * determines how values between keyframes are calculated.
 * 
 * @tparam T The value type (f32, Vector3f, Vector4f, u32, etc.)
 */
template<typename T>
struct Track {
    /**
     * @brief Simple keyframe with frame number and value
     * Used for linear and no interpolation.
     */
    struct Key {
        u32 frame = 0;  ///< Frame number in the animation
        T value{};      ///< Value at this frame
    };

    /**
     * @brief Keyframe with tangent information for smooth curves
     * Used for Hermite and Bezier interpolation.
     */
    struct TangentKey {
        u32 frame = 0;  ///< Frame number in the animation
        T value{};      ///< Value at this frame
        T inTan{};      ///< Incoming tangent for curve interpolation
        T outTan{};     ///< Outgoing tangent for curve interpolation
    };

    bool isUsed = false;                                            ///< Whether this track contains animation data
    InterpolationType interpolationType = InterpolationType::None;  ///< Interpolation type
    u32 globalSequenceId = 0;                                       ///< Global sequence ID if applicable
    size_t keyCount = 0;                                            ///< Number of keyframes
    std::vector<u8> keys_data;                                      ///< Raw keyframe data

    /**
     * @brief Get keyframes as a span (for linear/no interpolation)
     * @return Span of Key structures
     */
    std::span<Key> keys() {
        return std::span<Key>(reinterpret_cast<Key*>(keys_data.data()), keys_data.size() / sizeof(Key));
    }

    /**
     * @brief Get keyframes with tangents (for Hermite/Bezier interpolation)
     * @return Span of TangentKey structures
     */
    std::span<TangentKey> tangentKeys() {
        return std::span<TangentKey>(reinterpret_cast<TangentKey*>(keys_data.data()), keys_data.size() / sizeof(TangentKey));
    }
};

// ============================================================================
// Main MDX File Structure
// ============================================================================

/**
 * @brief Complete MDX model file structure
 * 
 * This structure represents an entire MDX model, including all geometry,
 * materials, animations, and metadata. It can represent models from
 * Warcraft III Classic (version 800) through Reforged (version 1200).
 * 
 * The structure is organized into chunks, each containing a specific type
 * of data. Not all chunks are required - optional chunks can have empty vectors.
 */
struct Model {
    u32 version = 800;  ///< MDX version: 800 (Classic), 900, 1000, 1100, 1200 (Reforged)
    
    // Model metadata
    std::string modelName;           ///< Name of the model
    std::string animationFileName;   ///< Path to external animation file (rarely used)
    Extent modelExtent;              ///< Bounding volume for the entire model
    u32 blendTime = 0;               ///< Time in milliseconds to blend between animations
    
    // Animation and timing
    std::vector<u32> globalSequences;           ///< Global looping animation durations
    std::vector<Sequence> sequences;            ///< Named animation sequences
    
    // Textures and materials
    std::vector<Texture> textures;              ///< Texture paths and settings
    std::vector<SoundTrack> soundTracks;        ///< Sound tracks (deprecated)
    std::vector<Material> materials;            ///< Material definitions
    std::vector<TextureAnimation> textureAnimations;  ///< UV animation data
    
    // Geometry
    std::vector<Geoset> geosets;                ///< Mesh geometry
    std::vector<GeosetAnimation> geosetAnimations;  ///< Mesh visibility/color animation
    
    // Hierarchy and attachments
    std::vector<Bone> bones;                    ///< Skeleton bones
    std::vector<Helper> helpers;                ///< Helper/attachment nodes
    std::vector<Attachment> attachments;        ///< Equipment attachment points
    std::vector<Vector3f> pivotPoints;          ///< Pivot points for nodes
    
    // Lights
    std::vector<Light> lights;                  ///< Light sources
    
    // Particle systems
    std::vector<ParticleEmitter> particleEmitters;    ///< Particle emitters v1
    std::vector<ParticleEmitter2> particleEmitters2;  ///< Particle emitters v2
    std::vector<RibbonEmitter> ribbonEmitters;        ///< Ribbon trail effects
    std::vector<CornEmitter> cornEmitters;            ///< PopcornFX emitters (Reforged)
    
    // Events and cameras
    std::vector<EventObject> eventObjects;      ///< Animation event triggers
    std::vector<Camera> cameras;                ///< Camera definitions
    
    // Collision and effects
    std::vector<CollisionShape> collisionShapes;  ///< Collision geometry
    std::vector<FaceEffect> faceEffects;          ///< Facial effects (Reforged)
    
    // Reforged skinning (version > 800)
    std::vector<std::array<f32, 12>> bindPoses;   ///< Bind pose matrices (3x4 each)
};

} // namespace mdx
} // namespace whiteout
