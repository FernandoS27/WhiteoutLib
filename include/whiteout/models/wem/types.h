// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file types.h
 * @brief Core type definitions and enums for the WEM (Whiteout Edit Model) format
 *
 * WEM is a format-agnostic intermediate representation for 3D model data.
 * It is a superset of MDX, M2, and M3 mesh and material properties.
 *
 * v1 scope: mesh and material only (static properties, no animation).
 */

#include <cstdint>

#include "../../common_types.h"
#include "../../vector_types.h"

namespace whiteout {
namespace models {
namespace wem {

// ============================================================================
// Extent
// ============================================================================

/**
 * @brief Unified axis-aligned bounding box with bounding sphere
 *
 * Superset of MDX/M2/M3 bounding volumes. All three formats define their own
 * Extent type; WEM uses a single unified definition.
 */
struct Extent {
    Vector3f minimum;     ///< AABB min corner
    Vector3f maximum;     ///< AABB max corner
    f32 sphereRadius = 0; ///< Bounding sphere radius
};

// ============================================================================
// BlendMode
// ============================================================================

/**
 * @brief Rendering blend mode (superset of MDX filterMode, M2 blendingMode, M3 BlendMode)
 */
enum class BlendMode : u32 {
    Opaque,        ///< MDX None(0), M2 Opaque(0), M3 Opaque(0)
    AlphaKey,      ///< M2 AlphaKey(1)
    AlphaBlend,    ///< MDX Blend(2), M2 Alpha(2), M3 AlphaBlend(1)
    Additive,      ///< MDX Additive(3), M2 Add(4), M3 Add(2)
    AdditiveAlpha, ///< MDX AddAlpha(4), M2 NoAlphaAdd(3), M3 AlphaAdd(3)
    Modulate,      ///< MDX Modulate(5), M2 Mod(5), M3 Mod(4)
    Modulate2x,    ///< MDX Modulate2x(6), M2 Mod2x(6), M3 Mod2x(5)
    BlendAdd,      ///< M2 BlendAdd(7)
    Transparent,   ///< MDX Transparent(1)
};

// ============================================================================
// UVMappingMode
// ============================================================================

/**
 * @brief UV coordinate mapping/projection mode (from M3, extended)
 */
enum class UVMappingMode : u32 {
    ExplicitUV0,
    ExplicitUV1,
    ExplicitUV2,
    ExplicitUV3,
    ExplicitUV4,
    ReflectCubicEnvironment, ///< M3 cubic environment map reflection
    SphericalEnvironment,    ///< MDX SphereEnvMap flag, M3
    PlanarLocalX,            ///< M3
    PlanarLocalY,            ///< M3
    PlanarLocalZ,            ///< M3
    PlanarWorldX,            ///< M3
    PlanarWorldY,            ///< M3
    PlanarWorldZ,            ///< M3
    TriPlanarLocal,          ///< M3
    TriPlanarWorld,          ///< M3
};

// ============================================================================
// TextureSlotSemantic
// ============================================================================

/**
 * @brief Named purpose for a texture slot (primarily from M3 StandardMaterial layers)
 */
enum class TextureSlotSemantic : u32 {
    Diffuse,
    Decal,
    Specular,
    Gloss,
    Emissive1,
    Emissive2,
    Environment,
    EnvironmentMask,
    Alpha1,
    Alpha2,
    Normal,
    Height,
    LightMap,
    AmbientOcclusion,
    NormalBlend1Mask,
    NormalBlend2Mask,
    NormalBlend1,
    NormalBlend2,
    Roughness,
    Metalness,
    ORM,
    Custom,
};

// ============================================================================
// MaterialFlags
// ============================================================================

/**
 * @brief Bitmask flags for material rendering properties (superset of all formats)
 */
enum class MaterialFlags : u32 {
    None = 0x00,
    Unlit = 0x01,            ///< MDX Unshaded, M2 Unlit, M3
    TwoSided = 0x02,         ///< MDX, M2, M3
    Unfogged = 0x04,         ///< MDX, M2
    DepthTest = 0x08,        ///< M2
    DepthWrite = 0x10,       ///< M2
    NoDepthTest = 0x20,      ///< MDX
    NoDepthWrite = 0x40,     ///< MDX (NoDepthSet)
    NoAlphaComposite = 0x80, ///< M2 (MoP+)
    SphereEnvMap = 0x100,    ///< MDX
};

inline MaterialFlags operator|(MaterialFlags lhs, MaterialFlags rhs) {
    return static_cast<MaterialFlags>(static_cast<u32>(lhs) | static_cast<u32>(rhs));
}

inline MaterialFlags operator&(MaterialFlags lhs, MaterialFlags rhs) {
    return static_cast<MaterialFlags>(static_cast<u32>(lhs) & static_cast<u32>(rhs));
}

inline MaterialFlags& operator|=(MaterialFlags& lhs, MaterialFlags rhs) {
    lhs = lhs | rhs;
    return lhs;
}

inline MaterialFlags& operator&=(MaterialFlags& lhs, MaterialFlags rhs) {
    lhs = lhs & rhs;
    return lhs;
}

inline MaterialFlags operator~(MaterialFlags flag) {
    return static_cast<MaterialFlags>(~static_cast<u32>(flag));
}

inline bool hasFlag(MaterialFlags flags, MaterialFlags flag) {
    return (static_cast<u32>(flags) & static_cast<u32>(flag)) != 0;
}

// ============================================================================
// SubmeshFlags
// ============================================================================

/**
 * @brief Bitmask flags for submesh properties
 */
enum class SubmeshFlags : u32 {
    None = 0x0,
    Hidden = 0x1,          ///< M3
    ClothSimulated = 0x2,  ///< M3
    ClothInfluenced = 0x4, ///< M3
};

inline SubmeshFlags operator|(SubmeshFlags lhs, SubmeshFlags rhs) {
    return static_cast<SubmeshFlags>(static_cast<u32>(lhs) | static_cast<u32>(rhs));
}

inline SubmeshFlags operator&(SubmeshFlags lhs, SubmeshFlags rhs) {
    return static_cast<SubmeshFlags>(static_cast<u32>(lhs) & static_cast<u32>(rhs));
}

inline bool hasFlag(SubmeshFlags flags, SubmeshFlags flag) {
    return (static_cast<u32>(flags) & static_cast<u32>(flag)) != 0;
}

// ============================================================================
// FresnelMode
// ============================================================================

/**
 * @brief Fresnel effect mode (M3)
 */
enum class FresnelMode : u32 {
    None,
    Standard,
    Inverted,
};

// ============================================================================
// ColorChannelSelect
// ============================================================================

/**
 * @brief Which color channels to sample from a texture (M3)
 */
enum class ColorChannelSelect : u32 {
    RGBA,
    RGB,
    Alpha,
    Red,
    Green,
    Blue,
};

// ============================================================================
// MaterialClass
// ============================================================================

/**
 * @brief Material classification (M3)
 */
enum class MaterialClass : u32 {
    Unit,
    Building,
    Doodad,
    SpecialFX,
};

// ============================================================================
// LayerBlendOp
// ============================================================================

/**
 * @brief Texture layer blending operation (M3)
 */
enum class LayerBlendOp : u32 {
    Mod,
    Mod2x,
    Add,
    Lerp,
    TeamColorEmissiveAdd,
    TeamColorDiffuse,
    OpNone,
};

// ============================================================================
// SpecularMode
// ============================================================================

/**
 * @brief Specular source mode (M3)
 */
enum class SpecularMode : u32 {
    RGB,
    AlphaOnly,
};

// ============================================================================
// MaterialType
// ============================================================================

/**
 * @brief Type discriminator for WEM materials
 *
 * Standard: single-surface material with one set of textures and rendering properties.
 * Composite: references sub-materials by index, serving as the "material combiner."
 */
enum class MaterialType : u32 {
    Standard,  ///< Maps from MDX Layer, M2 Batch, M3 StandardMaterial
    Composite, ///< Maps from MDX multi-layer Material, M3 CompositeMaterial
};

} // namespace wem
} // namespace models
} // namespace whiteout
