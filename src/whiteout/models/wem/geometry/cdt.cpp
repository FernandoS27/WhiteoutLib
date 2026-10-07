// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/geometry/cdt.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <utility>

namespace whiteout {
namespace models {
namespace wem {
namespace geom {

namespace {

constexpr u32 kNone = 0xFFFFFFFFu;

/// The grid the points snap to. The super-triangle reaches 9 times past it, so
/// every coordinate difference stays under 2^30: orientation fits 64 bits and
/// the in-circle determinant, a sum of three products of 2^61 by 2^61, fits 128.
constexpr f64 kGrid = 67108864.0; // 2^26
/// Grid steps within which two points are one, and an end lies on a segment.
constexpr i64 kSnap = 16;

struct Point {
    i64 x = 0;
    i64 y = 0;
};

/// A signed 128-bit integer, just enough for the in-circle sum.
struct Wide {
    u64 lo = 0;
    u64 hi = 0;
};

Wide Multiply(i64 a, i64 b) {
    const bool negative = (a < 0) != (b < 0);
    const u64 ua = a < 0 ? 0 - static_cast<u64>(a) : static_cast<u64>(a);
    const u64 ub = b < 0 ? 0 - static_cast<u64>(b) : static_cast<u64>(b);
    const u64 a0 = ua & 0xFFFFFFFFu;
    const u64 a1 = ua >> 32;
    const u64 b0 = ub & 0xFFFFFFFFu;
    const u64 b1 = ub >> 32;
    const u64 p00 = a0 * b0;
    const u64 p01 = a0 * b1;
    const u64 p10 = a1 * b0;
    const u64 p11 = a1 * b1;
    const u64 middle = (p00 >> 32) + (p01 & 0xFFFFFFFFu) + (p10 & 0xFFFFFFFFu);
    Wide out;
    out.lo = (p00 & 0xFFFFFFFFu) | (middle << 32);
    out.hi = p11 + (p01 >> 32) + (p10 >> 32) + (middle >> 32);
    if (negative) {
        out.lo = ~out.lo + 1;
        out.hi = ~out.hi + (out.lo == 0 ? 1 : 0);
    }
    return out;
}

Wide Add(Wide a, Wide b) {
    Wide out;
    out.lo = a.lo + b.lo;
    out.hi = a.hi + b.hi + (out.lo < a.lo ? 1 : 0);
    return out;
}

int SignOf(Wide a) {
    if (static_cast<i64>(a.hi) < 0) {
        return -1;
    }
    return (a.hi != 0 || a.lo != 0) ? 1 : 0;
}

/// > 0 when a, b, c turn counter-clockwise.
i64 Orient(const Point& a, const Point& b, const Point& c) {
    const i64 v = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    return v > 0 ? 1 : (v < 0 ? -1 : 0);
}

/// > 0 when d is inside the circle through the counter-clockwise a, b, c.
int InCircle(const Point& a, const Point& b, const Point& c, const Point& d) {
    const i64 adx = a.x - d.x;
    const i64 ady = a.y - d.y;
    const i64 bdx = b.x - d.x;
    const i64 bdy = b.y - d.y;
    const i64 cdx = c.x - d.x;
    const i64 cdy = c.y - d.y;
    const i64 alift = adx * adx + ady * ady;
    const i64 blift = bdx * bdx + bdy * bdy;
    const i64 clift = cdx * cdx + cdy * cdy;
    const i64 bc = bdx * cdy - cdx * bdy;
    const i64 ca = cdx * ady - adx * cdy;
    const i64 ab = adx * bdy - bdx * ady;
    return SignOf(Add(Add(Multiply(alift, bc), Multiply(blift, ca)), Multiply(clift, ab)));
}

struct Triangle {
    std::array<u32, 3> v{kNone, kNone, kNone};
    /// Across edge k, from v[k + 1] to v[k + 2].
    std::array<u32, 3> n{kNone, kNone, kNone};
    std::array<bool, 3> constrained{false, false, false};
};

u32 Next(u32 k) {
    return k == 2 ? 0 : k + 1;
}
u32 Prev(u32 k) {
    return k == 0 ? 2 : k - 1;
}

class Mesh2d {
public:
    explicit Mesh2d(std::vector<Point> points) : points_(std::move(points)) {}

    std::vector<Point> points_;
    std::vector<Triangle> tris_;
    std::vector<u32> vertexTri_;
    u32 last_ = 0;

