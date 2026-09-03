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
// `importStandard`, in declaration order. `Gloss`, `Height`, `Lightmap`, the
// normal-blend pair and the two alpha masks have no `SurfaceChannel` at all —
// that asymmetry is why `Flatten` (§7.2.3) is one-directional — so they are
// reported as dropped and stay in the native block, which is what the block is
// for.

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
    case m3::LayerBlendOp::Add:
    case m3::LayerBlendOp::AddNoAlpha:
    case m3::LayerBlendOp::TeamColorEmissiveAdd:
    case m3::LayerBlendOp::TeamColorDiffuseAdd:
        return CompositeOp::Add;
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
    case CompositeOp::Add:
    case CompositeOp::AddAlpha:
        return m3::LayerBlendOp::Add;
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
    case BlendMode::AlphaBlend:
    case BlendMode::Transparent:
    case BlendMode::AlphaKey:
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

    reportDropped(source.glossLayer, "glossLayer", "gloss is a LegacySlot, not a channel", out);
    reportDropped(source.heightLayer, "heightLayer", "parallax is not a surface channel", out);
    reportDropped(source.lightMapLayer, "lightMapLayer", "baked light is not a channel", out);
    reportDropped(source.alphaLayer1, "alphaLayer1",
                  "an alpha mask modulates, it is not a "
                  "channel",
                  out);
    reportDropped(source.alphaLayer2, "alphaLayer2",
                  "an alpha mask modulates, it is not a "
                  "channel",
                  out);
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
    default:
        return nullptr;
    }
}

m3::TextureLayer layerFrom(const TextureInput& input, const Context& context) {
    m3::TextureLayer layer;
    layer.texturePath = context.toPath(input.texture);
    switch (input.mapping) {
    case UVMappingMode::EnvCube:
        layer.uvMapping = m3::UVMappingMode::CubicEnvio;
        break;
    case UVMappingMode::EnvSphere:
        layer.uvMapping = m3::UVMappingMode::SphericalEnvio;
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
    return layer;
}

} // namespace

m3::MaterialMap ExportMaterial(const Material& material, ProfileId profile, const Context& context,
                               m3::Model& model, Diagnostics& out) {
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
    standard.priority = common.priorityPlane;
    standard.alphaTestThreshold = static_cast<u32>(common.alphaTestThreshold * 255.0f);
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
        for (const CompositeLayer& layer : body->layers) {
            std::optional<m3::TextureLayer>* slot = slotFor(layer.target, standard);
            if (slot == nullptr) {
                continue;
            }
            if (slot->has_value()) {
                // A second layer on a channel that has one slot. `emissiveLayer2`
                // is the only place M3 has room for one, so everything else
                // reports rather than overwriting what is already there.
                if (layer.target == SurfaceChannel::Emissive &&
                    !standard.emissiveLayer2.has_value()) {
                    standard.emissiveLayer2 = layerFrom(layer.input, context);
                    standard.emissiveBlendMode2 = blendOpFor(layer.op);
                    continue;
                }
                out.warn(DiagCode::LayerDropped,
                         std::string("a second ") + ToString(layer.target) +
                             " layer has no M3 slot to go back into",
                         ElementRef(), profile);
                continue;
            }
            *slot = layerFrom(layer.input, context);
            if (layer.target == SurfaceChannel::Color && layer.op != CompositeOp::Set) {
                standard.layerBlendMode = blendOpFor(layer.op);
            }
        }
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
