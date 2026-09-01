// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/geometry/checks.h>

#include <algorithm>
#include <string>

namespace whiteout {
namespace models {
namespace wem {
namespace geom {

namespace {

std::string number(u64 value) {
    return std::to_string(value);
}

} // namespace

void CheckStructural(const Mesh& mesh, u32 meshIndex, Diagnostics& out) {
    const ElementRef meshRef(ElementKind::Mesh, meshIndex);

    // --- C9: layer sizes agree with their domain ------------------------------
    for (const AttrLayer& layer : mesh.attributes.layers()) {
        const u32 expected = mesh.attributes.domainCount(layer.domain);
        if (layer.count() != expected) {
            out.error(DiagCode::AttributeCountMismatch,
                      "layer '" + layer.name + "' on the " + ToString(layer.domain) +
                          " domain holds " + number(layer.count()) + " elements, not " +
                          number(expected),
                      meshRef);
        }
    }

    const auto positions = mesh.attributes.get<const Vector3f>(names::kPosition, Domain::Vertex);
    if (positions.size() != mesh.vertexCount()) {
        out.error(DiagCode::AttributeCountMismatch,
                  "the required 'position' layer is absent or mis-sized", meshRef);
    }

    // --- section values are in range ------------------------------------------
    const auto sectionOf = mesh.faceSections();
    for (u32 f = 0; f < sectionOf.size(); ++f) {
        if (sectionOf[f] >= mesh.sections.size()) {
            out.error(DiagCode::IndexOutOfRange,
                      "face names section " + number(sectionOf[f]) + " of " +
                          number(mesh.sections.size()),
                      ElementRef(ElementKind::Face, f));
        }
    }

    // --- the skin binding's CSR shape ------------------------------------------
    if (!mesh.skin.empty()) {
        if (mesh.skin.offsets.size() != static_cast<std::size_t>(mesh.vertexCount()) + 1) {
            out.error(DiagCode::SkinBindingMalformed,
                      "offsets holds " + number(mesh.skin.offsets.size()) + " entries, not " +
                          number(static_cast<std::size_t>(mesh.vertexCount()) + 1),
                      meshRef);
        } else {
            for (std::size_t v = 0; v + 1 < mesh.skin.offsets.size(); ++v) {
                if (mesh.skin.offsets[v] > mesh.skin.offsets[v + 1]) {
                    out.error(DiagCode::SkinBindingMalformed,
                              "offsets are not monotonic at vertex " + number(v),
                              ElementRef(ElementKind::Vertex, static_cast<u32>(v)));
                    break;
                }
            }
            if (mesh.skin.offsets.back() != mesh.skin.influences.size()) {
                out.error(DiagCode::SkinBindingMalformed,
                          "the final offset does not equal the influence count", meshRef);
            }
        }
        if (!mesh.skin.isNormalized()) {
            // Not an error: a file that ships weights summing to 0.998 keeps them
            // (§5.6). The caller decides whether to `normalize()`.
            out.info(DiagCode::SkinBindingMalformed, "some vertices' weights do not sum to 1",
                     meshRef);
        }
    }

    if (!mesh.hasConnectivity()) {
        return;
    }
    const Topology& topology = mesh.topology();

    // --- C1, C2, C7 --------------------------------------------------------------
    for (u32 raw = 0; raw < topology.halfedgeCount(); ++raw) {
        const HalfedgeId h(raw);
        if (topology.isDeleted(h)) {
            continue;
        }
        const ElementRef ref(ElementKind::Halfedge, raw);
        if (Topology::opposite(Topology::opposite(h)) != h) {
            out.error(DiagCode::ConnectivityCorrupt, "opposite(opposite(h)) != h", ref);
        }
        const HalfedgeId nextHalfedge = topology.next(h);
        if (!nextHalfedge.valid() || nextHalfedge.value() >= topology.halfedgeCount()) {
            out.error(DiagCode::ConnectivityCorrupt, "next is out of range", ref);
            continue;
        }
        if (topology.prev(nextHalfedge) != h) {
            out.error(DiagCode::ConnectivityCorrupt, "prev(next(h)) != h", ref);
        }
        if (!topology.to(h).valid() || topology.to(h).value() >= topology.vertexCount()) {
            out.error(DiagCode::ConnectivityCorrupt, "to is out of range", ref);
            continue;
        }
        if (topology.from(nextHalfedge) != topology.to(h)) {
            out.error(DiagCode::ConnectivityCorrupt, "the next chain does not join end to end",
                      ref);
        }
        if (topology.face(h) != topology.face(nextHalfedge)) {
            out.error(DiagCode::ConnectivityCorrupt, "the face disagrees across the loop", ref);
        }
    }

    for (u32 raw = 0; raw < topology.vertexCount(); ++raw) {
        const VertexId v(raw);
        if (topology.isDeleted(v)) {
            continue;
        }
        const HalfedgeId h = topology.outgoing(v);
        if (h.valid() && topology.from(h) != v) {
            out.error(DiagCode::ConnectivityCorrupt, "the vertex's outgoing halfedge is not its",
                      ElementRef(ElementKind::Vertex, raw));
        }
    }

    for (u32 raw = 0; raw < topology.faceCount(); ++raw) {
        const FaceId f(raw);
        if (topology.isDeleted(f)) {
            continue;
        }
        const HalfedgeId h = topology.halfedge(f);
        if (!h.valid() || topology.face(h) != f) {
            out.error(DiagCode::ConnectivityCorrupt, "the face's halfedge is not its",
                      ElementRef(ElementKind::Face, raw));
        }
    }
}

void CheckManifold(const Mesh& mesh, u32 meshIndex, Diagnostics& out) {
    if (!mesh.hasConnectivity()) {
        return;
    }
    const Topology& topology = mesh.topology();
    const ElementRef meshRef(ElementKind::Mesh, meshIndex);

    // --- C4: every edge has at most two incident faces -------------------------
    //
    // Guaranteed by the pair allocation itself, so the check that can actually
    // fail is the one that says both halfedges of an edge exist and are distinct.
    for (u32 e = 0; e < topology.edgeCount(); ++e) {
        const EdgeId edge(e);
        if (topology.isDeleted(edge)) {
            continue;
        }
        const HalfedgeId h0 = Topology::halfedge(edge, 0);
        const HalfedgeId h1 = Topology::halfedge(edge, 1);
        if (topology.face(h0).valid() && topology.face(h0) == topology.face(h1)) {
            out.error(DiagCode::NonManifoldEdgeSplit,
                      "both sides of the edge belong to the same face",
                      ElementRef(ElementKind::Edge, e));
        }
    }

    // --- C5: each vertex's outgoing halfedges form one cycle --------------------
    std::vector<u32> degree(topology.vertexCount(), 0);
    for (u32 raw = 0; raw < topology.halfedgeCount(); ++raw) {
        const HalfedgeId h(raw);
        if (topology.isDeleted(h)) {
            continue;
        }
        const VertexId from = topology.from(h);
        if (from.valid() && from.value() < degree.size()) {
            ++degree[from.index()];
        }
    }
    for (u32 raw = 0; raw < topology.vertexCount(); ++raw) {
        const VertexId v(raw);
        if (topology.isDeleted(v) || !topology.outgoing(v).valid()) {
            continue;
        }
        u32 walked = 0;
        for (HalfedgeId h : topology.voh(v)) {
            (void)h;
            ++walked;
            if (walked > degree[raw]) {
                break;
            }
        }
        if (walked != degree[raw]) {
            out.error(DiagCode::NonManifoldVertexSplit,
                      "the vertex's fan is " + number(walked) + " of " + number(degree[raw]) +
                          " outgoing halfedges — more than one fan meets here",
                      ElementRef(ElementKind::Vertex, raw));
        }
    }

    // --- C6: no face repeats a vertex; no two faces share a vertex set ----------
    //
    // Faces are keyed by their sorted corner list, then the keys are sorted and
    // scanned — O(n log n), and the findings are re-ordered by face before they
    // are reported so the output still follows the mesh's own order.
    struct FaceKey {
        u64 hash;
        u32 face;
    };
    std::vector<FaceKey> keys;
    keys.reserve(topology.faceCount());
    std::vector<u32> corners;
    std::vector<u32> repeated;

    for (u32 raw = 0; raw < topology.faceCount(); ++raw) {
        const FaceId f(raw);
        if (topology.isDeleted(f) || !topology.halfedge(f).valid()) {
            continue;
        }
        corners.clear();
        for (VertexId v : topology.fv(f)) {
            corners.push_back(v.value());
        }
        std::sort(corners.begin(), corners.end());
        if (std::adjacent_find(corners.begin(), corners.end()) != corners.end()) {
            repeated.push_back(raw);
            continue;
        }
        u64 hash = 1469598103934665603ull;
        for (u32 v : corners) {
            hash ^= v;
            hash *= 1099511628211ull;
        }
        hash ^= static_cast<u64>(corners.size()) << 56;
        keys.push_back(FaceKey{hash, raw});
    }

    for (u32 face : repeated) {
        out.error(DiagCode::DegenerateFaceDropped, "the face repeats a vertex",
                  ElementRef(ElementKind::Face, face));
    }

    std::sort(keys.begin(), keys.end(), [](const FaceKey& a, const FaceKey& b) {
        return a.hash != b.hash ? a.hash < b.hash : a.face < b.face;
    });
    std::vector<std::pair<u32, u32>> duplicates; // (face, the face it duplicates)
    for (std::size_t i = 1; i < keys.size(); ++i) {
        if (keys[i].hash == keys[i - 1].hash) {
            duplicates.emplace_back(keys[i].face, keys[i - 1].face);
        }
    }
    std::sort(duplicates.begin(), duplicates.end());
    for (const auto& entry : duplicates) {
        out.error(DiagCode::DuplicateFaceDropped,
                  "shares its vertex set with face " + number(entry.second),
                  ElementRef(ElementKind::Face, entry.first));
    }

    (void)meshRef;
}

} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
