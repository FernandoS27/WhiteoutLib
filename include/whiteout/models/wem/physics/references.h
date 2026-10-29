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
 * | `WowBodyExtension` | `parent` (a body id) | cleared; the body stays |
 * | `ClothCollider` | `node` (`kInvalidNode` = the root) | the collider goes, and leaves every cloth's list |
 * | `ClothRecipe` | `bones` | the bone leaves the list; the cloth stays |
 * | `FractureRecipe` | `bones` | the bone leaves the list |
 * | `FractureRecipe` | `helper`, `field`, `wholeGate`, `piecesGate`, `skinNodes` | names none; a skin node keeps its place |
 * | `Cloth` | `cage` | the cloth goes |
 * | `ClothBinding` | `section` | the binding goes |
 * | `AnimChannel` of kind `Physics` | `target.sub` | invalidated (`sub` 0), as a removed node's channel is |
 * | `PoseStage` | `rig`, `cloth` | cleared; the stage stays |
 * | `cloth.bind.vertex` | vertex ids of its own mesh | rewritten by every vertex renumbering (`geom::IsVertexReferenceLayer`) |
 * | `SoftLump` | `carrier` | the lump hangs from the bone that took the removed one's skin (`SoftFollowRemovedNodes`), else from the model |
 * | `SoftLump` | `node` | a bone's own lump becomes a surface lump that hangs from nothing, with no lanes: its numbers stay, since layers name lumps by place |
 * | `SoftBulge` | `chain` | the bone leaves the chain |
 * | `SoftWobble`, `SoftSquash` | `axisBone` | names none |
 * | `SoftDrip` | `strandNodes`, `dropNodes` | names none, and keeps its place: three a strand, one a droplet |
 * | `SoftBody` | `meshes`, `drips`, a part's and a place's `mesh` | the mesh leaves the list; a body with none left goes |
 * | `SoftPart` of `inside`, `SoftDrip` | `material` (a slot) | takes the slot's replacement, else names none |
 * | `SoftPart` of `riders` | `material` (a slot), `mesh` | takes the replacement where its faces can still be told apart; else, and where its mesh is merged with another, the part is let go |
 * | The soft channels | `SoftBodyOfSub(target.sub)` | invalidated with their body |
 *
 * The node and mesh tables call in here (`RemapNodeReferencers`,
 * `RemapMeshReferencers`), so a removal anywhere carries the physics along
 * without a second list to keep in step. Where the node indices are is
 * `nodes/references.h`'s to say (`ForEachPhysicsNodeReference`).
 */

#include <span>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/vector_types.h>

#include "../diagnostics.h"
#include "../geometry/skin.h"
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

/// The same for the soft bodies. Returns the ids of the bodies left with no
/// mesh, which go.
std::vector<u32> RemapSoftMeshes(PhysicsSet& physics, std::span<const u32> meshRemap);

/// @p mesh's soft layers gone: what one that leaves its model's soft bodies
/// behind takes off. They name a body by its id and its lumps by number, so
/// in another model they would join whichever body has the id there.
void DropSoftLayers(Mesh& mesh);

/// Material slot @p slot of @p model is about to go, its users taking
/// @p replacement (`kInvalidIndex` for none): a soft body's parts and a Drip's
/// strands follow. Called while the sections still name @p slot. A part that
/// rides rigidly is let go where the replacement already draws faces of its
/// mesh, since it would grow to them; returns how many were.
u32 RemapSoftSlots(Model& model, u32 slot, u32 replacement);

/// Meshes are about to be merged into @p keep (@p merged, a byte a mesh, @p keep
/// among them): the soft bodies follow their points there. A place on an
/// absorbed mesh takes @p faceBase of that mesh more, where its faces start in
/// the merged one. A part that rode rigidly on any of them is let go, since
/// the merged mesh draws as one; returns how many were.
u32 SoftFollowMergedMeshes(PhysicsSet& physics, std::span<const u8> merged, u32 keep, std::span<const u32> faceBase);

/// Mesh @p mesh's faces were numbered again, from @p before to @p after: an
/// edit deleted, added or reordered some. A place is a face's number and
/// weights on its first triangle, so each one the soft bodies have on the mesh
/// (an effect's, a kick's, a Drip's site) is put on the face of @p after
/// nearest where it stood on @p before, a face of its body's where there is
/// one. One whose spot has no face left near any of the body's is left with no
/// place. Returns how many were written.
u32 SoftFollowRenumberedFaces(PhysicsSet& physics, u32 mesh, const Mesh& before, const Mesh& after);

