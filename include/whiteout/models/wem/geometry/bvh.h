// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file bvh.h
 * @brief A bounding-volume hierarchy over triangles
 *        (EDIT_MODE_SKIN_DESIGN.md §3.6).
 *
 * Hover, strokes, the visibility test and weight transfer query a mesh thousands
 * of times a second, so the posed triangles sit in a tree: a median split over
 * an AABB per node, with a ray query and a closest-point query.
 *
 * It is **refit**, not rebuilt, when a pose or a weight moves the vertices —
 * the topology has not changed, only where it is — and rebuilt when the set of
 * triangles does.
 *
 * It is new rather than borrowed: `snowball::TreeMesh` is quantised and belongs
 * to the physics module, and cornflakes' `MeshBvhNode` belongs to the effects
 * runtime. The skin tools must not depend on either.
 */

#include <limits>
#include <span>
#include <vector>

#include <whiteout/common_types.h>

#include "../bounds.h"
#include "ids.h"

namespace whiteout {
namespace models {
namespace wem {
namespace geom {

/// What a ray found: the triangle, how far along the ray, and where on it.
struct RayHit {
    u32 triangle = kInvalidId;
    f32 distance = 0.0f;
    f32 u = 0.0f; ///< Barycentric, on the second corner.
    f32 v = 0.0f; ///< Barycentric, on the third corner.

    bool hit() const {
        return triangle != kInvalidId;
    }
};

/// What a closest-point query found.
struct PointHit {
    u32 triangle = kInvalidId;
    Vector3f position{0, 0, 0};
    f32 distance = 0.0f;
    f32 u = 0.0f;
    f32 v = 0.0f;

    bool hit() const {
        return triangle != kInvalidId;
    }
};

/**
 * @brief A median-split AABB tree over a triangle list.
 *
 * `build` takes the indices and the positions; `refit` takes new positions for
 * the same indices. Every query is against the positions the last of those two
 * was given.
 */
class TriangleBvh {
public:
    /// @p indices is three per triangle into @p positions.
    void build(std::span<const u32> indices, std::span<const Vector3f> positions);

    /// The same triangles at new positions. Cheap: the tree's shape is kept and
    /// only its boxes are recomputed.
    void refit(std::span<const Vector3f> positions);

    bool empty() const {
        return nodes_.empty();
    }
    u32 triangleCount() const {
        return static_cast<u32>(indices_.size() / 3);
    }
    /// The whole tree's box; an empty extent when there is nothing in it.
    Extent bounds() const;

    /// The nearest triangle @p origin + t * @p direction meets, for
    /// `t` in `[tMin, tMax]`. @p direction need not be normalised; `distance` is
    /// in units of it.
    RayHit raycast(const Vector3f& origin, const Vector3f& direction, f32 tMin = 0.0f,
                   f32 tMax = std::numeric_limits<f32>::max()) const;

    /// The point on the nearest triangle to @p from, within @p maxDistance.
    PointHit closestPoint(const Vector3f& from,
                          f32 maxDistance = std::numeric_limits<f32>::max()) const;

private:
    struct Node {
        Vector3f low{0, 0, 0};
        Vector3f high{0, 0, 0};
        u32 first = 0; ///< Into `order_`, for a leaf.
        u32 count = 0; ///< 0 for an inner node.
        u32 right = 0; ///< The second child, for an inner node; left is `this + 1`.
    };

    u32 buildRange(u32 begin, u32 end, std::span<const Vector3f> positions);
    void refitNode(u32 node, std::span<const Vector3f> positions);

    std::vector<u32> indices_;
    std::vector<Vector3f> positions_;
    std::vector<u32> order_; ///< Triangles, leaf by leaf.
    std::vector<Node> nodes_;
};

} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
