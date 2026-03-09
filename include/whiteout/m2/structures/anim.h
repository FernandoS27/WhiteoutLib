// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/**
 * @file anim.h
 * @brief External animation file (.anim) structures
 *
 * Defines the chunk types found in external .anim files. Animations whose
 * sequence flags have (flags & 0x130) == 0 are stored externally, with one
 * .anim file per (animId, subAnimId) pair.
 *
 * @par Pre-Legion Format
 * Raw data blobs containing timestamps and keyframe values for animation
 * tracks. Filename: `<model>%04d-%02d.anim` (animId, subAnimId).
 *
 * @par Legion 24500+ Chunked Format
 * When the M2 header sets flag 0x200000 or a .skel file exists, .anim files
 * use a chunked layout with AFM2, AFSA, and AFSB chunks.
 *
 * @see M2_FILE_FORMAT_SPECIFICATION.md Section 16 — External Animation Files
 */

#pragma once

#include <vector>
#include "../../compatibility.h"
#include "base.h"

namespace whiteout {
namespace m2 {

// ============================================================================
// ANIM File Chunk Tags (Legion 24500+)
// ============================================================================

constexpr u32 AFM2_TAG = makeTag("AFM2"); ///< Animation data chunk tag
constexpr u32 AFSA_TAG = makeTag("AFSA"); ///< Attachment skeleton animation chunk tag
constexpr u32 AFSB_TAG = makeTag("AFSB"); ///< Bone skeleton animation chunk tag

// ============================================================================
// AFM2 — Animation Data
// ============================================================================

/**
 * @brief Raw animation data chunk (Legion 24500+)
 *
 * For new-format M2s, contains animation data for Events only.
 * For old-format converted files, this is bit-identical to the pre-Legion
 * raw .anim blob.
 */
struct AFM2Chunk {
    std::vector<u8> animationData; ///< Raw animation block data
};

// ============================================================================
// AFSA — Attachment Skeleton Animation
// ============================================================================

/**
 * @brief Attachment animation data chunk (Legion 24500+)
 *
 * Contains animation keyframes for attachment points (Attachment.animate
 * tracks) that were separated from the main M2 in the skeleton split.
 */
struct AFSAChunk {
    std::vector<u8> attachmentData; ///< Raw attachment animation data
};

// ============================================================================
// AFSB — Bone Skeleton Animation
// ============================================================================

/**
 * @brief Bone animation data chunk (Legion 24500+)
 *
 * Contains animation keyframes for bone transformations (translation,
 * rotation, scale tracks) that were separated from the main M2 during
 * the skeleton refactor.
 */
struct AFSBChunk {
    std::vector<u8> boneData; ///< Raw bone animation data
};

// ============================================================================
// Complete ANIM File
// ============================================================================

/**
 * @brief Complete parsed .anim file
 *
 * Represents a single external animation file. For pre-Legion files, the raw
 * data is in afm2_chunk.animationData. For chunked files, data is split across
 * AFM2 (events), AFSA (attachments), and AFSB (bones).
 */
struct AnimProfile {
    std::optional<AFM2Chunk> afm2_chunk = std::nullopt; ///< Animation data (events for new format)
    std::optional<AFSAChunk> afsa_chunk = std::nullopt; ///< Attachment animation data
    std::optional<AFSBChunk> afsb_chunk = std::nullopt; ///< Bone animation data

    bool isChunked = false; ///< True if this file uses chunked format (Legion 24500+)
};

} // namespace m2
} // namespace whiteout
