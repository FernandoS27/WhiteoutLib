// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/geometry/fit.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace whiteout {
namespace models {
namespace wem {
namespace geom {

namespace {

/// The direction @p points spread furthest along about @p mean, by power
/// iteration on their scatter.
Vector3f PrincipalAxis(std::span<const Vector3f> points, const Vector3f& mean) {
    std::array<f32, 9> c{};
    for (const Vector3f& p : points) {
        const std::array<f32, 3> d{p.x - mean.x, p.y - mean.y, p.z - mean.z};
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                c[i * 3 + j] += d[i] * d[j];
    }
    Vector3f axis{1, 1, 1};
    for (int step = 0; step < 32; ++step) {
        const Vector3f next{c[0] * axis.x + c[1] * axis.y + c[2] * axis.z, c[3] * axis.x + c[4] * axis.y + c[5] * axis.z,
                            c[6] * axis.x + c[7] * axis.y + c[8] * axis.z};
        if (next.length() < 1e-12f)
            return Vector3f{0, 0, 1};
        axis = next.normalized();
    }
    return axis;
}

/// The value @p share of the way up @p values.
f32 Quantile(std::vector<f32>& values, f32 share) {
    const std::size_t at = std::min(values.size() - 1, static_cast<std::size_t>(share * static_cast<f32>(values.size())));
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(at), values.end());
    return values[at];
}

} // namespace

OrientedBox FitOrientedBox(std::span<const Vector3f> points, const Vector3f& axis, f32 share) {
    OrientedBox box;
    if (points.empty())
        return box;
    share = std::clamp(share, 0.05f, 1.0f);
    Vector3f mean{0, 0, 0};
    for (const Vector3f& p : points)
        mean = mean + p;
    mean = mean * (1.0f / static_cast<f32>(points.size()));
    const Vector3f z = axis.length() > 1e-8f ? axis.normalized() : PrincipalAxis(points, mean);
    // The scatter across the axis, and its two principal directions there.
    const Vector3f helper = std::abs(z.z) < 0.9f ? Vector3f{0, 0, 1} : Vector3f{1, 0, 0};
    const Vector3f u = cross(helper, z).normalized(), v = cross(z, u);
    f32 suu = 0.0f, suv = 0.0f, svv = 0.0f;
    for (const Vector3f& p : points) {
        const Vector3f d = p - mean;
        suu += d.dot(u) * d.dot(u);
        suv += d.dot(u) * d.dot(v);
        svv += d.dot(v) * d.dot(v);
    }
    const f32 angle = 0.5f * std::atan2(2.0f * suv, suu - svv);
    const Vector3f major = (u * std::cos(angle) + v * std::sin(angle)).normalized();
    box.axes = {major, cross(z, major), z};

    std::vector<std::array<f32, 3>> local;
    local.reserve(points.size());
    std::array<f32, 3> lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    for (const Vector3f& p : points) {
        const Vector3f d = p - mean;
        const std::array<f32, 3> q{d.dot(box.axes[0]), d.dot(box.axes[1]), d.dot(box.axes[2])};
        for (int a = 0; a < 3; ++a) {
            lo[a] = std::min(lo[a], q[a]);
            hi[a] = std::max(hi[a], q[a]);
        }
        local.push_back(q);
    }
    f32 size = 0.0f;
    for (int a = 0; a < 3; ++a)
        size = std::max(size, hi[a] - lo[a]);
    const f32 floor = std::max(size * 1e-4f, 1e-6f);
    // Each point to the face it lies nearest, measured against the box so
    // far, and each face where the share of its points lies within it. A
    // box's own surface stands still on the first pass.
    for (int pass = 0; pass < 4; ++pass) {
        std::array<f32, 3> centre{}, half{};
        for (int a = 0; a < 3; ++a) {
            centre[a] = (lo[a] + hi[a]) * 0.5f;
            half[a] = std::max((hi[a] - lo[a]) * 0.5f, floor);
        }
        std::array<std::vector<f32>, 6> faces;
        for (const std::array<f32, 3>& q : local) {
            int best = 0;
            f32 reach = -1.0f;
            for (int a = 0; a < 3; ++a) {
                const f32 r = std::abs(q[a] - centre[a]) / half[a];
                if (r > reach) {
                    reach = r;
                    best = a;
                }
            }
            if (q[best] >= centre[best])
                faces[best * 2].push_back(q[best]);
            else
                faces[best * 2 + 1].push_back(-q[best]);
        }
        for (int a = 0; a < 3; ++a) {
            if (!faces[a * 2].empty())
                hi[a] = Quantile(faces[a * 2], share);
            if (!faces[a * 2 + 1].empty())
                lo[a] = -Quantile(faces[a * 2 + 1], share);
            if (hi[a] < lo[a])
                std::swap(hi[a], lo[a]);
        }
    }
    box.centre = mean;
    for (int a = 0; a < 3; ++a)
        box.centre = box.centre + box.axes[a] * ((lo[a] + hi[a]) * 0.5f);
    box.halfExtents = Vector3f{std::max((hi[0] - lo[0]) * 0.5f, floor), std::max((hi[1] - lo[1]) * 0.5f, floor),
                               std::max((hi[2] - lo[2]) * 0.5f, floor)};
    return box;
}

} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