    void begin(u32 superA, u32 superB, u32 superC) {
        Triangle t;
        t.v = {superA, superB, superC};
        tris_.push_back(t);
        vertexTri_.assign(points_.size(), kNone);
        touch(0);
    }

    void touch(u32 t) {
        for (u32 v : tris_[t].v) {
            vertexTri_[v] = t;
        }
    }

    /// The index of edge (a, b) in @p t, either way round, or kNone.
    u32 edgeOf(u32 t, u32 a, u32 b) const {
        const Triangle& tri = tris_[t];
        for (u32 k = 0; k < 3; ++k) {
            const u32 p = tri.v[Next(k)];
            const u32 q = tri.v[Prev(k)];
            if ((p == a && q == b) || (p == b && q == a)) {
                return k;
            }
        }
        return kNone;
    }

    /// Points @p t's edge @p k at @p u and @p u back at @p t.
    void link(u32 t, u32 k, u32 u, bool constrained) {
        tris_[t].n[k] = u;
        tris_[t].constrained[k] = constrained;
        if (u == kNone) {
            return;
        }
        const u32 j = edgeOf(u, tris_[t].v[Next(k)], tris_[t].v[Prev(k)]);
        if (j != kNone) {
            tris_[u].n[j] = t;
            tris_[u].constrained[j] = constrained;
        }
    }

    struct Side {
        u32 tri = kNone;
        bool constrained = false;
    };

    /// What lies across edge (a, b) of @p t.
    Side across(u32 t, u32 a, u32 b) const {
        const u32 k = edgeOf(t, a, b);
        return k == kNone ? Side{} : Side{tris_[t].n[k], tris_[t].constrained[k]};
    }

    /// The triangle holding @p p: kNone past the walk's bound. @p edge is the
    /// edge @p p lies on, or kNone.
    u32 locate(const Point& p, u32& edge) const {
        u32 t = last_;
        const std::size_t bound = tris_.size() * 4 + 64;
        for (std::size_t step = 0; step < bound; ++step) {
            const Triangle& tri = tris_[t];
            u32 moved = kNone;
            edge = kNone;
            for (u32 i = 0; i < 3; ++i) {
                const u32 k = static_cast<u32>((i + step) % 3);
                const i64 o = Orient(points_[tri.v[Next(k)]], points_[tri.v[Prev(k)]], p);
                if (o < 0) {
                    moved = tri.n[k];
                    break;
                }
                if (o == 0) {
                    edge = k;
                }
            }
            if (moved == kNone) {
                return t;
            }
            t = moved;
        }
        return kNone;
    }

    /// Rebuilds @p t as (a, b, c) with its three neighbours.
    void set(u32 t, u32 a, u32 b, u32 c, Side ab, Side bc, Side ca) {
        tris_[t].v = {a, b, c};
        // Edge 0 is (b, c), edge 1 (c, a), edge 2 (a, b).
        tris_[t].n = {kNone, kNone, kNone};
        link(t, 0, bc.tri, bc.constrained);
        link(t, 1, ca.tri, ca.constrained);
        link(t, 2, ab.tri, ab.constrained);
        touch(t);
    }

    u32 add() {
        tris_.push_back(Triangle{});
        return static_cast<u32>(tris_.size() - 1);
    }

    void legalize(u32 t, u32 p, std::vector<std::pair<u32, u32>>& stack) {
        // The edge of t opposite p.
        const Triangle& tri = tris_[t];
        for (u32 k = 0; k < 3; ++k) {
            if (tri.v[k] == p) {
                stack.emplace_back(tri.v[Next(k)], tri.v[Prev(k)]);
            }
        }
    }

