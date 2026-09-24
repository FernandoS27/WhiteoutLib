// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/geometry/uv/flatten.h>

#include "common.h"

#include <whiteout/models/wem/geometry/uv/seams.h>

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <utility>

namespace whiteout {
namespace models {
namespace wem {
namespace geom {
namespace uv {

namespace {

using detail::Tri;

constexpr f32 kPi = 3.14159265358979323846f;

/// The least-squares system, rows of at most six entries over the free wedges'
/// two columns each. Compressed by row because that is the order both `A` and
/// `Aᵀ` are applied in, and neither ever wants a column.
struct Csr {
    std::vector<u32> rowStart;
    std::vector<u32> column;
    std::vector<f32> value;
    std::vector<f32> rhs;
    u32 columns = 0;

    void beginRow() {
        rowStart.push_back(static_cast<u32>(column.size()));
    }
    void push(u32 col, f32 v) {
        column.push_back(col);
        value.push_back(v);
    }
    void endRow(f32 b) {
        rhs.push_back(b);
    }
    u32 rows() const {
        return static_cast<u32>(rhs.size());
    }
    void seal() {
        rowStart.push_back(static_cast<u32>(column.size()));
    }

    /// `out = A x`.
    void apply(const std::vector<f32>& x, std::vector<f32>& out) const {
        out.assign(rows(), 0.0f);
        for (u32 r = 0; r < rows(); ++r) {
            f32 sum = 0.0f;
            for (u32 i = rowStart[r]; i < rowStart[r + 1]; ++i) {
                sum += value[i] * x[column[i]];
            }
            out[r] = sum;
        }
    }

