// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/**
 * @file bone.h
 * @brief External bone file (.bone) structures
 *
 * Defines chunk types found in external .bone files. Bone files provide
 * per-bone offset/transform data used for face-pose animations and other
 * bone-specific overrides. Introduced in the Legion skeleton refactor.
 *
 * Binary layout: BIDA chunk (bone ID list) + BOMT chunk (offset matrices).
 *
 * @see M2_FILE_FORMAT_SPECIFICATION.md Section 15 — External Bone Files
 */

#pragma once

#include <vector>
#include "base.h"

namespace whiteout {
namespace m2 {

// ============================================================================
// BONE File Chunk Tags
// ============================================================================

constexpr u32 BIDA_TAG = makeTag("BIDA"); ///< Bone ID array chunk tag
constexpr u32 BOMT_TAG = makeTag("BOMT"); ///< Bone offset matrix table chunk tag

// ============================================================================
// BONE Header
// ============================================================================

/**
 * @brief Bone file header
 *
 * Contains a single version field. Currently always 1 in all known files.
 */
struct BONEHeader {
    u32 unk = 1; ///< Version (always 1; ignored by parser)
};

// ============================================================================
// BIDA — Bone ID Array
// ============================================================================

/**
 * @brief Bone ID mapping for face-pose animations
 *
 * Maps entries in the BOMT chunk to specific bone indices. Count should equal
 * the number of FacePose (animation ID 808) sequences minus one.
 */
struct BIDAChunk {
    std::vector<u16> boneIds; ///< Bone indices (count = number of FacePose sequences - 1)
};

// ============================================================================
// BOMT — Bone Offset Matrix Table
// ============================================================================

/**
 * @brief Bone transformation matrices for face-pose overrides
 *
 * Contains 4x4 transformation matrices, one per entry in BIDA. Applied as
 * offset transforms for the corresponding bones during face-pose playback.
 */
struct BOMTChunk {
    std::vector<Matrix44f> boneOffsetMatrices; ///< Transformation matrices (same count as BIDA)
};

} // namespace m2
} // namespace whiteout
