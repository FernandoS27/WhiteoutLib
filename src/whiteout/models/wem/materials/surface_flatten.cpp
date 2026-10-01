// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "whiteout/models/wem/materials/surface_flatten.h"

#include <whiteout/textures/pbr_bake.h>

namespace whiteout {
namespace models {
namespace wem {

namespace {

/// Folds a factor-only input (no texture) into a colour factor.
void FoldConstant(Vector4f& factor, const TextureInput& input) {
    factor.x *= input.constant.x;
    factor.y *= input.constant.y;
    factor.z *= input.constant.z;
    factor.w *= input.constant.w * input.weight;
}

bool ReferencesReplaceable(const Document& document, const TextureInput& input) {
    return input.hasTexture() && input.texture < document.textures.size() &&
           document.textures[input.texture].replaceableId != 0;
}

void Bind(FlatSurface& out, SurfaceRole role, const TextureInput& input) {
    out.bindings.push_back(SurfaceBinding{role, input, nullptr});
}

void Drop(FlatSurface& out, const char* what) {
    out.bindings.push_back(SurfaceBinding{SurfaceRole::Dropped, TextureInput{}, what});
}

void FlattenLegacy(const Document& document, const LegacyDeferredBody& body, FlatSurface& out) {
    out.baseColorFactor = body.diffuseFactor;
    out.metallicFactor = 0.0f;
    out.roughnessFactor = InterchangeRoughness(body.specularExponent);
    out.emissiveFactor =
        Vector3f{body.emissiveFactor.x, body.emissiveFactor.y, body.emissiveFactor.z};
    out.hasSpecular = true;
    out.specularFactor =
        Vector3f{body.specularFactor.x, body.specularFactor.y, body.specularFactor.z};
    out.specularExponent = body.specularExponent;
    for (const auto& [slot, input] : body.slots) {
        switch (slot) {
        case LegacySlot::Diffuse:
            if (TextureExportable(document, input)) {
                Bind(out, SurfaceRole::BaseColor, input);
            } else {
                if (ReferencesReplaceable(document, input)) {
                    out.gameComposited = true;
                }
                FoldConstant(out.baseColorFactor, input);
            }
            break;
        case LegacySlot::Normal:
            Bind(out, SurfaceRole::Normal, input);
            break;
        case LegacySlot::Emissive:
            Bind(out, SurfaceRole::Emissive, input);
            break;
        case LegacySlot::AmbientOcclusion:
            Bind(out, SurfaceRole::Occlusion, input);
            break;
        case LegacySlot::Specular:
            Bind(out, SurfaceRole::Specular, input);
            break;
        case LegacySlot::Gloss:
            Bind(out, SurfaceRole::Gloss, input);
            break;
        case LegacySlot::Height:
            Bind(out, SurfaceRole::Height, input);
            break;
        case LegacySlot::Environment:
            Drop(out, "the environment layer");
            break;
        case LegacySlot::Lightmap:
            Drop(out, "the lightmap");
            break;
        case LegacySlot::Detail:
            Drop(out, "the detail layer");
            break;
        case LegacySlot::Count:
            break;
        }
    }
}

void FlattenPbr(const PbrDeferredBody& body, FlatSurface& out) {
    out.baseColorFactor = body.baseColorFactor;
    out.metallicFactor = body.metallicFactor;
    out.roughnessFactor = body.roughnessFactor;
    out.emissiveFactor = body.emissiveFactor;
    for (const auto& [slot, input] : body.slots) {
        switch (slot) {
        case PbrSlot::BaseColor:
            if (input.hasTexture()) {
                Bind(out, SurfaceRole::BaseColor, input);
            } else {
                FoldConstant(out.baseColorFactor, input);
            }
            break;
        case PbrSlot::Normal:
            Bind(out, SurfaceRole::Normal, input);
            break;
        case PbrSlot::Orm:
            Bind(out, SurfaceRole::Orm, input);
            break;
        case PbrSlot::AmbientOcclusion:
            Bind(out, SurfaceRole::Occlusion, input);
            break;
        case PbrSlot::Metallic:
            Bind(out, SurfaceRole::Metallic, input);
            break;
        case PbrSlot::Roughness:
            Bind(out, SurfaceRole::Roughness, input);
            break;
        case PbrSlot::Emissive:
            Bind(out, SurfaceRole::Emissive, input);
            break;
        case PbrSlot::Environment:
            Drop(out, "the environment layer");
            break;
        case PbrSlot::TeamColorMask:
            Drop(out, "the team-colour mask");
            break;
        case PbrSlot::Count:
            break;
        }
    }
}

/// A stack that is not a slot map: each channel's first layer, the rest
/// dropped with a count — honest about how much shading is missing.
void FlattenComposite(const Document& document, const CompositeBody& body, FlatSurface& out) {
    out.baseColorFactor = body.diffuseFactor;
    out.metallicFactor = 0.0f;
    out.roughnessFactor = InterchangeRoughness(body.specularExponent);
    out.emissiveFactor =
        Vector3f{body.emissiveFactor.x, body.emissiveFactor.y, body.emissiveFactor.z};
    out.hasSpecular = true;
    out.specularFactor =
        Vector3f{body.specularFactor.x, body.specularFactor.y, body.specularFactor.z};
    out.specularExponent = body.specularExponent;
    bool haveColor = false;
    bool haveNormal = false;
    bool haveEmissive = false;
    bool haveOcclusion = false;
    bool haveSpecular = false;
    bool haveGloss = false;
    bool sawReplaceable = false;
    for (const CompositeLayer& layer : body.layers) {
        sawReplaceable = sawReplaceable || ReferencesReplaceable(document, layer.input);
        const auto claim = [&](bool& have, SurfaceRole role, bool eligible) {
            if (have || !eligible) {
                return false;
            }
            Bind(out, role, layer.input);
            have = true;
            return true;
        };
        bool taken = false;
        switch (layer.target) {
        case SurfaceChannel::Color:
            taken = claim(haveColor, SurfaceRole::BaseColor,
                          TextureExportable(document, layer.input));
            break;
        case SurfaceChannel::Normal:
            taken = claim(haveNormal, SurfaceRole::Normal, true);
            break;
        case SurfaceChannel::Emissive:
            taken = claim(haveEmissive, SurfaceRole::Emissive, true);
            break;
        case SurfaceChannel::AmbientOcclusion:
            taken = claim(haveOcclusion, SurfaceRole::Occlusion, true);
            break;
        case SurfaceChannel::Specular:
            taken = claim(haveSpecular, SurfaceRole::Specular, layer.input.hasTexture());
            break;
        case SurfaceChannel::Gloss:
            taken = claim(haveGloss, SurfaceRole::Gloss, layer.input.hasTexture());
            break;
        default:
            break;
        }
        if (!taken) {
            Drop(out, "a stacked composite layer");
        }
    }
    if (!haveColor && sawReplaceable) {
        out.gameComposited = true;
    }
}

void FlattenCombiners(const Document& document, const CombinersBody& body, FlatSurface& out) {
    out.baseColorFactor = body.diffuseFactor;
    out.metallicFactor = 0.0f;
    out.roughnessFactor = 0.9f;
    out.emissiveFactor =
        Vector3f{body.emissiveFactor.x, body.emissiveFactor.y, body.emissiveFactor.z};
    // The base-colour stage is the first whose texture actually crosses. WC3
    // body materials put the team-colour plate at stage 0 — no file behind it —
    // with the diffuse texture layered above; a positional stage-0 claim
    // exported those bodies flat white.
    std::size_t baseStage = body.stages.size();
    for (std::size_t stage = 0; stage < body.stages.size(); ++stage) {
        if (TextureExportable(document, body.stages[stage].input)) {
            baseStage = stage;
            break;
        }
    }
    bool sawReplaceable = false;
    for (std::size_t stage = 0; stage < body.stages.size(); ++stage) {
        const CombinerStage& entry = body.stages[stage];
        sawReplaceable = sawReplaceable || ReferencesReplaceable(document, entry.input);
        if (stage == baseStage) {
            Bind(out, SurfaceRole::BaseColor, entry.input);
            continue;
        }
        // A texture-less stage under the base is a plate the base draws over;
        // above it, only a plain modulate by a constant folds — the rest of a
        // combiner chain has no flat form and the count says how much is gone.
        if (!entry.input.hasTexture() &&
            (stage < baseStage || entry.rgb == CombinerOp::Mod ||
             entry.rgb == CombinerOp::Mod2x)) {
            FoldConstant(out.baseColorFactor, entry.input);
            if (entry.rgb == CombinerOp::Mod2x) {
                out.baseColorFactor.x *= 2.0f;
                out.baseColorFactor.y *= 2.0f;
                out.baseColorFactor.z *= 2.0f;
            }
            continue;
        }
        Drop(out, "a combiner stage");
    }
    if (baseStage == body.stages.size() && sawReplaceable) {
        out.gameComposited = true;
    }
}

} // namespace

const SurfaceBinding* FlatSurface::find(SurfaceRole role) const {
    for (const SurfaceBinding& binding : bindings) {
        if (binding.role == role) {
            return &binding;
        }
    }
    return nullptr;
}

bool TextureExportable(const Document& document, const TextureInput& input) {
    return input.hasTexture() && input.texture < document.textures.size() &&
           document.textures[input.texture].replaceableId == 0 &&
           input.mapping == UVMappingMode::ExplicitUV;
}

f32 InterchangeRoughness(f32 specularExponent) {
    // Perceptual roughness, as Reforged's `ggxNDF` and glTF read it: the bake's
    // `sqrt(sqrt(2 / (n + 2)))`, not the lobe-width alpha, which put exponent 20
    // at 0.30 and exported every surface lacquered.
    if (specularExponent <= 0.0f) {
        return 1.0f;
    }
    return textures::pbr::RoughnessFromExponent(specularExponent);
}

FlatSurface FlattenSurface(const Document& document, const Material& material) {
    const CommonMaterial& common = material.Common();
    FlatSurface out;
    out.blend = common.blend;
    out.alphaTestThreshold = common.alphaTestThreshold;
    out.cull = common.cull;
    out.unlit = hasFlag(common.flags, MaterialFlags::Unlit);
    out.invisible = hasFlag(common.flags, MaterialFlags::Invisible);
    out.source = common.kind();

    switch (common.kind()) {
    case MaterialKind::PBRDeferred:
        FlattenPbr(*common.pbr(), out);
        break;
    case MaterialKind::LegacyDeferred:
        FlattenLegacy(document, *common.legacy(), out);
        break;
    case MaterialKind::Composite:
        if (std::optional<LegacyDeferredBody> flattened = Flatten(*common.composite());
            flattened.has_value()) {
            FlattenLegacy(document, *flattened, out);
        } else {
            FlattenComposite(document, *common.composite(), out);
        }
        break;
    case MaterialKind::Combiners:
        FlattenCombiners(document, *common.combiners(), out);
        break;
    case MaterialKind::Count:
        break;
    }
    return out;
}

} // namespace wem
} // namespace models
} // namespace whiteout
