// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file mesh.h
 * @brief Skeleton and mesh structures — bones, regions, batches, divisions
 *
 * Defines the mesh-related M3 structures: Bone (BONE) for the skeleton
 * hierarchy, Region (REGN) for sub-mesh vertex/index ranges, Batch (BAT_) for
 * draw calls, MeshSection (MSEC) for per-node animated bounds, MeshDivision
 * (DIV_) grouping faces/regions/batches, InitialReference (IREF) for inverse
 * bind-pose matrices, and AttachmentPoint (ATT_) for named bone locations.
 *
 * @see M3_FILE_FORMAT_SPECIFICATION.md §9 Skeleton & Mesh
 */

#include "base.h"

namespace whiteout {
namespace m3 {

// ============================================================================
// Skeleton & Mesh
// ============================================================================

/**
 * @brief BONE — Skeleton bone (v0–v1, 160 bytes)
 *
 * Each bone has a parent index, animated position/rotation/scale/visibility,
 * and flags controlling inheritance, billboard mode, and IK.
 */
struct Bone {
    u32 unknown;                     ///< Unknown field
    std::string name;                ///< Bone name (Ref<CHAR>)
    BoneFlag flags = BoneFlag::None; ///< Bone flags (inherit, billboard, IK, skin)
    u16 parentIndex;                 ///< Parent bone index (0xFFFF = root)
    u16 padding;                     ///< Alignment padding
    AnimRef<Vector3f> position;      ///< Animated translation (36 bytes)
    AnimRef<Quaternion> rotation;    ///< Animated rotation (44 bytes)
    AnimRef<Vector3f> scale;         ///< Animated scale (36 bytes)
    AnimRef<u32> visibility;         ///< Animated visibility flag (20 bytes)
    M3_DEFINE_VERSION_ACCESSORS()
};

/**
 * @brief REGN — Region / submesh (v0–v5, 48 bytes)
 *
 * Describes a contiguous range of vertices and indices forming a submesh,
 * with bone lookup info for skinning and UV scale/offset for texturing.
 */
struct Region {
    u32 index;                           ///< Region index
    u32 unknown;                         ///< Unknown field
    u32 firstVertex;                     ///< First vertex in the vertex buffer
    u32 vertexCount;                     ///< Number of vertices
    u32 firstIndex;                      ///< First index in the index buffer
    u32 indexCount;                      ///< Number of indices (triangles × 3)
    u16 unknown2;                        ///< Unknown field
    u16 firstBoneLookup;                 ///< First entry in bone lookup table
    u16 boneLookupCount;                 ///< Number of bone lookup entries
    u16 padding;                         ///< Alignment padding
    u8 boneWeightPairs;                  ///< Number of bone weight pairs per vertex
    u8 boneIndexPairs;                   ///< Number of bone index pairs per vertex
    u16 rootBone;                        ///< Root bone for this region
    RegionFlag flags = RegionFlag::None; ///< Region flags (hidden, cloth, etc.)
    f32 uvScale;                         ///< UV coordinate scale factor
    f32 uvOffset;                        ///< UV coordinate offset
    M3_DEFINE_VERSION_ACCESSORS()
};

/**
 * @brief BAT_ — Batch / draw call (v0–v1, 14 bytes)
 *
 * Associates a Region with a material for rendering. Multiple batches
 * may reference the same region with different materials.
 */
struct Batch {
    u32 unknown;       ///< Unknown field
    u16 regionIndex;   ///< Index into REGN array
    u32 unknown2;      ///< Unknown field
    u16 materialIndex; ///< Index into MATM material map array
    u16 boneCount;     ///< Bone whose animated visibility gates this batch's draw
                       ///< (0xFFFF = always drawn). Misnamed — it is a bone index,
                       ///< not a count: the engine's submit loop reads it and skips
                       ///< the batch when that bone is invisible.
    M3_DEFINE_VERSION_ACCESSORS()
};

/**
 * @brief MSEC — Mesh section bounds (v0–v1, 80 bytes)
 *
 * Per-node animated bounding extent used for culling and LOD.
 */
struct MeshSection {
    u32 nodeIndex;          ///< Index into BONE array
    AnimRef<Extent> bounds; ///< Animated bounding volume (76 bytes)
    M3_DEFINE_VERSION_ACCESSORS()
};

/**
 * @brief DIV_ — Mesh division (v0–v2, 52 bytes)
 *
 * Top-level mesh container grouping face indices, regions, batches,
 * and mesh sections. Most models have a single division.
 */
struct MeshDivision {
    std::vector<u16> faces;        ///< Triangle indices (U16_)
    std::vector<Region> regions;   ///< Regions / submeshes (REGN)
    std::vector<Batch> batches;    ///< Draw call batches (BAT_)
    std::vector<MeshSection> msec; ///< Per-node mesh section bounds (MSEC)
    u32 instances;                 ///< Instance count
    M3_DEFINE_VERSION_ACCESSORS()
};

/**
 * @brief IREF — Initial reference / inverse bind-pose (v0, 64 bytes)
 *
 * Stores the 4×4 inverse bind-pose matrix for a bone, used to transform
 * vertices from model space into bone-local space for skinning.
 */
struct InitialReference {
    Matrix44f matrix; ///< 4×4 inverse bind-pose matrix
    M3_DEFINE_VERSION_ACCESSORS()
};

/**
 * @brief ATT_ — Attachment point (v0–v1, 20 bytes)
 *
 * Named bone location used by the engine to attach effects, weapons,
 * or other models to specific skeleton bones.
 */
struct AttachmentPoint {
    u32 unknown;      ///< Unknown field
    std::string name; ///< Attachment point name (Ref<CHAR>)
    u32 boneIndex;    ///< Index into BONE array
    M3_DEFINE_VERSION_ACCESSORS()
};

} // namespace m3
} // namespace whiteout
