// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "m3_core.h"

#include <whiteout/models/m3/engine_compat.h>

#include "../native/m3_copy.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace whiteout {
namespace models {
namespace wem {
namespace m3_core {

namespace {

std::string number(u64 value) {
    return std::to_string(value);
}

// ── layer -> channel ────────────────────────────────────────────────────────

// Which named `StandardMaterial` layer lands on which channel is decided in
// `importStandard`, in declaration order. `Gloss`, `Height`, `Lightmap` and the
// normal-blend pair have no `SurfaceChannel` at all — that asymmetry is why
// `Flatten` (§7.2.3) is one-directional — so they are reported as dropped and
// stay in the native block, which is what the block is for. The two alpha
// masks are `Coverage`: they are what the engine blends and alpha-tests by.

/// A shipped `.m3` string carries its terminator inside the `std::string` -- the
/// `Reference` count includes it -- so an unused layer's path is one NUL byte
/// and not the empty string.
std::string TrimNuls(const std::string& path) {
    std::string trimmed = path;
    while (!trimmed.empty() && trimmed.back() == '\0') {
        trimmed.pop_back();
    }
    return trimmed;
}

/// Whether @p layer contributes anything at all.
///
/// A shipped `.m3` carries a `LAYR` record for **every** slot of a standard
/// material, used or not, so "the optional is engaged" says nothing. The
/// engine's own test is the one used here: flag `Color` (0x400) makes a layer a
/// solid colour and therefore active with no texture; otherwise it needs a path.
/// Without it a material picks up ten layers that sample nothing and modulate
/// the stack by an unused constant, and the loss report claims seven dropped
/// layers per material that were never there.
bool LayerActive(const m3::TextureLayer& layer) {
    if (hasFlag(layer.flags, m3::TextureLayerFlag::Color)) {
        return true;
    }
    return !TrimNuls(layer.texturePath).empty();
}

CompositeOp opFor(m3::LayerBlendOp blend) {
    switch (blend) {
    case m3::LayerBlendOp::Mod:
        return CompositeOp::Modulate;
    case m3::LayerBlendOp::Mod2x:
        return CompositeOp::Modulate2x;
    // M3's `Add` weights by the layer's alpha and `AddNoAlpha` does not
    // (`CombineLayerColor`: `base + layer.rgb * layer.a` against
    // `base + layer.rgb`), which is exactly the difference between WEM's two.
    // Folding both onto `Add` drew a decal's white texels at full strength the
    // moment one was opened as Warcraft III, where `Additive` does not consult
    // alpha either.
    case m3::LayerBlendOp::Add:
        return CompositeOp::AddAlpha;
    case m3::LayerBlendOp::AddNoAlpha:
        return CompositeOp::Add;
    // Neither team op reaches the layer's own rgb — they add the team colour
    // scaled by its alpha — so no composite op says what they do. The
    // alpha-weighted add is the nearer of the two.
    case m3::LayerBlendOp::TeamColorEmissiveAdd:
    case m3::LayerBlendOp::TeamColorDiffuseAdd:
        return CompositeOp::AddAlpha;
    case m3::LayerBlendOp::Lerp:
        return CompositeOp::AlphaBlend;
    default:
        return CompositeOp::Modulate;
    }
}

m3::LayerBlendOp blendOpFor(CompositeOp op) {
    switch (op) {
    case CompositeOp::Modulate2x:
        return m3::LayerBlendOp::Mod2x;
    case CompositeOp::AddAlpha:
        return m3::LayerBlendOp::Add;
    case CompositeOp::Add:
        return m3::LayerBlendOp::AddNoAlpha;
    case CompositeOp::AlphaBlend:
        return m3::LayerBlendOp::Lerp;
    default:
        return m3::LayerBlendOp::Mod;
    }
}

BlendMode blendFor(m3::BlendMode mode) {
    switch (mode) {
    case m3::BlendMode::Opaque:
        return BlendMode::Opaque;
    case m3::BlendMode::AlphaBlend:
        return BlendMode::AlphaBlend;
    case m3::BlendMode::Add:
        return BlendMode::Additive;
    case m3::BlendMode::AlphaAdd:
        return BlendMode::AdditiveAlpha;
    case m3::BlendMode::Mod:
        return BlendMode::Modulate;
    case m3::BlendMode::Mod2x:
        return BlendMode::Modulate2x;
    default:
        return BlendMode::Opaque;
    }
}

m3::BlendMode blendFor(BlendMode mode) {
    switch (mode) {
    // An alpha KEY is `Opaque` plus the threshold in M3 — the test is a render
    // state there, not a blend — so the threshold field carries it back and
    // the blend stays opaque. `Transparent` (WC3's key) says the same thing.
    case BlendMode::AlphaKey:
    case BlendMode::Transparent:
        return m3::BlendMode::Opaque;
    case BlendMode::AlphaBlend:
        return m3::BlendMode::AlphaBlend;
    case BlendMode::Additive:
    case BlendMode::BlendAdd:
    case BlendMode::PremultipliedAlpha:
        return m3::BlendMode::Add;
    case BlendMode::AdditiveAlpha:
        return m3::BlendMode::AlphaAdd;
    case BlendMode::Modulate:
        return m3::BlendMode::Mod;
    case BlendMode::Modulate2x:
        return m3::BlendMode::Mod2x;
    default:
        return m3::BlendMode::Opaque;
    }
}

/// The projection family, and the UV *set*, are one field on disk and two here.
void applyMapping(m3::UVMappingMode mode, TextureInput& input) {
    switch (mode) {
    case m3::UVMappingMode::ExplicitUV0:
        input.uvSet = 0;
        break;
    case m3::UVMappingMode::ExplicitUV1:
        input.uvSet = 1;
        break;
    case m3::UVMappingMode::ExplicitUV2:
        input.uvSet = 2;
        break;
    case m3::UVMappingMode::ExplicitUV3:
        input.uvSet = 3;
        break;
    case m3::UVMappingMode::ReflectCubicEnvio:
    case m3::UVMappingMode::CubicEnvio:
        input.mapping = UVMappingMode::EnvCube;
        break;
    case m3::UVMappingMode::ReflectSphericalEnvio:
    case m3::UVMappingMode::SphericalEnvio:
        input.mapping = UVMappingMode::EnvSphere;
        break;
    default:
        // The planar and flipbook families have no `UVMappingMode` and no
        // channel meaning; the native layer keeps them.
        break;
    }
}

/// A restored MADD layer wraps, whatever the restore says.
///
/// The record only names an address mode inside a per-layer UV transform and
/// most layers have none, so 5778 of 7015 restored layers (82%) come back with
/// no wrap bit at all. Clamp is not what that silence means: shipped
/// fixed-function content wraps on 14464 of 14534 StarCraft II layers and 27664
/// of 28267 Heroes ones, and a Murky's body tiles to u ~ 2. Read literally, half
/// of him is a grey smear down the u = 1 seam -- which is the same correction
/// the renderer's own adapter makes, for the same reason.
void MaddLayersWrap(m3::StandardMaterial& material) {
    using Slot = std::optional<m3::TextureLayer> m3::StandardMaterial::*;
    static constexpr Slot kSlots[] = {
        &m3::StandardMaterial::diffuseLayer,          &m3::StandardMaterial::decalLayer,
        &m3::StandardMaterial::specularLayer,         &m3::StandardMaterial::glossLayer,
        &m3::StandardMaterial::emissiveLayer1,        &m3::StandardMaterial::emissiveLayer2,
        &m3::StandardMaterial::environmentLayer,      &m3::StandardMaterial::environmentMaskLayer,
        &m3::StandardMaterial::alphaLayer1,           &m3::StandardMaterial::alphaLayer2,
        &m3::StandardMaterial::normalLayer,           &m3::StandardMaterial::heightLayer,
        &m3::StandardMaterial::lightMapLayer,         &m3::StandardMaterial::ambientOcclusionLayer,
        &m3::StandardMaterial::normalBlend1MaskLayer, &m3::StandardMaterial::normalBlend2MaskLayer,
        &m3::StandardMaterial::normalBlend1Layer,     &m3::StandardMaterial::normalBlend2Layer,
    };
    for (const Slot slot : kSlots) {
        if (auto& layer = material.*slot; layer.has_value()) {
            layer->flags |= m3::TextureLayerFlag::UVWrapX | m3::TextureLayerFlag::UVWrapY;
        }
    }
}

TextureInput inputFor(const m3::TextureLayer& layer, const Context& context, u32 ordinal,
                      Diagnostics& out) {
    TextureInput input;
    input.texture = context.toDocument(layer.texturePath);
    // An unused `.m3` layer's path is one NUL byte, not the empty string: the
    // `Reference` count includes the terminator, so `std::string::empty()` is
    // false for every one of them. Trimming first is the difference between "no
    // texture here" and 963 unresolved references that were never broken.
    const std::string path = TrimNuls(layer.texturePath);
    if (input.texture == kInvalidIndex && !path.empty()) {
        out.warn(DiagCode::TextureUnresolved,
                 "layer names '" + path + "', which the document has no entry for",
                 ElementRef(ElementKind::Layer, ordinal));
    }
    applyMapping(layer.uvMapping, input);
    // The address mode. StarCraft II states it per LAYER and Warcraft III per
    // TEXTURE, so this is the field the `.mdx` export has to reconstruct a TEXS
    // flag word from -- and a layer read as clamp whose coordinates leave [0,1]
    // samples one edge column across the whole surface.
    input.wrapU = hasFlag(layer.flags, m3::TextureLayerFlag::UVWrapX) ? WrapMode::Repeat
                                                                     : WrapMode::Clamp;
    input.wrapV = hasFlag(layer.flags, m3::TextureLayerFlag::UVWrapY) ? WrapMode::Repeat
                                                                     : WrapMode::Clamp;
    // `color` is an AnimRef; the constant is its initial value, and the
    // animation lives in the channel table (§10.8).
    const m3::ColorBGRA tint = layer.color.initValue;
    input.constant = Vector4f(static_cast<f32>(tint.r) / 255.0f, static_cast<f32>(tint.g) / 255.0f,
                              static_cast<f32>(tint.b) / 255.0f, static_cast<f32>(tint.a) / 255.0f);
    input.uvTransform = UvTransformOf(layer);
    input.weight = layer.mapAlpha.initValue;
    return input;
}

void addFresnel(const m3::TextureLayer& layer, u32 ordinal, CommonMaterial& common) {
    if (layer.fresnelMode == m3::FresnelMode::None) {
        return;
    }
    FresnelFeature fresnel;
    fresnel.exponent = layer.fresnelExponent;
    // min/max are an OUTPUT range, not a clamp on the input: the shipped values
    // routinely run max < min, which only means the ramp is inverted.
    fresnel.outMin = layer.fresnelMin;
    fresnel.outMax = layer.fresnelMax;
    // Inverted runs `1 - term` through the same ramp, which is the Standard
    // ramp with its ends swapped: `(1-t)(max-min)+min == t(min-max)+max`. One
    // spelling in WEM; the export writes Standard.
    if (layer.fresnelMode == m3::FresnelMode::Inverted) {
        std::swap(fresnel.outMin, fresnel.outMax);
    }

    MaterialFeature feature;
    feature.id = NextFeatureId(common.features);
    feature.layer = ordinal;
    feature.payload = fresnel;
    common.features.push_back(feature);
}

/// Appends one optional layer to the stack, if present.
void appendLayer(const std::optional<m3::TextureLayer>& layer, SurfaceChannel channel,
                 CompositeOp op, const Context& context, CompositeBody& body,
                 CommonMaterial& common, Diagnostics& out, StandardLayer slot,
                 std::vector<u32>* ordinals) {
    if (!layer.has_value() || !LayerActive(*layer)) {
        return;
    }
    const u32 ordinal = static_cast<u32>(body.layers.size());
    if (ordinals != nullptr) {
        (*ordinals)[static_cast<std::size_t>(slot)] = ordinal;
    }
    CompositeLayer entry;
    entry.input = inputFor(*layer, context, ordinal, out);
    entry.target = channel;
    entry.op = op;
    body.layers.push_back(std::move(entry));
    addFresnel(*layer, ordinal, common);
}

void reportDropped(const std::optional<m3::TextureLayer>& layer, const char* name, const char* why,
                   Diagnostics& out) {
    if (!layer.has_value() || !LayerActive(*layer)) {
        return;
    }
    out.info(DiagCode::LayerDropped, std::string(name) + " has no surface channel (" + why +
                                         "); it stays in the native block");
}

// ── the standard material ───────────────────────────────────────────────────

void importStandard(const m3::StandardMaterial& source, const Context& context,
                    CommonMaterial& common, Diagnostics& out, std::vector<u32>* ordinals) {
    common.blend = blendFor(source.blendMode);
    common.priorityPlane = source.priority;
    // M3 stores the cut-off as 0..255; `CommonMaterial` normalises, so a
    // consumer never has to know which convention a material came from.
    common.alphaTestThreshold = static_cast<f32>(source.alphaTestThreshold) / 255.0f;
    // StarCraft II spells "alpha test" as an Opaque blend plus a threshold —
    // the test is a render state beside the blend, not a blend mode. WEM's
    // vocabulary names that surface `AlphaKey`, and it is what makes a `.mdx`
    // export choose FilterMode::Transparent: left as Opaque, the exported
    // cutout is never tested and the holes never appear.
    if (common.blend == BlendMode::Opaque && source.alphaTestThreshold > 0) {
        common.blend = BlendMode::AlphaKey;
    }

    if (hasFlag(source.flags, m3::MaterialFlag::TwoSided)) {
        common.cull = CullMode::None;
    }
    if (hasFlag(source.flags, m3::MaterialFlag::Unfogged)) {
        common.flags |= MaterialFlags::Unfogged;
    }
    if (hasFlag(source.flags, m3::MaterialFlag::Unshaded)) {
        common.flags |= MaterialFlags::Unlit;
    }
    if (hasFlag(source.flags, m3::MaterialFlag::NoShadowsCast)) {
        common.flags |= MaterialFlags::NoShadowCast;
    }
    if (hasFlag(source.flags, m3::MaterialFlag::NoShadowsReceive)) {
        common.flags |= MaterialFlags::NoShadowReceive;
    }

    CompositeBody body;
    body.specularExponent = source.specularExponent;
    body.specularFactor = Vector4f(source.hdrSpecularMultiplier, source.hdrSpecularMultiplier,
                                   source.hdrSpecularMultiplier, 1.0f);
    body.emissiveFactor = Vector4f(source.hdrEmissiveMultiplier, source.hdrEmissiveMultiplier,
                                   source.hdrEmissiveMultiplier, 1.0f);
    body.environmentFactor = source.hdrEnvironmentConstant;
    body.simulateRoughness = hasFlag(source.flags, m3::MaterialFlag::SimulateRoughness);

    const CompositeOp layerOp = opFor(source.layerBlendMode);
    appendLayer(source.diffuseLayer, SurfaceChannel::Color, CompositeOp::Set, context, body, common,
                out, StandardLayer::Diffuse, ordinals);
    appendLayer(source.decalLayer, SurfaceChannel::Color, layerOp, context, body, common, out,
                StandardLayer::Decal, ordinals);
    appendLayer(source.specularLayer, SurfaceChannel::Specular, CompositeOp::Set, context, body,
                common, out, StandardLayer::Specular, ordinals);
    appendLayer(source.emissiveLayer1, SurfaceChannel::Emissive, opFor(source.emissiveBlendMode1),
                context, body, common, out, StandardLayer::Emissive1, ordinals);
    appendLayer(source.emissiveLayer2, SurfaceChannel::Emissive, opFor(source.emissiveBlendMode2),
                context, body, common, out, StandardLayer::Emissive2, ordinals);
    appendLayer(source.environmentLayer, SurfaceChannel::Environment, CompositeOp::Set, context,
                body, common, out, StandardLayer::Environment, ordinals);
    appendLayer(source.environmentMaskLayer, SurfaceChannel::Environment, CompositeOp::Modulate,
                context, body, common, out, StandardLayer::EnvironmentMask, ordinals);
    appendLayer(source.normalLayer, SurfaceChannel::Normal, CompositeOp::Set, context, body, common,
                out, StandardLayer::Normal, ordinals);
    appendLayer(source.ambientOcclusionLayer, SurfaceChannel::AmbientOcclusion, CompositeOp::Set,
                context, body, common, out, StandardLayer::AmbientOcclusion, ordinals);
    // The alpha masks ARE the surface's coverage -- `cFinal.a = mask1.a *
    // mask2.a * alphaFactor` (psmaterial.fx:380), the diffuse alpha being the
    // team mask -- and 104,869 of 176,955 shipped materials carry one. Appended
    // last so every ordinal above predates them.
    appendLayer(source.alphaLayer1, SurfaceChannel::Coverage, CompositeOp::Set, context, body,
                common, out, StandardLayer::Alpha1, ordinals);
    appendLayer(source.alphaLayer2, SurfaceChannel::Coverage, CompositeOp::Modulate, context, body,
                common, out, StandardLayer::Alpha2, ordinals);

    appendLayer(source.glossLayer, SurfaceChannel::Gloss, CompositeOp::Set, context, body, common,
                out, StandardLayer::Gloss, ordinals);
    reportDropped(source.heightLayer, "heightLayer", "parallax is not a surface channel", out);
    reportDropped(source.lightMapLayer, "lightMapLayer", "baked light is not a channel", out);
    reportDropped(source.normalBlend1Layer, "normalBlend1Layer",
                  "normal blending has no "
                  "channel",
                  out);
    reportDropped(source.normalBlend2Layer, "normalBlend2Layer",
                  "normal blending has no "
                  "channel",
                  out);

    common.body = std::move(body);
}

/// The nine kinds that are not a layer stack. Their common material is a
/// best-effort composite over whatever layers they do hold, and the diagnostic
/// says so rather than letting a consumer believe the projection is faithful.
void importBestEffort(const char* kindName,
                      std::initializer_list<const std::optional<m3::TextureLayer>*> layers,
                      const Context& context, CommonMaterial& common, Diagnostics& out) {
    CompositeBody body;
    for (const std::optional<m3::TextureLayer>* layer : layers) {
        // No ordinal map: the non-standard kinds have no `StandardLayer` slots
        // to name, so their layers are unaddressable by animation either way.
        appendLayer(*layer, SurfaceChannel::Color,
                    body.layers.empty() ? CompositeOp::Set : CompositeOp::Modulate, context, body,
                    common, out, StandardLayer::Count, nullptr);
    }
    common.body = std::move(body);
    out.warn(DiagCode::LossyKindConversion,
             std::string("an M3 ") + kindName +
                 " material is not a layer stack; the common material is a likeness and the "
                 "truth stays native");
}

/// A CMP_ whose sections are each one pass -- what `ExportMaterial` writes for
/// a Warcraft III stack no single material could carry -- comes back as that
/// stack: one Color layer per section, its op the section's own blend, the
/// header the first section's, and a `LayerShading` feature where a section's
/// flags differ from it. Anything richer (a section with a decal, a second
/// emissive, an environment layer, a diffuse AND an emissive) is not one pass
/// and is refused, so the caller keeps the empty body and the warning.
bool importCompositeSections(const m3::Model& model, const m3::CompositeMaterial& source,
                             const Context& context, CommonMaterial& common, Diagnostics& out) {
    if (source.sections.empty()) {
        return false;
    }
    CompositeBody body;
    for (std::size_t k = 0; k < source.sections.size(); ++k) {
        const u32 mapIndex = source.sections[k].materialIndex;
        if (mapIndex >= model.materialMaps.size()) {
            return false;
        }
        const m3::MaterialMap& entry = model.materialMaps[mapIndex];
        if (entry.materialType != m3::MaterialType::Standard ||
            entry.materialIndex >= model.standardMaterials.size()) {
            return false;
        }
        const m3::StandardMaterial& standard = model.standardMaterials[entry.materialIndex];
        const auto active = [](const std::optional<m3::TextureLayer>& layer) {
            return layer.has_value() && LayerActive(*layer);
        };
        const bool hasDiffuse = active(standard.diffuseLayer);
        const bool hasEmissive = active(standard.emissiveLayer1);
        if (hasDiffuse == hasEmissive || active(standard.decalLayer) ||
            active(standard.emissiveLayer2) || active(standard.environmentLayer)) {
            return false;
        }
        const m3::TextureLayer& layer = hasDiffuse ? *standard.diffuseLayer : *standard.emissiveLayer1;
        CompositeLayer pass;
        pass.input = inputFor(layer, context, static_cast<u32>(k), out);
        pass.target = SurfaceChannel::Color;
        const bool unlit = hasFlag(standard.flags, m3::MaterialFlag::Unshaded);
        const bool twoSided = hasFlag(standard.flags, m3::MaterialFlag::TwoSided);
        const bool unfogged = hasFlag(standard.flags, m3::MaterialFlag::Unfogged);
        if (k == 0) {
            pass.op = CompositeOp::Set;
            common.blend = blendFor(standard.blendMode);
            if (common.blend == BlendMode::Opaque && standard.alphaTestThreshold > 0) {
                common.blend = BlendMode::AlphaKey;
            }
            common.alphaTestThreshold = static_cast<f32>(standard.alphaTestThreshold) / 255.0f;
            common.priorityPlane = standard.priority;
            if (twoSided) {
                common.cull = CullMode::None;
            }
            if (unfogged) {
                common.flags |= MaterialFlags::Unfogged;
            }
            if (unlit) {
                common.flags |= MaterialFlags::Unlit;
            }
            body.specularExponent = standard.specularExponent;
            body.emissiveFactor = Vector4f(standard.hdrEmissiveMultiplier,
                                           standard.hdrEmissiveMultiplier,
                                           standard.hdrEmissiveMultiplier, 1.0f);
        } else {
            switch (standard.blendMode) {
            case m3::BlendMode::AlphaBlend:
                pass.op = CompositeOp::AlphaBlend;
                break;
            case m3::BlendMode::AlphaAdd:
                pass.op = CompositeOp::AddAlpha;
                break;
            case m3::BlendMode::Add:
                pass.op = CompositeOp::Add;
                break;
            case m3::BlendMode::Mod:
                pass.op = CompositeOp::Modulate;
                break;
            case m3::BlendMode::Mod2x:
                pass.op = CompositeOp::Modulate2x;
                break;
            default:
                pass.op = standard.alphaTestThreshold > 0 ? CompositeOp::AlphaKey
                                                          : CompositeOp::Set;
                break;
            }
            if (unlit != hasFlag(common.flags, MaterialFlags::Unlit) ||
                twoSided != (common.cull == CullMode::None) ||
                unfogged != hasFlag(common.flags, MaterialFlags::Unfogged)) {
                LayerShadingFeature shading;
                shading.unlit = unlit;
                shading.twoSided = twoSided;
                shading.unfogged = unfogged;
                MaterialFeature feature;
                feature.id = NextFeatureId(common.features);
                feature.layer = static_cast<u32>(k);
                feature.payload = shading;
                common.features.push_back(feature);
            }
        }
        body.layers.push_back(std::move(pass));
    }
    common.body = std::move(body);
    out.info(DiagCode::LossyKindConversion,
             "an M3 CMP_ of " + number(source.sections.size()) +
                 " single-pass sections reads as a layer stack of as many passes");
    return true;
}

} // namespace

// ============================================================================
// Context
// ============================================================================

u32 Context::toDocument(const std::string& path) const {
    const std::string trimmed = TrimNuls(path);
    if (trimmed.empty()) {
        return kInvalidIndex;
    }
    for (const auto& [name, index] : texturesByPath) {
        if (name == trimmed) {
            return index;
        }
    }
    if (!internUnknownPaths) {
        return kInvalidIndex;
    }
    const u32 index = static_cast<u32>(texturesByPath.size());
    texturesByPath.emplace_back(trimmed, index);
    return index;
}

std::string Context::toPath(u32 documentTextureId) const {
    for (const auto& [name, index] : texturesByPath) {
        if (index == documentTextureId) {
            return name;
        }
    }
    return {};
}

// ============================================================================
// Import
// ============================================================================

const std::optional<m3::TextureLayer>& LayerOf(const m3::StandardMaterial& material,
                                               StandardLayer slot) {
    switch (slot) {
    case StandardLayer::Diffuse:
        return material.diffuseLayer;
    case StandardLayer::Decal:
        return material.decalLayer;
    case StandardLayer::Specular:
        return material.specularLayer;
    case StandardLayer::Emissive1:
        return material.emissiveLayer1;
    case StandardLayer::Emissive2:
        return material.emissiveLayer2;
    case StandardLayer::Environment:
        return material.environmentLayer;
    case StandardLayer::EnvironmentMask:
        return material.environmentMaskLayer;
    case StandardLayer::Normal:
        return material.normalLayer;
    case StandardLayer::Alpha1:
        return material.alphaLayer1;
    case StandardLayer::Alpha2:
        return material.alphaLayer2;
    case StandardLayer::Gloss:
        return material.glossLayer;
    case StandardLayer::AmbientOcclusion:
    case StandardLayer::Count:
        break;
    }
    return material.ambientOcclusionLayer;
}

std::optional<m3::TextureLayer>& MutableLayerOf(m3::StandardMaterial& material,
                                                StandardLayer slot) {
    // The one authority for which member a slot names, reached the const way
    // and cast back: writing a second switch is how the two drift.
    return const_cast<std::optional<m3::TextureLayer>&>(
        LayerOf(const_cast<const m3::StandardMaterial&>(material), slot));
}

Matrix3x2f UvTransformOf(const m3::TextureLayer& layer) {
    const f32 angle = layer.uvAngle.initValue.z;
    const f32 c = std::cos(angle);
    const f32 s = std::sin(angle);
    // A tiling of zero is silence, not a scale of zero: 965509 of Heroes'
    // 1039144 shipped layers tile at exactly one and 48032 leave the pair at
    // the struct's zero, which composed literally samples one texel over the
    // whole surface (`reference_m3_uv_and_fresnel`).
    const f32 tx = layer.uvTiling.initValue.x != 0.0f ? layer.uvTiling.initValue.x : 1.0f;
    const f32 ty = layer.uvTiling.initValue.y != 0.0f ? layer.uvTiling.initValue.y : 1.0f;

    Matrix3x2f matrix;
    matrix.m[0][0] = tx * c;
    matrix.m[0][1] = -tx * s;
    matrix.m[1][0] = ty * s;
    matrix.m[1][1] = ty * c;
    // The pivot and the offset, which the engine applies together: the offset
    // sits beside the 0.5 INSIDE the pre-multiply, so it is subtracted.
    const f32 px = 0.5f + layer.uvOffset.initValue.x;
    const f32 py = 0.5f + layer.uvOffset.initValue.y;
    matrix.m[0][2] = 0.5f - (matrix.m[0][0] * px + matrix.m[0][1] * py);
    matrix.m[1][2] = 0.5f - (matrix.m[1][0] * px + matrix.m[1][1] * py);
    return matrix;
}

void SetUvTransform(const Matrix3x2f& matrix, m3::TextureLayer& layer) {
    // Row 0 is `tiling.x * (cos, -sin)`, so it names the angle; both tilings
    // are then the projection of their row onto that angle, which is exact for
    // a true `tiling * rotation` and the closest scale-and-turn for anything
    // else.
    const f32 angle = std::atan2(-matrix.m[0][1], matrix.m[0][0]);
    const f32 c = std::cos(angle);
    const f32 s = std::sin(angle);
    const f32 tx = matrix.m[0][0] * c - matrix.m[0][1] * s;
    const f32 ty = matrix.m[1][0] * s + matrix.m[1][1] * c;

    layer.uvAngle.initValue = Vector3f(0.0f, 0.0f, angle);
    layer.uvTiling.initValue = Vector2f(tx, ty);

    // `matrix.translation = 0.5 - tiling * R * (0.5 + offset)`, read backwards.
    const f32 vx = 0.5f - matrix.m[0][2];
    const f32 vy = 0.5f - matrix.m[1][2];
    const f32 wx = tx != 0.0f ? vx / tx : 0.0f;
    const f32 wy = ty != 0.0f ? vy / ty : 0.0f;
    layer.uvOffset.initValue = Vector2f(c * wx + s * wy - 0.5f, -s * wx + c * wy - 0.5f);
}

Material ImportMaterial(const m3::Model& model, const m3::MaterialMap& entry, ProfileId profile,
                        const Context& context, Diagnostics& out, std::vector<u32>* layerOrdinals) {
    Material result;
    CommonMaterial& common = result.InitCommon();
    if (layerOrdinals != nullptr) {
        layerOrdinals->assign(static_cast<std::size_t>(StandardLayer::Count), kInvalidIndex);
    }

    native::M3Material block;
    block.sourceVersion = context.modelVersion;
    block.kind = static_cast<native::M3MaterialKind>(entry.materialType);

    const std::size_t index = entry.materialIndex;
    const auto outOfRange = [&](std::size_t count) {
        out.error(DiagCode::IndexOutOfRange,
                  std::string("material map names ") +
                      ToString(static_cast<native::M3MaterialKind>(entry.materialType)) +
                      " index " + number(index) + " of " + number(count),
                  ElementRef(ElementKind::Material, static_cast<u32>(index)), profile);
    };

    switch (entry.materialType) {
    case m3::MaterialType::Standard: {
        if (index >= model.standardMaterials.size()) {
            outOfRange(model.standardMaterials.size());
            break;
        }
        const m3::StandardMaterial& source = model.standardMaterials[index];
        result.name = source.name;
        importStandard(source, context, common, out, layerOrdinals);
        native::M3Standard mirror;
        CopyToNative(source, mirror);
        block.body = std::move(mirror);
        break;
    }
    case m3::MaterialType::Displacement: {
        if (index >= model.displacementMaterials.size()) {
            outOfRange(model.displacementMaterials.size());
            break;
        }
        const m3::DisplacementMaterial& source = model.displacementMaterials[index];
        result.name = source.name;
        importBestEffort("displacement", {&source.normalMap, &source.strengthMap}, context, common,
                         out);
        native::M3Displacement mirror;
        CopyToNative(source, mirror);
        block.body = std::move(mirror);
        break;
    }
    case m3::MaterialType::Composite: {
        if (index >= model.compositeMaterials.size()) {
            outOfRange(model.compositeMaterials.size());
            break;
        }
        const m3::CompositeMaterial& source = model.compositeMaterials[index];
        result.name = source.name;
        // A weighted blend of references to whole *other materials* — the name
        // collides with WEM's `Composite` kind and means something else
        // entirely (§7.2.1). One shape of it IS a layer stack, drawn a section
        // per pass (`reference_m3_composite_is_multipass`): the one the export
        // writes for a Warcraft III stack, which reads back as that stack.
        // Anything else has nothing to project: the sections name materials,
        // not textures.
        if (!importCompositeSections(model, source, context, common, out)) {
            common.body = CompositeBody{};
            out.warn(DiagCode::LossyKindConversion,
                     "an M3 CMP_ material blends " + number(source.sections.size()) +
                         " other materials by weight; that is not a layer stack and has no "
                         "common projection");
        }
        native::M3Composite mirror;
        CopyToNative(source, mirror);
        block.body = std::move(mirror);
        break;
    }
    case m3::MaterialType::Terrain: {
        if (index >= model.terrainMaterials.size()) {
            outOfRange(model.terrainMaterials.size());
            break;
        }
        const m3::TerrainMaterial& source = model.terrainMaterials[index];
        result.name = source.name;
        importBestEffort("terrain", {&source.terrainMap}, context, common, out);
        native::M3Terrain mirror;
        CopyToNative(source, mirror);
        block.body = std::move(mirror);
        break;
    }
    case m3::MaterialType::Volume: {
        if (index >= model.volumeMaterials.size()) {
            outOfRange(model.volumeMaterials.size());
            break;
        }
        const m3::VolumeMaterial& source = model.volumeMaterials[index];
        result.name = source.name;
        importBestEffort("volume", {&source.colorMap, &source.noiseMap1, &source.noiseMap2},
                         context, common, out);
        native::M3Volume mirror;
        CopyToNative(source, mirror);
        block.body = std::move(mirror);
        break;
    }
    case m3::MaterialType::VolumeNoise: {
        if (index >= model.volumeNoiseMaterials.size()) {
            outOfRange(model.volumeNoiseMaterials.size());
            break;
        }
        const m3::VolumeNoiseMaterial& source = model.volumeNoiseMaterials[index];
        result.name = source.name;
        importBestEffort("volume-noise", {&source.colorMap, &source.noiseMap1, &source.noiseMap2},
                         context, common, out);
        native::M3VolumeNoise mirror;
        CopyToNative(source, mirror);
        block.body = std::move(mirror);
        break;
    }
    case m3::MaterialType::Creep: {
        if (index >= model.creepMaterials.size()) {
            outOfRange(model.creepMaterials.size());
            break;
        }
        const m3::CreepMaterial& source = model.creepMaterials[index];
        result.name = source.name;
        importBestEffort("creep", {&source.maskMap}, context, common, out);
        native::M3Creep mirror;
        CopyToNative(source, mirror);
        block.body = std::move(mirror);
        break;
    }
    case m3::MaterialType::SplatTerrainBake: {
        if (index >= model.stbMaterials.size()) {
            outOfRange(model.stbMaterials.size());
            break;
        }
        const m3::STBMaterial& source = model.stbMaterials[index];
        result.name = source.name;
        importBestEffort("splat-terrain-bake",
                         {&source.diffuseMap, &source.normalMap, &source.specularMap}, context,
                         common, out);
        native::M3Stb mirror;
        CopyToNative(source, mirror);
        block.body = std::move(mirror);
        break;
    }
    case m3::MaterialType::Reflection: {
        if (index >= model.reflectionMaterials.size()) {
            outOfRange(model.reflectionMaterials.size());
            break;
        }
        const m3::ReflectionMaterial& source = model.reflectionMaterials[index];
        result.name = source.name;
        importBestEffort("reflection",
                         {&source.reflectionMap, &source.displacementMap, &source.blurMap}, context,
                         common, out);
        native::M3Reflection mirror;
        CopyToNative(source, mirror);
        block.body = std::move(mirror);
        break;
    }
    case m3::MaterialType::LensFlare: {
        if (index >= model.lensFlareMaterials.size()) {
            outOfRange(model.lensFlareMaterials.size());
            break;
        }
        const m3::LensFlare& source = model.lensFlareMaterials[index];
        result.name = source.name;
        importBestEffort("lens-flare", {&source.flareMap, &source.maskMap}, context, common, out);
        native::M3LensFlare mirror;
        CopyToNative(source, mirror);
        block.body = std::move(mirror);
        break;
    }
    case m3::MaterialType::DataDriven: {
        if (index >= model.dataDrivenMaterials.size()) {
            outOfRange(model.dataDrivenMaterials.size());
            break;
        }
        const m3::DataDrivenMaterial& source = model.dataDrivenMaterials[index];
        result.name = source.materialName;
        block.authoredDataDriven = true;

        native::M3DataDriven mirror;
        CopyToNative(source, mirror);
        block.body = std::move(mirror);

        // The authored blob, decoded once so nobody has to re-run it, and so a
        // property whose name hash we cannot resolve still round-trips.
        native::M3DataDrivenProperties properties;
        CopyToNative(source.decodeProperties(), properties);
        block.dataDrivenProperties = std::move(properties);

        // The restore is the load-time truth: this is what the renderer builds
        // and draws. It is lossy in the forward direction, which is why the
        // blob above is kept beside it rather than instead of it.
        m3::StandardMaterialConversion restored = source.toStandardMaterial();
        if (!restored.converted && context.approximateShaderGraphs) {
            restored = source.approximateStandardMaterial();
        }
        if (restored.converted) {
            MaddLayersWrap(restored.material);
            importStandard(restored.material, context, common, out, layerOrdinals);
            native::M3Standard standard;
            CopyToNative(restored.material, standard);
            block.restoredStandard = std::move(standard);
            for (const std::string& loss : restored.lossy) {
                out.info(DiagCode::LossyKindConversion, "MADD restore: " + loss);
            }
        } else {
            common.body = CompositeBody{};
            out.warn(DiagCode::LossyKindConversion,
                     "MADD has no StandardMaterial form (" + restored.blocker +
                         "); the common material is empty and the blob stays native");
        }
        break;
    }
    default:
        // `Hair` lands here, and so would a type a future MODL version adds.
        // HAI_ is defunct — the parser header says "always null" and the corpus
        // agrees — so this diagnoses rather than parsing.
        out.warn(DiagCode::UnsupportedMaterialKind,
                 std::string("material type ") +
                     ToString(static_cast<native::M3MaterialKind>(entry.materialType)) +
                     " has no WEM body",
                 ElementRef(ElementKind::Material, static_cast<u32>(index)), profile);
        break;
    }

    result.SetNativeInSync(std::move(block));
    return result;
}

std::vector<Material> ImportMaterials(const m3::Model& model, ProfileId profile,
                                      const Context& context, Diagnostics& out) {
    std::vector<Material> materials;
    materials.reserve(model.materialMaps.size());
    for (const m3::MaterialMap& entry : model.materialMaps) {
        materials.push_back(ImportMaterial(model, entry, profile, context, out));
    }
    if (!model.hairMaterials.empty()) {
        out.warn(DiagCode::UnsupportedMaterialKind,
                 "the model carries " + number(model.hairMaterials.size()) +
                     " HAI_ materials; the chunk is defunct and WEM has no body for it");
    }
    return materials;
}

// ============================================================================
// Export
// ============================================================================

namespace {

/// Writes @p standard into @p model and returns the map entry naming it.
///
/// `MAT_` is one of the four chunks the two engines disagree about, and the one
/// a conversion actually creates: StarCraft II reads v20, Heroes caps at v19,
/// and the three versions between them are the HDR environment multipliers this
/// export fills. So the material states its own version — the writer's default
/// is the version *both* clients read, which would drop those from a StarCraft
/// II file too, and the renderer reads it back off the unwritten model.
m3::MaterialMap pushStandard(m3::StandardMaterial standard, m3::Model& model, ProfileId profile,
                             Diagnostics& out) {
    standard.setVersion(profile == ProfileId::Heroes ? m3::HOTS_MAX_STANDARD_MATERIAL_VERSION
                                                     : m3::SC2_MAX_STANDARD_MATERIAL_VERSION);
    // From v19 a normal blend switches on factors the editor's draw reads without
    // looking: 0-3 for the first, 4-7 for the second. The flag with no factors
    // dereferences an empty array. All 20 shipped v20 materials that set it carry
    // four. A record restored from a pre-v19 source still has that version's 0x4
    // (17,012 of 59,704 v15-v18 materials), which Blizzard's loader clears when it
    // upgrades one; the native mirror keeps no version, so the factors decide.
    if (standard.normalBlendFactors.size() < 4) {
        standard.flags &= ~m3::MaterialFlag::NormalBlend;
    }
    if (standard.normalBlendFactors.size() < 8) {
        standard.flags &= ~m3::MaterialFlag::NormalBlend2;
    }
    if (profile == ProfileId::Heroes && standard.hdrEnvironmentConstant > 0.0f) {
        // Heroes keeps these as properties of a MADD blob instead, and this
        // export writes typed materials. §18.2.
        out.warn(DiagCode::FeatureDropped,
                 "MAT_ v19 has no HDR environment multiplier, so this material's environment "
                 "layer reaches Heroes of the Storm unscaled",
                 ElementRef(), profile);
    }
    m3::MaterialMap entry;
    entry.materialType = m3::MaterialType::Standard;
    entry.materialIndex = static_cast<u32>(model.standardMaterials.size());
    model.standardMaterials.push_back(std::move(standard));
    return entry;
}

m3::TextureLayer layerFrom(const TextureInput& input, const Context& context) {
    m3::TextureLayer layer;
    layer.texturePath = context.toPath(input.texture);
    switch (input.mapping) {
    // The Reflect variants: WoW's env texgen and WC3's SphereEnvMap are both
    // classic reflection-vector sphere maps, and the non-reflect forms aim
    // the lookup along the normal instead -- a sheen that never moves.
    case UVMappingMode::EnvCube:
        layer.uvMapping = m3::UVMappingMode::ReflectCubicEnvio;
        break;
    case UVMappingMode::EnvSphere:
        layer.uvMapping = m3::UVMappingMode::ReflectSphericalEnvio;
        break;
    default:
        layer.uvMapping = static_cast<m3::UVMappingMode>(
            input.uvSet <= 1 ? input.uvSet : (input.uvSet == 2 ? 9 : 10));
        break;
    }
    layer.color.initValue = m3::ColorBGRA{
        static_cast<u8>(input.constant.z * 255.0f), static_cast<u8>(input.constant.y * 255.0f),
        static_cast<u8>(input.constant.x * 255.0f), static_cast<u8>(input.constant.w * 255.0f)};
    SetUvTransform(input.uvTransform, layer);
    layer.mapAlpha.initValue = input.weight;
    // The multiply rests at ONE. The struct's default is zero, and a zero
    // multiply is a black layer in the real engine — the same trap the MADD
    // restore hit (`reference_madd_restore_defaults`).
    layer.rgbMultiply.initValue = 1.0f;
    // The unused transforms as every sampled shipped textured layer states them
    // (3,679 of 3,679): the tiling at (1, 1), the W tiling is one, and the
    // tri-planar scale is one on three layers in four. The multiply's and the
    // map alpha's nulls are the struct's.
    layer.uvTiling.nullValue = Vector2f(1.0f, 1.0f);
    layer.wTiling.initValue = 1.0f;
    layer.wTiling.nullValue = 1.0f;
    layer.triplanarScale.initValue = Vector3f{1.0f, 1.0f, 1.0f};
    // Every one of the 7,726 shipped layers that names a texture states this pair,
    // and the editor makes it the precondition for the clamp beside it:
    // `b_iClamp = (flags & 0xC0) && (flags & ColorClamp)` (`sub_141F96390`). Without
    // it the `Home::Toggle` alpha mask's `a * 1 + rgbAdd` went unsaturated.
    layer.flags |= m3::TextureLayerFlag::ColorAdd | m3::TextureLayerFlag::ColorMultiply;
    if (input.wrapU == WrapMode::Repeat) {
        layer.flags |= m3::TextureLayerFlag::UVWrapX;
    }
    if (input.wrapV == WrapMode::Repeat) {
        layer.flags |= m3::TextureLayerFlag::UVWrapY;
    }
    return layer;
}

/// Whether @p input's texture is Warcraft III replaceable @p id — the team
/// colour (1) or the team glow (2). Nothing but the texture table says so.
bool isReplaceable(const TextureInput& input, const Context& context, u32 id) {
    if (context.textureRefs == nullptr || input.texture >= context.textureRefs->size()) {
        return false;
    }
    return (*context.textureRefs)[input.texture].replaceableId == id;
}

/// A WoW combiner chain onto the fixed slots — WOW_TO_SC2_DESIGN.md §3.
///
/// The SC2 shader makes the crossing near-exact: a Mod/Mod2x emissive layer
/// folds multiplicatively into the LIT colour (psmaterial.fx:240-276), which
/// is what a mid-chain modulate stage is — WoW runs its combiners on the lit
/// vertex colour, and scalar lighting distributes over the product. The Add
/// family accumulates unlit there, which is WoW's Add/AddAlpha. Env-mapped
/// stages take the environment slot (ApplyEnv, psmaterial.fx:281-338), whose
/// op is the material's `layerBlendMode`.
void exportCombiners(const CombinersBody& body, const CommonMaterial& common,
                     const Context& context, ProfileId profile, m3::StandardMaterial& standard,
                     Diagnostics& out, std::vector<u32>* layerOrdinals) {
    const auto record = [&](StandardLayer slot, std::size_t ordinal) {
        if (layerOrdinals != nullptr) {
            (*layerOrdinals)[static_cast<std::size_t>(slot)] = static_cast<u32>(ordinal);
        }
    };
    const auto isEnv = [](const CombinerStage& stage) {
        return stage.input.mapping == UVMappingMode::EnvSphere ||
               stage.input.mapping == UVMappingMode::EnvCube;
    };

    // The diffuse seed: the first surface-mapped stage. WoW's env-first
    // vertex shaders (`Diffuse_Env_T1`) fold with the mod family, where the
    // product commutes, so the surface stage can take the lit slot and the
    // env stage its own. An all-env chain (`Diffuse_Env`, the spectral look)
    // seeds with the env stage itself and keeps its spherical mapping.
    std::size_t seed = body.stages.size();
    for (std::size_t i = 0; i < body.stages.size(); ++i) {
        if (!isEnv(body.stages[i])) {
            seed = i;
            break;
        }
    }
    if (seed == body.stages.size() && !body.stages.empty()) {
        seed = 0;
    }

    const bool blendsAlpha = common.blend == BlendMode::AlphaKey ||
                             common.blend == BlendMode::Transparent ||
                             common.blend == BlendMode::AlphaBlend ||
                             common.blend == BlendMode::AdditiveAlpha ||
                             common.blend == BlendMode::PremultipliedAlpha;

    // A mask that multiplies the chain's alpha lands in the two slots the
    // engine composes (`cFinal.a = mask1.a * mask2.a`). Shared by the
    // alpha-only branch below and the emissive stages further down; first
    // free slot wins, and a third mask reports rather than overwriting —
    // the D3 bake merges the static ones upstream so three fit in two
    // (D3_TO_SC2_DESIGN.md §3).
    const auto plantAlphaMask = [&](const CombinerStage& stage, std::size_t i) {
        std::optional<m3::TextureLayer>* slot = !standard.alphaLayer1.has_value()
                                                    ? &standard.alphaLayer1
                                                : !standard.alphaLayer2.has_value()
                                                    ? &standard.alphaLayer2
                                                    : nullptr;
        if (slot == nullptr) {
            out.warn(DiagCode::LayerDropped,
                     "combiner stage " + number(i) +
                         "'s alpha mask found both alpha slots taken",
                     ElementRef(), profile);
            return;
        }
        *slot = layerFrom(stage.input, context);
        (*slot)->colorType = m3::ColorChannelSelect::Alpha;
        record(slot == &standard.alphaLayer1 ? StandardLayer::Alpha1 : StandardLayer::Alpha2, i);
    };

    // The blended-or-keyed mask — the composite path's rule with the chain's
    // own refinement: `Opaque` in a combiner name says the seed contributes
    // no alpha, so only a seed whose alpha op multiplies carries the
    // texture's mask; the rest blend by the batch alpha alone. Planted FIRST
    // so the seed's mask keeps the first slot and the stage masks fill the
    // second, whatever order the chain lists them in.
    if (blendsAlpha && seed < body.stages.size() &&
        body.stages[seed].alpha == CombinerOp::Mod && body.stages[seed].input.hasTexture()) {
        standard.alphaLayer1 = layerFrom(body.stages[seed].input, context);
        standard.alphaLayer1->colorType = m3::ColorChannelSelect::Alpha;
        record(StandardLayer::Alpha1, seed);
    }

    bool emissiveAdds = false;
    for (std::size_t i = 0; i < body.stages.size(); ++i) {
        const CombinerStage& stage = body.stages[i];
        if (i == seed) {
            standard.diffuseLayer = layerFrom(stage.input, context);
            record(StandardLayer::Diffuse, i);
            continue;
        }
        // `Pass` is the identity: a stage that touches alpha alone, or a fold
        // the chain table cannot spell (`_3s`, where the mask reaches a third
        // unit rather than 1). An alpha-only MULTIPLY is Diablo III's mask
        // stage (types 12/14/19), and it used to vanish with the colour
        // identity — Imperius's wings drew as solid sheets with the masks
        // gone. Mod2x stays unexpressed for Kil'jaeden's reason (the
        // un-doubled mask halves the effect); the D3 bake folds those in
        // with their true gain instead.
        if (stage.rgb == CombinerOp::Pass) {
            if (blendsAlpha && stage.alpha == CombinerOp::Mod && stage.input.hasTexture()) {
                plantAlphaMask(stage, i);
            }
            continue;
        }
        // The `_Alpha` masked fold, `rgb * lerp(t*N, 1, t0.a)`, and the two
        // slots that can spell it: the environment layer under ApplyEnv's
        // ADD op (`color + env * mask`), with the SEED's alpha — inverted —
        // as the environment mask. Term for term: where t0.a = 1 the mask
        // closes and the lit colour passes untouched (exactly the native
        // fold); where it opens, `env * N * (1 - t0.a)` adds over the base.
        // The approximation is the open-mask term — native has
        // `lit(t0) * (t0.a + t1 * N * (1 - t0.a))` — but Add keeps the
        // surface's own colour under the glow, which is what the eye reads.
        // The LERP op was measured first and is WRONG: replacing the base
        // painted taurenprimalist's golden plates with the raw env sprite
        // (silver) and mixed its grey into every semi-open fur texel (the
        // "inverted colours" report). Keeping the old collapse-to-Pass
        // dropped the whole sheen — the black armor plates. A masked stage
        // that is NOT env-mapped has no slot that can read the seed's alpha
        // and stays unexpressed, like before.
        if (stage.rgb == CombinerOp::MaskedMod || stage.rgb == CombinerOp::MaskedMod2x) {
            const bool maskable = seed < body.stages.size() && body.stages[seed].input.hasTexture();
            if (!isEnv(stage) || standard.environmentLayer.has_value() || !maskable) {
                out.info(DiagCode::LossyKindConversion,
                         "a masked fold outside the environment slot stays unexpressed",
                         ElementRef(), profile);
                continue;
            }
            standard.environmentLayer = layerFrom(stage.input, context);
            record(StandardLayer::Environment, i);
            standard.layerBlendMode = m3::LayerBlendOp::Add;
            standard.hdrEnvironmentConstant =
                stage.rgb == CombinerOp::MaskedMod2x ? 2.0f : 1.0f;
            standard.environmentMaskLayer = layerFrom(body.stages[seed].input, context);
            standard.environmentMaskLayer->colorType = m3::ColorChannelSelect::Alpha;
            standard.environmentMaskLayer->flags |= m3::TextureLayerFlag::ColorInvert;
            continue;
        }
        if (isEnv(stage)) {
            if (standard.environmentLayer.has_value()) {
                out.warn(DiagCode::LayerDropped,
                         "a second environment-mapped stage has no M3 slot to go into",
                         ElementRef(), profile);
                continue;
            }
            standard.environmentLayer = layerFrom(stage.input, context);
            record(StandardLayer::Environment, i);
            // ApplyEnv knows Mod, Add and Lerp; a Mod2x rides the environment
            // constant, which the engine folds into the layer tint.
            f32 constant = 1.0f;
            switch (stage.rgb) {
            case CombinerOp::Mod2x:
                constant = 2.0f;
                standard.layerBlendMode = m3::LayerBlendOp::Mod;
                break;
            case CombinerOp::Add:
            case CombinerOp::AddAlpha:
                standard.layerBlendMode = m3::LayerBlendOp::Add;
                break;
            case CombinerOp::Decal:
            case CombinerOp::Fade:
                standard.layerBlendMode = m3::LayerBlendOp::Lerp;
                break;
            default: // Opaque / Mod — the fold is a multiply either way.
                standard.layerBlendMode = m3::LayerBlendOp::Mod;
                break;
            }
            // A chain the env stage SEEDED folds against whatever became the
            // diffuse. The product commutes for the mod family; an additive
            // seed does not, and saying so beats silently reordering it.
            if (i < seed &&
                (stage.rgb == CombinerOp::Add || stage.rgb == CombinerOp::AddAlpha)) {
                out.info(DiagCode::LossyKindConversion,
                         "an additive env seed folds as Mod against the surface stage",
                         ElementRef(), profile);
                standard.layerBlendMode = m3::LayerBlendOp::Mod;
            }
            // ApplyEnv's mask defaults to the env layer's own alpha, and the
            // RGB select forces that alpha to 1 — an alpha-weighted env stage
            // needs RGBA to stay masked.
            if (stage.rgb == CombinerOp::AddAlpha || stage.rgb == CombinerOp::Decal ||
                stage.rgb == CombinerOp::Fade) {
                standard.environmentLayer->colorType = m3::ColorChannelSelect::RGBA;
            }
            standard.hdrEnvironmentConstant = constant;
            continue;
        }
        // The two emissive slots, in chain order.
        std::optional<m3::TextureLayer>* slot = nullptr;
        m3::LayerBlendOp* op = nullptr;
        if (!standard.emissiveLayer1.has_value()) {
            slot = &standard.emissiveLayer1;
            op = &standard.emissiveBlendMode1;
            record(StandardLayer::Emissive1, i);
        } else if (!standard.emissiveLayer2.has_value()) {
            slot = &standard.emissiveLayer2;
            op = &standard.emissiveBlendMode2;
            record(StandardLayer::Emissive2, i);
        } else {
            out.warn(DiagCode::LayerDropped,
                     "combiner stage " + number(i) + " found both emissive slots taken",
                     ElementRef(), profile);
            continue;
        }
        *slot = layerFrom(stage.input, context);
        switch (stage.rgb) {
        case CombinerOp::Mod2x:
            *op = m3::LayerBlendOp::Mod2x;
            break;
        case CombinerOp::Add:
            *op = m3::LayerBlendOp::AddNoAlpha;
            emissiveAdds = true;
            break;
        case CombinerOp::AddAlpha:
            *op = m3::LayerBlendOp::Add;
            emissiveAdds = true;
            break;
        case CombinerOp::Decal:
        case CombinerOp::Fade:
            *op = m3::LayerBlendOp::Lerp;
            break;
        default: // Opaque / Mod — "Opaque" only says the unit adds no ALPHA.
            *op = m3::LayerBlendOp::Mod;
            break;
        }
        // The RGB select forces the sampled alpha to 1 (psmateriallayer.fx,
        // verbatim in the renderer), so an alpha-weighted op must carry the
        // texture's own alpha through the RGBA select — without it the Earth
        // Spirit's tiny glow spots added their cream base over the whole body.
        if (*op == m3::LayerBlendOp::Add || *op == m3::LayerBlendOp::Lerp) {
            (*slot)->colorType = m3::ColorChannelSelect::RGBA;
        }
        // A stage whose ALPHA op multiplies also masks the draw; the alpha
        // slots are the product's home (cFinal.a = mask1.a * mask2.a).
        // NOT Mod2x: its mask saturates (c.a * t1.a * 2), and carrying it
        // without the doubling halved Kil'jaeden's fire — measured 8.8 -> 23.1
        // — so the doubled mask stays unexpressed, like the op's colour side.
        if (blendsAlpha && stage.alpha == CombinerOp::Mod && stage.input.hasTexture()) {
            plantAlphaMask(stage, i);
        }
    }
    if (emissiveAdds) {
        // WoW's adds are unscaled, and the multiplier's struct default of
        // zero is a black glow — the Sorceress-silhouette trap from the
        // composite path, met from the other side.
        standard.hdrEmissiveMultiplier = 1.0f;
    }
}

/// A Diablo III slot map onto the fixed slots — D3_TO_SC2_DESIGN.md §2.
///
/// `LegacySlot` is this generation's own vocabulary, so most of the table is a
/// rename. The three rules that are not renames each cost a rendered frame:
///
/// * **A zero-factor environment is dropped.** Every shipped Diablo III
///   material carries the type-4 entry with `environmentFactor` 0 and the D3
///   shading model has no environment term at all — crossing it live under the
///   default `Mod` op multiplied the lit colour by `env * 0`, which was the
///   black-armour frame. The MDX exporter's rule, ported.
/// * **The specular strength lives in the MAP.** `flShininess` is 0.0 on 99.4%
///   of shipped materials and `vSpecular` ~1.0 on 99.0% — taking the factor
///   with no map is `pow(x, 0) = 1`, a hemisphere-wide highlight on every
///   surface. No map means no specular; the exponent is the global
///   `SpecularPower` (the D3 renderer's 24), never `flShininess`.
/// * **The glow ADDS.** `scene_opaque_glow` adds the map outside the albedo;
///   `actor2` adds it inside, and the two differ only where the albedo is
///   dark. The default `emissiveBlendMode` is Mod, which multiplies the lit
///   colour by a mostly-black map — the Sorceress-silhouette trap.
void exportLegacy(const LegacyDeferredBody& body, const CommonMaterial& common,
                  const Context& context, ProfileId profile, m3::StandardMaterial& standard,
                  Diagnostics& out, std::vector<u32>* layerOrdinals) {
    const auto record = [&](StandardLayer slot, std::size_t ordinal) {
        if (layerOrdinals != nullptr) {
            (*layerOrdinals)[static_cast<std::size_t>(slot)] = static_cast<u32>(ordinal);
        }
    };

    standard.specularExponent = 24.0f;
    standard.hdrSpecularMultiplier = 1.0f; // the shipped floor; a specular slot overrides it
    standard.hdrEmissiveMultiplier = 0.0f;
    standard.hdrEnvironmentConstant = 0.0f;

    for (std::size_t ordinal = 0; ordinal < body.slots.size(); ++ordinal) {
        const LegacySlot slot = body.slots[ordinal].first;
        const TextureInput& input = body.slots[ordinal].second;
        switch (slot) {
        case LegacySlot::Diffuse:
            // `diffuseFactor` (matDiffuse) is deliberately NOT folded into the
            // layer colour, though the D3 albedo genuinely contains it: the D3
            // rig is engineered hot to compensate (ambient SUMS, the vertex
            // colour at x2, the irradiance x2), and shipped bodies carry a
            // ~0.6 neutral grey there. Folding the dim half without the rig's
            // hot half rendered every character at two-thirds brightness with
            // the hue already right — the composite path never folded it
            // either, and pixel ratios (g/r, b/r) match native without it.
            standard.diffuseLayer = layerFrom(input, context);
            record(StandardLayer::Diffuse, ordinal);
            break;
        case LegacySlot::Normal:
            standard.normalLayer = layerFrom(input, context);
            record(StandardLayer::Normal, ordinal);
            break;
        case LegacySlot::Specular:
            standard.specularLayer = layerFrom(input, context);
            record(StandardLayer::Specular, ordinal);
            standard.specularMode = m3::SpecularMode::RGB;
            standard.hdrSpecularMultiplier =
                body.specularFactor.x > 0.0f ? body.specularFactor.x : 1.0f;
            break;
        case LegacySlot::Emissive:
            standard.emissiveLayer1 = layerFrom(input, context);
            record(StandardLayer::Emissive1, ordinal);
            standard.emissiveBlendMode1 = m3::LayerBlendOp::AddNoAlpha;
            standard.hdrEmissiveMultiplier = 1.0f;
            break;
        case LegacySlot::Environment:
            if (body.environmentFactor <= 0.0f) {
                out.info(DiagCode::LayerDropped,
                         "the environment entry rides a zero factor, and the source shading "
                         "model never sampled it",
                         ElementRef(), profile);
                break;
            }
            standard.environmentLayer = layerFrom(input, context);
            record(StandardLayer::Environment, ordinal);
            standard.layerBlendMode = m3::LayerBlendOp::Add;
            standard.hdrEnvironmentConstant = body.environmentFactor;
            break;
        case LegacySlot::Lightmap:
            // A baked lighting term multiplies the lit result on both sides.
            // No `StandardLayer` ordinal names it — it never was a composite
            // channel — so a UV animation on it cannot be reported; shipped
            // lightmaps carry none.
            standard.lightMapLayer = layerFrom(input, context);
            break;
        case LegacySlot::Detail:
            // MEASURED WRONG as a decal, twice over: the fold is
            // `lerp(diffuse, t25, t25.a·k)` and `k` is RUNTIME STATE resting
            // at zero — the Barbarian's type 25 is the gore overlay, shared
            // by every body material, and crossing it at full alpha painted
            // the whole body its grey. The reference renderer never samples
            // the slot either, so the crossing has no oracle until someone
            // traces which programs read it and from what constant `k` comes
            // (`feedback_trace_the_read_not_the_write`). It stays in the
            // native block.
            out.info(DiagCode::LayerDropped,
                     "the detail layer stays unexpressed (its blend factor is runtime "
                     "state resting at zero)",
                     ElementRef(), profile);
            break;
        case LegacySlot::Gloss:
            standard.glossLayer = layerFrom(input, context);
            break;
        case LegacySlot::AmbientOcclusion:
            standard.ambientOcclusionLayer = layerFrom(input, context);
            record(StandardLayer::AmbientOcclusion, ordinal);
            break;
        case LegacySlot::Height:
            standard.heightLayer = layerFrom(input, context);
            break;
        default:
            out.warn(DiagCode::LayerDropped,
                     std::string("legacy slot '") + ToString(slot) + "' has no M3 slot",
                     ElementRef(), profile);
            break;
        }
    }

    // A keyed or blended surface tests the COMPOSED alpha (`cFinal.a =
    // mask1.a * mask2.a`), so the diffuse's own alpha must become a mask or
    // nothing is ever cut — the composite path's rule, verbatim.
    if (!standard.alphaLayer1.has_value() &&
        (common.blend == BlendMode::AlphaKey || common.blend == BlendMode::Transparent ||
         common.blend == BlendMode::AlphaBlend || common.blend == BlendMode::AdditiveAlpha) &&
        standard.diffuseLayer.has_value() && !standard.diffuseLayer->texturePath.empty()) {
        standard.alphaLayer1 = *standard.diffuseLayer;
        standard.alphaLayer1->colorType = m3::ColorChannelSelect::Alpha;
    }
}

// ── the Warcraft III pass fold (WC3_SD_MATERIAL_TO_SC2_DESIGN.md §5) ────────
//
// A Warcraft III material is an ordered list of whole-geoset passes; a
// StandardMaterial is a fixed pipeline -- diffuse, decal, LIGHT, emissive1 and
// emissive2 (whose Mod-family ops fold straight into the lit colour, in slot
// order), the Add-family emissive sum, the environment layer. Each stage is
// the home of one kind of pass, and a pass's SHADING flag, not its filter,
// picks between the pre-lighting home (the decal) and the post-lighting ones
// (the emissive slots). A second material -- a CMP_ section -- opens only when
// no stage can take a pass the way Warcraft III drew it; `exactPasses` widens
// "cannot" to "cannot exactly". Measured over the shipped art this is one
// material for every official material and 71 composites in 1,116 community
// ones (design §5.7).

struct Pass {
    u32 ordinal = 0;
    TextureInput input;
    CompositeOp op = CompositeOp::Set;
    bool team = false; ///< replaceable 1 -- the team colour plate
    bool glow = false; ///< replaceable 2 -- the team glow
    bool unlit = false;
    bool twoSided = false;
    bool unfogged = false;
    bool noDepthTest = false;
    bool noDepthWrite = false;
    bool alphaTracked = false;
    bool frameTracked = false;
    TextureAlphaClass alpha = TextureAlphaClass::Unknown;

