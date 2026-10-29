// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/// A selection of one mesh's elements: what the selection walkers (`select.h`)
/// and the shading tools (`shading.h`) both take.

#include <vector>

#include <whiteout/common_types.h>

namespace whiteout {
namespace models {
namespace wem {
namespace geom {

/// Elements of one mesh, each list sorted and without repeats. A Border-level
/// selection is its border edges, in `edges`.
struct ElementSet {
    std::vector<u32> vertices;
    std::vector<u32> edges;
    std::vector<u32> faces;

    bool empty() const {
        return vertices.empty() && edges.empty() && faces.empty();
    }

    /// Sorts each list and drops repeats.
    void normalise();
};

} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
