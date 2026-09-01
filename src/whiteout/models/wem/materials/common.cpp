// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/materials/common.h>

namespace whiteout {
namespace models {
namespace wem {

// ============================================================================
// Matrix3x2f
// ============================================================================

bool Matrix3x2f::isIdentity() const {
    return m[0][0] == 1.0f && m[0][1] == 0.0f && m[0][2] == 0.0f && m[1][0] == 0.0f &&
           m[1][1] == 1.0f && m[1][2] == 0.0f;
}

Vector2f Matrix3x2f::apply(const Vector2f& uv) const {
    return Vector2f{m[0][0] * uv.x + m[0][1] * uv.y + m[0][2],
                    m[1][0] * uv.x + m[1][1] * uv.y + m[1][2]};
}

// ============================================================================
// Names
// ============================================================================

const char* ToString(UVMappingMode mode) {
    switch (mode) {
    case UVMappingMode::ExplicitUV:
        return "explicit_uv";
    case UVMappingMode::EnvSphere:
        return "env_sphere";
    case UVMappingMode::EnvCube:
        return "env_cube";
    }
    return "invalid";
}

const char* ToString(SurfaceChannel channel) {
    switch (channel) {
    case SurfaceChannel::Color:
        return "color";
    case SurfaceChannel::Emissive:
        return "emissive";
    case SurfaceChannel::Specular:
        return "specular";
    case SurfaceChannel::Normal:
        return "normal";
    case SurfaceChannel::AmbientOcclusion:
        return "ambient_occlusion";
    case SurfaceChannel::Environment:
        return "environment";
    case SurfaceChannel::Count:
        break;
    }
    return "invalid";
}

const char* ToString(CompositeOp op) {
    switch (op) {
    case CompositeOp::Set:
        return "set";
    case CompositeOp::AlphaKey:
        return "alpha_key";
    case CompositeOp::AlphaBlend:
        return "alpha_blend";
    case CompositeOp::Add:
        return "add";
    case CompositeOp::AddAlpha:
        return "add_alpha";
    case CompositeOp::Modulate:
        return "modulate";
    case CompositeOp::Modulate2x:
        return "modulate_2x";
    case CompositeOp::Count:
        break;
    }
    return "invalid";
}

const char* ToString(CombinerOp op) {
    switch (op) {
    case CombinerOp::Opaque:
        return "opaque";
    case CombinerOp::Mod:
        return "mod";
    case CombinerOp::Mod2x:
        return "mod2x";
    case CombinerOp::Add:
        return "add";
    case CombinerOp::Decal:
        return "decal";
    case CombinerOp::Fade:
        return "fade";
    case CombinerOp::Pass:
        return "pass";
    case CombinerOp::Count:
        break;
    }
    return "invalid";
}

const char* ToString(LegacySlot slot) {
    switch (slot) {
    case LegacySlot::Diffuse:
        return "diffuse";
    case LegacySlot::Normal:
        return "normal";
    case LegacySlot::Specular:
        return "specular";
    case LegacySlot::Gloss:
        return "gloss";
    case LegacySlot::Emissive:
        return "emissive";
    case LegacySlot::Environment:
        return "environment";
    case LegacySlot::AmbientOcclusion:
        return "ambient_occlusion";
    case LegacySlot::Height:
        return "height";
    case LegacySlot::Lightmap:
        return "lightmap";
    case LegacySlot::Count:
        break;
    }
    return "invalid";
}

const char* ToString(PbrSlot slot) {
    switch (slot) {
    case PbrSlot::BaseColor:
        return "base_color";
    case PbrSlot::Normal:
        return "normal";
    case PbrSlot::Orm:
        return "orm";
    case PbrSlot::Metallic:
        return "metallic";
    case PbrSlot::Roughness:
        return "roughness";
    case PbrSlot::AmbientOcclusion:
        return "ambient_occlusion";
    case PbrSlot::Emissive:
        return "emissive";
    case PbrSlot::Environment:
        return "environment";
    case PbrSlot::TeamColorMask:
        return "team_color_mask";
    case PbrSlot::Count:
        break;
    }
    return "invalid";
}

const char* ToString(FeatureKind kind) {
    switch (kind) {
    case FeatureKind::Fresnel:
        return "fresnel";
    case FeatureKind::UvAnimation:
        return "uv_animation";
    case FeatureKind::Count:
        break;
    }
    return "invalid";
}

// ============================================================================
// Features (features.h)
// ============================================================================

const MaterialFeature* FindFeature(const std::vector<MaterialFeature>& features, FeatureKind kind,
                                   u32 layer) {
    for (const MaterialFeature& feature : features) {
        if (feature.kind() == kind && feature.layer == layer) {
            return &feature;
        }
    }
    return nullptr;
}

u32 NextFeatureId(const std::vector<MaterialFeature>& features) {
    u32 next = 0;
    for (const MaterialFeature& feature : features) {
        if (feature.id >= next) {
            next = feature.id + 1;
        }
    }
    return next;
}

// ============================================================================
// Bodies
// ============================================================================

std::vector<u32> CompositeBody::layersOf(SurfaceChannel channel) const {
    std::vector<u32> out;
    for (std::size_t i = 0; i < layers.size(); ++i) {
        if (layers[i].target == channel) {
            out.push_back(static_cast<u32>(i));
        }
    }
    return out;
}

namespace {

/// One lookup shared by both deferred bodies; they differ only in the key type.
template <class Slot>
const TextureInput* findSlot(const std::vector<std::pair<Slot, TextureInput>>& slots, Slot slot) {
    for (const auto& entry : slots) {
        if (entry.first == slot) {
            return &entry.second;
        }
    }
    return nullptr;
}

template <class Slot>
TextureInput& setSlot(std::vector<std::pair<Slot, TextureInput>>& slots, Slot slot,
                      const TextureInput& input) {
    for (auto& entry : slots) {
        if (entry.first == slot) {
            entry.second = input;
            return entry.second;
        }
    }
    slots.emplace_back(slot, input);
    return slots.back().second;
}

} // namespace

const TextureInput* LegacyDeferredBody::find(LegacySlot slot) const {
    return findSlot(slots, slot);
}

TextureInput& LegacyDeferredBody::set(LegacySlot slot, const TextureInput& input) {
    return setSlot(slots, slot, input);
}

const TextureInput* PbrDeferredBody::find(PbrSlot slot) const {
    return findSlot(slots, slot);
}

TextureInput& PbrDeferredBody::set(PbrSlot slot, const TextureInput& input) {
    return setSlot(slots, slot, input);
}

// ============================================================================
// CommonMaterial
// ============================================================================

void CommonMaterial::setKind(MaterialKind kind) {
    switch (kind) {
    case MaterialKind::Composite:
        body = CompositeBody{};
        return;
    case MaterialKind::Combiners:
        body = CombinersBody{};
        return;
    case MaterialKind::LegacyDeferred:
        body = LegacyDeferredBody{};
        return;
    case MaterialKind::PBRDeferred:
        body = PbrDeferredBody{};
        return;
    case MaterialKind::Count:
        break;
    }
}

u32 CommonMaterial::ordinalCount() const {
    if (const CompositeBody* composite = std::get_if<CompositeBody>(&body)) {
        return static_cast<u32>(composite->layers.size());
    }
    if (const CombinersBody* combiners = std::get_if<CombinersBody>(&body)) {
        return static_cast<u32>(combiners->stages.size());
    }
    if (const LegacyDeferredBody* legacy = std::get_if<LegacyDeferredBody>(&body)) {
        return static_cast<u32>(legacy->slots.size());
    }
    if (const PbrDeferredBody* pbrBody = std::get_if<PbrDeferredBody>(&body)) {
        return static_cast<u32>(pbrBody->slots.size());
    }
    return 0;
}

const TextureInput* CommonMaterial::inputAt(u32 ordinal) const {
    return const_cast<CommonMaterial*>(this)->inputAt(ordinal);
}

TextureInput* CommonMaterial::inputAt(u32 ordinal) {
    if (ordinal >= ordinalCount()) {
        return nullptr;
    }
    const std::size_t index = static_cast<std::size_t>(ordinal);
    if (CompositeBody* composite = std::get_if<CompositeBody>(&body)) {
        return &composite->layers[index].input;
    }
    if (CombinersBody* combiners = std::get_if<CombinersBody>(&body)) {
        return &combiners->stages[index].input;
    }
    if (LegacyDeferredBody* legacy = std::get_if<LegacyDeferredBody>(&body)) {
        return &legacy->slots[index].second;
    }
    if (PbrDeferredBody* pbrBody = std::get_if<PbrDeferredBody>(&body)) {
        return &pbrBody->slots[index].second;
    }
    return nullptr;
}

// ============================================================================
// Colour space
// ============================================================================

ColorSpace AutoColorSpaceFor(SurfaceChannel channel) {
    switch (channel) {
    case SurfaceChannel::Color:
    case SurfaceChannel::Emissive:
    case SurfaceChannel::Specular:
    case SurfaceChannel::Environment:
        return ColorSpace::Srgb;
    case SurfaceChannel::Normal:
    case SurfaceChannel::AmbientOcclusion:
    case SurfaceChannel::Count:
        break;
    }
    return ColorSpace::Linear;
}

ColorSpace AutoColorSpaceFor(LegacySlot slot) {
    switch (slot) {
    case LegacySlot::Diffuse:
    case LegacySlot::Specular:
    case LegacySlot::Emissive:
    case LegacySlot::Environment:
    case LegacySlot::Lightmap:
        return ColorSpace::Srgb;
    case LegacySlot::Normal:
    case LegacySlot::Gloss:
    case LegacySlot::AmbientOcclusion:
    case LegacySlot::Height:
    case LegacySlot::Count:
        break;
    }
    return ColorSpace::Linear;
}

ColorSpace AutoColorSpaceFor(PbrSlot slot) {
    switch (slot) {
    case PbrSlot::BaseColor:
    case PbrSlot::Emissive:
    case PbrSlot::Environment:
        return ColorSpace::Srgb;
    case PbrSlot::Normal:
    case PbrSlot::Orm:
    case PbrSlot::Metallic:
    case PbrSlot::Roughness:
    case PbrSlot::AmbientOcclusion:
    // A team-colour mask is a coverage value, not a colour: it selects how much
    // of the team tint lands, and tone-mapping it would bend the ramp.
    case PbrSlot::TeamColorMask:
    case PbrSlot::Count:
        break;
    }
    return ColorSpace::Linear;
}

ColorSpace AutoColorSpaceForCombinerStage() {
    return ColorSpace::Srgb;
}

ColorSpace ResolvedColorSpace(const TextureInput& input, SurfaceChannel channel) {
    return input.colorSpace == ColorSpace::Auto ? AutoColorSpaceFor(channel) : input.colorSpace;
}

ColorSpace ResolvedColorSpace(const TextureInput& input, LegacySlot slot) {
    return input.colorSpace == ColorSpace::Auto ? AutoColorSpaceFor(slot) : input.colorSpace;
}

ColorSpace ResolvedColorSpace(const TextureInput& input, PbrSlot slot) {
    return input.colorSpace == ColorSpace::Auto ? AutoColorSpaceFor(slot) : input.colorSpace;
}

// ============================================================================
// Flatten
// ============================================================================

namespace {

LegacySlot legacySlotFor(SurfaceChannel channel) {
    switch (channel) {
    case SurfaceChannel::Color:
        return LegacySlot::Diffuse;
    case SurfaceChannel::Emissive:
        return LegacySlot::Emissive;
    case SurfaceChannel::Specular:
        return LegacySlot::Specular;
    case SurfaceChannel::Normal:
        return LegacySlot::Normal;
    case SurfaceChannel::AmbientOcclusion:
        return LegacySlot::AmbientOcclusion;
    case SurfaceChannel::Environment:
    case SurfaceChannel::Count:
        break;
    }
    return LegacySlot::Environment;
}

} // namespace

std::optional<LegacyDeferredBody> Flatten(const CompositeBody& composite) {
    LegacyDeferredBody out;
    bool seen[static_cast<std::size_t>(SurfaceChannel::Count)] = {};

    for (const CompositeLayer& layer : composite.layers) {
        const std::size_t channel = static_cast<std::size_t>(layer.target);
        if (channel >= static_cast<std::size_t>(SurfaceChannel::Count)) {
            return std::nullopt;
        }
        // Degenerate means "already a slot map": one layer per channel, and that
        // layer replacing rather than folding. Anything else has an evaluation
        // order a slot map cannot hold.
        if (seen[channel] || layer.op != CompositeOp::Set) {
            return std::nullopt;
        }
        seen[channel] = true;
        out.set(legacySlotFor(layer.target), layer.input);
    }

    out.diffuseFactor = composite.diffuseFactor;
    out.emissiveFactor = composite.emissiveFactor;
    out.specularFactor = composite.specularFactor;
    out.specularExponent = composite.specularExponent;
    out.environmentFactor = composite.environmentFactor;
    return out;
}

} // namespace wem
} // namespace models
} // namespace whiteout
