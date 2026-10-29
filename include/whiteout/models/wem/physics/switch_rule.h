// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file switch_rule.h
 * @brief What a physics switch plays where no clip keys it, and the channel it
 *        is keyed on: the part of the switch rule (`switches.h`) that playing
 *        and converting a document need.
 */

#include <whiteout/common_types.h>

#include "../document.h"

namespace whiteout {
namespace models {
namespace wem {

/// Whether a rig of @p start runs in every clip, its bodies' own states
/// applying (§4.1).
constexpr bool RigRuns(RigStart start) {
    return start == RigStart::Always || start == RigStart::Animated;
}

/// What a physics switch plays where its clip does not key it (§9.1): a
/// body's `PhysicsDynamic` 1 when a rig holding it runs and it is Dynamic and
/// simulates (in @p game StarCraft, one no rig holds when it simulates and is
/// not Static), a cloth's `ClothActive` its `active`, a rig's `PhysicsRagdoll`
/// 0 and any `PhysicsBlend` 1. Two defaults depend on the clip and are the
/// rule's, not this: an *On death* drop, and a drop carried into the clip.
f32 PhysicsSwitchRest(const Model& model, const TrackTarget& target, Game game);

/// @p record's @p channel switch on @p model, declared when it has none.
u32 PhysicsSwitchChannel(Model& model, u32 record, Channel channel);

} // namespace wem
} // namespace models
} // namespace whiteout
