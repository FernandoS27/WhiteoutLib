// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/anim/channel.h>

namespace whiteout {
namespace models {
namespace wem {

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
