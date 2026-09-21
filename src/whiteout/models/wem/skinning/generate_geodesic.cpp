// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// Geodesic voxel binding (EDIT_MODE_SKIN_DESIGN.md §8.5): the grid, the shell,
/// the solid, and one Dijkstra walk per bone through it. `SolveGeodesic` in
/// `generate.h` says what is measured and why.

#include <whiteout/models/wem/skinning/generate.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <limits>
#include <queue>

#include "generate_common.h"

namespace whiteout {
namespace models {
namespace wem {
namespace skinning {

namespace {

using detail::PositionOf;

/// Empty cells around the model on every side, so that the outside is one
/// connected region the flood can go all the way round.
constexpr i32 kPad = 2;

/// Where the parity lines run, off the cell centres, as a fraction of a cell.
/// A line through a centre would pass exactly along the edges of a mesh built
/// on the same spacing -- a box, say -- and count its crossings twice or not at
/// all.
constexpr f32 kLineJitterU = 0.0137f;
constexpr f32 kLineJitterV = 0.0291f;

constexpr f32 kUnreached = std::numeric_limits<f32>::infinity();

/// The grid: `origin` is cell (0, 0, 0)'s low corner.
struct Grid {
    Vector3f origin{0, 0, 0};
    f32 cell = 1.0f;
    std::array<i32, 3> size{0, 0, 0};

