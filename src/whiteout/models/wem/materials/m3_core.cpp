// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "m3_core.h"

#include "../native/m3_copy.h"

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
    input.uvTransform.m[0][2] = layer.uvOffset.initValue.x;
    input.uvTransform.m[1][2] = layer.uvOffset.initValue.y;
    if (layer.uvTiling.initValue.x != 0.0f) {
        input.uvTransform.m[0][0] = layer.uvTiling.initValue.x;
    }
    if (layer.uvTiling.initValue.y != 0.0f) {
        input.uvTransform.m[1][1] = layer.uvTiling.initValue.y;
    }
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

    reportDropped(source.glossLayer, "glossLayer", "gloss is a LegacySlot, not a channel", out);
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
        // entirely (§7.2.1). There is nothing to project: the sections name
        // materials, not textures.
        common.body = CompositeBody{};
        out.warn(DiagCode::LossyKindConversion,
                 "an M3 CMP_ material blends " + number(source.sections.size()) +
                     " other materials by weight; that is not a layer stack and has no common "
                     "projection");
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
m3::MaterialMap pushStandard(m3::StandardMaterial standard, m3::Model& model) {
    m3::MaterialMap entry;
    entry.materialType = m3::MaterialType::Standard;
    entry.materialIndex = static_cast<u32>(model.standardMaterials.size());
    model.standardMaterials.push_back(std::move(standard));
    return entry;
}

