// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file materials.h
 * @brief StarCraft II's physics material presets (EDIT_MODE_PHYSICS_REDESIGN.md §2.1).
 *
 * A body's `Sc2BodyExtension::physicsMaterial` is an index into the 22 presets
 * of the Art Tools' `PhysicsMaterials.smd`: a census of every shipped PHRB v4
 * record found each id's most common values to be that entry's. The game also
 * picks the sound and effect of an impact by it, so it is gameplay data.
 *
 * Four presets ship with values other than the 2013 file's -- Sand, Snow,
 * Paper and Hair -- and the table holds the shipped ones: they are what the
 * game's own models are made of.
 *
 * Engine-free, so a crossing that guesses a material for another game's body
 * reads the same table the editor does.
 */

#include <optional>
#include <span>

#include <whiteout/common_types.h>

#include "physics.h"

namespace whiteout {
namespace models {
namespace wem {

struct PhysicsMaterialPreset {
    u32 id = 0;           ///< `physicsType`.
    const char* name = ""; ///< As the Art Tools list it.
    PhysicsMaterial material;
    f32 linearDamping = 0.0f;
    f32 angularDamping = 0.0f;
};

/// The 22 presets, in id order.
std::span<const PhysicsMaterialPreset> Sc2PhysicsMaterials();

/// The preset whose values @p body carries: every shape's density, friction
/// and restitution and the body's damping equal to it within 1e-4. None when
/// its shapes disagree, when it has none, or when no preset matches.
std::optional<u32> MatchSc2Preset(const PhysicsBody& body);

/// Flesh: what a ragdoll's bodies are made of (D2).
inline constexpr u32 kSc2Flesh = 4;

} // namespace wem
} // namespace models
} // namespace whiteout
