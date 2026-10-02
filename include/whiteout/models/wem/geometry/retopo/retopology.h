// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file retopology.h
 * @brief Automatic quad retopology of one mesh, its attributes carried across
 *        (EDIT_MODE_RETOPOLOGY_DESIGN.md).
 *
 * The pipeline is the design's §1: a welded copy of the mesh is refined with
 * every vertex kept on the source surface, a cross field is solved on it, a
 * motorcycle layout of rectangles is traced in the field, the rectangles' sides
 * are quantized into integer counts, and each rectangle is filled with a grid.
 * The field method (§6) instead smooths a lattice over the surface and makes
 * its points the vertices. Every output vertex keeps the source triangle it sits on, so UVs, colours and
 * skin are interpolated there rather than searched for (§2).
 *
 * No device, no session, no exceptions: every failure is a value in the report.
 */

#include <functional>
#include <string>
#include <vector>

#include <whiteout/common_types.h>

#include "../mesh.h"
#include "quantize.h"

namespace whiteout {
namespace models {
namespace wem {
namespace geom {
namespace retopo {

/// Which source edges the layout must follow (§1.3). Open borders and section
/// borders always do.
struct FeatureOptions {
    bool sharp = true;          ///< The `sharp` layer: authored hard edges.
    /// Of those, only where the shading breaks by at least this, in radians: a
    /// gentler break is a soft fold (a low-poly cloth's), which the quads need
    /// not follow and which would split the layout into slivers.
    f32 sharpAngle = 1.30899694f;
    u32 uvSeamSets = 0xFFu;     ///< Bit N: the seams of `uvN` are features.
    bool crease = true;         ///< `crease` above zero.
    /// Edges whose dihedral angle exceeds this, in radians; 0 is off. Negative
    /// means "the default": off when the mesh has authored normals, 40 degrees
    /// when it has none.
    f32 angle = -1.0f;
    /// A feature vertex whose two feature edges turn by more than this is a
    /// corner and never moves.
    f32 cornerAngle = 0.698131701f;
};

/// How the quads are found.
enum class RetopoMethod : u8 {
    /// A position field: every output vertex on a lattice smoothed over the
    /// surface, so the quads come out even (§6).
    Field,
    /// A layout of patches traced in the cross field, each filled with a grid
    /// (§1.6-§1.8): exact to the features, less even.
    Layout,
};

struct RetopoOptions {
    RetopoMethod method = RetopoMethod::Field;
    /// Quads wanted for the whole mesh; kept pieces are not counted.
    u32 targetQuads = 1000;
    /// 0..1: how much the quad size follows curvature (0 is uniform): at one,
    /// a stretch whose shading turns a radian over a quad edge asks for three
    /// times the quads, the total kept to `targetQuads`. The layout's alone.
    f32 adaptivity = 0.0f;
    FeatureOptions features;
    /// A piece with less area than this many target quads is kept as it is.
    f32 keepBelowQuads = 4.0f;
    /// The layout's: let the quantization collapse whole strips of it to reach
    /// low budgets (§1.7). Off, an arc pays far more for each quad under one, so
    /// only a layout that balances no other way shortens one.
    bool collapseArcs = true;
    /// The field method: the lattice at twice the quad edge and every face split
    /// once, so each one is a quad (§6.3). Off, faces the lattice does not
    /// close round stay triangles.
    bool pureQuads = true;
    /// Relaxation sweeps after extraction.
    u32 relaxIterations = 20;
};

/// How far a run got and what it did, for the result line (§3).
struct RetopoReport {
    enum class Failure : u8 {
        None = 0,
        Cancelled,
        EmptyMesh,
        NotManifold, ///< The welded mesh would not build connectivity.
        Unsolved,    ///< No piece came out as quads: each was kept as it was.
    };

    Failure failure = Failure::None;
    u32 sourceTriangles = 0;
    u32 quads = 0;
    u32 otherFaces = 0;        ///< Kept pieces', unfilled and mismatched patches' faces.
    u32 piecesRemeshed = 0;
    u32 piecesKept = 0;        ///< Kept as they were: too small for a patch (§1.2), or no valid quads.
    u32 patches = 0;
    u32 patchesUnfilled = 0;   ///< Kept as triangles: the repair could not make them rectangles.
    u32 patchesMismatched = 0; ///< Opposite sides apart: triangles along the shorter.
    u32 singularities = 0;
    u32 irregularVertices = 0; ///< Interior output vertices of valence other than four.
    u32 arcsCollapsed = 0;     ///< Zero-length arcs in the final quantization.
    u32 validityRounds = 0;    ///< Re-solves the collapse check asked for.
    u32 minimumQuads = 0;      ///< The layout's floor: every arc at one quad.
    f64 targetEdge = 0.0;      ///< The quad edge the count asked for.
    f64 quantizeCost = 0.0;
    std::vector<std::string> notes;

    bool ok() const {
        return failure == Failure::None;
    }
};

/// Asked between stages -- each piece's remesh, field and layout, each
/// count and each re-solve -- from the calling thread.
struct RetopoControl {
    std::function<bool()> cancelled;
    std::function<void(u32 done, u32 total)> progress;

    bool stopped() const {
        return cancelled && cancelled();
    }
};

struct RetopoResult {
    Mesh mesh;
    RetopoReport report;
};

/**
 * @brief Retopologizes @p source into a new mesh with the same sections, every
 *        Halfedge, Vertex, Edge and Face layer and the skin carried across.
 *
 * @p solver quantizes the layout; empty uses `SolveQuantizeDoubleCover`.
 */
RetopoResult Retopologize(const Mesh& source, const RetopoOptions& options,
                          const QuantizeSolver& solver = {}, const RetopoControl& control = {});

const char* ToString(RetopoReport::Failure failure);

} // namespace retopo
} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
