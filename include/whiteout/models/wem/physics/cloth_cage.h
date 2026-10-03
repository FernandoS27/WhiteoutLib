// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file cloth_cage.h
 * @brief A cloth made from the faces of a mesh, and its cage made by the
 *        editor (EDIT_MODE_PHYSICS_CLOTH_DESIGN.md §4-§6).
 *
 * The cloth is chosen as faces. *Make cloth* moves them into a section of
 * their own and builds the simulation mesh Blizzard's artists simplify by
 * hand: the cage, a hidden `ClothSimulated` section of the same mesh, skinned
 * to its anchors, with the drawn faces bound to it. What holds it is Blizzard's
 * rule: a point whose skin is at least `threshold` on the cloth bones moves,
 * every other is pinned to the bones that are left.
 *
 * A section is a mesh-local index, so splitting, merging and erasing one
 * carries every section referencer along (`FollowSectionRemap`). Engine-free;
 * the editor journals what these write.
 */

#include <array>
#include <span>
#include <string>
#include <vector>

#include <whiteout/common_types.h>

#include "../diagnostics.h"
#include "../document.h"

namespace whiteout {
namespace models {
namespace wem {

/// The face slots of @p mesh whose section is @p section.
std::vector<u32> SectionFaces(const Mesh& mesh, u32 section);

/**
 * @brief Every holder of a section index of @p mesh renumbered through
 *        @p remap (`remap[old]` = new, `kInvalidIndex` for a section that is
 *        gone): `Section` channels, StarCraft II mesh emitters, cloth cages,
 *        bindings and their sections of origin.
 *
 * A channel of a section that is gone is erased with its sub-tracks; a cloth
 * whose cage is gone goes, and its channels and stages are let go as a removed
 * record's are. The mesh's own face layer is the caller's.
 */
void FollowSectionRemap(Document& document, u32 model, u32 mesh, std::span<const u32> remap, Diagnostics& out);

/// @p section of @p mesh gone with its faces, and every referencer following.
/// @p faces, face slots of the mesh, are renumbered past the faces that went,
/// in their order; one of the section's own becomes `kInvalidIndex`. False,
/// nothing changed, when its faces are every face of the mesh.
bool EraseSection(Document& document, u32 model, u32 mesh, u32 section, Diagnostics& out,
                  std::vector<u32>* faces = nullptr);

/// @p from's `Section` channels stated again for @p to, sub-tracks and all.
/// Returns how many.
u32 CloneSectionChannels(Document& document, u32 model, u32 mesh, u32 from, u32 to);

// ============================================================================
// Choosing (§4)
// ============================================================================

/// Per vertex of @p mesh, the share of its skin on @p bones: 0 for a vertex
/// with no skin.
std::vector<f32> BoneShare(const Mesh& mesh, std::span<const u32> bones);

/// *By bone* (§4.2): the faces of @p mesh whose vertices take, on average, at
/// least half their weight from @p bones.
std::vector<u32> FacesByBone(const Mesh& mesh, std::span<const u32> bones);

/// The cloth bones proposed for @p faces of mesh @p mesh (§4.3): every bone
/// with at least 90% of its skin weight, over the whole model, on their
/// vertices, and none above a bone that skins anything else. Node order, so a
/// chain comes root first.
std::vector<u32> ProposeClothBones(const Model& model, u32 mesh, std::span<const u32> faces);

/// The bones of @p bones whose parent is not one of them: the roots of the
/// chains they form.
std::vector<u32> ClothBoneRoots(const NodeTree& tree, std::span<const u32> bones);

// ============================================================================
// The source and the pin rule (§4.3, §5.3 step 1)
// ============================================================================

/// The chosen faces, welded by position: what every cage is built from.
struct ClothSource {
    u32 mesh = kInvalidIndex;
    std::vector<u32> faces;                    ///< The face slots it came from.
    std::vector<Vector3f> points;              ///< Welded positions.
    std::vector<std::array<u32, 3>> triangles; ///< Over `points`, each face's own triangulation.
    std::vector<u32> pointOf;                  ///< Per mesh vertex: its point, or `kInvalidIndex`.
    std::vector<std::vector<geom::Influence>> skin; ///< Per point: its first vertex's skin.
    std::vector<f32> share;                    ///< Per point: its skin's share on the cloth bones.
    std::vector<u8> seam;   ///< Per point: also on a face that is not cloth.
    std::vector<u8> border; ///< Per point: on an edge only one of the faces has.
    Vector3f lower{0, 0, 0};
    Vector3f upper{0, 0, 0};
    f32 edge = 0.0f; ///< The mean edge length.

