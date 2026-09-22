// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/anim/channel.h>

#include <iterator>
#include <span>

namespace whiteout {
namespace models {
namespace wem {

namespace {

using geom::AttrType;

// One row per enumerator of the kind's property enum, in its order -- the
// static_asserts below hold the two together.
constexpr bool kLength = true;
constexpr bool kPerElement = true;

constexpr EmitterPropertyDesc kWc3Particle1[] = {
    {"emissionRate", AttrType::F32},
    {"gravity", AttrType::F32, false, kLength},
    {"longitude", AttrType::F32},
    {"latitude", AttrType::F32},
    {"lifespan", AttrType::F32},
    {"speed", AttrType::F32, false, kLength},
};
static_assert(std::size(kWc3Particle1) == static_cast<std::size_t>(Wc3Particle1Property::Count));

constexpr EmitterPropertyDesc kWc3Particle2[] = {
    {"speed", AttrType::F32, false, kLength},
    {"variation", AttrType::F32},
    {"latitude", AttrType::F32},
    {"gravity", AttrType::F32, false, kLength},
    {"emissionRate", AttrType::F32},
    {"width", AttrType::F32, false, kLength},
    {"length", AttrType::F32, false, kLength},
};
static_assert(std::size(kWc3Particle2) == static_cast<std::size_t>(Wc3Particle2Property::Count));

constexpr EmitterPropertyDesc kWc3Ribbon[] = {
    {"heightAbove", AttrType::F32, false, kLength},
    {"heightBelow", AttrType::F32, false, kLength},
};
static_assert(std::size(kWc3Ribbon) == static_cast<std::size_t>(Wc3RibbonProperty::Count));

// Multipliers on the effect's own values, so none of them is a length: a
// rescale leaves a factor alone.
constexpr EmitterPropertyDesc kWc3Corn[] = {
    {"lifespan", AttrType::F32},
    {"emissionRate", AttrType::F32},
    {"speed", AttrType::F32},
};
static_assert(std::size(kWc3Corn) == static_cast<std::size_t>(Wc3CornProperty::Count));

constexpr EmitterPropertyDesc kM2Particle[] = {
    {"speed", AttrType::F32, false, kLength},
    {"speedVariation", AttrType::F32},
    {"verticalRange", AttrType::F32},
    {"horizontalRange", AttrType::F32},
    {"gravity", AttrType::F32x3, false, kLength},
    {"lifespan", AttrType::F32},
    {"emissionRate", AttrType::F32},
    {"width", AttrType::F32, false, kLength},
    {"length", AttrType::F32, false, kLength},
    {"zSource", AttrType::F32, false, kLength},
};
static_assert(std::size(kM2Particle) == static_cast<std::size_t>(M2ParticleProperty::Count));

constexpr EmitterPropertyDesc kSc2Particle[] = {
    {"initialSpeed", AttrType::F32, false, kLength},
    {"initialSpeedRandom", AttrType::F32, false, kLength},
    {"initialYaw", AttrType::F32},
    {"initialPitch", AttrType::F32},
    {"initialHorizontal", AttrType::F32},
    {"initialVertical", AttrType::F32},
    {"lifetime", AttrType::F32},
    {"lifetimeRandom", AttrType::F32},
    {"sizeAnimation", AttrType::F32x3, false, kLength},
    {"rotationAnimation", AttrType::F32x3},
    {"colorStart", AttrType::F32x4},
    {"colorMid", AttrType::F32x4},
    {"colorEnd", AttrType::F32x4},
    {"emissionRate", AttrType::F32},
    {"shapeOuter", AttrType::F32x3, false, kLength},
    {"shapeInner", AttrType::F32x3, false, kLength},
    {"outerRadius", AttrType::F32, false, kLength},
    {"innerRadius", AttrType::F32, false, kLength},
    {"sizeRandomAnimation", AttrType::F32x3, false, kLength},
    {"rotationRandomAnimation", AttrType::F32x3},
    {"colorStartRandom", AttrType::F32x4},
    {"colorMidRandom", AttrType::F32x4},
    {"colorEndRandom", AttrType::F32x4},
    // A burst count. Keyed in SDS6 on every shipped emitter that keys it.
    {"squirtAmount", AttrType::U32},
    {"pitch.amplitude", AttrType::F32},
    {"pitch.frequency", AttrType::F32},
    {"yaw.amplitude", AttrType::F32},
    {"yaw.frequency", AttrType::F32},
    {"speed.amplitude", AttrType::F32, false, kLength},
    {"speed.frequency", AttrType::F32},
    {"size.amplitude", AttrType::F32, false, kLength},
    {"size.frequency", AttrType::F32},
    {"alpha.amplitude", AttrType::F32},
    {"alpha.frequency", AttrType::F32},
    {"color.amplitude", AttrType::F32},
    {"color.frequency", AttrType::F32},
    {"rotation.amplitude", AttrType::F32},
    {"rotation.frequency", AttrType::F32},
    {"horizontal.amplitude", AttrType::F32},
    {"horizontal.frequency", AttrType::F32},
    {"vertical.amplitude", AttrType::F32},
    {"vertical.frequency", AttrType::F32},
    {"particleVelocity", AttrType::F32},
    {"phaseShift", AttrType::F32},
    {"alphaThreshold", AttrType::F32},
    {"uvOffset", AttrType::F32x2},
    {"uvAngle", AttrType::F32x3},
    {"uvTiling", AttrType::F32x2},
    {"splinePoints", AttrType::F32x3, kPerElement, kLength},
    {"lowerBound", AttrType::F32},
    {"upperBound", AttrType::F32},
    {"trailEmissionRate", AttrType::F32},
};
static_assert(std::size(kSc2Particle) == static_cast<std::size_t>(Sc2ParticleProperty::Count));

constexpr EmitterPropertyDesc kSc2Ribbon[] = {
    {"initialSpeed", AttrType::F32, false, kLength},
    {"initialSpeedRandom", AttrType::F32, false, kLength},
    {"initialYaw", AttrType::F32},
    {"initialPitch", AttrType::F32},
    {"initialHorizontal", AttrType::F32},
    {"initialVertical", AttrType::F32},
    {"lifetime", AttrType::F32},
    {"lifetimeRandom", AttrType::F32},
    {"sizeAnimation", AttrType::F32x3, false, kLength},
    {"rotationAnimation", AttrType::F32x3},
    {"colorStart", AttrType::F32x4},
    {"colorMid", AttrType::F32x4},
    {"colorEnd", AttrType::F32x4},
    {"maxLength", AttrType::F32, false, kLength},
    // A flag, keyed in SDFG as every shipped one is (248 of 248 measured), so
    // it is the 0-or-1 float a visibility is.
    {"active", AttrType::F32},
    {"yaw.amplitude", AttrType::F32},
    {"yaw.frequency", AttrType::F32},
    {"pitch.amplitude", AttrType::F32},
    {"pitch.frequency", AttrType::F32},
    {"speed.amplitude", AttrType::F32, false, kLength},
    {"speed.frequency", AttrType::F32},
    {"size.amplitude", AttrType::F32, false, kLength},
    {"size.frequency", AttrType::F32},
    {"alpha.amplitude", AttrType::F32},
    {"alpha.frequency", AttrType::F32},
    {"particleVelocity", AttrType::F32},
    {"overlay", AttrType::F32},
    {"splinePoints.velocity", AttrType::F32, kPerElement, kLength},
    {"splinePoints.velocityBaseFactor", AttrType::F32, kPerElement},
    {"splinePoints.velocityEndFactor", AttrType::F32, kPerElement},
    {"splinePoints.yawVariation.amplitude", AttrType::F32, kPerElement},
    {"splinePoints.yawVariation.frequency", AttrType::F32, kPerElement},
    {"splinePoints.pitchVariation.amplitude", AttrType::F32, kPerElement},
    {"splinePoints.pitchVariation.frequency", AttrType::F32, kPerElement},
    {"splinePoints.velocityVariation.amplitude", AttrType::F32, kPerElement, kLength},
    {"splinePoints.velocityVariation.frequency", AttrType::F32, kPerElement},
    {"splinePoints.yaw", AttrType::F32, kPerElement},
    {"splinePoints.pitch", AttrType::F32, kPerElement},
};
static_assert(std::size(kSc2Ribbon) == static_cast<std::size_t>(Sc2RibbonProperty::Count));

std::span<const EmitterPropertyDesc> PropertiesOf(NodeKind kind) {
    switch (kind) {
    case NodeKind::Wc3ParticleEmitter1:
        return kWc3Particle1;
    case NodeKind::Wc3ParticleEmitter2:
        return kWc3Particle2;
    case NodeKind::Wc3RibbonEmitter:
        return kWc3Ribbon;
    case NodeKind::Wc3CornEmitter:
        return kWc3Corn;
    case NodeKind::Sc2ParticleEmitter:
        return kSc2Particle;
    case NodeKind::Sc2RibbonEmitter:
        return kSc2Ribbon;
    case NodeKind::M2ParticleEmitter:
        return kM2Particle;
    default:
        return {};
    }
}

} // namespace

const EmitterPropertyDesc* FindEmitterProperty(NodeKind kind, u32 sub) {
    const std::span<const EmitterPropertyDesc> table = PropertiesOf(kind);
    const u32 property = EmitterPropertyOf(sub);
    if (property >= table.size()) {
        return nullptr;
    }
    const EmitterPropertyDesc& desc = table[property];
    // Only a per-element property has an element to name.
    if (!desc.perElement && EmitterElementOf(sub) != 0) {
        return nullptr;
    }
    return &desc;
}

u32 EmitterPropertyCount(NodeKind kind) {
    return static_cast<u32>(PropertiesOf(kind).size());
}

const char* ToString(Channel channel) {
    switch (channel) {
    case Channel::Translation:
        return "translation";
    case Channel::Rotation:
        return "rotation";
    case Channel::Scale:
        return "scale";
    case Channel::Visibility:
        return "visibility";
    case Channel::Color:
        return "color";
    case Channel::Alpha:
        return "alpha";
    case Channel::Intensity:
        return "intensity";
    case Channel::AttenuationStart:
        return "attenuationStart";
    case Channel::AttenuationEnd:
        return "attenuationEnd";
    case Channel::UvTranslate:
        return "uvTranslate";
    case Channel::UvRotate:
        return "uvRotate";
    case Channel::UvScale:
        return "uvScale";
    case Channel::Weight:
        return "weight";
    case Channel::TextureIndex:
        return "textureIndex";
    case Channel::Emissive:
        return "emissive";
    case Channel::EmitterProperty:
        return "emitterProperty";
    case Channel::ShadowCastingStart:
        return "shadowCastingStart";
    case Channel::ShadowCastingEnd:
        return "shadowCastingEnd";
    case Channel::QuadraticFalloff:
        return "quadraticFalloff";
    case Channel::LinearFalloff:
        return "linearFalloff";
    case Channel::Damping:
        return "damping";
    case Channel::Count:
        break;
    }
    return "invalid";
}

const char* ToString(TrackTarget::Kind kind) {
    switch (kind) {
    case TrackTarget::Kind::Node:
        return "node";
    case TrackTarget::Kind::MaterialLayer:
        return "materialLayer";
    case TrackTarget::Kind::MaterialFeature:
        return "materialFeature";
    case TrackTarget::Kind::Section:
        return "section";
    case TrackTarget::Kind::Count:
        break;
    }
    return "invalid";
}

geom::AttrType DefaultValueType(Channel channel) {
    switch (channel) {
    case Channel::Translation:
    case Channel::Scale:
    case Channel::Color:
    case Channel::UvTranslate:
    case Channel::UvScale:
        return geom::AttrType::F32x3;
    case Channel::Rotation:
    case Channel::UvRotate:
        return geom::AttrType::Quat;
    case Channel::TextureIndex:
        return geom::AttrType::U32;
    case Channel::Visibility:
    case Channel::Alpha:
    case Channel::Intensity:
    case Channel::AttenuationStart:
    case Channel::AttenuationEnd:
    case Channel::Weight:
    case Channel::Emissive:
    case Channel::EmitterProperty: // the property's own type -- FindEmitterProperty
    case Channel::ShadowCastingStart:
    case Channel::ShadowCastingEnd:
    case Channel::QuadraticFalloff:
    case Channel::LinearFalloff:
    case Channel::Damping:
    case Channel::Count:
        break;
    }
    return geom::AttrType::F32;
}

const AnimChannel* AnimChannelTable::find(u32 id) const {
    for (const AnimChannel& channel : channels) {
        if (channel.id == id) {
            return &channel;
        }
    }
    return nullptr;
}

AnimChannel* AnimChannelTable::find(u32 id) {
    const AnimChannelTable& self = *this;
    return const_cast<AnimChannel*>(self.find(id));
}

u32 AnimChannelTable::indexOf(u32 id) const {
    for (std::size_t i = 0; i < channels.size(); ++i) {
        if (channels[i].id == id) {
            return static_cast<u32>(i);
        }
    }
    return kInvalidIndex;
}

u32 AnimChannelTable::add(const AnimChannel& channel) {
    channels.push_back(channel);
    return channel.id;
}

u32 AnimChannelTable::nextFreeId() const {
    // Ids are never reused, so the successor of the largest is free even after
    // channels in the middle have been dropped.
    u32 highest = 0;
    for (const AnimChannel& channel : channels) {
        if (channel.id >= highest) {
            highest = channel.id + 1;
        }
    }
    return highest;
}

} // namespace wem
} // namespace models
} // namespace whiteout
