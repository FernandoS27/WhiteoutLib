// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file crossing.h
 * @brief Physics restated for the profile an export targets
 *        (WEM_PHYSICS_DESIGN.md §8.3).
 *
 * What the target cannot say is said its way where it has one, else dropped and
 * reported:
 * - **Rigs become the target's switches.** StarCraft II switches bodies by
 *   animation: each body rests as the switch rule says and is keyed wherever
 *   a clip's state leaves that (`switches.h`, EDIT_MODE_PHYSICS_BAKE_DESIGN.md
 *   §8.6), so an `OnDeath` rig's bodies simulate from the start of every
 *   *Death* clip. A rig stays when it is the death rig or moves somewhere.
 *   World of Warcraft simulates what it builds from creation, so it keeps only
 *   the rigs that start that way, and reports the clips that switch bodies.
 * - **Cages over the target's particle limit are decimated**, shortest edge
 *   first and a free particle into a pinned one before anything else; what a
 *   collapsed particle drove follows the particle it collapsed into.
 *
 * Runs on the export's own copy: `BakeStages` calls it first.
 */

#include <string_view>

#include "../diagnostics.h"
#include "../document.h"

namespace whiteout {
namespace models {
namespace wem {

/// Whether @p document holds physics that @p target restates.
bool NeedsPhysicsFit(const Document& document, ProfileId target);

/// Restates @p document's physics for @p target. Returns how many rigs and
/// cages it changed.
u32 FitPhysicsToProfile(Document& document, ProfileId target, Diagnostics& out);

/// Whether @p document holds a cloth, or a collider for one.
bool CarriesCloth(const Document& document);

/**
 * @brief A file for @p target, which runs no cloth, written without it
 *        (EDIT_MODE_PHYSICS_CLOTH_DESIGN.md §10.6): each cage section goes with
 *        its faces, the cloths, their colliders, layers and `ClothActive`
 *        channels go, and the faces they drove keep their own skin.
 *
 * Only on a file's copy, never on what draws the viewport: the geosets of the
 * edit actor are numbered one per section. Returns how many cloths went.
 */
u32 DropClothForProfile(Document& document, ProfileId target, Diagnostics& out);

/// "Death", or "Death" and then a space: a clip StarCraft II plays as one.
bool IsDeathClipName(std::string_view name);

/// @p record's @p channel switch on @p model, declared when it has none.
u32 PhysicsSwitchChannel(Model& model, u32 record, Channel channel);

} // namespace wem
} // namespace models
} // namespace whiteout
