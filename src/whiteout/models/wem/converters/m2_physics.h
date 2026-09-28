// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file m2_physics.h
 * @brief A `.phys` <-> WEM (WEM_PHYSICS_DESIGN.md §8.1).
 *
 * **Read as the client reads.** The parser already gives legacy records the
 * values `PhysUpgradeLegacyChunks` gives them; the import applies the one rule
 * that depends on the version (capsule density at version 4 and below). The
 * export writes version 6 only.
 *
 * **Frames need no change.** Bones are nodes 0..n-1 of a pivot-relative rig
 * with no bind rotation, so a `.phys` frame -- three axis rows and an origin --
 * is a node frame as it stands.
 *
 * **Angles** are degrees on disk. The import stores the client's own multiply;
 * the export writes a degree value that multiplies back to the same float.
 */

#include <functional>
#include <optional>

#include <whiteout/models/m2/structures.h>
#include <whiteout/models/wem/diagnostics.h>
#include <whiteout/models/wem/model.h>

namespace whiteout {
namespace models {
namespace wem {
namespace m2_physics {

/// The client's degrees-to-radians: one multiply.
f32 ToRadians(f32 degrees);
/// A degree value whose `ToRadians` is exactly @p radians when one exists
/// nearby, else the nearest.
f32 ToDegrees(f32 radians);

/// Fills `model.physics` from `source.physics`. Bones are nodes 0..n-1.
void Import(const m2::Model& source, Model& model, Diagnostics& out);

/// `model.physics` as a version-6 `.phys`, or nothing when it is empty.
/// @p boneOf maps a node to its bone (0xFFFF: none).
std::optional<m2::PhysicsData> Export(const Model& model, ProfileId profile,
                                      const std::function<u32(u32)>& boneOf, Diagnostics& out);

} // namespace m2_physics
} // namespace wem
} // namespace models
} // namespace whiteout
