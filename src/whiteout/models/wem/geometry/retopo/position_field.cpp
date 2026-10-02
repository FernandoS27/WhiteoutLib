// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "position_field.h"

#include <algorithm>
#include <cmath>

namespace whiteout {
namespace models {
namespace wem {
namespace geom {
namespace retopo {

namespace {

/// A vertex whose frames' mean normal is shorter than this share of their
/// area is on a fold: its sides face nearly opposite ways.
constexpr f64 kFold = 0.3;

/// The point nearest both @p x0 and @p x1 on both tangent planes: where two
/// neighbours' lattices are compared.
V3 Middle(const V3& x0, const V3& n0, const V3& x1, const V3& n1) {
    const V3 m = (x0 + x1) * 0.5;
    const f64 c = Dot(n0, n1);
    const f64 det = 1.0 - c * c;
    const f64 r0 = Dot(n0, x0 - m);
    const f64 r1 = Dot(n1, x1 - m);
    if (det < 1e-6) {
        return m + n0 * (0.5 * (r0 + c * r1));
    }
    const f64 l0 = (r0 - c * r1) / det;
    const f64 l1 = (r1 - c * r0) / det;
    return m + n0 * l0 + n1 * l1;
}

/// The four lattice points round @p q.
void CellCorners(const V3& q, const V3& origin, const V3& u, const V3& v, f64 h, V3 out[4]) {
    const V3 d = q - origin;
    const V3 base = origin + u * (h * std::floor(Dot(d, u) / h)) + v * (h * std::floor(Dot(d, v) / h));
    out[0] = base;
    out[1] = base + u * h;
    out[2] = base + v * h;
    out[3] = base + (u + v) * h;
}

/// The lattice point of (@p origin, @p u, @p v) nearest @p x.
V3 RoundNear(const V3& origin, const V3& u, const V3& v, f64 h, const V3& x) {
    const V3 d = x - origin;
    return origin + u * (h * std::round(Dot(d, u) / h)) + v * (h * std::round(Dot(d, v) / h));
}

struct Matched {
    V3 a;
    V3 b;
};

/// Of the four lattice points of each round the two vertices' middle, the
/// closest pair. Both frames' axes are matched already.
Matched MatchLattices(const V3& x0, const V3& n0, const V3& u0, const V3& v0, const V3& o0, const V3& x1,
                      const V3& n1, const V3& u1, const V3& v1, const V3& o1, f64 h) {
    const V3 q = Middle(x0, n0, x1, n1);
    V3 a[4];
    V3 b[4];
    CellCorners(q, o0, u0, v0, h, a);
    CellCorners(q, o1, u1, v1, h, b);
    Matched best{a[0], b[0]};
    f64 nearest = Length2(a[0] - b[0]);
    for (u32 i = 0; i < 4; ++i) {
        for (u32 j = 0; j < 4; ++j) {
            const f64 d = Length2(a[i] - b[j]);
            if (d < nearest) {
                nearest = d;
                best = Matched{a[i], b[j]};
            }
        }
    }
    return best;
}

/// One level of the hierarchy: vertices, their frames and lattices, and the
/// weighted graph between them.
struct Level {
    std::vector<V3> positions;
    std::vector<V3> normals;
    std::vector<V3> axes;
    std::vector<V3> origins;
    std::vector<f64> areas;
    std::vector<LatticePin> pins;
    std::vector<V3> pinAt; ///< Where a pinned vertex's line or point is.
    std::vector<u32> offsets{0};
    std::vector<u32> neighbours;
    std::vector<f64> weights;
    /// Per vertex, its vertex on the next coarser level.
    std::vector<u32> parents;

