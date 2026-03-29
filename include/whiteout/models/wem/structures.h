// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file structures.h
 * @brief Data structures for the WEM (Whiteout Edit Model) intermediate format
 *
 * This file defines all WEM data structures for mesh and material representation.
 * WEM is a format-agnostic superset of MDX, M2, and M3 static mesh/material properties.
 *
 * v1 scope: mesh and material only (static properties, no animation system).
 * Skeleton, animation, attachments, cameras deferred to future phases.
 */

#include <array>
#include <string>
#include <vector>

#include "types.h"

namespace whiteout {
namespace models {
namespace wem {

// ============================================================================
// TextureRef — texture file reference
// ============================================================================

/**
 * @brief Reference to a texture file
 *
 * Stores the file path and format-specific metadata. Models reference
 * textures by index into Model::textures[].
 */
struct TextureRef {
    std::string path;      ///< File path (BLP/DDS/TGA)
    u32 flags = 0;         ///< Wrap flags, etc.
    u32 replaceableId = 0; ///< MDX/M2: 0=normal, 1=team color, 2=team glow, etc.
    u32 type = 0;          ///< M2: 0=filename, 1=skin, 2=object skin, 6=hair, etc.
};

// ============================================================================
// FresnelProperties — per-material or per-layer Fresnel
// ============================================================================

/**
 * @brief Fresnel effect properties
 *
 * Used by M3 materials/layers and MDX Reforged materials.
 */
struct FresnelProperties {
    FresnelMode mode = FresnelMode::None; ///< None/Standard/Inverted (M3)
    f32 exponent = 0;                     ///< Fresnel power (M3)
    f32 min = 0;                          ///< Minimum Fresnel factor (M3)
    f32 max = 0;                          ///< Maximum Fresnel factor (M3)
    Vector3f translation;                 ///< Fresnel translation offset (M3)
    Vector3f mask;                        ///< Channel mask (M3)
    Vector2f rotation;                    ///< Rotation (M3)
    Vector3f color;                       ///< Reforged fresnelColor (MDX)
    f32 opacity = 0;                      ///< Reforged fresnelOpacity (MDX)
    f32 teamColor = 0;                    ///< Reforged fresnelTeamColor (MDX)
};

// ============================================================================
// TextureSlot — one texture layer binding
// ============================================================================

/**
 * @brief A single texture layer binding within a material
 *
 * Superset of MDX Layer, M2 resolved-batch texture, and M3 TextureLayer.
 * All static values only — animated UV/alpha/color properties deferred to
 * a future animation phase.
 */
struct TextureSlot {
    TextureSlotSemantic semantic = TextureSlotSemantic::Custom; ///< Named purpose (M3)
    u32 textureIndex = 0;                                       ///< Index into Model::textures[]
    u32 uvSetIndex = 0; ///< Which UV coordinate set (MDX coordId, M2 texCoordCombo)
    UVMappingMode uvMapping = UVMappingMode::ExplicitUV0; ///< Projection mode (M3)
    bool wrapU = true;                                    ///< Wrap in U direction
    bool wrapV = true;                                    ///< Wrap in V direction
    f32 alpha = 1.0f;                                     ///< Layer-level base opacity (MDX)
    ColorChannelSelect colorChannelSelect = ColorChannelSelect::RGBA; ///< Channel sampling (M3)
    f32 rgbMultiply = 1.0f;    ///< Static RGB intensity scale (M3)
    f32 rgbAdd = 0.0f;         ///< Static RGB offset (M3)
    u32 flipbookRows = 0;      ///< Flipbook atlas rows (0=no flipbook) (M3)
    u32 flipbookColumns = 0;   ///< Flipbook atlas columns (M3)
    FresnelProperties fresnel; ///< Per-layer Fresnel (M3)
};

// ============================================================================
// CompositeSection — one sub-material reference within a Composite material
// ============================================================================

/**
 * @brief References a sub-material within a Composite material
 *
 * Used by MDX multi-layer Materials and M3 CompositeMaterials.
 * The referenced material (by index) must be a Standard material.
 */
struct CompositeSection {
    u32 materialIndex = 0;                   ///< Index into Model::materials[] (must be Standard)
    f32 blendWeight = 1.0f;                  ///< Static blend multiplier (M3 CompositeSection)
    BlendMode blendMode = BlendMode::Opaque; ///< Per-section blend (MDX Layer filterMode)
};

// ============================================================================
// Material — type-tagged rendering material (Standard or Composite)
// ============================================================================

/**
 * @brief Rendering material definition
 *
 * Uses type enum + optional blocks. Standard fields are populated when
 * type == Standard, composite fields when type == Composite.
 * Shared fields (name, priorityPlane) apply to all types.
 *
 * All static values only — animated material properties deferred to a future phase.
 */
struct Material {
    // -- Shared fields (all types) --
    MaterialType type = MaterialType::Standard; ///< Discriminator
    std::string name;                           ///< Material name (M3)
    i32 priorityPlane = 0;                      ///< Render sort order

