// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file interpolate.h
 * @brief Sampling a face's corners and its vertices' skin at a new point
 *        (EDIT_MODE_MODELLING_DESIGN.md §2.4).
 *
 * Every tool that makes a vertex or a corner inside or along an existing face
 * samples that face, and does it here, so every tool follows the same rule
 * (§3.12). The same weights drive the corners and the skin: a vertex placed a
 * third of the way along an edge gets a third of the far vertex's weights, and
 * the texture a third of the way across.
 */

#include <span>
#include <string>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/vector_types.h>

#include "attributes.h"
#include "ids.h"
#include "mesh.h"
#include "skin.h"

namespace whiteout {
namespace models {
namespace wem {
namespace geom {

/**
 * @brief A polygon as it was at Plan: its corners' positions, every Halfedge
 *        layer's value at each corner, and each corner vertex's influences.
 *
 * Copied, so it stays valid after the tool relinks or deletes the corners it
 * came from. A two-corner polygon is an edge: `SplitEdge` samples one.
 */
struct SourcePolygon {
    struct Layer {
        std::string name;
        AttrType type = AttrType::F32;
        std::vector<u8> values; ///< `AttrTypeSize(type)` bytes per corner.
    };

    std::vector<Vector3f> positions;          ///< Per corner, in loop order.
    std::vector<u32> vertices;                ///< Each corner's vertex, at capture.
    /// The triangles the polygon was drawn as at capture (`TriangulateFace`),
    /// corner ordinals, three per triangle. Empty for an edge.
    std::vector<u32> cut;
    std::vector<std::vector<Influence>> skin; ///< Per corner; empty lists when unskinned.
    std::vector<Layer> layers;                ///< Every Halfedge layer of the mesh.

    u32 cornerCount() const {
        return static_cast<u32>(positions.size());
    }
};

/// Face @p face of @p mesh as it is now, from `halfedge(face)` round its loop,
/// cut as it is drawn (its stored row when valid). Needs connectivity; empty for
/// a deleted or out-of-range face.
SourcePolygon CapturePolygon(const Mesh& mesh, FaceId face);

/// The corners of @p corners (each a halfedge, the corner at its `from`), in the
/// order given, cut by the automatic rule when there are three or more.
/// `CapturePolygon` is this over a face's loop.
SourcePolygon CaptureCorners(const Mesh& mesh, std::span<const HalfedgeId> corners);

/**
 * @brief Weights of @p point against @p polygon, in the polygon's Newell plane:
 *        a partition of unity, linear-precise, defined outside the polygon too.
 *
 * - On a corner (within 1e-6 of the longest side): that corner, weight 1.
 * - On an edge: the lerp of its two corners. A two-corner polygon is an edge,
 *   and a point off it is placed by its projection onto the edge's line.
 * - Inside the polygon: barycentric in the triangle of @p cut (corner ordinals,
 *   three per triangle, the face as drawn) that holds the point, so the value is
 *   the one the picture draws.
 * - Outside it (an extrapolation): mean-value weights, with tan(a/2) computed as
 *   A / (|r||r'| + D), singular only on an edge itself, which the case above
 *   takes, never on an edge's line beyond it or at coincident corners.
 *
 * A polygon with no Newell normal takes its nearest corner. @p out holds at
 * least one weight per corner; the rest are zeroed.
 *
 * @return true when the point lies in the polygon (a corner, an edge or inside
 *         the cut), false when the weights extrapolate.
 */
bool SampleWeights(std::span<const Vector3f> polygon, std::span<const u32> cut,
                   const Vector3f& point, std::span<f32> out);

/**
 * @brief Every Halfedge layer of @p source's corners blended with @p weights,
 *        written, byte for byte the same, to every corner in @p targets.
 *
 * - F32 kinds: blended; `normal` and `binormal` renormalised; `tangent`'s xyz
 *   renormalised, its `w` the heaviest corner's.
 * - U8x4: blended in float, clamped to [0, 255], rounded.
 * - Integer and Bool: the heaviest corner's.
 * - Quat: nlerp, each corner's sign aligned with the heaviest's.
 *
 * A layer the mesh has and @p source does not is left alone.
 */
void BlendCorners(Mesh& mesh, const SourcePolygon& source, std::span<const f32> weights,
                  std::span<const HalfedgeId> targets);

/**
 * @brief Influences blended per bone.
 *
 * Negative weights (mean-value weights go negative outside a polygon and inside
 * a concave one) are clamped to 0 and the rest renormalised; then bones are
 * summed, sorted heaviest first, pruned under `kInfluencePrune`, cut to at most
 * `kMaxBlendedInfluences` (WEM's widest import and the classic group cap), and
 * renormalised last, so the result always sums to 1. The export's quantisers
 * narrow further (`writtenSkin`). Empty when every source is.
 */
std::vector<Influence> BlendInfluences(std::span<const std::vector<Influence>> sources,
                                       std::span<const f32> weights);

/// `BlendInfluences` over @p source's corners.
std::vector<Influence> BlendInfluences(const SourcePolygon& source, std::span<const f32> weights);

inline constexpr f32 kInfluencePrune = 1e-4f;
inline constexpr u32 kMaxBlendedInfluences = 8;

/**
 * @brief Every Vertex layer but `position` and `mergeGroup` blended from
 *        @p vertices with @p weights into @p target, by `BlendCorners`' rules,
 *        and the skin through `BlendInfluences`.
 *
 * `position` is the caller's to place, and `mergeGroup` its to choose: a new
 * vertex is its own point unless the caller says otherwise.
 */
void BlendVertex(Mesh& mesh, std::span<const u32> vertices, std::span<const f32> weights,
                 u32 target);

/// One more than the highest `mergeGroup` the mesh holds, or its vertex count
/// when that is larger: a group no vertex is in.
u32 FreshMergeGroup(const Mesh& mesh);

/// The mesh's `mergeGroup` layer, made if it has none. A mesh without one has
/// every vertex its own point, so a new layer starts each vertex in a group of
/// its own rather than all of them in group 0, which would weld them into one.
std::span<u32> MergeGroupsOf(Mesh& mesh);

} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
