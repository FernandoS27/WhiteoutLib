// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file structures.h
 * @brief Top-level M2 file system and aggregate data structures
 *
 * This file defines the top-level container types that represent an M2 model bundle:
 * - Format — Classic MD20 vs chunked MD21 discriminator
 * - BaseFile — The core .m2 file (MD20 header + optional chunks)
 * - SkinFile — A single .skin LOD profile
 * - AnimFile — A single .anim externalized animation
 * - BoneFile — A single .bone face-pose file
 * - SkeletonFile — The .skel shared skeleton
 * - PhysicsFile — Placeholder for .phys data
 * - FileSystem — The complete model bundle (base + skins + anims + bones + skeleton)
 * - FileType — Enum identifying which file type is being processed
 *
 * Include this header (or the umbrella m2.h) to access the full in-memory
 * representation of an M2 model.
 */

#include "structures/anim.h"
#include "structures/base.h"
#include "structures/bone.h"
#include "structures/chunks.h"
#include "structures/phys.h"
#include "structures/skeleton.h"
#include "structures/skin.h"
#include "types.h"

#include <limits>
#include "../compatibility.h"

namespace whiteout {
namespace m2 {

// ============================================================================
// Format Discriminator
// ============================================================================

/**
 * @brief M2 file format variant
 *
 * Distinguishes between the classic flat-binary (MD20) and modern chunked (MD21)
 * container formats. All files in the modern corpus use MD21.
 */
enum class Format : u32 {
    ClassicMD20 = 0, ///< Flat binary; file starts directly with the MD20 header
    LegionMD21 = 1,  ///< Chunked container; MD20 payload wrapped in MD21 chunk + sibling chunks

    Invalid = std::numeric_limits<u32>::max(), ///< Sentinel for uninitialized state
};

// ============================================================================
// Base M2 File
// ============================================================================

/**
 * @brief The core .m2 file containing the MD20 header and optional chunks
 *
 * In the chunked format (MD21), the MD20 header is embedded inside an MD21 chunk
 * and is followed by additional optional chunks (SFID, TXID, AFID, etc.) that
 * provide file-data IDs, extended particle data, LOD configuration, and more.
 *
 * Each optional chunk is stored as `std::optional` — `std::nullopt` means the
 * chunk was not present in the file.
 */
struct BaseFile {
    Format format = Format::Invalid; ///< Whether the source file was MD20 or MD21
    MD20Header header;               ///< Core model data (geometry, bones, materials, etc.)

    // File ID reference chunks (replace filename-based discovery with CASC IDs)
    std::optional<PFIDChunk> pfid_chunk = std::nullopt; ///< Physics .phys file data ID
    std::optional<SFIDChunk> sfid_chunk = std::nullopt; ///< Skin + LOD skin file data IDs
    std::optional<AFIDChunk> afid_chunk = std::nullopt; ///< Animation .anim file data IDs
    std::optional<BFIDChunk> bfid_chunk = std::nullopt; ///< Bone .bone file data IDs
    std::optional<SKIDChunk> skid_chunk = std::nullopt; ///< Skeleton .skel file data ID
    std::optional<TXIDChunk> txid_chunk =
        std::nullopt; ///< Texture file data IDs (replaces filenames)

    // Particle extension chunks
    std::optional<TXACChunk> txac_chunk = std::nullopt; ///< Texture/particle combiner hints
    std::optional<EXPTChunk> expt_chunk = std::nullopt; ///< Extended particle data v1
    std::optional<EXP2Chunk> exp2_chunk =
        std::nullopt; ///< Extended particle data v2 (alpha cutoff)

    // Parent data chunks (for models with parent skeletons)
    std::optional<PABCChunk> pabc_chunk = std::nullopt; ///< Parent sequence blacklist
    std::optional<PADCChunk> padc_chunk = std::nullopt; ///< Parent texture weights
    std::optional<PSBCChunk> psbc_chunk = std::nullopt; ///< Parent sequence bounds
    std::optional<PEDCChunk> pedc_chunk = std::nullopt; ///< Parent event data

    // LOD and rendering chunks
    std::optional<LDV1Chunk> ldv1_chunk = std::nullopt;   ///< LOD configuration (16 bytes)
    std::optional<M2RPIDChunk> rpid_chunk = std::nullopt; ///< Recursive particle model file IDs
    std::optional<GPIDChunk> gpid_chunk = std::nullopt;   ///< Geometry particle model file IDs

    // Waterfall / PBR chunks
    std::optional<WFV1Chunk> wfv1_chunk = std::nullopt; ///< Waterfall/PBR marker v1
    std::optional<WFV2Chunk> wfv2_chunk = std::nullopt; ///< Waterfall/PBR marker v2
    std::optional<WFV3Chunk> wfv3_chunk = std::nullopt; ///< Waterfall/PBR data v3

