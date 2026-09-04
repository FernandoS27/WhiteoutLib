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

bool wantsHd(ProfileId profile) {
    return profile == ProfileId::Wc3Reforged;
}

std::vector<const Layer*> layersFor(const mdx::Material& material, ProfileId profile,
                                    u32 modelVersion) {
    std::vector<const Layer*> out;
    const bool hd = wantsHd(profile);
    for (const Layer& layer : material.layers) {
        if (IsHdLayer(material, layer, modelVersion) == hd) {
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
    input.wrapU = hasFlag(layer.shadingFlags, Layer::ShadingFlag::WrapWidth) ? WrapMode::Repeat
                                                                             : WrapMode::Clamp;
    input.wrapV = hasFlag(layer.shadingFlags, Layer::ShadingFlag::WrapHeight) ? WrapMode::Repeat
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
            out.warn(DiagCode::MixedShadedUnshadedStack,
                     "layer " + number(i) + (unshaded ? " is unshaded" : " is shaded") +
                         " and the first layer is not; WEM lights the assembled surface once",
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
    // MDX carries no exponent, and `fresnelOpacity` is the effect's strength —
    // an output ceiling, which is what `outMax` is (§7.2.5).
    fresnel.exponent = 1.0f;
    fresnel.outMin = 0.0f;
    fresnel.outMax = layer.fresnelOpacity;
    fresnel.teamColor = layer.fresnelTeamColor;

    MaterialFeature feature;
    feature.id = NextFeatureId(common.features);
    feature.layer = ordinal;
    feature.payload = fresnel;
    common.features.push_back(feature);
}

// ── import: the classic stack ───────────────────────────────────────────────

void importComposite(const std::vector<const Layer*>& layers, const Context& context,
                     CommonMaterial& common, Diagnostics& out) {
    CompositeBody body;
    for (std::size_t i = 0; i < layers.size(); ++i) {
        const Layer& layer = *layers[i];
        CompositeLayer entry;
        entry.input = inputFor(layer, layer.textureId, context, static_cast<u32>(i), out);
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
        stage.input = inputFor(layer, layer.textureId, context, static_cast<u32>(i), out);
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
        // Where this layer's first slot landed in the body — the ordinal space
        // for a deferred kind is the `slots` vector, not the layer stack.
        ordinalOfLayer[i] = static_cast<u32>(body.slots.size());

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
        addFresnel(layer, ordinal, common);
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
        // pipeline, and its layers belong to the classic set.
        return layer.shader == Layer::ShaderType::HD || layer.shader == Layer::ShaderType::Crystal;
    }
    // Below v1100 there is no per-layer shader, so the material's name is the
    // only signal and the split is per material however the format is written.
    return material.shader == "Shader_HD_DefaultUnit" || material.shader == "Shader_HD_Crystal";
}

bool HasLayersFor(const mdx::Material& material, ProfileId profile, const Context& context) {
    const bool hd = wantsHd(profile);
    for (const Layer& layer : material.layers) {
        if (IsHdLayer(material, layer, context.modelVersion) == hd) {
            return true;
        }
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

    if (profile == ProfileId::Wc3Reforged) {
        importPbr(layers, context, common, out, ordinalOfLayer);
    } else if (StackCollapses(layers)) {
        importCombiners(layers, context, common, out);
    } else {
        importComposite(layers, context, common, out);
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

    // The native block holds only this profile's layers: that is what makes
    // `DeriveProfile(Wc3Reforged -> Wc3Classic)` a layer filter (§7.3).
    native::MdxMaterial block;
    CopyToNative(material, block);
    block.sourceVersion = context.modelVersion;
    std::vector<native::MdxLayer> kept;
    kept.reserve(layers.size());
    for (std::size_t i = 0; i < material.layers.size() && i < block.layers.size(); ++i) {
        if (IsHdLayer(material, material.layers[i], context.modelVersion) != wantsHd(profile)) {
            continue;
        }
        // The mirror's `isHd` is normalised on the way in, so a block written
        // from a v900 file — where the flag was never on disk — still says which
        // profile it belongs to.
        block.layers[i].isHd = wantsHd(profile);
        kept.push_back(std::move(block.layers[i]));
    }
    block.layers = std::move(kept);
    result.SetNativeInSync(std::move(block));

    if (layerOrdinals != nullptr) {
        layerOrdinals->assign(material.layers.size(), kInvalidIndex);
        std::size_t filtered = 0;
        for (std::size_t i = 0; i < material.layers.size(); ++i) {
            if (IsHdLayer(material, material.layers[i], context.modelVersion) != wantsHd(profile)) {
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

void exportFromNative(const native::MdxMaterial& block, mdx::Material& dst) {
    CopyFromNative(block, dst);
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
void pushLayer(Layer layer, Layer::FilterMode mode, BlendMode blend, mdx::Material& dst) {
    if (mode == Layer::FilterMode::None) {
        dst.layers.clear();
    }
    layer.filterMode = dst.layers.empty() ? filterModeFor(blend) : mode;
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
                     const Context& context, mdx::Material& dst, Diagnostics& out) {
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
        pushLayer(std::move(layer),
                  entry.target == SurfaceChannel::Color ? filterModeFor(entry.op)
                                                        : Layer::FilterMode::Additive,
                  common.blend, dst);
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
    case CombinerOp::Decal:
    case CombinerOp::Fade:
        return Layer::FilterMode::Blend;
    case CombinerOp::Pass:
    case CombinerOp::Count:
        break;
    }
    return std::nullopt;
}

void exportCombiners(const CombinersBody& body, const CommonMaterial& common,
                     const Context& context, mdx::Material& dst, Diagnostics& out) {
    // The §7.2.2 inverse: a stage becomes a layer with the matching filter mode,
    // and the layer that starts the stack takes the material's blend rather than
    // an unconditional `None` — a chain seeded by an opaque stage still meets the
    // scene however its header says, and Diablo III's additive wings met it as
    // solid plates while that was hard-coded.
    for (std::size_t i = 0; i < body.stages.size(); ++i) {
        const CombinerStage& stage = body.stages[i];
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
                out.warn(DiagCode::LayerDropped,
                         "combiner stage " + number(i) +
                             " passes the colour through, and an MDX layer is a draw: there is "
                             "no pass that contributes nothing",
                         layerRef(static_cast<u32>(i)));
                continue;
            }
            mode = Layer::FilterMode::None;
        }
        Layer layer;
        layer.textureId = context.toMdx(stage.input.texture);
        layer.coordId = stage.input.uvSet;
        layer.alpha = stage.input.weight;
        pushLayer(std::move(layer), *mode, common.blend, dst);
    }
}

void exportPbr(const PbrDeferredBody& body, const Context& context, u32 modelVersion,
               mdx::Material& dst, Diagnostics& out) {
    Layer layer;
    layer.is_hd = true;
    layer.shader = Layer::ShaderType::HD;
    layer.filterMode = Layer::FilterMode::None;

    if (modelVersion >= 1200) {
        for (const auto& [slot, input] : body.slots) {
            const Layer::SlotType type = slotTypeFor(slot);
            if (type == Layer::SlotType::Unknown) {
                out.warn(DiagCode::LayerDropped,
                         std::string("PBR slot ") + ToString(slot) + " has no MDX SlotType");
                continue;
            }
            Layer::SubTexture sub;
            sub.textureId = context.toMdx(input.texture);
            sub.slot = type;
            layer.subTextures.push_back(std::move(sub));
            layer.coordId = input.uvSet;
            layer.alpha = input.weight;
        }
        dst.layers.push_back(std::move(layer));
        return;
    }

    // Pre-1200 has no sub-texture array: the slots go back out as one layer
    // each, in SlotType order, which is the convention import read them by.
    for (u32 slot = 0; slot < static_cast<u32>(kHdPositionalSlotCount); ++slot) {
        const TextureInput* input = body.find(pbrSlotFor(static_cast<Layer::SlotType>(slot)));
        Layer positional = layer;
        positional.textureId = input != nullptr ? context.toMdx(input->texture) : 0;
        positional.coordId = input != nullptr ? input->uvSet : 0;
        positional.alpha = input != nullptr ? input->weight : 1.0f;
        dst.layers.push_back(std::move(positional));
    }
}

} // namespace

mdx::Material ExportMaterial(const Material& material, ProfileId profile, const Context& context,
                             Diagnostics& out) {
    mdx::Material dst;

    // §7.1: a native block that is not stale IS the answer. The kind mapping
    // below is the fallback path, and only that.
    if (material.hasNative() && material.sync() != NativeSync::CommonEdited) {
        if (material.nativeKind() == NativeKind::Mdx) {
            exportFromNative(std::get<native::MdxMaterial>(material.Native()), dst);
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

    switch (common.kind()) {
    case MaterialKind::Composite:
        exportComposite(*common.composite(), common, context, dst, out);
        break;
    case MaterialKind::Combiners:
        exportCombiners(*common.combiners(), common, context, dst, out);
        break;
    case MaterialKind::PBRDeferred:
        exportPbr(*common.pbr(), context, context.modelVersion, dst, out);
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
    return dst;
}

} // namespace mdx_core
} // namespace wem
} // namespace models
} // namespace whiteout