    std::size_t count() const {
        return static_cast<std::size_t>(size[0]) * static_cast<std::size_t>(size[1]) *
               static_cast<std::size_t>(size[2]);
    }
    std::size_t index(i32 x, i32 y, i32 z) const {
        return (static_cast<std::size_t>(z) * static_cast<std::size_t>(size[1]) +
                static_cast<std::size_t>(y)) *
                   static_cast<std::size_t>(size[0]) +
               static_cast<std::size_t>(x);
    }
    bool contains(i32 x, i32 y, i32 z) const {
        return x >= 0 && y >= 0 && z >= 0 && x < size[0] && y < size[1] && z < size[2];
    }
    Vector3f centre(i32 x, i32 y, i32 z) const {
        return Vector3f{origin.x + (static_cast<f32>(x) + 0.5f) * cell,
                        origin.y + (static_cast<f32>(y) + 0.5f) * cell,
                        origin.z + (static_cast<f32>(z) + 0.5f) * cell};
    }
    /// The cell @p position falls in, which may be outside the grid.
    std::array<i32, 3> cellOf(const Vector3f& position) const {
        return {static_cast<i32>(std::floor((position.x - origin.x) / cell)),
                static_cast<i32>(std::floor((position.y - origin.y) / cell)),
                static_cast<i32>(std::floor((position.z - origin.z) / cell))};
    }
};

f32 Axis(const Vector3f& v, i32 axis) {
    return axis == 0 ? v.x : (axis == 1 ? v.y : v.z);
}

/// Akenine-Möller's triangle/box overlap: the separating-axis test over the
/// box's three faces, the triangle's plane and the nine edge crosses. The box
/// is centred on the origin, and the triangle given relative to its centre.
bool TriangleOverlapsBox(const Vector3f& half, const Vector3f& v0, const Vector3f& v1,
                         const Vector3f& v2) {
    // The box's faces.
    for (i32 axis = 0; axis < 3; ++axis) {
        const f32 a = Axis(v0, axis);
        const f32 b = Axis(v1, axis);
        const f32 c = Axis(v2, axis);
        const f32 h = Axis(half, axis);
        if (std::min({a, b, c}) > h || std::max({a, b, c}) < -h) {
            return false;
        }
    }
    // The nine crosses of a triangle edge with a box axis.
    const Vector3f edges[3]{v1 - v0, v2 - v1, v0 - v2};
    const Vector3f units[3]{Vector3f{1, 0, 0}, Vector3f{0, 1, 0}, Vector3f{0, 0, 1}};
    for (const Vector3f& edge : edges) {
        for (const Vector3f& unit : units) {
            const Vector3f axis = cross(unit, edge);
            if (axis.length_squared() <= 1e-20f) {
                continue; // parallel: the face tests above already cover it
            }
            const f32 p0 = axis.dot(v0);
            const f32 p1 = axis.dot(v1);
            const f32 p2 = axis.dot(v2);
            const f32 radius = half.x * std::abs(axis.x) + half.y * std::abs(axis.y) +
                               half.z * std::abs(axis.z);
            if (std::min({p0, p1, p2}) > radius || std::max({p0, p1, p2}) < -radius) {
                return false;
            }
        }
    }
    // The triangle's plane.
    const Vector3f normal = cross(edges[0], edges[1]);
    const f32 offset = normal.dot(v0);
    const f32 radius = half.x * std::abs(normal.x) + half.y * std::abs(normal.y) +
                       half.z * std::abs(normal.z);
    return std::abs(offset) <= radius;
}

/// A mesh of the run, flattened: each scope point's mesh, id and position.
struct RunPoint {
    u32 mesh = 0;
    u32 point = 0;
    Vector3f at{0, 0, 0};
};

/// The solid (§8.5 steps 1 and 2): 1 where a walk may go.
std::vector<u8> BuildSolid(const Grid& grid, std::span<const MeshScope> meshes) {
    const std::size_t cells = grid.count();
    std::vector<u8> shell(cells, 0);
    // Per axis, per line: where the line crosses a triangle, along the axis.
    std::array<std::vector<std::vector<f32>>, 3> lines;
    for (i32 axis = 0; axis < 3; ++axis) {
        const i32 u = (axis + 1) % 3;
        const i32 v = (axis + 2) % 3;
        lines[axis].resize(static_cast<std::size_t>(grid.size[u]) *
                           static_cast<std::size_t>(grid.size[v]));
    }
    // A hair over half a cell, so a triangle lying exactly on a cell face marks
    // the cells on both sides rather than neither.
    const f32 halfCell = grid.cell * 0.5f * 1.0001f;
    const Vector3f half{halfCell, halfCell, halfCell};
    const f32 originOf[3]{grid.origin.x, grid.origin.y, grid.origin.z};

    for (const MeshScope& entry : meshes) {
        if (entry.mesh == nullptr) {
            continue;
        }
        const std::span<const Vector3f> positions =
            entry.mesh->attributes.get<Vector3f>(geom::names::kPosition, geom::Domain::Vertex);
        const std::vector<u32> triangles = detail::TrianglesOf(*entry.mesh);
        for (std::size_t t = 0; t + 2 < triangles.size(); t += 3) {
            if (triangles[t] >= positions.size() || triangles[t + 1] >= positions.size() ||
                triangles[t + 2] >= positions.size()) {
                continue;
            }
            const Vector3f a = positions[triangles[t]];
            const Vector3f b = positions[triangles[t + 1]];
            const Vector3f c = positions[triangles[t + 2]];

            // The shell: every cell of the triangle's box it overlaps.
            const Vector3f low{std::min({a.x, b.x, c.x}), std::min({a.y, b.y, c.y}),
                               std::min({a.z, b.z, c.z})};
            const Vector3f high{std::max({a.x, b.x, c.x}), std::max({a.y, b.y, c.y}),
                                std::max({a.z, b.z, c.z})};
            std::array<i32, 3> from = grid.cellOf(low);
            std::array<i32, 3> to = grid.cellOf(high);
            for (i32 axis = 0; axis < 3; ++axis) {
                from[axis] = std::clamp(from[axis] - 1, 0, grid.size[axis] - 1);
                to[axis] = std::clamp(to[axis] + 1, 0, grid.size[axis] - 1);
            }
            for (i32 z = from[2]; z <= to[2]; ++z) {
                for (i32 y = from[1]; y <= to[1]; ++y) {
                    for (i32 x = from[0]; x <= to[0]; ++x) {
                        const std::size_t cell = grid.index(x, y, z);
                        if (shell[cell] != 0) {
                            continue;
                        }
                        const Vector3f centre = grid.centre(x, y, z);
                        if (TriangleOverlapsBox(half, a - centre, b - centre, c - centre)) {
                            shell[cell] = 1;
                        }
                    }
                }
            }

            // The parity lines: where each line through the triangle's shadow
            // crosses it.
            for (i32 axis = 0; axis < 3; ++axis) {
                const i32 u = (axis + 1) % 3;
                const i32 v = (axis + 2) % 3;
                const f32 au = Axis(a, u), av = Axis(a, v);
                const f32 bu = Axis(b, u), bv = Axis(b, v);
                const f32 cu = Axis(c, u), cv = Axis(c, v);
                const f32 area = (bu - au) * (cv - av) - (bv - av) * (cu - au);
                if (std::abs(area) <= 1e-20f) {
                    continue; // edge-on to this axis: no line crosses it
                }
                const auto lineOf = [&](f32 value, i32 along, f32 jitter) {
                    return static_cast<i32>(
                        std::floor((value - originOf[along]) / grid.cell - jitter));
                };
                const i32 u0 = std::max(lineOf(std::min({au, bu, cu}), u, kLineJitterU), 0);
                const i32 u1 = std::min(lineOf(std::max({au, bu, cu}), u, kLineJitterU),
                                        grid.size[u] - 1);
                const i32 v0 = std::max(lineOf(std::min({av, bv, cv}), v, kLineJitterV), 0);
                const i32 v1 = std::min(lineOf(std::max({av, bv, cv}), v, kLineJitterV),
                                        grid.size[v] - 1);
                for (i32 iv = v0; iv <= v1; ++iv) {
                    const f32 qv =
                        originOf[v] + (static_cast<f32>(iv) + 0.5f + kLineJitterV) * grid.cell;
                    for (i32 iu = u0; iu <= u1; ++iu) {
                        const f32 qu =
                            originOf[u] + (static_cast<f32>(iu) + 0.5f + kLineJitterU) * grid.cell;
                        // Barycentric weights of the line's foot in the shadow.
                        const f32 wa = ((bu - qu) * (cv - qv) - (bv - qv) * (cu - qu)) / area;
                        const f32 wb = ((cu - qu) * (av - qv) - (cv - qv) * (au - qu)) / area;
                        const f32 wc = 1.0f - wa - wb;
                        if (wa < 0.0f || wb < 0.0f || wc < 0.0f) {
                            continue;
                        }
                        const f32 along = wa * Axis(a, axis) + wb * Axis(b, axis) +
                                          wc * Axis(c, axis);
                        lines[axis][static_cast<std::size_t>(iv) *
                                        static_cast<std::size_t>(grid.size[u]) +
                                    static_cast<std::size_t>(iu)]
                            .push_back(along);
                    }
                }
            }
        }
    }

    // The flood from outside, 6-connected through what is not shell. The pad
    // makes every border cell empty, so one seed in the corner reaches round.
    std::vector<u8> outside(cells, 0);
    std::deque<std::size_t> front;
    front.push_back(grid.index(0, 0, 0));
    outside[grid.index(0, 0, 0)] = 1;
    while (!front.empty()) {
        const std::size_t cell = front.front();
        front.pop_front();
        const i32 x = static_cast<i32>(cell % static_cast<std::size_t>(grid.size[0]));
        const i32 y = static_cast<i32>((cell / static_cast<std::size_t>(grid.size[0])) %
                                       static_cast<std::size_t>(grid.size[1]));
        const i32 z = static_cast<i32>(cell / (static_cast<std::size_t>(grid.size[0]) *
                                               static_cast<std::size_t>(grid.size[1])));
        static const i32 kSteps[6][3] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                         {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
        for (const auto& step : kSteps) {
            const i32 nx = x + step[0];
            const i32 ny = y + step[1];
            const i32 nz = z + step[2];
            if (!grid.contains(nx, ny, nz)) {
                continue;
            }
            const std::size_t next = grid.index(nx, ny, nz);
            if (outside[next] != 0 || shell[next] != 0) {
                continue;
            }
            outside[next] = 1;
            front.push_back(next);
        }
    }

    // The vote: a cell is inside for an axis when its line crosses the shell
    // an odd number of times on BOTH sides of it. An open line -- one through a
    // hole, or past the edge of a cape -- has an odd total, so it can never
    // say "inside" on its own; two of the three lines have to agree.
    std::vector<u8> votes(cells, 0);
    for (i32 axis = 0; axis < 3; ++axis) {
        const i32 u = (axis + 1) % 3;
        const i32 v = (axis + 2) % 3;
        for (i32 iv = 0; iv < grid.size[v]; ++iv) {
            for (i32 iu = 0; iu < grid.size[u]; ++iu) {
                std::vector<f32>& hits =
                    lines[axis][static_cast<std::size_t>(iv) * static_cast<std::size_t>(grid.size[u]) +
                                static_cast<std::size_t>(iu)];
                if (hits.size() < 2) {
                    continue;
                }
                std::sort(hits.begin(), hits.end());
                for (i32 k = 0; k < grid.size[axis]; ++k) {
                    const f32 centre =
                        originOf[axis] + (static_cast<f32>(k) + 0.5f) * grid.cell;
                    const std::size_t before = static_cast<std::size_t>(
                        std::lower_bound(hits.begin(), hits.end(), centre) - hits.begin());
                    const std::size_t after = hits.size() - before;
                    if ((before & 1u) == 0 || (after & 1u) == 0) {
                        continue;
                    }
                    i32 at[3];
                    at[axis] = k;
                    at[u] = iu;
                    at[v] = iv;
                    ++votes[grid.index(at[0], at[1], at[2])];
                }
            }
        }
    }

    std::vector<u8> solid(cells, 0);
    for (std::size_t cell = 0; cell < cells; ++cell) {
        solid[cell] = (outside[cell] == 0 || votes[cell] >= 2) ? 1 : 0;
    }
    return solid;
}

/// The 26 steps a walk takes, and their lengths in cells.
struct Step {
    i32 dx, dy, dz;
    f32 length;
};

std::vector<Step> Steps() {
    std::vector<Step> steps;
    for (i32 dz = -1; dz <= 1; ++dz) {
        for (i32 dy = -1; dy <= 1; ++dy) {
            for (i32 dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dy == 0 && dz == 0) {
                    continue;
                }
                steps.push_back(
                    {dx, dy, dz, std::sqrt(static_cast<f32>(dx * dx + dy * dy + dz * dz))});
            }
        }
    }
    return steps;
}

/// One bone's walk (§8.5 step 3) and its reading at every point of the run
/// (step 4): the pairs (flat point, `d^-k`) for the points it reached.
std::vector<std::pair<u32, f32>> WalkBone(const Grid& grid, const std::vector<u8>& solid,
                                          const std::vector<Step>& steps,
                                          const BoneSegments& segments, u32 bone,
                                          std::span<const RunPoint> run, f32 falloff) {
    std::vector<std::pair<u32, f32>> out;
    const std::size_t cells = grid.count();
    std::vector<f32> distance(cells, kUnreached);
    using Entry = std::pair<f32, std::size_t>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> queue;

    const auto seed = [&](i32 x, i32 y, i32 z, f32 d) {
        if (!grid.contains(x, y, z)) {
            return;
        }
        const std::size_t cell = grid.index(x, y, z);
        if (d < distance[cell]) {
            distance[cell] = d;
            queue.push({d, cell});
        }
    };

    // The seeds, and the bone's length for the cut-off.
    f32 length = 0.0f;
    for (const BoneSegments::Segment& segment : segments.segments) {
        if (segment.bone != bone) {
            continue;
        }
        const Vector3f along = segment.end - segment.start;
        const f32 span = along.length();
        if (segment.radius > 0.0f || span <= 0.0f) {
            // A leaf's sphere, at `distance - radius` as `NearestBone` has it,
            // so the hand is nearer than the forearm that ends on its joint.
            length = std::max(length, segment.radius * 2.0f);
            const i32 reach = static_cast<i32>(std::ceil(segment.radius / grid.cell)) + 1;
            const std::array<i32, 3> middle = grid.cellOf(segment.start);
            for (i32 z = middle[2] - reach; z <= middle[2] + reach; ++z) {
                for (i32 y = middle[1] - reach; y <= middle[1] + reach; ++y) {
                    for (i32 x = middle[0] - reach; x <= middle[0] + reach; ++x) {
                        const f32 d = (grid.centre(x, y, z) - segment.start).length() -
                                      segment.radius;
                        if (d <= grid.cell) {
                            seed(x, y, z, std::max(d, 0.0f));
                        }
                    }
                }
            }
            continue;
        }
        length = std::max(length, span);
        // Half a cell apart, so no cell the segment crosses is stepped over.
        const u32 samples = static_cast<u32>(std::ceil(span / (grid.cell * 0.5f))) + 1;
        for (u32 s = 0; s <= samples; ++s) {
            const Vector3f at = segment.start + along * (static_cast<f32>(s) / samples);
            const std::array<i32, 3> cell = grid.cellOf(at);
            seed(cell[0], cell[1], cell[2],
                 DistanceToSegment(grid.centre(cell[0], cell[1], cell[2]), segment.start,
                                   segment.end));
        }
    }
    // §8.5's cut-off, which is what bounds the cost; a bone of no length still
    // reaches a few cells, or a lone root would reach nothing at all.
    const f32 cutoff = 3.0f * std::max(length, grid.cell * 2.0f);

    std::array<i32, 3> low{grid.size[0], grid.size[1], grid.size[2]};
    std::array<i32, 3> high{-1, -1, -1};
    while (!queue.empty()) {
        const auto [d, cell] = queue.top();
        queue.pop();
        if (d > distance[cell]) {
            continue;
        }
        const i32 x = static_cast<i32>(cell % static_cast<std::size_t>(grid.size[0]));
        const i32 y = static_cast<i32>((cell / static_cast<std::size_t>(grid.size[0])) %
                                       static_cast<std::size_t>(grid.size[1]));
        const i32 z = static_cast<i32>(cell / (static_cast<std::size_t>(grid.size[0]) *
                                               static_cast<std::size_t>(grid.size[1])));
        low = {std::min(low[0], x), std::min(low[1], y), std::min(low[2], z)};
        high = {std::max(high[0], x), std::max(high[1], y), std::max(high[2], z)};
        for (const Step& step : steps) {
            const i32 nx = x + step.dx;
            const i32 ny = y + step.dy;
            const i32 nz = z + step.dz;
            if (!grid.contains(nx, ny, nz)) {
                continue;
            }
            const std::size_t next = grid.index(nx, ny, nz);
            if (solid[next] == 0) {
                continue; // a walk goes through the model, never round it
            }
            const f32 reached = d + step.length * grid.cell;
            if (reached < distance[next] && reached <= cutoff) {
                distance[next] = reached;
                queue.push({reached, next});
            }
        }
    }
    if (high[0] < 0) {
        return out;
    }

    // The reading: trilinear over the eight cells round a point, from the ones
    // the walk reached, re-weighted over those. Held at half a cell, the least
    // the grid can tell apart.
    const f32 nearest = grid.cell * 0.5f;
    for (u32 index = 0; index < run.size(); ++index) {
        const Vector3f& at = run[index].at;
        const f32 gx = (at.x - grid.origin.x) / grid.cell - 0.5f;
        const f32 gy = (at.y - grid.origin.y) / grid.cell - 0.5f;
        const f32 gz = (at.z - grid.origin.z) / grid.cell - 0.5f;
        const i32 x0 = static_cast<i32>(std::floor(gx));
        const i32 y0 = static_cast<i32>(std::floor(gy));
        const i32 z0 = static_cast<i32>(std::floor(gz));
        if (x0 + 1 < low[0] || y0 + 1 < low[1] || z0 + 1 < low[2] || x0 > high[0] ||
            y0 > high[1] || z0 > high[2]) {
            continue;
        }
        const f32 fx = gx - static_cast<f32>(x0);
        const f32 fy = gy - static_cast<f32>(y0);
        const f32 fz = gz - static_cast<f32>(z0);
        f32 sum = 0.0f;
        f32 weight = 0.0f;
        for (i32 corner = 0; corner < 8; ++corner) {
            const i32 x = x0 + (corner & 1);
            const i32 y = y0 + ((corner >> 1) & 1);
            const i32 z = z0 + ((corner >> 2) & 1);
            if (!grid.contains(x, y, z)) {
                continue;
            }
            const f32 d = distance[grid.index(x, y, z)];
            if (!std::isfinite(d)) {
                continue;
            }
            const f32 w = ((corner & 1) ? fx : 1.0f - fx) * (((corner >> 1) & 1) ? fy : 1.0f - fy) *
                          (((corner >> 2) & 1) ? fz : 1.0f - fz);
            sum += d * w;
            weight += w;
        }
        if (!(weight > 1e-6f)) {
            continue;
        }
        const f32 d = std::max(sum / weight, nearest);
        out.push_back({index, std::pow(d, -falloff)});
    }
    return out;
}

} // namespace

GeneratedRun SolveGeodesic(std::span<const MeshScope> meshes, const NodeTree& nodes,
                           const GenerateOptions& options, const GenerateControl& control) {
    GeneratedRun run;
    const BoneSegments segments = BuildBoneSegments(nodes, options.bones);

    // Every scope point of the run, flattened: a walk reads them all at once.
    std::vector<RunPoint> points;
    std::vector<std::vector<u32>> scopes(meshes.size());
    Vector3f low{std::numeric_limits<f32>::max(), std::numeric_limits<f32>::max(),
                 std::numeric_limits<f32>::max()};
    Vector3f high{std::numeric_limits<f32>::lowest(), std::numeric_limits<f32>::lowest(),
                  std::numeric_limits<f32>::lowest()};
    for (u32 m = 0; m < meshes.size(); ++m) {
        if (meshes[m].mesh == nullptr || meshes[m].points == nullptr) {
            continue;
        }
        const std::span<const Vector3f> positions = meshes[m].mesh->attributes.get<Vector3f>(
            geom::names::kPosition, geom::Domain::Vertex);
        // The grid holds the whole of every mesh, not only the scope: the
        // volume a distance is measured through is the model's.
        for (const Vector3f& at : positions) {
            low = {std::min(low.x, at.x), std::min(low.y, at.y), std::min(low.z, at.z)};
            high = {std::max(high.x, at.x), std::max(high.y, at.y), std::max(high.z, at.z)};
        }
        scopes[m] = detail::ScopeOrEvery(*meshes[m].points, meshes[m].scope);
        for (const u32 point : scopes[m]) {
            if (point < meshes[m].points->pointCount) {
                points.push_back({m, point, PositionOf(positions, *meshes[m].points, point)});
            }
        }
    }
    if (segments.empty() || points.empty()) {
        for (u32 m = 0; m < meshes.size(); ++m) {
            GeneratedWeights none;
            none.unreached = static_cast<u32>(scopes[m].size());
            run.meshes.push_back(std::move(none));
        }
        return run;
    }

    // ---- 1. The grid and the solid -----------------------------------------
    Grid grid;
    const f32 longest = std::max({high.x - low.x, high.y - low.y, high.z - low.z});
    const u32 resolution = std::max<u32>(options.voxelResolution, 4);
    grid.cell = longest > 0.0f ? longest / static_cast<f32>(resolution) : 1.0f;
    grid.origin = Vector3f{low.x - kPad * grid.cell, low.y - kPad * grid.cell,
                           low.z - kPad * grid.cell};
    const f32 extent[3]{high.x - low.x, high.y - low.y, high.z - low.z};
    for (i32 axis = 0; axis < 3; ++axis) {
        grid.size[axis] = static_cast<i32>(std::ceil(extent[axis] / grid.cell)) + 1 + 2 * kPad;
    }
    const std::vector<u8> solid = BuildSolid(grid, meshes);
    u32 done = 1;
    const u32 total = 1 + static_cast<u32>(segments.bones.size());
    if (control.progress) {
        control.progress(done, total);
    }
    if (control.stopped()) {
        run.cancelled = true;
        return run;
    }

    // ---- 2. One walk per bone ----------------------------------------------
    const std::vector<Step> steps = Steps();
    std::vector<std::vector<std::pair<u32, f32>>> reached(segments.bones.size());
    const f32 falloff = options.geodesicFalloff > 0.0f ? options.geodesicFalloff : 4.0f;
    if (!detail::RunJobs(static_cast<u32>(segments.bones.size()), control, done, total,
                         [&](u32 index) {
                             reached[index] = WalkBone(grid, solid, steps, segments,
                                                       segments.bones[index], points, falloff);
                         })) {
        run.cancelled = true;
        return run;
    }

    // ---- 3. Gather in bone order, and normalise ----------------------------
    std::vector<std::vector<geom::Influence>> given(points.size());
    for (std::size_t b = 0; b < reached.size(); ++b) {
        for (const auto& [index, weight] : reached[b]) {
            given[index].push_back({segments.bones[b], weight});
        }
    }
    run.meshes.resize(meshes.size());
    std::vector<std::vector<u32>> islandsSeen(meshes.size());
    for (u32 index = 0; index < points.size(); ++index) {
        const RunPoint& at = points[index];
        GeneratedWeights& out = run.meshes[at.mesh];
        std::vector<geom::Influence>& weights = given[index];
        f32 sum = 0.0f;
        for (const geom::Influence& influence : weights) {
            sum += influence.weight;
        }
        if (!(sum > 0.0f) || !std::isfinite(sum)) {
            // No walk reached it: §8.1's fallback, rigid to the nearest.
            weights.clear();
            ++out.unreached;
            const u32 bone = NearestBone(segments, at.at);
            if (bone == kInvalidNode) {
                continue;
            }
            weights.push_back({bone, 1.0f});
        } else {
            for (geom::Influence& influence : weights) {
                influence.weight /= sum;
            }
        }
        out.points.push_back(at.point);
        out.given.add(weights);
        const PointTable& table = *meshes[at.mesh].points;
        if (at.point < table.islandOf.size()) {
            islandsSeen[at.mesh].push_back(table.islandOf[at.point]);
        }
    }
    for (u32 m = 0; m < meshes.size(); ++m) {
        std::vector<u32>& seen = islandsSeen[m];
        std::sort(seen.begin(), seen.end());
        seen.erase(std::unique(seen.begin(), seen.end()), seen.end());
        run.meshes[m].islands = static_cast<u32>(seen.size());
    }
    return run;
}

GenerateResult GeodesicWeights(Mesh& mesh, const NodeTree& nodes, const PointTable& points,
                               std::span<const u32> scope, const GenerateOptions& options,
                               const GenerateControl& control) {
    const MeshScope one[1]{{&mesh, &points, scope}};
    return detail::WriteOneMesh(mesh, nodes, points, SolveGeodesic(one, nodes, options, control),
                                options);
}

} // namespace skinning
} // namespace wem
} // namespace models
} // namespace whiteout