    u32 size() const {
        return static_cast<u32>(positions.size());
    }
};

/// Applies @p level's pin at @p i to @p origin (in its tangent plane already).
V3 Pin(const Level& level, u32 i, const V3& origin) {
    switch (level.pins[i]) {
    case LatticePin::None:
        return origin;
    case LatticePin::Line: {
        const V3 across = Cross(level.normals[i], level.axes[i]);
        return origin + across * Dot(level.pinAt[i] - origin, across);
    }
    case LatticePin::Point: {
        const V3& n = level.normals[i];
        return level.pinAt[i] - n * Dot(level.pinAt[i] - level.positions[i], n);
    }
    }
    return origin;
}

/// Pairs alike neighbours into the next coarser level.
Level Coarsen(Level& fine) {
    const u32 count = fine.size();
    struct Candidate {
        f64 score;
        u32 a;
        u32 b;
    };
    std::vector<Candidate> candidates;
    for (u32 i = 0; i < count; ++i) {
        for (u32 k = fine.offsets[i]; k < fine.offsets[i + 1]; ++k) {
            const u32 j = fine.neighbours[k];
            if (j <= i) {
                continue;
            }
            const f64 small = std::min(fine.areas[i], fine.areas[j]);
            const f64 large = std::max(fine.areas[i], fine.areas[j]);
            const f64 alike = large > 0.0 ? small / large : 1.0;
            candidates.push_back({Dot(fine.normals[i], fine.normals[j]) * alike, i, j});
        }
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& x, const Candidate& y) {
        return x.score != y.score ? x.score > y.score : (x.a != y.a ? x.a < y.a : x.b < y.b);
    });
    fine.parents.assign(count, kNone);
    Level coarse;
    auto make = [&](u32 a, u32 b) {
        const u32 id = coarse.size();
        fine.parents[a] = id;
        f64 area = fine.areas[a];
        V3 position = fine.positions[a] * fine.areas[a];
        V3 normal = fine.normals[a] * fine.areas[a];
        if (b != kNone) {
            fine.parents[b] = id;
            area += fine.areas[b];
            position = position + fine.positions[b] * fine.areas[b];
            normal = normal + fine.normals[b] * fine.areas[b];
        }
        position = area > 0.0 ? position * (1.0 / area) : fine.positions[a];
        normal = Length2(normal) > 0.0 ? Unit(normal) : fine.normals[a];
        // The stronger pin of the two, its axis with it.
        u32 lead = a;
        if (b != kNone && static_cast<u8>(fine.pins[b]) > static_cast<u8>(fine.pins[a])) {
            lead = b;
        }
        V3 axis = fine.axes[lead] * fine.areas[lead];
        if (b != kNone && fine.pins[lead] == LatticePin::None) {
            const u32 other = lead == a ? b : a;
            const u32 k =
                MatchQuarter(fine.normals[lead], fine.axes[lead], fine.normals[other], fine.axes[other]);
            axis = axis + TurnQuarter(fine.normals[other], fine.axes[other], k) * fine.areas[other];
        }
        axis = Unit(Tangent(axis, normal));
        if (Length2(axis) == 0.0) {
            axis = Unit(Tangent(fine.axes[a], normal));
        }
        coarse.positions.push_back(position);
        coarse.normals.push_back(normal);
        coarse.axes.push_back(axis);
        coarse.origins.push_back(position);
        coarse.areas.push_back(area);
        coarse.pins.push_back(fine.pins[lead]);
        coarse.pinAt.push_back(fine.pinAt[lead]);
    };
    for (const Candidate& c : candidates) {
        if (fine.parents[c.a] == kNone && fine.parents[c.b] == kNone) {
            make(c.a, c.b);
        }
    }
    for (u32 i = 0; i < count; ++i) {
        if (fine.parents[i] == kNone) {
            make(i, kNone);
        }
    }
    // The coarse graph: every fine edge between two parents, weights summed.
    std::vector<std::vector<std::pair<u32, f64>>> links(coarse.size());
    for (u32 i = 0; i < count; ++i) {
        for (u32 k = fine.offsets[i]; k < fine.offsets[i + 1]; ++k) {
            const u32 pi = fine.parents[i];
            const u32 pj = fine.parents[fine.neighbours[k]];
            if (pi != pj) {
                links[pi].push_back({pj, fine.weights[k]});
            }
        }
    }
    for (std::vector<std::pair<u32, f64>>& list : links) {
        std::sort(list.begin(), list.end());
        u32 written = 0;
        for (const auto& [j, w] : list) {
            if (written > 0 && coarse.neighbours.back() == j) {
                coarse.weights.back() += w;
                continue;
            }
            coarse.neighbours.push_back(j);
            coarse.weights.push_back(w);
            ++written;
        }
        coarse.offsets.push_back(static_cast<u32>(coarse.neighbours.size()));
    }
    return coarse;
}

/// One Gauss-Seidel sweep: each origin to the running mean of the lattice
/// points its neighbours match it with.
void Sweep(Level& level, f64 h) {
    for (u32 i = 0; i < level.size(); ++i) {
        const V3& x = level.positions[i];
        const V3& n = level.normals[i];
        const V3& u = level.axes[i];
        const V3 v = Cross(n, u);
        if (level.pins[i] == LatticePin::Point) {
            level.origins[i] = Pin(level, i, level.origins[i]);
            continue;
        }
        V3 sum = level.origins[i];
        f64 weight = 0.0;
        for (u32 k = level.offsets[i]; k < level.offsets[i + 1]; ++k) {
            const u32 j = level.neighbours[k];
            const f64 w = level.weights[k];
            const u32 quarter = MatchQuarter(n, u, level.normals[j], level.axes[j]);
            const V3 uj = TurnQuarter(level.normals[j], level.axes[j], quarter);
            const V3 vj = Cross(level.normals[j], uj);
            const Matched m =
                MatchLattices(x, n, u, v, sum, level.positions[j], level.normals[j], uj, vj, level.origins[j], h);
            sum = (m.a * weight + m.b * w) * (1.0 / (weight + w));
            weight += w;
        }
        sum = sum - n * Dot(sum - x, n);
        sum = Pin(level, i, sum);
        level.origins[i] = RoundNear(sum, u, v, h, x);
    }
}

} // namespace

u32 MatchQuarter(const V3& normalA, const V3& axisA, const V3& normalB, const V3& axisB) {
    const V3 a[2] = {axisA, Cross(normalA, axisA)};
    const V3 b[2] = {axisB, Cross(normalB, axisB)};
    u32 bestI = 0;
    u32 bestJ = 0;
    f64 best = -1.0;
    f64 sign = 1.0;
    for (u32 i = 0; i < 2; ++i) {
        for (u32 j = 0; j < 2; ++j) {
            const f64 d = Dot(a[i], b[j]);
            if (std::abs(d) > best) {
                best = std::abs(d);
                sign = d >= 0.0 ? 1.0 : -1.0;
                bestI = i;
                bestJ = j;
            }
        }
    }
    // b's arm j, turned j quarters from its axis, lies along a's arm i, i
    // quarters from a's axis: b's axis turned j - i quarters (two more if
    // they point apart) lies along a's.
    return (bestJ + 4u - bestI + (sign < 0.0 ? 2u : 0u)) & 3u;
}

V3 TurnQuarter(const V3& normal, const V3& axis, u32 quarters) {
    switch (quarters & 3u) {
    case 1:
        return Cross(normal, axis);
    case 2:
        return axis * -1.0;
    case 3:
        return Cross(normal, axis) * -1.0;
    default:
        return axis;
    }
}

LatticeShift Shift(const LatticeFrame& a, const LatticeFrame& b, f64 scale) {
    const V3 ua = a.axis;
    const V3 va = Cross(a.normal, ua);
    const u32 quarter = MatchQuarter(a.normal, ua, b.normal, b.axis);
    const V3 ub = TurnQuarter(b.normal, b.axis, quarter);
    const V3 vb = Cross(b.normal, ub);
    const Matched m = MatchLattices(a.position, a.normal, ua, va, a.origin, b.position, b.normal, ub, vb, b.origin, scale);
    // From a's point to the matched point on a's lattice, then from b's
    // matched point to b's own on b's: both whole steps.
    LatticeShift shift;
    shift.x = static_cast<i32>(std::lround(Dot(m.a - a.origin, ua) / scale + Dot(b.origin - m.b, ub) / scale));
    shift.y = static_cast<i32>(std::lround(Dot(m.a - a.origin, va) / scale + Dot(b.origin - m.b, vb) / scale));
    shift.gap = Distance(m.a, m.b) / scale;
    return shift;
}

void TurnSteps(i32& x, i32& y, u32 quarters) {
    const i32 a = x;
    const i32 b = y;
    switch (quarters & 3u) {
    case 1:
        x = b;
        y = -a;
        break;
    case 2:
        x = -a;
        y = -b;
        break;
    case 3:
        x = -b;
        y = a;
        break;
    default:
        break;
    }
}

u32 FrameQuarter(const PositionField& field, u32 to, u32 from) {
    return MatchQuarter(field.normals[to], field.axes[to], field.normals[from], field.axes[from]);
}

LatticeSteps MeasureSteps(const WorkMesh& mesh, const PositionField& field) {
    LatticeSteps steps;
    const u32 halfedges = 3 * mesh.triangleCount();
    steps.x.assign(halfedges, 0);
    steps.y.assign(halfedges, 0);
    auto frame = [&](u32 v) { return LatticeFrame{mesh.positions[v], field.normals[v], field.axes[v], field.origins[v]}; };
    for (u32 h = 0; h < halfedges; ++h) {
        if (mesh.dead[h / 3]) {
            continue;
        }
        const u32 twin = mesh.twins[h];
        if (twin != kNone && twin < h) {
            continue;
        }
        const u32 a = mesh.from(h);
        const u32 b = mesh.to(h);
        const LatticeShift s = Shift(frame(a), frame(b), field.scale);
        steps.x[h] = s.x;
        steps.y[h] = s.y;
        if (twin != kNone) {
            // The way back, in the other end's frame.
            i32 x = -s.x;
            i32 y = -s.y;
            TurnSteps(x, y, FrameQuarter(field, b, a));
            steps.x[twin] = x;
            steps.y[twin] = y;
        }
    }
    return steps;
}

PositionField VertexFrames(const WorkMesh& mesh, const Surface& surface, const CrossField& cross) {
    PositionField field;
    const u32 count = mesh.vertexCount();
    field.normals.assign(count, V3{0.0, 0.0, 1.0});
    field.axes.assign(count, V3{1.0, 0.0, 0.0});
    field.origins = mesh.positions;
    field.pins.assign(count, LatticePin::None);
    std::vector<u32> ring;
    for (u32 vertex = 0; vertex < count; ++vertex) {
        if (!mesh.alive(vertex)) {
            continue;
        }
        mesh.ring(vertex, ring);
        // The plane: the frames' area-weighted mean, a crease's bisector. On a
        // fold (a sheet's hem) the sides cancel: there the surface faces out
        // of the fold, away from its neighbours, as a smooth one bent that
        // tight would, and both sheets turn into it the same way round.
        V3 normal{0.0, 0.0, 0.0};
        V3 centre{0.0, 0.0, 0.0};
        f64 total = 0.0;
        u32 largest = kNone;
        f64 largestArea = -1.0;
        for (const u32 o : ring) {
            const u32 t = o / 3;
            const f64 area = TriangleArea(mesh.positions[mesh.corners[3 * t]], mesh.positions[mesh.corners[3 * t + 1]],
                                          mesh.positions[mesh.corners[3 * t + 2]]);
            normal = normal + cross.normals[t] * area;
            centre = centre + mesh.positions[mesh.to(o)];
            total += area;
            if (area > largestArea) {
                largestArea = area;
                largest = t;
            }
        }
        if (Length(normal) < kFold * total && !ring.empty()) {
            const V3 out = mesh.positions[vertex] - centre * (1.0 / static_cast<f64>(ring.size()));
            normal = Length2(out) > 0.0 ? out : normal;
        }
        normal = Unit(normal);
        if (Length2(normal) == 0.0) {
            continue;
        }
        field.normals[vertex] = normal;
        // The crosses' mean as a 4-direction: their fourth powers averaged in
        // this tangent plane, each cross by whichever of its arms the plane
        // shows longer (a crease's other side shows one of them end on).
        const V3 seed = largest != kNone ? cross.axisX[largest] : V3{1.0, 0.0, 0.0};
        V3 e1 = Unit(Tangent(seed, normal));
        if (Length2(e1) == 0.0) {
            e1 = Unit(Tangent(std::abs(normal.x) < 0.9 ? V3{1.0, 0.0, 0.0} : V3{0.0, 1.0, 0.0}, normal));
        }
        const V3 e2 = Cross(normal, e1);
        f64 re = 0.0;
        f64 im = 0.0;
        f64 first = 0.0;
        bool any = false;
        for (const u32 o : ring) {
            const u32 t = o / 3;
            const V3 d0 = Tangent(cross.direction(t, 0), normal);
            const V3 d1 = Tangent(cross.direction(t, 1), normal);
            const V3 d = Length2(d0) >= Length2(d1) ? d0 : d1;
            const f64 angle = std::atan2(Dot(d, e2), Dot(d, e1));
            const f64 area = TriangleArea(mesh.positions[mesh.corners[3 * t]], mesh.positions[mesh.corners[3 * t + 1]],
                                          mesh.positions[mesh.corners[3 * t + 2]]);
            re += area * std::cos(4.0 * angle);
            im += area * std::sin(4.0 * angle);
            if (!any) {
                first = angle;
                any = true;
            }
        }
        // At the field's own singular points the crosses cancel: any of them.
        const f64 angle = re * re + im * im > 1e-24 ? 0.25 * std::atan2(im, re) : first;
        field.axes[vertex] = e1 * std::cos(angle) + e2 * std::sin(angle);

        // A feature vertex's lattice keeps a line along its curve, a corner's
        // a point on itself.
        if (mesh.kinds[vertex] == VertexKind::Free) {
            continue;
        }
        for (const u32 o : ring) {
            for (const u32 h : {o, WorkMesh::Prev(o)}) {
                if (!mesh.isFeature(h)) {
                    continue;
                }
                const V3 along = Unit(Tangent(FeatureDirection(mesh, surface, h), normal));
                if (Length2(along) > 0.0) {
                    field.axes[vertex] = along;
                    field.pins[vertex] = mesh.kinds[vertex] == VertexKind::Corner ? LatticePin::Point : LatticePin::Line;
                    break;
                }
            }
            if (field.pins[vertex] != LatticePin::None) {
                break;
            }
        }
        if (mesh.kinds[vertex] == VertexKind::Corner) {
            field.pins[vertex] = LatticePin::Point;
        }
    }
    return field;
}

void SolvePositions(const WorkMesh& mesh, PositionField& field, const PositionOptions& options) {
    const f64 h = options.scale;
    field.scale = h;
    // The finest level: the work mesh's vertices and edges.
    std::vector<Level> levels(1);
    Level& finest = levels[0];
    const u32 count = mesh.vertexCount();
    finest.positions = mesh.positions;
    finest.normals = field.normals;
    finest.axes = field.axes;
    finest.origins = mesh.positions;
    finest.pins = field.pins;
    finest.pinAt = mesh.positions;
    finest.areas.assign(count, 0.0);
    {
        std::vector<std::vector<u32>> links(count);
        for (u32 t = 0; t < mesh.triangleCount(); ++t) {
            if (mesh.dead[t]) {
                continue;
            }
            const u32 a = mesh.corners[3 * t];
            const u32 b = mesh.corners[3 * t + 1];
            const u32 c = mesh.corners[3 * t + 2];
            const f64 third = TriangleArea(mesh.positions[a], mesh.positions[b], mesh.positions[c]) / 3.0;
            for (u32 i = 0; i < 3; ++i) {
                const u32 p = mesh.corners[3 * t + i];
                const u32 q = mesh.corners[3 * t + (i + 1) % 3];
                finest.areas[p] += third;
                links[p].push_back(q);
                links[q].push_back(p);
            }
        }
        for (std::vector<u32>& list : links) {
            std::sort(list.begin(), list.end());
            list.erase(std::unique(list.begin(), list.end()), list.end());
            finest.neighbours.insert(finest.neighbours.end(), list.begin(), list.end());
            finest.offsets.push_back(static_cast<u32>(finest.neighbours.size()));
        }
        finest.weights.assign(finest.neighbours.size(), 1.0);
    }
    // Coarser levels until a few dozen vertices, or until pairing stalls.
    while (levels.back().size() > 32) {
        Level coarse = Coarsen(levels.back());
        if (coarse.size() * 10 > levels.back().size() * 9) {
            levels.back().parents.clear();
            break;
        }
        levels.push_back(std::move(coarse));
    }

    // Coarse to fine: each level seeded from the one above, then smoothed.
    for (std::size_t l = levels.size(); l-- > 0;) {
        Level& level = levels[l];
        if (l + 1 < levels.size()) {
            const Level& above = levels[l + 1];
            for (u32 i = 0; i < level.size(); ++i) {
                const V3& n = level.normals[i];
                V3 o = above.origins[level.parents[i]];
                o = o - n * Dot(o - level.positions[i], n);
                o = Pin(level, i, o);
                level.origins[i] = RoundNear(o, level.axes[i], Cross(n, level.axes[i]), h, level.positions[i]);
            }
        } else {
            for (u32 i = 0; i < level.size(); ++i) {
                level.origins[i] = Pin(level, i, level.positions[i]);
            }
        }
        const u32 sweeps = l == 0 ? options.finestSweeps : options.sweeps;
        for (u32 s = 0; s < sweeps; ++s) {
            Sweep(level, h);
        }
    }
    field.origins = levels[0].origins;
}

} // namespace retopo
} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
