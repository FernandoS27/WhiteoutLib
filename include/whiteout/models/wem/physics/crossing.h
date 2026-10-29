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
#include "switch_rule.h"

namespace whiteout {
namespace models {
namespace wem {

/// Whether @p document holds physics that @p target restates.
bool NeedsPhysicsFit(const Document& document, ProfileId target);

/// Restates @p document's physics for @p target. Returns how many rigs and
/// cages it changed.
u32 FitPhysicsToProfile(Document& document, ProfileId target, Diagnostics& out);

/**
 * @brief Each dynamic body whose gravity scale is not 1 pushed the rest of the
 *        way by a force field of the model's own, for @p target, which fixes
 *        the scale at 1 (StarCraft II).
 *
 * StarCraft II gives a body a field's acceleration times its mass, at the
 * point of its first shape nearest the field (EDIT_MODE_PHYSICS_REDESIGN.md
 * §25.1). So each field is directional and unbounded, pushes up by
 * `(1 - scale) * gravity` on a Local channel of its own, and stands at its
 * body's centre of mass at rest, a node of its own at the root that the
 * animation does not turn. While the free Local channels last each body has
 * one; past them, the bodies that fall alike and stand nearest share one at
 * their centre. @p gravity is the target's, in the document's units a second
 * squared. Returns how many fields it made.
 */
u32 CarryGravityScale(Document& document, ProfileId target, f32 gravity, Diagnostics& out);

/// The half-width a sprung twist is held to for StarCraft II: under the 2°
/// below which Domino locks a twist (`halfRange < 0.0349`).
inline constexpr f32 kHeldTwist = 0.01745f;

/**
 * @brief Each cone-twist joint whose position motor springs its twist back to
 *        rest (World of Warcraft's shoulder) has that twist held instead, for
 *        @p target, which has limits and no spring (StarCraft II): its limits
 *        narrowed to ±`kHeldTwist` round rest, its motor and spring dropped.
 *
 * Measured against the rigs themselves (`[.wow-sc2-fidelity]`), the swing free
 * and the twist held move nearer them than the twist let go. A hinge's motor
 * is left to be dropped: holding its one axis would weld it. Returns how many
 * joints it changed.
 */
u32 HoldSprungTwists(Document& document, ProfileId target, Diagnostics& out);

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

/// Whether @p document holds a soft body.
bool CarriesSoft(const Document& document);

/**
 * @brief A file for @p target written without its soft bodies
 *        (EDIT_MODE_SOFTBODY_DESIGN.md §12): the records go, with their layers
 *        and their four channels. No game runs one, so this is every target's.
 *
 * What a soft body moves is kept by `ExpandSoftToBones`, which runs first and
 * turns the lumps into bones; this only takes away what no file holds. Only on
 * a file's copy. Returns how many soft bodies went.
 */
u32 DropSoftForProfile(Document& document, ProfileId target, Diagnostics& out);

/// "Death", or "Death" and then a space: a clip StarCraft II plays as one.
bool IsDeathClipName(std::string_view name);

} // namespace wem
} // namespace models
} // namespace whiteout