    /// The diagonal of the points' box: what *Reach* and the weld are shares of.
    f32 size() const;
};

/// @p faces of @p mesh welded within a ten-thousandth of their size, with each
/// point's share on @p bones.
ClothSource GatherClothSource(const Mesh& mesh, u32 meshIndex, std::span<const u32> faces,
                              std::span<const u32> bones);

/// Blizzard's rule (§4.3): per point, whether it is pinned. A point moves when
/// at least @p threshold of its skin is on @p bones. When nothing would be
/// pinned, the row nearest the root cloth bone's pivot is; with no cloth bones,
/// the row on the seam with the rest of the mesh, else the highest tenth.
std::vector<u8> PinPoints(const NodeTree& tree, const ClothSource& source, std::span<const u32> bones,
                          f32 threshold);

/// The anchors of a pinned particle whose skin is @p skin: that skin with
/// @p bones' share taken out, renormalised; with nothing left, the parent of
/// the cloth bones' root at 1, Blizzard's "usually the parent bone".
std::vector<geom::Influence> PinAnchors(const NodeTree& tree, std::span<const geom::Influence> skin,
                                        std::span<const u32> bones);

// ============================================================================
// The cage (§5)
// ============================================================================

/// A simulation mesh: one particle per point.
struct ClothCage {
    ClothCageKind kind = ClothCageKind::AsModelled;
    std::vector<Vector3f> points;
    std::vector<std::array<u32, 3>> triangles;
    std::vector<u8> movable;
    std::vector<std::vector<geom::Influence>> anchors;
    /// Per particle, the source point it stands on, or `kInvalidIndex`.
    std::vector<u32> sourcePoint;
    /// Sheets merged into one cage (§5.5); 1 for a single layer.
    u32 layers = 1;
    /// How far merging moved the cage off its first sheet: what binding the
    /// second has to reach.
    f32 layerGap = 0.0f;
};

/// The kind *Auto* picks (§5.2): the faces as modelled when they have no more
/// points than @p particles, else a Strip when they wrap round the chain of
/// @p bones, else a Grid. `BuildCage` reduces when a Grid cannot be laid.
ClothCageKind PickCageKind(const NodeTree& tree, const ClothSource& source, std::span<const u8> pinned,
                           std::span<const u32> bones, u32 particles);

/// @p source's cage, of @p recipe's kind (Auto resolved), pinned as
/// @p pinned says and anchored by @p recipe's cloth bones. A Grid that cannot
/// be laid, or a Strip with no axis, is Reduced; sheets back to back are one
/// cage between them.
ClothCage BuildCage(const NodeTree& tree, const ClothSource& source, std::span<const u8> pinned,
                    const ClothRecipe& recipe);

/// *Reduced* (§5.3, last part): @p cage's edges collapsed by quadric error down
/// to @p target particles, pinned particles kept and a border point moving only
/// along the border; a collapse that flips a triangle or leaves a thinner one
/// than 15 degrees is refused.
void ReduceCage(ClothCage& cage, u32 target);

/// One drawn vertex's binding (§5.6): up to three cage particles and their
/// barycentric weights; none when it lies beyond reach.
struct ClothBind {
    std::array<u32, 4> lanes{kInvalidIndex, kInvalidIndex, kInvalidIndex, kInvalidIndex};
    std::array<f32, 4> weights{0, 0, 0, 0};
    f32 distance = 0.0f;
};

/// @p point bound to its closest point on @p cage, within @p reach.
ClothBind BindToCage(const ClothCage& cage, const Vector3f& point, f32 reach);

// ============================================================================
// The verbs (§6), on the document
// ============================================================================

/// What the view shows while faces are chosen (§4.2): the counts of the
/// readout, the pins the rule would make and the hang line between them and
/// the rest.
struct ClothPreview {
    u32 faces = 0;
    u32 points = 0;
    u32 pinned = 0;
    std::vector<u32> bones;
    ClothCageKind kind = ClothCageKind::AsModelled;
    u32 particles = 0; ///< What the cage would have.
    std::vector<Vector3f> pins;
    std::vector<std::pair<Vector3f, Vector3f>> hangLine;
};

/// The preview of @p recipe made from @p faces of @p mesh. Its cloth bones are
/// the recipe's, or the proposal when it names none. *From other faces* counts
/// @p cageFaces' points.
ClothPreview PreviewCloth(const Model& model, u32 mesh, std::span<const u32> faces, const ClothRecipe& recipe,
                          std::span<const u32> cageFaces = {});

/// Why @p faces of @p mesh cannot be made a cloth, as a catalog key; null when
/// they can.
const char* CanMakeClothFromFaces(const Document& document, u32 model, u32 mesh, std::span<const u32> faces);

/// The recipe a new cloth of @p model starts from: the next free name, the
/// proposed bones, and *Full detail* when it has none (D1).
ClothRecipe DefaultClothRecipe(const Model& model, u32 mesh, std::span<const u32> faces);

/**
 * @brief *Make cloth* (§6.1): @p faces of @p mesh moved into sections of their
 *        own, a cage built and bound, and a cloth record made of @p recipe.
 *
 * Each section the faces come from gives a "(cloth)" section with its
 * material, flags and channels, flagged `ClothInfluenced`; every face of a
 * section moves the section itself. The cage is a `Hidden | ClothSimulated`
 * section of the same mesh. The record takes the *Medium* preset and every
 * collider. Returns the cloth's id, 0 when refused.
 *
 * *From other faces* with @p cageFaces: those faces, which the user modelled,
 * move into the cage's section as they are, and are skinned to their anchors.
 */
u32 MakeClothFromFaces(Document& document, u32 model, u32 mesh, std::span<const u32> faces,
                       const ClothRecipe& recipe, Diagnostics& out, std::span<const u32> cageFaces = {});

/// Why @p cageFaces cannot be the cage of a cloth drawn by @p faces, as a
/// catalog key; null when they can.
const char* CanUseCageFaces(const Document& document, u32 model, u32 mesh, std::span<const u32> faces,
                            std::span<const u32> cageFaces);

/// The faces cloth @p cloth draws: every face of its bound sections.
std::vector<u32> ClothFaces(const Model& model, const Cloth& cloth);

/**
 * @brief *Update cloth* and *Remake* (§6.2): cloth @p cloth made again from
 *        @p faces (its own, for a remake) with @p recipe.
 *
 * Its id, parameters, colliders and *Active* stay; faces it no longer has go
 * back to their sections; pins painted by hand are carried to the nearest new
 * particle when the recipe says so. A cage of the user's own stays, and only
 * its pins, anchors and binding are made again. False, nothing changed, when
 * refused.
 */
bool RemakeCloth(Document& document, u32 model, u32 cloth, std::span<const u32> faces, const ClothRecipe& recipe,
                 Diagnostics& out);

/// *Delete cloth* (§6.2): each bound section merged back into its section of
/// origin, the cage erased, the record gone. The cloth bones stay.
bool DeleteCloth(Document& document, u32 model, u32 cloth, Diagnostics& out);

} // namespace wem
} // namespace models
} // namespace whiteout