    /// `out = Aᵀ y`.
    void applyTransposed(const std::vector<f32>& y, std::vector<f32>& out) const {
        out.assign(columns, 0.0f);
        for (u32 r = 0; r < rows(); ++r) {
            const f32 scale = y[r];
            if (scale == 0.0f) {
                continue;
            }
            for (u32 i = rowStart[r]; i < rowStart[r + 1]; ++i) {
                out[column[i]] += value[i] * scale;
            }
        }
    }
};

f32 norm2(const std::vector<f32>& v) {
    f64 sum = 0.0;
    for (const f32 x : v) {
        sum += static_cast<f64>(x) * static_cast<f64>(x);
    }
    return static_cast<f32>(sum);
}

/// CGLS: conjugate gradients on the normal equations, applying `A` and `Aᵀ` and
/// never forming `AᵀA` (which would square the condition number and the
/// memory both).
u32 SolveCgls(const Csr& a, std::vector<f32>& x, f32 tolerance, u32 maxIterations,
              f32& residualOut) {
    std::vector<f32> r(a.rows(), 0.0f);
    a.apply(x, r);
    for (u32 i = 0; i < a.rows(); ++i) {
        r[i] = a.rhs[i] - r[i];
    }
    std::vector<f32> s;
    a.applyTransposed(r, s);
    std::vector<f32> p = s;
    f32 gamma = norm2(s);
    const f32 gamma0 = gamma;
    residualOut = gamma0 > 0.0f ? 1.0f : 0.0f;
    if (gamma0 <= 0.0f) {
        return 0;
    }
    std::vector<f32> q;
    u32 iteration = 0;
    for (; iteration < maxIterations; ++iteration) {
        a.apply(p, q);
        const f32 qq = norm2(q);
        if (qq <= 0.0f) {
            break;
        }
        const f32 alpha = gamma / qq;
        for (u32 i = 0; i < a.columns; ++i) {
            x[i] += alpha * p[i];
        }
        for (u32 i = 0; i < a.rows(); ++i) {
            r[i] -= alpha * q[i];
        }
        a.applyTransposed(r, s);
        const f32 next = norm2(s);
        residualOut = std::sqrt(next / gamma0);
        if (residualOut < tolerance) {
            ++iteration;
            break;
        }
        const f32 beta = next / gamma;
        for (u32 i = 0; i < a.columns; ++i) {
            p[i] = s[i] + beta * p[i];
        }
        gamma = next;
    }
    return iteration;
}

/// Writes @p value onto every corner of @p wedge.
void writeWedge(const UvIslands& islands, std::span<Vector2f> uvs, u32 wedge,
                const Vector2f& value) {
    for (const u32 corner : islands.cornersOf(wedge)) {
        if (corner < uvs.size()) {
            uvs[corner] = value;
        }
    }
}

Vector2f readWedge(const UvIslands& islands, std::span<const Vector2f> uvs, u32 wedge) {
    const std::span<const u32> corners = islands.cornersOf(wedge);
    if (corners.empty() || corners[0] >= uvs.size()) {
        return Vector2f{0.0f, 0.0f};
    }
    return uvs[corners[0]];
}

} // namespace

FlattenResult Lscm(Mesh& mesh, const UvIslands& islands, u32 island, u32 set,
                   const LscmOptions& options) {
    return LscmPinning(mesh, islands, island, set, {}, options);
}

FlattenResult LscmPinning(Mesh& mesh, const UvIslands& islands, u32 island, u32 set,
                          std::span<const u32> alsoPinned, const LscmOptions& options) {
    FlattenResult result;
    if (!mesh.hasConnectivity() || island >= islands.count) {
        result.refusal = FlattenResult::Refusal::NoFaces;
        return result;
    }
    if (islands.closed[island] != 0) {
        // Nothing holds a sphere flat. Saying so is the honest answer, and the
        // caller's is a cut (§6.1).
        result.refusal = FlattenResult::Refusal::Closed;
        return result;
    }
    const std::span<const u32> faces = islands.facesOf(island);
    if (faces.empty()) {
        result.refusal = FlattenResult::Refusal::NoFaces;
        return result;
    }

    const Topology& topology = std::as_const(mesh).topology();
    const std::span<const Vector3f> positions =
        std::as_const(mesh).attributes.get<const Vector3f>(names::kPosition, Domain::Vertex);
    const std::span<Vector2f> uvs =
        mesh.attributes.getOrCreate<Vector2f>(names::uv(set), Domain::Halfedge, AttrType::F32x2);
    const std::span<const u8> pins =
        std::as_const(mesh).attributes.get<const u8>(names::uvPin(set), Domain::Halfedge);

    // --- the island's wedges, and which of them are held ---------------------
    const std::vector<u32> wedges = IslandWedges(islands, mesh, island);
    if (wedges.size() < 3) {
        result.refusal = FlattenResult::Refusal::NoFaces;
        return result;
    }
    std::vector<u32> localOf(islands.wedgeCount, kInvalidId);
    for (u32 i = 0; i < wedges.size(); ++i) {
        localOf[wedges[i]] = i;
    }
    std::vector<u8> pinned(wedges.size(), 0);
    u32 pinCount = 0;
    for (u32 i = 0; i < wedges.size(); ++i) {
        for (const u32 corner : islands.cornersOf(wedges[i])) {
            if (corner < pins.size() && pins[corner] != 0) {
                pinned[i] = 1;
                break;
            }
        }
        pinCount += pinned[i];
    }
    for (const u32 wedge : alsoPinned) {
        if (wedge < localOf.size() && localOf[wedge] != kInvalidId && pinned[localOf[wedge]] == 0) {
            pinned[localOf[wedge]] = 1;
            ++pinCount;
        }
    }

    std::vector<Vector2f> held(wedges.size(), Vector2f{0.0f, 0.0f});
    for (u32 i = 0; i < wedges.size(); ++i) {
        held[i] = readWedge(islands, uvs, wedges[i]);
    }

    if (pinCount < 2) {
        // The two boundary wedges farthest apart, over the boundary only
        // (EDIT_MODE_UV_PLAN.md P2): a pin's whole job is to fix the map's
        // place, turn and scale, and two ends of the island fix all three.
        std::vector<u32> border;
        for (const HalfedgeId h : islands.boundaryOf(island)) {
            const u32 wedge = islands.wedgeOf(h);
            if (wedge != kInvalidId && localOf[wedge] != kInvalidId) {
                border.push_back(localOf[wedge]);
            }
        }
        std::sort(border.begin(), border.end());
        border.erase(std::unique(border.begin(), border.end()), border.end());
        if (border.size() < 2) {
            result.refusal = FlattenResult::Refusal::TooFewPins;
            return result;
        }
        const detail::IslandArea areas = detail::AreasOf(mesh, islands, island, positions, uvs);
        // Farthest apart in the map when the map is what is being held, and in
        // the world when it is not. Holding a pair that the file's own layout
        // put close together would squeeze the whole island into the gap
        // between them, and that is where the flips come from.
        const bool byUv = options.holdCurrent && areas.uv > 0.0f;
        u32 bestA = border[0];
        u32 bestB = border[1];
        f32 best = -1.0f;
        for (std::size_t i = 0; i < border.size(); ++i) {
            const Vector3f pi = positions[islands.wedgeVertex[wedges[border[i]]]];
            const Vector2f qi = held[border[i]];
            for (std::size_t j = i + 1; j < border.size(); ++j) {
                f32 distance;
                if (byUv) {
                    const Vector2f qj = held[border[j]];
                    distance = std::sqrt((qj.x - qi.x) * (qj.x - qi.x) +
                                         (qj.y - qi.y) * (qj.y - qi.y));
                } else {
                    distance = (positions[islands.wedgeVertex[wedges[border[j]]]] - pi).length();
                }
                // The lower wedge id breaks a tie, so the same island gives the
                // same pair on every run.
                if (distance > best) {
                    best = distance;
                    bestA = border[i];
                    bestB = border[j];
                }
            }
        }
        if (!byUv) {
            // Nowhere to hold it: a unit segment, which fixes place, turn and
            // scale and leaves the caller to say where it really goes.
            held[bestA] = Vector2f{0.0f, 0.0f};
            held[bestB] = Vector2f{1.0f, 0.0f};
        }
        pinned[bestA] = 1;
        pinned[bestB] = 1;
        pinCount = 2;
    }

    // --- the system ----------------------------------------------------------
    std::vector<u32> columnOf(wedges.size(), kInvalidId);
    u32 columns = 0;
    for (u32 i = 0; i < wedges.size(); ++i) {
        if (pinned[i] == 0) {
            columnOf[i] = columns;
            columns += 2;
        }
    }
    if (columns == 0) {
        // Everything is held: the answer is what is already there.
        return result;
    }

    Csr a;
    a.columns = columns;
    for (const u32 face : faces) {
        for (const Tri& tri : detail::TrianglesOf(mesh, FaceId(face))) {
            const Vector3f p[3] = {positions[topology.from(tri.corner[0]).index()],
                                   positions[topology.from(tri.corner[1]).index()],
                                   positions[topology.from(tri.corner[2]).index()]};
            // The triangle's own plane, as a frame: the first edge is x, the
            // normal gives y. Conformality is a statement about this frame, so
            // the map never sees the model's axes at all.
            const Vector3f e1 = p[1] - p[0];
            const Vector3f e2 = p[2] - p[0];
            const Vector3f n = cross(e1, e2);
            const f32 twiceArea = n.length();
            const f32 xLength = e1.length();
            if (twiceArea <= 0.0f || xLength <= 0.0f) {
                ++result.degenerate;
                continue;
            }
            const Vector3f xAxis = e1 * (1.0f / xLength);
            const Vector3f yAxis = cross(n * (1.0f / twiceArea), xAxis);
            const f32 local[3][2] = {{0.0f, 0.0f},
                                     {xLength, 0.0f},
                                     {e2.dot(xAxis), e2.dot(yAxis)}};
            const f32 dT = local[1][0] * local[2][1] - local[1][1] * local[2][0];
            if (dT == 0.0f) {
                ++result.degenerate;
                continue;
            }
            const f32 scale = 1.0f / std::sqrt(std::abs(dT));
            // W_k = (x_{k+2} - x_{k+1}) + i (y_{k+2} - y_{k+1}).
            f32 wr[3];
            f32 wi[3];
            for (u32 k = 0; k < 3; ++k) {
                const u32 next = (k + 1) % 3;
                const u32 last = (k + 2) % 3;
                wr[k] = (local[last][0] - local[next][0]) * scale;
                wi[k] = (local[last][1] - local[next][1]) * scale;
            }

            u32 local3[3];
            bool ok = true;
            for (u32 k = 0; k < 3; ++k) {
                const u32 wedge = islands.wedgeOf(tri.corner[k]);
                if (wedge == kInvalidId || localOf[wedge] == kInvalidId) {
                    ok = false;
                    break;
                }
                local3[k] = localOf[wedge];
            }
            if (!ok) {
                continue;
            }

            // Two real rows per triangle: the real and imaginary parts of
            // `Σ W_k u_k = 0`, with the held columns moved to the right.
            for (u32 part = 0; part < 2; ++part) {
                a.beginRow();
                f32 b = 0.0f;
                for (u32 k = 0; k < 3; ++k) {
                    const f32 cu = part == 0 ? wr[k] : wi[k];
                    const f32 cv = part == 0 ? -wi[k] : wr[k];
                    const u32 index = local3[k];
                    if (pinned[index] != 0) {
                        b -= cu * held[index].x + cv * held[index].y;
                        continue;
                    }
                    const u32 column = columnOf[index];
                    a.push(column, cu);
                    a.push(column + 1, cv);
                }
                a.endRow(b);
            }
        }
    }
    a.seal();
    if (a.rows() == 0) {
        result.refusal = FlattenResult::Refusal::NoFaces;
        return result;
    }

    std::vector<f32> x(columns, 0.0f);
    for (u32 i = 0; i < wedges.size(); ++i) {
        if (columnOf[i] != kInvalidId) {
            // From where it is: a re-unwrap of a map that is nearly right
            // converges in a handful of iterations instead of from nothing.
            x[columnOf[i]] = held[i].x;
            x[columnOf[i] + 1] = held[i].y;
        }
    }
    const u32 cap = std::max<u32>(32u, options.iterationsPerUnknown * columns);
    result.iterations = SolveCgls(a, x, options.tolerance, cap, result.residual);

    for (u32 i = 0; i < wedges.size(); ++i) {
        const Vector2f value = pinned[i] != 0
                                   ? held[i]
                                   : Vector2f{x[columnOf[i]], x[columnOf[i] + 1]};
        writeWedge(islands, uvs, wedges[i], value);
    }

    // --- flips ---------------------------------------------------------------
    //
    // Counted against the island's own majority, not against a convention: a
    // map that came out mirrored as a whole is not wrong, and one triangle
    // that turned over is.
    i32 balance = 0;
    std::vector<f32> signs;
    for (const u32 face : faces) {
        for (const Tri& tri : detail::TrianglesOf(mesh, FaceId(face))) {
            const f32 area = detail::TriAreaUv(uvs[tri.corner[0].index()],
                                               uvs[tri.corner[1].index()],
                                               uvs[tri.corner[2].index()]);
            if (area == 0.0f) {
                continue;
            }
            signs.push_back(area);
            balance += area > 0.0f ? 1 : -1;
        }
    }
    const bool positive = balance >= 0;
    for (const f32 area : signs) {
        if ((area > 0.0f) != positive) {
            ++result.flipped;
        }
    }
    return result;
}

namespace {

/// One triangle of the island as the relaxer sees it: its three corners, the
/// three world positions, and the 3D area it is weighted by.
struct RingTri {
    u32 corner[3];
    u32 wedge[3];
    Vector3f p[3];
    f32 area = 0.0f;
};

/// Sander's L2 squared for one triangle at the given UVs, and the sign of its
/// UV area, which is what a flip is.
f32 triStretch(const RingTri& tri, const Vector2f q[3], f32& signedArea) {
    signedArea = detail::TriAreaUv(q[0], q[1], q[2]);
    if (signedArea == 0.0f) {
        return 0.0f;
    }
    const f32 twice = 2.0f * signedArea;
    const Vector3f ss = (tri.p[0] * (q[1].y - q[2].y) + tri.p[1] * (q[2].y - q[0].y) +
                         tri.p[2] * (q[0].y - q[1].y)) *
                        (1.0f / twice);
    const Vector3f st = (tri.p[0] * (q[2].x - q[1].x) + tri.p[1] * (q[0].x - q[2].x) +
                         tri.p[2] * (q[1].x - q[0].x)) *
                        (1.0f / twice);
    return (ss.dot(ss) + st.dot(st)) * 0.5f;
}

} // namespace

FlattenResult Relax(Mesh& mesh, const UvIslands& islands, std::span<const u32> wedges, u32 set,
                    const RelaxOptions& options) {
    FlattenResult result;
    if (!mesh.hasConnectivity() || wedges.empty()) {
        result.refusal = FlattenResult::Refusal::NoFaces;
        return result;
    }
    const Topology& topology = std::as_const(mesh).topology();
    const std::span<const Vector3f> positions =
        std::as_const(mesh).attributes.get<const Vector3f>(names::kPosition, Domain::Vertex);
    const std::span<Vector2f> uvs =
        mesh.attributes.getOrCreate<Vector2f>(names::uv(set), Domain::Halfedge, AttrType::F32x2);
    const std::span<const u8> pins =
        std::as_const(mesh).attributes.get<const u8>(names::uvPin(set), Domain::Halfedge);

    // The wedges asked for, each once, and the islands they are in.
    std::vector<u32> moving(wedges.begin(), wedges.end());
    std::sort(moving.begin(), moving.end());
    moving.erase(std::unique(moving.begin(), moving.end()), moving.end());
    std::vector<u32> touched;
    for (const u32 wedge : moving) {
        if (wedge < islands.wedgeIsland.size()) {
            touched.push_back(islands.wedgeIsland[wedge]);
        }
    }
    std::sort(touched.begin(), touched.end());
    touched.erase(std::unique(touched.begin(), touched.end()), touched.end());

    // Held: the pinned, and the boundary unless it was asked for.
    std::vector<u8> held(islands.wedgeCount, 0);
    for (u32 w = 0; w < islands.wedgeCount; ++w) {
        for (const u32 corner : islands.cornersOf(w)) {
            if (corner < pins.size() && pins[corner] != 0) {
                held[w] = 1;
                break;
            }
        }
    }
    if (options.holdBoundary) {
        for (const u32 island : touched) {
            for (const HalfedgeId h : islands.boundaryOf(island)) {
                const u32 wedge = islands.wedgeOf(h);
                if (wedge != kInvalidId) {
                    held[wedge] = 1;
                }
                const u32 far = islands.wedgeOf(topology.next(h));
                if (far != kInvalidId) {
                    held[far] = 1;
                }
            }
        }
    }

    // The one ring of every wedge that may move.
    std::vector<RingTri> tris;
    std::vector<std::vector<u32>> ringOf(islands.wedgeCount);
    for (const u32 island : touched) {
        for (const u32 face : islands.facesOf(island)) {
            for (const Tri& tri : detail::TrianglesOf(mesh, FaceId(face))) {
                RingTri ring;
                bool ok = true;
                for (u32 k = 0; k < 3; ++k) {
                    ring.corner[k] = tri.corner[k].index();
                    ring.wedge[k] = islands.wedgeOf(tri.corner[k]);
                    ring.p[k] = positions[topology.from(tri.corner[k]).index()];
                    ok = ok && ring.wedge[k] != kInvalidId;
                }
                if (!ok) {
                    continue;
                }
                ring.area = detail::TriArea3d(ring.p[0], ring.p[1], ring.p[2]);
                if (ring.area <= 0.0f) {
                    ++result.degenerate;
                    continue;
                }
                const u32 index = static_cast<u32>(tris.size());
                tris.push_back(ring);
                for (u32 k = 0; k < 3; ++k) {
                    ringOf[ring.wedge[k]].push_back(index);
                }
            }
        }
    }
    if (tris.empty()) {
        result.refusal = FlattenResult::Refusal::NoFaces;
        return result;
    }

    // A step in UV units: the island's own size decides what "a little" means.
    f32 span = 0.0f;
    for (const u32 island : touched) {
        const detail::IslandArea areas = detail::AreasOf(mesh, islands, island, positions, uvs);
        span = std::max(span, std::sqrt(std::max(areas.uv, 0.0f)));
    }
    if (span <= 0.0f) {
        result.refusal = FlattenResult::Refusal::NoFaces;
        return result;
    }
    const f32 h = span * 1e-4f;

    const auto cornerUv = [&](const RingTri& tri, u32 k, u32 wedge, const Vector2f& moved) {
        return tri.wedge[k] == wedge ? moved : uvs[tri.corner[k]];
    };
    // The ring's area-weighted stretch with `wedge` moved to `at`, and whether
    // any of it turned over on the way.
    const auto ringEnergy = [&](u32 wedge, const Vector2f& at, bool& flipped) {
        f32 sum = 0.0f;
        flipped = false;
        for (const u32 index : ringOf[wedge]) {
            const RingTri& tri = tris[index];
            const Vector2f q[3] = {cornerUv(tri, 0, wedge, at), cornerUv(tri, 1, wedge, at),
                                   cornerUv(tri, 2, wedge, at)};
            f32 signedArea = 0.0f;
            const f32 stretch = triStretch(tri, q, signedArea);
            if (signedArea == 0.0f) {
                flipped = true;
                return sum;
            }
            sum += stretch * tri.area;
        }
        return sum;
    };

    // Which way round the island already is, so a step that turns a triangle
    // the other way is refused whichever way that is.
    std::vector<i8> sign(tris.size(), 0);
    for (std::size_t i = 0; i < tris.size(); ++i) {
        const RingTri& tri = tris[i];
        const Vector2f q[3] = {uvs[tri.corner[0]], uvs[tri.corner[1]], uvs[tri.corner[2]]};
        const f32 area = detail::TriAreaUv(q[0], q[1], q[2]);
        sign[i] = area > 0.0f ? 1 : (area < 0.0f ? -1 : 0);
    }
    const auto turnedOver = [&](u32 wedge, const Vector2f& at) {
        for (const u32 index : ringOf[wedge]) {
            const RingTri& tri = tris[index];
            const Vector2f q[3] = {cornerUv(tri, 0, wedge, at), cornerUv(tri, 1, wedge, at),
                                   cornerUv(tri, 2, wedge, at)};
            const f32 area = detail::TriAreaUv(q[0], q[1], q[2]);
            if (sign[index] != 0 && (area == 0.0f || (area > 0.0f ? 1 : -1) != sign[index])) {
                return true;
            }
        }
        return false;
    };

    for (u32 pass = 0; pass < options.passes; ++pass) {
        ++result.iterations;
        f32 moved = 0.0f;
        for (const u32 wedge : moving) {
            if (wedge >= islands.wedgeCount || held[wedge] != 0 || ringOf[wedge].empty()) {
                continue;
            }
            const std::span<const u32> corners = islands.cornersOf(wedge);
            if (corners.empty()) {
                continue;
            }
            const Vector2f at = uvs[corners[0]];
            bool flipped = false;
            const f32 energy = ringEnergy(wedge, at, flipped);
            if (flipped) {
                continue;
            }
            // Numeric, because the stretch of a ring is a page of algebra and
            // two extra evaluations are cheaper than getting it wrong.
            bool ignore = false;
            const f32 dx = (ringEnergy(wedge, Vector2f{at.x + h, at.y}, ignore) - energy) / h;
            const f32 dy = (ringEnergy(wedge, Vector2f{at.x, at.y + h}, ignore) - energy) / h;
            const f32 gradient = std::sqrt(dx * dx + dy * dy);
            if (gradient <= 0.0f) {
                continue;
            }
            f32 step = span * 0.02f / gradient;
            for (u32 back = 0; back < 8; ++back) {
                const Vector2f next{at.x - dx * step, at.y - dy * step};
                bool turned = false;
                const f32 trial = ringEnergy(wedge, next, turned);
                // A step that turns a triangle over is not a smaller step in
                // the right direction; it is the wrong answer, and is refused.
                if (!turned && trial < energy && !turnedOver(wedge, next)) {
                    for (const u32 corner : corners) {
                        uvs[corner] = next;
                    }
                    moved += energy - trial;
                    break;
                }
                step *= 0.5f;
            }
        }
        if (moved <= 0.0f) {
            break;
        }
    }

    for (std::size_t i = 0; i < tris.size(); ++i) {
        const RingTri& tri = tris[i];
        const Vector2f q[3] = {uvs[tri.corner[0]], uvs[tri.corner[1]], uvs[tri.corner[2]]};
        const f32 area = detail::TriAreaUv(q[0], q[1], q[2]);
        if (sign[i] != 0 && area != 0.0f && (area > 0.0f ? 1 : -1) != sign[i]) {
            ++result.flipped;
        }
    }
    return result;
}

FlattenResult Straighten(Mesh& mesh, const UvIslands& islands,
                         std::span<const HalfedgeId> uvEdges, u32 set) {
    FlattenResult result;
    if (!mesh.hasConnectivity() || uvEdges.empty()) {
        result.refusal = FlattenResult::Refusal::NoFaces;
        return result;
    }
    const Topology& topology = std::as_const(mesh).topology();
    const std::span<Vector2f> uvs =
        mesh.attributes.getOrCreate<Vector2f>(names::uv(set), Domain::Halfedge, AttrType::F32x2);

    // The runs, as a graph over wedges: a UV edge is a pair of wedges, and a
    // run is a path through them.
    std::vector<std::pair<u32, u32>> pairs;
    for (const HalfedgeId h : uvEdges) {
        if (!h.valid() || h.index() >= islands.wedgeOfCorner.size()) {
            continue;
        }
        const u32 a = islands.wedgeOf(h);
        const u32 b = islands.wedgeOf(topology.next(h));
        if (a != kInvalidId && b != kInvalidId && a != b) {
            pairs.emplace_back(std::min(a, b), std::max(a, b));
        }
    }
    std::sort(pairs.begin(), pairs.end());
    pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());
    if (pairs.empty()) {
        result.refusal = FlattenResult::Refusal::NoFaces;
        return result;
    }

