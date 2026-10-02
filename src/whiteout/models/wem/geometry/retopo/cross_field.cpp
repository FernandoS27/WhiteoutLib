// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "cross_field.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "sparse.h"

namespace whiteout {
namespace models {
namespace wem {
namespace geom {
namespace retopo {

namespace {

struct Complex {
    f64 re = 0.0;
    f64 im = 0.0;
};

Complex Polar(f64 angle) {
    return Complex{std::cos(angle), std::sin(angle)};
}

Complex Mul(const Complex& a, const Complex& b) {
    return Complex{a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re};
}

/// Adds the complex entry @p value at (@p row, @p column) of the Hermitian
/// matrix to its real symmetric form, unknown k at rows 2k (re) and 2k + 1 (im).
void AddComplex(SparseBuilder& builder, u32 row, u32 column, const Complex& value) {
    builder.add(2 * row, 2 * column, value.re);
    builder.add(2 * row, 2 * column + 1, -value.im);
    builder.add(2 * row + 1, 2 * column, value.im);
    builder.add(2 * row + 1, 2 * column + 1, value.re);
}

/// A symmetric 2x2 tensor in a triangle's frame.
struct Tensor {
    f64 a = 0.0;
    f64 b = 0.0;
    f64 c = 0.0;
};

/// @p t's tensor as seen in a frame turned by @p angle.
Tensor Rotate(const Tensor& t, f64 angle) {
    const f64 cs = std::cos(angle);
    const f64 sn = std::sin(angle);
    // R T R^T with R = [cs -sn; sn cs].
    const f64 a = cs * cs * t.a - 2.0 * cs * sn * t.b + sn * sn * t.c;
    const f64 b = cs * sn * (t.a - t.c) + (cs * cs - sn * sn) * t.b;
    const f64 c = sn * sn * t.a + 2.0 * cs * sn * t.b + cs * cs * t.c;
    return Tensor{a, b, c};
}

/// Rusinkiewicz's per-face shape operator: the symmetric tensor that best maps
/// each edge to the change of normal along it.
Tensor ShapeOperator(const V3 p[3], const V3 n[3], const V3& x, const V3& y) {
    f64 m[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
    f64 r[3] = {0, 0, 0};
    for (u32 i = 0; i < 3; ++i) {
        const V3 e = p[(i + 2) % 3] - p[(i + 1) % 3];
        const V3 dn = n[(i + 2) % 3] - n[(i + 1) % 3];
        const f64 eu = Dot(e, x);
        const f64 ev = Dot(e, y);
        const f64 du = Dot(dn, x);
        const f64 dv = Dot(dn, y);
        // Rows [eu ev 0] -> du and [0 eu ev] -> dv.
        const f64 rows[2][3] = {{eu, ev, 0.0}, {0.0, eu, ev}};
        const f64 rhs[2] = {du, dv};
        for (u32 k = 0; k < 2; ++k) {
            for (u32 i0 = 0; i0 < 3; ++i0) {
                for (u32 j0 = 0; j0 < 3; ++j0) {
                    m[i0][j0] += rows[k][i0] * rows[k][j0];
                }
                r[i0] += rows[k][i0] * rhs[k];
            }
        }
    }
    const f64 det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
                    m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                    m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    if (std::abs(det) < 1e-30) {
        return {};
    }
    auto solve = [&](u32 column) {
        f64 a[3][3];
        for (u32 i0 = 0; i0 < 3; ++i0) {
            for (u32 j0 = 0; j0 < 3; ++j0) {
                a[i0][j0] = j0 == column ? r[i0] : m[i0][j0];
            }
        }
        return (a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) -
                a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
                a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0])) /
               det;
    };
    return Tensor{solve(0), solve(1), solve(2)};
}

} // namespace

V3 CrossField::direction(u32 t, u32 k) const {
    const f64 angle = angles[t] + k * (kPi * 0.5);
    return axisX[t] * std::cos(angle) + axisY[t] * std::sin(angle);
}

f64 CrossField::angleOf(u32 t, const V3& v) const {
    return std::atan2(Dot(v, axisY[t]), Dot(v, axisX[t]));
}

u32 CrossField::singularityCount() const {
    u32 count = 0;
    for (i32 i : index) {
        count += i != 0 ? 1 : 0;
    }
    return count;
}

f64 WrapQuarter(f64 angle) {
    const f64 quarter = kPi * 0.5;
    f64 wrapped = std::fmod(angle + quarter * 0.5, quarter);
    if (wrapped <= 0.0) {
        wrapped += quarter;
    }
    return wrapped - quarter * 0.5;
}

namespace {

/// The field's index at @p v in quarter turns; 0 on a border.
i32 VertexIndex(const WorkMesh& mesh, const CrossField& field, u32 v, std::vector<u32>& ring) {
    if (!mesh.alive(v) || !mesh.ring(v, ring) || ring.size() < 3) {
        return 0;
    }
    // The frames turn by the curvature they enclose on the way round v
    // counter-clockwise; the cross turns by that less its own winding.
    f64 holonomy = 0.0;
    f64 rotation = 0.0;
    f64 angles = 0.0;
    const V3& p = mesh.positions[v];
    for (const u32 o : ring) {
        const u32 t = o / 3;
        // Into the next triangle round v, across the edge v shares with it.
        const u32 across = WorkMesh::Prev(o);
        const u32 s = mesh.twins[across] / 3;
        const f64 transport = Transport(mesh, field, across);
        holonomy += transport;
        rotation += WrapQuarter(field.angles[s] - (field.angles[t] + transport));
        angles += Angle(mesh.positions[mesh.to(o)] - p, mesh.positions[mesh.from(across)] - p);
    }
    // The holonomy is the curvature only up to whole turns, and the turn the
    // facets' angle defect is nearest is the one: a cone's tip sharper than
    // half a turn encloses more than pi, and a wrap to [-pi, pi] would read
    // it as a saddle.
    const f64 defect = 2.0 * kPi - angles;
    const f64 curvature = holonomy + 2.0 * kPi * std::round((defect - holonomy) / (2.0 * kPi));
    return static_cast<i32>(std::llround((rotation + curvature) / (kPi * 0.5)));
}

} // namespace

V3 FeatureDirection(const WorkMesh& mesh, const Surface& surface, u32 h) {
    const V3 edge = mesh.positions[mesh.to(h)] - mesh.positions[mesh.from(h)];
    if (surface.tangents.empty()) {
        return edge;
    }
    for (const u32 v : {mesh.from(h), mesh.to(h)}) {
        const FeaturePoint& at = mesh.onCurve[v];
        if (!at.valid() || surface.curves[at.halfedge] != mesh.curves[h]) {
            continue;
        }
        const V3& tangent = surface.tangents[at.halfedge];
        if (Length2(tangent) > 0.0) {
            return Dot(tangent, edge) < 0.0 ? tangent * -1.0 : tangent;
        }
    }
    return edge;
}

void SetFrame(const WorkMesh& mesh, const Surface& surface, CrossField& field, u32 t) {
    const V3& p0 = mesh.positions[mesh.corners[3 * t]];
    const V3& p1 = mesh.positions[mesh.corners[3 * t + 1]];
    const V3& p2 = mesh.positions[mesh.corners[3 * t + 2]];
    const V3 facet = TriangleNormal(p0, p1, p2);
    V3 n{0.0, 0.0, 0.0};
    for (u32 i = 0; i < 3; ++i) {
        n = n + surface.normal(mesh.homes[mesh.corners[3 * t + i]]);
    }
    // Shading that leans more than about 30 degrees off the facet is lighting,
    // not this surface (a low-poly cape's is often 60 off): the facet's own
    // plane then. On the corpus it halves such a piece's singularities.
    if (Length2(n) == 0.0 || (Length2(facet) > 0.0 && Dot(Unit(n), facet) < 0.85)) {
        n = facet;
    }
    if (Length2(n) == 0.0) {
        n = surface.normal(mesh.homes[mesh.corners[3 * t]]);
    }
    n = Unit(n);
    field.normals[t] = n;
    field.axisX[t] = Unit(Tangent(p1 - p0, n));
    field.axisY[t] = Cross(n, field.axisX[t]);
}

f64 Transport(const WorkMesh& mesh, const CrossField& field, u32 h) {
    const u32 twin = mesh.twins[h];
    const V3 edge = mesh.positions[mesh.to(h)] - mesh.positions[mesh.from(h)];
    return field.angleOf(twin / 3, edge) - field.angleOf(h / 3, edge);
}

CrossField SolveCrossField(const WorkMesh& mesh, const Surface& surface, const FieldOptions& options) {
    CrossField field;
    const u32 triangles = mesh.triangleCount();
    field.axisX.resize(triangles);
    field.axisY.resize(triangles);
    field.normals.resize(triangles);
    field.angles.assign(triangles, 0.0);
    field.constrained.assign(triangles, 0);
    for (u32 t = 0; t < triangles; ++t) {
        SetFrame(mesh, surface, field, t);
    }

    // Known triangles: one feature edge, the field along it.
    std::vector<Complex> known(triangles);
    for (u32 t = 0; t < triangles; ++t) {
        if (mesh.dead[t]) {
            continue;
        }
        for (u32 i = 0; i < 3; ++i) {
            const u32 h = 3 * t + i;
            if (mesh.isFeature(h)) {
                known[t] = Polar(4.0 * field.angleOf(t, FeatureDirection(mesh, surface, h)));
                field.constrained[t] = 1;
                break;
            }
        }
    }

    // The curvature pull, from smoothed shape operators.
    std::vector<f64> pull(triangles, 0.0);
    std::vector<Complex> toward(triangles);
    if (options.curvature > 0.0) {
        std::vector<Tensor> shape(triangles);
        for (u32 t = 0; t < triangles; ++t) {
            if (mesh.dead[t]) {
                continue;
            }
            V3 p[3];
            V3 n[3];
            for (u32 i = 0; i < 3; ++i) {
                const u32 v = mesh.corners[3 * t + i];
                p[i] = mesh.positions[v];
                n[i] = surface.normal(mesh.homes[v]);
            }
            shape[t] = ShapeOperator(p, n, field.axisX[t], field.axisY[t]);
        }
        for (u32 pass = 0; pass < options.curvatureSmoothing; ++pass) {
            std::vector<Tensor> next = shape;
            for (u32 t = 0; t < triangles; ++t) {
                if (mesh.dead[t]) {
                    continue;
                }
                Tensor sum = shape[t];
                f64 count = 1.0;
                for (u32 i = 0; i < 3; ++i) {
                    const u32 h = 3 * t + i;
                    const u32 twin = mesh.twins[h];
                    if (twin == kNone || mesh.isFeature(h)) {
                        continue;
                    }
                    // The neighbour's tensor turned into this frame.
                    const Tensor turned = Rotate(shape[twin / 3], -Transport(mesh, field, h));
                    sum.a += turned.a;
                    sum.b += turned.b;
                    sum.c += turned.c;
                    count += 1.0;
                }
                next[t] = Tensor{sum.a / count, sum.b / count, sum.c / count};
            }
            shape.swap(next);
        }
        for (u32 t = 0; t < triangles; ++t) {
            const Tensor& s = shape[t];
            // |k1 - k2|, and the same over |k1| + |k2|: an umbilic region (a
            // sphere, a flat plate) has directions that are only noise.
            const f64 anisotropy = std::sqrt((s.a - s.c) * (s.a - s.c) + 4.0 * s.b * s.b);
            const f64 trace = std::abs(s.a + s.c);
            const f64 magnitude = std::max(anisotropy, trace);
            const f64 relative = magnitude > 0.0 ? anisotropy / magnitude : 0.0;
            pull[t] = options.curvature * std::min(1.0, anisotropy * options.scale) * relative * relative;
            const f64 principal = 0.5 * std::atan2(2.0 * s.b, s.a - s.c);
            toward[t] = Polar(4.0 * principal);
        }
    }

    // Unknowns: the free live triangles.
    std::vector<u32> unknown(triangles, kNone);
    u32 count = 0;
    for (u32 t = 0; t < triangles; ++t) {
        if (!mesh.dead[t] && !field.constrained[t]) {
            unknown[t] = count++;
        }
    }
    std::vector<Complex> solved(triangles);
    if (count > 0) {
        SparseBuilder builder(2 * count);
        std::vector<f64> rhs(2 * count, 0.0);
        f64 pullTotal = 0.0;
        bool anyKnown = false;
        for (u32 h = 0; h < mesh.corners.size(); ++h) {
            const u32 twin = mesh.twins[h];
            if (mesh.dead[h / 3] || twin == kNone || twin < h) {
                continue;
            }
            const u32 t = h / 3;
            const u32 s = twin / 3;
            // u_s ~ r u_t across the edge.
            const Complex r = Polar(4.0 * Transport(mesh, field, h));
            const Complex rConj{r.re, -r.im};
            const u32 ut = unknown[t];
            const u32 us = unknown[s];
            if (ut != kNone) {
                AddComplex(builder, ut, ut, Complex{1.0, 0.0});
            }
            if (us != kNone) {
                AddComplex(builder, us, us, Complex{1.0, 0.0});
            }
            if (ut != kNone && us != kNone) {
                AddComplex(builder, us, ut, Complex{-r.re, -r.im});
                AddComplex(builder, ut, us, Complex{-rConj.re, -rConj.im});
            } else if (us != kNone) {
                const Complex moved = Mul(r, known[t]);
                rhs[2 * us] += moved.re;
                rhs[2 * us + 1] += moved.im;
                anyKnown = true;
            } else if (ut != kNone) {
                const Complex moved = Mul(rConj, known[s]);
                rhs[2 * ut] += moved.re;
                rhs[2 * ut + 1] += moved.im;
                anyKnown = true;
            }
        }
        for (u32 t = 0; t < triangles; ++t) {
            const u32 ut = unknown[t];
            if (ut == kNone || pull[t] <= 0.0) {
                continue;
            }
            AddComplex(builder, ut, ut, Complex{pull[t], 0.0});
            rhs[2 * ut] += pull[t] * toward[t].re;
            rhs[2 * ut + 1] += pull[t] * toward[t].im;
            pullTotal += pull[t];
        }
        std::vector<f64> x(2 * count, 0.0);
        if (anyKnown || pullTotal > 1e-3 * count) {
            const SparseMatrix a = builder.build();
            SolveConjugateGradient(a, rhs, x, 1e-9, 20 * count + 200);
        } else {
            // Nothing to align to: the smoothest field, by inverse iteration.
            const f64 shift = 1e-6;
            for (u32 k = 0; k < count; ++k) {
                builder.add(2 * k, 2 * k, shift);
                builder.add(2 * k + 1, 2 * k + 1, shift);
            }
            const SparseMatrix a = builder.build();
            std::vector<f64> b(2 * count);
            for (u32 k = 0; k < count; ++k) {
                b[2 * k] = 1.0;
                b[2 * k + 1] = 0.0;
            }
            for (u32 iteration = 0; iteration < 12; ++iteration) {
                std::fill(x.begin(), x.end(), 0.0);
                SolveConjugateGradient(a, b, x, 1e-8, 20 * count + 200);
                f64 norm = 0.0;
                for (f64 v : x) {
                    norm += v * v;
                }
                norm = std::sqrt(norm);
                if (norm <= 0.0) {
                    break;
                }
                for (u32 k = 0; k < 2 * count; ++k) {
                    b[k] = x[k] / norm;
                }
            }
        }
        for (u32 t = 0; t < triangles; ++t) {
            if (unknown[t] != kNone) {
                solved[t] = Complex{x[2 * unknown[t]], x[2 * unknown[t] + 1]};
            }
        }
    }
    for (u32 t = 0; t < triangles; ++t) {
        const Complex& u = field.constrained[t] ? known[t] : solved[t];
        f64 angle = std::atan2(u.im, u.re) * 0.25;
        const f64 quarter = kPi * 0.5;
        angle = std::fmod(angle, quarter);
        if (angle < 0.0) {
            angle += quarter;
        }
        field.angles[t] = angle;
    }
    FindSingularities(mesh, field);
    if (options.cancelPairs > 0.0) {
        CancelSingularityPairs(mesh, field, options.cancelPairs * options.scale);
    }
    return field;
}

u32 CancelSingularityPairs(const WorkMesh& mesh, CrossField& field, f64 reach) {
    const f64 quarter = kPi * 0.5;
    auto wrapNear = [&](f64 value, f64 near) {
        return value + quarter * std::round((near - value) / quarter);
    };
    u32 cancelled = 0;
    std::vector<u32> ring;
    for (u32 pass = 0; pass < 3; ++pass) {
        std::vector<u32> positive;
        std::vector<u32> negative;
        for (u32 v = 0; v < field.index.size(); ++v) {
            if (field.index[v] == 1) {
                positive.push_back(v);
            } else if (field.index[v] == -1) {
                negative.push_back(v);
            }
        }
        std::vector<u8> taken(mesh.triangleCount(), 0);
        u32 thisPass = 0;
        for (u32 p : positive) {
            if (field.index[p] != 1) {
                continue;
            }
            // The nearest -1 within reach.
            u32 q = kNone;
            f64 nearest = reach;
            for (u32 n : negative) {
                const f64 d = Distance(mesh.positions[p], mesh.positions[n]);
                if (field.index[n] == -1 && d < nearest) {
                    nearest = d;
                    q = n;
                }
            }
            if (q == kNone) {
                continue;
            }
            // The disk round the pair, a few edges past it so the field has
            // room to turn by the curvature at the two points.
            const V3& a = mesh.positions[p];
            const V3& b = mesh.positions[q];
            f64 edge = 0.0;
            mesh.ring(p, ring);
            for (u32 o : ring) {
                edge += Distance(a, mesh.positions[mesh.to(o)]);
            }
            edge = ring.empty() ? nearest : edge / static_cast<f64>(ring.size());
            const f64 radius = 0.5 * nearest + std::max(nearest, 4.0 * edge);
            auto near = [&](u32 v) {
                const V3& x = mesh.positions[v];
                return Distance(x, Lerp(a, b, NearestOnSegment(x, a, b))) <= radius;
            };
            std::unordered_map<u32, u32> local; // triangle -> unknown
            std::vector<u32> triangles;
            std::vector<u32> stack;
            auto admit = [&](u32 t) {
                if (mesh.dead[t] || taken[t] || local.count(t)) {
                    return;
                }
                for (u32 i = 0; i < 3; ++i) {
                    if (!near(mesh.corners[3 * t + i])) {
                        return;
                    }
                }
                local[t] = static_cast<u32>(triangles.size());
                triangles.push_back(t);
                stack.push_back(t);
            };
            mesh.ring(p, ring);
            for (u32 o : ring) {
                admit(o / 3);
            }
            while (!stack.empty()) {
                const u32 t = stack.back();
                stack.pop_back();
                for (u32 i = 0; i < 3; ++i) {
                    const u32 twin = mesh.twins[3 * t + i];
                    if (twin != kNone) {
                        admit(twin / 3);
                    }
                }
            }
            bool inside = true;
            for (u32 v : {p, q}) {
                inside = inside && mesh.ring(v, ring);
                for (u32 o : ring) {
                    inside = inside && local.count(o / 3) != 0;
                }
            }
            if (!inside) {
                continue;
            }
            // A disk, with no net index inside.
            std::unordered_map<u64, u32> edges;
            std::unordered_map<u32, u32> vertexFaces;
            for (u32 t : triangles) {
                for (u32 i = 0; i < 3; ++i) {
                    const u32 u = mesh.corners[3 * t + i];
                    const u32 w = mesh.corners[3 * t + (i + 1) % 3];
                    ++edges[(static_cast<u64>(std::min(u, w)) << 32) | std::max(u, w)];
                    ++vertexFaces[u];
                }
            }
            const i64 euler = static_cast<i64>(vertexFaces.size()) - static_cast<i64>(edges.size()) +
                              static_cast<i64>(triangles.size());
            i32 enclosed = 0;
            for (const auto& [v, faces] : vertexFaces) {
                if (mesh.ring(v, ring) && faces == ring.size()) {
                    enclosed += field.index[v];
                }
            }
            std::vector<u32> border;
            for (u32 t : triangles) {
                for (u32 i = 0; i < 3; ++i) {
                    const u32 h = 3 * t + i;
                    if (mesh.twins[h] == kNone || !local.count(mesh.twins[h] / 3)) {
                        border.push_back(h);
                    }
                }
            }
            if (euler != 1 || enclosed != 0 || border.empty()) {
                continue;
            }
            std::vector<u32> loop;
            {
                std::unordered_map<u32, u8> isBorder;
                for (u32 h : border) {
                    isBorder[h] = 1;
                }
                u32 h = border.front();
                for (u32 guard = 0; guard <= border.size(); ++guard) {
                    loop.push_back(h);
                    u32 c = WorkMesh::Next(h);
                    for (u32 spin = 0; spin < 512 && !isBorder.count(c); ++spin) {
                        c = WorkMesh::Next(mesh.twins[c]);
                    }
                    h = c;
                    if (h == border.front()) {
                        break;
                    }
                }
            }
            if (loop.size() != border.size()) {
                continue;
            }
            // A reference direction carried over a spanning tree; the field is
            // solved relative to it, each cotree edge keeping the curvature it
            // closes less whole quarter turns.
            const u32 count = static_cast<u32>(triangles.size());
            std::vector<f64> reference(count, 0.0);
            std::vector<u8> reached(count, 0);
            std::vector<u8> treeEdge(3 * count, 0);
            std::vector<u32> queue{0};
            reached[0] = 1;
            reference[0] = field.angles[triangles[0]];
            for (std::size_t head = 0; head < queue.size(); ++head) {
                const u32 k = queue[head];
                for (u32 i = 0; i < 3; ++i) {
                    const u32 h = 3 * triangles[k] + i;
                    const u32 twin = mesh.twins[h];
                    const auto it = twin != kNone ? local.find(twin / 3) : local.end();
                    if (it == local.end() || reached[it->second]) {
                        continue;
                    }
                    reached[it->second] = 1;
                    reference[it->second] = reference[k] + Transport(mesh, field, h);
                    treeEdge[3 * k + i] = 1;
                    treeEdge[3 * it->second + twin % 3] = 1;
                    queue.push_back(it->second);
                }
            }
            // The field round the disk, lifted continuously along its border.
            std::vector<std::pair<u32, f64>> held;
            f64 previous = 0.0;
            f64 first = 0.0;
            for (u32 h : loop) {
                const u32 twin = mesh.twins[h];
                if (twin == kNone) {
                    continue;
                }
                const u32 k = local[h / 3];
                const f64 value = field.angles[twin / 3] - Transport(mesh, field, h) - reference[k];
                const f64 lifted = held.empty() ? wrapNear(value, 0.0) : wrapNear(value, previous);
                if (held.empty()) {
                    first = lifted;
                }
                held.push_back({k, lifted});
                previous = lifted;
            }
            if (held.empty() || std::abs(previous - first) > 0.5 * quarter) {
                continue; // the border winds: the disk is not index-free after all
            }
            constexpr f64 kHold = 1000.0;
            auto solve = [&](const std::vector<std::pair<u32, f64>>& holds) {
                SparseBuilder builder(count);
                std::vector<f64> rhs(count, 0.0);
                for (u32 k = 0; k < count; ++k) {
                    for (u32 i = 0; i < 3; ++i) {
                        const u32 h = 3 * triangles[k] + i;
                        const u32 twin = mesh.twins[h];
                        const auto it = twin != kNone ? local.find(twin / 3) : local.end();
                        if (it == local.end() || it->second < k) {
                            continue;
                        }
                        const u32 s = it->second;
                        f64 jump = 0.0;
                        if (!treeEdge[3 * k + i]) {
                            const f64 delta = reference[k] + Transport(mesh, field, h) - reference[s];
                            jump = delta - quarter * std::round(delta / quarter);
                        }
                        // (alpha_s - alpha_k - jump)^2
                        builder.add(k, k, 1.0);
                        builder.add(s, s, 1.0);
                        builder.add(k, s, -1.0);
                        builder.add(s, k, -1.0);
                        rhs[s] += jump;
                        rhs[k] -= jump;
                    }
                }
                std::vector<f64> alpha(count, 0.0);
                for (const auto& [k, value] : holds) {
                    builder.add(k, k, kHold);
                    rhs[k] += kHold * value;
                    alpha[k] = value;
                }
                const SparseMatrix matrix = builder.build();
                SolveConjugateGradient(matrix, rhs, alpha, 1e-10, 20 * count + 200);
                return alpha;
            };
            // Free first; then the feature triangles held at their own angle,
            // lifted to the turn nearest that free solution.
            const std::vector<f64> loose = solve(held);
            bool anyHeld = false;
            for (u32 k = 0; k < count; ++k) {
                if (field.constrained[triangles[k]]) {
                    held.push_back({k, wrapNear(field.angles[triangles[k]] - reference[k], loose[k])});
                    anyHeld = true;
                }
            }
            const std::vector<f64> alpha = anyHeld ? solve(held) : loose;
            // Kept only where it leaves fewer singular points round it.
            i32 before = 0;
            for (const auto& [v, faces] : vertexFaces) {
                before += std::abs(field.index[v]);
            }
            std::vector<f64> old(count);
            for (u32 k = 0; k < count; ++k) {
                old[k] = field.angles[triangles[k]];
                if (!field.constrained[triangles[k]]) {
                    f64 angle = std::fmod(reference[k] + alpha[k], quarter);
                    field.angles[triangles[k]] = angle < 0.0 ? angle + quarter : angle;
                }
            }
            i32 after = 0;
            for (const auto& [v, faces] : vertexFaces) {
                after += std::abs(VertexIndex(mesh, field, v, ring));
            }
            if (after >= before) {
                for (u32 k = 0; k < count; ++k) {
                    field.angles[triangles[k]] = old[k];
                }
                continue;
            }
            for (const auto& [v, faces] : vertexFaces) {
                field.index[v] = VertexIndex(mesh, field, v, ring);
            }
            for (u32 t : triangles) {
                taken[t] = 1;
            }
            ++thisPass;
        }
        if (thisPass == 0) {
            break;
        }
        cancelled += thisPass;
        FindSingularities(mesh, field);
    }
    return cancelled;
}

void FindSingularities(const WorkMesh& mesh, CrossField& field) {
    field.index.assign(mesh.vertexCount(), 0);
    std::vector<u32> ring;
    for (u32 v = 0; v < mesh.vertexCount(); ++v) {
        field.index[v] = VertexIndex(mesh, field, v, ring);
    }
}

} // namespace retopo
} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