    // Misc chunks
    std::optional<PGD1Chunk> pgd1_chunk = std::nullopt; ///< Particle geoset data
    std::optional<PFDCChunk> pfdc_chunk = std::nullopt; ///< Inline physics data (raw bytes)
    std::optional<EDGFChunk> edgf_chunk = std::nullopt; ///< Edge fade parameters
    std::optional<NERFChunk> nerf_chunk = std::nullopt; ///< Distance-based model alpha
    std::optional<DETLChunk> detl_chunk = std::nullopt; ///< Detail light parameters
    std::optional<DBOCChunk> dboc_chunk = std::nullopt; ///< Distance-based opacity control
    std::optional<AFRAChunk> afra_chunk = std::nullopt; ///< Animation frame rate data
    std::optional<PCOLChunk> pcol_chunk = std::nullopt; ///< Player housing collision mesh
    std::optional<DPIVChunk> dpiv_chunk = std::nullopt; ///< Pivot displacement data
    std::optional<TEXLChunk> texl_chunk = std::nullopt; ///< Textured light parameters
};

// ============================================================================
// Skin File
// ============================================================================

/**
 * @brief A single .skin LOD profile
 *
 * Skin files define Level-of-Detail views onto the model's vertex data.
 * Starting from WotLK, skins are stored as external files. Each skin
 * contains index buffers, submesh definitions, and draw batches.
 */
struct SkinFile {
    SkinProfile profile;    ///< Skin profile data (vertices, indices, submeshes, batches)
    bool isLodSkin = false; ///< True if this is a LOD skin (e.g., _lod01.skin)
    int lodLevel = 0;       ///< LOD level (0 = base quality)
    int index = 0;          ///< Skin index within its category (base or LOD)
};

// ============================================================================
// Animation File
// ============================================================================

/**
 * @brief A single .anim externalized animation file
 *
 * Animation files externalize keyframe data for low-priority sequences
 * (e.g., emotes, one-shot animations) to allow lazy loading. Files may
 * be raw binary blobs (pre-Legion) or chunked (AFM2/AFSA/AFSB).
 */
struct AnimFile {
    AnimProfile profile; ///< Animation data (chunked or raw)
    u32 animId = 0;      ///< Animation ID (from filename: {stem}{animId:04d}-{variant:02d}.anim)
    u32 variant = 0;     ///< Sub-animation variant index
};

// ============================================================================
// Bone File
// ============================================================================

/**
 * @brief A single .bone face-pose data file
 *
 * Bone files contain per-bone data for the face-pose system. They provide
 * bone ID arrays and offset transformation matrices used for facial animation.
 */
struct BoneFile {
    BONEHeader header;                                  ///< Bone file header (version marker)
    std::optional<BIDAChunk> bida_chunk = std::nullopt; ///< Bone ID array
    std::optional<BOMTChunk> bomt_chunk = std::nullopt; ///< Bone offset matrices
    u32 boneId = 0; ///< Bone index (from filename: {stem}_{boneId:02d}.bone)
};

// ============================================================================
// Skeleton File
// ============================================================================

/**
 * @brief The .skel shared skeleton file
 *
 * Skeleton files allow bone, attachment, and sequence data to be shared across
 * models via a parent-child hierarchy. When a model references a .skel via the
 * SKID chunk, the skeleton data overrides or supplements the M2 header's
 * bone/attachment/sequence arrays.
 */
struct SkeletonFile {
    std::optional<SKL1Chunk> skl1_chunk = std::nullopt; ///< Skeleton header (name, flags)
    std::optional<SKA1Chunk> ska1_chunk = std::nullopt; ///< Skeleton attachments
    std::optional<SKB1Chunk> skb1_chunk = std::nullopt; ///< Skeleton bones + key-bone lookup
    std::optional<SKS1Chunk> sks1_chunk = std::nullopt; ///< Skeleton sequences + global loops
    std::optional<SKPDChunk> skpd_chunk = std::nullopt; ///< Parent skeleton file ID reference
    std::optional<AFIDChunk> afid_chunk = std::nullopt; ///< Forwarded animation file IDs
    std::optional<BFIDChunk> bfid_chunk = std::nullopt; ///< Forwarded bone file IDs
};

// ============================================================================
// Physics File
// ============================================================================

/**
 * @brief Placeholder for .phys ragdoll physics data
 *
 * Physics files define rigid bodies, collision shapes, and constraints for
 * ragdoll simulation. Currently a placeholder — structured physics data is
 * stored as raw bytes in the PFDC chunk instead.
 */
struct PhysicsFile {
    // placeholder;
};

// ============================================================================
// File Type Discriminator
// ============================================================================

/**
 * @brief Identifies which type of M2 bundle file is being parsed
 */
enum class FileType {
    Base,     ///< The core .m2 file
    Skin,     ///< A .skin LOD profile
    Anim,     ///< A .anim externalized animation
    Skeleton, ///< A .skel shared skeleton
    Physics,  ///< A .phys ragdoll physics file
};

// ============================================================================
// Complete File System (Model Bundle)
// ============================================================================

/**
 * @brief Complete M2 model bundle (all associated files)
 *
 * Represents the entire in-memory state of an M2 model and its associated files.
 * The parser populates this by discovering and loading all sibling files
 * (.skin, .skel, .bone, .anim) in the same directory as the base .m2 file.
 *
 * Typical bundle discovery order:
 * 1. Base .m2 (detect MD20 vs MD21)
 * 2. Base skins (sorted by index)
 * 3. LOD skins (sorted by LOD level)
 * 4. .skel skeleton
 * 5. .bone files (sorted by bone ID)
 * 6. .anim files (sorted by anim ID, then variant)
 */
struct FileSystem {
    std::string baseName;                                ///< Model stem name (e.g., "scorpion")
    BaseFile base;                                       ///< Core .m2 file data
    std::vector<SkinFile> skins;                         ///< All skin profiles (base + LOD)
    std::vector<AnimFile> anims;                         ///< Externalized animation files
    std::vector<BoneFile> bones;                         ///< Face-pose bone files
    std::optional<SkeletonFile> skeleton = std::nullopt; ///< Shared skeleton (if present)
    std::optional<PhysicsFile> physics = std::nullopt;   ///< Physics data (placeholder)
};

} // namespace m2
} // namespace whiteout
