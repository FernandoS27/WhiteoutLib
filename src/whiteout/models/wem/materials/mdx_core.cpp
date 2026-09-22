// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "mdx_core.h"

#include "../native/mdx_copy.h"

#include <algorithm>
#include <optional>
#include <string>

namespace whiteout {
namespace models {
namespace wem {
namespace mdx_core {

namespace {

using mdx::Layer;

std::string number(u64 value) {
    return std::to_string(value);
}

ElementRef layerRef(u32 ordinal) {
    return ElementRef(ElementKind::Layer, ordinal);
}

// ── the §7.2.1 split ────────────────────────────────────────────────────────

/// The FIRST layer's filter mode: how the stack meets the scene.
BlendMode headerBlendFor(Layer::FilterMode mode) {
    switch (mode) {
    case Layer::FilterMode::None:
        return BlendMode::Opaque;
    case Layer::FilterMode::Transparent:
        return BlendMode::Transparent;
    case Layer::FilterMode::Blend:
        return BlendMode::AlphaBlend;
    case Layer::FilterMode::Additive:
        return BlendMode::Additive;
    case Layer::FilterMode::AddAlpha:
        return BlendMode::AdditiveAlpha;
    case Layer::FilterMode::Modulate:
        return BlendMode::Modulate;
    case Layer::FilterMode::Modulate2x:
        return BlendMode::Modulate2x;
    default:
        return BlendMode::Opaque;
    }
}

Layer::FilterMode filterModeFor(BlendMode blend) {
    switch (blend) {
    case BlendMode::Opaque:
        return Layer::FilterMode::None;
    case BlendMode::Transparent:
    case BlendMode::AlphaKey:
        return Layer::FilterMode::Transparent;
    case BlendMode::AlphaBlend:
        return Layer::FilterMode::Blend;
    case BlendMode::Additive:
        return Layer::FilterMode::Additive;
    case BlendMode::AdditiveAlpha:
    case BlendMode::BlendAdd:
    case BlendMode::PremultipliedAlpha:
        return Layer::FilterMode::AddAlpha;
    case BlendMode::Modulate:
        return Layer::FilterMode::Modulate;
    case BlendMode::Modulate2x:
        return Layer::FilterMode::Modulate2x;
    default:
        return Layer::FilterMode::None;
    }
}

/// A LATER layer's filter mode: a compositing op, 1:1.
CompositeOp compositeOpFor(Layer::FilterMode mode) {
    switch (mode) {
    case Layer::FilterMode::None:
        return CompositeOp::Set;
    case Layer::FilterMode::Transparent:
        return CompositeOp::AlphaKey;
    case Layer::FilterMode::Blend:
        return CompositeOp::AlphaBlend;
    case Layer::FilterMode::Additive:
        return CompositeOp::Add;
    case Layer::FilterMode::AddAlpha:
        return CompositeOp::AddAlpha;
    case Layer::FilterMode::Modulate:
        return CompositeOp::Modulate;
    case Layer::FilterMode::Modulate2x:
        return CompositeOp::Modulate2x;
    default:
        return CompositeOp::Set;
    }
}

Layer::FilterMode filterModeFor(CompositeOp op) {
    switch (op) {
    case CompositeOp::Set:
        return Layer::FilterMode::None;
    case CompositeOp::AlphaKey:
        return Layer::FilterMode::Transparent;
    case CompositeOp::AlphaBlend:
        return Layer::FilterMode::Blend;
    case CompositeOp::Add:
        return Layer::FilterMode::Additive;
    case CompositeOp::AddAlpha:
        return Layer::FilterMode::AddAlpha;
    case CompositeOp::Modulate:
        return Layer::FilterMode::Modulate;
    case CompositeOp::Modulate2x:
        return Layer::FilterMode::Modulate2x;
    default:
        return Layer::FilterMode::None;
    }
}

/// The §7.2.2 collapse, per later layer. Only these three never let a pass see
/// the framebuffer as anything but the previous layer's output.
bool collapsibleOp(Layer::FilterMode mode, CombinerOp& op) {
    switch (mode) {
    case Layer::FilterMode::Modulate:
        op = CombinerOp::Mod;
        return true;
    case Layer::FilterMode::Modulate2x:
        op = CombinerOp::Mod2x;
        return true;
    case Layer::FilterMode::Additive:
        op = CombinerOp::Add;
        return true;
    case Layer::FilterMode::AddAlpha:
        op = CombinerOp::AddAlpha;
        return true;
    default:
        return false;
    }
}

PbrSlot pbrSlotFor(Layer::SlotType slot) {
    switch (slot) {
    case Layer::SlotType::DiffuseMap:
        return PbrSlot::BaseColor;
    case Layer::SlotType::NormalMap:
        return PbrSlot::Normal;
    case Layer::SlotType::ORMMap:
        return PbrSlot::Orm;
    case Layer::SlotType::EmissiveMap:
        return PbrSlot::Emissive;
    case Layer::SlotType::TeamColor:
        return PbrSlot::TeamColorMask;
    case Layer::SlotType::EnvironmentMap:
        return PbrSlot::Environment;
    default:
        return PbrSlot::BaseColor;
    }
}

Layer::SlotType slotTypeFor(PbrSlot slot) {
    switch (slot) {
    case PbrSlot::BaseColor:
        return Layer::SlotType::DiffuseMap;
    case PbrSlot::Normal:
        return Layer::SlotType::NormalMap;
    case PbrSlot::Orm:
        return Layer::SlotType::ORMMap;
    case PbrSlot::Emissive:
        return Layer::SlotType::EmissiveMap;
    case PbrSlot::TeamColorMask:
        return Layer::SlotType::TeamColor;
    case PbrSlot::Environment:
        return Layer::SlotType::EnvironmentMap;
    default:
        return Layer::SlotType::Unknown;
    }
}

// ── shared bits ─────────────────────────────────────────────────────────────

std::vector<const Layer*> layersFor(const mdx::Material& material, ProfileId profile,
                                    u32 modelVersion) {
    std::vector<const Layer*> out;
    for (const Layer& layer : material.layers) {
        if (LayerAllowedIn(material, layer, modelVersion, profile)) {
            out.push_back(&layer);
        }
    }
    return out;
}

TextureInput inputFor(const Layer& layer, u32 mdxTextureId, const Context& context, u32 ordinal,
                      Diagnostics& out) {
    TextureInput input;
    input.texture = context.toDocument(mdxTextureId);
    if (input.texture == kInvalidIndex) {
        out.warn(DiagCode::TextureUnresolved,
                 "layer names texture " + number(mdxTextureId) +
                     ", which the document has no "
                     "entry for",
                 layerRef(ordinal));
    }
    input.uvSet = layer.coordId;
    input.weight = layer.alpha;
    if (hasFlag(layer.shadingFlags, Layer::ShadingFlag::SphereEnvMap)) {
        input.mapping = UVMappingMode::EnvSphere;
    }
    // The address mode is the TEXTURE's word (`TEXS` 0x1 wrap U, 0x2 wrap V):
    // the game's adapter and ours read `tex.flags & 0x3` and nothing off the
    // layer, whose own bits every Reforged file leaves clear while its
    // textures wrap. Only a context without a table falls back to the layer.
    u32 wrap = static_cast<u32>(layer.shadingFlags) & 0xCu;
    if (context.textureRefs != nullptr) {
        wrap = input.texture < context.textureRefs->size()
                   ? ((*context.textureRefs)[input.texture].flags & 0x3u) << 2u
                   : 0u;
    }
    input.wrapU = (wrap & static_cast<u32>(Layer::ShadingFlag::WrapWidth)) != 0
                      ? WrapMode::Repeat
                      : WrapMode::Clamp;
    input.wrapV = (wrap & static_cast<u32>(Layer::ShadingFlag::WrapHeight)) != 0
                      ? WrapMode::Repeat
                      : WrapMode::Clamp;
    return input;
}

/// The material header, read off the material flags plus the FIRST layer's
/// shading flags — the header is per material and the flags are per layer, so
/// the first layer is the one that decides and a disagreement diagnoses.
void applyHeader(const mdx::Material& material, const std::vector<const Layer*>& layers,
                 CommonMaterial& common, Diagnostics& out) {
    common.priorityPlane = material.priorityPlane;

    if (hasFlag(material.flags, mdx::Material::Flag::TwoSided)) {
        common.cull = CullMode::None;
    }
    if (hasFlag(material.flags, mdx::Material::Flag::Unfogged)) {
        common.flags |= MaterialFlags::Unfogged;
    }
    if (hasFlag(material.flags, mdx::Material::Flag::SortPrimsNearZ)) {
        common.flags |= MaterialFlags::SortNearZ;
    }
    if (hasFlag(material.flags, mdx::Material::Flag::SortPrimsFarZ)) {
        common.flags |= MaterialFlags::SortFarZ;
    }
    if (layers.empty()) {
        return;
    }

    const Layer& first = *layers.front();
    const auto shading = first.shadingFlags;
    if (hasFlag(shading, Layer::ShadingFlag::TwoSided)) {
        common.cull = CullMode::None;
    }
    if (hasFlag(shading, Layer::ShadingFlag::Unfogged)) {
        common.flags |= MaterialFlags::Unfogged;
    }
    if (hasFlag(shading, Layer::ShadingFlag::NoDepthTest)) {
        common.depth.test = false;
    }
    if (hasFlag(shading, Layer::ShadingFlag::NoDepthSet)) {
        common.depth.write = false;
    }

    const bool firstUnshaded = hasFlag(shading, Layer::ShadingFlag::Unshaded) ||
                               hasFlag(shading, Layer::ShadingFlag::Unlit);
    if (firstUnshaded) {
        common.flags |= MaterialFlags::Unlit;
    }

    // WEM's contract is fold-then-light, one lighting decision for the assembled
    // surface. A stack that lights some layers and not others cannot be read
    // that way; the per-layer flags stay in the native block, and this says so.
    for (std::size_t i = 1; i < layers.size(); ++i) {
        const auto later = layers[i]->shadingFlags;
        const bool unshaded = hasFlag(later, Layer::ShadingFlag::Unshaded) ||
                              hasFlag(later, Layer::ShadingFlag::Unlit);
        if (unshaded != firstUnshaded) {
            out.info(DiagCode::MixedShadedUnshadedStack,
                     "layer " + number(i) + (unshaded ? " is unshaded" : " is shaded") +
                         " and the first layer is not; WEM lights the assembled surface once and "
                         "the layer's own decision rides a LayerShading feature",
                     layerRef(static_cast<u32>(i)));
            break;
        }
    }
}

void addFresnel(const Layer& layer, u32 ordinal, CommonMaterial& common) {
    if (layer.fresnelOpacity == 0.0f && layer.fresnelTeamColor == 0.0f) {
        return;
    }
    FresnelFeature fresnel;
    fresnel.color = layer.fresnelColor;
    // MDX carries no exponent because the shader's is fixed: Reforged's overlay
    // is `opacity * (1 - n.v)^2` (fresnel_overlay.slang), so the square is the
    // feature's exponent, and `fresnelOpacity` is the effect's strength -- an
    // output ceiling, which is what `outMax` is (§7.2.5).
    fresnel.exponent = 2.0f;
    fresnel.outMin = 0.0f;
    fresnel.outMax = layer.fresnelOpacity;
    fresnel.teamColor = layer.fresnelTeamColor;

    MaterialFeature feature;
    feature.id = NextFeatureId(common.features);
    feature.layer = ordinal;
    feature.payload = fresnel;
    common.features.push_back(feature);
}

/// The per-layer shading bits the header cannot hold, as a `LayerShading`
/// feature on every later layer that disagrees with it (features.h). Absolute
/// values, so a reader needs no header to interpret them; only the layers that
/// differ carry one, so a plain stack carries none.
void attachLayerShading(const mdx::Material& material, const std::vector<const Layer*>& layers,
                        CommonMaterial& common) {
    const bool headerUnlit = hasFlag(common.flags, MaterialFlags::Unlit);
    const bool headerTwoSided = common.cull == CullMode::None;
    const bool headerUnfogged = hasFlag(common.flags, MaterialFlags::Unfogged);
    const bool materialTwoSided = hasFlag(material.flags, mdx::Material::Flag::TwoSided);
    const bool materialUnfogged = hasFlag(material.flags, mdx::Material::Flag::Unfogged);
    for (std::size_t i = 1; i < layers.size(); ++i) {
        const auto bits = layers[i]->shadingFlags;
        LayerShadingFeature shading;
        shading.unlit = hasFlag(bits, Layer::ShadingFlag::Unshaded) ||
                        hasFlag(bits, Layer::ShadingFlag::Unlit);
        shading.twoSided = materialTwoSided || hasFlag(bits, Layer::ShadingFlag::TwoSided);
        shading.unfogged = materialUnfogged || hasFlag(bits, Layer::ShadingFlag::Unfogged);
        shading.noDepthTest = hasFlag(bits, Layer::ShadingFlag::NoDepthTest);
        shading.noDepthWrite = hasFlag(bits, Layer::ShadingFlag::NoDepthSet);
        if (shading.unlit == headerUnlit && shading.twoSided == headerTwoSided &&
            shading.unfogged == headerUnfogged && shading.noDepthTest == !common.depth.test &&
            shading.noDepthWrite == !common.depth.write) {
            continue;
        }
        MaterialFeature feature;
        feature.id = NextFeatureId(common.features);
        feature.layer = static_cast<u32>(i);
        feature.payload = shading;
        common.features.push_back(feature);
    }
}

// ── import: the classic stack ───────────────────────────────────────────────

/// The texture an SD layer actually samples. **In memory the id lives in the
/// sub-texture array**: `Parser::upgradeModel` moves `textureId` into
/// `subTextures[0]` and zeroes it, so reading the field straight gave texture
/// 0 for every layer of every classic material -- the same Arthas trap
/// `exportFromNative` documents, alive on the import side. Every replaceable
/// (team colour, team glow) vanished this way: they are never texture 0.
u32 sdTextureId(const Layer& layer) {
    return layer.subTextures.empty() ? layer.textureId : layer.subTextures.front().textureId;
}

void importComposite(const std::vector<const Layer*>& layers, const Context& context,
                     CommonMaterial& common, Diagnostics& out) {
    CompositeBody body;
    for (std::size_t i = 0; i < layers.size(); ++i) {
        const Layer& layer = *layers[i];
        CompositeLayer entry;
        entry.input = inputFor(layer, sdTextureId(layer), context, static_cast<u32>(i), out);
        entry.target = SurfaceChannel::Color;
        // The first layer is the stack's meeting with the scene, not an op.
        entry.op = i == 0 ? CompositeOp::Set : compositeOpFor(layer.filterMode);
        body.layers.push_back(std::move(entry));
        addFresnel(layer, static_cast<u32>(i), common);
    }
    common.body = std::move(body);
}

void importCombiners(const std::vector<const Layer*>& layers, const Context& context,
                     CommonMaterial& common, Diagnostics& out) {
    CombinersBody body;
    for (std::size_t i = 0; i < layers.size(); ++i) {
        const Layer& layer = *layers[i];
        CombinerStage stage;
        stage.input = inputFor(layer, sdTextureId(layer), context, static_cast<u32>(i), out);
        stage.rgb = CombinerOp::Opaque;
        if (i != 0) {
            collapsibleOp(layer.filterMode, stage.rgb);
        }
        stage.alpha = stage.rgb;
        body.stages.push_back(std::move(stage));
        addFresnel(layer, static_cast<u32>(i), common);
    }
    common.body = std::move(body);
}

// ── import: the Reforged slot map ───────────────────────────────────────────

void importPbr(const std::vector<const Layer*>& layers, const Context& context,
               CommonMaterial& common, Diagnostics& out, std::vector<u32>& ordinalOfLayer) {
    PbrDeferredBody body;

    for (std::size_t i = 0; i < layers.size(); ++i) {
        const Layer& layer = *layers[i];
        const u32 ordinal = static_cast<u32>(i);

        if (!layer.subTextures.empty()) {
            // v1200+: the slots name themselves.
            for (const Layer::SubTexture& sub : layer.subTextures) {
                TextureInput input = inputFor(layer, sub.textureId, context, ordinal, out);
                body.set(pbrSlotFor(sub.slot), input);
            }
        } else if (layers.size() == kHdPositionalSlotCount) {
            // 900..1100: no sub-texture array, so the layer's position in the HD
            // stack IS the slot, in `SlotType` order. Only asserted when the
            // count matches — a stack of some other length is not that
            // convention and guessing at it would invent slots.
            body.set(pbrSlotFor(static_cast<Layer::SlotType>(i)),
                     inputFor(layer, layer.textureId, context, ordinal, out));
        } else {
            body.set(i == 0 ? PbrSlot::BaseColor : PbrSlot::Emissive,
                     inputFor(layer, layer.textureId, context, ordinal, out));
            out.warn(DiagCode::LayerDropped,
                     "HD layer " + number(i) + " has no sub-textures and the stack is not the " +
                         number(kHdPositionalSlotCount) +
                         "-layer positional convention; its "
                         "slot is a guess",
                     layerRef(ordinal));
        }
    }

    // A layer's ordinal is its position in the HD stack. It was where the
    // layer's first slot landed in the body, which is the same thing for the
    // one-layer stack nearly every model has — but a second layer re-sets the
    // slots the first one set, so its first slot landed one past the end: an
    // ordinal the material does not have, which `Validate` rejects and the
    // export dropped. The position is also what the native block is read back
    // by, and what each layer's fresnel feature already carried.
    const u32 ordinals = static_cast<u32>(body.slots.size());
    for (std::size_t i = 0; i < layers.size(); ++i) {
        const u32 ordinal = static_cast<u32>(i);
        if (ordinal >= ordinals) {
            out.warn(DiagCode::LayerDropped,
                     "HD layer " + number(i) + " has no ordinal: the stack has more layers than " +
                         number(ordinals) + " slots, so its animation cannot be kept",
                     layerRef(ordinal));
            ordinalOfLayer[i] = kInvalidIndex;
            continue;
        }
        ordinalOfLayer[i] = ordinal;
        addFresnel(*layers[i], ordinal, common);
    }

    if (!layers.empty()) {
        body.emissiveFactor = Vector3f(layers.front()->emissiveGain, layers.front()->emissiveGain,
                                       layers.front()->emissiveGain);
    }
    common.body = std::move(body);
}

} // namespace

// ============================================================================
// Context
// ============================================================================

u32 Context::toDocument(u32 mdxTextureId) const {
    if (mdxTextureId >= textureIndexMap.size()) {
        return kInvalidIndex;
    }
    return textureIndexMap[mdxTextureId];
}

u32 Context::toMdx(u32 documentTextureId) const {
    for (std::size_t i = 0; i < textureIndexMap.size(); ++i) {
        if (textureIndexMap[i] == documentTextureId) {
            return static_cast<u32>(i);
        }
    }
    return 0;
}

u32 Context::stockTexture(const std::string& path, u32 replaceableId) const {
    if (stockTextures == nullptr) {
        return kNoTexture;
    }
    for (std::size_t i = 0; i < stockTextures->size(); ++i) {
        const mdx::Texture& texture = (*stockTextures)[i];
        if (texture.replaceableId == replaceableId && texture.fileName == path) {
            return stockBase + static_cast<u32>(i);
        }
    }
    mdx::Texture texture;
    texture.fileName = path;
    texture.replaceableId = replaceableId;
    stockTextures->push_back(std::move(texture));
    return stockBase + static_cast<u32>(stockTextures->size() - 1);
}

StockSlotTexture StockTextureFor(Layer::SlotType slot) {
    switch (slot) {
    case Layer::SlotType::DiffuseMap:
        return {"Textures/white.blp", 0};
    case Layer::SlotType::NormalMap:
        return {"Textures/normal.blp", 0};
    case Layer::SlotType::ORMMap:
        return {"Textures/ORM.blp", 0};
    case Layer::SlotType::EmissiveMap:
        return {"Textures/Black32.blp", 0};
    case Layer::SlotType::TeamColor:
        // No file backs it: the engine substitutes the owning player's swatch,
        // and the ORM's alpha decides where it lands.
        return {"", 1};
    case Layer::SlotType::EnvironmentMap:
        return {"ReplaceableTextures/EnvironmentMap.blp", 0};
    default:
        break;
    }
    return {};
}

// ============================================================================
// Import
// ============================================================================

bool IsHdLayer(const mdx::Material& material, const Layer& layer, u32 modelVersion) {
    // The format's own flag first, so a producer that sets it is believed.
    if (layer.is_hd) {
        return true;
    }
    if (modelVersion >= 1100) {
        // `SDOnHD` is deliberately absent: it is SD content drawn through the HD
        // pipeline, and it shades the classic way.
        return layer.shader == Layer::ShaderType::HD || layer.shader == Layer::ShaderType::Crystal;
    }
    // Below v1100 there is no per-layer shader, so the material's name is the
    // only signal and the split is per material however the format is written.
    return material.shader == "Shader_HD_DefaultUnit" || material.shader == "Shader_HD_Crystal";
}

bool IsSdOnHdLayer(const mdx::Material& material, const Layer& layer, u32 modelVersion) {
    // v800 is an SD model's format, with no shader string to say otherwise.
    if (layer.is_hd || modelVersion <= 800) {
        return false;
    }
    if (modelVersion >= 1100) {
        return layer.shader == Layer::ShaderType::SDOnHD;
    }
    // The name the export writes for it below v1100 (`kSdOnHdShader`), and the
    // one `Parser::upgradeMaterials` reads.
    return material.shader == "Shader_SD_FixedFunction";
}

bool LayerAllowedIn(const mdx::Material& material, const Layer& layer, u32 modelVersion,
                    ProfileId profile) {
    // A Reforged model uses all four: HD, Crystal, SD on HD and SD.
    if (profile == ProfileId::Wc3Reforged) {
        return true;
    }
    // A classic model uses SD alone. From v1100 the layer names its shader, and
    // any other id — the HD family, SD on HD, a terrain or water shader — is
    // not a classic model's.
    if (IsHdLayer(material, layer, modelVersion) || IsSdOnHdLayer(material, layer, modelVersion)) {
        return false;
    }
    return modelVersion < 1100 || layer.shader == Layer::ShaderType::SD;
}

bool HasLayersFor(const mdx::Material& material, ProfileId profile, const Context& context) {
    for (const Layer& layer : material.layers) {
        if (!LayerAllowedIn(material, layer, context.modelVersion, profile)) {
            continue;
        }
        // A v800 file is an SD model: it declares a Reforged set only for an HD
        // layer, which it never has. From v900 a file is a Reforged-era one, and
        // everything in it is a Reforged model's.
        if (profile == ProfileId::Wc3Reforged && context.modelVersion <= 800 &&
            !IsHdLayer(material, layer, context.modelVersion)) {
            continue;
        }
        return true;
    }
    return false;
}

bool StackCollapses(const std::vector<const Layer*>& layers) {
    if (layers.size() < 2) {
        return false;
    }
    if (layers.front()->filterMode != Layer::FilterMode::None) {
        return false;
    }
    for (std::size_t i = 1; i < layers.size(); ++i) {
        CombinerOp unused = CombinerOp::Opaque;
        if (!collapsibleOp(layers[i]->filterMode, unused)) {
            return false;
        }
    }
    return true;
}

Material ImportMaterial(const mdx::Material& material, ProfileId profile, const Context& context,
                        Diagnostics& out, std::vector<u32>* layerOrdinals) {
    const std::vector<const Layer*> layers = layersFor(material, profile, context.modelVersion);

    // The filtered stack, back in the file's own numbering. `layersFor` keeps
    // source order, so a pointer comparison recovers the join without it having
    // to return one.
    std::vector<u32> ordinalOfLayer(layers.size(), kInvalidIndex);
    for (std::size_t i = 0; i < layers.size(); ++i) {
        ordinalOfLayer[i] = static_cast<u32>(i);
    }

    Material result;
    result.name = material.shader;
    CommonMaterial& common = result.InitCommon();
    applyHeader(material, layers, common, out);

    if (!layers.empty()) {
        common.blend = headerBlendFor(layers.front()->filterMode);
        if (common.blend == BlendMode::Transparent) {
            common.alphaTestThreshold = kTransparentAlphaRef;
        }
    }

    // The HD-shaded layers, where a Reforged material has any: they are what a
    // PBR body describes. A Reforged material of SD or SD-on-HD layers has no
    // slots to fill and takes the classic projection instead.
    std::vector<const Layer*> hdLayers;
    std::vector<std::size_t> hdAt;
    if (profile == ProfileId::Wc3Reforged) {
        for (std::size_t i = 0; i < layers.size(); ++i) {
            if (IsHdLayer(material, *layers[i], context.modelVersion)) {
                hdLayers.push_back(layers[i]);
                hdAt.push_back(i);
            }
        }
    }

    if (!hdLayers.empty()) {
        std::vector<u32> hdOrdinals(hdLayers.size(), kInvalidIndex);
        importPbr(hdLayers, context, common, out, hdOrdinals);
        ordinalOfLayer.assign(layers.size(), kInvalidIndex);
        for (std::size_t k = 0; k < hdAt.size(); ++k) {
            ordinalOfLayer[hdAt[k]] = hdOrdinals[k];
        }
        for (std::size_t i = 0; i < layers.size(); ++i) {
            if (!IsHdLayer(material, *layers[i], context.modelVersion)) {
                out.warn(DiagCode::LayerDropped,
                         "layer " + number(i) +
                             " is SD in a stack with HD layers: it is kept in the block and "
                             "drawn, but the PBR body does not describe it and its animation "
                             "has no ordinal",
                         layerRef(static_cast<u32>(i)));
            }
        }
    } else if (StackCollapses(layers)) {
        importCombiners(layers, context, common, out);
        attachLayerShading(material, layers, common);
    } else {
        importComposite(layers, context, common, out);
        attachLayerShading(material, layers, common);
        if (layers.size() > 1 && common.blend != BlendMode::Opaque) {
            // A later pass sees the scene where the fold sees the stack. WEM
            // stores the fold; the engine drew it the other way.
            out.warn(DiagCode::SceneReadingMultiLayerStack,
                     std::string("a ") + number(layers.size()) + "-layer stack over a " +
                         ToString(common.blend) +
                         " header: the passes read the scene, the fold "
                         "does not",
                     layerRef(0));
        }
        if (layers.size() > 1 && layers.front()->filterMode == Layer::FilterMode::Transparent) {
            out.info(DiagCode::CollapseRefused,
                     "an alpha-keyed first layer keeps the scene where a single alpha-tested "
                     "draw would discard it, so the stack stays Composite",
                     layerRef(0));
        }
    }

    // The native block holds only the layers this profile may hold: that is
    // what makes `DeriveProfile(Wc3Reforged -> Wc3Classic)` a layer filter
    // (§7.3).
    native::MdxMaterial block;
    CopyToNative(material, block);
    block.sourceVersion = context.modelVersion;
    std::vector<native::MdxLayer> kept;
    kept.reserve(layers.size());
    for (std::size_t i = 0; i < material.layers.size() && i < block.layers.size(); ++i) {
        if (!LayerAllowedIn(material, material.layers[i], context.modelVersion, profile)) {
            continue;
        }
        // The mirror's `isHd` is normalised on the way in to the layer's own
        // shading, so a block written from a v900 file — where the flag was
        // never on disk — still says it. It is not the set: a Reforged block's
        // SD layer is SD, and saying otherwise would read it back as HD.
        block.layers[i].isHd = IsHdLayer(material, material.layers[i], context.modelVersion);
        kept.push_back(std::move(block.layers[i]));
    }
    block.layers = std::move(kept);
    result.SetNativeInSync(std::move(block));

    if (layerOrdinals != nullptr) {
        layerOrdinals->assign(material.layers.size(), kInvalidIndex);
        std::size_t filtered = 0;
        for (std::size_t i = 0; i < material.layers.size(); ++i) {
            if (!LayerAllowedIn(material, material.layers[i], context.modelVersion, profile)) {
                continue;
            }
            if (filtered < ordinalOfLayer.size()) {
                (*layerOrdinals)[i] = ordinalOfLayer[filtered];
            }
            ++filtered;
        }
    }
    return result;
}

// ============================================================================
// Export
// ============================================================================

namespace {

/// The one string in an `.mdx` below v1100 that says a stack is HD.
///
/// A layer carries no shader id until v1100, so the MATERIAL's shader name is
/// the whole signal: `Parser::Impl::upgradeMaterials` tests it verbatim and only
/// then merges the six positional layers into one HD layer, and `IsHdLayer`
/// reads the same string on the way back in. A material this export DERIVED
/// arrives named for its source — `wing_mat#A` off a Diablo III appearance —
/// and writing that name into the shader field left Imperius's wings as six
/// opaque fixed-function passes with six different textures stacked on them.
constexpr const char* kHdDefaultShader = "Shader_HD_DefaultUnit";
constexpr const char* kHdCrystalShader = "Shader_HD_Crystal";
/// The classic pipeline, running inside a Reforged model. Below v1100 the
/// material's shader name is the whole signal -- `mdl_writer` and
/// `Parser::upgradeMaterials` both read this exact string -- and from v1100
/// the layer's own `SDOnHD` says it too.
constexpr const char* kSdOnHdShader = "Shader_SD_FixedFunction";

/// A native block, un-merged for a file below v1100.
///
/// **In memory a layer's textures live in its sub-texture array; below v1100 in a
/// FILE they do not exist.** `Parser::upgradeModel` moves an SD layer's
/// `textureId` into `subTextures[0]` and zeroes it, and folds a Reforged
/// material's six positional layers into one layer with six sub-textures. A
/// native block is a copy of that in-memory shape, so writing one straight back
/// out below v1100 wrote texture 0 for every layer of every material: Arthas
/// came out of his own round trip wearing his team colour head to foot, and the
/// chimaera lost its normal, ORM, emissive, team-colour and environment maps to
/// a single-layer material naming one texture.
///
/// v800 has no material shader string to say "HD" with, so there a merged layer
/// keeps its diffuse and nothing else -- which is what a classic file can say.
void exportFromNative(const native::MdxMaterial& block, u32 modelVersion, mdx::Material& dst) {
    CopyFromNative(block, dst);
    if (modelVersion >= 1100) {
        return;
    }
    const bool positional = modelVersion >= 900;
    std::vector<Layer> unmerged;
    bool anyHd = false;
    for (Layer& layer : dst.layers) {
        if (layer.subTextures.empty()) {
            unmerged.push_back(std::move(layer));
            continue;
        }
        if (layer.subTextures.size() == 1 || !positional) {
            layer.textureId = layer.subTextures.front().textureId;
            layer.subTextures.clear();
            unmerged.push_back(std::move(layer));
            continue;
        }
        // The six back out in `SlotType` order, which is the order the block
        // holds them in and the order `upgradeMaterials` reads them back by.
        anyHd = true;
        for (const Layer::SubTexture& sub : layer.subTextures) {
            Layer one = layer;
            one.subTextures.clear();
            one.textureId = sub.textureId;
            unmerged.push_back(std::move(one));
        }
    }
    dst.layers = std::move(unmerged);
    if (anyHd && dst.shader.empty()) {
        // Below v1100 the material's shader NAME is the only thing that says a
        // positional stack is one HD material rather than six SD passes.
        dst.shader = kHdDefaultShader;
    }
}

/// From v1100 a layer's texture lives only in its sub-texture list (the parser
/// zeroes `textureId`), so a layer the kind mapping built on the legacy field
/// moves it there -- the spelling shipped classic layers use. Left in place,
/// every layer of a v1800 export read as texture 0.
void placeTexturesForVersion(u32 modelVersion, mdx::Material& dst) {
    if (modelVersion < 1100) {
        return;
    }
    for (Layer& layer : dst.layers) {
        if (!layer.subTextures.empty()) {
            continue;
        }
        Layer::SubTexture sub;
        sub.textureId = layer.textureId;
        sub.slot = Layer::SlotType::DiffuseMap;
        sub.tracks = std::move(layer.textureIdTracks);
        layer.subTextures.push_back(std::move(sub));
        layer.textureId = 0;
        layer.textureIdTracks = {};
    }
}

/// Adds @p layer to @p dst under the compositing intent @p mode.
///
/// An `.mdx` layer is a **pass over the whole geoset**, not a step in a register
/// chain, so `FilterMode::None` is neither an identity nor a chain-replace: it
/// is an opaque draw, and everything written under it stops being visible. That
/// makes it the right reading of a replace -- a stage that replaces its register
/// really does kill the stages before it -- but only if the stack is restarted.
/// Left in place it is a stack whose LAST opaque layer is the entire material.
///
/// The layer that starts a stack takes @p blend instead, because the base layer
/// is how the whole draw meets the scene and is the one thing it must carry.
void pushLayer(Layer layer, Layer::FilterMode mode, BlendMode blend, mdx::Material& dst,
               std::vector<u32>& layerOfOrdinal, u32 ordinal) {
    if (mode == Layer::FilterMode::None) {
        dst.layers.clear();
        // The stack restarted, so no ordinal written before this one is still in
        // it -- the same statement the `clear()` above makes about the layers.
        std::fill(layerOfOrdinal.begin(), layerOfOrdinal.end(), kInvalidIndex);
    }
    layer.textureAnimationId = kNoTextureAnimation;
    layer.filterMode = dst.layers.empty() ? filterModeFor(blend) : mode;
    if (layerOfOrdinal.size() <= ordinal) {
        layerOfOrdinal.resize(ordinal + 1, kInvalidIndex);
    }
    layerOfOrdinal[ordinal] = static_cast<u32>(dst.layers.size());
    dst.layers.push_back(std::move(layer));
}

/// Whether an MDX layer stack has anywhere to put a layer aimed at @p channel.
///
/// An `.mdx` layer is a textured draw of the whole geoset and nothing else:
/// there is no slot, so the only question a layer answers is what colour comes
/// out. `Color` builds it, and `Emissive` and `Environment` are the two channels
/// Warcraft III spells as further *additive* passes over it — which is the op
/// they are written with, since a layer's own op is its fold within its channel
/// and says nothing about how that channel meets the others.
///
/// The other three have no SD expression at all, and writing them anyway did
/// not lose them quietly: a `Normal` layer inherits the material's blend, so a
/// StarCraft II material opened as Warcraft III repainted the whole model with
/// its normal map. A two-channel normal map reads as flat orange.
bool drawsColour(SurfaceChannel channel) {
    return channel == SurfaceChannel::Color || channel == SurfaceChannel::Emissive ||
           channel == SurfaceChannel::Environment;
}

/// How strongly a channel that MDX draws as a further pass contributes.
///
/// An `.mdx` layer has no strength of its own — it is a whole draw — so a
/// channel WEM scales by a factor has to carry it here or not be written at all.
/// Diablo III leaves `environmentFactor` at zero on every material and lets the
/// gloss map carry the reflection instead, so adding his environment map as a
/// full-weight pass turned the Skeleton King's gold armour teal and washed
/// Malthael's robe out. StarCraft II fills both factors, and keeps its layers.
f32 channelFactor(const CompositeBody& body, SurfaceChannel channel) {
    switch (channel) {
    case SurfaceChannel::Emissive:
        return std::max({body.emissiveFactor.x, body.emissiveFactor.y, body.emissiveFactor.z});
    case SurfaceChannel::Environment:
        return body.environmentFactor;
    default:
        return 1.0f;
    }
}

void exportComposite(const CompositeBody& body, const CommonMaterial& common,
                     const Context& context, mdx::Material& dst, Diagnostics& out,
                     std::vector<u32>& layerOfOrdinal) {
    for (std::size_t i = 0; i < body.layers.size(); ++i) {
        const CompositeLayer& entry = body.layers[i];
        if (!drawsColour(entry.target)) {
            out.warn(DiagCode::LayerDropped,
                     std::string("MDX has no slot for a '") + ToString(entry.target) + "' layer");
            continue;
        }
        const f32 factor = channelFactor(body, entry.target);
        if (factor <= 0.0f) {
            out.info(DiagCode::LayerDropped,
                     std::string("the '") + ToString(entry.target) +
                         "' factor is zero, so the layer contributes nothing",
                     layerRef(static_cast<u32>(i)));
            continue;
        }
        Layer layer;
        layer.textureId = context.toMdx(entry.input.texture);
        layer.coordId = entry.input.uvSet;
        layer.alpha = entry.input.weight * std::min(factor, 1.0f);
        if (entry.input.mapping == UVMappingMode::EnvSphere) {
            layer.shadingFlags |= Layer::ShadingFlag::SphereEnvMap;
        }
        if (entry.input.wrapU == WrapMode::Repeat) {
            layer.shadingFlags |= Layer::ShadingFlag::WrapWidth;
        }
        if (entry.input.wrapV == WrapMode::Repeat) {
            layer.shadingFlags |= Layer::ShadingFlag::WrapHeight;
        }
        // A layer's op is its fold WITHIN its own channel; against the *other*
        // channels an emissive or an environment layer is the additive pass
        // `drawsColour` says it is. Reading the op here was fine while the only
        // source was a `.m3` -- whose stacks carry a real `Add` -- and wrong the
        // moment a slot map arrived, because a converted one holds exactly one
        // layer per channel and so carries `Set` on every one of them. Diablo
        // III's environment map was drawn opaque over his own diffuse.
        //
        // The exception is a MODULATE-family op: a Mod/Mod2x/Lerp emissive is a
        // light gate — StarCraft II multiplies it into the LIT RESULT — and
        // Warcraft III can say exactly that as a modulate (or blend) pass.
        // Written additive, a darkening mask became a glow.
        Layer::FilterMode passMode = Layer::FilterMode::Additive;
        if (entry.target == SurfaceChannel::Color) {
            passMode = filterModeFor(entry.op);
        } else if (entry.op == CompositeOp::Modulate || entry.op == CompositeOp::Modulate2x ||
                   entry.op == CompositeOp::AlphaBlend) {
            passMode = filterModeFor(entry.op);
        }
        pushLayer(std::move(layer), passMode, common.blend, dst, layerOfOrdinal,
                  static_cast<u32>(i));
    }
}

/// One chain stage as a PASS, or nothing where the stage does not draw.
///
/// `Pass` is the identity — the stage leaves the colour alone — and there is no
/// filter mode that draws nothing, so the caller decides what to do with it
/// (drop it, or seed the stack with it; see `exportCombiners`). Diablo III is
/// the only source that spells an identity, a stage that masks alpha and nothing
/// else; Warcraft III's vocabulary has none, which is why the old mapping folded
/// it onto `None` and drew every alpha mask as an opaque pass of the mask.
std::optional<Layer::FilterMode> passModeFor(CombinerOp op) {
    switch (op) {
    case CombinerOp::Opaque:
        return Layer::FilterMode::None; // replaces the register; restarts the stack
    case CombinerOp::Mod:
        return Layer::FilterMode::Modulate;
    case CombinerOp::Mod2x:
        return Layer::FilterMode::Modulate2x;
    case CombinerOp::Add:
        return Layer::FilterMode::Additive;
    case CombinerOp::AddAlpha:
        return Layer::FilterMode::AddAlpha;
    case CombinerOp::Decal:
    case CombinerOp::Fade:
        return Layer::FilterMode::Blend;
    case CombinerOp::Pass:
    // The masked fold reads the SEED pass's alpha, which no MDX pass can:
    // exactly the identity the chain table used to spell as `Pass` before the
    // op existed for targets that can express it.
    case CombinerOp::MaskedMod:
    case CombinerOp::MaskedMod2x:
    case CombinerOp::Count:
        break;
    }
    return std::nullopt;
}

void exportCombiners(const CombinersBody& body, const CommonMaterial& common,
                     const Context& context, mdx::Material& dst, Diagnostics& out,
                     std::vector<u32>& layerOfOrdinal) {
    // The §7.2.2 inverse: a stage becomes a layer with the matching filter mode,
    // and the layer that starts the stack takes the material's blend rather than
    // an unconditional `None` — a chain seeded by an opaque stage still meets the
    // scene however its header says, and Diablo III's additive wings met it as
    // solid plates while that was hard-coded.
    for (std::size_t i = 0; i < body.stages.size(); ++i) {
        const CombinerStage& stage = body.stages[i];
        if (stage.rgb == CombinerOp::MaskedMod || stage.rgb == CombinerOp::MaskedMod2x) {
            out.info(DiagCode::LossyKindConversion,
                     "combiner stage " + number(i) +
                         " modulates under the seed's alpha mask, which no MDX pass reads",
                     layerRef(static_cast<u32>(i)));
            continue;
        }
        std::optional<Layer::FilterMode> mode = passModeFor(stage.rgb);
        if (!mode.has_value()) {
            // `Pass` on a stage the chain has already seeded contributes nothing
            // to the colour, and there is no filter mode that draws nothing.
            //
            // On the FIRST stage it is not nothing: the register has no value
            // yet, so what the chain draws is that stage's own sample. Diablo
            // III writes a wing exactly so — the base map's colour code is zero
            // and its alpha carries the shape — and dropping it left the wing
            // as its two masks, which is a bright sheet rather than a wing.
            if (!dst.layers.empty()) {
                // Alpha too is an identity (a masked fold baked into the seed):
                // dropping it loses nothing, so there is nothing to report.
                if (stage.alpha != CombinerOp::Pass) {
                    out.warn(DiagCode::LayerDropped,
                             "combiner stage " + number(i) +
                                 " passes the colour through, and an MDX layer is a draw: there "
                                 "is no pass that contributes nothing",
                             layerRef(static_cast<u32>(i)));
                }
                continue;
            }
            mode = Layer::FilterMode::None;
        }
        // A Modulate LAYER multiplies the framebuffer. Over a base that draws
        // its own colour into it — opaque, keyed, blended, or one that itself
        // multiplies — that framebuffer is (mostly) the chain's colour, which
        // is what the stage means. Over an ADDITIVE base the framebuffer is
        // still the untouched scene: the tauren primalist's flame chain
        // (AddAlpha then Mod) repainted the whole model red through its own
        // flame planes. Nor does the product have a two-pass spelling there —
        // adding both factors washed the same fire to white, and swapping the
        // moving factor into the base pass dragged the whole texture atlas
        // through the plane (the SEED's alpha is the sparse mask that keeps
        // the coverage honest; the mod stage is dense overlay detail). The
        // seed stays, the mod stage drops.
        if (!dst.layers.empty() &&
            (*mode == Layer::FilterMode::Modulate || *mode == Layer::FilterMode::Modulate2x)) {
            const Layer::FilterMode base = filterModeFor(common.blend);
            if (base == Layer::FilterMode::Additive || base == Layer::FilterMode::AddAlpha) {
                out.warn(DiagCode::LayerDropped,
                         "combiner stage " + number(i) +
                             " modulates an additive base, and a Modulate pass would "
                             "multiply the scene instead",
                         layerRef(static_cast<u32>(i)));
                continue;
            }
        }
        Layer layer;
        layer.textureId = context.toMdx(stage.input.texture);
        layer.coordId = stage.input.uvSet;
        layer.alpha = stage.input.weight;
        // A sphere-mapped stage says so, as `exportComposite`'s layers do;
        // without the flag the layer reads `coordId` as the mesh's own UVs.
        if (stage.input.mapping == UVMappingMode::EnvSphere) {
            layer.shadingFlags |= Layer::ShadingFlag::SphereEnvMap;
        }
        // The same two flags `exportComposite` writes. A scrolling stage is the
        // case that needs them: `mdx_anim` gives it a TXAN whose offset walks
        // past 1, and a clamped layer holds its last column there forever.
        if (stage.input.wrapU == WrapMode::Repeat) {
            layer.shadingFlags |= Layer::ShadingFlag::WrapWidth;
        }
        if (stage.input.wrapV == WrapMode::Repeat) {
            layer.shadingFlags |= Layer::ShadingFlag::WrapHeight;
        }
        pushLayer(std::move(layer), *mode, common.blend, dst, layerOfOrdinal, static_cast<u32>(i));
    }
}

/// Whether this material is a textured surface and nothing more.
///
/// Warcraft III has a shader for exactly that inside a Reforged model --
/// `Shader_SD_FixedFunction`, the classic pipeline running in the HD one -- and
/// a material with nothing but a colour map is what it is for. Put through the
/// HD shader instead, the same surface is lit as though it had a flat normal, a
/// mid roughness and no metalness: a plastic sheen over art whose lighting is
/// already painted in. A `.m3` effect plane and a `.m2` creature both arrive
/// this way, because neither source has any of the maps the HD shader wants.
///
/// The emissive is allowed to name the SAME texture as the colour and nothing
/// else, because that is how a self-lit surface comes out of a derive: one map,
/// bound twice. Anything further -- a normal, an ORM, a team colour, an emissive
/// that is its own map -- is a material the HD shader can say something about.
bool isPlainColour(const PbrDeferredBody& body) {
    const TextureInput* base = body.find(PbrSlot::BaseColor);
    if (base == nullptr || !base->hasTexture()) {
        return false;
    }
    // Every slot, not only the six with an MDX `SlotType`: a body carrying the
    // unpacked metal/rough alternative has PBR maps that this file has nowhere
    // to put, and "nowhere to put it" is not the same statement as "the surface
    // is a colour map".
    for (const auto& [slot, input] : body.slots) {
        if (!input.hasTexture() || slot == PbrSlot::BaseColor) {
            continue;
        }
        if (slot == PbrSlot::Emissive && input.texture == base->texture) {
            continue;
        }
        return false;
    }
    return true;
}

void exportPbr(const PbrDeferredBody& body, const CommonMaterial& common, const Context& context,
               u32 modelVersion, mdx::Material& dst, Diagnostics& out,
               std::vector<u32>& layerOfOrdinal) {
    // Crystal is the one other HD shader the format names, and a material that
    // came in as one is still one; everything else this writes is a default
    // unit, including every derived material.
    const bool crystal = dst.shader == kHdCrystalShader;
    if (!crystal) {
        dst.shader = kHdDefaultShader;
    }

    // A slot that is *present* but names no texture is an absent slot.
    // `Context::toMdx` maps `kInvalidIndex` to 0 -- there is no other answer it
    // could give -- and 0 is the base colour, so a slot left blank rather than
    // removed binds the colour map as a normal, an ORM or an emissive one. That
    // is the same defect as the old `textureId = 0` fallback, reached from the
    // other side: not "the slot is missing" but "the slot is empty".
    const auto textured = [&body](Layer::SlotType type) -> const TextureInput* {
        const TextureInput* input = body.find(pbrSlotFor(type));
        return input != nullptr && input->hasTexture() ? input : nullptr;
    };

    Layer layer;
    layer.is_hd = true;
    layer.shader = Layer::ShaderType::HD;
    // An HD stack is one draw and the header blend is its blend, the same way
    // `ImportMaterial` reads it back off this layer. Hardcoding `None` here made
    // every derived HD material opaque, which is what turned a translucent
    // Diablo III wing into a solid orange sheet.
    layer.filterMode = filterModeFor(common.blend);
    layer.textureAnimationId = kNoTextureAnimation;

    // --- a surface with only a colour map is not an HD surface ---------------
    //
    // It goes out as SD content drawn through the HD pipeline, which is a whole
    // material's worth of vocabulary Warcraft III has and the PBR slot map does
    // not: one textured pass, lit the classic way. Crystal is left alone -- it
    // is an HD shader with a look of its own, not a default unit with fewer
    // maps.
    if (!crystal && isPlainColour(body)) {
        const TextureInput* base = textured(Layer::SlotType::DiffuseMap);
        dst.shader = kSdOnHdShader;
        layer.is_hd = false;
        layer.shader = Layer::ShaderType::SDOnHD;
        layer.coordId = base->uvSet;
        layer.alpha = base->weight;
        const u32 id = context.toMdx(base->texture);
        if (modelVersion >= 1100) {
            // The spelling shipped content uses: 738 of the 739 classic layers
            // in `war3.w3mod` put the texture in `subTextures[0]` and leave
            // `textureId` at 0, which is also what `Parser::upgradeMaterials`
            // writes when it lifts a pre-1100 layer.
            Layer::SubTexture sub;
            sub.textureId = id;
            sub.slot = Layer::SlotType::DiffuseMap;
            layer.subTextures.push_back(std::move(sub));
        } else {
            layer.textureId = id;
        }
        const f32 gain =
            std::max({body.emissiveFactor.x, body.emissiveFactor.y, body.emissiveFactor.z});
        if (textured(Layer::SlotType::EmissiveMap) != nullptr && gain > 1.0f) {
            out.info(DiagCode::LossyKindConversion,
                     "the emissive map is the colour map, so the surface goes out as one "
                     "fixed-function pass and its emissive gain is not carried");
        }
        layerOfOrdinal.assign(body.slots.size(), static_cast<u32>(dst.layers.size()));
        dst.layers.push_back(std::move(layer));
        return;
    }

    // The emissive map's strength, which `emissiveGain` is MDX's name for and
    // `ImportMaterial` reads back off this same field. Dropping it published
    // every emissive map at gain 1: a StarCraft II material states its own in
    // `hdrEmissiveMultiplier` and routinely means it -- a Reaper's is **9** --
    // so a glow that carried the model came out as a faint tint of its map.
    // Only written when there is a map for it to scale; the factor is zero on
    // every material that has no emissive, and a gain of zero on a slot the
    // engine already binds black for says nothing.
    if (textured(Layer::SlotType::EmissiveMap) != nullptr) {
        layer.emissiveGain =
            std::max({body.emissiveFactor.x, body.emissiveFactor.y, body.emissiveFactor.z});
    }

    // The fresnel overlay — the one representable per-material view-dependent
    // effect, and `importPbr` already reads these three fields back into a
    // feature. Only a WHOLE-MATERIAL feature is written out: a per-layer one
    // (the M3 import's) indexes the source stack, not this slot map, and its
    // default white colour says nothing a rim should show. A converter that
    // wants a rim to survive puts one whole-material feature here.
    if (const MaterialFeature* feature = common.feature(FeatureKind::Fresnel)) {
        if (const auto* fresnel = std::get_if<FresnelFeature>(&feature->payload)) {
            layer.fresnelColor = fresnel->color;
            layer.fresnelOpacity = std::clamp(fresnel->outMax, 0.0f, 1.0f);
            layer.fresnelTeamColor = std::clamp(fresnel->teamColor, 0.0f, 1.0f);
        }
    }

    // --- every slot names a texture -----------------------------------------
    //
    // Not one of the 12,893 six-slot HD layers in `war3.w3mod` leaves a slot
    // empty, and `-1` is not a value the engine has a reading for: it is a
    // texture id like any other. A material with no map of its own for a slot
    // names Warcraft III's own neutral there instead (`StockTextureFor`), which
    // is what shipped content does -- `Textures/normal.blp` on 900 files,
    // `Textures/Black32.blp` on 3,237.
    //
    // The environment map is the one slot whose own input is *ignored*: all
    // 12,893 name `ReplaceableTextures/EnvironmentMap.blp`, because the engine
    // substitutes the current map's own reflection there. A source's reflection
    // map has nowhere to go, and binding it would be a cube the map disagrees
    // with.
    const auto textureIdFor = [&](Layer::SlotType type) -> u32 {
        if (type != Layer::SlotType::EnvironmentMap) {
            if (const TextureInput* input = textured(type)) {
                return context.toMdx(input->texture);
            }
        }
        const StockSlotTexture stock = StockTextureFor(type);
        return context.stockTexture(stock.path, stock.replaceableId);
    };

    // v1100 is where the sub-texture array exists -- `mdx/writer.cpp` gates it
    // on `>= 1100` and so does `parseLayer` -- and above it the merged one-layer
    // form is the only one a reader understands, because `upgradeMaterials`
    // stops at v1000 and never runs. Testing 1200 here wrote a v1100 model as
    // six positional layers with an empty sub-texture array each, which is five
    // textures lost.
    if (modelVersion >= 1100) {
        // Every slot, in `SlotType` order: a consumer is entitled to read this
        // array positionally -- `MdxModelAdapter` does -- and a short array
        // would hand it the environment map as a normal map.
        // An HD layer has ONE coordId and one alpha for all six slots. The
        // base colour's win — it is the map the mesh was unwrapped for — and
        // a slot that disagrees is reported instead of silently electing
        // whichever slot the loop visited last.
        const TextureInput* lead = textured(Layer::SlotType::DiffuseMap);
        for (u32 index = 0; index < static_cast<u32>(kHdPositionalSlotCount); ++index) {
            const auto type = static_cast<Layer::SlotType>(index);
            const TextureInput* input = textured(type);
            Layer::SubTexture sub;
            sub.textureId = textureIdFor(type);
            sub.slot = type;
            layer.subTextures.push_back(std::move(sub));
            if (input != nullptr && lead == nullptr) {
                lead = input;
            }
            if (input != nullptr && lead != nullptr && input->uvSet != lead->uvSet) {
                out.warn(DiagCode::LossyKindConversion,
                         std::string("HD slot ") + ToString(pbrSlotFor(type)) +
                             " samples uv set " + number(input->uvSet) +
                             " but the layer carries uv set " + number(lead->uvSet) +
                             "; an HD layer has one coordId");
            }
        }
        if (lead != nullptr) {
            layer.coordId = lead->uvSet;
            layer.alpha = lead->weight;
        }
        for (const auto& [slot, input] : body.slots) {
            if (slotTypeFor(slot) == Layer::SlotType::Unknown) {
                out.warn(DiagCode::LayerDropped,
                         std::string("PBR slot ") + ToString(slot) + " has no MDX SlotType");
            }
        }
        // Every slot is in this one layer, so every ordinal is. An HD stack has
        // one texture matrix and one alpha for all six slots, which is what a
        // track or a UV feature on any of them ends up driving.
        layerOfOrdinal.assign(body.slots.size(), static_cast<u32>(dst.layers.size()));
        dst.layers.push_back(std::move(layer));
        return;
    }

    // Pre-1100 has no sub-texture array: the slots go back out as one layer
    // each, in SlotType order, which is the convention import read them by.
    //
    // A slot the body does not fill takes the stock neutral above, NOT texture
    // 0. A derived material has a base colour and nothing else -- no normal map,
    // no ORM, no emissive, because the source format never had them -- and
    // falling back to index 0 pointed all five at the diffuse: the colour map
    // was read as a normal map, as a roughness/metalness map, and as an emissive
    // one, which is what made Imperius's wings a blown-out orange sheet.
    layerOfOrdinal.assign(body.slots.size(), kInvalidIndex);
    for (u32 slot = 0; slot < static_cast<u32>(kHdPositionalSlotCount); ++slot) {
        const auto type = static_cast<Layer::SlotType>(slot);
        const PbrSlot pbrSlot = pbrSlotFor(type);
        const TextureInput* input = textured(type);
        Layer positional = layer;
        positional.textureId = textureIdFor(type);
        positional.coordId = input != nullptr ? input->uvSet : 0;
        positional.alpha = input != nullptr ? input->weight : 1.0f;
        for (std::size_t ordinal = 0; ordinal < body.slots.size(); ++ordinal) {
            if (body.slots[ordinal].first == pbrSlot) {
                layerOfOrdinal[ordinal] = static_cast<u32>(dst.layers.size());
            }
        }
        dst.layers.push_back(std::move(positional));
    }
}

} // namespace

mdx::Material ExportMaterial(const Material& material, ProfileId profile, const Context& context,
                             Diagnostics& out, std::vector<u32>* layerOfOrdinal) {
    mdx::Material dst;
    const auto report = [&](const std::vector<u32>& ordinals) {
        if (layerOfOrdinal != nullptr) {
            *layerOfOrdinal = ordinals;
            layerOfOrdinal->resize(material.Common().ordinalCount(), kInvalidIndex);
        }
    };

    // §7.1: a native block that is not stale IS the answer. The kind mapping
    // below is the fallback path, and only that.
    if (material.hasNative() && material.sync() != NativeSync::CommonEdited) {
        if (material.nativeKind() == NativeKind::Mdx) {
            exportFromNative(std::get<native::MdxMaterial>(material.Native()), context.modelVersion,
                             dst);
            // A block parsed from a v800 file holds no sub-textures.
            placeTexturesForVersion(context.modelVersion, dst);
            // The block holds this profile's layers in ordinal order (§7.3), and
            // import numbers an HD layer by its position too, so here the map
            // really is the identity.
            std::vector<u32> identity(dst.layers.size());
            for (std::size_t i = 0; i < identity.size(); ++i) {
                identity[i] = static_cast<u32>(i);
            }
            report(identity);
            return dst;
        }
        out.warn(DiagCode::DroppedNativeBlock,
                 std::string("native block is ") + ToString(material.nativeKind()) +
                     ", not Mdx; exporting from the common material instead");
    }

    const CommonMaterial& common = material.Common();
    dst.priorityPlane = common.priorityPlane;
    if (common.cull == CullMode::None) {
        dst.flags |= mdx::Material::Flag::TwoSided;
    }
    if (hasFlag(common.flags, MaterialFlags::Unfogged)) {
        dst.flags |= mdx::Material::Flag::Unfogged;
    }
    if (hasFlag(common.flags, MaterialFlags::SortNearZ)) {
        dst.flags |= mdx::Material::Flag::SortPrimsNearZ;
    }
    if (hasFlag(common.flags, MaterialFlags::SortFarZ)) {
        dst.flags |= mdx::Material::Flag::SortPrimsFarZ;
    }
    dst.shader = material.name;

    std::vector<u32> ordinals;
    switch (common.kind()) {
    case MaterialKind::Composite:
        exportComposite(*common.composite(), common, context, dst, out, ordinals);
        break;
    case MaterialKind::Combiners:
        exportCombiners(*common.combiners(), common, context, dst, out, ordinals);
        break;
    case MaterialKind::PBRDeferred:
        exportPbr(*common.pbr(), common, context, context.modelVersion, dst, out, ordinals);
        break;
    default:
        out.warn(DiagCode::UnsupportedMaterialKind,
                 std::string("MDX cannot express a ") + ToString(common.kind()) + " material",
                 ElementRef(), profile);
        break;
    }

    // A material the kind mapping left with no layers at all. That is not "draws
    // nothing" in MDX — a geoset whose material has an empty stack draws
    // untextured white, and Diablo's `FX_EMIT` proxies (emitter markers, which
    // carry no texture) came through his chest as white shards. One transparent
    // layer says what the source meant.
    if (dst.layers.empty()) {
        out.info(DiagCode::LayerDropped,
                 "no layer of this material has an MDX expression; writing a transparent one so "
                 "the geoset does not draw untextured");
        Layer blank;
        blank.filterMode = Layer::FilterMode::Blend;
        blank.alpha = 0.0f;
        blank.textureAnimationId = kNoTextureAnimation;
        dst.layers.push_back(std::move(blank));
    }

    // A surface that does not draw. MDX has no such flag, and the idiom
    // Warcraft III uses for it is the one available here: an alpha-blended pass
    // at zero opacity covers nothing. The layers stay so the textures a tool
    // reads off the file are still the ones the surface would have worn.
    if (hasFlag(common.flags, MaterialFlags::Invisible)) {
        for (Layer& layer : dst.layers) {
            layer.filterMode = Layer::FilterMode::Blend;
            layer.alpha = 0.0f;
        }
    }

    report(ordinals);

    // The per-layer shading flags the header carries back. MDX puts them on
    // every layer; the header only ever held one set.
    for (Layer& layer : dst.layers) {
        if (hasFlag(common.flags, MaterialFlags::Unlit)) {
            layer.shadingFlags |= Layer::ShadingFlag::Unshaded;
        }
        if (!common.depth.test) {
            layer.shadingFlags |= Layer::ShadingFlag::NoDepthTest;
        }
        if (!common.depth.write) {
            layer.shadingFlags |= Layer::ShadingFlag::NoDepthSet;
        }
        if (common.cull == CullMode::None) {
            layer.shadingFlags |= Layer::ShadingFlag::TwoSided;
        }
        if (hasFlag(common.flags, MaterialFlags::Unfogged)) {
            layer.shadingFlags |= Layer::ShadingFlag::Unfogged;
        }
    }
    // A layer that kept its own decision (`LayerShadingFeature`) gets it back,
    // over the header's.
    for (const MaterialFeature& feature : common.features) {
        const LayerShadingFeature* shading = feature.layerShading();
        if (shading == nullptr || feature.layer >= ordinals.size() ||
            ordinals[feature.layer] >= dst.layers.size()) {
            continue;
        }
        Layer& layer = dst.layers[ordinals[feature.layer]];
        const auto set = [&layer](Layer::ShadingFlag bit, bool on) {
            const u32 bits = static_cast<u32>(layer.shadingFlags);
            layer.shadingFlags = static_cast<Layer::ShadingFlag>(
                on ? bits | static_cast<u32>(bit) : bits & ~static_cast<u32>(bit));
        };
        set(Layer::ShadingFlag::Unshaded, shading->unlit);
        set(Layer::ShadingFlag::TwoSided, shading->twoSided);
        set(Layer::ShadingFlag::Unfogged, shading->unfogged);
        set(Layer::ShadingFlag::NoDepthTest, shading->noDepthTest);
        set(Layer::ShadingFlag::NoDepthSet, shading->noDepthWrite);
    }
    placeTexturesForVersion(context.modelVersion, dst);
    return dst;
}

} // namespace mdx_core
} // namespace wem
} // namespace models
} // namespace whiteout
