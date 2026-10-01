// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file surface_flatten.h
 * @brief Any `CommonMaterial` as one flat surface — the material half every
 *        interchange exporter shares (FBX_OBJ_DESIGN §5).
 *
 * glTF, FBX and OBJ each want one base colour, one normal map, one emissive map
 * and a handful of factors; the four WEM body kinds hold layers, stages and slot
 * maps. This is the one place that decides which input fills which role and
 * what no interchange format can hold, so the exporters cannot drift apart.
 * Each lowering then decides what its own format keeps: glTF drops the Phong
 * roles, FBX and MTL carry them.
 */

#include <string>
#include <vector>

#include <whiteout/common_types.h>

#include "../document.h"
#include "material.h"

namespace whiteout {
namespace models {
namespace wem {

/// What a flattened texture binding feeds.
enum class SurfaceRole : u8 {
    BaseColor,
    Normal,
    Orm,       ///< Packed occlusion/roughness/metallic (glTF's MR layout plus R).
    Occlusion, ///< Unpacked ambient occlusion.
    Emissive,
    Specular,  ///< Phong specular colour.
    Gloss,     ///< Phong exponent map.
    Height,
    Metallic,  ///< Unpacked.
    Roughness, ///< Unpacked.
    Dropped,   ///< A layer no interchange format holds; `what` names it.
};

struct SurfaceBinding {
    SurfaceRole role = SurfaceRole::Dropped;
    TextureInput input;
    /// For `Dropped`: the layer, as a diagnostic phrase ("the lightmap").
    const char* what = nullptr;
};

struct FlatSurface {
    BlendMode blend = BlendMode::Opaque;
    f32 alphaTestThreshold = 0.0f;
    CullMode cull = CullMode::Back;
    bool unlit = false;
    bool invisible = false;
    /// No colour texture crosses and a colour source was a replaceable slot —
    /// pixels the game composites at run time (team colour, team glow).
    bool gameComposited = false;
    MaterialKind source = MaterialKind::PBRDeferred;

    Vector4f baseColorFactor{1.0f, 1.0f, 1.0f, 1.0f};
    f32 metallicFactor = 1.0f;
    f32 roughnessFactor = 1.0f;
    Vector3f emissiveFactor{0.0f, 0.0f, 0.0f};

    /// The source stated a Phong highlight (Legacy, Composite).
    bool hasSpecular = false;
    Vector3f specularFactor{0.0f, 0.0f, 0.0f};
    f32 specularExponent = 0.0f;

    /// In the order the source states them, so a lowering that creates file
    /// entries as it walks keeps a stable output order.
    std::vector<SurfaceBinding> bindings;

    const SurfaceBinding* find(SurfaceRole role) const;

    /// A composite's emissive factor is a gain on its emissive layer
    /// (`hdrEmissiveMultiplier`), not light of its own: with no emissive map
    /// crossing there is nothing to emit.
    bool emissiveIsGain() const {
        return source == MaterialKind::Composite;
    }
};

/// Whether @p input's texture crosses at all: a real document texture, not a
/// replaceable slot, on explicit UVs.
bool TextureExportable(const Document& document, const TextureInput& input);

/// Specular exponent to a perceptual roughness; exponent 0 (no highlight) is 1.
f32 InterchangeRoughness(f32 specularExponent);

FlatSurface FlattenSurface(const Document& document, const Material& material);

} // namespace wem
} // namespace models
} // namespace whiteout
