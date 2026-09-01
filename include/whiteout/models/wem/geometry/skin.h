// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file skin.h
 * @brief Vertex skinning (WEM v3, design §5.6).
 *
 * Variable width, global bone indices, explicit rigid case. Each of the four
 * rules fixes something a fixed-width palette-local binding gets wrong:
 *
 * - **Indices are global** node indices into `Model::nodes`, and must land on
 *   `Bone` nodes. M2 and M3 store region-local palettes and D3 stores a rigid
 *   `nBoneIndex`; the *converters* build and consume palettes. WEM never holds
 *   one, because a palette is an export detail and holding it makes every edit a
 *   repalettisation.
 * - **Width is a profile limit, not a storage limit.** Import keeps what the
 *   file had — D3's three influences stay three, not three-padded-to-four.
 * - **Rigid sections skip the array** (`MeshSection::rigidNode`): only 378 of
 *   4,372 D3 sub-objects are skinned at all, so the alternative is expanding
 *   ~91% of D3 geometry into weight arrays that say the same thing four times.
 * - Weights are **not** normalised on import. A file that ships weights summing
 *   to 0.998 keeps them; `Validate` reports it, `Normalize` fixes it if asked.
 */

#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/compatibility.h>

#include "ids.h"

namespace whiteout {
namespace models {
namespace wem {
namespace geom {

struct Influence {
    u32 bone = 0; ///< Global index into `Model::nodes`.
    f32 weight = 0;

    template <class V>
    void reflect(V& v) {
        v.field("bone", bone);
        v.field("weight", weight);
    }
};

/**
 * @brief Per-vertex influences in CSR form.
 *
 * `offsets` is `vertexCount + 1` entries; vertex `v` owns
 * `influences[offsets[v] .. offsets[v + 1])`, sorted by descending weight.
 * An empty binding (`offsets.empty()`) means the mesh is not skinned at all,
 * which is different from every vertex having zero influences.
 */
/// @bind methods
struct SkinBinding {
    std::vector<u32> offsets;
    std::vector<Influence> influences;

    template <class V>
    void reflect(V& v) {
        v.field("offsets", offsets);
        v.field("influences", influences);
    }

    bool empty() const {
        return offsets.empty();
    }
    u32 vertexCount() const {
        return offsets.empty() ? 0 : static_cast<u32>(offsets.size() - 1);
    }

    std::span<const Influence> forVertex(u32 vertex) const;
    std::span<Influence> forVertex(u32 vertex);

    /// Widest influence count over all vertices — what an exporter compares
    /// against `ProfileDesc::maxBoneInfluences`.
    u32 maxInfluences() const;

    /// Starts an empty binding for @p vertexCount vertices, all unskinned.
    void reset(u32 vertexCount);

    /// Appends one vertex's influences at the end. Valid only while building in
    /// vertex order, which is how importers and `VertexSplit` both work.
    void appendVertex(std::span<const Influence> values);

    /// Appends a vertex whose influences copy @p source's — what a
    /// `VertexSplit` needs, since a split always creates its vertex at the end.
    void appendCopyOf(u32 source);

    /// Rebuilds through a `GarbageCollection` table; `remap[old] == kInvalidId`
    /// drops the vertex.
    void remapVertices(std::span<const u32> remap, u32 newCount);

    /// Scales each vertex's weights to sum to 1. Vertices with no influence, or
    /// with a total of zero, are left alone.
    void normalize();

    /// True when every skinned vertex's weights sum to 1 within @p tolerance.
    bool isNormalized(f32 tolerance = 1e-4f) const;

    /// Sorts each vertex's influences by descending weight — the documented
    /// order, which import must establish and edits must preserve.
    void sortByWeight();
};

} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