/// The one layer slot a common channel maps back onto. `Composite` is a stack
/// over channels and `StandardMaterial` is a fixed set of named slots, so the
/// projection back keeps the FIRST layer of each channel and reports the rest:
/// two diffuse layers folded with an op have one slot to go back into.
std::optional<m3::TextureLayer>* slotFor(SurfaceChannel channel, m3::StandardMaterial& dst) {
    switch (channel) {
    case SurfaceChannel::Color:
        return &dst.diffuseLayer;
    case SurfaceChannel::Emissive:
        return &dst.emissiveLayer1;
    case SurfaceChannel::Specular:
        return &dst.specularLayer;
    case SurfaceChannel::Normal:
        return &dst.normalLayer;
    case SurfaceChannel::AmbientOcclusion:
        return &dst.ambientOcclusionLayer;
    case SurfaceChannel::Environment:
        return &dst.environmentLayer;
    case SurfaceChannel::Coverage:
        return &dst.alphaLayer1;
    default:
        return nullptr;
    }
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
    layer.uvOffset.initValue = Vector2f(input.uvTransform.m[0][2], input.uvTransform.m[1][2]);
    layer.uvTiling.initValue = Vector2f(input.uvTransform.m[0][0], input.uvTransform.m[1][1]);
    layer.mapAlpha.initValue = input.weight;
    // The multiply rests at ONE. The struct's default is zero, and a zero
    // multiply is a black layer in the real engine — the same trap the MADD
    // restore hit (`reference_madd_restore_defaults`).
    layer.rgbMultiply.initValue = 1.0f;
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

/// The slot a composite channel's FIRST layer projects onto — `slotFor`'s twin
/// in the ordinal vocabulary, for the map `ExportMaterial` reports.
StandardLayer standardLayerFor(SurfaceChannel channel) {
    switch (channel) {
    case SurfaceChannel::Color:
        return StandardLayer::Diffuse;
    case SurfaceChannel::Emissive:
        return StandardLayer::Emissive1;
    case SurfaceChannel::Specular:
        return StandardLayer::Specular;
    case SurfaceChannel::Normal:
        return StandardLayer::Normal;
    case SurfaceChannel::AmbientOcclusion:
        return StandardLayer::AmbientOcclusion;
    case SurfaceChannel::Environment:
        return StandardLayer::Environment;
    case SurfaceChannel::Coverage:
        return StandardLayer::Alpha1;
    default:
        return StandardLayer::Count;
    }
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
    standard.hdrSpecularMultiplier = 0.0f;
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

} // namespace

m3::MaterialMap ExportMaterial(const Material& material, ProfileId profile, const Context& context,
                               m3::Model& model, Diagnostics& out,
                               std::vector<u32>* layerOrdinals) {
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
            return pushStandard(std::move(standard), model);
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
    // can become, so everything projects onto it or reports.
    const CommonMaterial& common = material.Common();
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

    if (const CompositeBody* body = common.composite()) {
        standard.specularExponent = body->specularExponent;
        standard.hdrSpecularMultiplier = body->specularFactor.x;
        standard.hdrEmissiveMultiplier = body->emissiveFactor.x;
        standard.hdrEnvironmentConstant = body->environmentFactor;

        // ---- the two Warcraft III team conventions, the oracle's way ------
        //
        // Blizzard's own conversions (`mods/war3.sc2mod`) settle both:
        //
        // * A replaceable-1 layer UNDER a keyed diffuse becomes ONE diffuse
        //   layer with the RGBA select — the texture's alpha rides along and
        //   the standard shader shows the team colour where it is low, which
        //   is exactly WC3's under-layer. The alpha crosses UNINVERTED
        //   (measured: `war3_footman.dds` alpha == the BLP's, texel for
        //   texel). No coverage layer: the same alpha is the team mask.
        // * A replaceable-2 layer (team glow) becomes an ADDITIVE emissive
        //   whose op is `TeamColorEmissiveAdd` with a RED channel select —
        //   the texture is the mask and the team colour supplies the RGB.
        bool teamDiffuse = false;
        for (std::size_t i = 0; i < body->layers.size(); ++i) {
            const CompositeLayer& layer = body->layers[i];
            if (layer.target != SurfaceChannel::Color ||
                !isReplaceable(layer.input, context, 1)) {
                continue;
            }
            for (std::size_t j = i + 1; j < body->layers.size(); ++j) {
                const CompositeLayer& over = body->layers[j];
                if (over.target == SurfaceChannel::Color && over.input.hasTexture() &&
                    (over.op == CompositeOp::AlphaKey || over.op == CompositeOp::AlphaBlend)) {
                    teamDiffuse = true;
                    break;
                }
            }
            break;
        }

        const auto record = [&](StandardLayer slot, std::size_t ordinal) {
            if (layerOrdinals != nullptr) {
                (*layerOrdinals)[static_cast<std::size_t>(slot)] = static_cast<u32>(ordinal);
            }
        };
        for (std::size_t ordinal = 0; ordinal < body->layers.size(); ++ordinal) {
            const CompositeLayer& layer = body->layers[ordinal];
            // The team under-layer itself never lands in a slot — the RGBA
            // select on the real diffuse says everything it said. Its
            // unshaded-ness belonged to it alone, so it must not leave the
            // whole material unlit.
            if (teamDiffuse && layer.target == SurfaceChannel::Color &&
                isReplaceable(layer.input, context, 1)) {
                standard.flags &= ~m3::MaterialFlag::Unshaded;
                continue;
            }
            // The team glow.
            if (layer.target == SurfaceChannel::Color &&
                isReplaceable(layer.input, context, 2)) {
                if (!standard.emissiveLayer1.has_value()) {
                    standard.emissiveLayer1 = layerFrom(layer.input, context);
                    standard.emissiveLayer1->colorType = m3::ColorChannelSelect::Red;
                    standard.emissiveBlendMode1 = m3::LayerBlendOp::TeamColorEmissiveAdd;
                    record(StandardLayer::Emissive1, ordinal);
                }
                continue;
            }
            std::optional<m3::TextureLayer>* slot = slotFor(layer.target, standard);
            if (slot == nullptr) {
                continue;
            }
            if (slot->has_value()) {
                // A second layer on a channel that has one slot. An additive
                // second COLOUR layer is a glow in Warcraft III terms, and the
                // emissive slot is where Blizzard's own conversions put it;
                // everything else reports rather than overwriting.
                if (layer.target == SurfaceChannel::Color &&
                    (layer.op == CompositeOp::Add || layer.op == CompositeOp::AddAlpha) &&
                    !standard.emissiveLayer1.has_value()) {
                    standard.emissiveLayer1 = layerFrom(layer.input, context);
                    standard.emissiveBlendMode1 = blendOpFor(layer.op);
                    record(StandardLayer::Emissive1, ordinal);
                    continue;
                }
                if (layer.target == SurfaceChannel::Emissive &&
                    !standard.emissiveLayer2.has_value()) {
                    standard.emissiveLayer2 = layerFrom(layer.input, context);
                    standard.emissiveBlendMode2 = blendOpFor(layer.op);
                    record(StandardLayer::Emissive2, ordinal);
                    continue;
                }
                if (layer.target == SurfaceChannel::Coverage &&
                    !standard.alphaLayer2.has_value()) {
                    standard.alphaLayer2 = layerFrom(layer.input, context);
                    standard.alphaLayer2->colorType = m3::ColorChannelSelect::Alpha;
                    record(StandardLayer::Alpha2, ordinal);
                    continue;
                }
                // The keyed diffuse over a team layer replaces the team
                // texture in the diffuse slot and turns the RGBA select on.
                if (teamDiffuse && layer.target == SurfaceChannel::Color &&
                    (layer.op == CompositeOp::AlphaKey || layer.op == CompositeOp::AlphaBlend)) {
                    *slot = layerFrom(layer.input, context);
                    (*slot)->colorType = m3::ColorChannelSelect::RGBA;
                    record(StandardLayer::Diffuse, ordinal);
                    // The alpha is the team mask here, not coverage; the test
                    // would cut the team regions out.
                    standard.alphaTestThreshold = 0;
                    continue;
                }
                out.warn(DiagCode::LayerDropped,
                         std::string("a second ") + ToString(layer.target) +
                             " layer has no M3 slot to go back into",
                         ElementRef(), profile);
                continue;
            }
            *slot = layerFrom(layer.input, context);
            record(standardLayerFor(layer.target), ordinal);
            if (layer.target == SurfaceChannel::Coverage) {
                (*slot)->colorType = m3::ColorChannelSelect::Alpha;
            }
            // The emissive slot's op rides a separate field, and its DEFAULT
            // is Mod -- an emissive left there MULTIPLIES the lit colour, and
            // a mostly-black glow map multiplied the Sorceress to a
            // silhouette. Record the op on the first fill, not only when the
            // slot spills to emissiveLayer2.
            if (layer.target == SurfaceChannel::Emissive) {
                standard.emissiveBlendMode1 = blendOpFor(layer.op);
            }
            // With the team under-layer skipped the keyed diffuse lands here,
            // in the EMPTY slot -- same treatment as the occupied branch: the
            // RGBA select says team, the test would cut the team regions out,
            // and its op is the stack's meeting with the team layer, not a
            // decal op.
            if (teamDiffuse && layer.target == SurfaceChannel::Color &&
                (layer.op == CompositeOp::AlphaKey || layer.op == CompositeOp::AlphaBlend)) {
                (*slot)->colorType = m3::ColorChannelSelect::RGBA;
                standard.alphaTestThreshold = 0;
                continue;
            }
            if (layer.target == SurfaceChannel::Color && layer.op != CompositeOp::Set) {
                standard.layerBlendMode = blendOpFor(layer.op);
            }
        }

        // A keyed or blended material with no mask stated: StarCraft II tests
        // and blends by the COMPOSED alpha (`cFinal.a = mask1.a * mask2.a`),
        // so without an alpha layer nothing is ever cut. The diffuse's own
        // alpha is the mask — Opaque + threshold 192 + an Alpha select on the
        // diffuse's texture is, texel for texel, Blizzard's own spelling of a
        // Warcraft III Transparent filter. A team material stays out: its
        // alpha is the team mask.
        if (!teamDiffuse && !standard.alphaLayer1.has_value() &&
            (common.blend == BlendMode::AlphaKey || common.blend == BlendMode::Transparent ||
             common.blend == BlendMode::AlphaBlend) &&
            standard.diffuseLayer.has_value() && !standard.diffuseLayer->texturePath.empty()) {
            standard.alphaLayer1 = *standard.diffuseLayer;
            standard.alphaLayer1->colorType = m3::ColorChannelSelect::Alpha;
        }
    } else if (const CombinersBody* chain = common.combiners()) {
        // WoW fades a plain-additive batch through the COLOUR product (the
        // element alpha multiplies what draws, `reference_m2_element_alpha`),
        // so its ONE,ONE add still fades. M3's Add ignores alpha outright;
        // AlphaAdd is the blend that lets the mask carry the fade, and with
        // no mask present the composed alpha is 1 and the two are identical.
        if (common.blend == BlendMode::Additive) {
            standard.blendMode = m3::BlendMode::AlphaAdd;
        }
        standard.specularExponent = 20.0f;
        standard.hdrSpecularMultiplier = 0.0f;
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

    return pushStandard(std::move(standard), model);
}

} // namespace m3_core
} // namespace wem
} // namespace models
} // namespace whiteout