    /// Flips the edge (a, b) shared by @p t and its neighbour; the quad must be
    /// strictly convex. Returns the two new triangles' ids (t, u).
    std::pair<u32, u32> flip(u32 t, u32 k) {
        const u32 u = tris_[t].n[k];
        const u32 p = tris_[t].v[k];
        const u32 a = tris_[t].v[Next(k)];
        const u32 b = tris_[t].v[Prev(k)];
        const u32 j = edgeOf(u, a, b);
        const u32 d = tris_[u].v[j];
        const Side pa = across(t, p, a);
        const Side bp = across(t, b, p);
        const Side ad = across(u, a, d);
        const Side db = across(u, d, b);
        // Unlink both first so `link` cannot find the old shared edge.
        tris_[t].n = {kNone, kNone, kNone};
        tris_[u].n = {kNone, kNone, kNone};
        tris_[t].v = {p, a, d};
        tris_[u].v = {p, d, b};
        link(t, 0, ad.tri, ad.constrained); // (a, d)
        link(t, 2, pa.tri, pa.constrained); // (p, a)
        link(u, 0, db.tri, db.constrained); // (d, b)
        link(u, 1, bp.tri, bp.constrained); // (b, p)
        link(t, 1, u, false);               // (d, p)
        touch(t);
        touch(u);
        return {t, u};
    }

    void legalizeAll(u32 p, std::vector<std::pair<u32, u32>>& stack) {
        while (!stack.empty()) {
            const auto [a, b] = stack.back();
            stack.pop_back();
            const u32 t = triangleWithEdge(a, b, p);
            if (t == kNone) {
                continue;
            }
            const u32 k = edgeOf(t, a, b);
            const u32 u = tris_[t].n[k];
            if (u == kNone || tris_[t].constrained[k]) {
                continue;
            }
            const u32 d = tris_[u].v[edgeOf(u, a, b)];
            const Triangle& tri = tris_[t];
            if (InCircle(points_[tri.v[0]], points_[tri.v[1]], points_[tri.v[2]], points_[d]) > 0) {
                flip(t, k);
                stack.emplace_back(a, d);
                stack.emplace_back(d, b);
            }
        }
    }

    /// The triangle with edge (a, b) and corner @p c.
    u32 triangleWithEdge(u32 a, u32 b, u32 c) const {
        const u32 start = vertexTri_[c];
        if (start == kNone) {
            return kNone;
        }
        u32 t = start;
        for (std::size_t guard = 0; guard < tris_.size() + 1; ++guard) {
            const Triangle& tri = tris_[t];
            if (edgeOf(t, a, b) != kNone) {
                return t;
            }
            u32 k = 0;
            while (tri.v[k] != c) {
                ++k;
            }
            t = tri.n[Next(k)];
            if (t == kNone || t == start) {
                break;
            }
        }
        // Round the other way, in case the fan is open.
        t = start;
        for (std::size_t guard = 0; guard < tris_.size() + 1; ++guard) {
            const Triangle& tri = tris_[t];
            if (edgeOf(t, a, b) != kNone) {
                return t;
            }
            u32 k = 0;
            while (tri.v[k] != c) {
                ++k;
            }
            t = tri.n[Prev(k)];
            if (t == kNone || t == start) {
                break;
            }
        }
        return kNone;
    }

    bool insert(u32 p) {
        u32 edge = kNone;
        u32 t = locate(points_[p], edge);
        if (t == kNone) {
            for (u32 i = 0; i < tris_.size() && t == kNone; ++i) {
                const Triangle& tri = tris_[i];
                bool inside = true;
                edge = kNone;
                for (u32 k = 0; k < 3 && inside; ++k) {
                    const i64 o =
                        Orient(points_[tri.v[Next(k)]], points_[tri.v[Prev(k)]], points_[p]);
                    if (o < 0) {
                        inside = false;
                    } else if (o == 0) {
                        edge = k;
                    }
                }
                if (inside) {
                    t = i;
                }
            }
            if (t == kNone) {
                return false;
            }
        }
        std::vector<std::pair<u32, u32>> stack;
        if (edge == kNone) {
            const Triangle old = tris_[t];
            const u32 a = old.v[0];
            const u32 b = old.v[1];
            const u32 c = old.v[2];
            const Side ab{old.n[2], old.constrained[2]};
            const Side bc{old.n[0], old.constrained[0]};
            const Side ca{old.n[1], old.constrained[1]};
            const u32 t1 = add();
            const u32 t2 = add();
            tris_[t].n = {kNone, kNone, kNone};
            // Detach the outer neighbours from t so `link` rewires them.
            set(t, a, b, p, ab, Side{}, Side{});
            set(t1, b, c, p, bc, Side{}, Side{t, false});
            set(t2, c, a, p, ca, Side{t, false}, Side{t1, false});
            last_ = t;
            stack.emplace_back(a, b);
            stack.emplace_back(b, c);
            stack.emplace_back(c, a);
        } else {
            // On edge (a, b) of t, opposite c; u holds the other side, opposite d.
            const Triangle old = tris_[t];
            const u32 c = old.v[edge];
            const u32 a = old.v[Next(edge)];
            const u32 b = old.v[Prev(edge)];
            const u32 u = old.n[edge];
            const bool cut = old.constrained[edge];
            if (u == kNone) {
                return false;
            }
            const Triangle oldU = tris_[u];
            const u32 d = oldU.v[edgeOf(u, a, b)];
            const Side ca = across(t, c, a);
            const Side bc = across(t, b, c);
            const Side bd = across(u, b, d);
            const Side da = across(u, d, a);
            tris_[t].n = {kNone, kNone, kNone};
            tris_[u].n = {kNone, kNone, kNone};
            const u32 t2 = add();
            const u32 u2 = add();
            set(t, c, a, p, ca, Side{}, Side{});
            set(t2, c, p, b, Side{t, false}, Side{}, bc);
            set(u, d, b, p, bd, Side{t2, cut}, Side{});
            set(u2, d, p, a, Side{u, false}, Side{t, cut}, da);
            last_ = t;
            stack.emplace_back(c, a);
            stack.emplace_back(b, c);
            stack.emplace_back(b, d);
            stack.emplace_back(d, a);
        }
        legalizeAll(p, stack);
        return true;
    }

