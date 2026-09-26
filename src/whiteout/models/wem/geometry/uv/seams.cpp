// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/geometry/uv/seams.h>

#include <whiteout/models/wem/geometry/modelling.h>
#include <whiteout/models/wem/rigging/limbs.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace whiteout {
namespace models {
namespace wem {
namespace geom {
namespace uv {

u32 MarkDelimitSeams(Mesh& mesh) {
    if (!mesh.hasConnectivity()) {
        return 0;
    }
    const Topology& topology = std::as_const(mesh).topology();
    const u32 edgeCount = topology.edgeCount();
    if (edgeCount == 0) {
        return 0;
    }
    const std::span<u8> seam =
        mesh.attributes.getOrCreate<u8>(names::kSeam, Domain::Edge, AttrType::Bool);
    u32 marked = 0;
    for (u32 e = 0; e < edgeCount && e < seam.size(); ++e) {
        const EdgeId edge(e);
        if (topology.isDeleted(edge) || topology.isBoundary(edge)) {
            // A border does not delimit anything: there is only one side of it.
            continue;
        }
        const u8 value = SeamBetween(mesh, Topology::halfedge(edge, 0)) ? 1 : 0;
        seam[e] = value;
        marked += value;
    }
    return marked;
}

u32 MarkDelimitSeams(Mesh& mesh, std::span<const u32> edges) {
    if (!mesh.hasConnectivity() || edges.empty()) {
        return 0;
    }
    std::vector<u32> marked;
    marked.reserve(edges.size());
    for (const u32 e : edges) {
        if (SeamBetween(mesh, Topology::halfedge(EdgeId(e), 0))) {
            marked.push_back(e);
        }
    }
    if (marked.empty()) {
        return 0;
    }
    const std::span<u8> seam =
        mesh.attributes.getOrCreate<u8>(names::kSeam, Domain::Edge, AttrType::Bool);
    for (const u32 e : marked) {
        if (e < seam.size()) {
            seam[e] = 1;
        }
    }
    return static_cast<u32>(marked.size());
}

void ApplyMarks(Mesh& mesh, u32 set, std::span<const EdgeId> edges, bool mark) {
    if (edges.empty()) {
        return;
    }
    const std::span<u8> marks =
        mesh.attributes.getOrCreate<u8>(names::uvSeam(set), Domain::Edge, AttrType::Bool);
    for (const EdgeId edge : edges) {
        if (edge.index() < marks.size()) {
            marks[edge.index()] = mark ? 1 : 0;
        }
    }
}

namespace {

/// The mean of an edge's two corner normals, or the faces' geometric normals
/// where the mesh authored none: which way the surface looks along that edge.
Vector3f edgeNormal(const Topology& topology, EdgeId edge,
                    std::span<const Vector3f> positions, std::span<const Vector3f> normals) {
    Vector3f sum{0.0f, 0.0f, 0.0f};
    for (u32 side = 0; side < 2; ++side) {
        const HalfedgeId h = Topology::halfedge(edge, side);
        const FaceId face = topology.face(h);
        if (!face.valid()) {
            continue;
        }
        if (!normals.empty() && h.index() < normals.size()) {
            sum = sum + normals[h.index()];
            continue;
        }
        const HalfedgeId next = topology.next(h);
        const HalfedgeId last = topology.next(next);
        sum = sum + cross(positions[topology.from(next).index()] -
                              positions[topology.from(h).index()],
                          positions[topology.from(last).index()] -
                              positions[topology.from(h).index()]);
    }
    const f32 length = sum.length();
    return length > 0.0f ? sum * (1.0f / length) : Vector3f{0.0f, 0.0f, 0.0f};
}

/// How dear this edge is to cut: its length, dearer the more it faces the
/// camera. A fully hidden edge costs its length and a fully visible one costs
/// `1 + hiddenWeight` times it.
f32 edgeCost(f32 length, const Vector3f& normal, const SeamPathOptions& options) {
    const f32 facing = normal.dot(options.visible);
    return length * (1.0f + options.hiddenWeight * std::clamp(0.5f + 0.5f * facing, 0.0f, 1.0f));
}

} // namespace

SeamPath FindSeamPath(const Mesh& mesh, const skinning::PointTable& points, EdgeId from,
                      EdgeId to, const SeamPathOptions& options) {
    SeamPath out;
    if (!mesh.hasConnectivity() || !from.valid() || !to.valid()) {
        out.stopped = true;
        return out;
    }
    const Topology& topology = mesh.topology();
    const std::span<const Vector3f> positions =
        mesh.attributes.get<const Vector3f>(names::kPosition, Domain::Vertex);
    const std::span<const Vector3f> normals =
        mesh.attributes.get<const Vector3f>(names::kNormal, Domain::Halfedge);
    if (positions.empty()) {
        out.stopped = true;
        return out;
    }

    // Over points, not vertices: a file that split a vertex for its normals
    // did not mean to split the surface, and a path must not stop there.
    const auto pointAt = [&](VertexId v) { return points.pointOfVertex(v.value()); };
    const u32 start = pointAt(topology.to(Topology::halfedge(from, 0)));
    const u32 goalA = pointAt(topology.from(Topology::halfedge(to, 0)));
    const u32 goalB = pointAt(topology.to(Topology::halfedge(to, 0)));
    const auto known = [&](u32 point) { return point < points.pointCount; };
    if (!known(start) || (!known(goalA) && !known(goalB))) {
        out.stopped = true;
        return out;
    }

    // Every edge of the mesh, by the pair of points it joins: the graph the
    // walk is over, and how a step is turned back into an edge to mark.
    struct Step {
        u32 to = kInvalidId;
        EdgeId edge;
        f32 cost = 0.0f;
    };
    std::vector<std::vector<Step>> graph(points.pointCount);
    for (u32 e = 0; e < topology.edgeCount(); ++e) {
        const EdgeId edge(e);
        if (topology.isDeleted(edge)) {
            continue;
        }
        const HalfedgeId h = Topology::halfedge(edge, 0);
        const u32 a = pointAt(topology.from(h));
        const u32 b = pointAt(topology.to(h));
        if (!known(a) || !known(b) || a == b) {
            continue;
        }
        if (topology.from(h).index() >= positions.size() ||
            topology.to(h).index() >= positions.size()) {
            continue;
        }
        const f32 length =
            (positions[topology.to(h).index()] - positions[topology.from(h).index()]).length();
        const f32 cost =
            edgeCost(length, edgeNormal(topology, edge, positions, normals), options);
        graph[a].push_back(Step{b, edge, cost});
        graph[b].push_back(Step{a, edge, cost});
    }

    std::vector<f32> best(points.pointCount, std::numeric_limits<f32>::max());
    std::vector<u32> cameFrom(points.pointCount, kInvalidId);
    std::vector<EdgeId> cameBy(points.pointCount);
    // A plain binary heap over (cost, point): the meshes this runs on are tens
    // of thousands of edges and a heap is the whole of what it needs.
    std::vector<std::pair<f32, u32>> queue;
    const auto cheaper = [](const std::pair<f32, u32>& a, const std::pair<f32, u32>& b) {
        return a.first != b.first ? a.first > b.first : a.second > b.second;
    };
    best[start] = 0.0f;
    queue.emplace_back(0.0f, start);
    u32 reached = kInvalidId;
    while (!queue.empty()) {
        std::pop_heap(queue.begin(), queue.end(), cheaper);
        const auto [cost, point] = queue.back();
        queue.pop_back();
        if (cost > best[point]) {
            continue;
        }
        if (point == goalA || point == goalB) {
            reached = point;
            break;
        }
        for (const Step& step : graph[point]) {
            const f32 next = cost + step.cost;
            if (next >= best[step.to]) {
                continue;
            }
            best[step.to] = next;
            cameFrom[step.to] = point;
            cameBy[step.to] = step.edge;
            queue.emplace_back(next, step.to);
            std::push_heap(queue.begin(), queue.end(), cheaper);
        }
    }
    if (reached == kInvalidId) {
        // Nothing joins the two: a border, or another shell entirely.
        out.stopped = true;
        return out;
    }
    for (u32 point = reached; cameFrom[point] != kInvalidId; point = cameFrom[point]) {
        out.edges.push_back(cameBy[point]);
    }
    std::reverse(out.edges.begin(), out.edges.end());
    return out;
}

namespace {

/// A region's edges as a graph over points, each step costed twice: its plain
/// length, which "farthest" is measured in, and its hidden-side cost, which the
/// cut takes.
struct RegionGraph {
    struct Step {
        u32 to = kInvalidId;
        EdgeId edge;
        f32 length = 0.0f;
        f32 cost = 0.0f;
    };
    std::vector<std::vector<Step>> steps;
    std::vector<u32> points; ///< The points the region reaches, ascending.
};

RegionGraph regionGraph(const Mesh& mesh, const skinning::PointTable& points,
                        std::span<const FaceId> faces, const SeamPathOptions& options) {
    RegionGraph out;
    const Topology& topology = mesh.topology();
    const std::span<const Vector3f> positions =
        mesh.attributes.get<const Vector3f>(names::kPosition, Domain::Vertex);
    const std::span<const Vector3f> normals =
        mesh.attributes.get<const Vector3f>(names::kNormal, Domain::Halfedge);
    out.steps.resize(points.pointCount);
    std::vector<u32> edges;
    for (const FaceId face : faces) {
        if (!face.valid() || face.index() >= topology.faceCount() || topology.isDeleted(face)) {
            continue;
        }
        for (const HalfedgeId h : topology.fh(face)) {
            edges.push_back(Topology::edge(h).value());
        }
    }
    std::sort(edges.begin(), edges.end());
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
    for (const u32 e : edges) {
        const EdgeId edge(e);
        const HalfedgeId h = Topology::halfedge(edge, 0);
        const u32 a = points.pointOfVertex(topology.from(h).value());
        const u32 b = points.pointOfVertex(topology.to(h).value());
        if (a >= points.pointCount || b >= points.pointCount || a == b ||
            topology.from(h).index() >= positions.size() ||
            topology.to(h).index() >= positions.size()) {
            continue;
        }
        const f32 length =
            (positions[topology.to(h).index()] - positions[topology.from(h).index()]).length();
        const f32 cost = edgeCost(length, edgeNormal(topology, edge, positions, normals), options);
        out.steps[a].push_back(RegionGraph::Step{b, edge, length, cost});
        out.steps[b].push_back(RegionGraph::Step{a, edge, length, cost});
        out.points.push_back(a);
        out.points.push_back(b);
    }
    std::sort(out.points.begin(), out.points.end());
    out.points.erase(std::unique(out.points.begin(), out.points.end()), out.points.end());
    return out;
}

/// Dijkstra from @p start over @p graph, by length or by cost.
void walkRegion(const RegionGraph& graph, u32 start, bool byCost, std::vector<f32>& best,
                std::vector<u32>& cameFrom, std::vector<EdgeId>& cameBy) {
    best.assign(graph.steps.size(), std::numeric_limits<f32>::max());
    cameFrom.assign(graph.steps.size(), kInvalidId);
    cameBy.assign(graph.steps.size(), EdgeId());
    std::vector<std::pair<f32, u32>> queue;
    const auto cheaper = [](const std::pair<f32, u32>& a, const std::pair<f32, u32>& b) {
        return a.first != b.first ? a.first > b.first : a.second > b.second;
    };
    best[start] = 0.0f;
    queue.emplace_back(0.0f, start);
    while (!queue.empty()) {
        std::pop_heap(queue.begin(), queue.end(), cheaper);
        const auto [cost, point] = queue.back();
        queue.pop_back();
        if (cost > best[point]) {
            continue;
        }
        for (const RegionGraph::Step& step : graph.steps[point]) {
            const f32 next = cost + (byCost ? step.cost : step.length);
            if (next >= best[step.to]) {
                continue;
            }
            best[step.to] = next;
            cameFrom[step.to] = point;
            cameBy[step.to] = step.edge;
            queue.emplace_back(next, step.to);
            std::push_heap(queue.begin(), queue.end(), cheaper);
        }
    }
}

/// The reached point farthest from where @p best was walked from.
u32 farthestOf(const RegionGraph& graph, const std::vector<f32>& best) {
    u32 out = kInvalidId;
    f32 far = -1.0f;
    for (const u32 point : graph.points) {
        if (best[point] != std::numeric_limits<f32>::max() && best[point] > far) {
            far = best[point];
            out = point;
        }
    }
    return out;
}

} // namespace

SeamPath ClosingSeam(const Mesh& mesh, const skinning::PointTable& points,
                     std::span<const FaceId> faces, const SeamPathOptions& options) {
    SeamPath out;
    out.stopped = true;
    if (!mesh.hasConnectivity() || faces.empty()) {
        return out;
    }
    const RegionGraph graph = regionGraph(mesh, points, faces, options);
    if (graph.points.size() < 2) {
        return out;
    }
    // Two sweeps: the point farthest from anywhere is one end of the region's
    // longest way across, and the point farthest from it is the other.
    std::vector<f32> best;
    std::vector<u32> cameFrom;
    std::vector<EdgeId> cameBy;
    walkRegion(graph, graph.points.front(), false, best, cameFrom, cameBy);
    const u32 from = farthestOf(graph, best);
    walkRegion(graph, from, false, best, cameFrom, cameBy);
    const u32 to = farthestOf(graph, best);
    if (from == kInvalidId || to == kInvalidId || from == to) {
        return out;
    }
    // The cut itself hides: the same two ends, walked by cost.
    walkRegion(graph, from, true, best, cameFrom, cameBy);
    for (u32 point = to; cameFrom[point] != kInvalidId; point = cameFrom[point]) {
        out.edges.push_back(cameBy[point]);
    }
    std::reverse(out.edges.begin(), out.edges.end());
    out.stopped = out.edges.empty();
    return out;
}

SeamPath RingSeam(const Mesh& mesh, const skinning::PointTable& points, const UvIslands& islands,
                  u32 island, const SeamPathOptions& options) {
    SeamPath out;
    out.stopped = true;
    if (!mesh.hasConnectivity() || island >= islands.count) {
        return out;
    }
    const Topology& topology = mesh.topology();
    const std::span<const Vector3f> positions =
        mesh.attributes.get<const Vector3f>(names::kPosition, Domain::Vertex);
    // The boundary's loops, laid end to end: one closes where its walk comes
    // back to the vertex it started from.
    struct Loop {
        std::vector<u32> points;
        f32 length = 0.0f;
    };
    std::vector<Loop> loops;
    const std::span<const HalfedgeId> boundary = islands.boundaryOf(island);
    for (std::size_t start = 0; start < boundary.size();) {
        Loop loop;
        const VertexId first = topology.from(boundary[start]);
        std::size_t k = start;
        for (; k < boundary.size(); ++k) {
            const HalfedgeId h = boundary[k];
            loop.points.push_back(points.pointOfVertex(topology.from(h).value()));
            if (topology.from(h).index() < positions.size() && topology.to(h).index() < positions.size()) {
                loop.length += (positions[topology.to(h).index()] - positions[topology.from(h).index()]).length();
            }
            if (topology.to(h) == first) {
                break;
            }
        }
        loops.push_back(std::move(loop));
        start = k + 1;
    }
    if (loops.size() < 2) {
        return out;
    }
    std::sort(loops.begin(), loops.end(), [](const Loop& a, const Loop& b) { return a.length > b.length; });
    std::vector<FaceId> faces;
    for (const u32 f : islands.facesOf(island)) {
        faces.push_back(FaceId(f));
    }
    const RegionGraph graph = regionGraph(mesh, points, faces, options);
    // From every point of the longest loop at once, to the first point of the
    // second one reached: the cheapest way across, round the hidden side.
    std::vector<f32> best(graph.steps.size(), std::numeric_limits<f32>::max());
    std::vector<u32> cameFrom(graph.steps.size(), kInvalidId);
    std::vector<EdgeId> cameBy(graph.steps.size());
    std::vector<u8> goal(graph.steps.size(), 0);
    for (const u32 p : loops[1].points) {
        if (p < goal.size()) {
            goal[p] = 1;
        }
    }
    std::vector<std::pair<f32, u32>> queue;
    const auto cheaper = [](const std::pair<f32, u32>& a, const std::pair<f32, u32>& b) {
        return a.first != b.first ? a.first > b.first : a.second > b.second;
    };
    for (const u32 p : loops[0].points) {
        if (p < best.size() && best[p] != 0.0f) {
            best[p] = 0.0f;
            queue.emplace_back(0.0f, p);
        }
    }
    std::make_heap(queue.begin(), queue.end(), cheaper);
    u32 reached = kInvalidId;
    while (!queue.empty()) {
        std::pop_heap(queue.begin(), queue.end(), cheaper);
        const auto [cost, point] = queue.back();
        queue.pop_back();
        if (cost > best[point]) {
            continue;
        }
        if (goal[point]) {
            reached = point;
            break;
        }
        for (const RegionGraph::Step& step : graph.steps[point]) {
            const f32 next = cost + step.cost;
            if (next >= best[step.to]) {
                continue;
            }
            best[step.to] = next;
            cameFrom[step.to] = point;
            cameBy[step.to] = step.edge;
            queue.emplace_back(next, step.to);
            std::push_heap(queue.begin(), queue.end(), cheaper);
        }
    }
    if (reached == kInvalidId) {
        return out;
    }
    for (u32 point = reached; cameFrom[point] != kInvalidId; point = cameFrom[point]) {
        out.edges.push_back(cameBy[point]);
    }
    std::reverse(out.edges.begin(), out.edges.end());
    out.stopped = out.edges.empty();
    return out;
}

u32 MirrorMarks(Mesh& view, u32 set, std::span<const u32> pointMirror) {
    if (!view.hasConnectivity() || pointMirror.empty()) {
        return 0;
    }
    const Topology& topology = std::as_const(view).topology();
    const std::span<const u8> marks =
        std::as_const(view).attributes.get<const u8>(names::uvSeam(set), Domain::Edge);
    if (marks.empty()) {
        return 0;
    }
    std::vector<EdgeId> add;
    for (u32 e = 0; e < marks.size() && e < topology.edgeCount(); ++e) {
        if (marks[e] == 0) {
            continue;
        }
        const HalfedgeId h = Topology::halfedge(EdgeId(e), 0);
        const u32 a = topology.from(h).value();
        const u32 b = topology.to(h).value();
        if (a >= pointMirror.size() || b >= pointMirror.size()) {
            continue;
        }
        const u32 twinA = pointMirror[a];
        const u32 twinB = pointMirror[b];
        if (twinA >= topology.vertexCount() || twinB >= topology.vertexCount()) {
            continue;
        }
        const HalfedgeId twin = topology.findHalfedge(VertexId(twinA), VertexId(twinB));
        if (!twin.valid()) {
            continue;
        }
        const EdgeId edge = Topology::edge(twin);
        if (edge.index() < marks.size() && marks[edge.index()] == 0) {
            add.push_back(edge);
        }
    }
    if (add.empty()) {
        return 0;
    }
    std::sort(add.begin(), add.end(), [](EdgeId a, EdgeId b) { return a.value() < b.value(); });
    add.erase(std::unique(add.begin(), add.end(),
                          [](EdgeId a, EdgeId b) { return a.value() == b.value(); }),
              add.end());
    ApplyMarks(view, set, std::span<const EdgeId>(add.data(), add.size()), true);
    return static_cast<u32>(add.size());
}

SkeletonSeams SeamsFromSkeleton(const Model& model, u32 mesh, Mesh& view, u32 set,
                                std::span<const u32> pointMirror) {
    SkeletonSeams out;
    if (!view.hasConnectivity() || mesh >= model.meshes.size()) {
        out.refusal = SkeletonSeams::Refusal::NoLimbs;
        return out;
    }
    const NodeTree& tree = model.nodes;
    const Topology& topology = std::as_const(view).topology();
    const std::span<const Vector3f> positions =
        std::as_const(view).attributes.get<const Vector3f>(names::kPosition, Domain::Vertex);
    const std::span<const Vector3f> normals =
        std::as_const(view).attributes.get<const Vector3f>(names::kNormal, Domain::Halfedge);
    if (positions.size() < topology.vertexCount()) {
        // No positions is no shape: there is nothing here to read a cut from.
        out.refusal = SkeletonSeams::Refusal::NoLimbs;
        return out;
    }

    // --- the parts -----------------------------------------------------------
    //
    // A node folds to the joint that really moves the surface: a twist bone is
    // its forearm, a kneecap is its knee, and a hock belongs to the shin above
    // it (EDIT_MODE_UV_DESIGN.md §5.3). Everything else is its own part.
    bool anyLimb = false;
    std::vector<u32> foldOf(tree.nodes.size(), kInvalidId);
    for (u32 node = 0; node < tree.nodes.size(); ++node) {
        u32 walk = node;
        for (u32 guard = 0; guard < 64; ++guard) {
            const RigRole role = RoleOf(tree, walk);
            if (role == RigRole::Upper || role == RigRole::Lower || role == RigRole::End) {
                anyLimb = true;
                break;
            }
            // A part is a thing that moves as one piece of the figure, which is
            // fewer things than there are bones: a twist bone is its forearm, a
            // kneecap is its knee, a toe is its foot, a neck is its chest, and
            // a helper the record could not name belongs to whatever turns it.
            // A spine folds into the spine above it, so a four-bone back is one
            // back (EDIT_MODE_UV_DESIGN.md §5.3).
            const u32 parent = ParentOf(tree, walk);
            const bool rides = role == RigRole::Twist || role == RigRole::Pad ||
                               role == RigRole::Hock || role == RigRole::Toe ||
                               role == RigRole::Neck || role == RigRole::Clavicle ||
                               role == RigRole::None ||
                               (role == RigRole::Spine && parent < tree.nodes.size() &&
                                RoleOf(tree, parent) == RigRole::Spine);
            if (!rides || parent >= tree.nodes.size() || parent == walk) {
                break;
            }
            walk = parent;
        }
        foldOf[node] = walk;
    }
    if (!anyLimb) {
        // No limb in the record is no shape to read: the honest answer, and the
        // caller's is the rig detector.
        out.refusal = SkeletonSeams::Refusal::NoLimbs;
        return out;
    }

    const SkinBinding& skin = view.skin;
    std::vector<u32> partOfFace(topology.faceCount(), kInvalidId);
    std::vector<u32> parts;
    for (u32 f = 0; f < topology.faceCount(); ++f) {
        const FaceId face(f);
        if (topology.isDeleted(face)) {
            continue;
        }
        // The heaviest bone over the face, which is the part the face moves
        // with whatever the rest of its vertices say.
        u32 bone = kInvalidId;
        f32 weight = 0.0f;
        for (const HalfedgeId h : topology.fh(face)) {
            const u32 vertex = topology.from(h).value();
            if (vertex >= skin.vertexCount()) {
                continue;
            }
            for (const Influence& influence : skin.forVertex(vertex)) {
                if (influence.weight > weight) {
                    weight = influence.weight;
                    bone = influence.bone;
                }
            }
        }
        const u32 part = (bone != kInvalidId && bone < foldOf.size()) ? foldOf[bone] : kInvalidId;
        partOfFace[f] = part;
        if (part != kInvalidId) {
            parts.push_back(part);
        }
    }
    std::sort(parts.begin(), parts.end());
    parts.erase(std::unique(parts.begin(), parts.end()), parts.end());
    out.parts = static_cast<u32>(parts.size());

    // --- the edges between two parts -----------------------------------------
    std::vector<EdgeId> cuts;
    f64 hidden = 0.0;
    f64 total = 0.0;
    const Vector3f front{1.0f, 0.0f, 0.0f}; // +X is the figure's front (mirror.h)
    for (u32 e = 0; e < topology.edgeCount(); ++e) {
        const EdgeId edge(e);
        if (topology.isDeleted(edge) || topology.isBoundary(edge)) {
            continue;
        }
        const FaceId a = topology.face(Topology::halfedge(edge, 0));
        const FaceId b = topology.face(Topology::halfedge(edge, 1));
        if (!a.valid() || !b.valid()) {
            continue;
        }
        if (partOfFace[a.value()] == partOfFace[b.value()]) {
            continue;
        }
        cuts.push_back(edge);
        const HalfedgeId h = Topology::halfedge(edge, 0);
        const f32 length =
            (positions[topology.to(h).index()] - positions[topology.from(h).index()]).length();
        total += length;
        if (edgeNormal(topology, edge, positions, normals).dot(front) <= 0.0f) {
            hidden += length;
        }
    }

    // --- one cut along each part, on the side away from the front ------------
    //
    // A limb is a tube: cutting it from its neighbours leaves a sleeve, and a
    // sleeve has to be cut once along its length before it will lie flat.
    if (!cuts.empty()) {
        ApplyMarks(view, set, std::span<const EdgeId>(cuts.data(), cuts.size()), true);
    }
    const skinning::PointTable points = skinning::BuildPointTable(view);
    SeamPathOptions options;
    options.visible = front;
    for (const u32 part : parts) {
        // The part's own border edges, by the neighbour they face: the two ends
        // of a sleeve are the two longest runs of them.
        std::vector<EdgeId> border;
        for (const EdgeId edge : cuts) {
            // `cuts` grows as parts are opened, and a path may run along the
            // mesh border, where one side has no face at all.
            const FaceId a = topology.face(Topology::halfedge(edge, 0));
            const FaceId b = topology.face(Topology::halfedge(edge, 1));
            const u32 partA = a.valid() && a.value() < partOfFace.size()
                                  ? partOfFace[a.value()]
                                  : kInvalidId;
            const u32 partB = b.valid() && b.value() < partOfFace.size()
                                  ? partOfFace[b.value()]
                                  : kInvalidId;
            if (partA == part || partB == part) {
                border.push_back(edge);
            }
        }
        if (border.size() < 2) {
            continue;
        }
        // The two border edges farthest apart is a good enough stand-in for the
        // two ends of the sleeve, and needs no loop walk.
        EdgeId fromEdge = border.front();
        EdgeId toEdge = border.back();
        f32 best = -1.0f;
        for (std::size_t i = 0; i < border.size(); ++i) {
            const Vector3f pi =
                positions[topology.from(Topology::halfedge(border[i], 0)).index()];
            for (std::size_t j = i + 1; j < border.size(); ++j) {
                const Vector3f pj =
                    positions[topology.from(Topology::halfedge(border[j], 0)).index()];
                const f32 distance = (pj - pi).length();
                if (distance > best) {
                    best = distance;
                    fromEdge = border[i];
                    toEdge = border[j];
                }
            }
        }
        const SeamPath path = FindSeamPath(view, points, fromEdge, toEdge, options);
        if (path.stopped || path.edges.empty()) {
            continue;
        }
        ApplyMarks(view, set, std::span<const EdgeId>(path.edges.data(), path.edges.size()),
                   true);
        for (const EdgeId edge : path.edges) {
            const HalfedgeId h = Topology::halfedge(edge, 0);
            const f32 length =
                (positions[topology.to(h).index()] - positions[topology.from(h).index()])
                    .length();
            total += length;
            if (edgeNormal(topology, edge, positions, normals).dot(front) <= 0.0f) {
                hidden += length;
            }
            cuts.push_back(edge);
        }
    }

    out.cuts = static_cast<u32>(cuts.size());
    out.cuts += MirrorMarks(view, set, pointMirror);
    out.hiddenShare = total > 0.0 ? static_cast<f32>(hidden / total) : 0.0f;
    return out;
}

} // namespace uv
} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
