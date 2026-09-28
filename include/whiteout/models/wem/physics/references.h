// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file references.h
 * @brief What `PhysicsSet` holds of the rest of a model, and how it follows a
 *        change (WEM_PHYSICS_DESIGN.md §3.10).
 *
 * | Referencer | Field | When it goes |
 * |---|---|---|
 * | `PhysicsBody` | `node` | the body goes, and every joint on it |
 * | `ClothCollider` | `node` (`kInvalidNode` = the root) | the collider goes, and leaves every cloth's list |
 * | `Cloth` | `cage` | the cloth goes |
 * | `ClothBinding` | `section` | the binding goes |
 * | `AnimChannel` of kind `Physics` | `target.sub` | invalidated (`sub` 0), as a removed node's channel is |
 * | `PoseStage` | `rig`, `cloth` | cleared; the stage stays |
 * | `cloth.bind.vertex` | vertex ids of its own mesh | rewritten by every vertex renumbering (`geom::IsVertexReferenceLayer`) |
 *
 * The node and mesh tables call in here (`RemapNodeReferencers`,
 * `RemapMeshReferencers`), so a removal anywhere carries the physics along
 * without a second list to keep in step.
 */

#include <span>
#include <vector>

#include <whiteout/common_types.h>

#include "../diagnostics.h"
#include "../model.h"

namespace whiteout {
namespace models {
namespace wem {

/// Rewrites every node index the set holds through @p remap (`remap[old]` = new
/// index, or `kInvalidNode` for a node that is gone). Returns the ids of the
/// records that went with their node.
std::vector<u32> RemapPhysicsNodes(PhysicsSet& physics, std::span<const u32> remap);

/// Rewrites every mesh index through @p meshRemap (`kInvalidIndex` = gone).
/// Returns the ids of the cloths that lost their cage.
std::vector<u32> RemapPhysicsMeshes(PhysicsSet& physics, std::span<const u32> meshRemap);

/// Rewrites @p mesh's section indices through @p sectionRemap
/// (`kInvalidIndex` = gone). Returns the ids of the cloths that lost their cage.
std::vector<u32> RemapPhysicsSections(PhysicsSet& physics, u32 mesh, std::span<const u32> sectionRemap);

/// Invalidates the `Kind::Physics` channels that name one of @p removed:
/// `sub` becomes 0, the declaration and its sub-tracks stay.
void InvalidatePhysicsChannels(AnimChannelTable& channels, std::span<const u32> removed, Diagnostics& out);

/// Clears the `rig` and `cloth` of every stage naming one of @p removed: the
/// stage stays, as the capsule chain over its own nodes.
void DetachPoseStages(std::span<PoseStage> stages, std::span<const u32> removed);

/// Re-expresses every frame that sits on a node -- shapes, joint frames,
/// colliders -- through @p change, one matrix per node (row vectors): a node
/// whose bind frame went from `W_old` to `W_new` passes `W_old * W_new^-1`, and
/// what rode it stays where it was in model space. An identity entry changes
/// nothing.
void RebasePhysicsFrames(PhysicsSet& physics, std::span<const Matrix44f> change);

/// The inverse of an affine row-vector matrix (`p * M`).
Matrix44f AffineInverse(const Matrix44f& m);

/// Whether a body or a cloth collider sits on @p node.
bool PhysicsUsesNode(const PhysicsSet& physics, u32 node);

/// Restates every length of the set at @p factor: shape dimensions, points,
/// vertices and matrix translations, joint-frame translations, collider sizes
/// and translations, wind and the skin offset. Densities stay; gravity is a scale.
void RescalePhysics(PhysicsSet& physics, f32 factor);

/// The structural and warning rules of §3.11 against @p model.
void CheckPhysics(const Model& model, Diagnostics& out);

/// The profile rules of §3.11: what @p profile's format can carry.
void CheckPhysicsForProfile(const Model& model, ProfileId profile, Diagnostics& out);

} // namespace wem
} // namespace models
} // namespace whiteout
