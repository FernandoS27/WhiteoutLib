// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/geometry/lattice.h>

#include <algorithm>
#include <cfloat>
#include <cstddef>

namespace whiteout {
namespace models {
namespace wem {
namespace geom {

namespace {

/// The Bernstein basis of degree `count - 1` at @p t, into @p out.
void Bernstein(u32 count, f32 t, f32* out) {
    const u32 n = count - 1;
    const f32 u = 1.0f - t;
    // Pascal's row times the powers: small enough to build per call.
    f32 binomial = 1.0f;
    for (u32 i = 0; i <= n; ++i) {
        f32 term = binomial;
        for (u32 a = 0; a < i; ++a)
            term *= t;
        for (u32 b = i; b < n; ++b)
            term *= u;
        out[i] = term;
        binomial = binomial * static_cast<f32>(n - i) / static_cast<f32>(i + 1);
    }
}

} // namespace

Vector3f Lattice::rest(u32 index) const {
    const u32 i = index % points[0];
    const u32 j = (index / points[0]) % points[1];
    const u32 k = index / (points[0] * points[1]);
    const u32 at[3] = {i, j, k};
    Vector3f p = origin;
    for (u32 a = 0; a < 3; ++a)
        p = p + axis[a] * (size.data[a] * static_cast<f32>(at[a]) / static_cast<f32>(points[a] - 1));
    return p;
}

Lattice FitLattice(std::span<const Vector3f> positions, const Vector3f (&axes)[3], const u32 (&points)[3]) {
    Lattice lattice;
    Vector3f low{FLT_MAX, FLT_MAX, FLT_MAX};
    Vector3f high{-FLT_MAX, -FLT_MAX, -FLT_MAX};
    for (u32 a = 0; a < 3; ++a) {
        lattice.axis[a] = axes[a];
        lattice.points[a] = std::clamp(points[a], 2u, kMaxLatticePoints);
        for (const Vector3f& p : positions) {
            const f32 d = p.dot(axes[a]);
            low.data[a] = std::min(low.data[a], d);
            high.data[a] = std::max(high.data[a], d);
        }
        if (positions.empty())
            low.data[a] = high.data[a] = 0.0f;
    }
    const f32 widest = std::max({high.x - low.x, high.y - low.y, high.z - low.z});
    // A flat side is given some depth, centred on where it is: a box of no
    // thickness has no inside to deform.
    const f32 least = widest > 1e-6f ? widest * 0.1f : 1.0f;
    for (u32 a = 0; a < 3; ++a) {
        if (high.data[a] - low.data[a] < least * 0.1f) {
            const f32 middle = (high.data[a] + low.data[a]) * 0.5f;
            low.data[a] = middle - least * 0.5f;
            high.data[a] = middle + least * 0.5f;
        }
        lattice.size.data[a] = high.data[a] - low.data[a];
    }
    lattice.origin = axes[0] * low.x + axes[1] * low.y + axes[2] * low.z;
    lattice.controls.resize(lattice.count());
    for (u32 c = 0; c < lattice.count(); ++c)
        lattice.controls[c] = lattice.rest(c);
    return lattice;
}

Vector3f LatticeCoordinates(const Lattice& lattice, const Vector3f& p) {
    const Vector3f d = p - lattice.origin;
    Vector3f stu;
    for (u32 a = 0; a < 3; ++a)
        stu.data[a] = d.dot(lattice.axis[a]) / lattice.size.data[a];
    return stu;
}

void DeformByLattice(const Lattice& lattice, std::span<const Vector3f> base,
                     std::span<const Vector3f> coordinates, std::span<Vector3f> out) {
    // Only the controls that moved pull on anything, and a drag moves a few.
    struct Moved {
        u32 i, j, k;
        Vector3f by;
    };
    std::vector<Moved> moved;
    for (u32 c = 0; c < lattice.count() && c < lattice.controls.size(); ++c) {
        const Vector3f by = lattice.controls[c] - lattice.rest(c);
        if (by.x != 0.0f || by.y != 0.0f || by.z != 0.0f)
            moved.push_back({c % lattice.points[0], (c / lattice.points[0]) % lattice.points[1],
                             c / (lattice.points[0] * lattice.points[1]), by});
    }
    const std::size_t n = std::min({base.size(), coordinates.size(), out.size()});
    f32 weights[3][kMaxLatticePoints];
    for (std::size_t v = 0; v < n; ++v) {
        Vector3f at = base[v];
        if (!moved.empty()) {
            for (u32 a = 0; a < 3; ++a)
                Bernstein(lattice.points[a], coordinates[v].data[a], weights[a]);
            for (const Moved& m : moved)
                at = at + m.by * (weights[0][m.i] * weights[1][m.j] * weights[2][m.k]);
        }
        out[v] = at;
    }
}

} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
