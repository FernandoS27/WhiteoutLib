// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file flatten.h
 * @brief One flattener (EDIT_MODE_UV_DESIGN.md §6).
 *
 * LSCM -- least squares conformal maps, Lévy et al. 2002 -- and nothing else.
 * It is the one method that needs no boundary given to it, holds whatever the
 * caller pins, and fails in a way anyone can see: angles are kept and area is
 * not. A second flattener would be a second set of answers to the same
 * question, and the design says no.
 *
 * The solve is CGLS over the least-squares system, applying `A` and `Aᵀ` and
 * never forming `AᵀA`: a direct factorisation would be a dependency, and this
 * is a few hundred lines that reaches the same map.
 *
 * Everything here writes `uvN` on wedges (`islands.h`) and nothing else. No
 * device, no session, and every refusal is a value.
 */

#include <span>
#include <vector>

#include <whiteout/common_types.h>

#include "../ids.h"
#include "../mesh.h"
#include "islands.h"

namespace whiteout {
namespace models {
namespace wem {
namespace geom {
namespace uv {

struct LscmOptions {
    /// Stop when `‖Aᵀr‖ / ‖Aᵀb‖` falls below this.
    f32 tolerance = 1e-6f;
    /// And stop, whatever the residual, after this many iterations per unknown.
    u32 iterationsPerUnknown = 4;
    /// Hold the automatic pins at the UVs they already have, so re-unwrapping
    /// an island leaves it where it was rather than somewhere else the same
    /// shape. Off, the pair lands on a unit segment.
    bool holdCurrent = true;
};

struct FlattenResult {
    enum class Refusal : u8 {
        None = 0,
        Closed,    ///< No boundary at all: it must be cut before it can be laid flat.
        NoFaces,   ///< Nothing to solve.
        TooFewPins ///< Fewer than two wedges to hold, and none could be found.
    };

    Refusal refusal = Refusal::None;
    u32 flipped = 0;    ///< Triangles whose UV winding disagrees with the island's.
    u32 degenerate = 0; ///< Triangles with no 3D area, skipped.
    u32 iterations = 0;
    f32 residual = 0.0f;

    bool ok() const {
        return refusal == Refusal::None;
    }
};

/// Flattens @p island of @p islands into `uvN`, holding its `uvPinN` corners
/// and, below two of them, the two boundary wedges farthest apart in 3D.
///
/// The pins are what fix the map's place, turn and scale, which the conformal
/// energy says nothing about: two is the fewest that fixes all three, and more
/// is the caller's business (a Sew pins the larger side, a Straighten pins a
/// line). A closed island is refused rather than solved badly.
FlattenResult Lscm(Mesh& mesh, const UvIslands& islands, u32 island, u32 set,
                   const LscmOptions& options = {});

/// `Lscm` holding @p alsoPinned as well as the layer's own pins, each at the UV
/// it already has. The form Sew (which pins the larger side), Straighten and
/// Rectangle solve through: all three are "hold this much of it and re-solve
/// the rest", and none of them wants to write the pin layer to say so.
FlattenResult LscmPinning(Mesh& mesh, const UvIslands& islands, u32 island, u32 set,
                          std::span<const u32> alsoPinned, const LscmOptions& options = {});

struct RelaxOptions {
    u32 passes = 10;
    /// Hold the island's boundary where it is. Off, the boundary moves too,
    /// which is what a modeller asks for by selecting it.
    bool holdBoundary = true;
};

/// Walks @p wedges downhill in area-weighted stretch, one ring at a time
/// (EDIT_MODE_UV_DESIGN.md §6.3).
///
/// Not a solve: a local smoothing that never takes a step which would turn a
/// triangle over, so a modeller can hold it down on a bad patch and watch it
/// improve without ever watching it break. Boundary and pinned wedges stand
/// unless they were asked for.
FlattenResult Relax(Mesh& mesh, const UvIslands& islands, std::span<const u32> wedges, u32 set,
                    const RelaxOptions& options = {});

/// Pulls each connected run of @p uvEdges onto the straight line through its
/// two ends -- snapped to horizontal or vertical when it is within ten degrees
/// of one -- and re-solves the rest of the island around it.
FlattenResult Straighten(Mesh& mesh, const UvIslands& islands,
                         std::span<const HalfedgeId> uvEdges, u32 set);

struct RectangleResult {
    FlattenResult solve;
    /// How many corners the boundary turned at. Four is the answer; anything
    /// else is the refusal, and the number is what the panel says.
    u32 corners = 0;

    bool ok() const {
        return corners == 4 && solve.ok();
    }
};

/// Lays @p island out as a rectangle: its boundary's four corners at the
/// corners, every boundary wedge between them at its own share of that side,
/// and the inside solved around them. The sides are the boundary's own arc
/// lengths, so a strip that was twice as long as it was wide still is.
RectangleResult Rectangle(Mesh& mesh, const UvIslands& islands, u32 island, u32 set);

/// The three shapes a projection can take. The frame is the caller's: the
/// library does not know where the camera is, and Fit is `FitPlane`'s answer.
enum class ProjectShape : u8 { Planar, Cylinder, Sphere };

/// An orthonormal frame with a place. `axisN` is the projection's axis: the
/// plane's normal, the cylinder's and the sphere's pole.
struct ProjectFrame {
    Vector3f origin{0.0f, 0.0f, 0.0f};
    Vector3f axisU{1.0f, 0.0f, 0.0f};
    Vector3f axisV{0.0f, 1.0f, 0.0f};
    Vector3f axisN{0.0f, 0.0f, 1.0f};
};

/// Projects @p faces into `uvN` through @p frame, and marks the projection's
/// own cuts: the cylinder's and the sphere's wrap, where a face would span the
/// turn. A projection is not a solve and cannot fail on the geometry, so the
/// result carries only its counts.
FlattenResult Project(Mesh& mesh, std::span<const FaceId> faces, u32 set, ProjectShape shape,
                      const ProjectFrame& frame);

/// Sander's L2 stretch per face slot, normalised by its island's own scale: 1
/// is isometric, above 1 is stretched, below 1 is squashed, and 0 is a face
/// with no UV area at all. `kInvalidId`-island faces get 0.
std::vector<f32> FaceStretch(const Mesh& mesh, const UvIslands& islands, u32 set);

} // namespace uv
} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
