// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file flatten.h
 * @brief The flatteners (EDIT_MODE_UV_DESIGN.md §6, EDIT_MODE_UV_AUDIT.md U6).
 *
 * LSCM -- least squares conformal maps, Lévy et al. 2002 -- is the one every
 * solve starts from: it needs no boundary given to it, holds whatever the
 * caller pins, and fails in a way anyone can see: angles are kept and area is
 * not. Minimum stretch refines its map by SLIM where area matters too, and
 * starts over from Tutte's embedding where LSCM's own minimum folds; which one
 * a command uses is the workspace's one option.
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
    /// Stop when the normal residual `‖DAᵀr‖`, `D` scaling each column to
    /// unit length, falls below this share of where the solve started.
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
    u32 iterations = 0; ///< The least-squares solve's.
    u32 rounds = 0;     ///< Minimum stretch's, after LSCM's solve.
    /// The residual `LscmOptions::tolerance` is measured in, at the end.
    /// Above it with `capped` unset, the doubles' floor stopped it: as
    /// converged as the arithmetic allows.
    f32 residual = 0.0f;
    /// The solve ran out of iterations short of the tolerance: its map may be
    /// unfinished, and a fold in it may be the solve's.
    bool capped = false;

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

/// Minimum stretch (EDIT_MODE_UV_AUDIT.md U6), Blender's method of the name:
/// `LscmPinning`'s map, then SLIM (Rabinovich et al. 2017) on the symmetric
/// Dirichlet energy, which grows without bound as a triangle loses its area --
/// 25 rounds at most, or until a round buys under a ten-thousandth of it. Each
/// round stops short of turning a triangle over, so it never makes a fold; an
/// island LSCM folded without pins starts from Tutte's embedding instead, and
/// one that still folds (pins holding the fold) keeps LSCM's map. Keeps the
/// map's size and, with nothing held, the place and turn LSCM landed it at;
/// the pins hold as they do in LSCM.
FlattenResult MinimumStretch(Mesh& mesh, const UvIslands& islands, u32 island, u32 set,
                             std::span<const u32> alsoPinned = {}, const LscmOptions& options = {});

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
/// and the inside solved around them. The sides are in proportion to the
/// boundary's own arc lengths, so a strip that was twice as long as it was wide
/// still is; the result is fitted back over the island's old map -- its area,
/// its centre -- as any re-solve lands.
RectangleResult Rectangle(Mesh& mesh, const UvIslands& islands, u32 island, u32 set);

/// The shapes a projection can take. The frame is the caller's: the library
/// does not know where the camera is. Box sends each face to the frame's axis
/// its normal is nearest, plane by plane, and cuts where two planes meet
/// (EDIT_MODE_UV_REDESIGN.md §8).
enum class ProjectShape : u8 { Planar, Cylinder, Sphere, Box };

/// A projector, framed as a camera is (EDIT_MODE_UV_AUDIT.md §4.1): `axisU` is
/// the image's right, `axisV` its down -- the way a texture's v runs -- and
/// `axisN = axisU x axisV` the way it looks. A surface the projector looks at
/// from outside therefore reads upright and unmirrored, whatever the shape:
/// - Planar maps (d.U, d.V).
/// - Box's front is the side facing the projector (-N); each of the six reads
///   as seen from outside, upright, with -V up and the front at a top's top.
/// - Cylinder's axis is V, so its height runs down the image; U runs round
///   from the front, and the wrap is at the back (+N).
/// - Sphere's up pole is -V; the same turn and wrap.
struct ProjectFrame {
    Vector3f origin{0.0f, 0.0f, 0.0f};
    Vector3f axisU{0.0f, 1.0f, 0.0f};
    Vector3f axisV{0.0f, 0.0f, -1.0f};
    Vector3f axisN{-1.0f, 0.0f, 0.0f};
    /// World units per tile: the map at the world's own scale when set (Max's
    /// Length and Tile), and the caller's to scale when zero.
    f32 extent = 0.0f;
};

/// The frame that looks along @p look at @p origin, upright: the image's up is
/// @p up laid on the view, or @p front where the view runs along @p up (a top
/// or a bottom view, the front at the image's top).
ProjectFrame UprightFrame(const Vector3f& origin, const Vector3f& look, const Vector3f& up,
                          const Vector3f& front);

/// A cylinder's or a sphere's frame round @p pole: the pole down the image from
/// its end nearer @p up (else nearer @p front), looked at from the front, so
/// the wrap falls on the back.
ProjectFrame PoleFrame(const Vector3f& origin, const Vector3f& pole, const Vector3f& up,
                       const Vector3f& front);

struct ProjectOptions {
    /// Cylinder: the faces within 45 degrees of its axis are laid flat as its
    /// caps, each seen from outside as Box's top and bottom are, and cut from
    /// the side -- Max's Cap.
    bool cap = false;
};

/// Projects @p faces into `uvN` through @p frame, and marks the projection's
/// own cuts: the cylinder's and the sphere's wrap, where a face would span the
/// turn, Box's edges between planes, and a cap's rim. A projection is not a
/// solve and cannot fail on the geometry, so the result carries only its
/// counts.
FlattenResult Project(Mesh& mesh, std::span<const FaceId> faces, u32 set, ProjectShape shape,
                      const ProjectFrame& frame, const ProjectOptions& options = {});

/// Sander's L2 stretch per face slot, normalised by its island's own scale: 1
/// is isometric, above 1 is stretched, below 1 is squashed, and 0 is a face
/// with no UV area at all. `kInvalidId`-island faces get 0.
std::vector<f32> FaceStretch(const Mesh& mesh, const UvIslands& islands, u32 set);

} // namespace uv
} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
