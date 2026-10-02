// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "work_mesh.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

namespace whiteout {
namespace models {
namespace wem {
namespace geom {
namespace retopo {

namespace {

void Link(WorkMesh& mesh, u32 a, u32 b) {
    if (a != kNone) {
        mesh.twins[a] = b;
    }
    if (b != kNone) {
        mesh.twins[b] = a;
    }
}

u32 NewTriangle(WorkMesh& mesh, u32 a, u32 b, u32 c) {
    const u32 t = mesh.triangleCount();
    mesh.corners.insert(mesh.corners.end(), {a, b, c});
    mesh.twins.insert(mesh.twins.end(), {kNone, kNone, kNone});
    mesh.features.insert(mesh.features.end(), {0, 0, 0});
    mesh.curves.insert(mesh.curves.end(), {kNone, kNone, kNone});
    mesh.dead.push_back(0);
    return t;
}

/// The neighbours of @p vertex, from its ring: each outgoing halfedge's end,
/// and on a border the start of the last triangle's incoming border edge.
void Neighbours(const WorkMesh& mesh, u32 vertex, std::vector<u32>& ring,
                std::vector<u32>& out) {
    out.clear();
    const bool closed = mesh.ring(vertex, ring);
    for (u32 h : ring) {
        out.push_back(mesh.to(h));
    }
    if (!closed && !ring.empty()) {
        out.push_back(mesh.from(WorkMesh::Prev(ring.back())));
    }
}

} // namespace

bool WorkMesh::ring(u32 vertex, std::vector<u32>& outgoing) const {
    outgoing.clear();
    const u32 start = out[vertex];
    if (start == kNone) {
        return true;
    }
    u32 h = start;
    bool closed = true;
    u32 rewound = 0; // a corrupt fan never loops forever
    for (;;) {
        const u32 twin = twins[h];
        if (twin == kNone) {
            closed = false;
            break;
        }
        const u32 back = Next(twin);
        if (back == start || ++rewound > 4096) {
            break;
        }
        h = back;
    }
    const u32 first = h;
    for (;;) {
        outgoing.push_back(h);
        const u32 twin = twins[Prev(h)];
        if (twin == kNone || twin == first) {
            break;
        }
        h = twin;
        if (outgoing.size() > 4096) {
            break; // a corrupt fan; never loop forever
        }
    }
    return closed;
}

bool WorkMesh::isBorder(u32 vertex) const {
    std::vector<u32> outgoing;
    return !ring(vertex, outgoing);
}

u32 WorkMesh::valence(u32 vertex) const {
    std::vector<u32> outgoing;
    const bool closed = ring(vertex, outgoing);
    return static_cast<u32>(outgoing.size()) + (closed ? 0 : 1);
}

u32 WorkMesh::find(u32 a, u32 b) const {
    std::vector<u32> outgoing;
    ring(a, outgoing);
    for (u32 h : outgoing) {
        if (to(h) == b) {
            return h;
        }
    }
    return kNone;
}

V3 WorkMesh::triangleNormal(u32 t) const {
    return TriangleNormal(positions[corners[3 * t]], positions[corners[3 * t + 1]],
                          positions[corners[3 * t + 2]]);
}

u32 WorkMesh::addVertex(const V3& position, const SurfacePoint& home, VertexKind kind) {
    const u32 v = vertexCount();
    positions.push_back(position);
    homes.push_back(home);
    onCurve.push_back({});
    curve.push_back(kNone);
    kinds.push_back(kind);
    sourceVertex.push_back(kNone);
    out.push_back(kNone);
    return v;
}

void WorkMesh::split(u32 h, u32 m) {
    const u32 a = from(h);
    const u32 b = to(h);
    const u32 h0n = Next(h);
    const u32 c = to(h0n);
    const u32 x1 = twins[h0n];
    const u8 f0n = features[h0n];
    const u32 c0n = curves[h0n];
    const u32 twin = twins[h];

    // t0 = (a, b, c) becomes (a, m, c); (m, b, c) is new.
    corners[h0n] = m;
    const u32 t2 = NewTriangle(*this, m, b, c);
    const u32 g0 = 3 * t2;
    const u32 g1 = 3 * t2 + 1;
    const u32 g2 = 3 * t2 + 2;
    Link(*this, h0n, g2);
    features[h0n] = features[g2] = 0;
    curves[h0n] = curves[g2] = kNone;
    twins[g1] = kNone;
    Link(*this, g1, x1);
    features[g1] = f0n;
    curves[g1] = c0n;
    features[g0] = features[h];
    curves[g0] = curves[h];
    if (out[b] == h0n) {
        out[b] = g1;
    }
    out[m] = g0;

    if (twin == kNone) {
        twins[h] = kNone;
        twins[g0] = kNone;
        return;
    }
    // t1 = (b, a, d) becomes (b, m, d); (m, a, d) is new.
    const u32 h1n = Next(twin);
    const u32 d = to(h1n);
    const u32 y1 = twins[h1n];
    const u8 f1n = features[h1n];
    const u32 c1n = curves[h1n];
    corners[h1n] = m;
    const u32 t3 = NewTriangle(*this, m, a, d);
    const u32 k0 = 3 * t3;
    const u32 k1 = 3 * t3 + 1;
    const u32 k2 = 3 * t3 + 2;
    Link(*this, h1n, k2);
    features[h1n] = features[k2] = 0;
    curves[h1n] = curves[k2] = kNone;
    twins[k1] = kNone;
    Link(*this, k1, y1);
    features[k1] = f1n;
    curves[k1] = c1n;
    features[k0] = features[twin];
    curves[k0] = curves[twin];
    Link(*this, h, k0);
    Link(*this, g0, twin);
    if (out[a] == h1n) {
        out[a] = k1;
    }
}

bool WorkMesh::canCollapse(u32 h) const {
    const u32 a = from(h);
    const u32 b = to(h);
    const u32 twin = twins[h];
    const u32 c = to(Next(h));
    const u32 d = twin != kNone ? to(Next(twin)) : kNone;
    std::vector<u32> ring;
    std::vector<u32> na;
    std::vector<u32> nb;
    Neighbours(*this, a, ring, na);
    const bool aBorder = !this->ring(a, ring);
    Neighbours(*this, b, ring, nb);
    const bool bBorder = !this->ring(b, ring);
    if (twin != kNone && aBorder && bBorder) {
        return false;
    }
    u32 shared = 0;
    for (u32 x : na) {
        if (std::find(nb.begin(), nb.end(), x) != nb.end()) {
            if (x != c && x != d) {
                return false;
            }
            ++shared;
        }
    }
    if (shared != (twin != kNone ? 2u : 1u)) {
        return false;
    }
    // The opposite corners keep a proper fan: three neighbours inside, two on
    // a border.
    for (u32 x : {c, d}) {
        if (x == kNone) {
            continue;
        }
        const u32 minimum = isBorder(x) ? 3u : 4u;
        if (valence(x) < minimum) {
            return false;
        }
    }
    // A collapse may not leave a triangle with no interior (two borders meeting).
    return na.size() + nb.size() > 4;
}

void WorkMesh::collapse(u32 h) {
    const u32 a = from(h);
    const u32 b = to(h);
    const u32 twin = twins[h];
    const u32 h0n = Next(h);
    const u32 h0p = Prev(h);
    const u32 c = to(h0n);
    const u32 x1 = twins[h0n];
    const u32 x2 = twins[h0p];
    const u8 fx = features[h0n] | features[h0p];
    const u32 cx = curves[h0n] != kNone ? curves[h0n] : curves[h0p];

    std::vector<u32> fan;
    ring(a, fan);
    u32 d = kNone;
    u32 y1 = kNone;
    u32 y2 = kNone;
    u8 fy = 0;
    u32 cy = kNone;
    if (twin != kNone) {
        const u32 h1n = Next(twin);
        const u32 h1p = Prev(twin);
        d = to(h1n);
        y1 = twins[h1n];
        y2 = twins[h1p];
        fy = features[h1n] | features[h1p];
        cy = curves[h1n] != kNone ? curves[h1n] : curves[h1p];
        dead[twin / 3] = 1;
    }
    dead[h / 3] = 1;
    for (u32 o : fan) {
        if (!dead[o / 3]) {
            corners[o] = b;
        }
    }
    Link(*this, x1, x2);
    if (x1 != kNone) {
        features[x1] = fx;
        curves[x1] = cx;
    }
    if (x2 != kNone) {
        features[x2] = fx;
        curves[x2] = cx;
    }
    if (twin != kNone) {
        Link(*this, y1, y2);
        if (y1 != kNone) {
            features[y1] = fy;
            curves[y1] = cy;
        }
        if (y2 != kNone) {
            features[y2] = fy;
            curves[y2] = cy;
        }
    }
    out[a] = kNone;
    // Re-point the survivors at a halfedge that still exists.
    auto pick = [&](u32 vertex, std::initializer_list<u32> candidates) {
        if (out[vertex] != kNone && !dead[out[vertex] / 3] && from(out[vertex]) == vertex) {
            return;
        }
        for (u32 candidate : candidates) {
            if (candidate != kNone && !dead[candidate / 3] && from(candidate) == vertex) {
                out[vertex] = candidate;
                return;
            }
        }
        out[vertex] = kNone;
    };
    pick(b, {x2, y2, x1 != kNone ? Next(x1) : kNone, y1 != kNone ? Next(y1) : kNone});
    pick(c, {x1, x2 != kNone ? Next(x2) : kNone});
    if (d != kNone) {
        pick(d, {y1, y2 != kNone ? Next(y2) : kNone});
    }
    if (out[b] == kNone) {
        for (u32 o : fan) {
            if (!dead[o / 3] && from(o) == b) {
                out[b] = o;
                break;
            }
        }
    }
}

void WorkMesh::flip(u32 h) {
    const u32 twin = twins[h];
    const u32 t0 = h / 3;
    const u32 t1 = twin / 3;
    const u32 a = from(h);
    const u32 b = to(h);
    const u32 c = to(Next(h));
    const u32 d = to(Next(twin));
    struct Outer {
        u32 twin;
        u8 feature;
        u32 curve;
    };
    auto save = [&](u32 e) { return Outer{twins[e], features[e], curves[e]}; };
    const Outer bc = save(Next(h));
    const Outer ca = save(Prev(h));
    const Outer ad = save(Next(twin));
    const Outer db = save(Prev(twin));
    auto set = [&](u32 e, u32 from, const Outer& o) {
        corners[e] = from;
        twins[e] = kNone;
        Link(*this, e, o.twin);
        features[e] = o.feature;
        curves[e] = o.curve;
    };
    // (c, a, d) and (d, b, c), the new diagonal d-c / c-d last in each.
    set(3 * t0, c, ca);
    set(3 * t0 + 1, a, ad);
    corners[3 * t0 + 2] = d;
    set(3 * t1, d, db);
    set(3 * t1 + 1, b, bc);
    corners[3 * t1 + 2] = c;
    Link(*this, 3 * t0 + 2, 3 * t1 + 2);
    features[3 * t0 + 2] = features[3 * t1 + 2] = 0;
    curves[3 * t0 + 2] = curves[3 * t1 + 2] = kNone;
    out[a] = 3 * t0 + 1;
    out[b] = 3 * t1 + 1;
    out[c] = 3 * t0;
    out[d] = 3 * t1;
}

std::vector<u32> WorkMesh::compact() {
    std::vector<u32> triangleMap(triangleCount(), kNone);
    u32 keptTriangles = 0;
    for (u32 t = 0; t < triangleCount(); ++t) {
        if (!dead[t]) {
            triangleMap[t] = keptTriangles++;
        }
    }
    std::vector<u32> vertexMap(vertexCount(), kNone);
    std::vector<u8> used(vertexCount(), 0);
    for (u32 t = 0; t < triangleCount(); ++t) {
        if (!dead[t]) {
            for (u32 i = 0; i < 3; ++i) {
                used[corners[3 * t + i]] = 1;
            }
        }
    }
    u32 keptVertices = 0;
    for (u32 v = 0; v < vertexCount(); ++v) {
        if (used[v] && out[v] != kNone) {
            vertexMap[v] = keptVertices++;
        }
    }
    WorkMesh next;
    next.positions.reserve(keptVertices);
    for (u32 v = 0; v < vertexCount(); ++v) {
        if (vertexMap[v] == kNone) {
            continue;
        }
        next.positions.push_back(positions[v]);
        next.homes.push_back(homes[v]);
        next.onCurve.push_back(onCurve[v]);
        next.curve.push_back(curve[v]);
        next.kinds.push_back(kinds[v]);
        next.sourceVertex.push_back(sourceVertex[v]);
        next.out.push_back(kNone);
    }
    for (u32 t = 0; t < triangleCount(); ++t) {
        if (dead[t]) {
            continue;
        }
        for (u32 i = 0; i < 3; ++i) {
            const u32 h = 3 * t + i;
            next.corners.push_back(vertexMap[corners[h]]);
            const u32 twin = twins[h];
            next.twins.push_back(twin == kNone || dead[twin / 3]
                                     ? kNone
                                     : 3 * triangleMap[twin / 3] + twin % 3);
            next.features.push_back(features[h]);
            next.curves.push_back(curves[h]);
        }
        next.dead.push_back(0);
    }
    for (u32 h = 0; h < next.corners.size(); ++h) {
        const u32 v = next.corners[h];
        if (next.out[v] == kNone) {
            next.out[v] = h;
        }
    }
    *this = std::move(next);
    return vertexMap;
}

// ============================================================================
// Seeding
// ============================================================================

WorkMesh SeedWorkMesh(const Surface& surface, u32 component) {
    WorkMesh mesh;
    std::unordered_map<u32, u32> vertexOf;
    std::vector<u32> sourceTriangles;
    for (u32 t = 0; t < surface.triangleCount(); ++t) {
        if (surface.components[t] == component) {
            sourceTriangles.push_back(t);
        }
    }
    std::unordered_map<u32, u32> triangleOf;
    for (u32 t : sourceTriangles) {
        triangleOf[t] = static_cast<u32>(triangleOf.size());
    }
    for (u32 t : sourceTriangles) {
        u32 ids[3];
        for (u32 i = 0; i < 3; ++i) {
            const u32 sv = surface.corners[3 * t + i];
            auto it = vertexOf.find(sv);
            if (it == vertexOf.end()) {
                const VertexKind kind = surface.kinds[sv];
                const u32 v = mesh.addVertex(surface.positions[sv], surface.atVertex(sv), kind);
                mesh.sourceVertex[v] = sv;
                if (kind == VertexKind::Feature) {
                    const u32 h = surface.featureEdges(sv).front();
                    mesh.onCurve[v] = FeaturePoint{h, surface.from(h) == sv ? 0.0 : 1.0};
                    mesh.curve[v] = surface.curves[h];
                    mesh.homes[v] = surface.toSurface(mesh.onCurve[v]);
                }
                it = vertexOf.emplace(sv, v).first;
            }
            ids[i] = it->second;
        }
        NewTriangle(mesh, ids[0], ids[1], ids[2]);
    }
    for (u32 i = 0; i < sourceTriangles.size(); ++i) {
        const u32 t = sourceTriangles[i];
        for (u32 k = 0; k < 3; ++k) {
            const u32 h = 3 * t + k;
            const u32 twin = surface.twins[h];
            mesh.twins[3 * i + k] = twin == kNone ? kNone : 3 * triangleOf[twin / 3] + twin % 3;
            mesh.features[3 * i + k] = surface.features[h];
            mesh.curves[3 * i + k] = surface.curves[h];
        }
    }
    for (u32 h = 0; h < mesh.corners.size(); ++h) {
        if (mesh.out[mesh.corners[h]] == kNone) {
            mesh.out[mesh.corners[h]] = h;
        }
    }
    return mesh;
}

FeaturePoint CurveStart(const WorkMesh& mesh, const Surface& surface, u32 vertex, u32 curve, const V3& toward) {
    if (mesh.kinds[vertex] == VertexKind::Feature && mesh.curve[vertex] == curve) {
        return mesh.onCurve[vertex];
    }
    FeaturePoint best;
    if (mesh.kinds[vertex] == VertexKind::Corner) {
        const u32 sv = mesh.sourceVertex[vertex];
        f64 nearest = std::numeric_limits<f64>::infinity();
        for (u32 h : surface.featureEdges(sv)) {
            if (surface.curves[h] != curve) {
                continue;
            }
            const bool leaves = surface.from(h) == sv;
            const f64 d = Distance(surface.positions[leaves ? surface.to(h) : surface.from(h)], toward);
            if (d < nearest) {
                nearest = d;
                best = FeaturePoint{h, leaves ? 0.0 : 1.0};
            }
        }
    }
    return best;
}

// ============================================================================
// Remeshing
// ============================================================================

namespace {

/// Moves @p vertex to @p target re-homed on the surface, unless that folds a
/// triangle round it. Returns whether it moved.
bool MoveVertex(WorkMesh& mesh, const Surface& surface, u32 vertex, const V3& target,
                std::vector<u32>& ring) {
    const V3 from = mesh.positions[vertex];
    const f64 step = Distance(from, target);
    if (step <= 0.0) {
        return false;
    }
    SurfacePoint home;
    FeaturePoint onCurve;
    if (mesh.kinds[vertex] == VertexKind::Feature) {
        onCurve = surface.locateOnCurve(mesh.onCurve[vertex], target, 2.0 * step);
        home = surface.toSurface(onCurve);
    } else {
        const u32 start = mesh.homes[vertex].triangle;
        home = surface.locate(std::span<const u32>(&start, 1), target, 2.0 * step);
    }
    if (!home.valid()) {
        return false;
    }
    const V3 placed = mesh.kinds[vertex] == VertexKind::Feature ? surface.position(onCurve)
                                                                 : surface.position(home);
    mesh.ring(vertex, ring);
    for (u32 h : ring) {
        const u32 t = h / 3;
        // A sliver has no side to keep: the surface's at the vertex stands in.
        V3 before = mesh.triangleNormal(t);
        if (Length2(before) == 0.0) {
            before = surface.normal(mesh.homes[vertex]);
        }
        const V3 b = mesh.positions[mesh.to(h)];
        const V3 c = mesh.positions[mesh.to(WorkMesh::Next(h))];
        const V3 cross = Cross(b - placed, c - placed);
        if (Dot(cross, before) <= 0.0 || Length(cross) <= 1e-12 * Length2(b - c)) {
            return false;
        }
    }
    mesh.positions[vertex] = placed;
    mesh.homes[vertex] = home;
    if (mesh.kinds[vertex] == VertexKind::Feature) {
        mesh.onCurve[vertex] = onCurve;
    }
    return true;
}

/// The vertex a collapse of @p h's edge removes, as a halfedge leaving it, or
/// kNone when neither end may go (§1.4's feature rules).
u32 CollapseDirection(const WorkMesh& mesh, u32 h) {
    const u32 a = mesh.from(h);
    const u32 b = mesh.to(h);
    const VertexKind ka = mesh.kinds[a];
    const VertexKind kb = mesh.kinds[b];
    const bool alongFeature = mesh.isFeature(h);
    auto mayRemove = [&](VertexKind kind, u32 vertex, u32 other) {
        if (kind == VertexKind::Corner) {
            return false;
        }
        if (kind == VertexKind::Feature) {
            // Only along its own curve, onto a vertex of that curve.
            return alongFeature && mesh.curves[h] == mesh.curve[vertex] &&
                   (mesh.kinds[other] == VertexKind::Corner || mesh.curve[other] == mesh.curve[vertex]);
        }
        return true;
    };
    if (!alongFeature && ka != VertexKind::Free && kb != VertexKind::Free) {
        return kNone; // a chord between two features: neither may slide off
    }
    const bool removeA = mayRemove(ka, a, b);
    const bool removeB = mayRemove(kb, b, a);
    if (removeA && (!removeB || static_cast<u8>(ka) <= static_cast<u8>(kb))) {
        return h;
    }
    if (removeB) {
        return mesh.twins[h];
    }
    return kNone;
}

bool CollapseKeepsShape(const WorkMesh& mesh, u32 h, f64 high, std::vector<u32>& ring) {
    const u32 a = mesh.from(h);
    const u32 b = mesh.to(h);
    const V3 target = mesh.positions[b];
    mesh.ring(a, ring);
    for (u32 o : ring) {
        const u32 x = mesh.to(o);
        if (x == b) {
            continue;
        }
        if (Distance(target, mesh.positions[x]) > high) {
            return false;
        }
        const u32 y = mesh.to(WorkMesh::Next(o));
        if (y == b) {
            continue; // one of the two triangles the collapse removes
        }
        // A sliver has no side to keep: the collapsing edge's triangle's.
        V3 before = mesh.triangleNormal(o / 3);
        if (Length2(before) == 0.0) {
            before = mesh.triangleNormal(h / 3);
        }
        const V3 cross = Cross(mesh.positions[x] - target, mesh.positions[y] - target);
        const f64 length = Length(cross);
        if (length <= 1e-12 || Dot(cross, before) < 0.2 * length) {
            return false;
        }
    }
    // On a border, the last neighbour has no outgoing halfedge from a.
    if (!ring.empty()) {
        const u32 last = mesh.from(WorkMesh::Prev(ring.back()));
        if (last != b && mesh.twins[WorkMesh::Prev(ring.back())] == kNone &&
            Distance(target, mesh.positions[last]) > high) {
            return false;
        }
    }
    return true;
}

} // namespace

u32 SplitLong(WorkMesh& mesh, const Surface& surface, f64 high) {
    u32 total = 0;
    std::vector<std::pair<f64, u64>> candidates;
    // Longest first, by vertex pair: a halfedge's index names another edge once
    // its triangle has been split, and bisecting a short edge before the long
    // one beside it breeds slivers that never stop splitting. A coarse game mesh
    // needs several rounds before nothing is long.
    for (u32 round = 0; round < 32; ++round) {
        candidates.clear();
        for (u32 h = 0; h < mesh.corners.size(); ++h) {
            const u32 twin = mesh.twins[h];
            if (mesh.dead[h / 3] || (twin != kNone && twin < h)) {
                continue;
            }
            const f64 length = Distance(mesh.positions[mesh.from(h)], mesh.positions[mesh.to(h)]);
            if (length > high) {
                candidates.push_back({length, (static_cast<u64>(mesh.from(h)) << 32) | mesh.to(h)});
            }
        }
        if (candidates.empty()) {
            break;
        }
        std::sort(candidates.begin(), candidates.end(),
                  [](const auto& x, const auto& y) { return x.first > y.first; });
        for (const auto& [length, key] : candidates) {
            const u32 a = static_cast<u32>(key >> 32);
            const u32 b = static_cast<u32>(key & 0xFFFFFFFFu);
            u32 h = mesh.find(a, b);
            if (h == kNone) {
                h = mesh.find(b, a);
            }
            if (h == kNone) {
                continue;
            }
            const V3 middle = Lerp(mesh.positions[a], mesh.positions[b], 0.5);
            u32 m = kNone;
            if (mesh.isFeature(h)) {
                const u32 curve = mesh.curves[h];
                FeaturePoint start = CurveStart(mesh, surface, a, curve, middle);
                if (!start.valid()) {
                    start = CurveStart(mesh, surface, b, curve, middle);
                }
                if (!start.valid()) {
                    continue;
                }
                const FeaturePoint at = surface.locateOnCurve(start, middle, length);
                m = mesh.addVertex(surface.position(at), surface.toSurface(at), VertexKind::Feature);
                mesh.onCurve[m] = at;
                mesh.curve[m] = curve;
            } else {
                const u32 start[2] = {mesh.homes[a].triangle, mesh.homes[b].triangle};
                const SurfacePoint home = surface.locate(start, middle, length);
                if (!home.valid()) {
                    continue;
                }
                m = mesh.addVertex(surface.position(home), home, VertexKind::Free);
            }
            mesh.split(h, m);
            ++total;
        }
    }
    return total;
}

u32 CollapseShort(WorkMesh& mesh, f64 low, f64 high) {
    u32 collapses = 0;
    std::vector<u32> ring;
    std::vector<std::pair<f64, u32>> shortEdges;
    for (u32 h = 0; h < mesh.corners.size(); ++h) {
        const u32 twin = mesh.twins[h];
        if (mesh.dead[h / 3] || (twin != kNone && twin < h)) {
            continue;
        }
        const f64 length = Distance(mesh.positions[mesh.from(h)], mesh.positions[mesh.to(h)]);
        if (length < low) {
            shortEdges.push_back({length, h});
        }
    }
    std::sort(shortEdges.begin(), shortEdges.end());
    for (const auto& [length, edge] : shortEdges) {
        if (mesh.dead[edge / 3]) {
            continue;
        }
        const f64 now = Distance(mesh.positions[mesh.from(edge)], mesh.positions[mesh.to(edge)]);
        if (now >= low) {
            continue;
        }
        const u32 h = CollapseDirection(mesh, edge);
        if (h == kNone || !mesh.canCollapse(h) || !CollapseKeepsShape(mesh, h, high, ring)) {
            continue;
        }
        mesh.collapse(h);
        ++collapses;
    }
    return collapses;
}

u32 FlipToValence(WorkMesh& mesh) {
    u32 flips = 0;
    for (u32 h = 0; h < mesh.corners.size(); ++h) {
        const u32 twin = mesh.twins[h];
        if (mesh.dead[h / 3] || twin == kNone || twin < h || mesh.isFeature(h)) {
            continue;
        }
        const u32 a = mesh.from(h);
        const u32 b = mesh.to(h);
        const u32 c = mesh.to(WorkMesh::Next(h));
        const u32 d = mesh.to(WorkMesh::Next(twin));
        if (c == d || mesh.find(c, d) != kNone || mesh.find(d, c) != kNone) {
            continue;
        }
        auto deviation = [&](u32 v, i32 delta) {
            const i32 target = mesh.isBorder(v) ? 4 : 6;
            return std::abs(static_cast<i32>(mesh.valence(v)) + delta - target);
        };
        const i32 before = deviation(a, 0) + deviation(b, 0) + deviation(c, 0) + deviation(d, 0);
        const i32 after = deviation(a, -1) + deviation(b, -1) + deviation(c, 1) + deviation(d, 1);
        if (after >= before) {
            continue;
        }
        // A sliver has no crease to keep: its neighbour's normal stands for
        // both, so the flip that clears it is not refused.
        V3 n0 = mesh.triangleNormal(h / 3);
        V3 n1 = mesh.triangleNormal(twin / 3);
        n0 = Length2(n0) > 0.0 ? n0 : n1;
        n1 = Length2(n1) > 0.0 ? n1 : n0;
        if (Length2(n0) == 0.0 || Dot(n0, n1) < std::cos(30.0 * kPi / 180.0)) {
            continue; // across a crease the flip would change the shape
        }
        const V3& pa = mesh.positions[a];
        const V3& pb = mesh.positions[b];
        const V3& pc = mesh.positions[c];
        const V3& pd = mesh.positions[d];
        const V3 average = Unit(n0 + n1);
        const V3 m0 = Cross(pa - pc, pd - pc);
        const V3 m1 = Cross(pb - pd, pc - pd);
        if (Dot(m0, average) <= 0.1 * Length(m0) || Dot(m1, average) <= 0.1 * Length(m1)) {
            continue; // not convex: the new diagonal would leave the quad
        }
        if (mesh.valence(a) <= 3 || mesh.valence(b) <= 3) {
            continue;
        }
        mesh.flip(h);
        ++flips;
    }
    return flips;
}

void Relax(WorkMesh& mesh, const Surface& surface, f64 relaxation) {
    std::vector<u32> ring;
    std::vector<u32> neighbours;
    for (u32 v = 0; v < mesh.vertexCount(); ++v) {
        if (!mesh.alive(v) || mesh.kinds[v] == VertexKind::Corner) {
            continue;
        }
        V3 target = mesh.positions[v];
        if (mesh.kinds[v] == VertexKind::Feature) {
            mesh.ring(v, ring);
            V3 sum{0.0, 0.0, 0.0};
            u32 count = 0;
            for (u32 o : ring) {
                if (mesh.isFeature(o) && mesh.curves[o] == mesh.curve[v]) {
                    sum = sum + mesh.positions[mesh.to(o)];
                    ++count;
                }
                const u32 in = WorkMesh::Prev(o);
                if (mesh.twins[in] == kNone && mesh.isFeature(in) &&
                    mesh.curves[in] == mesh.curve[v]) {
                    sum = sum + mesh.positions[mesh.from(in)];
                    ++count;
                }
            }
            if (count != 2) {
                continue;
            }
            target = sum * 0.5;
        } else {
            Neighbours(mesh, v, ring, neighbours);
            if (neighbours.empty()) {
                continue;
            }
            V3 sum{0.0, 0.0, 0.0};
            for (u32 n : neighbours) {
                sum = sum + mesh.positions[n];
            }
            const V3 centroid = sum * (1.0 / static_cast<f64>(neighbours.size()));
            const V3 normal = surface.normal(mesh.homes[v]);
            target = mesh.positions[v] + Tangent(centroid - mesh.positions[v], normal);
        }
        const V3 step = (target - mesh.positions[v]) * relaxation;
        MoveVertex(mesh, surface, v, mesh.positions[v] + step, ring);
    }
}

RemeshStats Remesh(WorkMesh& mesh, const Surface& surface, const RemeshOptions& options) {
    RemeshStats stats;
    const f64 high = options.targetLength * 4.0 / 3.0;
    const f64 low = options.targetLength * 4.0 / 5.0;
    for (u32 iteration = 0; iteration < options.iterations; ++iteration) {
        stats.splits += SplitLong(mesh, surface, high);
        stats.collapses += CollapseShort(mesh, low, high);
        stats.flips += FlipToValence(mesh);
        Relax(mesh, surface, options.relaxation);
        mesh.compact();
    }
    return stats;
}

u32 SplitFeatureCorners(WorkMesh& mesh, const Surface& surface) {
    u32 count = 0;
    const u32 triangles = mesh.triangleCount();
    for (u32 t = 0; t < triangles; ++t) {
        if (mesh.dead[t]) {
            continue;
        }
        u32 featureEdges = 0;
        for (u32 i = 0; i < 3; ++i) {
            featureEdges += mesh.isFeature(3 * t + i) ? 1 : 0;
        }
        if (featureEdges < 2) {
            continue;
        }
        const u32 a = mesh.corners[3 * t];
        const u32 b = mesh.corners[3 * t + 1];
        const u32 c = mesh.corners[3 * t + 2];
        const V3 centroid = (mesh.positions[a] + mesh.positions[b] + mesh.positions[c]) * (1.0 / 3.0);
        const u32 start[3] = {mesh.homes[a].triangle, mesh.homes[b].triangle, mesh.homes[c].triangle};
        const f64 reach = std::max({Distance(centroid, mesh.positions[a]),
                                    Distance(centroid, mesh.positions[b]),
                                    Distance(centroid, mesh.positions[c])});
        const SurfacePoint home = surface.locate(start, centroid, reach);
        if (!home.valid()) {
            continue;
        }
        const u32 m = mesh.addVertex(surface.position(home), home, VertexKind::Free);
        // (a, b, c) becomes (a, b, m), (b, c, m), (c, a, m).
        struct Outer {
            u32 twin;
            u8 feature;
            u32 curve;
        };
        Outer edges[3];
        for (u32 i = 0; i < 3; ++i) {
            edges[i] = {mesh.twins[3 * t + i], mesh.features[3 * t + i], mesh.curves[3 * t + i]};
        }
        const u32 ids[3] = {a, b, c};
        mesh.corners[3 * t + 2] = m;
        const u32 t1 = NewTriangle(mesh, b, c, m);
        const u32 t2 = NewTriangle(mesh, c, a, m);
        const u32 tris[3] = {t, t1, t2};
        for (u32 i = 0; i < 3; ++i) {
            const u32 e = 3 * tris[i];
            mesh.corners[e] = ids[i];
            mesh.corners[e + 1] = ids[(i + 1) % 3];
            mesh.corners[e + 2] = m;
            mesh.twins[e] = kNone;
            Link(mesh, e, edges[i].twin);
            mesh.features[e] = edges[i].feature;
            mesh.curves[e] = edges[i].curve;
            mesh.features[e + 1] = mesh.features[e + 2] = 0;
            mesh.curves[e + 1] = mesh.curves[e + 2] = kNone;
        }
        for (u32 i = 0; i < 3; ++i) {
            // (x, y, m): its y->m meets the next triangle's m->y.
            Link(mesh, 3 * tris[i] + 1, 3 * tris[(i + 1) % 3] + 2);
            mesh.out[ids[i]] = 3 * tris[i];
        }
        mesh.out[m] = 3 * tris[0] + 2;
        ++count;
    }
    return count;
}

std::string Validate(const WorkMesh& mesh) {
    for (u32 h = 0; h < mesh.corners.size(); ++h) {
        if (mesh.dead[h / 3]) {
            continue;
        }
        const u32 t = h / 3;
        if (mesh.corners[3 * t] == mesh.corners[3 * t + 1] || mesh.corners[3 * t + 1] == mesh.corners[3 * t + 2] ||
            mesh.corners[3 * t] == mesh.corners[3 * t + 2]) {
            return "triangle " + std::to_string(t) + " repeats a vertex";
        }
        if (!mesh.alive(mesh.from(h))) {
            return "halfedge " + std::to_string(h) + " leaves a dead vertex";
        }
        const u32 twin = mesh.twins[h];
        if (twin == kNone) {
            continue;
        }
        if (mesh.dead[twin / 3]) {
            return "halfedge " + std::to_string(h) + " twins a dead triangle";
        }
        if (mesh.twins[twin] != h || mesh.from(twin) != mesh.to(h) || mesh.to(twin) != mesh.from(h)) {
            return "halfedge " + std::to_string(h) + " and its twin disagree";
        }
        if (mesh.features[twin] != mesh.features[h] || mesh.curves[twin] != mesh.curves[h]) {
            return "halfedge " + std::to_string(h) + " and its twin differ in feature";
        }
    }
    for (u32 v = 0; v < mesh.vertexCount(); ++v) {
        const u32 o = mesh.out[v];
        if (o == kNone) {
            continue;
        }
        if (o >= mesh.corners.size() || mesh.dead[o / 3] || mesh.from(o) != v) {
            return "vertex " + std::to_string(v) + " points at a halfedge not leaving it";
        }
    }
    // Every directed edge once, and every vertex's triangles one fan.
    std::unordered_map<u64, u32> directed;
    std::vector<u32> fanSize(mesh.vertexCount(), 0);
    for (u32 h = 0; h < mesh.corners.size(); ++h) {
        if (mesh.dead[h / 3]) {
            continue;
        }
        const u64 key = (static_cast<u64>(mesh.from(h)) << 32) | mesh.to(h);
        if (!directed.emplace(key, h).second) {
            return "edge " + std::to_string(mesh.from(h)) + "->" + std::to_string(mesh.to(h)) + " twice";
        }
        ++fanSize[mesh.from(h)];
    }
    std::vector<u32> ring;
    for (u32 v = 0; v < mesh.vertexCount(); ++v) {
        if (!mesh.alive(v)) {
            continue;
        }
        mesh.ring(v, ring);
        if (ring.size() != fanSize[v]) {
            return "vertex " + std::to_string(v) + " has more than one fan";
        }
    }
    return {};
}

bool HomesConsistent(const WorkMesh& mesh, const Surface& surface, f64 tolerance) {
    for (u32 v = 0; v < mesh.vertexCount(); ++v) {
        if (!mesh.alive(v) || !mesh.homes[v].valid()) {
            return false;
        }
        if (Distance(surface.position(mesh.homes[v]), mesh.positions[v]) > tolerance) {
            return false;
        }
        if (mesh.kinds[v] == VertexKind::Feature) {
            if (!mesh.onCurve[v].valid() || surface.curves[mesh.onCurve[v].halfedge] != mesh.curve[v] ||
                Distance(surface.position(mesh.onCurve[v]), mesh.positions[v]) > tolerance) {
                return false;
            }
        }
    }
    return true;
}

} // namespace retopo
} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
