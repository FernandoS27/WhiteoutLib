// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file chunks.h
 * @brief MD21 (chunked M2) auxiliary chunk structures
 *
 * When an M2 file uses the MD21 wrapper format (Legion+), the core MD20 data
 * is accompanied by auxiliary chunks that provide file data IDs, extended
 * particle properties, LOD configuration, rendering hints, collision, and more.
 *
 * WhiteoutLib implements 30 chunk types. Chunks can appear in any order;
 * the parser dispatches them order-independently. The writer emits them in a
 * fixed deterministic order.
 *
 * @see M2_FILE_FORMAT_SPECIFICATION.md §13 for binary layout and corpus statistics
 */

#include "base.h"

namespace whiteout {
namespace m2 {

// ============================================================================
// Chunk Tag Constants
// ============================================================================

constexpr u32 PFID_TAG = makeTag("PFID"); ///< Physics .phys fileDataId
constexpr u32 SFID_TAG = makeTag("SFID"); ///< Skin + LOD skin fileDataIds
constexpr u32 AFID_TAG = makeTag("AFID"); ///< Animation .anim fileDataIds
constexpr u32 BFID_TAG = makeTag("BFID"); ///< Bone .bone fileDataIds
constexpr u32 TXAC_TAG = makeTag("TXAC"); ///< Texture/particle combiner hints
constexpr u32 EXPT_TAG = makeTag("EXPT"); ///< Extended particle data v1
constexpr u32 EXP2_TAG = makeTag("EXP2"); ///< Extended particle data v2
constexpr u32 PABC_TAG = makeTag("PABC"); ///< Parent sequence blacklist
constexpr u32 PADC_TAG = makeTag("PADC"); ///< Parent texture weights
constexpr u32 PSBC_TAG = makeTag("PSBC"); ///< Parent sequence bounds
constexpr u32 PEDC_TAG = makeTag("PEDC"); ///< Parent event data
constexpr u32 SKID_TAG = makeTag("SKID"); ///< Skeleton .skel fileDataId
constexpr u32 TXID_TAG = makeTag("TXID"); ///< Texture fileDataIds (replaces filenames)
constexpr u32 LDV1_TAG = makeTag("LDV1"); ///< LOD configuration (16 bytes)
constexpr u32 RPID_TAG = makeTag("RPID"); ///< Recursive particle model fileDataIds
constexpr u32 GPID_TAG = makeTag("GPID"); ///< Geometry particle model fileDataIds
constexpr u32 WFV1_TAG = makeTag("WFV1"); ///< Waterfall/PBR marker v1 (empty payload)
constexpr u32 WFV2_TAG = makeTag("WFV2"); ///< Waterfall/PBR marker v2 (empty payload)
constexpr u32 WFV3_TAG = makeTag("WFV3"); ///< Waterfall/PBR data v3
constexpr u32 PFDC_TAG = makeTag("PFDC"); ///< Inline physics data (SL+)
constexpr u32 EDGF_TAG = makeTag("EDGF"); ///< Edge fade parameters
constexpr u32 NERF_TAG = makeTag("NERF"); ///< Distance-based model alpha
constexpr u32 DETL_TAG = makeTag("DETL"); ///< Detail light parameters
constexpr u32 DBOC_TAG = makeTag("DBOC"); ///< Distance-based opacity control
constexpr u32 AFRA_TAG = makeTag("AFRA"); ///< Animation frame rate (DF+)
constexpr u32 PCOL_TAG = makeTag("PCOL"); ///< Player housing collision mesh
constexpr u32 DPIV_TAG = makeTag("DPIV"); ///< Pivot displacement data
constexpr u32 TEXL_TAG = makeTag("TEXL"); ///< Textured light parameters
constexpr u32 PGD1_TAG = makeTag("PGD1"); ///< Particle geoset data

// ============================================================================
// Texture / Particle Combiner Hints (TXAC)
// ============================================================================

/**
 * @brief Texture/particle combiner hints
 *
 * Used in `CM2SceneRender::SetupTextureTransforms`. The array contains
 * `materials.count + particles.count` entries, influencing `CParticleEmitter2`
 * vertex buffer format selection.
 */
struct TXACChunk {
    std::vector<std::array<u8, 2>>
        unknown; ///< Per-material/particle combiner parameters (2 bytes each)
};

// ============================================================================
// File ID Reference Chunks
// ============================================================================

/**
 * @brief Physics file data ID (PFID)
 *
 * References the external `.phys` file. Always 4 bytes.
 */
struct PFIDChunk {
    u32 physFileDataId = 0; ///< CASC fileDataId for the .phys file
};

/**
 * @brief Skin file data IDs (SFID)
 *
 * References external `.skin` and LOD skin files. The first `numSkinProfiles`
 * entries are the main skin files; remaining entries are LOD skins.
 */
struct SFIDChunk {
    std::vector<u32> skinFileDataIds;    ///< Main skin fileDataIds (count = numSkinProfiles)
    std::vector<u32> lodSkinFileDataIds; ///< LOD skin fileDataIds (remaining entries)
};

/**
 * @brief Single animation file reference
 */
struct AFIDEntry {
    u16 animId = 0;     ///< Animation ID
    u16 subAnimId = 0;  ///< Sub-animation index
    u32 fileDataId = 0; ///< CASC fileDataId (0 = none)
};

/**
 * @brief Animation file data IDs (AFID)
 *
 * References external `.anim` files by animation ID and sub-animation index.
 */
struct AFIDChunk {
    std::vector<AFIDEntry> animFileIds; ///< Animation file references
};

/**
 * @brief Bone file data IDs (BFID)
 *
 * References external `.bone` files.
 */
struct BFIDChunk {
    std::vector<u32> boneFileDataIds; ///< CASC fileDataIds for .bone files
};

// ============================================================================
// Particle Extension Chunks
// ============================================================================

/**
 * @brief Extended particle data v1 entry (EXPT)
 *
 * If EXP2 doesn't exist, the client reconstructs it from EXPT data.
 */
struct EXPTEntry {
    f32 zSource = 0.0f;   ///< Z-axis source offset
    f32 colorMult = 0.0f; ///< Diffuse color multiplier
    f32 alphaMult = 0.0f; ///< Opacity multiplier
};

/**
 * @brief Extended particle data v1 chunk (EXPT)
 *
 * Possibly superseded by EXP2 in 7.3+.
 */
struct EXPTChunk {
    std::vector<EXPTEntry> extendedParticles; ///< Per-emitter extended particle data
};

/**
 * @brief Extended particle data v2 chunk (EXP2)
 */
struct EXP2Chunk {
    std::vector<ParticleEmitterExtension> content; ///< Per-emitter extended particle data v2
};

// ============================================================================
// Parent Data Chunks
// ============================================================================

/**
 * @brief Parent sequence blacklist (PABC)
 *
 * A flat array of animation IDs. If a target animation is in this array,
 * the parent's sequence lookup is bypassed.
 */
struct PABCChunk {
    std::vector<u16> replacementParentSequenceLookups; ///< Animation IDs to blacklist from parent
};

/**
 * @brief Parent texture weights (PADC)
 *
 * Replacement texture weights for models with a parent skeleton.
 */
struct PADCChunk {
    std::vector<TextureWeight> textureWeights; ///< Replacement texture weight animations
};

/**
 * @brief Bounding volume for a single sequence
 */
struct SequenceBounds {
    Extent extent;     ///< Axis-aligned bounding volume
    f32 radius = 0.0f; ///< Bounding sphere radius
};

/**
 * @brief Parent sequence bounds (PSBC)
 *
 * Bounding volumes for parent skeleton sequences.
 */
struct PSBCChunk {
    std::vector<SequenceBounds> parentSequenceBounds; ///< Per-sequence bounding data
};

/**
 * @brief Parent event data (PEDC)
 *
 * Animation event tracks for parent skeleton override.
 */
struct PEDCChunk {
    std::vector<AnimationTrackBase> parentEventData; ///< Event tracks from parent
};

// ============================================================================
// Skeleton / Texture File ID Chunks
// ============================================================================

/**
 * @brief Skeleton file data ID (SKID)
 *
 * References the external `.skel` skeleton file.
 */
struct SKIDChunk {
    u32 skeletonFileDataId = 0; ///< CASC fileDataId for the .skel file
};

/**
 * @brief Single texture file reference
 */
struct TXIDEntry {
    u32 fileDataId = 0; ///< CASC fileDataId for the texture
};

/**
 * @brief Texture file data IDs (TXID)
 *
 * Replaces in-file texture filenames with CASC fileDataIds (BfA+).
 * Count matches the number of textures in the MD20 header.
 */
struct TXIDChunk {
    std::vector<TXIDEntry> textureIds; ///< Per-texture fileDataIds
};

// ============================================================================
// LOD Configuration (LDV1)
// ============================================================================

/**
 * @brief LOD configuration chunk (LDV1, 16 bytes)
 *
 * Controls LOD skin selection and particle visibility at each LOD level.
 * LOD skins are selected based on `entityLodDist` / `doodadLodDist` CVars.
 * The `particleBoneLod` array determines particle suppression per LOD:
 * @code
 * u32 mask = 0x10000 << particleBoneLod[lodLevel];
 * if (mask & bones[particle.boneIndex].flags)
 *     // Suppress this emitter at this LOD
 * @endcode
 */
struct LDV1Chunk {
    u16 unknown0 = 0;    ///< Unknown
    u16 lodCount = 0;    ///< Number of LOD levels (maxLod = lodCount - 1)
    f32 unknown2 = 0.0f; ///< Used in: fmaxf(fminf(740.0/unk2, 5.0), 0.5)
    std::array<u8, 4> particleBoneLod = {0, 0, 0, 0}; ///< Per-LOD particle bone visibility mask
    u32 unknown4 = 0;                                 ///< Unknown
};

// ============================================================================
// Particle Model File ID Chunks (RPID / GPID)
// ============================================================================

/**
 * @brief Single recursive particle model file reference
 */
struct M2RPIDEntry {
    u32 fileDataId = 0; ///< CASC fileDataId for recursive particle model
};

/**
 * @brief Recursive particle model fileDataIds (RPID)
 *
 * Replaces `M2ParticleOld.recursionModelFilename` string references.
 */
struct M2RPIDChunk {
    std::vector<M2RPIDEntry> recursiveParticleModels; ///< Per-emitter recursive model IDs
};

/**
 * @brief Single geometry particle model file reference
 */
struct GPIDEntry {
    u32 fileDataId = 0; ///< CASC fileDataId for geometry particle model
};

/**
 * @brief Geometry particle model fileDataIds (GPID)
 *
 * Replaces `M2ParticleOld.geometryModelFilename` string references.
 */
struct GPIDChunk {
    std::vector<GPIDEntry> geometryParticleModels; ///< Per-emitter geometry model IDs
};

// ============================================================================
// Waterfall / PBR Chunks
// ============================================================================

/**
 * @brief Waterfall/PBR marker v1 (WFV1)
 *
 * Empty payload. Signals the model uses the waterfall render path.
 */
struct WFV1Chunk {};

/**
 * @brief Waterfall/PBR marker v2 (WFV2)
 *
 * Empty payload. Signals the model uses the waterfall render path.
 */
struct WFV2Chunk {};

// ============================================================================
// Particle Geoset Data (PGD1)
// ============================================================================

/**
 * @brief Single particle geoset entry
 */
struct PGD1Entry {
    u16 geoset = 0; ///< Geoset group ID (0 = always visible)
};

/**
 * @brief Particle geoset assignment (PGD1)
 *
 * Each entry assigns a particle emitter to a geoset group, obeying the same
 * geoset visibility rules as `SkinSection.skinSectionId`. A value of 0 means
 * the emitter is always visible.
 *
 * Binary layout: M2Array-style header (16 bytes) + u16 array padded to 16-byte alignment.
 */
struct PGD1Chunk {
    std::vector<PGD1Entry> particleGeosetData; ///< Per-emitter geoset assignment
};

// ============================================================================
// Waterfall / PBR Data v3 (WFV3)
// ============================================================================

/**
 * @brief Waterfall/PBR rendering data (WFV3, ~96 bytes)
 *
 * Despite the name, this technology is used for many non-waterfall models
 * (Shadowlands environment dressing, etc.). The "value" fields are passed
 * directly to the fragment shader.
 */
struct WFV3Data {
    f32 bumpScale = 0.0f; ///< Passed to vertex shader as bump/normal scale
    f32 value0_x = 0.0f;  ///< Shader parameter
    f32 value0_y = 0.0f;  ///< Shader parameter
    f32 value0_z = 0.0f;  ///< Shader parameter
    f32 value1_w = 0.0f;  ///< Shader parameter
    f32 value0_w = 0.0f;  ///< Shader parameter
    f32 value1_x = 0.0f;  ///< Shader parameter
    f32 value1_y = 0.0f;  ///< Shader parameter
    f32 value2_w = 0.0f;  ///< Shader parameter
    f32 value3_y = 0.0f;  ///< Shader parameter
    f32 value3_x = 0.0f;  ///< Shader parameter
    Vector4f baseColor;   ///< Base RGBA color (not BGRA)
    u16 flags = 0;        ///< Rendering flags
    u16 unknown0 = 0;     ///< Unknown
    f32 value3_w = 0.0f;  ///< Shader parameter
    f32 value3_z = 0.0f;  ///< Shader parameter
    f32 value4_y = 0.0f;  ///< Shader parameter
    f32 unknown1 = 0.0f;  ///< Unknown
    f32 unknown2 = 0.0f;  ///< Unknown
    f32 unknown3 = 0.0f;  ///< Unknown
    f32 unknown4 = 0.0f;  ///< Unknown
};

/**
 * @brief Waterfall/PBR data v3 chunk (WFV3)
 */
struct WFV3Chunk {
    WFV3Data data; ///< Waterfall rendering parameters
};

// ============================================================================
// Inline Physics Data (PFDC)
// ============================================================================

/**
 * @brief Inline physics data chunk (PFDC, SL+)
 *
 * Contains raw physics data inline rather than referencing an external .phys file.
 * Size ranges from 352 to 5584 bytes in the corpus.
 */
struct PFDCChunk {
    std::vector<u8> physicsData; ///< Raw physics binary data
    std::array<u8, 6> padding;   ///< Alignment padding
};

// ============================================================================
// Edge Fade (EDGF)
// ============================================================================

/**
 * @brief Edge fade rendering entry (24 bytes)
 *
 * Applied to meshes when `ShadowBatch.flags2 & 0x8` is set.
 * Creates edge-fade effects on mesh boundaries.
 */
struct EDGFEntry {
    std::array<f32, 2> value0 = {0.0f, 0.0f}; ///< Fade start/end distance factors [0..1]
    f32 value8 = 0.0f;                        ///< Distance multiplier or falloff [0..10]
    std::array<u8, 0xC> valueC = {0};         ///< Submesh index + reserved (12 bytes)
};

/**
 * @brief Edge fade parameters chunk (EDGF)
 *
 * Chunk is padded to 16-byte alignment.
 */
struct EDGFChunk {
    std::vector<EDGFEntry> entries; ///< Per-mesh edge fade parameters
};

// ============================================================================
// Distance-Based Model Alpha (NERF)
// ============================================================================

/**
 * @brief Distance-based alpha coefficient entry
 *
 * Creates a fade-out effect: alpha = (farDist² - dist²) / (farDist² - nearDist²)
 */
struct NERFEntry {
    Vector2f coefs; ///< (squaredFarDist, squaredNearDist)
};

/**
 * @brief Distance-based model alpha chunk (NERF)
 *
 * Multiplies the model instance's alpha by a distance-based factor, creating
 * a fade-out effect. Total chunk size: 16 bytes.
 */
struct NERFChunk {
    std::vector<NERFEntry> entries; ///< Distance coefficients
};

// ============================================================================
// Detail Lights (DETL)
// ============================================================================

/**
 * @brief Detail light parameters entry (12 bytes)
 *
 * Per-light rendering parameters. Count matches `header.lights.count`.
 */
struct DETLEntry {
    u16 flags = 0;                  ///< Light flags (always 0 in corpus)
    f16 scale = 0;                  ///< Shadow RT matrix scale (corpus: ~0.00062)
    f16 diffuseColorMultiplier = 0; ///< M2Light diffuse color multiplier (corpus: 1.0)
    u16 unknown0 = 0;               ///< Unknown (always 0)
    u32 unknown1 = 0;               ///< Unknown (always 0)
};

/**
 * @brief Detail light parameters chunk (DETL, SL+)
 *
 * Chunk is padded to 16-byte alignment.
 */
struct DETLChunk {
    std::vector<DETLEntry> records; ///< Per-light detail parameters
};

// ============================================================================
// Distance-Based Opacity Control (DBOC)
// ============================================================================

/**
 * @brief Distance-based opacity control entry (16 bytes)
 *
 * Controls per-submesh opacity based on distance. The `submeshIndex` values
 * form a sequential pattern tracking which submesh each control applies to.
 */
struct DBOCEntry {
    f32 unknown1_1 = 0.0f; ///< Distance or scale factor [0.001..10.0]
    f32 unknown1_2 = 0.0f; ///< Multiplier [0.0..13.0]
    u32 unknown1_3 = 0;    ///< Submesh/batch index reference [0..25]
    u32 unknown1_4 = 0;    ///< Reserved (always 0)
};

/**
 * @brief Distance-based opacity control chunk (DBOC, SL+)
 *
 * 1–32 entries per file. Chunk size = count × 16 (naturally aligned).
 */
struct DBOCChunk {
    std::vector<DBOCEntry> entries; ///< Per-submesh opacity controls
};

// ============================================================================
// Animation Frame Rate (AFRA)
// ============================================================================

/**
 * @brief Animation frame rate data (AFRA, DF+)
 */
struct AFRAChunk {
    std::vector<u8> data; ///< Raw frame rate data
};

// ============================================================================
// Player Housing Collision (PCOL)
// ============================================================================

/**
 * @brief Player housing collision mesh (PCOL, 11.1.7+)
 *
 * Separate collision mesh for player housing placement. Uses internal
 * offset-based layout; offsets are relative to chunk data start.
 */
struct PCOLChunk {
    std::vector<Vector3f> vertexPositions; ///< Collision vertex positions
    std::vector<Vector3f> faceNormals;     ///< Per-face normals
    std::vector<i16> indices;              ///< Triangle indices
    std::vector<i16> flags;                ///< Per-face flags
};

// ============================================================================
// Pivot Displacement (DPIV)
// ============================================================================

/**
 * @brief Pivot displacement data (DPIV, 11.1.7+)
 *
 * Small displacement offsets for model pivots. Typically 1–2 entries
 * (32 bytes each). 15 files in the corpus contain this chunk.
 */
struct DPIVChunk {
    std::array<u8, 32> data = {0}; ///< Raw pivot displacement data (32 bytes per entry)
};

// ============================================================================
// Textured Lights (TEXL)
// ============================================================================

/**
 * @brief Textured light parameters entry (16 bytes)
 *
 * Per-light texture lookup for light cookies. Count matches `header.lights.count`.
 */
struct TEXLEntry {
    f32 unknown0 = 0.0f;    ///< UV scale U (corpus: always 1.0)
    f32 unknown1 = 0.0f;    ///< UV scale V (corpus: always 1.0)
    i32 textureLookup = -1; ///< Index in TXID for light cookie, or -1 if none
    i32 unknown2 = 0;       ///< Unknown (always 0)
};

/**
 * @brief Textured light parameters chunk (TEXL, 12.0.0+)
 *
 * Only 3 files in the corpus contain this chunk.
 */
struct TEXLChunk {
    std::vector<TEXLEntry> texturedLights; ///< Per-light texture parameters
};

} // namespace m2
} // namespace whiteout
