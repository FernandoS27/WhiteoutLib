// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/geometry/bvh.h>

#include <algorithm>
#include <cmath>

namespace whiteout {
namespace models {
namespace wem {
namespace geom {

namespace {

constexpr u32 kLeafSize = 4;

void Grow(Vector3f& low, Vector3f& high, const Vector3f& p) {
    low.x = std::min(low.x, p.x);
    low.y = std::min(low.y, p.y);
    low.z = std::min(low.z, p.z);
    high.x = std::max(high.x, p.x);
    high.y = std::max(high.y, p.y);
    high.z = std::max(high.z, p.z);
}

f32 Component(const Vector3f& v, u32 axis) {
    return axis == 0 ? v.x : (axis == 1 ? v.y : v.z);
}

Vector3f Sub(const Vector3f& a, const Vector3f& b) {
    return Vector3f{a.x - b.x, a.y - b.y, a.z - b.z};
}

Vector3f Cross(const Vector3f& a, const Vector3f& b) {
    return Vector3f{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

f32 Dot(const Vector3f& a, const Vector3f& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

/// Möller-Trumbore, two-sided: an editor's ray has to hit a back face too.
bool RayTriangle(const Vector3f& origin, const Vector3f& direction, const Vector3f& a,
                 const Vector3f& b, const Vector3f& c, f32& t, f32& u, f32& v) {
    const Vector3f ab = Sub(b, a);
    const Vector3f ac = Sub(c, a);
    const Vector3f p = Cross(direction, ac);
    const f32 determinant = Dot(ab, p);
    // A grazing ray is parallel to the triangle: counted as a miss rather than
    // divided by zero, which is what turns a hover into a NaN.
    if (std::abs(determinant) < 1e-12f) {
        return false;
    }
    const f32 inverse = 1.0f / determinant;
    const Vector3f ao = Sub(origin, a);
    u = Dot(ao, p) * inverse;
    if (u < -1e-6f || u > 1.0f + 1e-6f) {
        return false;
    }
    const Vector3f q = Cross(ao, ab);
    v = Dot(direction, q) * inverse;
    if (v < -1e-6f || u + v > 1.0f + 1e-6f) {
        return false;
    }
    t = Dot(ac, q) * inverse;
    return std::isfinite(t);
}

/// The slab test, on a ray with precomputed reciprocals.
bool RayBox(const Vector3f& origin, const Vector3f& inverse, const Vector3f& low,
            const Vector3f& high, f32 tMin, f32 tMax) {
    for (u32 axis = 0; axis < 3; ++axis) {
        const f32 o = Component(origin, axis);
        const f32 d = Component(inverse, axis);
        f32 near = (Component(low, axis) - o) * d;
        f32 far = (Component(high, axis) - o) * d;
        if (near > far) {
            std::swap(near, far);
        }
        tMin = std::max(tMin, near);
        tMax = std::min(tMax, far);
        if (tMin > tMax) {
            return false;
        }
    }
    return true;
}

f32 BoxDistanceSquared(const Vector3f& from, const Vector3f& low, const Vector3f& high) {
    f32 total = 0.0f;
    for (u32 axis = 0; axis < 3; ++axis) {
        const f32 p = Component(from, axis);
        const f32 d = std::max({Component(low, axis) - p, 0.0f, p - Component(high, axis)});
        total += d * d;
    }
    return total;
}

/// The point on triangle `abc` nearest @p from, with its barycentrics.
Vector3f ClosestOnTriangle(const Vector3f& from, const Vector3f& a, const Vector3f& b,
                           const Vector3f& c, f32& u, f32& v) {
    const Vector3f ab = Sub(b, a);
    const Vector3f ac = Sub(c, a);
    const Vector3f ap = Sub(from, a);
    const f32 d1 = Dot(ab, ap);
    const f32 d2 = Dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) {
        u = 0.0f;
        v = 0.0f;
        return a;
    }
    const Vector3f bp = Sub(from, b);
    const f32 d3 = Dot(ab, bp);
    const f32 d4 = Dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) {
        u = 1.0f;
        v = 0.0f;
        return b;
    }
    const f32 vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        const f32 t = d1 / (d1 - d3);
        u = t;
        v = 0.0f;
        return Vector3f{a.x + ab.x * t, a.y + ab.y * t, a.z + ab.z * t};
    }
    const Vector3f cp = Sub(from, c);
    const f32 d5 = Dot(ab, cp);
    const f32 d6 = Dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) {
        u = 0.0f;
        v = 1.0f;
        return c;
    }
    const f32 vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        const f32 t = d2 / (d2 - d6);
        u = 0.0f;
        v = t;
        return Vector3f{a.x + ac.x * t, a.y + ac.y * t, a.z + ac.z * t};
    }
    const f32 va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        const f32 t = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        u = 1.0f - t;
        v = t;
        const Vector3f bc = Sub(c, b);
        return Vector3f{b.x + bc.x * t, b.y + bc.y * t, b.z + bc.z * t};
    }
    const f32 denominator = 1.0f / (va + vb + vc);
    u = vb * denominator;
    v = vc * denominator;
    return Vector3f{a.x + ab.x * u + ac.x * v, a.y + ab.y * u + ac.y * v,
                    a.z + ab.z * u + ac.z * v};
}

} // namespace

void TriangleBvh::build(std::span<const u32> indices, std::span<const Vector3f> positions) {
    indices_.assign(indices.begin(), indices.end());
    positions_.assign(positions.begin(), positions.end());
    nodes_.clear();
    order_.clear();
    const u32 triangles = triangleCount();
    if (triangles == 0) {
        return;
    }
    order_.resize(triangles);
    for (u32 t = 0; t < triangles; ++t) {
        order_[t] = t;
    }
    nodes_.reserve(triangles * 2);
    buildRange(0, triangles, positions_);
}

u32 TriangleBvh::buildRange(u32 begin, u32 end, std::span<const Vector3f> positions) {
    const u32 self = static_cast<u32>(nodes_.size());
    nodes_.emplace_back();
    Vector3f low{std::numeric_limits<f32>::max(), std::numeric_limits<f32>::max(),
                 std::numeric_limits<f32>::max()};
    Vector3f high{-std::numeric_limits<f32>::max(), -std::numeric_limits<f32>::max(),
                  -std::numeric_limits<f32>::max()};
    Vector3f centroidLow = low;
    Vector3f centroidHigh = high;
    for (u32 i = begin; i < end; ++i) {
        const u32 triangle = order_[i];
        Vector3f centroid{0, 0, 0};
        for (u32 k = 0; k < 3; ++k) {
            const u32 vertex = indices_[triangle * 3 + k];
            const Vector3f& p = positions[vertex];
            Grow(low, high, p);
            centroid.x += p.x / 3.0f;
            centroid.y += p.y / 3.0f;
            centroid.z += p.z / 3.0f;
        }
        Grow(centroidLow, centroidHigh, centroid);
    }
    nodes_[self].low = low;
    nodes_[self].high = high;

    if (end - begin <= kLeafSize) {
        nodes_[self].first = begin;
        nodes_[self].count = end - begin;
        return self;
    }

    // The widest axis of the centroids, split at their median: cheap, and it
    // keeps the tree balanced whatever the triangles' sizes are.
    const Vector3f span = Sub(centroidHigh, centroidLow);
    const u32 axis = span.x > span.y ? (span.x > span.z ? 0 : 2) : (span.y > span.z ? 1 : 2);
    const u32 middle = begin + (end - begin) / 2;
    std::nth_element(order_.begin() + begin, order_.begin() + middle, order_.begin() + end,
                     [&](u32 a, u32 b) {
                         const auto centroid = [&](u32 triangle) {
                             f32 sum = 0.0f;
                             for (u32 k = 0; k < 3; ++k) {
                                 sum += Component(positions[indices_[triangle * 3 + k]], axis);
                             }
                             return sum;
                         };
                         return centroid(a) < centroid(b);
                     });
    nodes_[self].count = 0;
    buildRange(begin, middle, positions);
    nodes_[self].right = buildRange(middle, end, positions);
    return self;
}

void TriangleBvh::refit(std::span<const Vector3f> positions) {
    if (positions.size() != positions_.size()) {
        return;
    }
    positions_.assign(positions.begin(), positions.end());
    if (!nodes_.empty()) {
        refitNode(0, positions_);
    }
}

void TriangleBvh::refitNode(u32 node, std::span<const Vector3f> positions) {
    Node& self = nodes_[node];
    Vector3f low{std::numeric_limits<f32>::max(), std::numeric_limits<f32>::max(),
                 std::numeric_limits<f32>::max()};
    Vector3f high{-std::numeric_limits<f32>::max(), -std::numeric_limits<f32>::max(),
                  -std::numeric_limits<f32>::max()};
    if (self.count != 0) {
        for (u32 i = self.first; i < self.first + self.count; ++i) {
            const u32 triangle = order_[i];
            for (u32 k = 0; k < 3; ++k) {
                Grow(low, high, positions[indices_[triangle * 3 + k]]);
            }
        }
    } else {
        refitNode(node + 1, positions);
        refitNode(self.right, positions);
        const Node& left = nodes_[node + 1];
        const Node& right = nodes_[self.right];
        Grow(low, high, left.low);
        Grow(low, high, left.high);
        Grow(low, high, right.low);
        Grow(low, high, right.high);
    }
    self.low = low;
    self.high = high;
}

Extent TriangleBvh::bounds() const {
    Extent out;
    ResetExtent(out);
    if (!nodes_.empty()) {
        GrowExtent(out, nodes_[0].low);
        GrowExtent(out, nodes_[0].high);
    }
    FinishExtent(out);
    return out;
}

RayHit TriangleBvh::raycast(const Vector3f& origin, const Vector3f& direction, f32 tMin,
                            f32 tMax) const {
    RayHit hit;
    if (nodes_.empty()) {
        return hit;
    }
    const auto reciprocal = [](f32 value) {
        // An axis the ray does not move along gives an infinite slab, which the
        // min/max below handle; zero would give a NaN.
        return value != 0.0f ? 1.0f / value : std::numeric_limits<f32>::infinity();
    };
    const Vector3f inverse{reciprocal(direction.x), reciprocal(direction.y),
                           reciprocal(direction.z)};
    f32 best = tMax;

    u32 stack[64];
    u32 depth = 0;
    stack[depth++] = 0;
    while (depth != 0) {
        const Node& node = nodes_[stack[--depth]];
        if (!RayBox(origin, inverse, node.low, node.high, tMin, best)) {
            continue;
        }
        if (node.count != 0) {
            for (u32 i = node.first; i < node.first + node.count; ++i) {
                const u32 triangle = order_[i];
                f32 t = 0.0f;
                f32 u = 0.0f;
                f32 v = 0.0f;
                if (RayTriangle(origin, direction, positions_[indices_[triangle * 3]],
                                positions_[indices_[triangle * 3 + 1]],
                                positions_[indices_[triangle * 3 + 2]], t, u, v) &&
                    t >= tMin && t <= best) {
                    best = t;
                    hit.triangle = triangle;
                    hit.distance = t;
                    hit.u = u;
                    hit.v = v;
                }
            }
            continue;
        }
        if (depth + 2 <= 64) {
            stack[depth++] = static_cast<u32>(&node - nodes_.data()) + 1;
            stack[depth++] = node.right;
        }
    }
    return hit;
}

PointHit TriangleBvh::closestPoint(const Vector3f& from, f32 maxDistance) const {
    PointHit hit;
    if (nodes_.empty()) {
        return hit;
    }
    f32 best = maxDistance == std::numeric_limits<f32>::max()
                   ? std::numeric_limits<f32>::max()
                   : maxDistance * maxDistance;

    u32 stack[64];
    u32 depth = 0;
    stack[depth++] = 0;
    while (depth != 0) {
        const u32 index = stack[--depth];
        const Node& node = nodes_[index];
        if (BoxDistanceSquared(from, node.low, node.high) > best) {
            continue;
        }
        if (node.count != 0) {
            for (u32 i = node.first; i < node.first + node.count; ++i) {
                const u32 triangle = order_[i];
                f32 u = 0.0f;
                f32 v = 0.0f;
                const Vector3f on = ClosestOnTriangle(
                    from, positions_[indices_[triangle * 3]], positions_[indices_[triangle * 3 + 1]],
                    positions_[indices_[triangle * 3 + 2]], u, v);
                const Vector3f offset = Sub(on, from);
                const f32 distance = Dot(offset, offset);
                if (distance < best) {
                    best = distance;
                    hit.triangle = triangle;
                    hit.position = on;
                    hit.distance = std::sqrt(distance);
                    hit.u = u;
                    hit.v = v;
                }
            }
            continue;
        }
        if (depth + 2 <= 64) {
            // The nearer child first, so the far one is usually cut.
            const u32 left = index + 1;
            const u32 right = node.right;
            const f32 toLeft = BoxDistanceSquared(from, nodes_[left].low, nodes_[left].high);
            const f32 toRight = BoxDistanceSquared(from, nodes_[right].low, nodes_[right].high);
            if (toLeft < toRight) {
                stack[depth++] = right;
                stack[depth++] = left;
            } else {
                stack[depth++] = left;
                stack[depth++] = right;
            }
        }
    }
    return hit;
}

} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
