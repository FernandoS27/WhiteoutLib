// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file soft_region.h
 * @brief A soft body's region: its points, and what their skin allows
 *        (EDIT_MODE_SOFTBODY_DESIGN.md §5, §6.2).
 *
 * A region is points of one mesh or several. Everything here is a pure
 * function of a model and those points, at the bind: nothing is posed and
 * nothing is written.
 */

#include <array>
#include <span>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/vector_types.h>

#include "../geometry/skin.h"
#include "../model.h"
#include "references.h"

namespace whiteout {
namespace models {
namespace wem {

/// A region's points on one mesh: its vertices, ascending, none twice.
struct SoftMeshPoints {
    u32 mesh = kInvalidIndex;
    std::vector<u32> vertices;
};

/// The vertices of @p faces of mesh @p mesh of @p model, as a region's points
/// on it: what *By material* and *Whole mesh* choose (§5.1).
SoftMeshPoints SoftPointsOfFaces(const Model& model, u32 mesh, std::span<const u32> faces);

/// Every vertex a face of mesh @p mesh draws.
SoftMeshPoints SoftPointsOfMesh(const Model& model, u32 mesh);

/// What a bone is to a region that rides it (§6.2).
struct SoftBoneCount {
    u32 node = kInvalidNode;
    f32 share = 0.0f;          ///< Of the region's area: its weight on each point, times the point's area.
    bool outside = false;      ///< A point outside the region rides it.
    bool outsideBelow = false; ///< A point outside the region rides a bone below it.
    bool skinBelow = false;    ///< A bone that carries skin hangs below it.
    bool parentless = false;   ///< No bone above it: it hangs from the model.
    bool staged = false;       ///< A stage places it or reads it (`SoftBoneIsStaged`).

    /// The bone can be a lump as it is: an end of the skeleton, the region's
    /// alone, and no stage's. Otherwise it is copied.
    bool asItIs() const {
        return !outside && !outsideBelow && !skinBelow && !staged;
    }
};

/// Whether a pose stage of @p model stands between @p node's keys and what
/// plays (§6.5): one that places it writes over what a bake keyed on it, and
/// one that reads it hands the bone's departure on to what it moves. A Link
/// carries its own bone with its keys, so being a Link's is not being staged.
/// Whether the stage is on does not enter: a switch must not remake a body.
bool SoftBoneIsStaged(const Model& model, u32 node);

/// One connected piece of a region's surface, joined across seams.
struct SoftShell {
    u32 mesh = kInvalidIndex;
    u32 faces = 0;
    f32 area = 0.0f;
    /// Signed, by the faces' winding: negative on a shell turned inside out
    /// (§5.4). Meaningful only when the shell is closed.
    f32 volume = 0.0f;
    bool closed = false; ///< No edge of it has one face only.
};

/// A region counted (§5.3's readout, §6.2's proposal).
struct SoftCount {
    u32 points = 0; ///< Vertices a face of the region draws.
    u32 faces = 0;  ///< Faces whose corners are all the region's.
    f32 area = 0.0f;
    /// The share of the area on points that ride one, two, three, and four or
    /// more bones.
    std::array<f32, 4> byBones{};
    std::vector<SoftBoneCount> bones; ///< The bones it rides, largest share first, ties by node.
    std::vector<SoftShell> shells;    ///< By mesh, then by their first face.

    u32 asTheyAre() const;
    u32 copied() const;
    u32 parentless() const;
    /// The share the @p count largest bones carry together.
    f32 largestShare(u32 count) const;
};

/// @p region of @p model counted, under @p cap weights a point. A face is the
/// region's when all its corners are; a point stands for a third of each of
/// its region triangles.
SoftCount CountSoftRegion(const Model& model, std::span<const SoftMeshPoints> region, u32 cap);

/// The largest side of the box round @p region's points: what *Blend*, a
/// place's radius and every setting given as a share of "the region" mean.
f32 SoftRegionSize(const Model& model, std::span<const SoftMeshPoints> region);

/// Per mesh of @p model, per section: whether a face of it draws a point a
/// soft body holds. What a host must be able to move (§10).
std::vector<std::vector<u8>> SoftSections(const Model& model);

/**
 * @brief *Blend at the edge* (§5.2): each of @p region's points' softness, one
 *        list a part in its vertices' order.
 *
 * Softness is 0 on the region's border and rises to 1 at @p blend of the
 * region's size inside, by distance over the surface, seam twins together.
 * The border is where the region meets the rest of its own mesh, so a region
 * that is a whole mesh has none and is soft everywhere, open edges included.
 * A @p blend of 0 is soft everywhere but on the border itself.
 */
std::vector<std::vector<f32>> SoftBlendSoftness(const Model& model, std::span<const SoftMeshPoints> region,
                                                f32 blend);

/// What *Fold small weights* would do to a region (§5.3): each changed
/// vertex and the skin it would get.
struct SoftFoldPlan {
    struct Change {
        u32 mesh = 0;
        u32 vertex = 0;
        std::vector<geom::Influence> skin; ///< Heaviest first, summing to 1.
    };
    std::vector<Change> changes;
    /// The region's points riding one, two, three, and four or more bones,
    /// before and after.
    std::array<u32, 4> before{};
    std::array<u32, 4> after{};
};

/// @p region's points with every weight under @p under folded away, one
/// merge at a time and the skeleton's cheapest first (`geom::FoldInfluences`),
/// until none is left: nothing is written.
SoftFoldPlan PlanSoftFold(const Model& model, std::span<const SoftMeshPoints> region, f32 under, u32 cap);

/// The farthest any point @p plan changes would move in the pose @p skin
/// gives (a pose's skinning matrices by node).
f32 SoftFoldShift(const Model& model, const SoftFoldPlan& plan, std::span<const Matrix44f> skin);

/// Writes @p plan to @p model's skin. The one thing here that changes it.
void ApplySoftFold(Model& model, const SoftFoldPlan& plan);

} // namespace wem
} // namespace models
} // namespace whiteout
