// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/**
 * @file skeleton.h
 * @brief External skeleton file (.skel) structures
 *
 * Defines chunk types found in external .skel files. Skeleton files allow
 * multiple M2 models to share a common skeleton hierarchy, reducing data
 * duplication. When present, the .skel file's bones, attachments, sequences,
 * and global loops override the values in the MD20 header.
 *
 * Skeleton files use a parent chain: SKPD links to a parent skeleton so that
 * multiple creature variants can inherit a common base skeleton.
 *
 * @par Chunk Layout
 * - SKL1: Label/header (name, flags)
 * - SKA1: Attachments and attachment lookup
 * - SKB1: Bones and key bone lookup
 * - SKS1: Global sequences, named sequences, and sequence lookup
 * - SKPD: Parent skeleton file ID for de-duplication
 *
 * @see M2_FILE_FORMAT_SPECIFICATION.md Section 14 — External Skeleton Files
 */

#pragma once

#include "base.h"

namespace whiteout {
namespace m2 {

// ============================================================================
// Skeleton File Chunk Tags
// ============================================================================

constexpr u32 SKL1_TAG = makeTag("SKL1"); ///< Skeleton label/header chunk tag
constexpr u32 SKA1_TAG = makeTag("SKA1"); ///< Skeleton attachments chunk tag
constexpr u32 SKB1_TAG = makeTag("SKB1"); ///< Skeleton bones chunk tag
constexpr u32 SKS1_TAG = makeTag("SKS1"); ///< Skeleton sequences chunk tag
constexpr u32 SKPD_TAG = makeTag("SKPD"); ///< Skeleton parent data chunk tag

// ============================================================================
// SKL1 — Skeleton Label / Header
// ============================================================================

/**
 * @brief Skeleton identification and flags
 *
 * The header chunk for a .skel file. Contains the skeleton name and flags.
 * In Legion 7.3.2.25079, flags is always 0x100.
 */
struct SKL1Chunk {
    u32 flags = 0x100;               ///< Skeleton flags (always 0x100 in Legion)
    std::string name;                ///< Skeleton name
    std::array<u8, 4> reserved = {}; ///< Reserved (always 0 in Legion)
};

// ============================================================================
// SKA1 — Skeleton Attachments
// ============================================================================

/**
 * @brief Skeleton attachment points and lookup table
 *
 * Overrides the M2's attachment definitions when a skeleton file is present.
 * The lookup table maps attachment type IDs to indices in the attachments vector.
 */
struct SKA1Chunk {
    std::vector<Attachment> attachments;    ///< Attachment point definitions
    std::vector<u16> attachmentLookupTable; ///< Attachment type -> index lookup
};

// ============================================================================
// SKB1 — Skeleton Bones
// ============================================================================

/**
 * @brief Skeleton bone hierarchy and key bone lookup
 *
 * Overrides the M2's bone definitions when a skeleton file is present.
 * The key bone lookup maps well-known bone roles to bone indices.
 */
struct SKB1Chunk {
    std::vector<Bone> bones;        ///< Bone definitions with animation tracks
    std::vector<u16> keyBoneLookup; ///< Key bone role -> bone index lookup
};

// ============================================================================
// SKS1 — Skeleton Sequences
// ============================================================================

/**
 * @brief Skeleton animation sequences, global loops, and lookup table
 *
 * Overrides the M2's animation data when a skeleton file is present.
 * Contains global sequence durations, named animation sequences, and the
 * sequence ID hash-based lookup table.
 */
struct SKS1Chunk {
    std::vector<GlobalSequence> globalLoops; ///< Global sequence loop durations
    std::vector<Sequence> sequences;         ///< Named animation sequences
    std::vector<u16> sequenceLookups;        ///< Animation ID -> sequence index hash lookup
    std::array<u8, 8> reserved = {};         ///< Reserved (always 0 in Legion)
};

// ============================================================================
// SKPD — Skeleton Parent Data
// ============================================================================

/**
 * @brief Parent skeleton reference for skeleton de-duplication
 *
 * Links this skeleton to a parent skeleton file. The client merges parent
 * and child skeleton data to support skeleton inheritance across multiple
 * creature variants sharing a common base.
 */
struct SKPDChunk {
    std::array<u8, 8> reserved0 = {}; ///< Reserved (always 0 in Legion)
    u32 parentSkeletonFileId = 0;     ///< FileDataID of the parent .skel file
    std::array<u8, 4> reserved1 = {}; ///< Reserved (always 0 in Legion)
};

} // namespace m2
} // namespace whiteout
