// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/geometry/select.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <unordered_map>
#include <unordered_set>

namespace whiteout {
namespace models {
namespace wem {
namespace geom {

namespace {

u64 pairKey(u32 a, u32 b) {
    return (static_cast<u64>(std::min(a, b)) << 32) | std::max(a, b);
}

// Straight on over triangles (`Walk::straightOn`), in radians: how far the
// line may turn at a point, how much less it must turn than the next edge, and
// how much further off an edge marked a diagonal counts.
constexpr f32 kStraightLimit = 0.610865f; // 35 degrees
constexpr f32 kStraightApart = 0.174533f; // 10 degrees
constexpr f32 kDiagonalTurn = 0.174533f;  // 10 degrees

f32 dot3(const Vector3f& a, const Vector3f& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

/// @p v laid into the plane square to @p normal, at unit length; false when it
/// has no length there.
bool flatten(const Vector3f& v, const Vector3f& normal, Vector3f& out) {
    const f32 nn = dot3(normal, normal);
    out = nn > 0.0f ? v - normal * (dot3(v, normal) / nn) : v;
    const f32 length = std::sqrt(dot3(out, out));
    if (!(length > 0.0f)) {
        return false;
    }
    out = out / length;
    return true;
}

/// The mesh seen as points: each vertex's point, each point's members, and the
/// edges realising each pair of points. A table that does not cover the mesh
/// (stale, or never built) makes every vertex its own point.
///
/// With @p diagonals (`JoinDiagonals`), the faces are seen as the quads those
/// edges cut: `next`, `prev`, `neighbours` and `turns` step over a marked edge.
class Walk {
public:
    Walk(const Mesh& mesh, const PointTable& points, std::span<const u8> diagonals = {})
        : mesh_(mesh), topology_(mesh.topology()), diagonals_(diagonals),
          positions_(mesh.attributes.get<const Vector3f>(names::kPosition, Domain::Vertex)) {
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

    /// A diagonal the walk steps over: marked, with a face on both sides.
    bool hidden(HalfedgeId h) const {
        const u32 e = Topology::edge(h).value();
        return e < diagonals_.size() && diagonals_[e] != 0 && topology_.face(h).valid() &&
               topology_.face(Topology::opposite(h)).valid();
    }

    /// The halfedge after @p h round its face, over a diagonal into the
    /// triangle beyond it; `prev` the one before. A triangle has one diagonal
    /// at most, and the count holds a mask that says otherwise to a few steps.
    HalfedgeId next(HalfedgeId h) const {
        HalfedgeId n = topology_.next(h);
        for (u32 i = 0; i < 4 && hidden(n); ++i) {
            n = topology_.next(Topology::opposite(n));
        }
        return n;
    }

    HalfedgeId prev(HalfedgeId h) const {
        HalfedgeId p = topology_.prev(h);
        for (u32 i = 0; i < 4 && hidden(p); ++i) {
            p = topology_.prev(Topology::opposite(p));
        }
        return p;
    }

    /// The four sides of the quad on @p h's side, from @p h round; false when
    /// the face there, diagonals stepped over, is not one.
    bool quad(HalfedgeId h, std::array<HalfedgeId, 4>& sides) const {
        HalfedgeId at = h;
        for (HalfedgeId& side : sides) {
            side = at;
            at = next(at);
        }
        return at == h && sides[1] != h && sides[2] != h && sides[3] != h;
    }

    /// Every edge realising the pair @p key (point ids).
    const std::vector<u32>& edgesOf(u64 key) const {
        static const std::vector<u32> none;
        const auto found = pairEdges_.find(key);
        return found == pairEdges_.end() ? none : found->second;
    }

    /// The points one edge away from @p point, sorted; @p cuts, across a
    /// diagonal too.
    std::vector<u32> neighbours(u32 point, bool cuts = false) const {
        std::vector<u32> out;
        for (const u32 m : members(point)) {
            if (!topology_.outgoing(VertexId(m)).valid()) {
                continue;
            }
            for (const HalfedgeId h : topology_.voh(VertexId(m))) {
                const u32 q = this->point(topology_.to(h).value());
                if (q != point && (cuts || !hidden(h))) {
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
                if (!topology_.face(h).valid() || hidden(h)) {
                    continue;
                }
                out.push_back({this->point(topology_.to(h).value()), this->point(topology_.from(prev(h)).value())});
            }
        }
        return out;
    }

    /// Whether the walk was handed diagonals, and so passes triangles.
    bool overTriangles() const {
        return !diagonals_.empty();
    }

    /// Whether a triangle has a corner at @p point.
    bool atTriangle(u32 point) const {
        for (const u32 f : faces(point)) {
            if (topology_.valence(FaceId(f)) == 3) {
                return true;
            }
        }
        return false;
    }

    /// Straight on through @p current, arriving from @p previous, where the
    /// faces there are not four quads: the neighbour whose edge turns least
    /// once both are laid into the point's tangent plane, which is where a
    /// line round a limb is straight. kInvalidId when none is within
    /// kStraightLimit, or the next is within kStraightApart of it.
    u32 straightOn(u32 previous, u32 current) const {
        if (members(previous).empty() || members(current).empty() || positions_.size() < topology_.vertexCount()) {
            return kInvalidId;
        }
        const Vector3f at = place(current);
        // The faces' area normals, summed: Newell's, so a polygon counts whole.
        Vector3f normal{0.0f, 0.0f, 0.0f};
        for (const u32 f : faces(current)) {
            const std::vector<u32> ring = faceVertices(f);
            for (std::size_t i = 0; i < ring.size(); ++i) {
                const Vector3f& a = positions_[ring[i]];
                const Vector3f& b = positions_[ring[(i + 1) % ring.size()]];
                normal.x += (a.y - b.y) * (a.z + b.z);
                normal.y += (a.z - b.z) * (a.x + b.x);
                normal.z += (a.x - b.x) * (a.y + b.y);
            }
        }
        Vector3f in;
        if (!flatten(at - place(previous), normal, in)) {
            return kInvalidId;
        }
        f32 best = 8.0f;
        f32 second = 8.0f;
        u32 chosen = kInvalidId;
        for (const u32 q : neighbours(current, true)) {
            Vector3f out;
            if (q == previous || !flatten(place(q) - at, normal, out)) {
                continue;
            }
            f32 turn = std::acos(std::clamp(dot3(out, in), -1.0f, 1.0f));
            // A line of the quads before the cut beside it.
            const std::vector<u32>& edges = edgesOf(pairKey(current, q));
            if (std::all_of(edges.begin(), edges.end(),
                            [&](u32 e) { return hidden(Topology::halfedge(EdgeId(e), 0)); })) {
                turn += kDiagonalTurn;
            }
            if (turn < best) {
                second = best;
                best = turn;
                chosen = q;
            } else if (turn < second) {
                second = turn;
            }
        }
        return best <= kStraightLimit && second - best >= kStraightApart ? chosen : kInvalidId;
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
    /// Where @p point is: its first member's position.
    const Vector3f& place(u32 point) const {
        return positions_[members(point).front()];
    }

    const Mesh& mesh_;
    const Topology& topology_;
    std::span<const u8> diagonals_;
    std::span<const Vector3f> positions_;
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

ElementSet EdgeLoop(const Mesh& mesh, const PointTable& points, EdgeId edge, std::span<const u8> diagonals) {
    ElementSet out;
    if (!mesh.hasConnectivity()) {
        return out;
    }
    const Topology& topology = mesh.topology();
    if (edge.index() >= topology.edgeCount() || topology.isDeleted(edge)) {
        return out;
    }
    const Walk walk(mesh, points, diagonals);
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
            // Straight on: the one neighbour no face at `current` turns
            // through together with `previous`.
            u32 next = kInvalidId;
            const std::vector<u32> around = walk.neighbours(current);
            if (around.size() == 4) {
                std::vector<u32> beside{previous};
                for (const auto& [x, y] : walk.turns(current)) {
                    if (x == previous) {
                        beside.push_back(y);
                    } else if (y == previous) {
                        beside.push_back(x);
                    }
                }
                sortUnique(beside);
                u32 candidates = 0;
                for (const u32 q : around) {
                    if (!contains(beside, q)) {
                        next = q;
                        ++candidates;
                    }
                }
                if (candidates != 1) {
                    next = kInvalidId;
                }
            }
            // Where triangles break the quads: straight on over the surface,
            // when the line read back from there is this one.
            if (next == kInvalidId && walk.overTriangles() && walk.atTriangle(current)) {
                next = walk.straightOn(previous, current);
                if (next != kInvalidId && walk.straightOn(next, current) != previous) {
                    next = kInvalidId;
                }
            }
            if (next == kInvalidId || !visited.insert(pairKey(current, next)).second) {
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
/// crossed, a quad two triangles make as both.
ElementSet ringOf(const Mesh& mesh, const PointTable& points, EdgeId edge, std::span<const u8> diagonals) {
    ElementSet out;
    if (!mesh.hasConnectivity()) {
        return out;
    }
    const Topology& topology = mesh.topology();
    if (edge.index() >= topology.edgeCount() || topology.isDeleted(edge)) {
        return out;
    }
    const Walk walk(mesh, points, diagonals);
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
                std::array<HalfedgeId, 4> sides;
                if (!topology.face(h).valid() || walk.hidden(h) || !walk.quad(h, sides) ||
                    crossed.count(topology.face(h).value()) != 0) {
                    continue;
                }
                for (const HalfedgeId s : sides) {
                    if (crossed.insert(topology.face(s).value()).second) {
                        out.faces.push_back(topology.face(s).value());
                    }
                }
                const u64 opposite = walk.key(sides[2]);
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

ElementSet EdgeRing(const Mesh& mesh, const PointTable& points, EdgeId edge, std::span<const u8> diagonals) {
    ElementSet ring = ringOf(mesh, points, edge, diagonals);
    ring.faces.clear();
    return ring;
}

ElementSet FaceLoop(const Mesh& mesh, const PointTable& points, EdgeId edge, std::span<const u8> diagonals) {
    ElementSet ring = ringOf(mesh, points, edge, diagonals);
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

ElementSet FacesOfMaterials(const Mesh& mesh, std::span<const u32> slots) {
    ElementSet out;
    std::vector<u8> wanted(mesh.sections.size(), 0);
    for (std::size_t s = 0; s < mesh.sections.size(); ++s) {
        wanted[s] = std::find(slots.begin(), slots.end(), mesh.sections[s].materialSlot) != slots.end() ? 1 : 0;
    }
    const std::span<const u32> sections = mesh.faceSections();
    for (u32 f = 0; f < mesh.faceCount(); ++f) {
        // A face past the layer is in section 0, as `faceSections` says.
        const u32 section = f < sections.size() ? sections[f] : 0;
        if (section < wanted.size() && wanted[section] != 0) {
            out.faces.push_back(f);
        }
    }
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
