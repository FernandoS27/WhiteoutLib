// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/geometry/select.h>

#include <algorithm>
#include <deque>
#include <unordered_map>
#include <unordered_set>

namespace whiteout {
namespace models {
namespace wem {
namespace geom {

void ElementSet::normalise() {
    for (std::vector<u32>* list : {&vertices, &edges, &faces}) {
        std::sort(list->begin(), list->end());
        list->erase(std::unique(list->begin(), list->end()), list->end());
    }
}

namespace {

u64 pairKey(u32 a, u32 b) {
    return (static_cast<u64>(std::min(a, b)) << 32) | std::max(a, b);
}

/// The mesh seen as points: each vertex's point, each point's members, and the
/// edges realising each pair of points. A table that does not cover the mesh
/// (stale, or never built) makes every vertex its own point.
class Walk {
public:
    Walk(const Mesh& mesh, const PointTable& points) : mesh_(mesh), topology_(mesh.topology()) {
        const u32 vertexCount = topology_.vertexCount();
        const bool covers = points.pointOf.size() == vertexCount;
        pointOf_.resize(vertexCount);
        u32 pointCount = 0;
        for (u32 v = 0; v < vertexCount; ++v) {
            pointOf_[v] = covers ? points.pointOf[v] : v;
            pointCount = std::max(pointCount, pointOf_[v] + 1);
        }
        members_.resize(pointCount);
        for (u32 v = 0; v < vertexCount; ++v) {
            if (!topology_.isDeleted(VertexId(v))) {
                members_[pointOf_[v]].push_back(v);
            }
        }
        for (u32 e = 0; e < topology_.edgeCount(); ++e) {
            if (topology_.isDeleted(EdgeId(e))) {
                continue;
            }
            const HalfedgeId h = Topology::halfedge(EdgeId(e), 0);
            pairEdges_[key(h)].push_back(e);
        }
    }

    const Topology& topology() const {
        return topology_;
    }

    u32 point(u32 vertex) const {
        return vertex < pointOf_.size() ? pointOf_[vertex] : kInvalidId;
    }

    const std::vector<u32>& members(u32 point) const {
        static const std::vector<u32> none;
        return point < members_.size() ? members_[point] : none;
    }

    u64 key(HalfedgeId h) const {
        return pairKey(point(topology_.from(h).value()), point(topology_.to(h).value()));
    }

    /// Every edge realising the pair @p key (point ids).
    const std::vector<u32>& edgesOf(u64 key) const {
        static const std::vector<u32> none;
        const auto found = pairEdges_.find(key);
        return found == pairEdges_.end() ? none : found->second;
    }

    /// The points one edge away from @p point, sorted.
    std::vector<u32> neighbours(u32 point) const {
        std::vector<u32> out;
        for (const u32 m : members(point)) {
            if (!topology_.outgoing(VertexId(m)).valid()) {
                continue;
            }
            for (const HalfedgeId h : topology_.voh(VertexId(m))) {
                const u32 q = this->point(topology_.to(h).value());
                if (q != point) {
                    out.push_back(q);
                }
            }
        }
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }

    /// The live faces around any member of @p point, sorted.
    std::vector<u32> faces(u32 point) const {
        std::vector<u32> out;
        for (const u32 m : members(point)) {
            if (!topology_.outgoing(VertexId(m)).valid()) {
                continue;
            }
            for (const HalfedgeId h : topology_.voh(VertexId(m))) {
                const FaceId f = topology_.face(h);
                if (f.valid() && !topology_.isDeleted(f)) {
                    out.push_back(f.value());
                }
            }
        }
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }

    /// A border edge whose pair of points no other edge realises: the mesh's
    /// own border. The two sides of a crease the weld kept apart are border
    /// edges of the WEM, but each pair has two, so over points it is interior.
    bool realBorder(u32 edge) const {
        return topology_.isBoundary(EdgeId(edge)) &&
               edgesOf(key(Topology::halfedge(EdgeId(edge), 0))).size() < 2;
    }