    std::unordered_map<u32, std::vector<u32>> neighbours;
    for (const auto& pair : pairs) {
        neighbours[pair.first].push_back(pair.second);
        neighbours[pair.second].push_back(pair.first);
    }
    // In wedge order, so the runs come out the same on every run of the
    // program however the map happened to bucket them.
    std::vector<u32> starts;
    starts.reserve(neighbours.size());
    for (const auto& entry : neighbours) {
        starts.push_back(entry.first);
    }
    std::sort(starts.begin(), starts.end());

    std::unordered_map<u32, u8> visited;
    std::vector<u32> pinnedWedges;
    constexpr f32 kSnap = 0.17364817766f; // ten degrees
    for (u32 round = 0; round < 2; ++round) {
        for (const u32 start : starts) {
            // Ends first, so an open run is walked from its end; a closed one
            // is picked up on the second round, from its lowest wedge.
            const bool isEnd = neighbours[start].size() <= 1;
            if (visited[start] != 0 || (round == 0) != isEnd) {
                continue;
            }
            std::vector<u32> run;
            u32 current = start;
            u32 previous = kInvalidId;
            while (current != kInvalidId && visited[current] == 0) {
                visited[current] = 1;
                run.push_back(current);
                u32 next = kInvalidId;
                for (const u32 candidate : neighbours[current]) {
                    if (candidate != previous && visited[candidate] == 0) {
                        next = candidate;
                        break;
                    }
                }
                previous = current;
                current = next;
            }
            if (run.size() < 3) {
                for (const u32 wedge : run) {
                    pinnedWedges.push_back(wedge);
                }
                continue;
            }
            const Vector2f from = uvs[islands.cornersOf(run.front())[0]];
            const Vector2f to = uvs[islands.cornersOf(run.back())[0]];
            Vector2f along{to.x - from.x, to.y - from.y};
            const f32 length = std::sqrt(along.x * along.x + along.y * along.y);
            if (length <= 0.0f) {
                continue;
            }
            along.x /= length;
            along.y /= length;
            // Within ten degrees of an axis is an axis: a modeller straightening
            // a nearly vertical run means vertical.
            if (std::abs(along.y) < kSnap) {
                along = Vector2f{along.x > 0.0f ? 1.0f : -1.0f, 0.0f};
            } else if (std::abs(along.x) < kSnap) {
                along = Vector2f{0.0f, along.y > 0.0f ? 1.0f : -1.0f};
            }
            for (const u32 wedge : run) {
                const std::span<const u32> corners = islands.cornersOf(wedge);
                if (corners.empty()) {
                    continue;
                }
                const Vector2f at = uvs[corners[0]];
                const f32 t = (at.x - from.x) * along.x + (at.y - from.y) * along.y;
                const Vector2f onto{from.x + along.x * t, from.y + along.y * t};
                for (const u32 corner : corners) {
                    uvs[corner] = onto;
                }
                pinnedWedges.push_back(wedge);
            }
        }
    }
    if (pinnedWedges.empty()) {
        result.refusal = FlattenResult::Refusal::TooFewPins;
        return result;
    }

