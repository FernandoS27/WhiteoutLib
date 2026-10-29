// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file engine_names.h
 * @brief The bone names a game's engine looks a bone up by: what an export, the
 *        key and bone reductions and the MDX bind all leave alone.
 */

#include <string>

namespace whiteout {
namespace models {
namespace wem {

/// Whether Warcraft III resolves @p name as one of its bones (`CBoneTokenizer`:
/// `bone_head`, `bone_chest`, `bone_foot`, `bone_hand`, `bone_turret`, as a
/// token of the name). Such a node is never reduced.
bool IsWarcraftEngineBoneName(const std::string& name);

/// Whether StarCraft II or Heroes finds @p name as one of its bones: the
/// engines' table spells `Bone_Chest`, `Bone_Face`, `Bone_Leg` and
/// `Bone_Turret`, each with or without a suffix word, as the name's token.
bool IsStarCraftEngineBoneName(const std::string& name);

/// Whether @p name is a bone the game aims (EDIT_MODE_BIND_ORIENTATION_DESIGN.md
/// §4.1): `bone_head` or `bone_chest`, which `SetUnitLookAt` names, or
/// `bone_turret`, which the attack turns. The game points its bind +X at the
/// target, the turret turning about bind +Z, so its `BPOS` turn changes where
/// it looks.
bool IsWarcraftLookAtBoneName(const std::string& name);

} // namespace wem
} // namespace models
} // namespace whiteout
