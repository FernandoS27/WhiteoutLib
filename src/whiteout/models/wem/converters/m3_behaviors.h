// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file m3_behaviors.h
 * @brief `IKJT`, `PATU` and `TRGD` <-> pose stages (EDIT_MODE_SC2_PROFILE_PLAN.md
 *        S1.4, P1).
 *
 * StarCraft II's leg adaptation and its turrets are behaviours its runtime runs:
 * a leg reaches for the ground, a turret turns to its target. They are kept as
 * the stages `Sc2LegIk` and `Sc2Turret`, each holding the record it came from,
 * so the editor lists them and an export for StarCraft II or Heroes writes the
 * records back -- `BakeStages` keeps them for a target that runs them.
 *
 * A `TRGD` is a named group of turrets the game aims together ("Chest",
 * "Head", "EyeLeft"); each turret stage keeps the group's name, where the group
 * stood among the file's, and where it stood in the group's list, so the file
 * comes back in its own order. `IK2J`, `IKCC` and `PAOB` ship in no model and
 * are reported if met.
 */

#include <vector>

#include <whiteout/models/m3/structures.h>
#include <whiteout/models/wem/diagnostics.h>
#include <whiteout/models/wem/model.h>

namespace whiteout {
namespace models {
namespace wem {
namespace m3_behaviors {

/// Adds a stage to @p model per `IKJT` and per `PATU` of @p source, IK first,
/// each in file order. Bones are the model's first nodes, in order.
void Import(const m3::Model& source, Model& model, Diagnostics& diagnostics);

/// Writes @p model's enabled `Sc2LegIk` and `Sc2Turret` stages into @p out, with
/// one `TRGD` per group. @p boneOf maps a node to the bone written for it,
/// `0xFFFF` for none.
void Export(const Model& model, const std::vector<u32>& boneOf, m3::Model& out, Diagnostics& diagnostics);

} // namespace m3_behaviors
} // namespace wem
} // namespace models
} // namespace whiteout