    bool hasEdge(u32 a, u32 b) const {
        return triangleWithEdge(a, b, a) != kNone;
    }

    void constrain(u32 a, u32 b) {
        const u32 t = triangleWithEdge(a, b, a);
        if (t != kNone) {
            const u32 k = edgeOf(t, a, b);
            link(t, k, tris_[t].n[k], true);
        }
    }

    /// Strictly between: the segments cross at a point inside both.
    bool crosses(u32 a, u32 b, u32 p, u32 q) const {
        if (p == a || p == b || q == a || q == b) {
            return false;
        }
        const i64 o1 = Orient(points_[a], points_[b], points_[p]);
        const i64 o2 = Orient(points_[a], points_[b], points_[q]);
        const i64 o3 = Orient(points_[p], points_[q], points_[a]);
        const i64 o4 = Orient(points_[p], points_[q], points_[b]);
        return o1 * o2 < 0 && o3 * o4 < 0;
    }

    bool onSegment(u32 a, u32 b, u32 c) const {
        if (Orient(points_[a], points_[b], points_[c]) != 0) {
            return false;
        }
        const Point& pa = points_[a];
        const Point& pb = points_[b];
        const Point& pc = points_[c];
        const i64 dot = (pc.x - pa.x) * (pb.x - pa.x) + (pc.y - pa.y) * (pb.y - pa.y);
        const i64 len = (pb.x - pa.x) * (pb.x - pa.x) + (pb.y - pa.y) * (pb.y - pa.y);
        return dot > 0 && dot < len;
    }

    void unconstrain(u32 a, u32 b) {
        const u32 t = triangleWithEdge(a, b, a);
        if (t != kNone) {
            const u32 k = edgeOf(t, a, b);
            link(t, k, tris_[t].n[k], false);
        }
    }

    /// Segment (a, b) meets constraint (p, q), which the snap left across it:
    /// both are split where they cross, at a vertex of its own, or at an end
    /// the crossing falls on.
    bool cross(u32 a, u32 b, u32 p, u32 q, u32 depth) {
        const Point& pa = points_[a];
        const Point& pb = points_[b];
        const Point& pp = points_[p];
        const Point& pq = points_[q];
        const f64 rx = static_cast<f64>(pb.x - pa.x);
        const f64 ry = static_cast<f64>(pb.y - pa.y);
        const f64 sx = static_cast<f64>(pq.x - pp.x);
        const f64 sy = static_cast<f64>(pq.y - pp.y);
        const f64 denominator = rx * sy - ry * sx;
        if (denominator == 0.0) {
            return false;
        }
        const f64 t = (static_cast<f64>(pp.x - pa.x) * sy - static_cast<f64>(pp.y - pa.y) * sx) / denominator;
        const Point x{pa.x + static_cast<i64>(std::llround(t * rx)), pa.y + static_cast<i64>(std::llround(t * ry))};
        const auto near = [&](u32 e) {
            const i64 dx = points_[e].x - x.x;
            const i64 dy = points_[e].y - x.y;
            return dx * dx + dy * dy <= kSnap * kSnap;
        };
        // On an end of the constraint: the segment runs through it.
        for (u32 e : {p, q}) {
            if (near(e)) {
                return force(a, e, depth + 1) && force(e, b, depth + 1);
            }
        }
        // On an end of the segment: the constraint runs through it.
        for (u32 e : {a, b}) {
            if (near(e)) {
                unconstrain(p, q);
                return force(p, e, depth + 1) && force(e, q, depth + 1) && force(a, b, depth + 1);
            }
        }
        const u32 made = static_cast<u32>(points_.size());
        points_.push_back(x);
        vertexTri_.push_back(kNone);
        unconstrain(p, q);
        if (!insert(made)) {
            return false;
        }
        return force(p, made, depth + 1) && force(made, q, depth + 1) && force(a, made, depth + 1) &&
               force(made, b, depth + 1);
    }

