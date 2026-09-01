// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file builder.h
 * @brief `MeshBuilder` — the importer-facing API (WEM v3, design §12).
 *
 * > `MeshBuilder` matters more than it looks: every converter goes through it,
 * > so the structural and manifold invariants (§5.7) are established by
 * > construction and no converter can produce a `Topology` that violates the
 * > §5.10 contract — `addFace` is where the repair happens, and it never fails.
 *
 * The one place the implementation differs from the design's sketch: corner
 * attributes are addressed by `(face, corner ordinal)` rather than by
 * `HalfedgeId`, because halfedge handles do not exist until `build()` runs the
 * connectivity. The builder records corner values in the order corners are added
 * and maps them onto halfedges at build time, which is also what keeps the §5.8
 * identity property true — a corner's attributes reach the halfedge that corner
 * became, repair or no repair.
 */

#include <string>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/compatibility.h>
#include <whiteout/vector_types.h>

#include "attributes.h"
#include "ids.h"
#include "mesh.h"
#include "repair.h"
#include "skin.h"

namespace whiteout {
namespace models {
namespace wem {
namespace geom {

class MeshBuilder {
public:
    MeshBuilder() = default;

    // --- vertices -------------------------------------------------------------

    VertexId addVertex(const Vector3f& position);

    /// Reserves @p count vertices at once; positions are set afterwards through
    /// `setVertexAttr` or `positions()`.
    void reserveVertices(u32 count);

    template <class T>
    void setVertexAttr(VertexId vertex, const std::string& name, const T& value) {
        setAttr(Domain::Vertex, vertex.value(), name, &value, sizeof(T));
    }

    /// Direct access to the position array, for a bulk de-interleave.
    std::span<Vector3f> positions();

    // --- faces ----------------------------------------------------------------

    /**
     * @brief Adds a face. Never fails.
     *
     * Degenerate faces are recorded and dropped; everything else is accepted as
     * given and made manifold at `build()`. The returned handle addresses the
     * face for corner attributes even when the repair later drops it — a
     * dropped face's corner values simply go nowhere.
     */
    FaceId addFace(std::span<const VertexId> corners, u32 section = 0);
    FaceId addTriangle(VertexId a, VertexId b, VertexId c, u32 section = 0);

    template <class T>
    void setCornerAttr(FaceId face, u32 corner, const std::string& name, const T& value) {
        const u32 index = cornerIndex(face, corner);
        if (index != kInvalidId) {
            setAttr(Domain::Halfedge, index, name, &value, sizeof(T));
        }
    }

    /// The corner's position in the builder's global corner order — the number a
    /// caller can hold onto instead of re-deriving `(face, corner)`.
    u32 cornerIndex(FaceId face, u32 corner) const;

    // --- skinning --------------------------------------------------------------

    /// Influences accumulate per vertex in call order and are sorted by
    /// descending weight at `build()`. Weights are kept as given (§5.6).
    void addInfluence(VertexId vertex, u32 bone, f32 weight);

    // --- sections ---------------------------------------------------------------

    /// Adds a section and returns its index, for use as `addFace`'s @p section.
    u32 addSection(MeshSection section);
    std::vector<MeshSection>& sections() {
        return sections_;
    }

    // --- build -------------------------------------------------------------------

    struct BuildOutcome {
        Mesh mesh;
        RepairLog::Stats repairStats;
        /// Set when @p policy was `Refuse` and the input needed repair. The mesh
        /// is then empty.
        bool refused = false;
    };

    /**
     * @brief Produces the mesh.
     *
     * Runs the repair, builds the connectivity, distributes corner attributes
     * onto halfedges and writes the `mergeGroup` layer. The builder is left
     * empty afterwards.
     */
    BuildOutcome build(RepairPolicy policy = RepairPolicy::RepairAndRecord);

    // --- introspection, mostly for tests ------------------------------------------

    u32 vertexCount() const {
        return static_cast<u32>(positions_.size());
    }
    u32 faceCount() const {
        return static_cast<u32>(faceValence_.size());
    }
    u32 cornerCount() const {
        return static_cast<u32>(cornerVertex_.size());
    }

private:
    void setAttr(Domain domain, u32 element, const std::string& name, const void* value,
                 std::size_t size);
    AttrLayer& layerFor(Domain domain, const std::string& name, std::size_t size);

    std::vector<Vector3f> positions_;
    std::vector<u32> faceValence_;
    std::vector<u32> faceCornerBase_;
    std::vector<u32> cornerVertex_;
    std::vector<u32> faceSection_;
    std::vector<MeshSection> sections_;
    std::vector<std::vector<Influence>> influences_;

    /// Pending attribute values, keyed by domain and name. Vertex layers are
    /// indexed by vertex; Halfedge layers by *corner*, which build() translates.
    AttributeSet pending_;
};

} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
