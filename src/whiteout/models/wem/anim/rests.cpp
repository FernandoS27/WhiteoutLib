// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "whiteout/models/wem/anim/rests.h"

#include <bit>
#include <cstring>
#include <optional>
#include <variant>

#include "whiteout/models/wem/anim/track_read.h"
#include "whiteout/models/wem/converters.h"
#include "whiteout/models/wem/native/mdx_native.h"

namespace whiteout {
namespace models {
namespace wem {

namespace {

template <class T>
std::vector<u8> Bytes(const T& value) {
    std::vector<u8> out(sizeof(T));
    std::memcpy(out.data(), &value, sizeof(T));
    return out;
}

/// An f32 kept as its bit pattern in a native bag, as the MDX import keeps a
/// light's ambient colour.
std::optional<f32> NativeBits(const NativeBag& bag, const char* name) {
    const NativeBag::Entry* entry = bag.find(name);
    if (!entry)
        return std::nullopt;
    return std::bit_cast<f32>(static_cast<u32>(entry->value));
}

Vector3f NativeColour(const NativeBag& bag, const char* r, const char* g, const char* b,
                      const Vector3f& otherwise) {
    const std::optional<f32> x = NativeBits(bag, r);
    const std::optional<f32> y = NativeBits(bag, g);
    const std::optional<f32> z = NativeBits(bag, b);
    return x && y && z ? Vector3f{*x, *y, *z} : otherwise;
}

/// A section's static tint, read through the converter that writes it: the
/// geoset's bag layout is the MDX converter's to know (L1).
GeosetTint SectionTint(const MeshSection& section) {
    static const MdxConverter converter;
    return converter.geosetTint(section);
}

/// The static field of an emitter's property, read where `toMdx` writes it
/// from (`mdx_converter.cpp`, the PREM, PRE2 and RIBB branches).
std::optional<f32> EmitterStatic(const Node& node, u32 sub) {
    const u32 property = EmitterPropertyOf(sub);
    if (const auto* pe = std::get_if<Wc3ParticleEmitter1Payload>(&node.payload)) {
        using P = Wc3Particle1Property;
        switch (static_cast<P>(property)) {
        case P::EmissionRate: return pe->emissionRate;
        case P::Gravity: return pe->gravity;
        case P::Longitude: return pe->longitude;
        case P::Latitude: return pe->latitude;
        case P::Lifespan: return pe->lifespan;
        case P::Speed: return pe->speed;
        default: return std::nullopt;
        }
    }
    if (const auto* pe = std::get_if<Wc3ParticleEmitter2Payload>(&node.payload)) {
        using P = Wc3Particle2Property;
        switch (static_cast<P>(property)) {
        case P::Speed: return pe->speed;
        case P::Variation: return pe->variation;
        case P::Latitude: return pe->latitude;
        case P::Gravity: return pe->gravity;
        case P::EmissionRate: return pe->emissionRate;
        case P::Width: return pe->width;
        case P::Length: return pe->length;
        default: return std::nullopt;
        }
    }
    if (const auto* ribbon = std::get_if<Wc3RibbonEmitterPayload>(&node.payload)) {
        using P = Wc3RibbonProperty;
        switch (static_cast<P>(property)) {
        case P::HeightAbove: return ribbon->heightAbove;
        case P::HeightBelow: return ribbon->heightBelow;
        default: return std::nullopt;
        }
    }
    if (const auto* corn = std::get_if<Wc3CornEmitterPayload>(&node.payload)) {
        using P = Wc3CornProperty;
        switch (static_cast<P>(property)) {
        case P::Lifespan: return corn->lifespan;
        case P::EmissionRate: return corn->emissionRate;
        case P::Speed: return corn->speed;
        default: return std::nullopt;
        }
    }
    return std::nullopt;
}

/// The layer @p ordinal of @p profile's material at @p slot, in its native
/// block: the block holds exactly the layers the profile keeps, in order, so an
/// ordinal is its index (`ImportMaterial`).
const native::MdxLayer* NativeLayerOf(const Model& model, const MaterialChannelRef& ref,
                                           u32 ordinal) {
    const Material* material = Resolve(model, ref.slot, ref.profile, ref.look);
    if (!material)
        return nullptr;
    const auto* block = std::get_if<native::MdxMaterial>(&material->Native());
    return block && ordinal < block->layers.size() ? &block->layers[ordinal] : nullptr;
}

/// The ordinal a `MaterialFeature` target's feature sits on, or none.
std::optional<u32> FeatureLayer(const Model& model, const TrackTarget& target) {
    const Material* material =
        Resolve(model, target.material.slot, target.material.profile, target.material.look);
    if (!material)
        return std::nullopt;
    for (const MaterialFeature& feature : material->Common().features)
        if (feature.id == target.sub)
            return feature.layer;
    return std::nullopt;
}

/// @p value as @p type's bytes: a float, a colour or vector, a quaternion or an
/// integer, whichever the type is.
std::vector<u8> AsType(geom::AttrType type, f32 scalar, const Vector3f& vector = {0, 0, 0}) {
    switch (type) {
    case geom::AttrType::F32:
        return Bytes(scalar);
    case geom::AttrType::F32x2:
        return Bytes(Vector2f{vector.x, vector.y});
    case geom::AttrType::F32x3:
        return Bytes(vector);
    case geom::AttrType::Quat:
        return Bytes(Quaternion{0, 0, 0, 1});
    case geom::AttrType::U32:
        return Bytes(static_cast<u32>(std::max(scalar, 0.0f)));
    default:
        return std::vector<u8>(geom::AttrTypeSize(type), 0);
    }
}

/// What a source's channel holds where nothing keys it: its own weight.
f32 StageStatic(const Model& model, const TrackTarget& target) {
    for (const PoseStage& stage : model.poseStages) {
        if (stage.id != StageOfSub(target.sub))
            continue;
        for (const StageSource& source : stage.sources)
            if (source.id == SourceOfSub(target.sub))
                return source.weight;
    }
    return 1.0f;
}

TrackRests Same(std::vector<u8> value) {
    TrackRests rests;
    rests.unkeyed = value;
    rests.keyedElsewhere = std::move(value);
    return rests;
}

TrackRests Pair(std::vector<u8> unkeyed, std::vector<u8> keyedElsewhere) {
    TrackRests rests;
    rests.unkeyed = std::move(unkeyed);
    rests.keyedElsewhere = std::move(keyedElsewhere);
    return rests;
}

TrackRests WarcraftRests(const Document& document, u32 model, const TrackTarget& target,
                         geom::AttrType type) {
    const std::size_t size = geom::AttrTypeSize(type);
    const TrackRests none = Same(std::vector<u8>(size, 0));
    if (model >= document.models.size())
        return none;
    const Model& source = document.models[model];
    const Channel channel = target.channel;

    // A transform, and a texture animation: the identity offset either way.
    switch (channel) {
    case Channel::Translation:
    case Channel::UvTranslate:
        return Same(AsType(type, 0.0f));
    case Channel::Rotation:
    case Channel::UvRotate:
        return Same(AsType(type, 0.0f));
    case Channel::Scale:
    case Channel::UvScale:
        return Same(AsType(type, 1.0f, Vector3f{1, 1, 1}));
    case Channel::Visibility:
    case Channel::StageWeight:
        return Same(AsType(type, 1.0f));
    case Channel::StageSourceWeight:
    case Channel::StageSourceEnabled:
        return Same(AsType(type, StageStatic(source, target)));
    default:
        break;
    }

    switch (target.kind) {
    case TrackTarget::Kind::Node: {
        if (target.node >= source.nodes.size())
            return none;
        const Node& node = source.nodes.nodes[target.node];
        if (const auto* light = std::get_if<LightPayload>(&node.payload)) {
            const bool ambient = target.sub == 1;
            switch (channel) {
            case Channel::Color:
                return Pair(AsType(type, 0.0f,
                                   ambient ? NativeColour(node.native, "ambientColorR", "ambientColorG",
                                                          "ambientColorB", Vector3f{0, 0, 0})
                                           : light->color),
                            AsType(type, 0.0f));
            case Channel::Intensity:
                return Pair(AsType(type, ambient ? Milli(node.native, "ambientIntensity", 0.0f)
                                                 : light->intensity),
                            AsType(type, 0.0f));
            // Neither the game nor the renderer animates a light's attenuation:
            // it plays the static field whatever keys it.
            case Channel::AttenuationStart:
                return Same(AsType(type, light->attenuationStart));
            case Channel::AttenuationEnd:
                return Same(AsType(type, light->attenuationEnd));
            // Nor its 3.0 shadow range and falloff: the renderer hands the
            // static fields to the lighting frame as they are.
            case Channel::ShadowCastingStart:
                return Same(AsType(type, light->shadowCastingStart));
            case Channel::ShadowCastingEnd:
                return Same(AsType(type, light->shadowCastingEnd));
            case Channel::QuadraticFalloff:
                return Same(AsType(type, light->quadraticFalloff));
            case Channel::LinearFalloff:
                return Same(AsType(type, light->linearFalloff));
            case Channel::Damping:
                return Same(AsType(type, light->damping));
            default:
                return none;
            }
        }
        // A camera's depth of field has no value where its track has no key
        // (`AnimateCamera` writes a focus of 0 and leaves the blur unset); the
        // rests reach the file as keys (`KeyCameraRests`).
        if (const auto* camera = std::get_if<CameraPayload>(&node.payload)) {
            switch (channel) {
            case Channel::FocusDistance:
                return Pair(AsType(type, camera->focusDistance), AsType(type, 0.0f));
            case Channel::FocalLength:
                return Pair(AsType(type, camera->focalLength), AsType(type, 0.0f));
            case Channel::FStop:
                return Pair(AsType(type, camera->fStop), AsType(type, 0.0f));
            default:
                return none;
            }
        }
        // A PopcornFX emitter (`SetPopcornValues`): its multipliers play the
        // static unkeyed and 1 keyed elsewhere, its colour the static or
        // black, and its alpha 1 either way — the static alpha is never read.
        if (const auto* corn = std::get_if<Wc3CornEmitterPayload>(&node.payload)) {
            switch (channel) {
            case Channel::Color:
                return Pair(AsType(type, 0.0f, corn->color), AsType(type, 0.0f));
            case Channel::Alpha:
                return Same(AsType(type, 1.0f));
            case Channel::EmitterProperty:
                if (const std::optional<f32> value = EmitterStatic(node, target.sub))
                    return Pair(AsType(type, *value), AsType(type, 1.0f));
                return none;
            default:
                return none;
            }
        }
        if (const auto* ribbon = std::get_if<Wc3RibbonEmitterPayload>(&node.payload)) {
            switch (channel) {
            case Channel::Color:
                return Pair(AsType(type, 0.0f, ribbon->color), AsType(type, 0.0f));
            case Channel::Alpha:
                return Pair(AsType(type, ribbon->alpha), AsType(type, 0.0f));
            case Channel::TextureIndex:
                return Pair(Bytes(ribbon->textureSlot), Bytes(u32{0}));
            default:
                break;
            }
        }
        if (channel == Channel::EmitterProperty)
            if (const std::optional<f32> value = EmitterStatic(node, target.sub))
                return Pair(AsType(type, *value), AsType(type, 0.0f));
        return none;
    }
    case TrackTarget::Kind::MaterialLayer: {
        const native::MdxLayer* layer = NativeLayerOf(source, target.material, target.sub);
        if (!layer)
            return none;
        switch (channel) {
        case Channel::Alpha:
            return Pair(AsType(type, layer->alpha), AsType(type, 1.0f));
        case Channel::Emissive:
            return Pair(AsType(type, layer->emissiveGain), AsType(type, 1.0f));
        case Channel::TextureIndex: {
            // The game resets a flipbook to the layer's own texture every frame
            // it writes none, so both rests are the texture.
            const u32 texture = layer->subTextures.empty() ? layer->textureId : layer->subTextures.front().textureId;
            return Same(Bytes(texture));
        }
        default:
            return none;
        }
    }
    case TrackTarget::Kind::MaterialFeature: {
        const std::optional<u32> ordinal = FeatureLayer(source, target);
        const native::MdxLayer* layer = ordinal ? NativeLayerOf(source, target.material, *ordinal) : nullptr;
        if (!layer)
            return none;
        switch (channel) {
        case Channel::Color:
            return Pair(AsType(type, 0.0f, layer->fresnelColor), AsType(type, 1.0f, Vector3f{1, 1, 1}));
        case Channel::Alpha:
            return Pair(AsType(type, layer->fresnelOpacity), AsType(type, 0.0f));
        case Channel::Weight:
            return Pair(AsType(type, layer->fresnelTeamColor), AsType(type, 0.0f));
        default:
            return none;
        }
    }
    case TrackTarget::Kind::Section: {
        // A geoset channel names a mesh and is written onto every geoset it
        // became (C7); the first section's record is the one read.
        if (target.mesh >= source.meshes.size() || source.meshes[target.mesh].sections.empty())
            return none;
        const MeshSection& section = source.meshes[target.mesh].sections.front();
        switch (channel) {
        case Channel::Alpha:
            return Pair(AsType(type, SectionStaticAlpha(section)), AsType(type, 1.0f));
        case Channel::Color:
            return Pair(AsType(type, 0.0f, SectionTint(section).color), AsType(type, 0.0f));
        default:
            return none;
        }
    }
    default:
        return none;
    }
}


TrackRests StarCraftRests(const Document& document, u32 model, const TrackTarget& target,
                          geom::AttrType type, const std::vector<u8>* initValue) {
    const Model& source = document.models[model];
    if (target.kind == TrackTarget::Kind::Node && target.node < source.nodes.size() &&
        source.nodes.rig == RigConvention::ExplicitBind) {
        const Transform& local = source.nodes.nodes[target.node].local;
        switch (target.channel) {
        case Channel::Translation:
            return Same(AsType(type, 0.0f, local.translation));
        case Channel::Rotation:
            return type == geom::AttrType::Quat ? Same(Bytes(local.rotation)) : Same(AsType(type, 0.0f));
        case Channel::Scale:
            return Same(AsType(type, local.scale.x, local.scale));
        default:
            break;
        }
    }
    if (target.kind == TrackTarget::Kind::Node && target.channel == Channel::Visibility) {
        return Same(AsType(type, VisibilityRest(source, target.node)));
    }
    // One rest: an STC that does not key a channel falls to the AnimRef's
    // `initValue`, which `toM3` writes from the channel's own where it has one
    // and from the static field where it does not.
    if (initValue != nullptr)
        return Same(*initValue);
    return Same(WarcraftRests(document, model, target, type).unkeyed);
}

} // namespace

f32 SectionStaticAlpha(const MeshSection& section) {
    const GeosetTint tint = SectionTint(section);
    return tint.hidden ? 0.0f : tint.alpha;
}


TrackRests RestsOf(const Document& document, u32 model, const TrackTarget& target,
                   geom::AttrType type, Game game) {
    if (model >= document.models.size()) {
        return Same(std::vector<u8>(geom::AttrTypeSize(type), 0));
    }
    return game == Game::StarCraft ? StarCraftRests(document, model, target, type, nullptr)
                                   : WarcraftRests(document, model, target, type);
}

TrackRests RestsOf(const Document& document, u32 model, const AnimChannel& channel, Game game) {
    if (model >= document.models.size()) {
        return Same(std::vector<u8>(geom::AttrTypeSize(channel.valueType), 0));
    }
    return game == Game::StarCraft
               ? StarCraftRests(document, model, channel.target, channel.valueType,
                                channel.hasInitValue() ? &channel.initValue : nullptr)
               : WarcraftRests(document, model, channel.target, channel.valueType);
}

TrackRests RestsOf(const Document& document, u32 model, const TrackTarget& target,
                   geom::AttrType type) {
    return RestsOf(document, model, target, type, GameOf(document.defaultProfile));
}

TrackRests RestsPlayed(const Document& document, u32 model, const AnimChannel& channel, Game storage) {
    const TrackTarget& target = channel.target;
    const bool transform = target.kind == TrackTarget::Kind::Node &&
                           (target.channel == Channel::Translation ||
                            target.channel == Channel::Rotation || target.channel == Channel::Scale);
    if (model < document.models.size() && transform) {
        const NodeTree& tree = document.models[model].nodes;
        if (tree.rig == RigConvention::ExplicitBind && target.node < tree.size()) {
            // A bone keyed absolutely rests at its own local transform; a pivot
            // rig's offsets rest at the identity, which is what either game's
            // rest says.
            const Transform& local = tree.nodes[target.node].local;
            std::vector<u8> rest;
            switch (target.channel) {
            case Channel::Translation:
                rest = Bytes(local.translation);
                break;
            case Channel::Rotation:
                rest = Bytes(local.rotation);
                break;
            default:
                rest = channel.valueType == geom::AttrType::F32 ? Bytes(local.scale.x)
                                                               : Bytes(local.scale);
                break;
            }
            if (rest.size() == geom::AttrTypeSize(channel.valueType)) {
                return Same(std::move(rest));
            }
        }
    }
    return RestsOf(document, model, channel, storage);
}

const Clip* ClockOwner(const Document& document, u32 model, u32 channelId) {
    for (const Clip& clip : document.clips) {
        if (clip.model != model)
            continue;
        const SubTrack* track = FindSubTrack(clip, channelId);
        if (track && !track->times.empty())
            return &clip;
    }
    return nullptr;
}

bool KeyedAnywhere(const Document& document, u32 model, u32 channelId) {
    return ClockOwner(document, model, channelId) != nullptr;
}

std::vector<u8> RestValue(const Document& document, u32 model, const AnimChannel& channel) {
    TrackRests rests = RestsOf(document, model, channel, GameOf(document.defaultProfile));
    return KeyedAnywhere(document, model, channel.id) ? std::move(rests.keyedElsewhere)
                                                      : std::move(rests.unkeyed);
}

f32 VisibilityRest(const Model& model, u32 node) {
    for (const AnimChannel& channel : model.animChannels.channels) {
        if (channel.target.kind != TrackTarget::Kind::Node || channel.target.node != node ||
            channel.target.channel != Channel::Visibility || !channel.hasInitValue()) {
            continue;
        }
        f32 rest = 1.0f;
        std::memcpy(&rest, channel.initValue.data(), sizeof(f32));
        return rest;
    }
    return 1.0f;
}

bool HeldByRenderer(const Document& document, u32 model, const AnimChannel& channel) {
    const TrackTarget& target = channel.target;
    if (model >= document.models.size() || target.kind != TrackTarget::Kind::Node ||
        target.channel != Channel::EmitterProperty ||
        EmitterPropertyOf(target.sub) != static_cast<u32>(Wc3Particle2Property::EmissionRate) ||
        target.node >= document.models[model].nodes.size())
        return false;
    const auto* emitter =
        std::get_if<Wc3ParticleEmitter2Payload>(&document.models[model].nodes.nodes[target.node].payload);
    return emitter && emitter->squirt;
}

} // namespace wem
} // namespace models
} // namespace whiteout