    bool onBorder(u32 point) const {
        for (const u32 m : members(point)) {
            if (!topology_.outgoing(VertexId(m)).valid()) {
                continue;
            }
            for (const HalfedgeId h : topology_.voh(VertexId(m))) {
                if (realBorder(Topology::edge(h).value())) {
                    return true;
                }
            }
        }
        return false;
    }

    /// The points on either side of @p point in the faces around it: the pairs
    /// a face turns through there.
    std::vector<std::pair<u32, u32>> turns(u32 point) const {
        std::vector<std::pair<u32, u32>> out;
        for (const u32 m : members(point)) {
            if (!topology_.outgoing(VertexId(m)).valid()) {
                continue;
            }
            for (const HalfedgeId h : topology_.voh(VertexId(m))) {
                if (!topology_.face(h).valid()) {
                    continue;
                }
                out.push_back({this->point(topology_.to(h).value()),
                               this->point(topology_.from(topology_.prev(h)).value())});
            }
        }
        return out;
    }

    std::vector<u32> faceVertices(u32 face) const {
        std::vector<u32> out;
        for (const VertexId v : topology_.fv(FaceId(face))) {
            out.push_back(v.value());
        }
        return out;
    }

    std::vector<u32> faceEdges(u32 face) const {
        std::vector<u32> out;
        for (const HalfedgeId h : topology_.fh(FaceId(face))) {
            out.push_back(Topology::edge(h).value());
        }
        return out;
    }

    /// Every member of every point of @p vertices.
    std::vector<u32> closePoints(std::span<const u32> vertices) const {
        std::vector<u32> out;
        for (const u32 v : vertices) {
            for (const u32 m : members(point(v))) {
                out.push_back(m);
            }
        }
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }

    /// Every edge realising the pair of any edge of @p edges.
    std::vector<u32> closePairs(std::span<const u32> edges) const {
        std::vector<u32> out;
        for (const u32 e : edges) {
            if (e >= topology_.edgeCount() || topology_.isDeleted(EdgeId(e))) {
                continue;
            }
            for (const u32 same : edgesOf(key(Topology::halfedge(EdgeId(e), 0)))) {
                out.push_back(same);
            }
        }
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }

    bool liveEdge(u32 e) const {
        return e < topology_.edgeCount() && !topology_.isDeleted(EdgeId(e));
    }

    bool liveFace(u32 f) const {
        return f < topology_.faceCount() && !topology_.isDeleted(FaceId(f)) &&
               topology_.halfedge(FaceId(f)).valid();
    }

    bool liveVertex(u32 v) const {
        return v < topology_.vertexCount() && !topology_.isDeleted(VertexId(v));
    }

