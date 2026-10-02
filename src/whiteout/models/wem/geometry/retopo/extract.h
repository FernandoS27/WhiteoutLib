// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file extract.h
 * @brief From a quantized layout to quads on the source surface
 *        (EDIT_MODE_RETOPOLOGY_DESIGN.md §1.7, §1.8).
 *
 * Each rectangle of n x m quads is laid on [0,n] x [0,m] by its patch map with
 * its sides pinned arc by arc, and its grid read back out of the map. A point
 * on an arc is made once for the arc, so the two patches beside it share it,
 * and a zero-length arc or a collapsed patch merges the points it squeezes
 * together. Every output vertex has a home on the source.
 */

#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/models/wem/geometry/retopo/quantize.h>

#include "layout.h"

namespace whiteout {
namespace models {
namespace wem {
namespace geom {
namespace retopo {

/// The quantization of @p layout: a node per rectangle and direction, an arc
/// per layout arc (aiming at `targets[arc]` quads, at least @p lower long). An
/// arc beside no rectangle on a side ends at the free node there.
///
/// With @p mismatch above zero, each node also gets two slack loops, after the
/// layout's arcs, costing `mismatch` times the square of the difference between
/// its opposite sides: a layout whose sides cannot match with every arc kept
/// still has an answer, and the extraction fills such a patch quad-dominant.
/// A loop on the node itself, two heads or two tails, so the sides differ by
/// even amounts: slack to the free node would make it a hub of every patch,
/// which libSatsuma's refinement crawls through.
QuantizeProblem BuildQuantizeProblem(const Layout& layout, const std::vector<f64>& targets, i32 lower,
                                     f64 mismatch = 0.0);

/// What the extraction made: quads and kept polygons, every vertex homed.
struct QuadMesh {
    std::vector<V3> positions;
    std::vector<SurfacePoint> homes;
    std::vector<VertexKind> kinds;
    std::vector<FeaturePoint> onCurve; ///< Feature vertices' point on their curve.
    std::vector<u32> curve;            ///< Feature vertices' curve; kNone otherwise.
    /// Faces as corner lists; `faceOffsets` is CSR into `faceVertices`.
    std::vector<u32> faceOffsets{0};
    std::vector<u32> faceVertices;
    std::vector<u32> facePatch; ///< The layout patch each face came from.

    u32 vertexCount() const {
        return static_cast<u32>(positions.size());
    }
    u32 faceCount() const {
        return static_cast<u32>(facePatch.size());
    }
    std::span<const u32> face(u32 f) const {
        return std::span<const u32>(faceVertices.data() + faceOffsets[f], faceOffsets[f + 1] - faceOffsets[f]);
    }
};

struct ExtractStats {
    u32 quads = 0;
    u32 patchesFilled = 0;
    u32 patchesCollapsed = 0;
    u32 patchesSkipped = 0;   ///< Not rectangles, or sides that did not close.
    u32 patchesMismatched = 0; ///< Opposite sides apart: triangles along the shorter.
    u32 patchesFanned = 0;     ///< Disks no rectangle: a fan from the middle.
    u32 mergedVertices = 0;   ///< Squeezed together by zero-length arcs.
    u32 degenerateFaces = 0;  ///< Dropped: a face left with a repeated vertex.
};

/**
 * @brief The quads of @p layout at arc lengths @p lengths. Patches that are not
 *        rectangles keep their work triangles, so the surface stays closed.
 */
QuadMesh ExtractQuads(const WorkMesh& mesh, const Surface& surface, const Layout& layout,
                      const std::vector<i32>& lengths, ExtractStats& stats);

/// `ExtractQuads`, also telling, per arc of length zero, the output vertex its
/// two nodes were squeezed into (kNone for every other arc): what the
/// collapse loop raises the penalty of when that vertex is in a bad face.
QuadMesh ExtractQuads(const WorkMesh& mesh, const Surface& surface, const Layout& layout,
                      const std::vector<i32>& lengths, ExtractStats& stats, std::vector<u32>& zeroArcVertex);

/// Takes away every inside vertex with only two faces round it (a doublet, a
/// valence-2 point): two quads round a free point become one, any other pair
/// loses the corner.
/// Returns how many it took; vertex numbers are unchanged, the vertex unused.
u32 RemoveDoublets(QuadMesh& quads);

/// Drops the vertices no face uses, renumbering the rest.
void CompactQuads(QuadMesh& quads);

/// Evens the quads on the source: each vertex toward the mean of its
/// neighbours in its tangent plane, a feature vertex along its curve, a
/// corner never; re-homed after each move, and no move that folds a face.
void RelaxQuads(QuadMesh& quads, const Surface& surface, u32 iterations, f64 step = 0.5);

/// Faces whose corners repeat, edges shared by more than two faces, and edges
/// two faces run the same way: what a valid output must have none of.
struct QuadValidity {
    u32 repeated = 0;
    u32 nonManifoldEdges = 0;
    u32 flippedEdges = 0;
    u32 thinVertices = 0; ///< Inside vertices with fewer than three faces.
    std::vector<u32> badVertices; ///< The vertices of every fault above.
    bool ok() const {
        return repeated == 0 && nonManifoldEdges == 0 && flippedEdges == 0 && thinVertices == 0;
    }
};
QuadValidity CheckQuads(const QuadMesh& quads);

} // namespace retopo
} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