/// Nodes are about to go (@p doomed, a byte a node), their skin moving to
/// @p replacement (a node each, or `kInvalidNode`): the lumps follow the skin.
/// A lump that hung from a doomed bone hangs from its replacement, and a
/// doomed bone's own lump becomes a surface lump that hangs from nothing,
/// with no lanes.
void SoftFollowRemovedNodes(PhysicsSet& physics, std::span<const u8> doomed, std::span<const u32> replacement);

/// Rewrites @p mesh's section indices through @p sectionRemap
/// (`kInvalidIndex` = gone). Returns the ids of the cloths that lost their cage.
std::vector<u32> RemapPhysicsSections(PhysicsSet& physics, u32 mesh, std::span<const u32> sectionRemap);

/// Removes the bodies @p ids name, and the joints on them. Returns the ids of
/// every record that went.
std::vector<u32> RemovePhysicsBodies(PhysicsSet& physics, std::span<const u32> ids);

/// Invalidates the `Kind::Physics` channels that name one of @p removed, a
/// soft body's by the body in their `sub`: `sub` becomes 0, the declaration
/// and its sub-tracks stay.
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

struct Clip;

/// The same for a baked cloth's drivers (`Channel::ClothDriverTranslation` /
/// `ClothDriverRotation`) in @p clips: their keys are relative to the cloth's
/// holder's frame, so a rig restatement that moves it must carry them, or the
/// cloth turns about the holder by the difference.
void RebaseClothDrivers(Model& model, std::span<Clip* const> clips, std::span<const Matrix44f> change);

/// The inverse of an affine row-vector matrix (`p * M`).
Matrix44f AffineInverse(const Matrix44f& m);

/// Whether a body or a cloth collider sits on @p node.
bool PhysicsUsesNode(const PhysicsSet& physics, u32 node);

/// Restates every length of the set at @p factor: shape dimensions, points,
/// vertices and matrix translations, joint-frame translations, a slide's limits
/// and speed, collider sizes and translations, wind, the skin offset and a
/// vegetation push; and break, motor and friction forces and torques at its
/// fourth power. Densities stay; gravity is a scale. A soft body's lumps, its
/// places' radii, a travel's line and a melt's floor are lengths too; the rest
/// of its settings are shares.
void RescalePhysics(PhysicsSet& physics, f32 factor);

/// The structural and warning rules of §3.11 against @p model.
void CheckPhysics(const Model& model, Diagnostics& out);

/// Why @p effect could not stand in @p body as `CheckPhysics` reads one: its
/// settings, its place, the nodes it names and its lists that run side by
/// side. Empty when it could. What an edit that takes an effect whole asks
/// before it writes.
std::string SoftEffectFault(const Model& model, const SoftBody& body, const SoftEffect& effect);

/// The profile rules of §3.11: what @p profile's format can carry.
void CheckPhysicsForProfile(const Model& model, ProfileId profile, Diagnostics& out);

/// A *Full detail* cloth's drivers (§10.3): its free particles, by cage
/// vertex ascending, and the holder they hang from, the bone outside the
/// cloth's own its pins' anchors weigh most on (`kInvalidNode`, the model
/// root, when none does).
struct ClothDrivers {
    u32 holder = kInvalidNode;
    std::vector<u32> vertices;
};
ClothDrivers ClothDriversOf(const Model& model, const Cloth& cloth);

/**
 * @brief Each vertex's skin as a file holding @p cap weights a point writes it.
 *
 * Normalised; a vertex of a rigid section that binds nothing rides that
 * section's node whole; more than @p cap influences are folded over the
 * skeleton (`geom::FoldInfluences`), which is what an export does. The count
 * by bones a point (§5.3) and the lanes (§6.4) are for this skin, never the
 * document's wider one.
 */
class SoftSkinSource {
public:
    SoftSkinSource(const Model& model, u32 cap);

    /// Vertex @p vertex of mesh @p mesh: heaviest first, ties by bone. Valid
    /// until the next call.
    std::span<const geom::Influence> of(u32 mesh, u32 vertex);

    u32 cap() const {
        return cap_;
    }

private:
    const Model* model_ = nullptr;
    u32 cap_ = 4;
    std::vector<u32> parents_;
    std::vector<Vector3f> pivots_;
    std::vector<std::vector<u32>> rigidOf_; ///< Per mesh, per vertex; empty when no section is rigid.
    std::vector<geom::Influence> scratch_;
};

} // namespace wem
} // namespace models
} // namespace whiteout
