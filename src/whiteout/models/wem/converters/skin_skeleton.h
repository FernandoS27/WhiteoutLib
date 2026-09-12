// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file skin_skeleton.h
 * @brief The skeleton an exporter's render view folds surplus influences over.
 *
 * `geom::RenderMeshDesc` views a parent and a bind-space pivot per node; this
 * owns the two arrays for one model, so it must outlive every build that reads
 * them. See `geom::FoldInfluences`.
 */

#include <vector>

#include <whiteout/models/wem/geometry/render_view.h>
#include <whiteout/models/wem/nodes/tree.h>

namespace whiteout {
namespace models {
namespace wem {

struct SkinSkeleton {
    std::vector<u32> parents;
    std::vector<Vector3f> pivots;

    explicit SkinSkeleton(const NodeTree& tree) {
        parents.reserve(tree.size());
        pivots.reserve(tree.size());
        for (u32 n = 0; n < tree.size(); ++n) {
            parents.push_back(tree.nodes[n].parent);
            // Where the bind frame puts the bone's origin, in mesh space: the
            // translation row of the bind matrix (row vectors).
            const Matrix44f bind = Matrix44f::inverse(tree.inverseBindMatrix(n));
            pivots.push_back(Vector3f{bind.data[3][0], bind.data[3][1], bind.data[3][2]});
        }
    }

    void describe(geom::RenderMeshDesc& desc) const {
        desc.skinParents = parents;
        desc.skinPivots = pivots;
    }
};

} // namespace wem
} // namespace models
} // namespace whiteout
