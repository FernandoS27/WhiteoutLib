// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/geometry/fracture/winding.h>

#include <algorithm>
#include <cmath>
#include <numeric>

#include <whiteout/models/wem/geometry/attributes.h>
#include <whiteout/models/wem/geometry/mesh.h>
#include <whiteout/models/wem/geometry/triangulation.h>

namespace whiteout {
namespace models {
namespace wem {
namespace geom {
namespace fracture {

namespace {

constexpr f64 kFourPi = 12.566370614359172953850573533118;

/// A cluster seen from more than this many of its radii away is one dipole.
constexpr f64 kFarRadii = 2.0;
constexpr u32 kLeafTriangles = 8;
/// Below this many triangles the sum is exact: there is nothing to save.
constexpr std::size_t kTreeFrom = 256;

Vector3d ToDouble(const Vector3f& v) {
    return Vector3d(v.x, v.y, v.z);
}

f64 Dot(const Vector3d& a, const Vector3d& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

f64 Length(const Vector3d& a) {
    return std::sqrt(Dot(a, a));
}

f64 SolidAngle(const WindingMesh& target, std::size_t t, const Vector3d& point) {
    const Vector3d a = target.corners[3 * t] - point;
    const Vector3d b = target.corners[3 * t + 1] - point;
    const Vector3d c = target.corners[3 * t + 2] - point;
    const f64 la = Length(a);
    const f64 lb = Length(b);
    const f64 lc = Length(c);
    const f64 numerator = Dot(a, cross(b, c));
    const f64 denominator = la * lb * lc + Dot(a, b) * lc + Dot(a, c) * lb + Dot(b, c) * la;
    return 2.0 * std::atan2(numerator, denominator);
}

u32 Build(WindingMesh& out, std::span<u32> order, u32 begin, u32 end) {
    const u32 index = static_cast<u32>(out.tree.size());
    out.tree.emplace_back();
    WindingNode node;
    Vector3d low(1e300, 1e300, 1e300);
    Vector3d high(-1e300, -1e300, -1e300);
    f64 area = 0.0;
    Vector3d weighted(0, 0, 0);
    for (u32 i = begin; i < end; ++i) {
        const u32 t = order[i];
        const Vector3d& a = out.corners[3 * t];
        const Vector3d& b = out.corners[3 * t + 1];
        const Vector3d& c = out.corners[3 * t + 2];
        const Vector3d n = cross(b - a, c - a) * 0.5;
        const f64 s = Length(n);
        node.normal += n;
        area += s;
        weighted += (a + b + c) * (s / 3.0);
        for (const Vector3d* p : {&a, &b, &c}) {
            low = Vector3d(std::min(low.x, p->x), std::min(low.y, p->y), std::min(low.z, p->z));
            high = Vector3d(std::max(high.x, p->x), std::max(high.y, p->y), std::max(high.z, p->z));
        }
    }
    node.centre = area > 0.0 ? weighted * (1.0 / area) : (low + high) * 0.5;
    for (u32 i = begin; i < end; ++i) {
        const u32 t = order[i];
        for (u32 k = 0; k < 3; ++k) {
            node.radius = std::max(node.radius, Length(out.corners[3 * t + k] - node.centre));
        }
    }
    if (end - begin <= kLeafTriangles) {
        node.first = begin;
        node.count = end - begin;
        out.tree[index] = node;
        return index;
    }
    // Split at the median along the longest side of the centroids' box.
    Vector3d cl(1e300, 1e300, 1e300);
    Vector3d ch(-1e300, -1e300, -1e300);
    const auto centroid = [&](u32 t) {
        return (out.corners[3 * t] + out.corners[3 * t + 1] + out.corners[3 * t + 2]) * (1.0 / 3.0);
    };
    for (u32 i = begin; i < end; ++i) {
        const Vector3d m = centroid(order[i]);
        cl = Vector3d(std::min(cl.x, m.x), std::min(cl.y, m.y), std::min(cl.z, m.z));
        ch = Vector3d(std::max(ch.x, m.x), std::max(ch.y, m.y), std::max(ch.z, m.z));
    }
    const Vector3d size = ch - cl;
    const u32 axis = size.x >= size.y && size.x >= size.z ? 0 : (size.y >= size.z ? 1 : 2);
    const u32 middle = begin + (end - begin) / 2;
    std::nth_element(order.begin() + begin, order.begin() + middle, order.begin() + end, [&](u32 l, u32 r) {
        const f64 a = centroid(l).data[axis];
        const f64 b = centroid(r).data[axis];
        return a != b ? a < b : l < r;
    });
    Build(out, order, begin, middle);
    node.right = Build(out, order, middle, end);
    out.tree[index] = node;
    return index;
}

} // namespace

WindingMesh WindingMeshOf(const Mesh& mesh) {
    WindingMesh out;
    ResetExtent(out.bounds);
    const auto positions = mesh.attributes.get<Vector3f>(names::kPosition, Domain::Vertex);
    std::vector<u32> triangles;
    TriangulateMesh(mesh, triangles);
    out.corners.reserve(triangles.size());
    for (u32 v : triangles) {
        const Vector3f p = v < positions.size() ? positions[v] : Vector3f{0, 0, 0};
        out.corners.push_back(ToDouble(p));
        GrowExtent(out.bounds, p);
    }
    FinishExtent(out.bounds);
    const std::size_t count = out.corners.size() / 3;
    if (count >= kTreeFrom) {
        out.order.resize(count);
        std::iota(out.order.begin(), out.order.end(), 0u);
        Build(out, out.order, 0, static_cast<u32>(count));
    }
    return out;
}

f64 WindingNumber(const WindingMesh& target, const Vector3d& point) {
    f64 sum = 0.0;
    if (target.tree.empty()) {
        const std::size_t count = target.corners.size() / 3;
        for (std::size_t t = 0; t < count; ++t) {
            sum += SolidAngle(target, t, point);
        }
        return target.sign * sum / kFourPi;
    }
    u32 stack[64];
    u32 depth = 0;
    stack[depth++] = 0;
    while (depth > 0) {
        const WindingNode& node = target.tree[stack[--depth]];
        const Vector3d r = node.centre - point;
        const f64 d = Length(r);
        if (d > kFarRadii * node.radius) {
            sum += Dot(node.normal, r) / (d * d * d);
        } else if (node.count > 0) {
            for (u32 i = node.first; i < node.first + node.count; ++i) {
                sum += SolidAngle(target, target.order[i], point);
            }
        } else if (depth + 2 <= 64) {
            stack[depth++] = node.right;
            stack[depth++] = static_cast<u32>(&node - target.tree.data()) + 1;
        }
    }
    return target.sign * sum / kFourPi;
}

bool OrientWinding(WindingMesh& target, std::span<const Vector3d> probes) {
    target.sign = 1.0;
    u32 positive = 0;
    u32 negative = 0;
    for (const Vector3d& p : probes) {
        const f64 w = WindingNumber(target, p);
        if (w >= 0.5) {
            ++positive;
        } else if (w <= -0.5) {
            ++negative;
        }
    }
    if (negative > positive) {
        target.sign = -1.0;
        return true;
    }
    return false;
}

} // namespace fracture
} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
