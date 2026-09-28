// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file physics_upgrade.h
 * @brief Old physics chunks become current on read, as StarCraft II upgrades them
 *
 * The SC2 5.0 client has no minimum chunk version: it converts every old
 * PHRB, PHSH, PHCL and FOR_ record into the current layout on load
 * (`M3_UpgradePHRB` / `PHSH` / `PHCL` / `FOR`), filling what the old layout
 * lacks with fixed defaults and discarding what the current one has no room
 * for — a Havok body's material among them. It then cooks the shapes the
 * converters left as raw points (`ModelAsset_CookPhysicsAndCloth`).
 *
 * `UpgradePhysics` is that pass, run by the parser after every read, so the
 * viewer, the WEM import and a Save As all hold what the game simulates, and
 * the writer only ever spells current versions. It deliberately breaks the
 * library's "an old chunk writes back as it was read" rule for physics alone:
 * the physics versions are the same in both clients, so an upgraded chunk
 * always loads.
 */

#include <string>
#include <vector>

#include "structures.h"

namespace whiteout {
namespace m3 {

/// The current versions, as both clients' descriptor tables name them.
inline constexpr i32 kCurrentRigidBodyVersion = 4;
inline constexpr i32 kCurrentPhysicsShapeVersion = 3;
inline constexpr i32 kCurrentClothVersion = 4;
inline constexpr i32 kCurrentForceVersion = 2;
inline constexpr i32 kCurrentWarpVersion = 1;

/// A switch the client's upgrade writes for a record that had none: never
/// sampled (`animId` -1, flags 0), so the sampler keeps @p init.
AnimRef<u32> UnsampledSwitch(u32 init);

/**
 * @brief Brings every physics record of @p model to its current version.
 *
 * Idempotent: a model already current is left as it is, except that any
 * shape still carrying source points is cooked. Returns one line per thing it
 * had to drop or could not cook.
 */
std::vector<std::string> UpgradePhysics(Model& model);

/// @p point through @p matrix as row vectors, in the client's float order.
Vector3f TransformPointRowMajor(const Matrix44f& matrix, const Vector3f& point);

} // namespace m3
} // namespace whiteout