    // Every island the run touched is re-solved around what is now held.
    std::vector<u32> touched;
    for (const u32 wedge : pinnedWedges) {
        if (wedge < islands.wedgeIsland.size()) {
            touched.push_back(islands.wedgeIsland[wedge]);
        }
    }
    std::sort(touched.begin(), touched.end());
    touched.erase(std::unique(touched.begin(), touched.end()), touched.end());
    for (const u32 island : touched) {
        const FlattenResult solved =
            LscmPinning(mesh, islands, island, set,
                        std::span<const u32>(pinnedWedges.data(), pinnedWedges.size()));
        result.flipped += solved.flipped;
        result.degenerate += solved.degenerate;
        result.iterations += solved.iterations;
        result.residual = std::max(result.residual, solved.residual);
        if (!solved.ok()) {
            result.refusal = solved.refusal;
        }
    }
    return result;
}

RectangleResult Rectangle(Mesh& mesh, const UvIslands& islands, u32 island, u32 set) {
    RectangleResult out;
    if (!mesh.hasConnectivity() || island >= islands.count) {
        out.solve.refusal = FlattenResult::Refusal::NoFaces;
        return out;
    }
    if (islands.closed[island] != 0) {
        out.solve.refusal = FlattenResult::Refusal::Closed;
        return out;
    }
    if (islands.loops[island] != 1) {
        // More than one loop is an island with a hole in it, and a hole has no
        // place in a rectangle.
        out.solve.refusal = FlattenResult::Refusal::NoFaces;
        return out;
    }
    const Topology& topology = std::as_const(mesh).topology();
    const std::span<const Vector3f> positions =
        std::as_const(mesh).attributes.get<const Vector3f>(names::kPosition, Domain::Vertex);
    const std::span<Vector2f> uvs =
        mesh.attributes.getOrCreate<Vector2f>(names::uv(set), Domain::Halfedge, AttrType::F32x2);

    const std::span<const HalfedgeId> loop = islands.boundaryOf(island);
    const u32 count = static_cast<u32>(loop.size());
    if (count < 4) {
        out.solve.refusal = FlattenResult::Refusal::NoFaces;
        return out;
    }
    std::vector<u32> ring;       // the wedge at each step of the loop
    std::vector<Vector3f> place; // and where it is in the world
    ring.reserve(count);
    place.reserve(count);
    for (const HalfedgeId h : loop) {
        const u32 wedge = islands.wedgeOf(h);
        if (wedge == kInvalidId) {
            out.solve.refusal = FlattenResult::Refusal::NoFaces;
            return out;
        }
        ring.push_back(wedge);
        place.push_back(positions[topology.from(h).index()]);
    }

    // A corner is where the boundary turns by more than sixty degrees -- read
    // in the world, not in the map, because the map is what is about to change.
    constexpr f32 kTurn = 1.04719755f; // sixty degrees
    std::vector<u32> corners;
    for (u32 i = 0; i < count; ++i) {
        const Vector3f before = place[i] - place[(i + count - 1) % count];
        const Vector3f after = place[(i + 1) % count] - place[i];
        const f32 lengths = before.length() * after.length();
        if (lengths <= 0.0f) {
            continue;
        }
        const f32 turn = std::acos(std::clamp(before.dot(after) / lengths, -1.0f, 1.0f));
        if (turn > kTurn) {
            corners.push_back(i);
        }
    }
    out.corners = static_cast<u32>(corners.size());
    if (out.corners != 4) {
        out.solve.refusal = FlattenResult::Refusal::TooFewPins;
        return out;
    }

    // The four sides, by the arc length the boundary really has.
    f32 sides[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    std::vector<f32> along(count, 0.0f);
    for (u32 s = 0; s < 4; ++s) {
        const u32 from = corners[s];
        const u32 to = corners[(s + 1) % 4];
        f32 walked = 0.0f;
        for (u32 i = from; i != to; i = (i + 1) % count) {
            along[i] = walked;
            walked += (place[(i + 1) % count] - place[i]).length();
        }
        sides[s] = walked;
    }
    const f32 width = (sides[0] + sides[2]) * 0.5f;
    const f32 height = (sides[1] + sides[3]) * 0.5f;
    if (width <= 0.0f || height <= 0.0f) {
        out.solve.refusal = FlattenResult::Refusal::NoFaces;
        return out;
    }
    const Vector2f at[4] = {{0.0f, 0.0f}, {width, 0.0f}, {width, height}, {0.0f, height}};

    std::vector<u32> pinnedWedges;
    for (u32 s = 0; s < 4; ++s) {
        const u32 from = corners[s];
        const u32 to = corners[(s + 1) % 4];
        const Vector2f a = at[s];
        const Vector2f b = at[(s + 1) % 4];
        for (u32 i = from; i != to; i = (i + 1) % count) {
            const f32 t = sides[s] > 0.0f ? along[i] / sides[s] : 0.0f;
            const Vector2f value{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
            for (const u32 corner : islands.cornersOf(ring[i])) {
                uvs[corner] = value;
            }
            pinnedWedges.push_back(ring[i]);
        }
    }
    out.solve = LscmPinning(mesh, islands, island, set,
                            std::span<const u32>(pinnedWedges.data(), pinnedWedges.size()));
    return out;
}

FlattenResult Project(Mesh& mesh, std::span<const FaceId> faces, u32 set, ProjectShape shape,
                      const ProjectFrame& frame) {
    FlattenResult result;
    if (!mesh.hasConnectivity() || faces.empty()) {
        result.refusal = FlattenResult::Refusal::NoFaces;
        return result;
    }
    const Topology& topology = std::as_const(mesh).topology();
    const std::span<const Vector3f> positions =
        std::as_const(mesh).attributes.get<const Vector3f>(names::kPosition, Domain::Vertex);
    const std::span<Vector2f> uvs =
        mesh.attributes.getOrCreate<Vector2f>(names::uv(set), Domain::Halfedge, AttrType::F32x2);

    const auto project = [&](const Vector3f& world) {
        const Vector3f d = world - frame.origin;
        switch (shape) {
        case ProjectShape::Planar:
            return Vector2f{d.dot(frame.axisU), d.dot(frame.axisV)};
        case ProjectShape::Cylinder: {
            const f32 angle = std::atan2(d.dot(frame.axisV), d.dot(frame.axisU));
            return Vector2f{angle / (2.0f * kPi) + 0.5f, d.dot(frame.axisN)};
        }
        case ProjectShape::Sphere:
        default: {
            const f32 angle = std::atan2(d.dot(frame.axisV), d.dot(frame.axisU));
            const f32 length = d.length();
            const f32 up = length > 0.0f ? d.dot(frame.axisN) / length : 0.0f;
            return Vector2f{angle / (2.0f * kPi) + 0.5f,
                            std::acos(std::clamp(up, -1.0f, 1.0f)) / kPi};
        }
        }
    };

    std::vector<EdgeId> wrapped;
    for (const FaceId face : faces) {
        if (!face.valid() || topology.isDeleted(face)) {
            continue;
        }
        f32 lowest = 2.0f;
        f32 highest = -1.0f;
        for (const HalfedgeId h : topology.fh(face)) {
            const Vector2f value = project(positions[topology.from(h).index()]);
            uvs[h.index()] = value;
            lowest = std::min(lowest, value.x);
            highest = std::max(highest, value.x);
        }
        // A round projection wraps somewhere, and where it wraps is a cut: a
        // face that spans the turn would otherwise stretch the whole way back
        // across the tile.
        if (shape != ProjectShape::Planar && highest - lowest > 0.5f) {
            for (const HalfedgeId h : topology.fh(face)) {
                wrapped.push_back(Topology::edge(h));
            }
            ++result.degenerate;
        }
    }
    if (!wrapped.empty()) {
        std::sort(wrapped.begin(), wrapped.end(),
                  [](EdgeId a, EdgeId b) { return a.value() < b.value(); });
        wrapped.erase(std::unique(wrapped.begin(), wrapped.end(),
                                  [](EdgeId a, EdgeId b) { return a.value() == b.value(); }),
                      wrapped.end());
        ApplyMarks(mesh, set, std::span<const EdgeId>(wrapped.data(), wrapped.size()), true);
    }
    return result;
}

std::vector<f32> FaceStretch(const Mesh& mesh, const UvIslands& islands, u32 set) {
    std::vector<f32> out;
    if (!mesh.hasConnectivity()) {
        return out;
    }
    const Topology& topology = std::as_const(mesh).topology();
    out.assign(topology.faceCount(), 0.0f);
    const std::span<const Vector3f> positions =
        mesh.attributes.get<const Vector3f>(names::kPosition, Domain::Vertex);
    const std::span<const Vector2f> uvs =
        mesh.attributes.get<const Vector2f>(names::uv(set), Domain::Halfedge);
    if (uvs.empty()) {
        return out;
    }

    for (u32 island = 0; island < islands.count; ++island) {
        const detail::IslandArea areas = detail::AreasOf(mesh, islands, island, positions, uvs);
        if (areas.uv <= 0.0f || areas.world <= 0.0f) {
            continue;
        }
        // The island's own scale, so "stretched" means stretched against its
        // neighbours in the same island and not against the model's units.
        const f32 scale = std::sqrt(areas.world / areas.uv);
        for (const u32 face : islands.facesOf(island)) {
            f32 weighted = 0.0f;
            f32 total = 0.0f;
            for (const Tri& tri : detail::TrianglesOf(mesh, FaceId(face))) {
                const Vector3f p[3] = {positions[topology.from(tri.corner[0]).index()],
                                       positions[topology.from(tri.corner[1]).index()],
                                       positions[topology.from(tri.corner[2]).index()]};
                const Vector2f q[3] = {uvs[tri.corner[0].index()], uvs[tri.corner[1].index()],
                                       uvs[tri.corner[2].index()]};
                const f32 area = detail::TriAreaUv(q[0], q[1], q[2]);
                if (area == 0.0f) {
                    continue;
                }
                const f32 twice = 2.0f * area;
                // Sander's Ss and St: how far the surface moves per unit of u
                // and of v.
                const Vector3f ss = (p[0] * (q[1].y - q[2].y) + p[1] * (q[2].y - q[0].y) +
                                     p[2] * (q[0].y - q[1].y)) *
                                    (1.0f / twice);
                const Vector3f st = (p[0] * (q[2].x - q[1].x) + p[1] * (q[0].x - q[2].x) +
                                     p[2] * (q[1].x - q[0].x)) *
                                    (1.0f / twice);
                const f32 l2 = std::sqrt((ss.dot(ss) + st.dot(st)) * 0.5f);
                const f32 weight = detail::TriArea3d(p[0], p[1], p[2]);
                weighted += (l2 / scale) * weight;
                total += weight;
            }
            if (total > 0.0f) {
                out[face] = weighted / total;
            }
        }
    }
    return out;
}

} // namespace uv
} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
