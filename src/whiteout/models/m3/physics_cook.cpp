// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/m3/physics_cook.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <vector>

// The mesh tree's values have to match the client's float arithmetic bit for
// bit, so no multiply-add may be fused.
#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(_MSC_VER)
#pragma fp_contract(off)
#endif

namespace whiteout {
namespace m3 {
namespace {

// ============================================================================
// Double-precision geometry for the hull
// ============================================================================

struct D3 {
    double x = 0, y = 0, z = 0;
};

D3 operator+(const D3& a, const D3& b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
D3 operator-(const D3& a, const D3& b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
D3 operator*(const D3& a, double s) {
    return {a.x * s, a.y * s, a.z * s};
}
double Dot(const D3& a, const D3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
D3 Cross(const D3& a, const D3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
double Length(const D3& a) {
    return std::sqrt(Dot(a, a));
}
D3 Normalized(const D3& a) {
    const double l = Length(a);
    return l > 0 ? a * (1.0 / l) : D3{};
}

u64 EdgeKey(u32 a, u32 b) {
    return (static_cast<u64>(a) << 32) | b;
}

/// A convex polytope: vertex indices into the caller's points, and each face
/// as a loop wound counter-clockwise about its outward normal.
struct Polytope {
    std::vector<u32> vertices;             ///< Point indices, ascending
    std::vector<std::vector<u32>> faces;   ///< Loops of point indices
};

/// The points' convex hull as outward triangles, incremental, in double.
/// False when the points span no volume or the topology came out inconsistent.
bool TriangleHull(const std::vector<D3>& p, double eps, std::vector<std::array<u32, 3>>& out) {
    const u32 n = static_cast<u32>(p.size());
    out.clear();
    if (n < 4) {
        return false;
    }
    // The initial tetrahedron: the extremes of the widest axis, the point
    // farthest from their line, and the point farthest from that plane.
    D3 lo = p[0], hi = p[0];
    std::array<u32, 3> loIdx{0, 0, 0}, hiIdx{0, 0, 0};
    for (u32 i = 1; i < n; ++i) {
        const std::array<double, 3> c{p[i].x, p[i].y, p[i].z};
        std::array<double*, 3> l{&lo.x, &lo.y, &lo.z}, h{&hi.x, &hi.y, &hi.z};
        for (int a = 0; a < 3; ++a) {
            if (c[a] < *l[a]) {
                *l[a] = c[a];
                loIdx[a] = i;
            }
            if (c[a] > *h[a]) {
                *h[a] = c[a];
                hiIdx[a] = i;
            }
        }
    }
    const D3 span = hi - lo;
    int axis = 0;
    if (span.y > span.x && span.y >= span.z) {
        axis = 1;
    } else if (span.z > span.x && span.z > span.y) {
        axis = 2;
    }
    const u32 i0 = loIdx[axis];
    const u32 i1 = hiIdx[axis];
    if (i0 == i1) {
        return false;
    }
    const D3 dir = Normalized(p[i1] - p[i0]);
    u32 i2 = i0;
    double best = 0;
    for (u32 i = 0; i < n; ++i) {
        const D3 d = p[i] - p[i0];
        const double dist = Length(d - dir * Dot(d, dir));
        if (dist > best) {
            best = dist;
            i2 = i;
        }
    }
    if (best <= eps) {
        return false;
    }
    const D3 normal = Normalized(Cross(p[i1] - p[i0], p[i2] - p[i0]));
    u32 i3 = i0;
    best = 0;
    for (u32 i = 0; i < n; ++i) {
        const double dist = std::abs(Dot(p[i] - p[i0], normal));
        if (dist > best) {
            best = dist;
            i3 = i;
        }
    }
    if (best <= eps) {
        return false;
    }

    struct Tri {
        std::array<u32, 3> v;
        D3 n;
        double d = 0;
        bool alive = true;
    };
    std::vector<Tri> tris;
    std::unordered_map<u64, u32> edgeFace;
    bool consistent = true;
    auto addTri = [&](u32 a, u32 b, u32 c) {
        Tri t;
        t.v = {a, b, c};
        t.n = Normalized(Cross(p[b] - p[a], p[c] - p[a]));
        t.d = Dot(t.n, p[a]);
        const u32 index = static_cast<u32>(tris.size());
        tris.push_back(t);
        for (int e = 0; e < 3; ++e) {
            if (!edgeFace.emplace(EdgeKey(t.v[e], t.v[(e + 1) % 3]), index).second) {
                consistent = false;
            }
        }
    };
    u32 a = i0, b = i1, c = i2;
    if (Dot(Cross(p[b] - p[a], p[c] - p[a]), p[i3] - p[a]) > 0) {
        std::swap(b, c);
    }
    addTri(a, b, c);
    addTri(a, i3, b);
    addTri(b, i3, c);
    addTri(c, i3, a);

    std::vector<u32> visible;
    std::vector<std::array<u32, 2>> horizon;
    std::vector<u8> isVisible;
    for (u32 i = 0; i < n && consistent; ++i) {
        if (i == i0 || i == i1 || i == i2 || i == i3) {
            continue;
        }
        visible.clear();
        isVisible.assign(tris.size(), 0);
        for (u32 t = 0; t < tris.size(); ++t) {
            if (tris[t].alive && Dot(tris[t].n, p[i]) - tris[t].d > eps) {
                visible.push_back(t);
                isVisible[t] = 1;
            }
        }
        if (visible.empty()) {
            continue;
        }
        horizon.clear();
        for (const u32 t : visible) {
            for (int e = 0; e < 3; ++e) {
                const u32 ea = tris[t].v[e];
                const u32 eb = tris[t].v[(e + 1) % 3];
                const auto twin = edgeFace.find(EdgeKey(eb, ea));
                if (twin == edgeFace.end()) {
                    consistent = false;
                    break;
                }
                if (!isVisible[twin->second]) {
                    horizon.push_back({ea, eb});
                }
            }
        }
        for (const u32 t : visible) {
            tris[t].alive = false;
            for (int e = 0; e < 3; ++e) {
                edgeFace.erase(EdgeKey(tris[t].v[e], tris[t].v[(e + 1) % 3]));
            }
        }
        for (const auto& h : horizon) {
            addTri(h[0], h[1], i);
        }
    }
    if (!consistent) {
        return false;
    }
    for (const Tri& t : tris) {
        if (t.alive) {
            out.push_back(t.v);
        }
    }
    return out.size() >= 4;
}

/// Merges coplanar hull triangles into polygons and drops the vertices that
/// end up inside a face or on a straight edge.
bool MergeFaces(const std::vector<D3>& p, const std::vector<std::array<u32, 3>>& tris, double tol,
                Polytope& out) {
    const std::size_t count = tris.size();
    std::vector<D3> normal(count);
    std::vector<double> area(count);
    std::unordered_map<u64, u32> edgeTri;
    for (std::size_t t = 0; t < count; ++t) {
        const D3 c = Cross(p[tris[t][1]] - p[tris[t][0]], p[tris[t][2]] - p[tris[t][0]]);
        area[t] = Length(c) * 0.5;
        normal[t] = Normalized(c);
        for (int e = 0; e < 3; ++e) {
            edgeTri.emplace(EdgeKey(tris[t][e], tris[t][(e + 1) % 3]), static_cast<u32>(t));
        }
    }
    // Faces are groups of triangles. Two neighbouring groups merge while the
    // edge between them is not clearly convex: either one's centroid lies
    // less than `tol` below the other's plane. That is the quickhull rule the
    // client's cooker uses; a distance test on the vertices merges or splits
    // the few shipped faces a hair off planar the other way.
    std::vector<u32> root(count);
    std::vector<D3> weighted(count), moment(count);
    std::vector<double> total(count);
    for (u32 t = 0; t < count; ++t) {
        root[t] = t;
        weighted[t] = normal[t] * area[t];
        const D3 centroid = (p[tris[t][0]] + p[tris[t][1]] + p[tris[t][2]]) * (1.0 / 3.0);
        moment[t] = centroid * area[t];
        total[t] = area[t];
    }
    auto find = [&](u32 t) {
        while (root[t] != t) {
            root[t] = root[root[t]];
            t = root[t];
        }
        return t;
    };
    for (bool changed = true; changed;) {
        changed = false;
        for (u32 t = 0; t < count; ++t) {
            for (int e = 0; e < 3; ++e) {
                const auto twin = edgeTri.find(EdgeKey(tris[t][(e + 1) % 3], tris[t][e]));
                if (twin == edgeTri.end()) {
                    continue;
                }
                const u32 a = find(t);
                const u32 b = find(twin->second);
                if (a == b || !(total[a] > 0) || !(total[b] > 0)) {
                    continue;
                }
                const D3 na = Normalized(weighted[a]), nb = Normalized(weighted[b]);
                const D3 ca = moment[a] * (1.0 / total[a]), cb = moment[b] * (1.0 / total[b]);
                if (Dot(nb, ca - cb) > -tol || Dot(na, cb - ca) > -tol) {
                    root[b] = a;
                    weighted[a] = weighted[a] + weighted[b];
                    moment[a] = moment[a] + moment[b];
                    total[a] += total[b];
                    changed = true;
                }
            }
        }
    }
    std::vector<i32> faceOf(count, -1);
    std::vector<std::vector<u32>> members;
    std::unordered_map<u32, i32> faceOfRoot;
    for (u32 t = 0; t < count; ++t) {
        const u32 r = find(t);
        const auto [it, fresh] = faceOfRoot.emplace(r, static_cast<i32>(members.size()));
        if (fresh) {
            members.emplace_back();
        }
        faceOf[t] = it->second;
        members[static_cast<std::size_t>(it->second)].push_back(t);
    }

    // Each face's boundary: the directed edges whose twin lies in another face.
    std::vector<std::vector<u32>> loops;
    for (std::size_t f = 0; f < members.size(); ++f) {
        std::unordered_map<u32, u32> next;
        u32 start = 0;
        for (const u32 t : members[f]) {
            for (int e = 0; e < 3; ++e) {
                const u32 ea = tris[t][e];
                const u32 eb = tris[t][(e + 1) % 3];
                const auto twin = edgeTri.find(EdgeKey(eb, ea));
                if (twin != edgeTri.end() && faceOf[twin->second] == static_cast<i32>(f)) {
                    continue;
                }
                if (!next.emplace(ea, eb).second) {
                    return false; // the face touches itself at a vertex
                }
                start = ea;
            }
        }
        std::vector<u32> loop;
        u32 v = start;
        do {
            loop.push_back(v);
            const auto it = next.find(v);
            if (it == next.end() || loop.size() > next.size()) {
                return false;
            }
            v = it->second;
        } while (v != start);
        if (loop.size() != next.size() || loop.size() < 3) {
            return false; // a face with two boundaries
        }
        loops.push_back(std::move(loop));
    }

    // A vertex only two faces meet at lies on their shared edge: drop it.
    std::unordered_map<u32, u32> facesAt;
    for (const auto& loop : loops) {
        for (const u32 v : loop) {
            ++facesAt[v];
        }
    }
    for (auto& loop : loops) {
        std::erase_if(loop, [&](u32 v) { return facesAt[v] < 3; });
        if (loop.size() < 3) {
            return false;
        }
    }

    out.faces = std::move(loops);
    out.vertices.clear();
    for (const auto& [v, faces] : facesAt) {
        if (faces >= 3) {
            out.vertices.push_back(v);
        }
    }
    std::sort(out.vertices.begin(), out.vertices.end());
    std::size_t edges = 0;
    for (const auto& loop : out.faces) {
        edges += loop.size();
    }
    // Euler: a closed genus-0 polytope has V - E + F = 2.
    return out.vertices.size() + out.faces.size() == edges / 2 + 2 && edges % 2 == 0;
}

/// @p p in a frame of unit size about its middle, so both hull tolerances are
/// relative to the hull's size, and the tolerance faces merge within there.
/// False when the points have no extent.
bool UnitFrame(const std::vector<D3>& p, std::vector<D3>& unit, double& mergeTolerance) {
    if (p.empty()) {
        return false;
    }
    D3 lo = p[0], hi = p[0];
    for (const D3& q : p) {
        lo = {std::min(lo.x, q.x), std::min(lo.y, q.y), std::min(lo.z, q.z)};
        hi = {std::max(hi.x, q.x), std::max(hi.y, q.y), std::max(hi.z, q.z)};
    }
    const D3 center = (lo + hi) * 0.5;
    const double radius = Length(hi - lo) * 0.5;
    if (!(radius > 0)) {
        return false;
    }
    unit.resize(p.size());
    for (std::size_t i = 0; i < p.size(); ++i) {
        unit[i] = (p[i] - center) * (1.0 / radius);
    }
    D3 extreme;
    for (const D3& q : unit) {
        extreme = {std::max(extreme.x, std::abs(q.x)), std::max(extreme.y, std::abs(q.y)),
                   std::max(extreme.z, std::abs(q.z))};
    }
    mergeTolerance = 3.0 * std::numeric_limits<f32>::epsilon() * (extreme.x + extreme.y + extreme.z);
    return true;
}

/// The exact hull of @p p, merged into polygons.
bool ExactHull(const std::vector<D3>& p, Polytope& out) {
    std::vector<D3> unit;
    double mergeTolerance = 0;
    if (!UnitFrame(p, unit, mergeTolerance)) {
        return false;
    }
    std::vector<std::array<u32, 3>> tris;
    for (double eps = 1e-10; eps < 1e-5; eps *= 10) {
        if (TriangleHull(unit, eps, tris) && MergeFaces(unit, tris, mergeTolerance, out)) {
            return true;
        }
    }
    return false;
}

/// Merges the most parallel faces until no two are within @p cosAngle and at
/// most @p maxFaces remain, then rebuilds the polytope from the merged planes
/// (`SimpleHull_SimplifyFaces`). False leaves @p hull alone.
bool SimplifyFaces(std::vector<D3>& points, Polytope& hull, double cosAngle, std::size_t maxFaces) {
    struct Cluster {
        D3 weighted; ///< Sum of member faces' normal * area
        D3 normal;
    };
    std::vector<Cluster> clusters;
    for (const auto& loop : hull.faces) {
        D3 newell;
        for (std::size_t i = 0; i < loop.size(); ++i) {
            newell = newell + Cross(points[loop[i]], points[loop[(i + 1) % loop.size()]]);
        }
        clusters.push_back({newell * 0.5, Normalized(newell)});
    }
    while (clusters.size() >= 2) {
        double bestDot = -2;
        std::size_t bi = 0, bj = 1;
        for (std::size_t i = 0; i < clusters.size(); ++i) {
            for (std::size_t j = i + 1; j < clusters.size(); ++j) {
                const double d = Dot(clusters[i].normal, clusters[j].normal);
                if (d > bestDot) {
                    bestDot = d;
                    bi = i;
                    bj = j;
                }
            }
        }
        if (clusters.size() <= maxFaces && bestDot < cosAngle) {
            break;
        }
        clusters[bi].weighted = clusters[bi].weighted + clusters[bj].weighted;
        clusters[bi].normal = Normalized(clusters[bi].weighted);
        clusters.erase(clusters.begin() + static_cast<std::ptrdiff_t>(bj));
    }
    if (clusters.size() < 4) {
        return false;
    }
    // Planes about the vertex average, then the dual hull: each dual face is
    // a primal vertex.
    D3 center;
    for (const u32 v : hull.vertices) {
        center = center + points[v];
    }
    center = center * (1.0 / static_cast<double>(hull.vertices.size()));
    std::vector<D3> dual;
    for (const Cluster& c : clusters) {
        double offset = -std::numeric_limits<double>::max();
        for (const u32 v : hull.vertices) {
            offset = std::max(offset, Dot(points[v] - center, c.normal));
        }
        if (!(offset > 1e-9)) {
            return false;
        }
        dual.push_back(c.normal * (1.0 / offset));
    }
    Polytope dualHull;
    if (!ExactHull(dual, dualHull)) {
        return false;
    }
    std::vector<D3> primal;
    for (const auto& loop : dualHull.faces) {
        D3 newell, mean;
        for (std::size_t i = 0; i < loop.size(); ++i) {
            newell = newell + Cross(dual[loop[i]], dual[loop[(i + 1) % loop.size()]]);
            mean = mean + dual[loop[i]];
        }
        const D3 n = Normalized(newell);
        const double e = Dot(n, mean * (1.0 / static_cast<double>(loop.size())));
        if (!(e > 1e-12)) {
            return false;
        }
        primal.push_back(center + n * (1.0 / e));
    }
    Polytope rebuilt;
    if (!ExactHull(primal, rebuilt)) {
        return false;
    }
    points = std::move(primal);
    hull = std::move(rebuilt);
    return true;
}

std::size_t HalfEdgeCount(const Polytope& hull) {
    std::size_t edges = 0;
    for (const auto& loop : hull.faces) {
        edges += loop.size();
    }
    return edges;
}

bool WithinLimits(const Polytope& hull) {
    return hull.vertices.size() <= kMaxHullVertices && hull.faces.size() <= kMaxHullFaces &&
           HalfEdgeCount(hull) <= kMaxHullHalfEdges;
}

void ClearHull(PhysicsShape& shape) {
    shape.hullVertices.clear();
    shape.hullPlanes.clear();
    shape.hullHalfEdges.clear();
    shape.hullFaceFirstEdges.clear();
    shape.hullCentroid = {};
    shape.hullVertexCount = 0;
    shape.hullFaceCount = 0;
    shape.hullHalfEdgeCount = 0;
    shape.hullVolume = 0.0f;
    shape.hullSurfaceArea = 0.0f;
}

/// Writes @p hull into @p shape's tables. @p exact holds the float value of
/// each point that came straight from the input, so those stay bit-exact.
void WriteHull(PhysicsShape& shape, const std::vector<D3>& points, const Polytope& hull,
               const std::vector<Vector3f>* exact) {
    ClearHull(shape);
    std::unordered_map<u32, u8> local;
    for (const u32 v : hull.vertices) {
        local.emplace(v, static_cast<u8>(shape.hullVertices.size()));
        if (exact != nullptr) {
            shape.hullVertices.push_back((*exact)[v]);
        } else {
            const D3& q = points[v];
            shape.hullVertices.push_back(
                {static_cast<f32>(q.x), static_cast<f32>(q.y), static_cast<f32>(q.z)});
        }
    }
    auto at = [&](u32 v) {
        const Vector3f& f = shape.hullVertices[local.at(v)];
        return D3{f.x, f.y, f.z};
    };

    // Half-edges in twin pairs: the first face to use an edge takes the even
    // entry, its neighbour the odd one.
    std::unordered_map<u64, u32> pairOf;
    std::vector<std::array<u32, 3>> edges; // origin, face, next (filled below)
    std::vector<std::vector<u32>> faceEdges(hull.faces.size());
    for (std::size_t f = 0; f < hull.faces.size(); ++f) {
        const auto& loop = hull.faces[f];
        for (std::size_t i = 0; i < loop.size(); ++i) {
            const u32 a = loop[i];
            const u32 b = loop[(i + 1) % loop.size()];
            u32 index;
            const auto twin = pairOf.find(EdgeKey(b, a));
            if (twin != pairOf.end()) {
                index = twin->second + 1;
            } else {
                index = static_cast<u32>(edges.size());
                pairOf.emplace(EdgeKey(a, b), index);
                edges.push_back({});
                edges.push_back({});
            }
            edges[index] = {local.at(a), static_cast<u32>(f), 0};
            faceEdges[f].push_back(index);
        }
    }
    for (const auto& list : faceEdges) {
        for (std::size_t i = 0; i < list.size(); ++i) {
            edges[list[i]][2] = list[(i + 1) % list.size()];
        }
    }
    for (std::size_t e = 0; e < edges.size(); ++e) {
        ConvexHullHalfEdge h;
        h.twinOffset = (e % 2 == 0) ? i8{1} : i8{-1};
        h.originVertex = static_cast<u8>(edges[e][0]);
        h.face = static_cast<u8>(edges[e][1]);
        h.nextInFace = static_cast<u8>(edges[e][2]);
        shape.hullHalfEdges.push_back(h);
    }

    // Planes, area, volume and centroid, from the written floats.
    D3 mean;
    for (const u32 v : hull.vertices) {
        mean = mean + at(v);
    }
    mean = mean * (1.0 / static_cast<double>(hull.vertices.size()));
    double area = 0, volume = 0;
    D3 moment;
    for (std::size_t f = 0; f < hull.faces.size(); ++f) {
        const auto& loop = hull.faces[f];
        D3 newell;
        for (std::size_t i = 0; i < loop.size(); ++i) {
            newell = newell + Cross(at(loop[i]), at(loop[(i + 1) % loop.size()]));
        }
        const D3 n = Normalized(newell);
        double d = -std::numeric_limits<double>::max();
        for (const u32 v : hull.vertices) {
            d = std::max(d, Dot(n, at(v)));
        }
        shape.hullPlanes.push_back(
            {static_cast<f32>(n.x), static_cast<f32>(n.y), static_cast<f32>(n.z), static_cast<f32>(d)});
        shape.hullFaceFirstEdges.push_back(static_cast<u8>(faceEdges[f].front()));
        area += Length(newell) * 0.5;
        const D3 a = at(loop[0]) - mean;
        for (std::size_t i = 1; i + 1 < loop.size(); ++i) {
            const D3 b = at(loop[i]) - mean;
            const D3 c = at(loop[i + 1]) - mean;
            const double v = Dot(a, Cross(b, c)) / 6.0;
            volume += v;
            moment = moment + (a + b + c) * (v * 0.25);
        }
    }
    const D3 centroid = volume > 0 ? mean + moment * (1.0 / volume) : mean;
    shape.hullCentroid = {static_cast<f32>(centroid.x), static_cast<f32>(centroid.y),
                          static_cast<f32>(centroid.z)};
    shape.hullVertexCount = static_cast<u32>(shape.hullVertices.size());
    shape.hullFaceCount = static_cast<u32>(shape.hullPlanes.size());
    shape.hullHalfEdgeCount = static_cast<u32>(shape.hullHalfEdges.size());
    shape.hullVolume = static_cast<f32>(volume);
    shape.hullSurfaceArea = static_cast<f32>(area);
}

// ============================================================================
// The mesh tree, in the client's float arithmetic
// ============================================================================

struct F3 {
    f32 x = 0, y = 0, z = 0;

    f32 operator[](int i) const {
        return i == 0 ? x : (i == 1 ? y : z);
    }
};

F3 Min(const F3& a, const F3& b) {
    return {a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y, a.z < b.z ? a.z : b.z};
}
F3 Max(const F3& a, const F3& b) {
    return {a.x > b.x ? a.x : b.x, a.y > b.y ? a.y : b.y, a.z > b.z ? a.z : b.z};
}
F3 Sub(const F3& a, const F3& b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

struct TreeTri {
    F3 min, max, centroid;
    i32 bin = 0;
};

/// `(e.z + e.x) * e.y + e.z * e.x`: half a box's surface area, in the
/// builder's order.
f32 HalfArea(const F3& lo, const F3& hi) {
    const F3 e = Sub(hi, lo);
    const f32 a = (e.z + e.x) * e.y;
    const f32 b = e.z * e.x;
    return a + b;
}

/// `dmMeshBuilder_BuildBVHRecursive` (0x103378cb0), reduced to the height it
/// produces: at most four triangles a leaf, else a 64-bin surface-area split
/// on the widest centroid axis (a count split when the centroids nearly
/// coincide), partitioned in place as the client partitions.
u32 TreeHeight(TreeTri* tris, i32 count) {
    if (count <= 4) {
        return 1;
    }
    F3 cmin = tris[0].centroid, cmax = tris[0].centroid;
    for (i32 i = 1; i < count; ++i) {
        cmin = Min(cmin, tris[i].centroid);
        cmax = Max(cmax, tris[i].centroid);
    }
    const F3 ext = Sub(cmax, cmin);
    constexpr f32 kCoincident = 0.005f;
    if (ext.z < kCoincident && ext.x < kCoincident && ext.y < kCoincident) {
        const i32 half = count / 2;
        return std::max(TreeHeight(tris, half), TreeHeight(tris + half, count - half)) + 1;
    }
    int axis = 0;
    if (ext.x <= ext.y || ext.x <= ext.z) {
        axis = ext.y > ext.z ? 1 : 2;
    }
    constexpr f32 kMax = std::numeric_limits<f32>::max();
    // Spelled out: MSVC will not read an enclosing function's constant in a
    // local class's member initialisers.
    struct Bin {
        F3 min{std::numeric_limits<f32>::max(), std::numeric_limits<f32>::max(), std::numeric_limits<f32>::max()};
        F3 max{-std::numeric_limits<f32>::max(), -std::numeric_limits<f32>::max(), -std::numeric_limits<f32>::max()};
        i32 count = 0;
    };
    std::array<Bin, 64> bins{};
    const f32 scale = 64.0f / ext[axis];
    const f32 origin = cmin[axis];
    for (i32 i = 0; i < count; ++i) {
        const f32 t = (tris[i].centroid[axis] - origin) * scale;
        // cvttss2si: truncation, and 0x80000000 for what does not fit.
        i32 bin = (std::isfinite(t) && std::abs(t) < 2147483648.0f) ? static_cast<i32>(t)
                                                                     : std::numeric_limits<i32>::min();
        if (bin >= 64) {
            bin = 63;
        }
        if (bin < 0) {
            bin = 0;
        }
        tris[i].bin = bin;
        Bin& b = bins[static_cast<std::size_t>(bin)];
        ++b.count;
        b.min = Min(b.min, tris[i].min);
        b.max = Max(b.max, tris[i].max);
    }
    std::array<Bin, 63> left{}, right{};
    left[0] = bins[0];
    for (std::size_t i = 1; i < 63; ++i) {
        left[i].count = left[i - 1].count + bins[i].count;
        left[i].min = Min(left[i - 1].min, bins[i].min);
        left[i].max = Max(left[i - 1].max, bins[i].max);
    }
    right[62] = bins[63];
    for (std::size_t i = 62; i-- > 0;) {
        right[i].count = right[i + 1].count + bins[i + 1].count;
        right[i].min = Min(right[i + 1].min, bins[i + 1].min);
        right[i].max = Max(right[i + 1].max, bins[i + 1].max);
    }
    f32 best = kMax;
    i32 split = 0;
    for (i32 i = 0; i < 63; ++i) {
        const f32 r = static_cast<f32>(right[i].count) * HalfArea(right[i].min, right[i].max);
        const f32 l = static_cast<f32>(left[i].count) * HalfArea(left[i].min, left[i].max);
        const f32 half = r + l;
        const f32 cost = half + half;
        if (cost < best) {
            split = i;
        }
        best = std::fmin(best, cost);
    }
    i32 last = -1;
    for (i32 i = 0; i < count; ++i) {
        if (tris[i].bin <= split) {
            ++last;
            std::swap(tris[last], tris[i]);
        }
    }
    const i32 leftCount = last + 1;
    return std::max(TreeHeight(tris, leftCount), TreeHeight(tris + leftCount, count - leftCount)) + 1;
}

bool Finite(const Vector3f& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

} // namespace

// ============================================================================
// Public
// ============================================================================

HullCookReport CookHull(PhysicsShape& shape, std::span<const Vector3f> points, HullCook mode) {
    HullCookReport report;
    std::vector<Vector3f> input;
    for (const Vector3f& v : points) {
        if (!Finite(v)) {
            continue;
        }
        // Exact duplicates are one point.
        const bool seen = std::any_of(input.begin(), input.end(), [&](const Vector3f& u) {
            return u.x == v.x && u.y == v.y && u.z == v.z;
        });
        if (!seen) {
            input.push_back(v);
        }
    }
    std::vector<D3> p;
    p.reserve(input.size());
    for (const Vector3f& v : input) {
        p.push_back({v.x, v.y, v.z});
    }
    Polytope hull;
    if (!ExactHull(p, hull)) {
        ClearHull(shape);
        return report;
    }
    bool fromInput = true;
    if (mode == HullCook::ClientLoad) {
        constexpr double kClientMergeAngle = 0.3490658503988659; // 20°
        if (SimplifyFaces(p, hull, std::cos(kClientMergeAngle), std::numeric_limits<std::size_t>::max())) {
            fromInput = false;
        }
    }
    // Over the u8 limits: merge the most parallel faces until it fits.
    for (std::size_t target = hull.faces.size(); !WithinLimits(hull) && target > 4;) {
        target = std::max<std::size_t>(4, target * 9 / 10);
        if (SimplifyFaces(p, hull, 2.0, target)) {
            fromInput = false;
            report.simplified = true;
        }
    }
    if (!WithinLimits(hull)) {
        ClearHull(shape);
        return report;
    }
    WriteHull(shape, p, hull, fromInput ? &input : nullptr);
    report.ok = shape.hullVolume > 0.0f;
    if (!report.ok) {
        ClearHull(shape);
    }
    return report;
}

bool HullTriangles(std::span<const Vector3f> points, std::vector<std::array<u32, 3>>& triangles) {
    triangles.clear();
    std::vector<D3> p;
    p.reserve(points.size());
    for (const Vector3f& v : points) {
        if (!Finite(v)) {
            return false;
        }
        p.push_back({v.x, v.y, v.z});
    }
    std::vector<D3> unit;
    double mergeTolerance = 0;
    if (!UnitFrame(p, unit, mergeTolerance)) {
        return false;
    }
    for (double eps = 1e-10; eps < 1e-5; eps *= 10) {
        if (TriangleHull(unit, eps, triangles)) {
            return true;
        }
    }
    triangles.clear();
    return false;
}

MeshTree ComputeMeshTree(std::span<const Vector3f> vertices, std::span<const u32> triangles) {
    MeshTree tree;
    constexpr f32 kMax = std::numeric_limits<f32>::max();
    constexpr f32 kMaxExtent = 1000000.0f;
    constexpr f32 kMinArea = 2.5e-5f;
    const f32 kMinCross = std::bit_cast<f32>(0x057A0000u); // 1.1754944e-35
    F3 lo{kMax, kMax, kMax};
    F3 hi{-kMax, -kMax, -kMax};
    std::vector<TreeTri> tris;
    tris.reserve(triangles.size() / 3);
    u32 vertexCount = 0;
    for (std::size_t t = 0; t + 2 < triangles.size(); t += 3) {
        const u32 i0 = triangles[t], i1 = triangles[t + 1], i2 = triangles[t + 2];
        if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size()) {
            continue;
        }
        if (!Finite(vertices[i0]) || !Finite(vertices[i1]) || !Finite(vertices[i2])) {
            continue;
        }
        const F3 a{vertices[i0].x, vertices[i0].y, vertices[i0].z};
        const F3 b{vertices[i1].x, vertices[i1].y, vertices[i1].z};
        const F3 c{vertices[i2].x, vertices[i2].y, vertices[i2].z};
        const F3 mn = Min(Min(a, b), c);
        const F3 mx = Max(Max(a, b), c);
        const F3 ext = Sub(mx, mn);
        if (!(ext.z <= kMaxExtent) || !(ext.x <= kMaxExtent) || !(ext.y <= kMaxExtent)) {
            continue;
        }
        const F3 e1 = Sub(b, a);
        const F3 e2 = Sub(c, a);
        const F3 cross{e2.z * e1.y - e2.y * e1.z, e2.x * e1.z - e2.z * e1.x, e2.y * e1.x - e2.x * e1.y};
        const f32 sx = cross.x * cross.x, sy = cross.y * cross.y, sz = cross.z * cross.z;
        const f32 sum = (sy + sx) + sz;
        if (!(std::sqrt(sum) * 0.5f > kMinArea) || !(kMinCross < sum)) {
            continue;
        }
        TreeTri tri;
        tri.min = mn;
        tri.max = mx;
        const F3 s{mx.x + mn.x, mx.y + mn.y, mx.z + mn.z};
        tri.centroid = {s.x * 0.5f, s.y * 0.5f, s.z * 0.5f};
        tris.push_back(tri);
        lo = Min(lo, mn);
        hi = Max(hi, mx);
        vertexCount = std::max({vertexCount, i0 + 1, i1 + 1, i2 + 1});
    }
    if (tris.empty()) {
        tree.extent = {1.0f, 1.0f, 1.0f};
        tree.tolerance = {1.0f, 1.0f, 1.0f};
        return tree;
    }
    constexpr f32 kPad = 0.1f;
    const F3 extent{((kPad - lo.x) + hi.x) * 0.5f, ((kPad - lo.y) + hi.y) * 0.5f,
                    ((kPad - lo.z) + hi.z) * 0.5f};
    const F3 center{(hi.x + lo.x) * 0.5f, (hi.y + lo.y) * 0.5f, (hi.z + lo.z) * 0.5f};
    const f32 kStep = std::bit_cast<f32>(0x38000100u); // 1 / 32767
    tree.center = {center.x, center.y, center.z};
    tree.extent = {extent.x, extent.y, extent.z};
    tree.tolerance = {extent.x * kStep, extent.y * kStep, extent.z * kStep};
    tree.vertexCount = vertexCount;
    for (TreeTri& t : tris) {
        t.min = Sub(t.min, center);
        t.max = Sub(t.max, center);
        t.centroid = Sub(t.centroid, center);
    }
    tree.height = TreeHeight(tris.data(), static_cast<i32>(tris.size()));
    return tree;
}

void CookMesh(PhysicsShape& shape, std::span<const Vector3f> vertices, std::span<const u32> triangles) {
    const MeshTree tree = ComputeMeshTree(vertices, triangles);
    shape.meshBvhNodes.clear();
    shape.meshFaceIndices16.clear();
    shape.meshFaceIndices32.clear();
    shape.meshVertexPositions.clear();
    shape.meshVertexPositions.reserve(vertices.size());
    // Stored about the centre, which the client adds back.
    for (const Vector3f& v : vertices) {
        shape.meshVertexPositions.push_back(
            {v.x - tree.center.x, v.y - tree.center.y, v.z - tree.center.z, 0.0f});
    }
    // Each edge's neighbour: the far vertex of the triangle across it.
    std::unordered_map<u64, u32> across;
    const std::size_t count = triangles.size() / 3;
    for (std::size_t t = 0; t < count; ++t) {
        for (int e = 0; e < 3; ++e) {
            const u32 a = triangles[t * 3 + e];
            const u32 b = triangles[t * 3 + (e + 1) % 3];
            across.emplace(EdgeKey(a, b), triangles[t * 3 + (e + 2) % 3]);
        }
    }
    shape.meshFaceIndices32.reserve(count);
    for (std::size_t t = 0; t < count; ++t) {
        std::array<u32, 7> face{triangles[t * 3], triangles[t * 3 + 1], triangles[t * 3 + 2],
                                0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0};
        for (int e = 0; e < 3; ++e) {
            const auto it = across.find(EdgeKey(face[static_cast<std::size_t>((e + 1) % 3)],
                                                face[static_cast<std::size_t>(e)]));
            if (it != across.end()) {
                face[static_cast<std::size_t>(3 + e)] = it->second;
            }
        }
        shape.meshFaceIndices32.push_back(face);
    }
    shape.meshBoundsCenter = tree.center;
    shape.meshBoundsExtent = tree.extent;
    shape.meshTolerance = tree.tolerance;
    shape.meshTreeDepth = tree.height;
    shape.meshNormalCount = 0;
    shape.meshVertexCount = static_cast<u32>(vertices.size());
    shape.meshFaceIndex16Count = 0;
    shape.meshFaceIndex32Count = static_cast<u32>(count);
    shape.meshUnknown1 = 0;
    shape.meshReserved = 0;
    shape.meshCollisionMargin = 0.0f;
}

} // namespace m3
} // namespace whiteout
