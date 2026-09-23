// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/geometry/triangulation.h>

#include <whiteout/models/wem/geometry/mesh.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace whiteout {
namespace models {
namespace wem {
namespace geom {

// ============================================================================
// FaceTriangulation
// ============================================================================

std::span<const u32> FaceTriangulation::row(u32 face) const {
    if (face + 1 >= offsets.size()) {
        return {};
    }
    const u32 begin = offsets[face];
    const u32 end = offsets[face + 1];
    if (begin > end || end > vertices.size()) {
        return {};
    }
    return std::span<const u32>(vertices.data() + begin, end - begin);
}

void FaceTriangulation::appendFace() {
    if (!offsets.empty()) {
        offsets.push_back(offsets.back());
    }
}

void FaceTriangulation::clear() {
    offsets.clear();
    vertices.clear();
}

void FaceTriangulation::remapFaces(std::span<const u32> remap, u32 newCount) {
    if (offsets.empty()) {
        return;
    }
    FaceTriangulationBuilder builder(newCount);
    for (u32 f = 0; f + 1 < offsets.size() && f < remap.size(); ++f) {
        if (remap[f] != kInvalidId && remap[f] < newCount) {
            builder.set(remap[f], row(f));
        }
    }
    *this = builder.build();
}

void FaceTriangulation::remapVertices(std::span<const u32> remap) {
    if (offsets.empty()) {
        return;
    }
    const u32 slots = static_cast<u32>(offsets.size() - 1);
    FaceTriangulationBuilder builder(slots);
    std::vector<u32> mapped;
    for (u32 f = 0; f < slots; ++f) {
        mapped.clear();
        bool whole = true;
        for (const u32 v : row(f)) {
            const u32 to = v < remap.size() ? remap[v] : kInvalidId;
            whole = whole && to != kInvalidId;
            mapped.push_back(to);
        }
        if (whole) {
            builder.set(f, mapped);
        }
    }
    *this = builder.build();
}

void FaceTriangulation::rewriteRow(u32 face, std::span<const u32> from, std::span<const u32> to) {
    if (face + 1 >= offsets.size()) {
        return;
    }
    const std::size_t count = std::min(from.size(), to.size());
    for (u32 i = offsets[face]; i < offsets[face + 1] && i < vertices.size(); ++i) {
        for (std::size_t k = 0; k < count; ++k) {
            if (vertices[i] == from[k]) {
                vertices[i] = to[k];
                break;
            }
        }
    }
}

void FaceTriangulation::reverseRow(u32 face) {
    if (face + 1 >= offsets.size()) {
        return;
    }
    const u32 end = std::min<u32>(offsets[face + 1], static_cast<u32>(vertices.size()));
    for (u32 i = offsets[face]; i + 3 <= end; i += 3) {
        std::swap(vertices[i + 1], vertices[i + 2]);
    }
}

void FaceTriangulation::setRow(u32 face, std::span<const u32> triangles, u32 slotCount) {
    if (offsets.empty()) {
        if (triangles.empty()) {
            return;
        }
        offsets.assign(static_cast<std::size_t>(slotCount) + 1, 0);
    }
    if (face + 1 >= offsets.size()) {
        return;
    }
    const u32 begin = offsets[face];
    const u32 end = offsets[face + 1];
    const i64 delta = static_cast<i64>(triangles.size()) - static_cast<i64>(end - begin);
    vertices.erase(vertices.begin() + begin, vertices.begin() + end);
    vertices.insert(vertices.begin() + begin, triangles.begin(), triangles.end());
    for (std::size_t f = face + 1; f < offsets.size(); ++f) {
        offsets[f] = static_cast<u32>(static_cast<i64>(offsets[f]) + delta);
    }
}

FaceTriangulationBuilder::FaceTriangulationBuilder(u32 slotCount) : rows_(slotCount) {}

void FaceTriangulationBuilder::set(u32 face, std::span<const u32> triangles) {
    if (face < rows_.size()) {
        rows_[face].assign(triangles.begin(), triangles.end());
    }
}

FaceTriangulation FaceTriangulationBuilder::build() const {
    FaceTriangulation out;
    const bool any =
        std::any_of(rows_.begin(), rows_.end(), [](const std::vector<u32>& r) { return !r.empty(); });
    if (!any) {
        return out;
    }
    out.offsets.reserve(rows_.size() + 1);
    out.offsets.push_back(0);
    for (const std::vector<u32>& r : rows_) {
        out.vertices.insert(out.vertices.end(), r.begin(), r.end());
        out.offsets.push_back(static_cast<u32>(out.vertices.size()));
    }
    return out;
}

// ============================================================================
// The Newell plane
// ============================================================================

namespace {

struct Vec2 {
    f64 x = 0.0;
    f64 y = 0.0;
};

/// A face projected into its Newell plane, oriented so the loop runs
/// counter-clockwise: a triangle turning the loop's way has a positive area.
struct Plane {
    std::vector<Vec2> points; ///< Per corner.
    f64 facing = 0.0;         ///< The signed-area threshold, 1e-6 x the longest side squared.
    bool valid = false;       ///< False for a face with no Newell normal.
};

Plane project(std::span<const u32> corners, std::span<const Vector3f> positions) {
    Plane plane;
    const std::size_t n = corners.size();
    if (n < 3) {
        return plane;
    }
    const auto at = [&](std::size_t i) -> Vector3f {
        const u32 v = corners[i % n];
        return v < positions.size() ? positions[v] : Vector3f{0.0f, 0.0f, 0.0f};
    };
    f64 nx = 0.0;
    f64 ny = 0.0;
    f64 nz = 0.0;
    f64 longest = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const Vector3f a = at(i);
        const Vector3f b = at(i + 1);
        nx += (static_cast<f64>(a.y) - b.y) * (static_cast<f64>(a.z) + b.z);
        ny += (static_cast<f64>(a.z) - b.z) * (static_cast<f64>(a.x) + b.x);
        nz += (static_cast<f64>(a.x) - b.x) * (static_cast<f64>(a.y) + b.y);
        const f64 dx = static_cast<f64>(b.x) - a.x;
        const f64 dy = static_cast<f64>(b.y) - a.y;
        const f64 dz = static_cast<f64>(b.z) - a.z;
        longest = std::max(longest, dx * dx + dy * dy + dz * dz);
    }
    const f64 length = std::sqrt(nx * nx + ny * ny + nz * nz);
    // The Newell vector's length is twice the face's area. An area within the
    // facing threshold is a line, whose f32 corners never quite cancel.
    if (!(length > 2e-6 * longest) || !(longest > 0.0)) {
        return plane;
    }
    nx /= length;
    ny /= length;
    nz /= length;
    // u: the world axis least along n, made orthogonal; v = n x u.
    f64 ax = 0.0;
    f64 ay = 0.0;
    f64 az = 0.0;
    if (std::abs(nx) <= std::abs(ny) && std::abs(nx) <= std::abs(nz)) {
        ax = 1.0;
    } else if (std::abs(ny) <= std::abs(nz)) {
        ay = 1.0;
    } else {
        az = 1.0;
    }
    const f64 dot = ax * nx + ay * ny + az * nz;
    f64 ux = ax - nx * dot;
    f64 uy = ay - ny * dot;
    f64 uz = az - nz * dot;
    const f64 ul = std::sqrt(ux * ux + uy * uy + uz * uz);
    ux /= ul;
    uy /= ul;
    uz /= ul;
    const f64 vx = ny * uz - nz * uy;
    const f64 vy = nz * ux - nx * uz;
    const f64 vz = nx * uy - ny * ux;
    const Vector3f origin = at(0);
    plane.points.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        const Vector3f p = at(i);
        const f64 px = static_cast<f64>(p.x) - origin.x;
        const f64 py = static_cast<f64>(p.y) - origin.y;
        const f64 pz = static_cast<f64>(p.z) - origin.z;
        plane.points[i] = Vec2{px * ux + py * uy + pz * uz, px * vx + py * vy + pz * vz};
    }
    plane.facing = 1e-6 * longest;
    plane.valid = true;
    return plane;
}

/// Twice the signed area of (a, b, c): positive when it turns counter-clockwise.
f64 area2(const Vec2& a, const Vec2& b, const Vec2& c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

bool faces(const Plane& plane, u32 a, u32 b, u32 c) {
    return 0.5 * area2(plane.points[a], plane.points[b], plane.points[c]) > plane.facing;
}

/// Closed containment: on an edge or a corner counts as inside.
bool holds(const Vec2& a, const Vec2& b, const Vec2& c, const Vec2& p) {
    const f64 ab = area2(a, b, p);
    const f64 bc = area2(b, c, p);
    const f64 ca = area2(c, a, p);
    return (ab >= 0.0 && bc >= 0.0 && ca >= 0.0) || (ab <= 0.0 && bc <= 0.0 && ca <= 0.0);
}

/// The smallest angle of (a, b, c), the same whichever corner it starts from.
f64 minAngle(const Plane& plane, std::array<u32, 3> t) {
    std::sort(t.begin(), t.end());
    f64 smallest = 4.0;
    for (u32 k = 0; k < 3; ++k) {
        const Vec2& p = plane.points[t[k]];
        const Vec2& q = plane.points[t[(k + 1) % 3]];
        const Vec2& r = plane.points[t[(k + 2) % 3]];
        const f64 ux = q.x - p.x;
        const f64 uy = q.y - p.y;
        const f64 wx = r.x - p.x;
        const f64 wy = r.y - p.y;
        const f64 angle = std::atan2(std::abs(ux * wy - uy * wx), ux * wx + uy * wy);
        smallest = std::min(smallest, angle);
    }
    return smallest;
}

/// The four-corner rule. Emits corner ordinals.
CutNote cutQuad(const Plane& plane, std::span<const u32> corners,
                std::span<const Vector3f> positions, std::vector<u32>& out) {
    const bool across02 = faces(plane, 0, 1, 2) && faces(plane, 0, 2, 3);
    const bool across13 = faces(plane, 1, 2, 3) && faces(plane, 1, 3, 0);
    bool take02 = true;
    CutNote note = CutNote::None;
    if (across02 && across13) {
        const auto length = [&](u32 a, u32 b) {
            const Vector3f& p = positions[corners[a]];
            const Vector3f& q = positions[corners[b]];
            const f64 dx = static_cast<f64>(p.x) - q.x;
            const f64 dy = static_cast<f64>(p.y) - q.y;
            const f64 dz = static_cast<f64>(p.z) - q.z;
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        };
        const f64 d02 = length(0, 2);
        const f64 d13 = length(1, 3);
        take02 = !(d13 < d02 - 1e-6 * std::max(d02, d13));
    } else if (across13) {
        take02 = false;
    } else if (!across02) {
        note = CutNote::Folded;
    }
    if (take02) {
        out.insert(out.end(), {0u, 1u, 2u, 0u, 2u, 3u});
    } else {
        out.insert(out.end(), {1u, 2u, 3u, 1u, 3u, 0u});
    }
    return note;
}

/// Ear clipping, then Lawson flips. Emits corner ordinals.
CutNote clipEars(const Plane& plane, u32 n, std::vector<u32>& out) {
    CutNote note = CutNote::None;
    std::vector<u32> left(n);
    for (u32 i = 0; i < n; ++i) {
        left[i] = i;
    }
    std::vector<std::array<u32, 3>> triangles;
    triangles.reserve(n - 2);
    while (left.size() > 3) {
        const std::size_t m = left.size();
        const auto cornerAt = [&](std::size_t k) {
            return std::array<u32, 3>{left[(k + m - 1) % m], left[k], left[(k + 1) % m]};
        };
        const auto areaAt = [&](std::size_t k) {
            const std::array<u32, 3> t = cornerAt(k);
            return 0.5 * area2(plane.points[t[0]], plane.points[t[1]], plane.points[t[2]]);
        };
        const auto empty = [&](std::size_t k) {
            const std::array<u32, 3> t = cornerAt(k);
            for (std::size_t j = 0; j < m; ++j) {
                const u32 other = left[j];
                if (other == t[0] || other == t[1] || other == t[2]) {
                    continue;
                }
                if (holds(plane.points[t[0]], plane.points[t[1]], plane.points[t[2]],
                          plane.points[other])) {
                    return false;
                }
            }
            return true;
        };
        std::size_t pick = m;
        for (std::size_t k = 0; k < m && pick == m; ++k) {
            if (areaAt(k) > plane.facing && empty(k)) {
                pick = k;
            }
        }
        for (std::size_t k = 0; k < m && pick == m; ++k) {
            if (std::abs(areaAt(k)) <= plane.facing && empty(k)) {
                pick = k;
            }
        }
        if (pick == m) {
            // No ear: the non-reflex corner of largest area, or failing that
            // the corner of largest area, so every step still clips one.
            note = CutNote::NoEar;
            f64 best = 0.0;
            for (std::size_t k = 0; k < m; ++k) {
                const f64 area = areaAt(k);
                if (area >= -plane.facing && (pick == m || area > best)) {
                    pick = k;
                    best = area;
                }
            }
            for (std::size_t k = 0; pick == m && k < m; ++k) {
                const f64 area = areaAt(k);
                if (k == 0 || area > best) {
                    best = area;
                }
            }
            for (std::size_t k = 0; pick == m && k < m; ++k) {
                if (areaAt(k) == best) {
                    pick = k;
                }
            }
        }
        triangles.push_back(cornerAt(pick));
        left.erase(left.begin() + static_cast<std::ptrdiff_t>(pick));
    }
    triangles.push_back({left[0], left[1], left[2]});

    // Lawson flips across interior diagonals, each on a strict gain only: above
    // 1e-6 rad, since a cocircular polygon's f32 corners tie only to within
    // their rounding, and flipping on that noise is flipping on a tie.
    const std::size_t cap = static_cast<std::size_t>(4) * n * n;
    bool changed = true;
    for (std::size_t round = 0; changed && round < cap; ++round) {
        changed = false;
        for (std::size_t i = 0; i < triangles.size() && !changed; ++i) {
            for (u32 side = 0; side < 3 && !changed; ++side) {
                const u32 a = triangles[i][side];
                const u32 b = triangles[i][(side + 1) % 3];
                const u32 c = triangles[i][(side + 2) % 3];
                // A loop edge has no triangle beyond it.
                if ((a + 1) % n == b || (b + 1) % n == a) {
                    continue;
                }
                for (std::size_t j = i + 1; j < triangles.size() && !changed; ++j) {
                    for (u32 s = 0; s < 3; ++s) {
                        if (triangles[j][s] != b || triangles[j][(s + 1) % 3] != a) {
                            continue;
                        }
                        const u32 d = triangles[j][(s + 2) % 3];
                        // The quad a, d, b, c must be strictly convex.
                        const Vec2& pa = plane.points[a];
                        const Vec2& pb = plane.points[b];
                        const Vec2& pc = plane.points[c];
                        const Vec2& pd = plane.points[d];
                        const bool convex = 0.5 * area2(pa, pd, pb) > plane.facing &&
                                            0.5 * area2(pd, pb, pc) > plane.facing &&
                                            0.5 * area2(pb, pc, pa) > plane.facing &&
                                            0.5 * area2(pc, pa, pd) > plane.facing;
                        if (!convex) {
                            break;
                        }
                        const f64 before =
                            std::min(minAngle(plane, {a, b, c}), minAngle(plane, {b, a, d}));
                        const f64 after =
                            std::min(minAngle(plane, {c, a, d}), minAngle(plane, {d, b, c}));
                        if (after > before + 1e-6) {
                            triangles[i] = {c, a, d};
                            triangles[j] = {d, b, c};
                            changed = true;
                        }
                        break;
                    }
                }
            }
        }
    }
    for (const std::array<u32, 3>& t : triangles) {
        out.insert(out.end(), t.begin(), t.end());
    }
    return note;
}

} // namespace

// ============================================================================
// Validity and the one function
// ============================================================================

bool RowValid(std::span<const u32> cornerVertices, std::span<const Vector3f> positions,
              std::span<const u32> row) {
    const std::size_t n = cornerVertices.size();
    if (n < 4 || row.size() != 3 * (n - 2)) {
        return false;
    }
    // Vertex ids to loop ordinals; a face never repeats a vertex (C6).
    std::vector<std::pair<u32, u32>> ordinals(n);
    for (u32 i = 0; i < n; ++i) {
        ordinals[i] = {cornerVertices[i], i};
    }
    std::sort(ordinals.begin(), ordinals.end());
    const auto ordinalOf = [&](u32 vertex) -> u32 {
        const auto found = std::lower_bound(ordinals.begin(), ordinals.end(),
                                            std::pair<u32, u32>{vertex, 0u});
        return found != ordinals.end() && found->first == vertex ? found->second : kInvalidId;
    };
    const Plane plane = project(cornerVertices, positions);
    if (!plane.valid) {
        return false;
    }
    // Each directed side of each triangle, as (from, to) ordinals.
    std::vector<std::pair<u32, u32>> sides;
    sides.reserve(row.size());
    for (std::size_t t = 0; t < row.size(); t += 3) {
        const u32 a = ordinalOf(row[t]);
        const u32 b = ordinalOf(row[t + 1]);
        const u32 c = ordinalOf(row[t + 2]);
        if (a == kInvalidId || b == kInvalidId || c == kInvalidId || a == b || b == c || a == c) {
            return false;
        }
        if (!faces(plane, a, b, c)) {
            return false;
        }
        sides.push_back({a, b});
        sides.push_back({b, c});
        sides.push_back({c, a});
    }
    std::sort(sides.begin(), sides.end());
    if (std::adjacent_find(sides.begin(), sides.end()) != sides.end()) {
        return false; // a directed side twice
    }
    const auto has = [&](u32 from, u32 to) {
        return std::binary_search(sides.begin(), sides.end(), std::pair<u32, u32>{from, to});
    };
    std::vector<std::pair<u32, u32>> diagonals;
    for (const auto& [from, to] : sides) {
        const bool loopForward = (from + 1) % n == to;
        const bool loopBackward = (to + 1) % n == from;
        if (loopBackward) {
            return false; // a loop edge run against the loop
        }
        if (loopForward) {
            continue;
        }
        // A diagonal: both ways, exactly once each.
        if (!has(to, from)) {
            return false;
        }
        if (from < to) {
            diagonals.push_back({from, to});
        }
    }
    for (u32 i = 0; i < n; ++i) {
        if (!has(i, (i + 1) % static_cast<u32>(n))) {
            return false; // a loop edge in no triangle
        }
    }
    // No two diagonals cross: nested or disjoint intervals only.
    std::sort(diagonals.begin(), diagonals.end(), [](const auto& x, const auto& y) {
        return x.first != y.first ? x.first < y.first : x.second > y.second;
    });
    std::vector<u32> open;
    for (const auto& [a, b] : diagonals) {
        while (!open.empty() && open.back() <= a) {
            open.pop_back();
        }
        if (!open.empty() && open.back() < b) {
            return false;
        }
        open.push_back(b);
    }
    return true;
}

CutNote TriangulateFace(std::span<const u32> cornerVertices, std::span<const Vector3f> positions,
                        std::span<const u32> storedRow, std::vector<u32>& out) {
    const u32 n = static_cast<u32>(cornerVertices.size());
    if (n < 3) {
        return CutNote::None;
    }
    if (n == 3) {
        out.insert(out.end(), {0u, 1u, 2u});
        return CutNote::None;
    }
    if (!storedRow.empty() && RowValid(cornerVertices, positions, storedRow)) {
        for (const u32 vertex : storedRow) {
            for (u32 i = 0; i < n; ++i) {
                if (cornerVertices[i] == vertex) {
                    out.push_back(i);
                    break;
                }
            }
        }
        return CutNote::Stored;
    }
    const Plane plane = project(cornerVertices, positions);
    if (!plane.valid) {
        for (u32 c = 1; c + 1 < n; ++c) {
            out.insert(out.end(), {0u, c, c + 1});
        }
        return CutNote::Degenerate;
    }
    if (n == 4) {
        return cutQuad(plane, cornerVertices, positions, out);
    }
    return clipEars(plane, n, out);
}

// ============================================================================
// Over a mesh
// ============================================================================

std::vector<u32> FaceSetSlots(const Mesh& mesh) {
    // `toFaceSet` skips exactly these.
    std::vector<u32> slots;
    if (mesh.hasConnectivity()) {
        const Topology& topology = mesh.topology();
        for (u32 f = 0; f < topology.faceCount(); ++f) {
            if (!topology.isDeleted(FaceId(f)) && topology.halfedge(FaceId(f)).valid()) {
                slots.push_back(f);
            }
        }
        return slots;
    }
    slots.resize(mesh.faceSet().faceCount());
    for (u32 f = 0; f < slots.size(); ++f) {
        slots[f] = f;
    }
    return slots;
}

void TriangulateMesh(const Mesh& mesh, std::vector<u32>& triangles, std::vector<u32>* faceOf) {
    const FaceSet& faces = mesh.faceSet();
    const std::span<const Vector3f> positions =
        mesh.attributes.get<const Vector3f>(names::kPosition, Domain::Vertex);
    const std::vector<u32> slotOf = FaceSetSlots(mesh);
    std::vector<u32> cut;
    std::size_t corner = 0;
    for (std::size_t f = 0; f < faces.faceCount(); ++f) {
        const u32 valence = faces.faceValence[f];
        const std::span<const u32> loop(faces.cornerVertex.data() + corner, valence);
        const u32 slot = f < slotOf.size() ? slotOf[f] : static_cast<u32>(f);
        cut.clear();
        TriangulateFace(loop, positions, mesh.triangulation.row(slot), cut);
        for (const u32 ordinal : cut) {
            triangles.push_back(loop[ordinal]);
        }
        if (faceOf != nullptr) {
            faceOf->insert(faceOf->end(), cut.size() / 3, slot);
        }
        corner += valence;
    }
}

u32 MaterialiseRows(Mesh& mesh, std::span<const u32> faces) {
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        return 0;
    }
    const Mesh& readable = mesh;
    const Topology& topology = readable.topology();
    const std::span<const Vector3f> positions =
        readable.attributes.get<const Vector3f>(names::kPosition, Domain::Vertex);
    const u32 slots = topology.faceCount();
    std::vector<u32> all;
    if (faces.empty()) {
        all.resize(slots);
        for (u32 f = 0; f < slots; ++f) {
            all[f] = f;
        }
        faces = all;
    }
    std::vector<u32> loop;
    std::vector<u32> cut;
    std::vector<std::pair<u32, std::vector<u32>>> written;
    for (const u32 f : faces) {
        if (f >= slots || topology.isDeleted(FaceId(f))) {
            continue;
        }
        loop.clear();
        for (const HalfedgeId h : topology.fh(FaceId(f))) {
            loop.push_back(topology.from(h).value());
        }
        if (loop.size() < 4) {
            continue;
        }
        const std::span<const u32> stored = readable.triangulation.row(f);
        if (!stored.empty() && RowValid(loop, positions, stored)) {
            continue;
        }
        cut.clear();
        TriangulateFace(loop, positions, {}, cut);
        std::vector<u32> row;
        row.reserve(cut.size());
        for (const u32 ordinal : cut) {
            row.push_back(loop[ordinal]);
        }
        written.push_back({f, std::move(row)});
    }
    if (written.empty()) {
        return 0;
    }
    // One build for the lot: the rows that stay, then the new ones.
    FaceTriangulationBuilder builder(slots);
    for (u32 f = 0; f < slots; ++f) {
        builder.set(f, readable.triangulation.row(f));
    }
    for (const auto& [face, row] : written) {
        builder.set(face, row);
    }
    mesh.triangulation = builder.build();
    return static_cast<u32>(written.size());
}

} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
