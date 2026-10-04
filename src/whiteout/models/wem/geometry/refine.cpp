// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/geometry/refine.h>

#include <whiteout/models/wem/geometry/attributes.h>
#include <whiteout/models/wem/geometry/interpolate.h>
#include <whiteout/models/wem/geometry/ops.h>

#include "rebuild.h"

#include <algorithm>
#include <numeric>
#include <unordered_map>
#include <utility>
#include <vector>

namespace whiteout {
namespace models {
namespace wem {
namespace geom {

namespace {

u64 EdgeKey(u32 a, u32 b) {
    return (static_cast<u64>(std::min(a, b)) << 32) | std::max(a, b);
}

std::vector<u32> SortedUnique(std::vector<u32> values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    return values;
}

bool LiveFace(const Topology& topology, u32 face) {
    return face < topology.faceCount() && !topology.isDeleted(FaceId(face));
}

/// @p face's corner at @p vertex: the halfedge of its loop leaving it.
HalfedgeId CornerAt(const Topology& topology, FaceId face, u32 vertex) {
    for (const HalfedgeId h : topology.fh(face)) {
        if (topology.from(h).value() == vertex) {
            return h;
        }
    }
    return HalfedgeId();
}

bool Sharp(const Mesh& mesh, EdgeId edge) {
    const std::span<const u8> flags = mesh.attributes.get<u8>(names::kSharp, Domain::Edge);
    return edge.index() < flags.size() && flags[edge.index()] != 0;
}

/// Every live face, or the live ones of @p faces.
std::vector<u32> ChosenFaces(const Mesh& mesh, const ElementSet& faces) {
    const Topology& topology = mesh.topology();
    std::vector<u32> out;
    if (faces.faces.empty()) {
        for (u32 f = 0; f < topology.faceCount(); ++f) {
            if (LiveFace(topology, f)) {
                out.push_back(f);
            }
        }
        return out;
    }
    for (const u32 f : SortedUnique(faces.faces)) {
        if (LiveFace(topology, f)) {
            out.push_back(f);
        }
    }
    return out;
}

/// One level of `PlanSubdivide` over @p chosen; the faces it made, or none
/// when a cut refused.
std::vector<u32> SubdivideOnce(Mesh& mesh, const std::vector<u32>& chosen, const SubdivideParams& params,
                               std::vector<u32>& neighbours) {
    const Topology& topology = std::as_const(mesh).topology();
    const std::vector<Vector3f> before = [&] {
        const std::span<const Vector3f> p = std::as_const(mesh).attributes.get<Vector3f>(names::kPosition, Domain::Vertex);
        return std::vector<Vector3f>(p.begin(), p.end());
    }();
    std::vector<u8> inSet(topology.faceCount(), 0);
    for (const u32 f : chosen) {
        inSet[f] = 1;
    }
    struct FaceRecord {
        u32 face = 0;
        std::vector<u32> corners;
        SourcePolygon source;
        Vector3f centre{0, 0, 0};
    };
    std::vector<FaceRecord> records;
    std::vector<Vector3f> facePoint(topology.faceCount(), Vector3f{0, 0, 0});
    for (const u32 f : chosen) {
        FaceRecord record;
        record.face = f;
        for (const VertexId v : topology.fv(FaceId(f))) {
            record.corners.push_back(v.value());
            record.centre = record.centre + before[v.index()];
        }
        record.centre = record.centre * (1.0f / static_cast<f32>(record.corners.size()));
        record.source = CapturePolygon(mesh, FaceId(f));
        facePoint[f] = record.centre;
        records.push_back(std::move(record));
    }
    const bool smooth = params.scheme == SubdivideScheme::CatmullClark;
    // The edges the set's faces have, each with where its new point goes.
    struct EdgeRecord {
        u32 a = 0;
        u32 b = 0;
        Vector3f point{0, 0, 0};
    };
    std::vector<EdgeRecord> edges;
    std::unordered_map<u64, u32> edgeOf; // looked up, never iterated
    for (const FaceRecord& record : records) {
        const u32 n = static_cast<u32>(record.corners.size());
        for (u32 i = 0; i < n; ++i) {
            const u32 a = record.corners[i];
            const u32 b = record.corners[(i + 1) % n];
            if (!edgeOf.emplace(EdgeKey(a, b), static_cast<u32>(edges.size())).second) {
                continue;
            }
            const HalfedgeId h = topology.findHalfedge(VertexId(a), VertexId(b));
            const EdgeId e = Topology::edge(h);
            const FaceId f0 = topology.face(Topology::halfedge(e, 0));
            const FaceId f1 = topology.face(Topology::halfedge(e, 1));
            const bool interior = f0.valid() && f1.valid() && inSet[f0.index()] && inSet[f1.index()];
            const bool crease = !interior || (params.creases && Sharp(mesh, e));
            EdgeRecord edge{a, b, (before[a] + before[b]) * 0.5f};
            if (smooth && !crease) {
                edge.point = (before[a] + before[b] + facePoint[f0.index()] + facePoint[f1.index()]) * 0.25f;
            }
            edges.push_back(edge);
        }
    }
    // Where each old vertex of the set goes (Catmull-Clark only).
    std::vector<std::pair<u32, Vector3f>> moved;
    if (smooth) {
        std::vector<u8> seen(topology.vertexCount(), 0);
        for (const FaceRecord& record : records) {
            for (const u32 v : record.corners) {
                if (seen[v]) {
                    continue;
                }
                seen[v] = 1;
                bool inside = true;
                u32 valence = 0;
                u32 faces = 0;
                Vector3f q{0, 0, 0};
                Vector3f r{0, 0, 0};
                std::vector<u32> creases;
                for (const HalfedgeId h : topology.voh(VertexId(v))) {
                    const u32 w = topology.to(h).value();
                    const EdgeId e = Topology::edge(h);
                    ++valence;
                    r = r + (before[v] + before[w]) * 0.5f;
                    const bool border = topology.isBoundary(e);
                    if (border || (params.creases && Sharp(mesh, e))) {
                        creases.push_back(w);
                    }
                    if (const FaceId f = topology.face(h); f.valid()) {
                        inside = inside && inSet[f.index()];
                        q = q + facePoint[f.index()];
                        ++faces;
                    }
                }
                // A vertex beside a face that was not chosen holds that face's shape.
                if (!inside || valence == 0 || creases.size() >= 3) {
                    continue;
                }
                if (creases.size() == 2) {
                    moved.emplace_back(v, before[v] * 0.75f + (before[creases[0]] + before[creases[1]]) * 0.125f);
                    continue;
                }
                const f32 n = static_cast<f32>(valence);
                q = q * (1.0f / static_cast<f32>(std::max(faces, 1u)));
                r = r * (1.0f / n);
                moved.emplace_back(v, (q + r * 2.0f + before[v] * (n - 3.0f)) * (1.0f / n));
            }
        }
    }
    // The topology: every edge split, then each face's midpoints joined to a
    // point at its centre.
    std::unordered_map<u64, u32> midpointOf;
    for (const EdgeRecord& edge : edges) {
        const HalfedgeId h = topology.findHalfedge(VertexId(edge.a), VertexId(edge.b));
        const VertexId made = h.valid() ? SplitEdge(mesh, Topology::edge(h), 0.5f) : VertexId();
        if (!made.valid()) {
            return {};
        }
        midpointOf.emplace(EdgeKey(edge.a, edge.b), made.value());
        for (const FaceId f : topology.vf(made)) {
            if (f.valid() && !inSet[f.index()]) {
                neighbours.push_back(f.value());
            }
        }
    }
    std::vector<u32> made;
    std::vector<std::pair<u32, Vector3f>> centres;
    for (const FaceRecord& record : records) {
        const u32 n = static_cast<u32>(record.corners.size());
        std::vector<u32> mids(n);
        for (u32 i = 0; i < n; ++i) {
            mids[i] = midpointOf[EdgeKey(record.corners[i], record.corners[(i + 1) % n])];
        }
        const u32 across = n >= 4 ? n / 2 : 1;
        const FaceId face(record.face);
        if (!SplitFaceAt(mesh, CornerAt(topology, face, mids[0]), CornerAt(topology, face, mids[across])).valid()) {
            return {};
        }
        const HalfedgeId diagonal = topology.findHalfedge(VertexId(mids[0]), VertexId(mids[across]));
        const VertexId centre = diagonal.valid() ? SplitEdge(mesh, Topology::edge(diagonal), 0.5f) : VertexId();
        if (!centre.valid()) {
            return {};
        }
        for (u32 i = 1; i < n; ++i) {
            if (i == across) {
                continue;
            }
            std::vector<FaceId> around;
            for (const FaceId f : topology.vf(centre)) {
                if (f.valid()) {
                    around.push_back(f);
                }
            }
            for (const FaceId f : around) {
                const HalfedgeId from = CornerAt(topology, f, centre.value());
                const HalfedgeId to = CornerAt(topology, f, mids[i]);
                if (from.valid() && to.valid()) {
                    SplitFaceAt(mesh, from, to);
                    break;
                }
            }
        }
        // The centre is the face's mean: its corners, its skin and its place.
        const std::vector<f32> weights(record.source.cornerCount(), 1.0f / static_cast<f32>(record.source.cornerCount()));
        std::vector<HalfedgeId> targets;
        for (const HalfedgeId h : topology.voh(centre)) {
            if (topology.face(h).valid()) {
                targets.push_back(h);
                made.push_back(topology.face(h).value());
            }
        }
        BlendCorners(mesh, record.source, weights, targets);
        const std::vector<f32> vertexWeights(n, 1.0f / static_cast<f32>(n));
        BlendVertex(mesh, record.corners, vertexWeights, centre.value());
        centres.emplace_back(centre.value(), record.centre);
    }
    const std::span<Vector3f> positions = mesh.attributes.get<Vector3f>(names::kPosition, Domain::Vertex);
    for (const auto& [v, at] : centres) {
        positions[v] = at;
    }
    for (const EdgeRecord& edge : edges) {
        positions[midpointOf[EdgeKey(edge.a, edge.b)]] = edge.point;
    }
    for (const auto& [v, at] : moved) {
        positions[v] = at;
    }
    return SortedUnique(std::move(made));
}

} // namespace

ModelPlan PlanSubdivide(Mesh& mesh, const ElementSet& faces, const SubdivideParams& params) {
    ModelPlan plan;
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        plan.refusal = ModelRefusal::NotBuiltYet;
        return plan;
    }
    std::vector<u32> chosen = ChosenFaces(mesh, faces);
    if (chosen.empty()) {
        plan.refusal = ModelRefusal::EmptySelection;
        return plan;
    }
    std::vector<u32> neighbours;
    for (u32 level = 0; level < std::max(1u, params.levels); ++level) {
        chosen = SubdivideOnce(mesh, chosen, params, neighbours);
        if (chosen.empty()) {
            plan.refusal = ModelRefusal::WouldFold;
            return plan;
        }
    }
    const Topology& topology = std::as_const(mesh).topology();
    for (const u32 f : chosen) {
        for (const HalfedgeId h : topology.fh(FaceId(f))) {
            plan.touchedEdges.push_back(Topology::edge(h).value());
        }
    }
    // A neighbour a later level split again was renumbered by nothing (faces
    // keep their slots), so the list stands; dead ones drop out.
    std::vector<u32> changed = chosen;
    for (const u32 f : neighbours) {
        if (LiveFace(topology, f)) {
            changed.push_back(f);
        }
    }
    plan.selection.faces = chosen;
    plan.changedFaces = SortedUnique(std::move(changed));
    plan.touchedEdges = SortedUnique(std::move(plan.touchedEdges));
    plan.changed = static_cast<u32>(chosen.size());
    return plan;
}

ModelPlan PlanSmooth(Mesh& mesh, const ElementSet& elements, const SmoothParams& params) {
    ModelPlan plan;
    plan.renumbers = false;
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        plan.refusal = ModelRefusal::NotBuiltYet;
        return plan;
    }
    const Topology& topology = std::as_const(mesh).topology();
    std::vector<u32> chosen = elements.vertices;
    if (chosen.empty()) {
        for (const u32 f : ChosenFaces(mesh, elements)) {
            for (const VertexId v : topology.fv(FaceId(f))) {
                chosen.push_back(v.value());
            }
        }
    }
    chosen = SortedUnique(std::move(chosen));
    std::vector<u32> moving;
    for (const u32 v : chosen) {
        if (v < topology.vertexCount() && !topology.isDeleted(VertexId(v)) &&
            !(params.pinBorders && topology.isBoundary(VertexId(v)))) {
            moving.push_back(v);
        }
    }
    if (moving.empty()) {
        plan.refusal = ModelRefusal::EmptySelection;
        return plan;
    }
    const std::span<Vector3f> positions = mesh.attributes.get<Vector3f>(names::kPosition, Domain::Vertex);
    const auto pass = [&](f32 weight) {
        std::vector<Vector3f> next(moving.size());
        for (std::size_t i = 0; i < moving.size(); ++i) {
            const u32 v = moving[i];
            Vector3f mean{0, 0, 0};
            u32 count = 0;
            for (const VertexId w : topology.vv(VertexId(v))) {
                mean = mean + positions[w.index()];
                ++count;
            }
            next[i] = count ? positions[v] + (mean * (1.0f / static_cast<f32>(count)) - positions[v]) * weight
                            : positions[v];
        }
        for (std::size_t i = 0; i < moving.size(); ++i) {
            positions[moving[i]] = next[i];
        }
    };
    const f32 factor = std::clamp(params.factor, 0.0f, 1.0f);
    // Taubin's inflate: mu = 1 / (kPB - 1 / lambda), kPB 0.1.
    const f32 inflate = -factor / (1.0f - 0.1f * factor);
    for (u32 i = 0; i < std::max(1u, params.iterations); ++i) {
        pass(factor);
        if (params.scheme == SmoothScheme::Taubin) {
            pass(inflate);
        }
    }
    for (const u32 v : moving) {
        for (const HalfedgeId h : topology.voh(VertexId(v))) {
            plan.touchedEdges.push_back(Topology::edge(h).value());
            if (const FaceId f = topology.face(h); f.valid()) {
                plan.changedFaces.push_back(f.value());
            }
        }
    }
    plan.touchedEdges = SortedUnique(std::move(plan.touchedEdges));
    plan.changedFaces = SortedUnique(std::move(plan.changedFaces));
    plan.selection.vertices = chosen;
    plan.changed = static_cast<u32>(moving.size());
    return plan;
}

ModelPlan PlanOrientOutward(Mesh& mesh, const ElementSet& faces) {
    ModelPlan plan;
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        plan.refusal = ModelRefusal::NotBuiltYet;
        return plan;
    }
    // The face set's order is the faces' slots only with nothing lazily deleted.
    if (std::as_const(mesh).topology().hasDeleted()) {
        GarbageCollect(mesh);
    }
    const Topology& topology = std::as_const(mesh).topology();
    const u32 faceCount = topology.faceCount();
    const std::span<const Vector3f> positions =
        std::as_const(mesh).attributes.get<Vector3f>(names::kPosition, Domain::Vertex);
    // Parts: faces joined across an edge.
    std::vector<u32> parent(faceCount);
    std::iota(parent.begin(), parent.end(), 0u);
    const auto root = [&](u32 f) {
        while (parent[f] != f) {
            parent[f] = parent[parent[f]];
            f = parent[f];
        }
        return f;
    };
    for (u32 f = 0; f < faceCount; ++f) {
        for (const HalfedgeId h : topology.fh(FaceId(f))) {
            if (const FaceId other = topology.face(Topology::opposite(h)); other.valid()) {
                parent[root(f)] = root(other.value());
            }
        }
    }
    std::vector<u8> asked(faceCount, faces.faces.empty() ? 1 : 0);
    for (const u32 f : faces.faces) {
        if (f < faceCount) {
            asked[root(f)] = 1;
        }
    }
    // Each part's volume about its own centre.
    std::vector<Vector3f> centre(faceCount, Vector3f{0, 0, 0});
    std::vector<u32> corners(faceCount, 0);
    for (u32 f = 0; f < faceCount; ++f) {
        for (const VertexId v : topology.fv(FaceId(f))) {
            centre[root(f)] = centre[root(f)] + positions[v.index()];
            ++corners[root(f)];
        }
    }
    std::vector<f32> volume(faceCount, 0.0f);
    for (u32 f = 0; f < faceCount; ++f) {
        const u32 part = root(f);
        const Vector3f c = centre[part] * (1.0f / static_cast<f32>(std::max(corners[part], 1u)));
        std::vector<Vector3f> loop;
        for (const VertexId v : topology.fv(FaceId(f))) {
            loop.push_back(positions[v.index()] - c);
        }
        for (std::size_t i = 1; i + 1 < loop.size(); ++i) {
            volume[part] += loop[0].dot(cross(loop[i], loop[i + 1]));
        }
    }
    std::vector<u8> flip(faceCount, 0);
    for (u32 f = 0; f < faceCount; ++f) {
        const u32 part = root(f);
        if ((asked[part] || faces.faces.empty()) && volume[part] < 0.0f) {
            flip[f] = 1;
            plan.selection.faces.push_back(f);
            ++plan.changed;
        }
    }
    if (plan.changed == 0) {
        plan.renumbers = false;
        return plan;
    }
    // Reversed through a rebuild, as `UnifyWinding` reverses: each corner keeps
    // its vertex's values.
    const std::vector<u32> snapshot = detail::snapshotCorners(mesh);
    detail::RebuildMapping mapping = detail::identityMapping(mesh);
    u32 base = 0;
    for (u32 f = 0; f < mapping.faces.faceValence.size(); ++f) {
        const u32 valence = mapping.faces.faceValence[f];
        if (f < faceCount && flip[f]) {
            std::reverse(mapping.faces.cornerVertex.begin() + base, mapping.faces.cornerVertex.begin() + base + valence);
            std::reverse(mapping.cornerSource.begin() + base, mapping.cornerSource.begin() + base + valence);
        }
        base += valence;
    }
    detail::rebuild(mesh, std::move(mapping), snapshot);
    // Corner normals turn with their faces.
    const std::span<Vector3f> normals = mesh.attributes.get<Vector3f>(names::kNormal, Domain::Halfedge);
    const Topology& rebuilt = std::as_const(mesh).topology();
    for (const u32 f : plan.selection.faces) {
        if (f >= rebuilt.faceCount()) {
            continue;
        }
        for (const HalfedgeId h : rebuilt.fh(FaceId(f))) {
            if (h.index() < normals.size()) {
                normals[h.index()] = normals[h.index()] * -1.0f;
            }
        }
    }
    for (u32 f = 0; f < rebuilt.faceCount(); ++f) {
        plan.changedFaces.push_back(f);
    }
    return plan;
}

} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