    /// Forces segment (a, b) in. Splits it at a point lying on it, and where it
    /// crosses a constraint already in. False when the flips do not converge.
    bool force(u32 a, u32 b, u32 depth = 0) {
        if (a == b || depth > 64) {
            return a == b;
        }
        if (hasEdge(a, b)) {
            constrain(a, b);
            return true;
        }
        // The edges the segment crosses, walked from a.
        std::deque<std::pair<u32, u32>> crossing;
        u32 t = vertexTri_[a];
        u32 right = kNone;
        u32 left = kNone;
        {
            const u32 start = t;
            bool found = false;
            for (std::size_t guard = 0; guard < tris_.size() + 1 && t != kNone; ++guard) {
                const Triangle& tri = tris_[t];
                u32 k = 0;
                while (tri.v[k] != a) {
                    ++k;
                }
                const u32 x = tri.v[Next(k)];
                const u32 y = tri.v[Prev(k)];
                if (onSegment(a, b, x)) {
                    return force(a, x, depth + 1) && force(x, b, depth + 1);
                }
                if (onSegment(a, b, y)) {
                    return force(a, y, depth + 1) && force(y, b, depth + 1);
                }
                if (Orient(points_[a], points_[x], points_[b]) > 0 &&
                    Orient(points_[a], points_[y], points_[b]) < 0) {
                    right = x;
                    left = y;
                    found = true;
                    break;
                }
                t = tri.n[Next(k)];
                if (t == start) {
                    break;
                }
            }
            if (!found) {
                return false;
            }
        }
        // Walk the corridor.
        for (std::size_t guard = 0; guard < tris_.size() + 1; ++guard) {
            const u32 k = edgeOf(t, right, left);
            if (tris_[t].constrained[k]) {
                return cross(a, b, right, left, depth);
            }
            crossing.emplace_back(right, left);
            const u32 u = tris_[t].n[k];
            if (u == kNone) {
                return false;
            }
            const u32 z = tris_[u].v[edgeOf(u, right, left)];
            if (z == b) {
                break;
            }
            if (onSegment(a, b, z)) {
                return force(a, z, depth + 1) && force(z, b, depth + 1);
            }
            if (Orient(points_[a], points_[b], points_[z]) > 0) {
                left = z;
            } else {
                right = z;
            }
            t = u;
        }
        // Flip until the segment is an edge (Sloan).
        std::vector<std::pair<u32, u32>> made;
        std::size_t budget = crossing.size() * crossing.size() * 4 + 256;
        while (!crossing.empty()) {
            if (budget-- == 0) {
                return false;
            }
            const auto [p, q] = crossing.front();
            crossing.pop_front();
            const u32 t1 = triangleWithEdge(p, q, p);
            if (t1 == kNone) {
                return false;
            }
            const u32 k = edgeOf(t1, p, q);
            const u32 t2 = tris_[t1].n[k];
            if (t2 == kNone) {
                return false;
            }
            const u32 r = tris_[t1].v[k];
            const u32 s = tris_[t2].v[edgeOf(t2, p, q)];
            const bool convex = Orient(points_[r], points_[s], points_[p]) *
                                    Orient(points_[r], points_[s], points_[q]) <
                                0;
            if (!convex) {
                crossing.emplace_back(p, q);
                continue;
            }
            flip(t1, k);
            if (crosses(a, b, r, s)) {
                crossing.emplace_back(r, s);
            } else {
                made.emplace_back(r, s);
            }
        }
        constrain(a, b);
        // Restore Delaunay among the new edges, never across a constraint.
        for (u32 pass = 0; pass < 8; ++pass) {
            bool changed = false;
            for (auto& [p, q] : made) {
                if ((p == a && q == b) || (p == b && q == a)) {
                    continue;
                }
                const u32 t1 = triangleWithEdge(p, q, p);
                if (t1 == kNone) {
                    continue;
                }
                const u32 k = edgeOf(t1, p, q);
                const u32 t2 = tris_[t1].n[k];
                if (t2 == kNone || tris_[t1].constrained[k]) {
                    continue;
                }
                const u32 r = tris_[t1].v[k];
                const u32 s = tris_[t2].v[edgeOf(t2, p, q)];
                const Triangle& tri = tris_[t1];
                if (InCircle(points_[tri.v[0]], points_[tri.v[1]], points_[tri.v[2]],
                             points_[s]) > 0 &&
                    Orient(points_[r], points_[s], points_[p]) *
                            Orient(points_[r], points_[s], points_[q]) <
                        0) {
                    flip(t1, k);
                    p = r;
                    q = s;
                    changed = true;
                }
            }
            if (!changed) {
                break;
            }
        }
        return true;
    }
};

/// Where segments i and j cross strictly inside both, as a parameter on each.
bool Crossing(const Vector2<f64>& a, const Vector2<f64>& b, const Vector2<f64>& c,
              const Vector2<f64>& d, f64& s, f64& t) {
    const f64 rx = b.x - a.x;
    const f64 ry = b.y - a.y;
    const f64 qx = d.x - c.x;
    const f64 qy = d.y - c.y;
    const f64 denominator = rx * qy - ry * qx;
    if (denominator == 0.0) {
        return false;
    }
    const f64 wx = c.x - a.x;
    const f64 wy = c.y - a.y;
    s = (wx * qy - wy * qx) / denominator;
    t = (wx * ry - wy * rx) / denominator;
    return s > 0.0 && s < 1.0 && t > 0.0 && t < 1.0;
}

} // namespace

Cdt2d ConstrainedTriangulation2d(std::span<const Vector2<f64>> points,
                                 std::span<const u32> segments) {
    Cdt2d out;
    out.points.assign(points.begin(), points.end());
    if (points.size() < 3) {
        return out;
    }

    // --- split the segments where they cross --------------------------------
    struct Segment {
        u32 a;
        u32 b;
    };
    std::vector<Segment> input;
    for (std::size_t i = 0; i + 1 < segments.size(); i += 2) {
        if (segments[i] != segments[i + 1] && segments[i] < points.size() &&
            segments[i + 1] < points.size()) {
            input.push_back(Segment{segments[i], segments[i + 1]});
        }
    }
    std::vector<std::vector<std::pair<f64, u32>>> splits(input.size());
    // An end within the snap of another segment is on it, and splits it, so
    // the snap cannot leave it across.
    f64 near = 0.0;
    {
        f64 lowX = out.points[0].x;
        f64 lowY = out.points[0].y;
        f64 highX = lowX;
        f64 highY = lowY;
        for (const Vector2<f64>& p : out.points) {
            lowX = std::min(lowX, p.x);
            lowY = std::min(lowY, p.y);
            highX = std::max(highX, p.x);
            highY = std::max(highY, p.y);
        }
        near = static_cast<f64>(kSnap) * std::max(highX - lowX, highY - lowY) / kGrid;
    }
    // Whether both ends of @p other lie within the snap of @p segment's line.
    const auto alongLine = [&](u32 segment, const Segment& other) {
        const Vector2<f64>& a = out.points[input[segment].a];
        const Vector2<f64>& b = out.points[input[segment].b];
        const f64 dx = b.x - a.x;
        const f64 dy = b.y - a.y;
        const f64 length = std::sqrt(dx * dx + dy * dy);
        if (!(length > 0.0)) {
            return false;
        }
        for (u32 p : {other.a, other.b}) {
            const Vector2<f64>& q = out.points[p];
            if (std::fabs((q.x - a.x) * dy - (q.y - a.y) * dx) / length > near) {
                return false;
            }
        }
        return true;
    };
    const auto touch = [&](u32 segment, u32 point) {
        const Vector2<f64>& a = out.points[input[segment].a];
        const Vector2<f64>& b = out.points[input[segment].b];
        const Vector2<f64>& p = out.points[point];
        const f64 dx = b.x - a.x;
        const f64 dy = b.y - a.y;
        const f64 length2 = dx * dx + dy * dy;
        if (!(length2 > 0.0)) {
            return;
        }
        const f64 t = ((p.x - a.x) * dx + (p.y - a.y) * dy) / length2;
        const f64 cx = a.x + t * dx - p.x;
        const f64 cy = a.y + t * dy - p.y;
        const f64 reach = near / std::sqrt(length2);
        if (t > reach && t < 1.0 - reach && cx * cx + cy * cy <= near * near) {
            auto& list = splits[segment];
            if (std::find_if(list.begin(), list.end(), [&](const auto& e) { return e.second == point; }) ==
                list.end()) {
                list.emplace_back(t, point);
            }
        }
    };
    {
        std::vector<u32> order(input.size());
        for (u32 i = 0; i < order.size(); ++i) {
            order[i] = i;
        }
        const auto lowX = [&](const Segment& s) {
            return std::min(out.points[s.a].x, out.points[s.b].x);
        };
        const auto highX = [&](const Segment& s) {
            return std::max(out.points[s.a].x, out.points[s.b].x);
        };
        std::sort(order.begin(), order.end(), [&](u32 l, u32 r) {
            const f64 x0 = lowX(input[l]);
            const f64 x1 = lowX(input[r]);
            return x0 != x1 ? x0 < x1 : l < r;
        });
        for (std::size_t oi = 0; oi < order.size(); ++oi) {
            const Segment& si = input[order[oi]];
            const f64 reach = highX(si);
            for (std::size_t oj = oi + 1; oj < order.size(); ++oj) {
                const Segment& sj = input[order[oj]];
                if (lowX(sj) > reach + near) {
                    break;
                }
                if (si.a != sj.a && si.a != sj.b) {
                    touch(order[oj], si.a);
                }
                if (si.b != sj.a && si.b != sj.b) {
                    touch(order[oj], si.b);
                }
                if (sj.a != si.a && sj.a != si.b) {
                    touch(order[oi], sj.a);
                }
                if (sj.b != si.a && sj.b != si.b) {
                    touch(order[oi], sj.b);
                }
                if (si.a == sj.a || si.a == sj.b || si.b == sj.a || si.b == sj.b) {
                    continue;
                }
                // Segments along one line overlap rather than cross: the ends
                // above split them, and a crossing point would land anywhere.
                if (alongLine(order[oi], sj) || alongLine(order[oj], si)) {
                    continue;
                }
                f64 s = 0.0;
                f64 t = 0.0;
                if (!Crossing(out.points[si.a], out.points[si.b], out.points[sj.a],
                              out.points[sj.b], s, t)) {
                    continue;
                }
                const Vector2<f64>& a = out.points[si.a];
                const Vector2<f64>& b = out.points[si.b];
                const u32 index = static_cast<u32>(out.points.size());
                out.points.push_back(Vector2<f64>(a.x + s * (b.x - a.x), a.y + s * (b.y - a.y)));
                splits[order[oi]].emplace_back(s, index);
                splits[order[oj]].emplace_back(t, index);
            }
        }
    }
    std::vector<Segment> pieces;
    for (std::size_t i = 0; i < input.size(); ++i) {
        auto& cuts = splits[i];
        std::sort(cuts.begin(), cuts.end());
        u32 from = input[i].a;
        for (const auto& cut : cuts) {
            pieces.push_back(Segment{from, cut.second});
            from = cut.second;
        }
        pieces.push_back(Segment{from, input[i].b});
    }

    // --- snap to the grid -----------------------------------------------------
    f64 lowX = out.points[0].x;
    f64 lowY = out.points[0].y;
    f64 highX = lowX;
    f64 highY = lowY;
    for (const Vector2<f64>& p : out.points) {
        lowX = std::min(lowX, p.x);
        lowY = std::min(lowY, p.y);
        highX = std::max(highX, p.x);
        highY = std::max(highY, p.y);
    }
    const f64 extent = std::max(highX - lowX, highY - lowY);
    if (!(extent > 0.0) || !std::isfinite(extent)) {
        return out;
    }
    const f64 scale = kGrid / extent;
    const u32 count = static_cast<u32>(out.points.size());
    std::vector<Point> grid(count + 3);
    for (u32 i = 0; i < count; ++i) {
        grid[i].x = static_cast<i64>(std::llround((out.points[i].x - lowX) * scale));
        grid[i].y = static_cast<i64>(std::llround((out.points[i].y - lowY) * scale));
    }
    // Each point names the first point within the snap of it: points that
    // close are one point keyed two ways, and two segments leaving them would
    // cross at their start once snapped.
    std::vector<u32> same(count);
    {
        for (u32 i = 0; i < count; ++i) {
            same[i] = i;
        }
        const auto root = [&](u32 x) {
            while (same[x] != x) {
                same[x] = same[same[x]];
                x = same[x];
            }
            return x;
        };
        std::vector<u32> order(count);
        for (u32 i = 0; i < count; ++i) {
            order[i] = i;
        }
        std::sort(order.begin(), order.end(), [&](u32 l, u32 r) {
            return grid[l].x != grid[r].x ? grid[l].x < grid[r].x : l < r;
        });
        for (std::size_t i = 0; i < order.size(); ++i) {
            for (std::size_t j = i + 1; j < order.size() && grid[order[j]].x - grid[order[i]].x <= kSnap; ++j) {
                const i64 dx = grid[order[j]].x - grid[order[i]].x;
                const i64 dy = grid[order[j]].y - grid[order[i]].y;
                if (dx * dx + dy * dy <= kSnap * kSnap) {
                    const u32 a = root(order[i]);
                    const u32 b = root(order[j]);
                    same[std::max(a, b)] = std::min(a, b);
                }
            }
        }
        for (u32 i = 0; i < count; ++i) {
            same[i] = root(i);
        }
    }
    out.same = same;
    const i64 g = static_cast<i64>(kGrid);
    grid[count] = Point{-4 * g, -4 * g};
    grid[count + 1] = Point{9 * g, -4 * g};
    grid[count + 2] = Point{-4 * g, 9 * g};

    Mesh2d mesh(std::move(grid));
    mesh.begin(count, count + 1, count + 2);
    for (u32 i = 0; i < count; ++i) {
        if (same[i] == i && !mesh.insert(i)) {
            ++out.droppedSegments;
        }
    }
    for (const Segment& s : pieces) {
        const u32 a = same[s.a];
        const u32 b = same[s.b];
        if (a != b && !mesh.force(a, b)) {
            ++out.droppedSegments;
        }
    }
    // Where constraints crossed once snapped: points past the super-triangle's.
    const u32 firstMade = count + 3;
    for (u32 i = firstMade; i < mesh.points_.size(); ++i) {
        out.points.push_back(Vector2<f64>(lowX + static_cast<f64>(mesh.points_[i].x) / scale,
                                          lowY + static_cast<f64>(mesh.points_[i].y) / scale));
    }
    const auto named = [&](u32 v) { return v < count ? v : count + (v - firstMade); };
    const auto super = [&](u32 v) { return v >= count && v < firstMade; };

    // --- regions --------------------------------------------------------------
    const std::size_t triCount = mesh.tris_.size();
    std::vector<u32> region(triCount, kNone);
    constexpr u32 kOutside = kNone - 1;
    std::vector<u32> queue;
    const auto flood = [&](u32 seed, u32 id) {
        queue.assign(1, seed);
        region[seed] = id;
        while (!queue.empty()) {
            const u32 t = queue.back();
            queue.pop_back();
            for (u32 k = 0; k < 3; ++k) {
                const u32 u = mesh.tris_[t].n[k];
                if (u != kNone && !mesh.tris_[t].constrained[k] && region[u] == kNone) {
                    region[u] = id;
                    queue.push_back(u);
                }
            }
        }
    };
    for (u32 t = 0; t < triCount; ++t) {
        const auto& v = mesh.tris_[t].v;
        if (region[t] == kNone && (super(v[0]) || super(v[1]) || super(v[2]))) {
            flood(t, kOutside);
        }
    }
    for (u32 t = 0; t < triCount; ++t) {
        if (region[t] != kNone) {
            continue;
        }
        flood(t, out.regionCount++);
    }
    for (u32 t = 0; t < triCount; ++t) {
        if (region[t] == kOutside) {
            continue;
        }
        const auto& v = mesh.tris_[t].v;
        out.triangles.insert(out.triangles.end(), {named(v[0]), named(v[1]), named(v[2])});
        out.regions.push_back(region[t]);
    }
    return out;
}

} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
