// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file skin.h
 * @brief M2 skin (LOD view) structures
 *
 * Skin files define Level-of-Detail (LOD) views onto the model's global vertex
 * data. Starting from WotLK, they are stored as external `.skin` files; earlier
 * versions embedded them in the `.m2` file. Each `.skin` file contains a single
 * SkinProfile with index buffers, submesh definitions, and draw batches.
 *
 * The `vertices` array is a level of indirection: `indices[i]` indexes into
 * `vertices[]`, and `vertices[j]` indexes into the M2's global vertex array.
 *
 * @see M2_FILE_FORMAT_SPECIFICATION.md §12 for binary layout details
 */

#include "base.h"

namespace whiteout {
namespace m2 {

constexpr u32 SKIN_TAG = makeTag("SKIN"); ///< "SKIN" magic (0x534B494E)

// ============================================================================
// Skin Section (Submesh)
// ============================================================================

/**
 * @brief Submesh definition within a skin profile
 *
 * Skin sections partition the model geometry into submeshes for geoset-based
 * visibility toggling (e.g., hairstyles, armor pieces). The `skinSectionId`
 * groups submeshes — only one submesh per group is active at a time based on
 * equipped items or character customization.
 *
 * Binary layout: 48 bytes per entry.
 */
struct SkinSection {
    u16 skinSectionId = 0;       ///< Submesh group identifier (for geoset toggling)
    u16 level = 0;               ///< LOD level (0 = highest detail)
    u16 vertexStart = 0;         ///< Start index in skin's vertices[]
    u16 vertexCount = 0;         ///< Number of vertices in this submesh
    u16 indexStart = 0;          ///< Start index in skin's indices[]
    u16 indexCount = 0;          ///< Number of triangle indices
    u16 boneCount = 0;           ///< Number of bones used by this submesh
    u16 boneComboIndex = 0;      ///< Start index in header's boneCombos[]
    u16 boneInfluences = 0;      ///< Max bones per vertex (1–4)
    u16 centerBoneIndex = 0;     ///< Bone used for distance calculation
    Vector3f centerPosition;     ///< Center position for culling
    Vector3f sortCenterPosition; ///< Center position for draw-order sorting
    f32 sortRadius = 0.0f;       ///< Sorting radius
};

// ============================================================================
// Batch (Texture Unit / Draw Call)
// ============================================================================

/**
 * @brief Draw call definition (24 bytes)
 *
 * Each batch represents a single draw call. The `shaderId` selects the
 * vertex/pixel shader combination and which UV sets to pass. Material,
 * texture, and animation indices use the combo tables as indirection layers,
 * enabling multiple skin sections to share resources efficiently.
 *
 * When `UseTextureCombinerCombos` is set in global flags, `shaderId` also
 * indexes into the `textureCombinerCombos` array.
 */
struct Batch {
    u8 flags = 0;                       ///< Batch flags
    i8 priorityPlane = 0;               ///< Rendering priority plane
    u16 shaderId = 0;                   ///< Vertex/pixel shader selector
    u16 skinSectionIndex = 0;           ///< Index into skin's submeshes[]
    u16 geosetIndex = 0;                ///< Geoset index
    i16 colorIndex = -1;                ///< Index into header's colors[], or -1 for none
    u16 materialIndex = 0;              ///< Index into header's materials[]
    u16 materialLayer = 0;              ///< Material layer for multi-pass rendering
    u16 textureCount = 0;               ///< Number of textures used (1–2)
    u16 textureComboIndex = 0;          ///< Start index into header's textureCombos[]
    u16 textureCoordComboIndex = 0;     ///< Start index into header's textureCoordCombos[]
    u16 textureWeightComboIndex = 0;    ///< Index into header's textureWeightCombos[]
    u16 textureTransformComboIndex = 0; ///< Start index into header's textureTransformCombos[]
};

// ============================================================================
// Shadow Batch
// ============================================================================

/**
 * @brief Shadow rendering pass definition (12 bytes)
 *
 * Present from Cata onwards (version >= 265). Defines shadow rendering
 * for submeshes. When `flags2 & 0x8`, edge-fade rendering is enabled
 * using the EDGF chunk data.
 */
struct ShadowBatch {
    u8 flags = 0;           ///< Shadow flags (common: 0x10, 0x90, 0x80)
    u8 flags2 = 0;          ///< Secondary flags (0x8 = use EDGF edge fade data)
    u16 unknown0 = 0;       ///< Unknown
    u16 submeshId = 0;      ///< Submesh index
    u16 textureId = 0;      ///< Texture index
    u16 colorId = 0;        ///< Color animation index
    u16 transparencyId = 0; ///< Transparency animation index
};

// ============================================================================
// Skin Profile
// ============================================================================

/**
 * @brief Complete skin (LOD view) file structure
 *
 * Encapsulates all data from a single `.skin` file. The `vertices` array
 * is an indirection: `indices[i]` indexes into `vertices[]`, and
 * `vertices[j]` indexes into the M2's global vertex array (offset by
 * `lodVertexBase` for HD models with >65535 vertices).
 *
 * Binary layout: 64 bytes header (Cata+), 48 bytes (pre-Cata).
 */
struct SkinProfile {
    u32 magic = SKIN_TAG; ///< File magic ("SKIN" = 0x534B494E)

    std::vector<u16> vertices;            ///< Indices into M2 global vertex array
    std::vector<u16> indices;             ///< Triangle indices into vertices[]
    std::vector<std::array<u8, 4>> bones; ///< Per-vertex bone remapping (4 bone indices)
    std::vector<SkinSection> submeshes;   ///< Submesh definitions
    std::vector<Batch> batches;           ///< Draw call definitions

    /// Cumulative vertex base for LOD skins. Always 0 for the base skin (skin00).
    /// For LOD skin N, equals the sum of vertexCount from skins 0..N-1.
    /// This offsets u16 vertex indices into the M2's global vertex array, allowing
    /// models with >65535 total vertices to use multiple LOD skins while keeping
    /// per-skin indices within u16 range.
    u32 lodVertexBase = 0;

    /// Shadow rendering batches (present from Cata onwards, version >= 265).
    std::vector<ShadowBatch> shadowBatches;
};

} // namespace m2
} // namespace whiteout