    bool additive() const {
        return op == CompositeOp::Add || op == CompositeOp::AddAlpha;
    }
    bool blendLike() const {
        return op == CompositeOp::AlphaKey || op == CompositeOp::AlphaBlend;
    }
    bool modulate() const {
        return op == CompositeOp::Modulate || op == CompositeOp::Modulate2x;
    }
    bool env() const {
        return input.mapping == UVMappingMode::EnvSphere ||
               input.mapping == UVMappingMode::EnvCube;
    }
    /// Whether the pass's own alpha ever leaves 1: a track, or a static weight.
    bool fades() const {
        return alphaTracked || input.weight < 0.999f;
    }
};

TextureAlphaClass alphaClassOf(const TextureInput& input, const Context& context) {
    if (context.textureAlphaClasses == nullptr ||
        input.texture >= context.textureAlphaClasses->size()) {
        return TextureAlphaClass::Unknown;
    }
    return static_cast<TextureAlphaClass>((*context.textureAlphaClasses)[input.texture]);
}

Pass passOf(u32 ordinal, const TextureInput& input, CompositeOp op, const CommonMaterial& common,
            const Context& context, const ExportHints* hints) {
    Pass pass;
    pass.ordinal = ordinal;
    pass.input = input;
    pass.op = op;
    pass.team = isReplaceable(input, context, 1);
    pass.glow = isReplaceable(input, context, 2);
    pass.alpha = alphaClassOf(input, context);
    if (hints != nullptr && ordinal < 64) {
        pass.alphaTracked = ((hints->alphaTrackedOrdinals >> ordinal) & 1u) != 0;
        pass.frameTracked = ((hints->textureIndexTrackedOrdinals >> ordinal) & 1u) != 0;
    }
    // The header's decision, then the layer's own where it kept one.
    pass.unlit = hasFlag(common.flags, MaterialFlags::Unlit);
    pass.twoSided = common.cull == CullMode::None;
    pass.unfogged = hasFlag(common.flags, MaterialFlags::Unfogged);
    pass.noDepthTest = !common.depth.test;
    pass.noDepthWrite = !common.depth.write;
    if (const MaterialFeature* feature = common.feature(FeatureKind::LayerShading, ordinal)) {
        if (const LayerShadingFeature* shading = feature->layerShading()) {
            pass.unlit = shading->unlit;
            pass.twoSided = shading->twoSided;
            pass.unfogged = shading->unfogged;
            pass.noDepthTest = shading->noDepthTest;
            pass.noDepthWrite = shading->noDepthWrite;
        }
    }
    return pass;
}

/// The Color-channel layers of a composite body, in order. Every other channel
/// is a slot the first section takes afterwards (`placeExtras`).
std::vector<Pass> passesOf(const CompositeBody& body, const CommonMaterial& common,
                           const Context& context, const ExportHints* hints) {
    std::vector<Pass> passes;
    for (std::size_t i = 0; i < body.layers.size(); ++i) {
        const CompositeLayer& layer = body.layers[i];
        if (layer.target != SurfaceChannel::Color) {
            continue;
        }
        passes.push_back(
            passOf(static_cast<u32>(i), layer.input, layer.op, common, context, hints));
    }
    return passes;
}

/// A collapsed Warcraft III stack (§7.2.2: an opaque pass under modulate or
/// additive ones) is a combiner chain in WEM and a pass list here.
std::vector<Pass> passesOf(const CombinersBody& body, const CommonMaterial& common,
                           const Context& context, const ExportHints* hints) {
    std::vector<Pass> passes;
    for (std::size_t i = 0; i < body.stages.size(); ++i) {
        const CombinerStage& stage = body.stages[i];
        CompositeOp op = CompositeOp::Set;
        switch (stage.rgb) {
        case CombinerOp::Opaque:
            op = CompositeOp::Set;
            break;
        case CombinerOp::Mod:
            op = CompositeOp::Modulate;
            break;
        case CombinerOp::Mod2x:
            op = CompositeOp::Modulate2x;
            break;
        case CombinerOp::Add:
            op = CompositeOp::Add;
            break;
        case CombinerOp::AddAlpha:
            op = CompositeOp::AddAlpha;
            break;
        case CombinerOp::Decal:
        case CombinerOp::Fade:
            op = CompositeOp::AlphaBlend;
            break;
        default:
            continue; // Pass and the masked folds: nothing a Warcraft III stack spells
        }
        passes.push_back(passOf(static_cast<u32>(i), stage.input, i == 0 ? CompositeOp::Set : op,
                                common, context, hints));
    }
    return passes;
}

/// A solid-colour layer: the fade carriers Blizzard's own conversions animate,
/// the team-weight decal, the environment mask. `alpha` is what every op reads
/// off it; `rgb` matters only to an op that reads the colour (the env mask
/// under Add).
m3::TextureLayer carrierLayer(f32 alpha, f32 rgb = 1.0f) {
    const auto byte = [](f32 v) {
        return static_cast<u8>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    m3::TextureLayer layer;
    layer.flags = m3::TextureLayerFlag::Color;
    layer.color.initValue = m3::ColorBGRA{byte(rgb), byte(rgb), byte(rgb), byte(alpha)};
    layer.rgbMultiply.initValue = 1.0f;
    layer.mapAlpha.initValue = 1.0f;
    return layer;
}

/// A pass's static alpha on the layer that carries the pass. Retail multiplies
/// `mapAlpha` into the sampled alpha (`cResult.a *= z`, ComputeLayerColorInternal)
/// and never reads a textured layer's `color`; this build's renderer folds
/// `color.a` and never reads a textured layer's `mapAlpha`. Both are written so
/// each engine applies the weight exactly once.
void weightLayer(m3::TextureLayer& layer, f32 weight) {
    layer.mapAlpha.initValue = weight;
    layer.color.initValue.a = static_cast<u8>(std::clamp(weight, 0.0f, 1.0f) * 255.0f + 0.5f);
}

/// A solid-colour carrier in @p rgb: the rim overlay's two halves.
m3::TextureLayer tintCarrier(const Vector3f& rgb, f32 alpha) {
    const auto byte = [](f32 v) {
        return static_cast<u8>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    m3::TextureLayer layer = carrierLayer(alpha);
    layer.color.initValue.r = byte(rgb.x);
    layer.color.initValue.g = byte(rgb.y);
    layer.color.initValue.b = byte(rgb.z);
    return layer;
}

/// `(1 - |n.v|)^exponent` remapped onto [from, to]: the layer's min/max are the
/// ramp's ends, not a clamp on the term (`fresnelExponentBiasScale`).
void fresnelRamp(m3::TextureLayer& layer, f32 exponent, f32 from, f32 to) {
    layer.fresnelMode = m3::FresnelMode::Standard;
    layer.fresnelExponent = exponent;
    layer.fresnelMin = from;
    layer.fresnelMax = to;
    // The view-vector transform stays off (flag 0x4000 clear); its fields are
    // written at their identity rather than whatever the stack held.
    layer.fresnelTranslation = Vector3f{0.0f, 0.0f, 0.0f};
    layer.fresnelMask = Vector3f{1.0f, 1.0f, 1.0f};
    layer.fresnelRotation = Vector2f{0.0f, 0.0f};
}

bool modFamily(m3::LayerBlendOp op) {
    return op == m3::LayerBlendOp::Mod || op == m3::LayerBlendOp::Mod2x ||
           op == m3::LayerBlendOp::Lerp;
}

/// A pass's own filter as a material blend, for a section whose base it is.
m3::BlendMode blendForPass(CompositeOp op, u32& threshold) {
    threshold = 0;
    switch (op) {
    case CompositeOp::AlphaKey:
        threshold = 192; // 0.75 * 256, the Warcraft III key
        return m3::BlendMode::Opaque;
    case CompositeOp::AlphaBlend:
        return m3::BlendMode::AlphaBlend;
    case CompositeOp::Add:
    case CompositeOp::AddAlpha:
        return m3::BlendMode::AlphaAdd;
    case CompositeOp::Modulate:
        return m3::BlendMode::Mod;
    case CompositeOp::Modulate2x:
        return m3::BlendMode::Mod2x;
    default:
        return m3::BlendMode::Opaque;
    }
}

/// One StandardMaterial being filled: a section of the answer.
struct Section {
    enum class Kind : u8 {
        Opaque,       ///< the base covers its geoset: every fold onto it is exact
        Partial,      ///< keyed or blended base: a later pass is confined to its coverage
        Additive,     ///< an additive base with nothing additive after it
        Modulate,     ///< a modulate base: nothing folds onto it
        AdditiveOnly, ///< no diffuse; every pass an emissive, self-weighted (R3b)
        Glow,         ///< the team glow (R4)
    };
    m3::StandardMaterial standard;
    std::vector<u32> ordinals;
    std::vector<u32> passOrdinals;
    Kind kind = Kind::Opaque;
    bool unlit = false;
    bool twoSided = false;
    bool unfogged = false;
    bool teamDiffuse = false; ///< the RGBA select carries the team colour
    bool hasDecal = false;
    m3::LayerBlendOp decalOp = m3::LayerBlendOp::Mod;
    u32 emisMod = 0;
    u32 emisAdd = 0;
    bool hasEnv = false;
    u32 stage = 1;
    bool baseFades = false;
    f32 baseWeight = 1.0f;
    u32 baseTexture = kInvalidIndex;
    u32 baseOrdinal = kInvalidIndex;
    TextureAlphaClass baseAlpha = TextureAlphaClass::Unknown;
    bool toggle = false;

    Section() : ordinals(static_cast<std::size_t>(StandardLayer::Count), kInvalidIndex) {}

    bool emissiveFree() const {
        return emisMod + emisAdd < 2;
    }
    void record(StandardLayer slot, u32 ordinal) {
        ordinals[static_cast<std::size_t>(slot)] = ordinal;
    }
};

enum class Home : u8 { Boundary, Emissive, Decal, Env, Toggle, TeamDecal };

struct Placement {
    Home home = Home::Boundary;
    m3::LayerBlendOp op = m3::LayerBlendOp::Mod;
    std::vector<DiagCode> approx;
};

/// Where @p p goes in @p s and what that costs -- or `Boundary`.
Placement decide(const Section& s, const Pass& p, const Context& context) {
    using Op = m3::LayerBlendOp;
    Placement r;
    const bool decalFree = !s.hasDecal;
    const bool emisFree = s.emissiveFree();
    const bool hasAdd = s.emisAdd > 0;
    const bool hasMod = s.emisMod > 0;
    // Only a Warcraft III stack picks a home by shading; a body that already
    // had a decal slot (an edited .m3) means that slot.
    const bool byShading = context.warcraftPasses;
    const auto place = [&r](Home home, Op op) {
        r.home = home;
        r.op = op;
    };
    if (s.kind == Section::Kind::Modulate) {
        return r;
    }
    if (s.kind == Section::Kind::Additive && !p.additive()) {
        // One blend state cannot add and then replace: the base added to the
        // frame, this pass would overwrite it (the Jackal Tank family).
        return r;
    }
    // The material's fixed order puts the decal before lighting and the
    // Add-family emissive sum after everything but the environment; a blend
    // or modulate pass that FOLLOWED an additive one in Warcraft III can still
    // fold, but the add is not blended down with the rest (`PassOrderFolded`).
    // A Mod-family emissive must precede the Add-family ones in slot order,
    // which `apply` arranges by shifting the add to the second slot.
    const bool decalOrder = s.stage <= 2 || s.emisMod == 0;
    if (p.op == CompositeOp::AlphaKey && s.passOrdinals.size() == 1 &&
        s.kind == Section::Kind::Opaque && !s.teamDiffuse && s.baseFades && p.input.hasTexture() &&
        p.input.texture == s.baseTexture && !s.standard.alphaLayer1.has_value() &&
        s.standard.diffuseLayer.has_value()) {
        // R6b: the same texture keyed over its own fading opaque pass is a
        // coverage switch, not a colour stack.
        place(Home::Toggle, Op::Mod);
    } else if (p.glow) {
        if (!emisFree) {
            return r;
        }
        place(Home::Emissive, Op::TeamColorEmissiveAdd);
    } else if (p.team) {
        if (p.additive()) {
            if (!emisFree) {
                return r;
            }
            place(Home::Emissive, Op::TeamColorDiffuseAdd);
        } else if (p.blendLike() &&
                   (s.kind == Section::Kind::Opaque || s.kind == Section::Kind::Partial) &&
                   decalFree && s.stage <= 2 && s.standard.diffuseLayer.has_value()) {
            place(Home::TeamDecal, Op::TeamColorDiffuseAdd);
        } else {
            return r;
        }
    } else if (s.kind == Section::Kind::AdditiveOnly || s.kind == Section::Kind::Glow) {
        if (!p.additive() || !emisFree) {
            return r;
        }
        place(Home::Emissive, Op::Add);
        if (!p.unlit && !s.unlit) {
            r.approx.push_back(DiagCode::ShadedAdditiveFolded);
        }
    } else if (!byShading) {
        // An additive colour layer is a glow and goes where Blizzard's own
        // conversions put one; the rest is the source's decal.
        const Op op = p.additive() ? (p.op == CompositeOp::Add ? Op::AddNoAlpha : Op::Add)
                      : p.modulate() ? (p.op == CompositeOp::Modulate2x ? Op::Mod2x : Op::Mod)
                                     : Op::Lerp;
        if (p.additive() && emisFree) {
            place(Home::Emissive, op);
        } else if (decalFree && s.stage <= 2) {
            place(Home::Decal, op);
        } else if (emisFree && (!modFamily(op) || !hasAdd)) {
            place(Home::Emissive, op);
        } else {
            return r;
        }
    } else if (p.env()) {
        const Op op = p.additive() ? Op::Add : p.modulate() ? Op::Mod : Op::Lerp;
        if (s.hasEnv || (s.hasDecal && s.decalOp != op)) {
            return r;
        }
        place(Home::Env, op);
        if (!p.unlit) {
            r.approx.push_back(DiagCode::LitEnvFolded);
        }
    } else if (p.blendLike()) {
        if (!p.unlit) {
            if (decalFree && decalOrder) {
                place(Home::Decal, Op::Lerp);
            } else if (emisFree) {
                place(Home::Emissive, Op::Lerp);
                r.approx.push_back(DiagCode::UnlitFold);
            } else {
                return r;
            }
        } else {
            if (emisFree) {
                place(Home::Emissive, Op::Lerp);
            } else if (decalFree && decalOrder) {
                place(Home::Decal, Op::Lerp);
                r.approx.push_back(DiagCode::LitFold);
            } else {
                return r;
            }
        }
        if (p.op == CompositeOp::AlphaKey && p.alpha == TextureAlphaClass::Gradient) {
            r.approx.push_back(DiagCode::SoftKey);
        }
    } else if (p.modulate()) {
        const Op op = p.op == CompositeOp::Modulate2x ? Op::Mod2x : Op::Mod;
        if (decalFree && decalOrder) {
            place(Home::Decal, op);
        } else if (emisFree) {
            place(Home::Emissive, op);
        } else {
            return r;
        }
        if (!p.unlit) {
            r.approx.push_back(DiagCode::DoubleLit);
        }
        if (p.alphaTracked) {
            r.approx.push_back(DiagCode::ModAlphaFolded);
        }
    } else if (p.additive()) {
        // Both Warcraft III additive filters are `rgb * a` at the blend, so
        // both are the alpha-weighted `Add`.
        if (p.unlit) {
            if (emisFree) {
                place(Home::Emissive, Op::Add);
            } else if (s.unlit && decalFree && !hasMod) {
                place(Home::Decal, Op::Add); // adds commute; an unlit material lights nothing
            } else {
                return r;
            }
        } else {
            if (decalFree && !hasMod && !s.unlit) {
                place(Home::Decal, Op::Add); // lighting is linear in albedo
            } else if (emisFree) {
                place(Home::Emissive, Op::Add);
                r.approx.push_back(DiagCode::ShadedAdditiveFolded);
            } else {
                return r;
            }
        }
    } else {
        return r;
    }
    if (r.home != Home::Toggle) {
        if (s.kind == Section::Kind::Partial ||
            (s.kind == Section::Kind::Additive && s.baseAlpha != TextureAlphaClass::Opaque)) {
            r.approx.push_back(DiagCode::CoverageClipped);
        }
        if (s.baseFades) {
            r.approx.push_back(DiagCode::BaseFadeShared);
        }
        if ((r.home == Home::Decal && !p.additive() && s.stage > 2) ||
            (r.home == Home::Emissive && modFamily(r.op) && hasAdd)) {
            r.approx.push_back(DiagCode::PassOrderFolded);
        }
    }
    if (p.twoSided != s.twoSided || p.unfogged != s.unfogged) {
        r.approx.push_back(DiagCode::PassFlagsFolded);
    }
    if (context.exactPasses && !r.approx.empty()) {
        r.home = Home::Boundary;
    }
    return r;
}

void reportPass(const Pass& p, const std::vector<DiagCode>& approx, ProfileId profile,
                Diagnostics& out) {
    const ElementRef where(ElementKind::Layer, p.ordinal);
    if (p.noDepthTest || p.noDepthWrite) {
        out.info(DiagCode::DepthFlagsDropped,
                 "pass " + number(p.ordinal) +
                     " asked for no depth test or write; no StarCraft II material says that",
                 where, profile);
    }
    if (p.frameTracked) {
        out.info(DiagCode::FlipbookDropped,
                 "pass " + number(p.ordinal) + " keys its texture id; the first frame stays",
                 where, profile);
    }
    for (const DiagCode code : approx) {
        out.info(code, "pass " + number(p.ordinal) + " folded approximately: " + ToString(code),
                 where, profile);
    }
}

void apply(Section& s, const Pass& p, const Placement& r, const Context& context,
           ProfileId profile, Diagnostics& out, ExportReport* report) {
    using Op = m3::LayerBlendOp;
    m3::StandardMaterial& m = s.standard;
    const auto textured = [&](Op op) {
        m3::TextureLayer layer = layerFrom(p.input, context);
        if (op == Op::Add || op == Op::Lerp) {
            // The RGB select forces the sampled alpha to 1; these ops weight by it.
            layer.colorType = m3::ColorChannelSelect::RGBA;
        }
        weightLayer(layer, p.input.weight);
        return layer;
    };
    switch (r.home) {
    case Home::Emissive: {
        if (modFamily(r.op) && m.emissiveLayer1.has_value() && !modFamily(m.emissiveBlendMode1) &&
            !m.emissiveLayer2.has_value()) {
            // The Mod-family slot must come first (retail's accumulator
            // oddity); the add's sum is applied after both slots either way.
            m.emissiveLayer2 = std::move(m.emissiveLayer1);
            m.emissiveBlendMode2 = m.emissiveBlendMode1;
            m.emissiveLayer1.reset();
            s.ordinals[static_cast<std::size_t>(StandardLayer::Emissive2)] =
                s.ordinals[static_cast<std::size_t>(StandardLayer::Emissive1)];
            s.ordinals[static_cast<std::size_t>(StandardLayer::Emissive1)] = kInvalidIndex;
        }
        const bool first = !m.emissiveLayer1.has_value();
        std::optional<m3::TextureLayer>& slot = first ? m.emissiveLayer1 : m.emissiveLayer2;
        m3::LayerBlendOp& op = first ? m.emissiveBlendMode1 : m.emissiveBlendMode2;
        if (p.glow) {
            slot = layerFrom(p.input, context);
            slot->colorType = m3::ColorChannelSelect::Red;
            weightLayer(*slot, p.input.weight);
        } else if (p.team) {
            slot = carrierLayer(p.input.weight);
        } else {
            slot = textured(r.op);
        }
        op = r.op;
        s.record(first ? StandardLayer::Emissive1 : StandardLayer::Emissive2, p.ordinal);
        if (modFamily(r.op)) {
            ++s.emisMod;
            s.stage = (std::max)(s.stage, 3u);
        } else {
            ++s.emisAdd;
            s.stage = (std::max)(s.stage, 4u);
        }
        break;
    }
    case Home::Decal:
        m.decalLayer = textured(r.op);
        m.layerBlendMode = r.op;
        s.hasDecal = true;
        s.decalOp = r.op;
        s.record(StandardLayer::Decal, p.ordinal);
        s.stage = (std::max)(s.stage, 2u);
        break;
    case Home::TeamDecal: {
        // `lerp(base, team, w)`: the diffuse scaled by `1 - w` and the team
        // colour added at `w` -- the tint alpha cannot do it, the RGBA lerp
        // keys on the RAW sampled alpha.
        const f32 w = std::clamp(p.input.weight, 0.0f, 1.0f);
        if (w >= 0.999f) {
            // The plate covers the base outright: solid team colour (R3).
            m.diffuseLayer = layerFrom(p.input, context);
            m.diffuseLayer->colorType = m3::ColorChannelSelect::RGBA;
            m.alphaTestThreshold = 0;
            s.teamDiffuse = true;
            s.record(StandardLayer::Diffuse, p.ordinal);
        } else {
            // Never 0: this build's renderer reads a zero multiply as the
            // unauthored sentinel (1).
            m.diffuseLayer->rgbMultiply.initValue = (std::max)(1.0f - w, 0.002f);
            m.decalLayer = carrierLayer(w);
            m.layerBlendMode = Op::TeamColorDiffuseAdd;
            s.hasDecal = true;
            s.decalOp = Op::TeamColorDiffuseAdd;
            s.record(StandardLayer::Decal, p.ordinal);
        }
        s.stage = (std::max)(s.stage, 2u);
        break;
    }
    case Home::Env: {
        m.environmentLayer = layerFrom(p.input, context);
        if (r.op != Op::Mod) {
            m.environmentLayer->colorType = m3::ColorChannelSelect::RGBA;
        }
        // ApplyEnv reads the mask's rgb under Add and its alpha under Lerp,
        // times the env sample's alpha -- so the weight sits on all four.
        m.environmentMaskLayer = carrierLayer(p.input.weight, p.input.weight);
        m.layerBlendMode = r.op;
        if (m.hdrEnvironmentConstant <= 0.0f) {
            m.hdrEnvironmentConstant = 1.0f;
        }
        s.hasEnv = true;
        s.record(StandardLayer::EnvironmentMask, p.ordinal);
        s.stage = 5;
        break;
    }
    case Home::Toggle:
        // Alive the base pass covers the windows; dead it has faded and the
        // keyed pass leaves holes. `a * 1 + rgbAdd`, clamped: an add of 1
        // passes every texel, an add of 0 keys them. The base's alpha track
        // drives the add (`ExportReport::coverageSwitchOrdinal`).
        m.alphaLayer1 = *m.diffuseLayer;
        m.alphaLayer1->colorType = m3::ColorChannelSelect::Alpha;
        m.alphaLayer1->flags |= m3::TextureLayerFlag::ColorClamp;
        m.alphaLayer1->mapAlpha.initValue = 1.0f;
        m.alphaLayer1->rgbAdd.initValue = std::clamp(s.baseWeight, 0.0f, 1.0f);
        m.alphaTestThreshold = 192;
        s.toggle = true;
        s.record(StandardLayer::Alpha1, p.ordinal);
        if (report != nullptr) {
            report->coverageSwitchOrdinal = s.baseOrdinal;
        }
        break;
    case Home::Boundary:
        break;
    }
    if (p.twoSided) {
        // Promoted: drawing the base's back faces too is the lesser error.
        m.flags |= m3::MaterialFlag::TwoSided;
    }
    reportPass(p, r.approx, profile, out);
    s.passOrdinals.push_back(p.ordinal);
}

/// Opens a material on @p passes[i] -- and on i + 1 too for a team plate under
/// a textured pass. Returns how many passes it took.
std::size_t open(Section& s, const std::vector<Pass>& passes, std::size_t i,
                 const CommonMaterial& common, const Context& context, ProfileId profile,
                 Diagnostics& out) {
    using Op = m3::LayerBlendOp;
    const Pass& p = passes[i];
    const Pass* next = i + 1 < passes.size() ? &passes[i + 1] : nullptr;
    const bool nextTextured = next != nullptr && !next->team && !next->glow;
    m3::StandardMaterial& m = s.standard;

    u32 threshold = 0;
    m3::BlendMode blend = m3::BlendMode::Opaque;
    if (p.ordinal == 0) {
        // The stack's own meeting with the scene: the header.
        blend = blendFor(common.blend);
        if (context.warcraftPasses && common.blend == BlendMode::Additive) {
            blend = m3::BlendMode::AlphaAdd;
        }
        threshold = (std::min)(255u, static_cast<u32>(common.alphaTestThreshold * 256.0f + 0.5f));
    } else {
        blend = blendForPass(p.op, threshold);
    }
    s.unlit = p.unlit;
    s.twoSided = p.twoSided;
    s.unfogged = p.unfogged;
    s.baseOrdinal = p.ordinal;
    s.baseWeight = p.input.weight;
    s.baseFades = p.fades();
    s.baseTexture = p.input.texture;
    s.baseAlpha = p.alpha;
    std::size_t taken = 1;
    const bool additiveBlend = blend == m3::BlendMode::AlphaAdd || blend == m3::BlendMode::Add;

    if (p.glow) {
        // R4. `Opaque` never reads the material alpha, so an opaque glow's
        // static weight rides the layer; a blended one rides the carrier
        // `finish` plants -- and only there. The team op reads the layer's own
        // alpha (`model_material.slang:355`), so a weight on both dimmed the
        // glow twice (0.75 * 0.75 = 0.56 on the Skink hero). The oracle's hero
        // glows state mapAlpha 1 beside a 191/255 carrier.
        s.kind = Section::Kind::Glow;
        m.emissiveLayer1 = layerFrom(p.input, context);
        m.emissiveLayer1->colorType = m3::ColorChannelSelect::Red;
        m.emissiveBlendMode1 = Op::TeamColorEmissiveAdd;
        weightLayer(*m.emissiveLayer1,
                    blend == m3::BlendMode::Opaque ? p.input.weight : 1.0f);
        s.record(StandardLayer::Emissive1, p.ordinal);
        s.emisAdd = 1;
        s.stage = 4;
    } else if (p.team && nextTextured && (next->blendLike() || next->modulate())) {
        // R2: the texture's alpha IS the team mask. Shading, sidedness and fog
        // are the texture pass's; the plate's own vanish with it.
        m.diffuseLayer = layerFrom(next->input, context);
        m.diffuseLayer->colorType = m3::ColorChannelSelect::RGBA;
        blend = m3::BlendMode::Opaque;
        threshold = 0;
        s.teamDiffuse = true;
        s.kind = Section::Kind::Opaque;
        s.unlit = next->unlit;
        s.twoSided = next->twoSided;
        s.unfogged = next->unfogged;
        s.baseOrdinal = next->ordinal;
        s.baseWeight = 1.0f;
        s.baseFades = next->alphaTracked;
        s.baseTexture = next->input.texture;
        s.record(StandardLayer::Diffuse, next->ordinal);
        if (p.unlit && !next->unlit) {
            out.info(DiagCode::TeamPlateUnlit,
                     "the unlit team plate is lit with the texture over it, as every one of "
                     "Blizzard's own conversions has it",
                     ElementRef(ElementKind::Layer, p.ordinal), profile);
        }
        if (next->frameTracked) {
            reportPass(*next, {}, profile, out);
        }
        taken = 2;
    } else if (p.team && nextTextured && next->additive()) {
        // R3b, the ghosts: the plate is a team add, the texture follows.
        s.kind = Section::Kind::AdditiveOnly;
        blend = m3::BlendMode::AlphaAdd;
        threshold = 0;
        m.emissiveLayer1 = carrierLayer(p.input.weight);
        m.emissiveBlendMode1 = Op::TeamColorDiffuseAdd;
        s.record(StandardLayer::Emissive1, p.ordinal);
        s.emisAdd = 1;
        s.stage = 4;
    } else if (p.team) {
        // R3: solid team colour -- the RGBA select on whatever the plate's
        // reference names (the driver hands it an alpha-0 stock).
        m.diffuseLayer = layerFrom(p.input, context);
        m.diffuseLayer->colorType = m3::ColorChannelSelect::RGBA;
        threshold = 0;
        s.teamDiffuse = true;
        s.kind = Section::Kind::Opaque;
        s.record(StandardLayer::Diffuse, p.ordinal);
    } else if (additiveBlend && nextTextured && next->additive()) {
        // R3b: no diffuse, every pass an emissive weighted by its own alpha --
        // the diffuse spelling would weight the later passes by the base's.
        s.kind = Section::Kind::AdditiveOnly;
        blend = m3::BlendMode::AlphaAdd;
        threshold = 0;
        m.emissiveLayer1 = layerFrom(p.input, context);
        m.emissiveLayer1->colorType = m3::ColorChannelSelect::RGBA;
        weightLayer(*m.emissiveLayer1, p.input.weight);
        m.emissiveBlendMode1 = Op::Add;
        s.record(StandardLayer::Emissive1, p.ordinal);
        s.emisAdd = 1;
        s.stage = 4;
    } else {
        // R1.
        m.diffuseLayer = layerFrom(p.input, context);
        s.record(StandardLayer::Diffuse, p.ordinal);
        switch (blend) {
        case m3::BlendMode::Opaque:
            s.kind = (threshold == 0 || p.alpha == TextureAlphaClass::Opaque)
                         ? Section::Kind::Opaque
                         : Section::Kind::Partial;
            break;
        case m3::BlendMode::AlphaBlend:
            s.kind = p.alpha == TextureAlphaClass::Opaque ? Section::Kind::Opaque
                                                           : Section::Kind::Partial;
            break;
        case m3::BlendMode::Add:
        case m3::BlendMode::AlphaAdd:
            s.kind = Section::Kind::Additive;
            break;
        default:
            s.kind = Section::Kind::Modulate;
            break;
        }
    }
    m.blendMode = blend;
    m.alphaTestThreshold = threshold;
    if (s.unlit) {
        m.flags |= m3::MaterialFlag::Unshaded;
    }
    if (s.twoSided) {
        m.flags |= m3::MaterialFlag::TwoSided;
    }
    if (s.unfogged) {
        m.flags |= m3::MaterialFlag::Unfogged;
    }
    reportPass(p, {}, profile, out);
    s.passOrdinals.push_back(p.ordinal);
    if (taken == 2) {
        s.passOrdinals.push_back(next->ordinal);
    }
    return taken;
}

/// The channels that are slots rather than passes -- specular, normal, the
/// masks, an explicit emissive or environment layer -- onto the first section.
/// After the passes, so a stack's own emissive homes come first.
void placeExtras(Section& s, const CompositeBody& body, const Context& context,
                 ProfileId profile, Diagnostics& out) {
    using Op = m3::LayerBlendOp;
    m3::StandardMaterial& m = s.standard;
    for (std::size_t i = 0; i < body.layers.size(); ++i) {
        const CompositeLayer& layer = body.layers[i];
        const u32 ordinal = static_cast<u32>(i);
        std::optional<m3::TextureLayer>* slot = nullptr;
        StandardLayer name = StandardLayer::Count;
        switch (layer.target) {
        case SurfaceChannel::Color:
            continue;
        case SurfaceChannel::Emissive: {
            if (!s.emissiveFree()) {
                break;
            }
            const bool first = !m.emissiveLayer1.has_value();
            slot = first ? &m.emissiveLayer1 : &m.emissiveLayer2;
            name = first ? StandardLayer::Emissive1 : StandardLayer::Emissive2;
            *slot = layerFrom(layer.input, context);
            // The op's DEFAULT is Mod -- an emissive left there multiplies the
            // lit colour, and a mostly-black glow map multiplied the Sorceress
            // to a silhouette. Written on the first fill, always.
            const Op op = layer.op == CompositeOp::Set ? Op::AddNoAlpha : blendOpFor(layer.op);
            (first ? m.emissiveBlendMode1 : m.emissiveBlendMode2) = op;
            if (modFamily(op)) {
                ++s.emisMod;
            } else {
                ++s.emisAdd;
            }
            s.record(name, ordinal);
            continue;
        }
        case SurfaceChannel::Specular:
            slot = &m.specularLayer;
            name = StandardLayer::Specular;
            break;
        case SurfaceChannel::Normal:
            slot = &m.normalLayer;
            name = StandardLayer::Normal;
            break;
        case SurfaceChannel::AmbientOcclusion:
            slot = &m.ambientOcclusionLayer;
            name = StandardLayer::AmbientOcclusion;
            break;
        case SurfaceChannel::Gloss:
            slot = &m.glossLayer;
            name = StandardLayer::Gloss;
            break;
        case SurfaceChannel::Coverage:
            if (!m.alphaLayer1.has_value()) {
                slot = &m.alphaLayer1;
                name = StandardLayer::Alpha1;
            } else if (!m.alphaLayer2.has_value()) {
                slot = &m.alphaLayer2;
                name = StandardLayer::Alpha2;
            }
            break;
        case SurfaceChannel::Environment:
            if (!m.environmentLayer.has_value()) {
                slot = &m.environmentLayer;
                name = StandardLayer::Environment;
                // ApplyEnv's op is the decal's field (psmaterial.fx:332).
                // With no decal to claim it the layer's own op names it --
                // Add on 9,877 of 10,569 shipped env materials, where the
                // struct's default Mod multiplies the lit colour by the
                // reflection and blackens whatever the mask leaves out.
                if (!s.hasDecal) {
                    m.layerBlendMode =
                        layer.op == CompositeOp::Modulate || layer.op == CompositeOp::Modulate2x
                            ? Op::Mod
                        : layer.op == CompositeOp::AlphaBlend ? Op::Lerp
                                                              : Op::Add;
                }
            } else if (!m.environmentMaskLayer.has_value() &&
                       layer.op == CompositeOp::Modulate) {
                slot = &m.environmentMaskLayer;
                name = StandardLayer::EnvironmentMask;
            }
            break;
        default:
            break;
        }
        if (slot == nullptr || slot->has_value()) {
            out.warn(DiagCode::LayerDropped,
                     std::string("a second ") + ToString(layer.target) +
                         " layer has no M3 slot to go back into",
                     ElementRef(ElementKind::Layer, ordinal), profile);
            continue;
        }
        *slot = layerFrom(layer.input, context);
        // Both read one scalar off the sample's alpha: the coverage by
        // definition, the gloss because MaterialSpecularity squares `.a`.
        if (layer.target == SurfaceChannel::Coverage || layer.target == SurfaceChannel::Gloss) {
            (*slot)->colorType = m3::ColorChannelSelect::Alpha;
        }
        if (name == StandardLayer::Environment) {
            s.hasEnv = true;
        }
        s.record(name, ordinal);
    }
}

/// The material's fresnel features. One naming a layer this section placed is
/// that layer's own term -- the import's inverse. A whole-material one, or any
/// on a Warcraft III stack (its engine has only the overlay), is Reforged's
/// rim, `lerp(lit, tint, opacity * (1 - n.v)^2)`, spelled as two solid
/// carriers: a Mod that dims the lit colour by `1 - w` and an alpha-free add
/// of `tint * w`. Retail multiplies the lit colour by a Mod-family emissive1
/// (psmaterial.fx:240) but sends a Mod emissive2 to the add accumulator once
/// emissive1 added, so beside an emissive map the dim takes the decal -- the
/// albedo before lighting, the specular undimmed. The add slots' sum is scaled
/// by the HDR multiplier, so the tint is written under it.
void placeFresnel(Section& s, const CompositeBody* body, const CommonMaterial& common,
                  const Context& context, ProfileId profile, Diagnostics& out) {
    using Op = m3::LayerBlendOp;
    m3::StandardMaterial& m = s.standard;
    const f32 hdr = body != nullptr ? (std::max)(1.0f, body->emissiveFactor.x) : 1.0f;
    for (const MaterialFeature& feature : common.features) {
        const FresnelFeature* fresnel = feature.fresnel();
        if (fresnel == nullptr) {
            continue;
        }
        const f32 exponent = fresnel->exponent > 0.0f ? fresnel->exponent : 1.0f;
        if (feature.layer != kWholeMaterial && !context.warcraftPasses) {
            bool placed = false;
            for (std::size_t k = 0; k < s.ordinals.size() && !placed; ++k) {
                std::optional<m3::TextureLayer>& slot =
                    MutableLayerOf(m, static_cast<StandardLayer>(k));
                if (s.ordinals[k] == feature.layer && slot.has_value()) {
                    fresnelRamp(*slot, exponent, fresnel->outMin, fresnel->outMax);
                    placed = true;
                }
            }
            if (!placed) {
                out.info(DiagCode::FeatureDropped,
                         "a fresnel feature names layer " + number(feature.layer) +
                             ", which no slot carries",
                         ElementRef(ElementKind::Layer, feature.layer), profile);
            }
            continue;
        }

        const f32 low = std::clamp(fresnel->outMin, 0.0f, 1.0f);
        const f32 high = std::clamp(fresnel->outMax, 0.0f, 1.0f);
        if (low <= 0.0f && high <= 0.0f) {
            continue;
        }
        if (fresnel->teamColor > 0.0f) {
            out.info(DiagCode::FeatureDropped,
                     "the rim's team share has no StarCraft II carrier; the authored tint stands",
                     ElementRef(), profile);
        }
        m3::TextureLayer dim = tintCarrier(Vector3f{1.0f, 1.0f, 1.0f}, 1.0f);
        fresnelRamp(dim, exponent, 1.0f - low, 1.0f - high);
        m3::TextureLayer rim = tintCarrier(
            Vector3f{fresnel->color.x / hdr, fresnel->color.y / hdr, fresnel->color.z / hdr},
            1.0f);
        fresnelRamp(rim, exponent, low, high);

        const bool emis1Free = !m.emissiveLayer1.has_value();
        const bool emis2Free = !m.emissiveLayer2.has_value();
        if (emis1Free && emis2Free) {
            m.emissiveLayer1 = dim;
            m.emissiveBlendMode1 = Op::Mod;
            ++s.emisMod;
            m.emissiveLayer2 = rim;
            m.emissiveBlendMode2 = Op::AddNoAlpha;
            ++s.emisAdd;
        } else if (!m.decalLayer.has_value() && (emis1Free || emis2Free) &&
                   (!s.hasEnv || m.layerBlendMode == Op::Mod)) {
            // The decal's op is the reflection's too (ApplyEnv reads the
            // same field), so an additive reflection keeps the dim off the
            // decal: the banshee's Mod dim multiplied her whole colour by an
            // F0 mask that is black on cloth.
            m.decalLayer = dim;
            m.layerBlendMode = Op::Mod;
            s.hasDecal = true;
            s.decalOp = Op::Mod;
            (emis1Free ? m.emissiveLayer1 : m.emissiveLayer2) = rim;
            (emis1Free ? m.emissiveBlendMode1 : m.emissiveBlendMode2) = Op::AddNoAlpha;
            ++s.emisAdd;
        } else if (emis1Free || (emis2Free && modFamily(m.emissiveBlendMode1))) {
            // One slot on the lit colour: a lerp toward the tint by the term
            // dims exactly and brings the tint at the term squared.
            rim.color.initValue.a = 255;
            (emis1Free ? m.emissiveLayer1 : m.emissiveLayer2) = rim;
            (emis1Free ? m.emissiveBlendMode1 : m.emissiveBlendMode2) = Op::Lerp;
            ++s.emisMod;
            out.info(DiagCode::FresnelFolded,
                     "the rim overlay is one lerp: its tint arrives at the term squared",
                     ElementRef(), profile);
        } else if (emis2Free) {
            m.emissiveLayer2 = rim;
            m.emissiveBlendMode2 = Op::AddNoAlpha;
            ++s.emisAdd;
            out.info(DiagCode::FresnelFolded,
                     "the rim overlay adds over an undimmed base: no slot was left for the dim",
                     ElementRef(), profile);
        } else {
            out.warn(DiagCode::FeatureDropped, "the rim overlay found no free emissive slot",
                     ElementRef(), profile);
        }
    }
}

/// The header fields every section shares, the multipliers, the base's own
/// coverage and static alpha.
void finish(Section& s, const CompositeBody* body, const CommonMaterial& common,
            const std::string& name, const Context& context, ProfileId profile,
            Diagnostics& out) {
    m3::StandardMaterial& m = s.standard;
    m.name = name;
    m.priority = common.priorityPlane;
    m.specularExponent = body != nullptr ? body->specularExponent : 0.0f;
    // At least one: 1,270 of 1,275 sampled shipped materials state it, 436 of
    // them with no specular layer at all. The engine scales the light's
    // specular by it CPU-side; this build's renderer guards a zero as one, the
    // game does not.
    const f32 specularMultiplier = body != nullptr ? body->specularFactor.x : 0.0f;
    m.hdrSpecularMultiplier = specularMultiplier > 0.0f ? specularMultiplier : 1.0f;
    // Warcraft III adds its passes unscaled, and the field's zero is a black
    // glow in the real engine (this build's renderer guards it, the game does
    // not).
    m.hdrEmissiveMultiplier =
        body != nullptr ? (std::max)(1.0f, body->emissiveFactor.x) : 1.0f;
    // A team glow adds the ENGINE's emissive team colour, not the source
    // texture's own red -- and that colour is authored dark (red is 0.545,
    // 0.26 de-gamma'd) where Warcraft III adds the glow texture itself (its
    // red peaks at 0.48) over two stacked quads. So the glow needs a scale the
    // source pass never carried. All 541 of the oracle's team-glow materials
    // state one: 540 say 1.5, the rest 2 to 4. Measured on the Skink hero,
    // 1.5 adds a peak of +46/255 and 3 adds +88; the user picked 2.25 between
    // them. Warcraft III stacks only, so an `.m3` round trip keeps whatever
    // its own header stated.
    const bool teamGlow =
        (m.emissiveLayer1.has_value() &&
         m.emissiveBlendMode1 == m3::LayerBlendOp::TeamColorEmissiveAdd) ||
        (m.emissiveLayer2.has_value() &&
         m.emissiveBlendMode2 == m3::LayerBlendOp::TeamColorEmissiveAdd);
    if (context.warcraftPasses && teamGlow) {
        m.hdrEmissiveMultiplier = 2.25f;
    }
    if (body != nullptr && body->environmentFactor > 0.0f) {
        m.hdrEnvironmentConstant = body->environmentFactor;
    } else if (s.hasEnv && m.hdrEnvironmentConstant <= 0.0f) {
        m.hdrEnvironmentConstant = 1.0f;
    }
    if (body != nullptr && body->simulateRoughness) {
        m.flags |= m3::MaterialFlag::SimulateRoughness;
    }

    // A keyed or blended material with no mask stated: StarCraft II tests and
    // blends by the COMPOSED alpha (`cFinal.a = mask1.a * mask2.a`), so without
    // an alpha layer nothing is ever cut. The diffuse's own alpha is the mask
    // -- Opaque + threshold 192 + an Alpha select on the diffuse's texture is,
    // texel for texel, Blizzard's spelling of a Warcraft III Transparent
    // filter, and `rgb * a` under AlphaAdd is both its additive filters. A
    // team material stays out: its alpha is the team mask.
    const bool keyed = m.blendMode == m3::BlendMode::Opaque && m.alphaTestThreshold > 0;
    const bool blends =
        m.blendMode == m3::BlendMode::AlphaBlend || m.blendMode == m3::BlendMode::AlphaAdd;
    if (!s.teamDiffuse && !s.toggle && !m.alphaLayer1.has_value() && (keyed || blends) &&
        m.diffuseLayer.has_value() && !TrimNuls(m.diffuseLayer->texturePath).empty()) {
        m.alphaLayer1 = *m.diffuseLayer;
        m.alphaLayer1->colorType = m3::ColorChannelSelect::Alpha;
        m.alphaLayer1->mapAlpha.initValue = 1.0f; // the static weight rides its own carrier
    }
    // An explicit coverage layer answers what the team select gave up, and the
    // header's own blend and threshold stand again: `psmainshading.fx` tests
    // `alphaFactor * mask`, never the diffuse's alpha.
    if (s.teamDiffuse && !s.toggle && m.alphaLayer1.has_value() &&
        m.blendMode == m3::BlendMode::Opaque && m.alphaTestThreshold == 0) {
        m.blendMode = blendFor(common.blend);
        if (m.blendMode == m3::BlendMode::Opaque) {
            m.alphaTestThreshold =
                (std::min)(255u, static_cast<u32>(common.alphaTestThreshold * 256.0f + 0.5f));
        }
    }

    // The base pass's static alpha: a Color-flag carrier whose multiply is the
    // weight -- retail multiplies rgba, so it scales the mask -- leaving its
    // `mapAlpha` for a fade track to share. A fading opaque pass is promoted to
    // a blend, as Warcraft III promotes it.
    const f32 w = std::clamp(s.baseWeight, 0.0f, 1.0f);
    const bool glowOnLayer = s.kind == Section::Kind::Glow && m.blendMode == m3::BlendMode::Opaque;
    const bool modulates =
        m.blendMode == m3::BlendMode::Mod || m.blendMode == m3::BlendMode::Mod2x;
    if (w < 0.999f && !s.teamDiffuse && s.kind != Section::Kind::AdditiveOnly && !glowOnLayer &&
        !modulates) {
        if (m.blendMode == m3::BlendMode::Opaque) {
            m.blendMode = m3::BlendMode::AlphaBlend;
        }
        std::optional<m3::TextureLayer>* slot = !m.alphaLayer1.has_value()   ? &m.alphaLayer1
                                                : !m.alphaLayer2.has_value() ? &m.alphaLayer2
                                                                             : nullptr;
        if (slot != nullptr) {
            *slot = carrierLayer(1.0f);
            (*slot)->rgbMultiply.initValue = (std::max)(w, 0.002f);
        } else {
            out.warn(DiagCode::LayerDropped,
                     "the base pass's static alpha found both alpha layers taken",
                     ElementRef(ElementKind::Layer, s.baseOrdinal), profile);
        }
    }
}

/// The header of a material that has no colour pass at all -- a slot-only body.
void headerFrom(const CommonMaterial& common, m3::StandardMaterial& m) {
    m.blendMode = blendFor(common.blend);
    m.alphaTestThreshold =
        (std::min)(255u, static_cast<u32>(common.alphaTestThreshold * 256.0f + 0.5f));
    if (common.cull == CullMode::None) {
        m.flags |= m3::MaterialFlag::TwoSided;
    }
    if (hasFlag(common.flags, MaterialFlags::Unfogged)) {
        m.flags |= m3::MaterialFlag::Unfogged;
    }
    if (hasFlag(common.flags, MaterialFlags::Unlit)) {
        m.flags |= m3::MaterialFlag::Unshaded;
    }
}

/// The fold: §5.0 normalise, then open a material and place passes into it
/// until one cannot go, then open the next on that pass. One section is a
/// StandardMaterial; more are a CMP_ over them, in pass order.
m3::MaterialMap exportPasses(std::vector<Pass> passes, const CompositeBody* body,
                             const Material& material, ProfileId profile, const Context& context,
                             m3::Model& model, Diagnostics& out, std::vector<u32>* layerOrdinals,
                             ExportReport* report) {
    const CommonMaterial& common = material.Common();

    // §5.0: a later opaque pass replaces everything below it; a team plate
    // under an opaque-alpha texture never shows.
    std::size_t base = 0;
    for (std::size_t i = 1; i < passes.size(); ++i) {
        if (passes[i].op == CompositeOp::Set && !passes[i].team && !passes[i].glow) {
            base = i;
        }
    }
    if (base > 0) {
        out.info(DiagCode::LayerDropped,
                 number(base) + " pass(es) under an opaque later pass never show; dropped",
                 ElementRef(ElementKind::Layer, 0), profile);
        passes.erase(passes.begin(), passes.begin() + static_cast<std::ptrdiff_t>(base));
    }
    if (passes.size() >= 2 && passes[0].team && !passes[1].team && !passes[1].glow &&
        passes[1].blendLike() && passes[1].alpha == TextureAlphaClass::Opaque) {
        out.info(DiagCode::TeamPlateCovered,
                 "the team plate sits under a texture whose alpha is opaque; no texel shows it",
                 ElementRef(ElementKind::Layer, passes[0].ordinal), profile);
        passes.erase(passes.begin());
        // The plate was the stack's opaque meeting with the scene; the texture
        // over it, opaque itself, inherits that rather than blending with
        // whatever is behind.
        passes[0].op = CompositeOp::Set;
    }

    std::vector<Section> sections;
    std::size_t i = 0;
    while (i < passes.size()) {
        Section s;
        i += open(s, passes, i, common, context, profile, out);
        while (i < passes.size()) {
            const Placement r = decide(s, passes[i], context);
            if (r.home == Home::Boundary) {
                if (context.compositeSections) {
                    break;
                }
                out.warn(DiagCode::LayerDropped,
                         "pass " + number(passes[i].ordinal) +
                             " has no home in a single material and the caller allows no composite",
                         ElementRef(ElementKind::Layer, passes[i].ordinal), profile);
                ++i;
                continue;
            }
            apply(s, passes[i], r, context, profile, out, sections.empty() ? report : nullptr);
            ++i;
        }
        if (sections.empty()) {
            if (body != nullptr) {
                placeExtras(s, *body, context, profile, out);
            }
            placeFresnel(s, body, common, context, profile, out);
        }
        finish(s, body, common, material.name, context, profile, out);
        sections.push_back(std::move(s));
    }
    if (sections.empty()) {
        Section s;
        headerFrom(common, s.standard);
        if (body != nullptr) {
            placeExtras(s, *body, context, profile, out);
        }
        placeFresnel(s, body, common, context, profile, out);
        finish(s, body, common, material.name, context, profile, out);
        sections.push_back(std::move(s));
    }

    const std::size_t count = static_cast<std::size_t>(StandardLayer::Count);
    if (sections.size() == 1) {
        if (layerOrdinals != nullptr) {
            *layerOrdinals = sections[0].ordinals;
        }
        const m3::MaterialMap entry =
            pushStandard(std::move(sections[0].standard), model, profile, out);
        if (report != nullptr) {
            report->standardIndices.assign(1, entry.materialIndex);
        }
        return entry;
    }

    if (layerOrdinals != nullptr) {
        layerOrdinals->assign(count * sections.size(), kInvalidIndex);
    }
    m3::CompositeMaterial composite;
    composite.name = material.name;
    composite.priority = common.priorityPlane;
    for (std::size_t k = 0; k < sections.size(); ++k) {
        const m3::MaterialMap entry =
            pushStandard(std::move(sections[k].standard), model, profile, out);
        if (report != nullptr) {
            report->standardIndices.push_back(entry.materialIndex);
        }
        m3::CompositeSection section;
        section.materialIndex =
            context.materialMapBase + static_cast<u32>(context.trailingMaps.size());
        section.mapMultiplier.initValue = 1.0f;
        context.trailingMaps.push_back(entry);
        composite.sections.push_back(section);
        if (layerOrdinals != nullptr) {
            std::copy(sections[k].ordinals.begin(), sections[k].ordinals.end(),
                      layerOrdinals->begin() + static_cast<std::ptrdiff_t>(k * count));
        }
    }
    out.info(DiagCode::CompositeEmitted,
             "the stack needed " + number(sections.size()) +
                 " materials; a composite draws them in pass order",
             ElementRef(), profile);
    m3::MaterialMap entry;
    entry.materialType = m3::MaterialType::Composite;
    entry.materialIndex = static_cast<u32>(model.compositeMaterials.size());
    model.compositeMaterials.push_back(std::move(composite));
    return entry;
}

} // namespace

m3::MaterialMap ExportMaterial(const Material& material, ProfileId profile, const Context& context,
                               m3::Model& model, Diagnostics& out,
                               std::vector<u32>* layerOrdinals, const ExportHints* hints,
                               ExportReport* report) {
    if (layerOrdinals != nullptr) {
        layerOrdinals->assign(static_cast<std::size_t>(StandardLayer::Count), kInvalidIndex);
    }
    // §7.1: a native block that is not stale IS the answer.
    if (material.hasNative() && material.sync() != NativeSync::CommonEdited &&
        material.nativeKind() == NativeKind::M3) {
        const auto& block = std::get<native::M3Material>(material.Native());
        m3::MaterialMap entry;
        entry.materialType = static_cast<m3::MaterialType>(block.kind);

        if (const auto* body = std::get_if<native::M3Standard>(&block.body)) {
            m3::StandardMaterial standard;
            CopyFromNative(*body, standard);
            return pushStandard(std::move(standard), model, profile, out);
        }
        if (const auto* body = std::get_if<native::M3DataDriven>(&block.body)) {
            m3::DataDrivenMaterial madd;
            CopyFromNative(*body, madd);
            entry.materialIndex = static_cast<u32>(model.dataDrivenMaterials.size());
            model.dataDrivenMaterials.push_back(std::move(madd));
            return entry;
        }
        if (const auto* body = std::get_if<native::M3Displacement>(&block.body)) {
            m3::DisplacementMaterial displacement;
            CopyFromNative(*body, displacement);
            entry.materialIndex = static_cast<u32>(model.displacementMaterials.size());
            model.displacementMaterials.push_back(std::move(displacement));
            return entry;
        }
        if (const auto* body = std::get_if<native::M3Composite>(&block.body)) {
            m3::CompositeMaterial composite;
            CopyFromNative(*body, composite);
            entry.materialIndex = static_cast<u32>(model.compositeMaterials.size());
            model.compositeMaterials.push_back(std::move(composite));
            return entry;
        }
        if (const auto* body = std::get_if<native::M3Terrain>(&block.body)) {
            m3::TerrainMaterial terrain;
            CopyFromNative(*body, terrain);
            entry.materialIndex = static_cast<u32>(model.terrainMaterials.size());
            model.terrainMaterials.push_back(std::move(terrain));
            return entry;
        }
        if (const auto* body = std::get_if<native::M3Volume>(&block.body)) {
            m3::VolumeMaterial volume;
            CopyFromNative(*body, volume);
            entry.materialIndex = static_cast<u32>(model.volumeMaterials.size());
            model.volumeMaterials.push_back(std::move(volume));
            return entry;
        }
        if (const auto* body = std::get_if<native::M3VolumeNoise>(&block.body)) {
            m3::VolumeNoiseMaterial noise;
            CopyFromNative(*body, noise);
            entry.materialIndex = static_cast<u32>(model.volumeNoiseMaterials.size());
            model.volumeNoiseMaterials.push_back(std::move(noise));
            return entry;
        }
        if (const auto* body = std::get_if<native::M3Creep>(&block.body)) {
            m3::CreepMaterial creep;
            CopyFromNative(*body, creep);
            entry.materialIndex = static_cast<u32>(model.creepMaterials.size());
            model.creepMaterials.push_back(std::move(creep));
            return entry;
        }
        if (const auto* body = std::get_if<native::M3Stb>(&block.body)) {
            m3::STBMaterial stb;
            CopyFromNative(*body, stb);
            entry.materialIndex = static_cast<u32>(model.stbMaterials.size());
            model.stbMaterials.push_back(std::move(stb));
            return entry;
        }
        if (const auto* body = std::get_if<native::M3Reflection>(&block.body)) {
            m3::ReflectionMaterial reflection;
            CopyFromNative(*body, reflection);
            entry.materialIndex = static_cast<u32>(model.reflectionMaterials.size());
            model.reflectionMaterials.push_back(std::move(reflection));
            return entry;
        }
        if (const auto* body = std::get_if<native::M3LensFlare>(&block.body)) {
            m3::LensFlare flare;
            CopyFromNative(*body, flare);
            entry.materialIndex = static_cast<u32>(model.lensFlareMaterials.size());
            model.lensFlareMaterials.push_back(std::move(flare));
            return entry;
        }
    }

    // The fallback path: `StandardMaterial` is the only M3 kind a generic body
    // can become, so everything projects onto it or reports. A layer stack --
    // a composite body, or a Warcraft III chain the import collapsed -- goes
    // through the pass fold, which may answer with a composite of its own.
    const CommonMaterial& common = material.Common();
    if (const CompositeBody* body = common.composite()) {
        return exportPasses(passesOf(*body, common, context, hints), body, material, profile,
                            context, model, out, layerOrdinals, report);
    }
    if (const CombinersBody* chain = common.combiners();
        chain != nullptr && context.warcraftPasses) {
        return exportPasses(passesOf(*chain, common, context, hints), nullptr, material, profile,
                            context, model, out, layerOrdinals, report);
    }

    m3::StandardMaterial standard;
    standard.name = material.name;
    standard.blendMode = blendFor(common.blend);
    // Diablo III's `_pma` family splits on the PROGRAM's mode, not the blend
    // factors — both modes ship the same pair and differ only in the alpha the
    // program writes (`TAG_VS_PMA_FUNC`, 0xA002B, on the pass). Mode 1
    // composites `C·a + dst·(1−a)` — plain AlphaBlend of the straight colour
    // this export writes — and mode 2 ends `C·a + dst`, which is AlphaAdd.
    // The generic mapping (`Add`, ONE·ONE) dropped the alpha weighting on
    // both, so a faded ghost drew at full strength (D3_TO_SC2_DESIGN.md §3).
    if (common.blend == BlendMode::PremultipliedAlpha) {
        standard.blendMode = m3::BlendMode::AlphaBlend;
        if (const auto* d3 = std::get_if<native::D3Material>(&material.Native());
            d3 != nullptr && material.NativeIsAuthoritative() && !d3->opaquePasses.empty()) {
            for (const native::D3ShaderTagValue& tag : d3->opaquePasses.front().shaderParams) {
                if (tag.tagId == 0xA002Bu && tag.value == 2u) {
                    standard.blendMode = m3::BlendMode::AlphaAdd;
                }
            }
        }
    }
    standard.priority = common.priorityPlane;
    standard.alphaTestThreshold =
        (std::min)(255u, static_cast<u32>(common.alphaTestThreshold * 256.0f + 0.5f));
    if (common.cull == CullMode::None) {
        standard.flags |= m3::MaterialFlag::TwoSided;
    }
    if (hasFlag(common.flags, MaterialFlags::Unfogged)) {
        standard.flags |= m3::MaterialFlag::Unfogged;
    }
    if (hasFlag(common.flags, MaterialFlags::Unlit)) {
        standard.flags |= m3::MaterialFlag::Unshaded;
    }

    if (const CombinersBody* chain = common.combiners()) {
        // WoW fades a plain-additive batch through the COLOUR product (the
        // element alpha multiplies what draws, `reference_m2_element_alpha`),
        // so its ONE,ONE add still fades. M3's Add ignores alpha outright;
        // AlphaAdd is the blend that lets the mask carry the fade, and with
        // no mask present the composed alpha is 1 and the two are identical.
        if (common.blend == BlendMode::Additive) {
            standard.blendMode = m3::BlendMode::AlphaAdd;
        }
        standard.specularExponent = 20.0f;
        standard.hdrSpecularMultiplier = 1.0f; // the shipped floor
        standard.hdrEmissiveMultiplier = 0.0f;
        standard.hdrEnvironmentConstant = 0.0f;
        exportCombiners(*chain, common, context, profile, standard, out, layerOrdinals);
    } else if (const LegacyDeferredBody* legacy = common.legacy()) {
        exportLegacy(*legacy, common, context, profile, standard, out, layerOrdinals);
    } else {
        out.warn(DiagCode::LossyKindConversion,
                 std::string("an M3 export flattens a ") + ToString(common.kind()) +
                     " material onto a StandardMaterial",
                 ElementRef(), profile);
    }

    const m3::MaterialMap entry = pushStandard(std::move(standard), model, profile, out);
    if (report != nullptr) {
        report->standardIndices.assign(1, entry.materialIndex);
    }
    return entry;
}

} // namespace m3_core
} // namespace wem
} // namespace models
} // namespace whiteout
