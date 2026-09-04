// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/retarget.h>

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace whiteout {
namespace models {
namespace wem {

namespace {

std::string number(u64 value) {
    return std::to_string(value);
}

// ---------------------------------------------------------------------------
// Blend modes
// ---------------------------------------------------------------------------

/// Where a mode goes when the target cannot write it, best first. The chains are
/// short on purpose: every step is a visible change and the caller is told which
/// one was taken.
std::span<const BlendMode> fallbacksFor(BlendMode mode) {
    static const BlendMode kOpaque[] = {BlendMode::Opaque};
    static const BlendMode kAlphaKey[] = {BlendMode::Transparent, BlendMode::AlphaBlend,
                                          BlendMode::Opaque};
    static const BlendMode kTransparent[] = {BlendMode::AlphaKey, BlendMode::AlphaBlend,
                                             BlendMode::Opaque};
    static const BlendMode kAlphaBlend[] = {BlendMode::AlphaKey, BlendMode::Transparent,
                                            BlendMode::Opaque};
    static const BlendMode kAdditive[] = {BlendMode::AdditiveAlpha, BlendMode::BlendAdd,
                                          BlendMode::AlphaBlend};
    static const BlendMode kAdditiveAlpha[] = {BlendMode::Additive, BlendMode::BlendAdd,
                                               BlendMode::AlphaBlend};
    static const BlendMode kBlendAdd[] = {BlendMode::Additive, BlendMode::AdditiveAlpha,
                                          BlendMode::AlphaBlend};
    static const BlendMode kModulate[] = {BlendMode::Modulate2x, BlendMode::AlphaBlend};
    static const BlendMode kModulate2x[] = {BlendMode::Modulate, BlendMode::AlphaBlend};
    static const BlendMode kPremultiplied[] = {BlendMode::AlphaBlend, BlendMode::Additive};

    switch (mode) {
    case BlendMode::Opaque:
        return std::span<const BlendMode>(kOpaque, 1);
    case BlendMode::AlphaKey:
        return std::span<const BlendMode>(kAlphaKey, 3);
    case BlendMode::Transparent:
        return std::span<const BlendMode>(kTransparent, 3);
    case BlendMode::AlphaBlend:
        return std::span<const BlendMode>(kAlphaBlend, 3);
    case BlendMode::Additive:
        return std::span<const BlendMode>(kAdditive, 3);
    case BlendMode::AdditiveAlpha:
        return std::span<const BlendMode>(kAdditiveAlpha, 3);
    case BlendMode::BlendAdd:
        return std::span<const BlendMode>(kBlendAdd, 3);
    case BlendMode::Modulate:
        return std::span<const BlendMode>(kModulate, 2);
    case BlendMode::Modulate2x:
        return std::span<const BlendMode>(kModulate2x, 2);
    case BlendMode::PremultipliedAlpha:
        return std::span<const BlendMode>(kPremultiplied, 2);
    }
    return std::span<const BlendMode>(kOpaque, 1);
}

BlendMode nearestBlend(const ProfileDesc& desc, BlendMode mode) {
    if (desc.acceptsBlendMode(mode)) {
        return mode;
    }
    for (BlendMode candidate : fallbacksFor(mode)) {
        if (desc.acceptsBlendMode(candidate)) {
            return candidate;
        }
    }
    return BlendMode::Opaque;
}

// ---------------------------------------------------------------------------
// Kind conversion
//
// The four bodies say four different things, so every cross-kind move loses
// something. What matters is that each loss is named: a dropped slot is
// `LayerDropped`, an op approximated into a neighbouring vocabulary is
// `LossyKindConversion`, and the caller can count both.
// ---------------------------------------------------------------------------

struct KindContext {
    Diagnostics* out = nullptr;
    ElementRef where;
    ProfileId profile = ProfileId::Count;

    void dropped(const std::string& what) const {
        out->warn(DiagCode::LayerDropped, what + " has no place in the target kind", where,
                  profile);
    }
    void approximated(const std::string& what) const {
        out->warn(DiagCode::LossyKindConversion, what, where, profile);
    }
};

std::optional<SurfaceChannel> channelOf(LegacySlot slot) {
    switch (slot) {
    case LegacySlot::Diffuse:
        return SurfaceChannel::Color;
    case LegacySlot::Normal:
        return SurfaceChannel::Normal;
    case LegacySlot::Specular:
        return SurfaceChannel::Specular;
    case LegacySlot::Emissive:
        return SurfaceChannel::Emissive;
    case LegacySlot::Environment:
        return SurfaceChannel::Environment;
    case LegacySlot::AmbientOcclusion:
        return SurfaceChannel::AmbientOcclusion;
    // Gloss, Height and Lightmap are the legacy-only slots no channel feeds —
    // the direction `Flatten` cannot invert.
    case LegacySlot::Gloss:
    case LegacySlot::Height:
    case LegacySlot::Lightmap:
    case LegacySlot::Count:
        break;
    }
    return std::nullopt;
}

std::optional<SurfaceChannel> channelOf(PbrSlot slot) {
    switch (slot) {
    case PbrSlot::BaseColor:
        return SurfaceChannel::Color;
    case PbrSlot::Normal:
        return SurfaceChannel::Normal;
    case PbrSlot::Emissive:
        return SurfaceChannel::Emissive;
    case PbrSlot::Environment:
        return SurfaceChannel::Environment;
    case PbrSlot::AmbientOcclusion:
        return SurfaceChannel::AmbientOcclusion;
    case PbrSlot::Orm:
    case PbrSlot::Metallic:
    case PbrSlot::Roughness:
    case PbrSlot::TeamColorMask:
    case PbrSlot::Count:
        break;
    }
    return std::nullopt;
}

std::optional<LegacySlot> legacyOf(PbrSlot slot) {
    const std::optional<SurfaceChannel> channel = channelOf(slot);
    if (!channel.has_value()) {
        return std::nullopt;
    }
    switch (*channel) {
    case SurfaceChannel::Color:
        return LegacySlot::Diffuse;
    case SurfaceChannel::Normal:
        return LegacySlot::Normal;
    case SurfaceChannel::Emissive:
        return LegacySlot::Emissive;
    case SurfaceChannel::Environment:
        return LegacySlot::Environment;
    case SurfaceChannel::AmbientOcclusion:
        return LegacySlot::AmbientOcclusion;
    case SurfaceChannel::Specular:
    case SurfaceChannel::Count:
        break;
    }
    return std::nullopt;
}

std::optional<PbrSlot> pbrOf(SurfaceChannel channel) {
    switch (channel) {
    case SurfaceChannel::Color:
        return PbrSlot::BaseColor;
    case SurfaceChannel::Normal:
        return PbrSlot::Normal;
    case SurfaceChannel::Emissive:
        return PbrSlot::Emissive;
    case SurfaceChannel::Environment:
        return PbrSlot::Environment;
    case SurfaceChannel::AmbientOcclusion:
        return PbrSlot::AmbientOcclusion;
    // A spec/gloss term has no PBR slot; roughness is not its inverse in any way
    // this layer could compute.
    case SurfaceChannel::Specular:
    case SurfaceChannel::Count:
        break;
    }
    return std::nullopt;
}

CompositeOp compositeOpOf(CombinerOp op, const KindContext& ctx) {
    switch (op) {
    case CombinerOp::Opaque:
        return CompositeOp::Set;
    case CombinerOp::Mod:
        return CompositeOp::Modulate;
    case CombinerOp::Mod2x:
        return CompositeOp::Modulate2x;
    case CombinerOp::Add:
        return CompositeOp::Add;
    case CombinerOp::AddAlpha:
        return CompositeOp::AddAlpha;
    case CombinerOp::Decal:
        ctx.approximated("combiner op 'decal' became alpha_blend");
        return CompositeOp::AlphaBlend;
    case CombinerOp::Fade:
        ctx.approximated("combiner op 'fade' became alpha_blend");
        return CompositeOp::AlphaBlend;
    // `Pass` never reaches here: a stage that does not touch the channel is not
    // a layer of it, so the caller drops it rather than folding an identity.
    case CombinerOp::Pass:
    case CombinerOp::Count:
        break;
    }
    return CompositeOp::Set;
}

CombinerOp combinerOpOf(CompositeOp op, const KindContext& ctx) {
    switch (op) {
    case CompositeOp::Set:
        return CombinerOp::Opaque;
    case CompositeOp::Modulate:
        return CombinerOp::Mod;
    case CompositeOp::Modulate2x:
        return CombinerOp::Mod2x;
    case CompositeOp::Add:
        return CombinerOp::Add;
    case CompositeOp::AddAlpha:
        return CombinerOp::AddAlpha;
    case CompositeOp::AlphaBlend:
        return CombinerOp::Fade;
    case CompositeOp::AlphaKey:
        ctx.approximated("composite op 'alpha_key' became opaque; the key is on the header");
        return CombinerOp::Opaque;
    case CompositeOp::Count:
        break;
    }
    return CombinerOp::Opaque;
}

/// The first layer targeting each channel, in body order. What the two slot-map
/// kinds can hold of an ordered stack; everything after the first is reported.
std::vector<std::pair<SurfaceChannel, const TextureInput*>> firstPerChannel(
    const CompositeBody& body, const KindContext& ctx) {
    std::vector<std::pair<SurfaceChannel, const TextureInput*>> out;
    bool taken[static_cast<std::size_t>(SurfaceChannel::Count)] = {};
    for (const CompositeLayer& layer : body.layers) {
        const std::size_t channel = static_cast<std::size_t>(layer.target);
        if (channel >= static_cast<std::size_t>(SurfaceChannel::Count)) {
            continue;
        }
        if (taken[channel]) {
            ctx.dropped(std::string("a second '") + ToString(layer.target) + "' layer");
            continue;
        }
        taken[channel] = true;
        out.emplace_back(layer.target, &layer.input);
    }
    return out;
}

CompositeBody toComposite(const CommonMaterial& source, const KindContext& ctx) {
    CompositeBody out;
    if (const CombinersBody* combiners = source.combiners()) {
        out.diffuseFactor = combiners->diffuseFactor;
        out.emissiveFactor = combiners->emissiveFactor;
        for (const CombinerStage& stage : combiners->stages) {
            // A stage whose colour op is `Pass` touches alpha alone, and a
            // composite stack has no alpha-only layer to put it in.
            if (stage.rgb == CombinerOp::Pass) {
                continue;
            }
            CompositeLayer layer;
            layer.input = stage.input;
            layer.target = SurfaceChannel::Color;
            layer.op = compositeOpOf(stage.rgb, ctx);
            out.layers.push_back(layer);
        }
        // Stage 0 seeds the register, which is exactly what `Set` means.
        if (!out.layers.empty()) {
            out.layers[0].op = CompositeOp::Set;
        }
        return out;
    }
    if (const LegacyDeferredBody* legacy = source.legacy()) {
        out.diffuseFactor = legacy->diffuseFactor;
        out.emissiveFactor = legacy->emissiveFactor;
        out.specularFactor = legacy->specularFactor;
        out.specularExponent = legacy->specularExponent;
        out.environmentFactor = legacy->environmentFactor;
        for (const auto& entry : legacy->slots) {
            const std::optional<SurfaceChannel> channel = channelOf(entry.first);
            if (!channel.has_value()) {
                ctx.dropped(std::string("legacy slot '") + ToString(entry.first) + "'");
                continue;
            }
            CompositeLayer layer;
            layer.input = entry.second;
            layer.target = *channel;
            layer.op = CompositeOp::Set;
            out.layers.push_back(layer);
        }
        return out;
    }
    if (const PbrDeferredBody* pbr = source.pbr()) {
        out.diffuseFactor = pbr->baseColorFactor;
        out.emissiveFactor =
            Vector4f{pbr->emissiveFactor.x, pbr->emissiveFactor.y, pbr->emissiveFactor.z, 1.0f};
        for (const auto& entry : pbr->slots) {
            const std::optional<SurfaceChannel> channel = channelOf(entry.first);
            if (!channel.has_value()) {
                ctx.dropped(std::string("pbr slot '") + ToString(entry.first) + "'");
                continue;
            }
            CompositeLayer layer;
            layer.input = entry.second;
            layer.target = *channel;
            layer.op = CompositeOp::Set;
            out.layers.push_back(layer);
        }
        return out;
    }
    if (const CompositeBody* composite = source.composite()) {
        out = *composite;
    }
    return out;
}

CombinersBody toCombiners(const CommonMaterial& source, const KindContext& ctx) {
    CombinersBody out;
    if (const CompositeBody* composite = source.composite()) {
        out.diffuseFactor = composite->diffuseFactor;
        out.emissiveFactor = composite->emissiveFactor;
        for (const CompositeLayer& layer : composite->layers) {
            if (layer.target != SurfaceChannel::Color) {
                // A combiner chain has one register and no channels, so anything
                // that was not building the colour has nowhere to go.
                ctx.dropped(std::string("a '") + ToString(layer.target) + "' layer");
                continue;
            }
            CombinerStage stage;
            stage.input = layer.input;
            stage.rgb = combinerOpOf(layer.op, ctx);
            stage.alpha = stage.rgb;
            out.stages.push_back(stage);
        }
        if (!out.stages.empty()) {
            out.stages[0].rgb = CombinerOp::Opaque;
            out.stages[0].alpha = CombinerOp::Opaque;
        }
        return out;
    }
    if (const CombinersBody* combiners = source.combiners()) {
        return *combiners;
    }

    // A slot map into a chain: only the colour slot has an unambiguous place, and
    // saying so beats inventing a stage order the source never had.
    if (const LegacyDeferredBody* legacy = source.legacy()) {
        out.diffuseFactor = legacy->diffuseFactor;
        out.emissiveFactor = legacy->emissiveFactor;
        for (const auto& entry : legacy->slots) {
            if (entry.first == LegacySlot::Diffuse) {
                CombinerStage stage;
                stage.input = entry.second;
                out.stages.push_back(stage);
            } else {
                ctx.dropped(std::string("legacy slot '") + ToString(entry.first) + "'");
            }
        }
        return out;
    }
    if (const PbrDeferredBody* pbr = source.pbr()) {
        out.diffuseFactor = pbr->baseColorFactor;
        for (const auto& entry : pbr->slots) {
            if (entry.first == PbrSlot::BaseColor) {
                CombinerStage stage;
                stage.input = entry.second;
                out.stages.push_back(stage);
            } else {
                ctx.dropped(std::string("pbr slot '") + ToString(entry.first) + "'");
            }
        }
    }
    return out;
}

LegacyDeferredBody toLegacy(const CommonMaterial& source, const KindContext& ctx) {
    if (const LegacyDeferredBody* legacy = source.legacy()) {
        return *legacy;
    }
    LegacyDeferredBody out;
    if (const CompositeBody* composite = source.composite()) {
        if (std::optional<LegacyDeferredBody> flat = Flatten(*composite)) {
            return *flat;
        }
        // Not degenerate, so the fold is real and a slot map cannot hold it. Keep
        // the first layer of each channel and say what went.
        ctx.approximated("composite stack flattened to one layer per channel");
        out.diffuseFactor = composite->diffuseFactor;
        out.emissiveFactor = composite->emissiveFactor;
        out.specularFactor = composite->specularFactor;
        out.specularExponent = composite->specularExponent;
        out.environmentFactor = composite->environmentFactor;
        for (const auto& entry : firstPerChannel(*composite, ctx)) {
            switch (entry.first) {
            case SurfaceChannel::Color:
                out.set(LegacySlot::Diffuse, *entry.second);
                break;
            case SurfaceChannel::Normal:
                out.set(LegacySlot::Normal, *entry.second);
                break;
            case SurfaceChannel::Specular:
                out.set(LegacySlot::Specular, *entry.second);
                break;
            case SurfaceChannel::Emissive:
                out.set(LegacySlot::Emissive, *entry.second);
                break;
            case SurfaceChannel::Environment:
                out.set(LegacySlot::Environment, *entry.second);
                break;
            case SurfaceChannel::AmbientOcclusion:
                out.set(LegacySlot::AmbientOcclusion, *entry.second);
                break;
            case SurfaceChannel::Count:
                break;
            }
        }
        return out;
    }
    if (const CombinersBody* combiners = source.combiners()) {
        out.diffuseFactor = combiners->diffuseFactor;
        out.emissiveFactor = combiners->emissiveFactor;
        for (std::size_t i = 0; i < combiners->stages.size(); ++i) {
            if (i == 0) {
                out.set(LegacySlot::Diffuse, combiners->stages[i].input);
            } else {
                ctx.dropped("combiner stage " + number(i));
            }
        }
        return out;
    }
    if (const PbrDeferredBody* pbr = source.pbr()) {
        out.diffuseFactor = pbr->baseColorFactor;
        out.emissiveFactor =
            Vector4f{pbr->emissiveFactor.x, pbr->emissiveFactor.y, pbr->emissiveFactor.z, 1.0f};
        for (const auto& entry : pbr->slots) {
            const std::optional<LegacySlot> slot = legacyOf(entry.first);
            if (!slot.has_value()) {
                ctx.dropped(std::string("pbr slot '") + ToString(entry.first) + "'");
                continue;
            }
            out.set(*slot, entry.second);
        }
    }
    return out;
}

PbrDeferredBody toPbr(const CommonMaterial& source, const KindContext& ctx) {
    if (const PbrDeferredBody* pbr = source.pbr()) {
        return *pbr;
    }
    PbrDeferredBody out;
    if (const LegacyDeferredBody* legacy = source.legacy()) {
        out.baseColorFactor = legacy->diffuseFactor;
        out.emissiveFactor =
            Vector3f{legacy->emissiveFactor.x, legacy->emissiveFactor.y, legacy->emissiveFactor.z};
        for (const auto& entry : legacy->slots) {
            const std::optional<SurfaceChannel> channel = channelOf(entry.first);
            const std::optional<PbrSlot> slot =
                channel.has_value() ? pbrOf(*channel) : std::nullopt;
            if (!slot.has_value()) {
                ctx.dropped(std::string("legacy slot '") + ToString(entry.first) + "'");
                continue;
            }
            out.set(*slot, entry.second);
        }
        return out;
    }
    if (const CombinersBody* combiners = source.combiners()) {
        out.baseColorFactor = combiners->diffuseFactor;
        for (std::size_t i = 0; i < combiners->stages.size(); ++i) {
            if (i == 0) {
                out.set(PbrSlot::BaseColor, combiners->stages[i].input);
            } else {
                ctx.dropped("combiner stage " + number(i));
            }
        }
        return out;
    }
    if (const CompositeBody* composite = source.composite()) {
        out.baseColorFactor = composite->diffuseFactor;
        out.emissiveFactor = Vector3f{composite->emissiveFactor.x, composite->emissiveFactor.y,
                                      composite->emissiveFactor.z};
        for (const auto& entry : firstPerChannel(*composite, ctx)) {
            const std::optional<PbrSlot> slot = pbrOf(entry.first);
            if (!slot.has_value()) {
                ctx.dropped(std::string("a '") + ToString(entry.first) + "' layer");
                continue;
            }
            out.set(*slot, *entry.second);
        }
    }
    return out;
}

/// Which kind a material becomes when the target does not accept its own. The
/// order is "nearest first": ordered kinds prefer the other ordered kind, slot
/// maps prefer the other slot map.
MaterialKind targetKindFor(MaterialKind source, MaterialKindMask accepted) {
    static const MaterialKind kFromComposite[] = {
        MaterialKind::Combiners, MaterialKind::LegacyDeferred, MaterialKind::PBRDeferred};
    static const MaterialKind kFromCombiners[] = {
        MaterialKind::Composite, MaterialKind::LegacyDeferred, MaterialKind::PBRDeferred};
    static const MaterialKind kFromLegacy[] = {MaterialKind::PBRDeferred, MaterialKind::Composite,
                                               MaterialKind::Combiners};
    static const MaterialKind kFromPbr[] = {MaterialKind::LegacyDeferred, MaterialKind::Composite,
                                            MaterialKind::Combiners};

    const MaterialKind* order = kFromComposite;
    switch (source) {
    case MaterialKind::Composite:
        order = kFromComposite;
        break;
    case MaterialKind::Combiners:
        order = kFromCombiners;
        break;
    case MaterialKind::LegacyDeferred:
        order = kFromLegacy;
        break;
    case MaterialKind::PBRDeferred:
    case MaterialKind::Count:
        order = kFromPbr;
        break;
    }
    for (std::size_t i = 0; i < 3; ++i) {
        if (HasMaterialKind(accepted, order[i])) {
            return order[i];
        }
    }
    return source;
}

void convertKind(CommonMaterial& common, const ProfileDesc& desc, const KindContext& ctx) {
    if (HasMaterialKind(desc.commonKinds, common.kind())) {
        return;
    }
    if (desc.commonKinds == 0) {
        return;
    }

    if (const CompositeBody* composite = common.composite()) {
        if (HasMaterialKind(desc.commonKinds, MaterialKind::LegacyDeferred)) {
            // §7.2.3's named refinement first: a degenerate composite is *already*
            // a slot map, so when the target takes one this is the lossless move.
            if (std::optional<LegacyDeferredBody> flat = Flatten(*composite)) {
                common.body = *flat;
                return;
            }
            // Not degenerate. A chain keeps the fold but has one register and no
            // channels, so a stack that builds normals or emissives loses every
            // one of them there and only its Color fold here — take the slot map.
            bool multiChannel = false;
            for (const CompositeLayer& layer : composite->layers) {
                multiChannel = multiChannel || layer.target != SurfaceChannel::Color;
            }
            if (multiChannel) {
                ctx.out->info(DiagCode::LossyKindConversion,
                              std::string("kind ") + ToString(common.kind()) + " -> " +
                                  ToString(MaterialKind::LegacyDeferred),
                              ctx.where, ctx.profile);
                common.body = toLegacy(common, ctx);
                return;
            }
        }
    }

    const MaterialKind target = targetKindFor(common.kind(), desc.commonKinds);
    if (target == common.kind()) {
        return;
    }
    ctx.out->info(DiagCode::LossyKindConversion,
                  std::string("kind ") + ToString(common.kind()) + " -> " + ToString(target),
                  ctx.where, ctx.profile);

    switch (target) {
    case MaterialKind::Composite:
        common.body = toComposite(common, ctx);
        break;
    case MaterialKind::Combiners:
        common.body = toCombiners(common, ctx);
        break;
    case MaterialKind::LegacyDeferred:
        common.body = toLegacy(common, ctx);
        break;
    case MaterialKind::PBRDeferred:
        common.body = toPbr(common, ctx);
        break;
    case MaterialKind::Count:
        break;
    }
}

// ---------------------------------------------------------------------------
// Textures
// ---------------------------------------------------------------------------

bool sameTexture(const TextureRef& a, const TextureRef& b) {
    if (KeyKind(a.key) != KeyKind(b.key)) {
        return false;
    }
    switch (KeyKind(a.key)) {
    case TextureKeyKind::Path:
        return std::get<TexturePath>(a.key).value == std::get<TexturePath>(b.key).value;
    case TextureKeyKind::FileDataId:
        return std::get<TextureFileDataId>(a.key).value == std::get<TextureFileDataId>(b.key).value;
    case TextureKeyKind::SnoId:
        return std::get<TextureSnoId>(a.key).group == std::get<TextureSnoId>(b.key).group &&
               std::get<TextureSnoId>(a.key).id == std::get<TextureSnoId>(b.key).id;
    case TextureKeyKind::None:
        break;
    }
    return a.path == b.path;
}

/// Merges @p from's table into @p into and returns `remap[fromIndex]`.
std::vector<u32> mergeTextures(std::vector<TextureRef>& into, const std::vector<TextureRef>& from) {
    std::vector<u32> remap(from.size(), kInvalidIndex);
    for (std::size_t i = 0; i < from.size(); ++i) {
        for (std::size_t j = 0; j < into.size(); ++j) {
            if (sameTexture(into[j], from[i])) {
                remap[i] = static_cast<u32>(j);
                break;
            }
        }
        if (remap[i] == kInvalidIndex) {
            into.push_back(from[i]);
            remap[i] = static_cast<u32>(into.size() - 1);
        }
    }
    return remap;
}

void remapTextures(CommonMaterial& common, const std::vector<u32>& remap, Diagnostics& out,
                   const ElementRef& where, ProfileId profile) {
    for (u32 ordinal = 0; ordinal < common.ordinalCount(); ++ordinal) {
        TextureInput* input = common.inputAt(ordinal);
        if (input == nullptr || !input->hasTexture()) {
            continue;
        }
        if (input->texture >= remap.size()) {
            out.error(DiagCode::TextureUnresolved,
                      "texture index " + number(input->texture) + " is not in the source table",
                      where, profile);
            input->texture = kInvalidIndex;
            continue;
        }
        input->texture = remap[input->texture];
    }
}

// ---------------------------------------------------------------------------
// Geometry comparison
// ---------------------------------------------------------------------------

bool geometryMatches(const Model& a, const Model& b, Diagnostics& out, u32 modelIndex) {
    if (a.meshes.size() != b.meshes.size()) {
        out.error(DiagCode::GeometryMismatch,
                  "model holds " + number(a.meshes.size()) + " meshes, the import " +
                      number(b.meshes.size()),
                  ElementRef(ElementKind::Document, modelIndex));
        return false;
    }
    for (std::size_t m = 0; m < a.meshes.size(); ++m) {
        const Mesh& lhs = a.meshes[m];
        const Mesh& rhs = b.meshes[m];
        const ElementRef where(ElementKind::Mesh, static_cast<u32>(m));
        if (lhs.vertexCount() != rhs.vertexCount() || lhs.faceCount() != rhs.faceCount()) {
            out.error(DiagCode::GeometryMismatch,
                      "mesh has " + number(lhs.vertexCount()) + " vertices / " +
                          number(lhs.faceCount()) + " faces, the import " +
                          number(rhs.vertexCount()) + " / " + number(rhs.faceCount()),
                      where);
            return false;
        }
        if (lhs.sections.size() != rhs.sections.size()) {
            out.error(DiagCode::GeometryMismatch,
                      "mesh has " + number(lhs.sections.size()) + " sections, the import " +
                          number(rhs.sections.size()),
                      where);
            return false;
        }
        for (std::size_t s = 0; s < lhs.sections.size(); ++s) {
            if (lhs.sections[s].name == rhs.sections[s].name) {
                continue;
            }
            out.error(DiagCode::GeometryMismatch,
                      "section " + number(s) + " is '" + lhs.sections[s].name + "' here and '" +
                          rhs.sections[s].name + "' in the import",
                      ElementRef(ElementKind::Section, static_cast<u32>(s), static_cast<u32>(m)));
            return false;
        }
    }
    return true;
}

} // namespace

// ============================================================================
// DeriveProfile
// ============================================================================

DeriveResult DeriveProfile(Document& document, ProfileId from, ProfileId to,
                           const RetargetOptions& options) {
    DeriveResult result;
    if (!document.carries(from)) {
        result.diagnostics.error(DiagCode::ProfileNotCarried,
                                 std::string("the document does not carry ") + ToString(from),
                                 ElementRef(), from);
        return result;
    }
    if (static_cast<u32>(to) >= static_cast<u32>(ProfileId::Count)) {
        result.diagnostics.error(DiagCode::ProfileNotCarried, "target profile is out of range",
                                 ElementRef(), to);
        return result;
    }
    if (options.rescale) {
        result.diagnostics.info(
            DiagCode::OperationUnsupported,
            "rescale is ignored: geometry is shared by every set in the model, so a "
            "material-set derive does not get to change it — Retarget does",
            ElementRef(), to);
    }

    const ProfileDesc& targetDesc = Profile(to);
    const ProfileDesc& sourceDesc = Profile(from);
    const bool sharedNative = sourceDesc.nativeMaterialKind == targetDesc.nativeMaterialKind &&
                              targetDesc.nativeMaterialKind != NativeKind::None;

    for (std::size_t modelIndex = 0; modelIndex < document.models.size(); ++modelIndex) {
        Model& model = document.models[modelIndex];
        const ProfileMaterialSet* source = model.setFor(from);
        if (source == nullptr) {
            result.diagnostics.info(
                DiagCode::ProfileCoverageIncomplete,
                std::string("model has no ") + ToString(from) + " set to derive from",
                ElementRef(ElementKind::Document, static_cast<u32>(modelIndex)), from);
            continue;
        }

        ProfileMaterialSet derived;
        derived.profile = to;

        // --- looks ------------------------------------------------------------
        u32 keptLook = 0;
        if (targetDesc.supportsLooks) {
            derived.looks = source->looks;
        } else {
            keptLook = options.keepLook < source->looks.size() ? options.keepLook : 0;
            derived.looks.looks.push_back(source->looks.looks[keptLook]);
            for (std::size_t look = 0; look < source->looks.size(); ++look) {
                if (look == keptLook) {
                    continue;
                }
                result.diagnostics.warn(DiagCode::LookDropped,
                                        "look '" + source->looks.looks[look].name + "' — " +
                                            ToString(to) + " has no look table",
                                        ElementRef(ElementKind::Look, static_cast<u32>(look)), to);
            }
        }

        // --- materials ---------------------------------------------------------
        derived.materials.reserve(source->materials.size());
        for (std::size_t m = 0; m < source->materials.size(); ++m) {
            const Material& sourceMaterial = source->materials[m];
            const ElementRef where(ElementKind::Material, static_cast<u32>(m));
            const KindContext ctx{&result.diagnostics, where, to};

            Material material;
            material.name = sourceMaterial.name;
            CommonMaterial& common = material.InitCommon();
            common = sourceMaterial.Common();

            convertKind(common, targetDesc, ctx);

            const BlendMode blend = nearestBlend(targetDesc, common.blend);
            if (blend != common.blend) {
                result.diagnostics.warn(DiagCode::LossyBlendMode,
                                        std::string("blend '") + ToString(common.blend) +
                                            "' became '" + ToString(blend) + "'",
                                        where, to);
                common.blend = blend;
            }

            // A feature's `layer` is an ordinal into the body that just changed
            // shape, so anything pointing past the new end goes.
            const u32 ordinals = common.ordinalCount();
            std::vector<MaterialFeature> keptFeatures;
            keptFeatures.reserve(common.features.size());
            for (MaterialFeature& feature : common.features) {
                if (feature.layer != kWholeMaterial && feature.layer >= ordinals) {
                    result.diagnostics.warn(
                        DiagCode::FeatureDropped,
                        std::string(ToString(feature.kind())) + " feature targeted ordinal " +
                            number(feature.layer) + ", and the body now has " + number(ordinals),
                        ElementRef(ElementKind::Feature, static_cast<u32>(m), feature.id), to);
                    continue;
                }
                keptFeatures.push_back(std::move(feature));
            }
            common.features = std::move(keptFeatures);

            // --- the native block ----------------------------------------------
            if (sourceMaterial.hasNative()) {
                if (sharedNative && options.keepSharedNative) {
                    // §7.3's cheap-derive path. The *filter* — the `is_hd` layer
                    // partition, the version-gated M3 kinds — needs the native
                    // bodies the codegen produces, so today the block is carried
                    // whole and the filter lands with them.
                    material.SetNativeAuthoritative(sourceMaterial.Native());
                    result.diagnostics.info(DiagCode::LossyKindConversion,
                                            std::string("carried the shared ") +
                                                ToString(material.nativeKind()) +
                                                " native block unfiltered",
                                            where, to);
                } else {
                    result.diagnostics.warn(DiagCode::DroppedNativeBlock,
                                            std::string("a ") +
                                                ToString(sourceMaterial.nativeKind()) +
                                                " block has no place in a " + ToString(to) + " set",
                                            where, to);
                }
            }

            derived.materials.push_back(std::move(material));
        }

        // --- bindings -----------------------------------------------------------
        derived.slotBindings.resize(model.materialSlots.size());
        for (std::size_t slot = 0; slot < derived.slotBindings.size(); ++slot) {
            std::vector<u32>& byLook = derived.slotBindings[slot].byLook;
            byLook.assign(derived.looks.size(), kInvalidIndex);
            if (slot >= source->slotBindings.size()) {
                continue;
            }
            const std::vector<u32>& sourceByLook = source->slotBindings[slot].byLook;
            if (targetDesc.supportsLooks) {
                for (std::size_t look = 0; look < byLook.size() && look < sourceByLook.size();
                     ++look) {
                    byLook[look] = sourceByLook[look];
                }
            } else if (keptLook < sourceByLook.size()) {
                byLook[0] = sourceByLook[keptLook];
            }
        }

        // --- geometry limits: reported, never applied ---------------------------
        for (std::size_t meshIndex = 0; meshIndex < model.meshes.size(); ++meshIndex) {
            const Mesh& mesh = model.meshes[meshIndex];
            const ElementRef where(ElementKind::Mesh, static_cast<u32>(meshIndex));
            const u32 influences = mesh.skin.maxInfluences();
            if (influences > targetDesc.maxBoneInfluences) {
                result.diagnostics.warn(DiagCode::BoneInfluenceLimit,
                                        number(influences) + " influences exceeds " + ToString(to) +
                                            "'s " + number(targetDesc.maxBoneInfluences) +
                                            "; the geometry is shared and was not changed",
                                        where, to);
            }
            u32 uvSets = 0;
            while (uvSets < 8 &&
                   mesh.attributes.has(geom::names::uv(uvSets), geom::Domain::Halfedge)) {
                ++uvSets;
            }
            if (uvSets > targetDesc.maxUvSets) {
                result.diagnostics.warn(DiagCode::UvSetLimit,
                                        number(uvSets) + " uv sets exceeds " + ToString(to) +
                                            "'s " + number(targetDesc.maxUvSets) +
                                            "; the geometry is shared and was not changed",
                                        where, to);
            }
        }

        // --- the material channels ----------------------------------------------
        //
        // §10.8 names a channel's material by `(profile, slot, look)`, so a
        // derived set arrives with no animation at all: every material channel
        // in the table still names `from`, and an exporter asked for `to` skips
        // them all. That is not a small loss. World of Warcraft hides a
        // conditional batch by keying its `M2Color` alpha to zero — a lich's
        // glow and its shadow plane are written exactly so — and both drew at
        // full strength over the model for as long as the derived set had no
        // channel to hide them with. The scrolling UVs and per-unit fades went
        // the same way.
        //
        // Twins rather than a repoint, because the source set is still here and
        // still animated. Their sub-tracks are copied with them: a channel is
        // only a join key, and the curve lives in the clip.
        std::unordered_map<u32, u32> twinOfChannel;
        {
            // Deriving twice is a refresh here too, so anything a previous
            // derive left addressed to the target goes first — ids and all.
            std::unordered_set<u32> retired;
            for (const AnimChannel& channel : model.animChannels.channels) {
                if (IsMaterialTarget(channel.target.kind) &&
                    channel.target.material.profile == to) {
                    retired.insert(channel.id);
                }
            }
            if (!retired.empty()) {
                std::vector<AnimChannel> kept;
                kept.reserve(model.animChannels.channels.size());
                for (AnimChannel& channel : model.animChannels.channels) {
                    if (retired.count(channel.id) == 0) {
                        kept.push_back(std::move(channel));
                    }
                }
                model.animChannels.channels = std::move(kept);
                for (Clip& clip : document.clips) {
                    if (clip.model != modelIndex) {
                        continue;
                    }
                    for (SubTrackContainer& container : clip.containers) {
                        std::vector<SubTrack> keptTracks;
                        keptTracks.reserve(container.subTracks.size());
                        for (SubTrack& track : container.subTracks) {
                            if (retired.count(track.channel) == 0) {
                                keptTracks.push_back(std::move(track));
                            }
                        }
                        container.subTracks = std::move(keptTracks);
                    }
                }
            }

            std::vector<AnimChannel> twins;
            u32 nextId = model.animChannels.nextFreeId();
            for (const AnimChannel& source : model.animChannels.channels) {
                if (!IsMaterialTarget(source.target.kind) ||
                    source.target.material.profile != from) {
                    continue;
                }
                AnimChannel twin = source;
                twin.target.material.profile = to;
                if (!targetDesc.supportsLooks) {
                    if (source.target.material.look != keptLook) {
                        // The look it drove is not in this set; `LookDropped`
                        // above already said the look went.
                        continue;
                    }
                    twin.target.material.look = 0;
                }
                const u32 slot = twin.target.material.slot;
                const u32 look = twin.target.material.look;
                if (slot >= derived.slotBindings.size() ||
                    look >= derived.slotBindings[slot].byLook.size()) {
                    continue;
                }
                const u32 material = derived.slotBindings[slot].byLook[look];
                if (material >= derived.materials.size()) {
                    continue;
                }
                const CommonMaterial& common = derived.materials[material].Common();
                const ElementRef where(ElementKind::Slot, slot);
                if (source.target.kind == TrackTarget::Kind::MaterialLayer) {
                    // `kWholeMaterial` is not an ordinal and survives any kind
                    // change; an ordinal only survives one the body kept.
                    if (source.target.sub != kWholeMaterial &&
                        source.target.sub >= common.ordinalCount()) {
                        result.diagnostics.warn(
                            DiagCode::AnimTrackDropped,
                            std::string("a ") + ToString(source.target.channel) +
                                " track names ordinal " + number(source.target.sub) +
                                ", and the derived body has " + number(common.ordinalCount()),
                            where, to);
                        continue;
                    }
                } else {
                    bool alive = false;
                    for (const MaterialFeature& feature : common.features) {
                        alive = alive || feature.id == source.target.sub;
                    }
                    if (!alive) {
                        result.diagnostics.warn(
                            DiagCode::AnimTrackDropped,
                            std::string("a ") + ToString(source.target.channel) +
                                " track names feature " + number(source.target.sub) +
                                ", which the derived material dropped",
                            where, to);
                        continue;
                    }
                }
                twin.id = nextId++;
                twinOfChannel.emplace(source.id, twin.id);
                twins.push_back(std::move(twin));
            }
            for (AnimChannel& twin : twins) {
                model.animChannels.add(twin);
            }
        }
        if (!twinOfChannel.empty()) {
            for (Clip& clip : document.clips) {
                if (clip.model != modelIndex) {
                    continue;
                }
                for (SubTrackContainer& container : clip.containers) {
                    const std::size_t before = container.subTracks.size();
                    for (std::size_t t = 0; t < before; ++t) {
                        const auto twin = twinOfChannel.find(container.subTracks[t].channel);
                        if (twin == twinOfChannel.end()) {
                            continue;
                        }
                        SubTrack copy = container.subTracks[t];
                        copy.channel = twin->second;
                        container.subTracks.push_back(std::move(copy));
                    }
                }
            }
        }

        // Replace any set the target profile already had — deriving twice is a
        // refresh, not an accumulation.
        for (std::size_t i = 0; i < model.profileSets.size(); ++i) {
            if (model.profileSets[i].profile == to) {
                model.profileSets.erase(model.profileSets.begin() + static_cast<std::ptrdiff_t>(i));
                break;
            }
        }
        model.profileSets.push_back(std::move(derived));
    }

    document.declare(to);
    result.ok = true;
    return result;
}

// ============================================================================
// AddProfileFromImport
// ============================================================================

DeriveResult AddProfileFromImport(Document& document, ProfileId profile, const Document& imported) {
    DeriveResult result;
    if (static_cast<u32>(profile) >= static_cast<u32>(ProfileId::Count)) {
        result.diagnostics.error(DiagCode::ProfileNotCarried, "profile is out of range",
                                 ElementRef(), profile);
        return result;
    }
    if (document.models.size() != imported.models.size()) {
        result.diagnostics.error(DiagCode::GeometryMismatch,
                                 "document holds " + number(document.models.size()) +
                                     " models, the import " + number(imported.models.size()),
                                 ElementRef(ElementKind::Document, 0));
        return result;
    }

    for (std::size_t i = 0; i < document.models.size(); ++i) {
        if (!geometryMatches(document.models[i], imported.models[i], result.diagnostics,
                             static_cast<u32>(i))) {
            return result;
        }
    }

    const std::vector<u32> textureRemap = mergeTextures(document.textures, imported.textures);

    for (std::size_t i = 0; i < document.models.size(); ++i) {
        Model& model = document.models[i];
        const Model& source = imported.models[i];

        const ProfileMaterialSet* sourceSet = source.setFor(profile);
        if (sourceSet == nullptr && source.profileSets.size() == 1) {
            // The common case: a second file imported under its own profile, being
            // adopted as this one. Relabelling is the whole operation.
            sourceSet = &source.profileSets[0];
        }
        if (sourceSet == nullptr) {
            result.diagnostics.error(DiagCode::ProfileNotCarried,
                                     std::string("the import has no ") + ToString(profile) +
                                         " set, and more than one to guess from",
                                     ElementRef(ElementKind::Document, static_cast<u32>(i)),
                                     profile);
            return result;
        }

        ProfileMaterialSet adopted = *sourceSet;
        adopted.profile = profile;
        for (std::size_t m = 0; m < adopted.materials.size(); ++m) {
            // Not MutableCommon: the material's own sync state came across with it
            // and this is a table remap, not an authoring edit.
            remapTextures(adopted.materials[m].InitCommon(), textureRemap, result.diagnostics,
                          ElementRef(ElementKind::Material, static_cast<u32>(m)), profile);
        }

        // Slots join by NAME, because the two files numbered them independently.
        std::vector<SlotBinding> rebound(model.materialSlots.size());
        for (SlotBinding& binding : rebound) {
            binding.byLook.assign(adopted.looks.size(), kInvalidIndex);
        }
        for (std::size_t slot = 0; slot < source.materialSlots.size(); ++slot) {
            const u32 target = model.addSlot(source.materialSlots[slot]);
            if (target >= rebound.size()) {
                rebound.resize(target + 1);
                rebound[target].byLook.assign(adopted.looks.size(), kInvalidIndex);
            }
            if (slot < adopted.slotBindings.size()) {
                rebound[target] = adopted.slotBindings[slot];
            }
        }
        adopted.slotBindings = std::move(rebound);

        for (std::size_t s = 0; s < model.profileSets.size(); ++s) {
            if (model.profileSets[s].profile == profile) {
                model.profileSets.erase(model.profileSets.begin() + static_cast<std::ptrdiff_t>(s));
                break;
            }
        }
        model.profileSets.push_back(std::move(adopted));

        // Which sections the new profile draws is the *import's* answer, not
        // "all of them": a file that left a section out of every profile left it
        // out on purpose, and the geometries matched section for section.
        const ProfileMask bit = ProfileBit(profile);
        for (std::size_t mi = 0; mi < model.meshes.size(); ++mi) {
            for (std::size_t si = 0; si < model.meshes[mi].sections.size(); ++si) {
                if (source.meshes[mi].sections[si].profiles != kNoProfiles) {
                    model.meshes[mi].sections[si].profiles |= bit;
                }
            }
        }
    }

    document.declare(profile);
    result.ok = true;
    return result;
}

} // namespace wem
} // namespace models
} // namespace whiteout