    const Mesh& mesh() const {
        return mesh_;
    }

private:
    const Mesh& mesh_;
    const Topology& topology_;
    std::vector<u32> pointOf_;
    std::vector<std::vector<u32>> members_;
    std::unordered_map<u64, std::vector<u32>> pairEdges_; // looked up, never iterated
};

bool contains(const std::vector<u32>& sorted, u32 value) {
    return std::binary_search(sorted.begin(), sorted.end(), value);
}

void sortUnique(std::vector<u32>& list) {
    std::sort(list.begin(), list.end());
    list.erase(std::unique(list.begin(), list.end()), list.end());
}

/// The points of @p vertices, sorted.
std::vector<u32> pointsOf(const Walk& walk, std::span<const u32> vertices) {
    std::vector<u32> out;
    for (const u32 v : vertices) {
        if (walk.liveVertex(v)) {
            out.push_back(walk.point(v));
        }
    }
    sortUnique(out);
    return out;
}

/// The points at the ends of @p edges, sorted.
std::vector<u32> pointsOfEdges(const Walk& walk, std::span<const u32> edges) {
    std::vector<u32> out;
    for (const u32 e : edges) {
        if (!walk.liveEdge(e)) {
            continue;
        }
        const HalfedgeId h = Topology::halfedge(EdgeId(e), 0);
        out.push_back(walk.point(walk.topology().from(h).value()));
        out.push_back(walk.point(walk.topology().to(h).value()));
    }
    sortUnique(out);
    return out;
}

/// The points of @p faces' corners, sorted.
std::vector<u32> pointsOfFaces(const Walk& walk, std::span<const u32> faces) {
    std::vector<u32> out;
    for (const u32 f : faces) {
        if (!walk.liveFace(f)) {
            continue;
        }
        for (const u32 v : walk.faceVertices(f)) {
            out.push_back(walk.point(v));
        }
    }
    sortUnique(out);
    return out;
}

std::vector<u32> membersOf(const Walk& walk, const std::vector<u32>& points) {
    std::vector<u32> out;
    for (const u32 p : points) {
        for (const u32 m : walk.members(p)) {
            out.push_back(m);
        }
    }
    sortUnique(out);
    return out;
}

/// Every live edge with an end at one of @p points (sorted).
std::vector<u32> edgesTouching(const Walk& walk, const std::vector<u32>& points) {
    std::vector<u32> out;
    for (const u32 p : points) {
        for (const u32 m : walk.members(p)) {
            if (!walk.topology().outgoing(VertexId(m)).valid()) {
                continue;
            }
            for (const HalfedgeId h : walk.topology().voh(VertexId(m))) {
                out.push_back(Topology::edge(h).value());
            }
        }
    }
    sortUnique(out);
    return out;
}

/// Every live face with a corner at one of @p points (sorted).
std::vector<u32> facesTouching(const Walk& walk, const std::vector<u32>& points) {
    std::vector<u32> out;
    for (const u32 p : points) {
        for (const u32 f : walk.faces(p)) {
            out.push_back(f);
        }
    }
    sortUnique(out);
    return out;
}

} // namespace

// ============================================================================
// Loops and rings
// ============================================================================

ElementSet BorderLoop(const Mesh& mesh, const PointTable& points, EdgeId edge) {
    ElementSet out;
    if (!mesh.hasConnectivity()) {
        return out;
    }
    const Topology& topology = mesh.topology();
    if (edge.index() >= topology.edgeCount() || topology.isDeleted(edge)) {
        return out;
    }
    const Walk walk(mesh, points);
    if (!walk.realBorder(edge.value())) {
        return out;
    }
    HalfedgeId start = Topology::halfedge(edge, 0);
    if (topology.face(start).valid()) {
        start = Topology::halfedge(edge, 1);
    }
    // Along the mesh's border over points: from each vertex's point, the border
    // halfedge that leaves it, stepping past a crease's twins.
    HalfedgeId current = start;
    u32 guard = topology.halfedgeCount();
    do {
        out.edges.push_back(Topology::edge(current).value());
        HalfedgeId next;
        for (const u32 m : walk.members(walk.point(topology.to(current).value()))) {
            if (!topology.outgoing(VertexId(m)).valid()) {
                continue;
            }
            for (const HalfedgeId h : topology.voh(VertexId(m))) {
                if (!topology.face(h).valid() && h != Topology::opposite(current) &&
                    walk.realBorder(Topology::edge(h).value())) {
                    next = h;
                    break;
                }
            }
            if (next.valid()) {
                break;
            }
        }
        if (!next.valid()) {
            break;
        }
        current = next;
    } while (current != start && guard-- > 0);
    out.normalise();
    return out;
}

ElementSet EdgeLoop(const Mesh& mesh, const PointTable& points, EdgeId edge) {
    ElementSet out;
    if (!mesh.hasConnectivity()) {
        return out;
    }
    const Topology& topology = mesh.topology();
    if (edge.index() >= topology.edgeCount() || topology.isDeleted(edge)) {
        return out;
    }
    const Walk walk(mesh, points);
    if (walk.realBorder(edge.value())) {
        return BorderLoop(mesh, points, edge);
    }
    const HalfedgeId h = Topology::halfedge(edge, 0);
    const u32 a = walk.point(topology.from(h).value());
    const u32 b = walk.point(topology.to(h).value());
    std::unordered_set<u64> visited{pairKey(a, b)};
    for (const u32 e : walk.edgesOf(pairKey(a, b))) {
        out.edges.push_back(e);
    }
    for (const auto& [from, to] : {std::pair<u32, u32>{a, b}, std::pair<u32, u32>{b, a}}) {
        u32 previous = from;
        u32 current = to;
        while (!walk.onBorder(current)) {
            const std::vector<u32> around = walk.neighbours(current);
            if (around.size() != 4) {
                break;
            }
            // Straight on: the one neighbour no face at `current` turns
            // through together with `previous`.
            std::vector<u32> beside{previous};
            for (const auto& [x, y] : walk.turns(current)) {
                if (x == previous) {
                    beside.push_back(y);
                } else if (y == previous) {
                    beside.push_back(x);
                }
            }
            sortUnique(beside);
            u32 next = kInvalidId;
            u32 candidates = 0;
            for (const u32 q : around) {
                if (!contains(beside, q)) {
                    next = q;
                    ++candidates;
                }
            }
            if (candidates != 1 || !visited.insert(pairKey(current, next)).second) {
                break;
            }
            for (const u32 e : walk.edgesOf(pairKey(current, next))) {
                out.edges.push_back(e);
            }
            previous = current;
            current = next;
        }
    }
    out.normalise();
    return out;
}

namespace {

/// Across quads from @p edge's pair, both ways: the pairs reached and the quads
/// crossed.
ElementSet ringOf(const Mesh& mesh, const PointTable& points, EdgeId edge) {
    ElementSet out;
    if (!mesh.hasConnectivity()) {
        return out;
    }
    const Topology& topology = mesh.topology();
    if (edge.index() >= topology.edgeCount() || topology.isDeleted(edge)) {
        return out;
    }
    const Walk walk(mesh, points);
    const u64 start = walk.key(Topology::halfedge(edge, 0));
    std::unordered_set<u64> visited{start};
    std::unordered_set<u32> crossed;
    std::deque<u64> queue{start};
    while (!queue.empty()) {
        const u64 pair = queue.front();
        queue.pop_front();
        for (const u32 e : walk.edgesOf(pair)) {
            out.edges.push_back(e);
            for (u32 side = 0; side < 2; ++side) {
                const HalfedgeId h = Topology::halfedge(EdgeId(e), side);
                const FaceId f = topology.face(h);
                if (!f.valid() || topology.valence(f) != 4 || !crossed.insert(f.value()).second) {
                    continue;
                }
                out.faces.push_back(f.value());
                const u64 opposite = walk.key(topology.next(topology.next(h)));
                if (visited.insert(opposite).second) {
                    queue.push_back(opposite);
                }
            }
        }
    }
    out.normalise();
    return out;
}

} // namespace

ElementSet EdgeRing(const Mesh& mesh, const PointTable& points, EdgeId edge) {
    ElementSet ring = ringOf(mesh, points, edge);
    ring.faces.clear();
    return ring;
}

ElementSet FaceLoop(const Mesh& mesh, const PointTable& points, EdgeId edge) {
    ElementSet ring = ringOf(mesh, points, edge);
    ring.edges.clear();
    return ring;
}

// ============================================================================
// Grow, Shrink, Linked
// ============================================================================

ElementSet Grow(const Mesh& mesh, const PointTable& points, const ElementSet& selection) {
    ElementSet out;
    if (!mesh.hasConnectivity()) {
        return out;
    }
    const Walk walk(mesh, points);
    if (!selection.vertices.empty()) {
        // Face step: every corner of every face around a selected point.
        const std::vector<u32> faces = facesTouching(walk, pointsOf(walk, selection.vertices));
        std::vector<u32> grown = pointsOf(walk, selection.vertices);
        const std::vector<u32> more = pointsOfFaces(walk, faces);
        grown.insert(grown.end(), more.begin(), more.end());
        sortUnique(grown);
        out.vertices = membersOf(walk, grown);
    }
    if (!selection.edges.empty()) {
        out.edges = edgesTouching(walk, pointsOfEdges(walk, selection.edges));
    }
    if (!selection.faces.empty()) {
        out.faces = facesTouching(walk, pointsOfFaces(walk, selection.faces));
    }
    out.normalise();
    return out;
}

ElementSet Shrink(const Mesh& mesh, const PointTable& points, const ElementSet& selection) {
    ElementSet out;
    if (!mesh.hasConnectivity()) {
        return out;
    }
    const Walk walk(mesh, points);
    if (!selection.vertices.empty()) {
        const std::vector<u32> chosen = pointsOf(walk, selection.vertices);
        std::vector<u32> kept;
        for (const u32 p : chosen) {
            bool inside = true;
            for (const u32 f : walk.faces(p)) {
                for (const u32 v : walk.faceVertices(f)) {
                    inside = inside && contains(chosen, walk.point(v));
                }
            }
            if (inside) {
                kept.push_back(p);
            }
        }
        out.vertices = membersOf(walk, kept);
    }
    if (!selection.edges.empty()) {
        std::vector<u32> chosen = walk.closePairs(selection.edges);
        for (const u32 e : chosen) {
            const HalfedgeId h = Topology::halfedge(EdgeId(e), 0);
            const std::vector<u32> ends{walk.point(walk.topology().from(h).value()),
                                        walk.point(walk.topology().to(h).value())};
            bool inside = true;
            for (const u32 other : edgesTouching(walk, ends)) {
                inside = inside && contains(chosen, other);
            }
            if (inside) {
                out.edges.push_back(e);
            }
        }
    }
    if (!selection.faces.empty()) {
        std::vector<u32> chosen = selection.faces;
        sortUnique(chosen);
        for (const u32 f : chosen) {
            if (!walk.liveFace(f)) {
                continue;
            }
            bool inside = true;
            for (const u32 other : facesTouching(walk, pointsOfFaces(walk, std::vector<u32>{f}))) {
                inside = inside && contains(chosen, other);
            }
            if (inside) {
                out.faces.push_back(f);
            }
        }
    }
    out.normalise();
    return out;
}

namespace {

/// The angle two faces meet at, across @p edge: 0 where they are flat and pi
/// where they fold back on each other. Zero without two live faces.
f32 dihedralAcross(const Mesh& mesh, const Topology& topology, u32 edge) {
    const HalfedgeId left = Topology::halfedge(EdgeId(edge), 0);
    const HalfedgeId right = Topology::opposite(left);
    const FaceId first = topology.face(left);
    const FaceId second = topology.face(right);
    if (!first.valid() || !second.valid() || topology.isDeleted(first) || topology.isDeleted(second)) {
        return 0.0f;
    }
    const std::span<const Vector3f> at = mesh.attributes.get<const Vector3f>(names::kPosition, Domain::Vertex);
    const auto normalOf = [&](FaceId face) {
        Vector3f sum{0.0f, 0.0f, 0.0f};
        for (const HalfedgeId h : topology.fh(face)) {
            const u32 a = topology.from(h).value();
            const u32 b = topology.to(h).value();
            if (a < at.size() && b < at.size()) {
                sum = sum + cross(at[a], at[b]);
            }
        }
        const f32 length = sum.length();
        return length > 1e-12f ? sum * (1.0f / length) : Vector3f{0.0f, 0.0f, 0.0f};
    };
    const Vector3f a = normalOf(first);
    const Vector3f b = normalOf(second);
    if (a.length() < 0.5f || b.length() < 0.5f) {
        return 0.0f;
    }
    return std::acos(std::clamp(a.dot(b), -1.0f, 1.0f));
}

} // namespace

ElementSet SharpEdges(const Mesh& mesh, f32 angle) {
    ElementSet out;
    if (!mesh.hasConnectivity()) {
        return out;
    }
    const Topology& topology = mesh.topology();
    for (u32 e = 0; e < topology.edgeCount(); ++e) {
        if (!topology.isDeleted(EdgeId(e)) && dihedralAcross(mesh, topology, e) > angle) {
            out.edges.push_back(e);
        }
    }
    out.normalise();
    return out;
}

ElementSet LinkedByAngle(const Mesh& mesh, const PointTable& points, const ElementSet& selection,
                         LinkDelimit delimit, f32 angle) {
    return detail::LinkedWithin(mesh, points, selection, delimit, angle);
}

ElementSet Linked(const Mesh& mesh, const PointTable& points, const ElementSet& selection,
                  LinkDelimit delimit) {
    return detail::LinkedWithin(mesh, points, selection, delimit, 0.0f);
}

namespace detail {

ElementSet LinkedWithin(const Mesh& mesh, const PointTable& points, const ElementSet& selection,
                        LinkDelimit delimit, f32 angle) {
    ElementSet out;
    if (!mesh.hasConnectivity()) {
        return out;
    }
    const Walk walk(mesh, points);
    const Topology& topology = walk.topology();
    const std::span<const u8> seam = mesh.attributes.get<const u8>(names::kSeam, Domain::Edge);
    const std::span<const u8> sharp = mesh.attributes.get<const u8>(names::kSharp, Domain::Edge);
    const std::span<const u32> sections = mesh.faceSections();
    const auto stops = [&](u32 e) {
        return (HasDelimit(delimit, LinkDelimit::Seam) && e < seam.size() && seam[e] != 0) ||
               (HasDelimit(delimit, LinkDelimit::Sharp) && e < sharp.size() && sharp[e] != 0) ||
               (HasDelimit(delimit, LinkDelimit::Angle) && dihedralAcross(mesh, topology, e) > angle);
    };
    const auto sectionOf = [&](u32 f) { return f < sections.size() ? sections[f] : 0u; };

    // Seeds: every face a selected element touches.
    std::vector<u32> seeds = facesTouching(walk, pointsOf(walk, selection.vertices));
    for (const u32 e : selection.edges) {
        if (!walk.liveEdge(e)) {
            continue;
        }
        for (u32 side = 0; side < 2; ++side) {
            const FaceId f = topology.face(Topology::halfedge(EdgeId(e), side));
            if (f.valid()) {
                seeds.push_back(f.value());
            }
        }
    }
    for (const u32 f : selection.faces) {
        if (walk.liveFace(f)) {
            seeds.push_back(f);
        }
    }
    sortUnique(seeds);

    std::vector<u8> reached(topology.faceCount(), 0);
    std::deque<u32> queue;
    for (const u32 f : seeds) {
        reached[f] = 1;
        queue.push_back(f);
    }
    while (!queue.empty()) {
        const u32 f = queue.front();
        queue.pop_front();
        for (const HalfedgeId h : topology.fh(FaceId(f))) {
            const u32 edgeHere = Topology::edge(h).value();
            if (stops(edgeHere)) {
                continue;
            }
            for (const u32 e : walk.edgesOf(walk.key(h))) {
                if (stops(e)) {
                    continue;
                }
                for (u32 side = 0; side < 2; ++side) {
                    const FaceId g = topology.face(Topology::halfedge(EdgeId(e), side));
                    if (!g.valid() || reached[g.value()] != 0 || !walk.liveFace(g.value())) {
                        continue;
                    }
                    if (HasDelimit(delimit, LinkDelimit::Section) && sectionOf(g.value()) != sectionOf(f)) {
                        continue;
                    }
                    reached[g.value()] = 1;
                    queue.push_back(g.value());
                }
            }
        }
    }
    std::vector<u32> component;
    for (u32 f = 0; f < reached.size(); ++f) {
        if (reached[f] != 0) {
            component.push_back(f);
        }
    }
    if (!selection.faces.empty()) {
        out.faces = component;
    }
    if (!selection.vertices.empty()) {
        out.vertices = membersOf(walk, pointsOfFaces(walk, component));
    }
    if (!selection.edges.empty()) {
        for (const u32 f : component) {
            for (const u32 e : walk.faceEdges(f)) {
                out.edges.push_back(e);
            }
        }
        out.edges = walk.closePairs(out.edges);
    }
    out.normalise();
    return out;
}

} // namespace detail

// ============================================================================
// Convert
// ============================================================================

ElementSet Convert(const Mesh& mesh, const PointTable& points, const ElementSet& selection,
                   ElementLevel from, ElementLevel to, ConvertRule rule) {
    ElementSet out;
    if (!mesh.hasConnectivity()) {
        return out;
    }
    const Walk walk(mesh, points);
    const Topology& topology = walk.topology();

    // What the selection is, whatever its level: its points, and its faces.
    std::vector<u32> chosenPoints;
    std::vector<u32> chosenFaces;
    switch (from) {
    case ElementLevel::Vertex:
        chosenPoints = pointsOf(walk, selection.vertices);
        break;
    case ElementLevel::Edge:
    case ElementLevel::Border:
        chosenPoints = pointsOfEdges(walk, selection.edges);
        break;
    case ElementLevel::Face:
        chosenFaces = selection.faces;
        sortUnique(chosenFaces);
        chosenPoints = pointsOfFaces(walk, chosenFaces);
        break;
    }
    const std::vector<u32> chosenEdges = walk.closePairs(selection.edges);

    switch (to) {
    case ElementLevel::Vertex:
        out.vertices = membersOf(walk, chosenPoints);
        break;
    case ElementLevel::Edge:
        if (from == ElementLevel::Edge || from == ElementLevel::Border) {
            out.edges = chosenEdges;
        } else if (from == ElementLevel::Face) {
            for (const u32 f : chosenFaces) {
                if (!walk.liveFace(f)) {
                    continue;
                }
                for (const u32 e : walk.faceEdges(f)) {
                    if (rule == ConvertRule::Touching) {
                        out.edges.push_back(e);
                        continue;
                    }
                    bool enclosed = true;
                    for (u32 side = 0; side < 2; ++side) {
                        const FaceId g = topology.face(Topology::halfedge(EdgeId(e), side));
                        enclosed = enclosed && (!g.valid() || contains(chosenFaces, g.value()));
                    }
                    if (enclosed) {
                        out.edges.push_back(e);
                    }
                }
            }
            out.edges = walk.closePairs(out.edges);
        } else {
            for (const u32 e : edgesTouching(walk, chosenPoints)) {
                const HalfedgeId h = Topology::halfedge(EdgeId(e), 0);
                const bool a = contains(chosenPoints, walk.point(topology.from(h).value()));
                const bool b = contains(chosenPoints, walk.point(topology.to(h).value()));
                if (rule == ConvertRule::Touching ? (a || b) : (a && b)) {
                    out.edges.push_back(e);
                }
            }
        }
        break;
    case ElementLevel::Border: {
        std::vector<u32> seen;
        for (const u32 e : edgesTouching(walk, chosenPoints)) {
            if (!walk.realBorder(e) || contains(seen, e)) {
                continue;
            }
            const ElementSet loop = BorderLoop(mesh, points, EdgeId(e));
            out.edges.insert(out.edges.end(), loop.edges.begin(), loop.edges.end());
            seen = out.edges;
            sortUnique(seen);
        }
        break;
    }
    case ElementLevel::Face:
        if (from == ElementLevel::Face) {
            out.faces = chosenFaces;
            break;
        }
        for (const u32 f : facesTouching(walk, chosenPoints)) {
            if (rule == ConvertRule::Touching) {
                // A face touches a vertex selection through a corner, and an
                // edge selection through one of its edges.
                bool touches = from == ElementLevel::Vertex;
                for (const u32 e : walk.faceEdges(f)) {
                    touches = touches || contains(chosenEdges, e);
                }
                if (touches) {
                    out.faces.push_back(f);
                }
                continue;
            }
            bool enclosed = true;
            if (from == ElementLevel::Vertex) {
                for (const u32 v : walk.faceVertices(f)) {
                    enclosed = enclosed && contains(chosenPoints, walk.point(v));
                }
            } else {
                for (const u32 e : walk.faceEdges(f)) {
                    enclosed = enclosed && contains(chosenEdges, e);
                }
            }
            if (enclosed) {
                out.faces.push_back(f);
            }
        }
        break;
    }
    out.normalise();
    return out;
}

} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