    // -- Standard-only fields (populated when type == Standard) --
    BlendMode blendMode = BlendMode::Opaque;           ///< Rendering blend mode
    MaterialFlags flags = MaterialFlags::None;         ///< Superset bitmask
    std::string shader;                                ///< Reforged shader name (MDX)
    u16 shaderId = 0;                                  ///< Vertex/pixel shader combo selector (M2)
    std::vector<TextureSlot> textureSlots;             ///< All texture bindings
    f32 specularExponent = 0;                          ///< Phong specular power (M3)
    f32 alphaTestThreshold = 0;                        ///< Alpha test cutoff [0..255] (M3)
    f32 depthBlendFalloff = 0;                         ///< Depth blend distance (M3)
    f32 emissiveGain = 0;                              ///< Reforged global emissive gain (MDX)
    f32 hdrSpecularMultiplier = 1.0f;                  ///< HDR specular scale (M3)
    f32 hdrEmissiveMultiplier = 1.0f;                  ///< HDR emissive scale (M3)
    f32 hdrEnvironmentConstant = 0;                    ///< HDR env constant (M3 v20+)
    f32 hdrEnvironmentDiffuse = 1.0f;                  ///< HDR env diffuse (M3)
    f32 hdrEnvironmentSpecular = 1.0f;                 ///< HDR env specular (M3)
    FresnelProperties fresnel;                         ///< Material-level Fresnel (MDX, M3)
    MaterialClass materialClass = MaterialClass::Unit; ///< M3 classification
    LayerBlendOp layerBlendMode = LayerBlendOp::Mod;   ///< Main layer blend op (M3)
    LayerBlendOp emissiveBlendMode1 = LayerBlendOp::Mod; ///< Emissive1 blend op (M3)
    LayerBlendOp emissiveBlendMode2 = LayerBlendOp::Mod; ///< Emissive2 blend op (M3)
    SpecularMode specularMode = SpecularMode::RGB;       ///< Spec from RGB or alpha (M3)

    // -- Composite-only fields (populated when type == Composite) --
    std::vector<CompositeSection> sections; ///< Ordered list of sub-material references
};

// ============================================================================
// Submesh — logical section within a Mesh
// ============================================================================

/**
 * @brief A logical draw section within a mesh
 *
 * Superset of MDX geoset face groups, M2 SkinSection+Batch, M3 Region+Batch.
 */
struct Submesh {
    std::string name;                        ///< MDX lodName or generated name
    u32 indexStart = 0;                      ///< Start in parent Mesh::indices
    u32 indexCount = 0;                      ///< Triangle index count
    u32 vertexStart = 0;                     ///< Start in vertex arrays (M2, M3)
    u32 vertexCount = 0;                     ///< Vertex count in this submesh (M2, M3)
    u32 materialIndex = 0;                   ///< Index into Model::materials[]
    u16 selectionGroup = 0;                  ///< Geoset toggle group (MDX, M2)
    u16 selectionFlags = 0;                  ///< MDX-specific selection flags
    Vector3f centerPosition;                 ///< Submesh center for culling (M2)
    Vector3f sortCenterPosition;             ///< Center for draw-order sorting (M2)
    f32 sortRadius = 0;                      ///< LOD/sort radius (M2)
    u16 centerBoneIndex = 0;                 ///< Bone for distance calculation (M2)
    u16 maxBoneInfluences = 4;               ///< Max bones per vertex (M2, M3)
    u16 rootBone = 0;                        ///< Root bone for this region (M3)
    Extent bounds;                           ///< Submesh bounding volume
    SubmeshFlags flags = SubmeshFlags::None; ///< Hidden, ClothSimulated, etc. (M3)
};

// ============================================================================
// Mesh — self-contained geometry
// ============================================================================

/**
 * @brief A self-contained mesh (geometry + submeshes)
 *
 * Superset of MDX Geoset, M2 global vertices+SkinProfile, M3 VertexBuffer+MeshDivision.
 * Uses struct-of-arrays (SoA) layout for vertex data — format writers produce
 * their own interleaved layouts as needed.
 */
struct Mesh {
    std::string name;                            ///< Human-readable name
    u32 lodLevel = 0;                            ///< 0=highest detail
    std::vector<Vector3f> positions;             ///< Vertex positions
    std::vector<Vector3f> normals;               ///< Vertex normals
    std::vector<Vector4f> tangents;              ///< Tangent vectors (w=bitangent sign)
    std::vector<std::array<u8, 4>> boneIndices;  ///< Per-vertex bone indices (4 max)
    std::vector<std::array<u8, 4>> boneWeights;  ///< Per-vertex bone weights (sum=255)
    std::vector<std::vector<Vector2f>> uvSets;   ///< [uvSetIndex][vertexIndex]
    std::vector<std::array<u8, 4>> vertexColors; ///< Per-vertex RGBA color (M3)
    std::vector<u32> indices;                    ///< Triangle index list (u32 for >65K verts)
    std::vector<Submesh> submeshes;              ///< Logical draw sections
    Extent bounds;                               ///< Overall mesh bounding volume
};

// ============================================================================
// Model — root container
// ============================================================================

/**
 * @brief Root container for a WEM model
 *
 * Holds all mesh geometry, materials, and texture references.
 * Meshes reference materials by index; materials reference textures by index.
 */
struct Model {
    std::string name;                 ///< Model name
    Extent bounds;                    ///< Overall bounding volume
    std::vector<Mesh> meshes;         ///< Geometry (one per geoset/skin/division)
    std::vector<Material> materials;  ///< Rendering materials
    std::vector<TextureRef> textures; ///< Texture file references
};

} // namespace wem
} // namespace models
} // namespace whiteout
