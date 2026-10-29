// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/physics/switch_rule.h>

#include <algorithm>

#include "switch_rule_common.h"

namespace whiteout {
namespace models {
namespace wem {

using switchdetail::Holds;

f32 PhysicsSwitchRest(const Model& model, const TrackTarget& target, Game game) {
    const PhysicsSet& physics = model.physics;
    switch (target.channel) {
    case Channel::PhysicsDynamic: {
        const PhysicsBody* body = physics.body(target.sub);
        if (body == nullptr || body->motion == BodyMotion::Static || !body->simulates) {
            return 0.0f;
        }
        const bool held = std::any_of(physics.rigs.begin(), physics.rigs.end(),
                                      [&](const PhysicsRig& rig) { return Holds(rig, body->id); });
        if (!held && game == Game::StarCraft) {
            return 1.0f;
        }
        const bool runs = std::any_of(physics.rigs.begin(), physics.rigs.end(), [&](const PhysicsRig& rig) {
            return RigRuns(rig.start) && Holds(rig, body->id);
        });
        return runs && body->motion == BodyMotion::Dynamic ? 1.0f : 0.0f;
    }
    case Channel::ClothActive: {
        const Cloth* cloth = physics.cloth(target.sub);
        return cloth != nullptr && cloth->active ? 1.0f : 0.0f;
    }
    case Channel::PhysicsBlend:
        return 1.0f;
    default:
        return 0.0f;
    }
}

u32 PhysicsSwitchChannel(Model& model, u32 record, Channel channel) {
    for (const AnimChannel& entry : model.animChannels.channels) {
        if (entry.target.kind == TrackTarget::Kind::Physics && entry.target.sub == record &&
            entry.target.channel == channel) {
            return entry.id;
        }
    }
    AnimChannel made;
    made.id = model.animChannels.nextFreeId();
    made.target.kind = TrackTarget::Kind::Physics;
    made.target.sub = record;
    made.target.channel = channel;
    made.valueType = geom::AttrType::F32;
    return model.animChannels.add(made);
}

} // namespace wem
} // namespace models
} // namespace whiteout
