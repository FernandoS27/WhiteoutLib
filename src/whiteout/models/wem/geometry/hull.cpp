// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/geometry/hull.h>

#include <whiteout/models/m3/physics_cook.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace whiteout {
namespace models {
namespace wem {
namespace geom {

namespace {

bool Finite(const Vector3f& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

/// The extreme tetrahedron's corners, as the cooker's hull starts: the ends of
/// the widest axis, the point farthest from their line, and the one farthest
/// from that plane. Fewer than four when the points span no volume.
std::vector<u32> Tetrahedron(std::span<const Vector3f> p, f32 eps) {
    std::array<u32, 3> lo{0, 0, 0}, hi{0, 0, 0};
    for (u32 i = 1; i < p.size(); ++i)
        for (int a = 0; a < 3; ++a) {
            const f32 c = a == 0 ? p[i].x : a == 1 ? p[i].y : p[i].z;
            const auto at = [&](u32 k) { return a == 0 ? p[k].x : a == 1 ? p[k].y : p[k].z; };
            if (c < at(lo[a]))
                lo[a] = i;
            if (c > at(hi[a]))
                hi[a] = i;
        }
    int axis = 0;
    f32 widest = -1.0f;
    for (int a = 0; a < 3; ++a) {
        const f32 span = (p[hi[a]] - p[lo[a]]).length();
        if (span > widest) {
            widest = span;
            axis = a;
        }
    }
    std::vector<u32> out{lo[axis]};
    if (hi[axis] == lo[axis] || widest <= eps)
        return out;
    out.push_back(hi[axis]);
    const Vector3f a = p[out[0]], dir = (p[out[1]] - a).normalized();
    f32 best = 0.0f;
    u32 third = out[0];
    for (u32 i = 0; i < p.size(); ++i) {
        const Vector3f d = p[i] - a;
        const f32 off = (d - dir * d.dot(dir)).length();
        if (off > best) {
            best = off;
            third = i;
        }
    }
    if (best <= eps)
        return out;
    out.push_back(third);
    const Vector3f normal = cross(p[out[1]] - a, p[third] - a).normalized();
    best = 0.0f;
    u32 fourth = out[0];
    for (u32 i = 0; i < p.size(); ++i) {
        const f32 off = std::abs((p[i] - a).dot(normal));
        if (off > best) {
            best = off;
            fourth = i;
        }
    }
    if (best > eps)
        out.push_back(fourth);
    return out;
}

} // namespace

std::vector<Vector3f> HullWithin(std::span<const Vector3f> points, u32 maxPoints) {
    std::vector<Vector3f> input;
    for (const Vector3f& v : points)
        if (Finite(v) && std::none_of(input.begin(), input.end(), [&](const Vector3f& u) { return u == v; }))
            input.push_back(v);
    const u32 budget = std::max<u32>(maxPoints, 4);
    if (input.size() <= 4) {
        input.resize(std::min<std::size_t>(input.size(), budget));
        return input;
    }
    Vector3f low = input[0], high = input[0];
    for (const Vector3f& v : input) {
        low = Vector3f{std::min(low.x, v.x), std::min(low.y, v.y), std::min(low.z, v.z)};
        high = Vector3f{std::max(high.x, v.x), std::max(high.y, v.y), std::max(high.z, v.z)};
    }
    // What counts as outside: the cooker's own relative tolerance, a hair
    // over a float's precision at the hull's size.
    const f32 eps = std::max((high - low).length(), 1e-6f) * 1e-5f;
    std::vector<u32> kept = Tetrahedron(input, eps);
    if (kept.size() < 4) {
        std::vector<Vector3f> out;
        for (std::size_t i = 0; i < input.size() && out.size() < budget; ++i)
            out.push_back(input[i]);
        return out;
    }
    std::vector<u8> used(input.size(), 0);
    for (const u32 k : kept)
        used[k] = 1;
    std::vector<Vector3f> hull;
    std::vector<std::array<u32, 3>> triangles;
    while (kept.size() < budget) {
        hull.clear();
        for (const u32 k : kept)
            hull.push_back(input[k]);
        if (!m3::HullTriangles(hull, triangles))
            break;
        f32 farthest = eps;
        u32 next = 0;
        bool any = false;
        for (u32 i = 0; i < input.size(); ++i) {
            if (used[i] != 0)
                continue;
            // How far outside the nearest face plane that sees it.
            f32 out = -1e30f;
            for (const std::array<u32, 3>& t : triangles) {
                const Vector3f n = cross(hull[t[1]] - hull[t[0]], hull[t[2]] - hull[t[0]]);
                const f32 length = n.length();
                if (length > 0.0f)
                    out = std::max(out, n.dot(input[i] - hull[t[0]]) / length);
            }
            if (out > farthest) {
                farthest = out;
                next = i;
                any = true;
            }
        }
        if (!any)
            break;
        kept.push_back(next);
        used[next] = 1;
    }
    std::vector<Vector3f> out;
    for (const u32 k : kept)
        out.push_back(input[k]);
    return out;
}

} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
