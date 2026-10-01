// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/geometry/uv/flatten.h>

#include "common.h"

#include <whiteout/models/wem/geometry/uv/seams.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
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
/// `Aᵀ` are applied in, and neither ever wants a column. In doubles, which cost
/// little here: a system two pins hold is badly conditioned.
struct Csr {
    std::vector<u32> rowStart;
    std::vector<u32> column;
    std::vector<f64> value;
    std::vector<f64> rhs;
    u32 columns = 0;

    void beginRow() {
        rowStart.push_back(static_cast<u32>(column.size()));
    }
    void push(u32 col, f64 v) {
        column.push_back(col);
        value.push_back(v);
    }
    void endRow(f64 b) {
        rhs.push_back(b);
    }
    u32 rows() const {
        return static_cast<u32>(rhs.size());
    }
    void seal() {
        rowStart.push_back(static_cast<u32>(column.size()));
    }

    /// `out = A x`.
    void apply(const std::vector<f64>& x, std::vector<f64>& out) const {
        out.assign(rows(), 0.0);
        for (u32 r = 0; r < rows(); ++r) {
            f64 sum = 0.0;
            for (u32 i = rowStart[r]; i < rowStart[r + 1]; ++i) {
                sum += value[i] * x[column[i]];
            }
            out[r] = sum;
        }
    }

    /// `out = Aᵀ y`.
    void applyTransposed(const std::vector<f64>& y, std::vector<f64>& out) const {
        out.assign(columns, 0.0);
        for (u32 r = 0; r < rows(); ++r) {
            const f64 scale = y[r];
            if (scale == 0.0) {
                continue;
            }
            for (u32 i = rowStart[r]; i < rowStart[r + 1]; ++i) {
                out[column[i]] += value[i] * scale;
            }
        }
    }

    /// Each column's length: what the Jacobi preconditioner divides by, and
    /// zero for a column no row reaches.
    std::vector<f64> columnNorms() const {
        std::vector<f64> out(columns, 0.0);
        for (u32 i = 0; i < column.size(); ++i) {
            out[column[i]] += value[i] * value[i];
        }
        for (f64& n : out) {
            n = std::sqrt(n);
        }
        return out;
    }
};

f64 norm2(const std::vector<f64>& v) {
    f64 sum = 0.0;
    for (const f64 x : v) {
        sum += x * x;
    }
    return sum;
}

/// CGLS: conjugate gradients on the normal equations, applying `A` and `Aᵀ` and
/// never forming `AᵀA` (which would square the condition number and the
/// memory both), on `A` with its columns scaled to unit length -- the Jacobi
/// preconditioner, which LSCM's rows, weighted by each triangle's area, need.
/// A column no row reaches stays where it started.
///
/// It stops on the residual `LscmOptions::tolerance` names -- the scaled
/// normal residual `‖DAᵀr‖`, relative to where the solve started -- and
/// reports the same one, measured: the recursion's own r drifts, so where it
/// says done the true one is taken, and the solve restarts from there when
/// that is not. A restart that buys nothing is the doubles' floor (a start
/// already at the answer has no millionth left to shed); @p cappedOut is set
/// only when the iterations ran out short of the tolerance.
u32 SolveCgls(const Csr& a, std::vector<f64>& x, f64 tolerance, u32 maxIterations, f64& residualOut,
              bool& cappedOut) {
    std::vector<f64> scale = a.columnNorms();
    for (f64& d : scale) {
        d = d > 0.0 ? 1.0 / d : 0.0;
    }
    std::vector<f64> r;
    std::vector<f64> s;
    // Leaves s = DAᵀr, in the scaled unknowns y (x = x0 + D y).
    const auto trueResidual = [&]() {
        a.apply(x, r);
        for (u32 i = 0; i < a.rows(); ++i) {
            r[i] = a.rhs[i] - r[i];
        }
        a.applyTransposed(r, s);
        for (u32 j = 0; j < a.columns; ++j) {
            s[j] *= scale[j];
        }
        return norm2(s);
    };
    f64 now = trueResidual();
    const f64 start = now;
    cappedOut = false;
    if (start <= 0.0) {
        residualOut = 0.0;
        return 0;
    }
    const f64 goal = tolerance * tolerance * start;
    std::vector<f64> p;
    std::vector<f64> q;
    std::vector<f64> dp(a.columns, 0.0);
    u32 iteration = 0;
    while (now > goal && iteration < maxIterations) {
        p = s;
        f64 gamma = now;
        for (; iteration < maxIterations && gamma > 0.0; ++iteration) {
            for (u32 j = 0; j < a.columns; ++j) {
                dp[j] = scale[j] * p[j];
            }
            a.apply(dp, q);
            const f64 qq = norm2(q);
            if (qq <= 0.0) {
                break;
            }
            const f64 alpha = gamma / qq;
            for (u32 j = 0; j < a.columns; ++j) {
                x[j] += alpha * dp[j];
            }
            for (u32 i = 0; i < a.rows(); ++i) {
                r[i] -= alpha * q[i];
            }
            a.applyTransposed(r, s);
            for (u32 j = 0; j < a.columns; ++j) {
                s[j] *= scale[j];
            }
            const f64 next = norm2(s);
            if (next <= goal) {
                ++iteration;
                break;
            }
            const f64 beta = next / gamma;
            for (u32 j = 0; j < a.columns; ++j) {
                p[j] = s[j] + beta * p[j];
            }
            gamma = next;
        }
        const f64 before = now;
        now = trueResidual();
        // A restart that bought nothing will buy nothing next time either.
        if (!(now < before)) {
            break;
        }
    }
    residualOut = std::sqrt(now / start);
    cappedOut = now > goal && iteration >= maxIterations;
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

    // Set when the automatic pair holds a map the island already had: the pair
    // fixes the solve's gauge, and the fit below puts it back over that map.
    bool refit = false;
    f32 oldUvArea = 0.0f;
    // A pin of the user's own, when there is exactly one: the fit keeps it.
    u32 userPin = kInvalidId;
    if (pinCount == 1) {
        userPin = static_cast<u32>(std::find(pinned.begin(), pinned.end(), u8{1}) - pinned.begin());
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
        refit = byUv;
        oldUvArea = areas.uv;
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
            // The triangle's own plane, as a frame: the first edge is x, the
            // normal gives y. Conformality is a statement about this frame, so
            // the map never sees the model's axes at all. In doubles, as the
            // solve is.
            f64 local[3][2];
            if (!detail::LocalTriangle(positions[topology.from(tri.corner[0]).index()],
                                       positions[topology.from(tri.corner[1]).index()],
                                       positions[topology.from(tri.corner[2]).index()], local)) {
                ++result.degenerate;
                continue;
            }
            const f64 dT = local[1][0] * local[2][1] - local[1][1] * local[2][0];
            const f64 scale = 1.0 / std::sqrt(std::abs(dT));
            // W_k = (x_{k+2} - x_{k+1}) + i (y_{k+2} - y_{k+1}).
            f64 wr[3];
            f64 wi[3];
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
                f64 b = 0.0;
                for (u32 k = 0; k < 3; ++k) {
                    const f64 cu = part == 0 ? wr[k] : wi[k];
                    const f64 cv = part == 0 ? -wi[k] : wr[k];
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

    std::vector<f64> x(columns, 0.0);
    for (u32 i = 0; i < wedges.size(); ++i) {
        if (columnOf[i] != kInvalidId) {
            // From where it is: a re-unwrap of a map that is nearly right
            // converges in a handful of iterations instead of from nothing.
            x[columnOf[i]] = held[i].x;
            x[columnOf[i] + 1] = held[i].y;
        }
    }
    const u32 cap = std::max<u32>(32u, options.iterationsPerUnknown * columns);
    f64 residual = 0.0;
    result.iterations = SolveCgls(a, x, options.tolerance, cap, residual, result.capped);
    result.residual = static_cast<f32>(residual);

    std::vector<Vector2f> solved(wedges.size());
    for (u32 i = 0; i < wedges.size(); ++i) {
        solved[i] = pinned[i] != 0 ? held[i]
                                   : Vector2f{static_cast<f32>(x[columnOf[i]]), static_cast<f32>(x[columnOf[i] + 1])};
    }
    // A wedge only triangles with no area touch is in no row, and would keep
    // whatever it had -- a spike out of the new map. It goes to the mean of
    // the wedges it shares a face with instead.
    {
        const std::vector<f64> norms = a.columnNorms();
        std::vector<u8> floating(wedges.size(), 0);
        bool any = false;
        for (u32 i = 0; i < wedges.size(); ++i) {
            floating[i] = columnOf[i] != kInvalidId && norms[columnOf[i]] == 0.0 ? 1 : 0;
            any = any || floating[i] != 0;
        }
        if (any) {
            std::vector<Vector2f> sum(wedges.size(), Vector2f{0.0f, 0.0f});
            std::vector<u32> count(wedges.size(), 0);
            std::vector<u32> ring;
            for (const u32 face : faces) {
                ring.clear();
                for (const HalfedgeId h : topology.fh(FaceId(face))) {
                    const u32 w = islands.wedgeOf(h);
                    if (w != kInvalidId && localOf[w] != kInvalidId) {
                        ring.push_back(localOf[w]);
                    }
                }
                for (const u32 i : ring) {
                    if (floating[i] == 0) {
                        continue;
                    }
                    for (const u32 j : ring) {
                        if (floating[j] == 0) {
                            sum[i] = sum[i] + solved[j];
                            ++count[i];
                        }
                    }
                }
            }
            for (u32 i = 0; i < wedges.size(); ++i) {
                if (floating[i] != 0 && count[i] != 0) {
                    solved[i] = sum[i] * (1.0f / static_cast<f32>(count[i]));
                }
            }
        }
    }
    if (refit) {
        // Two held points fix the solve's turn along whatever line joined them
        // in the old map, and an old map squashed one way (a 4 x 2 sheet on a
        // square tile) comes back turned and grown along its diagonal. The map
        // goes back over the old one instead: the turn (or, for a mirrored
        // map, the reflection) that fits it best, its UV area, its centroid.
        Vector2f c0{0.0f, 0.0f};
        Vector2f c1{0.0f, 0.0f};
        for (u32 i = 0; i < wedges.size(); ++i) {
            c0 = c0 + solved[i];
            c1 = c1 + held[i];
        }
        c0 = c0 * (1.0f / static_cast<f32>(wedges.size()));
        c1 = c1 * (1.0f / static_cast<f32>(wedges.size()));
        // As complex numbers: the turn is arg Σ conj(p) q, the reflection's
        // arg Σ p q applied to conj(p); the larger modulus fits better.
        f64 turnRe = 0.0, turnIm = 0.0, flipRe = 0.0, flipIm = 0.0;
        for (u32 i = 0; i < wedges.size(); ++i) {
            const f64 px = solved[i].x - c0.x, py = solved[i].y - c0.y;
            const f64 qx = held[i].x - c1.x, qy = held[i].y - c1.y;
            turnRe += px * qx + py * qy;
            turnIm += px * qy - py * qx;
            flipRe += px * qx - py * qy;
            flipIm += px * qy + py * qx;
        }
        const bool mirrored = std::hypot(flipRe, flipIm) > std::hypot(turnRe, turnIm);
        const f64 angle = mirrored ? std::atan2(flipIm, flipRe) : std::atan2(turnIm, turnRe);
        const f32 cs = static_cast<f32>(std::cos(angle));
        const f32 sn = static_cast<f32>(std::sin(angle));
        for (u32 i = 0; i < wedges.size(); ++i) {
            const Vector2f d = solved[i] - c0;
            const f32 dy = mirrored ? -d.y : d.y;
            solved[i] = Vector2f{d.x * cs - dy * sn, d.x * sn + dy * cs};
        }
        for (u32 i = 0; i < wedges.size(); ++i) {
            writeWedge(islands, uvs, wedges[i], solved[i]);
        }
        const f32 newUvArea = detail::AreasOf(mesh, islands, island, positions, uvs).uv;
        const f32 scale = newUvArea > 0.0f ? std::sqrt(oldUvArea / newUvArea) : 1.0f;
        for (u32 i = 0; i < wedges.size(); ++i) {
            solved[i] = c1 + solved[i] * scale;
        }
        if (userPin < wedges.size()) {
            const Vector2f back = held[userPin] - solved[userPin];
            for (u32 i = 0; i < wedges.size(); ++i) {
                solved[i] = solved[i] + back;
            }
        }
    }
    // --- folds ---------------------------------------------------------------
    //
    // LSCM keeps angles, not orientation: where a cut bends hard it can fold a
    // sliver over. Each free wedge of a folded triangle moves toward the mean
    // of the wedges it shares a triangle with, a step kept only when it leaves
    // fewer folds round it and folds nothing that was not; a few passes, no
    // second solve.
    {
        std::vector<std::array<u32, 3>> tris;
        for (const u32 face : faces) {
            for (const Tri& tri : detail::TrianglesOf(mesh, FaceId(face))) {
                std::array<u32, 3> t{};
                bool ok = true;
                for (u32 k = 0; k < 3 && ok; ++k) {
                    const u32 wedge = islands.wedgeOf(tri.corner[k]);
                    ok = wedge != kInvalidId && localOf[wedge] != kInvalidId;
                    if (ok) {
                        t[k] = localOf[wedge];
                    }
                }
                if (ok && t[0] != t[1] && t[1] != t[2] && t[0] != t[2]) {
                    tris.push_back(t);
                }
            }
        }
        const auto areaOf = [&](u32 t) {
            return detail::TriAreaUv(solved[tris[t][0]], solved[tris[t][1]], solved[tris[t][2]]);
        };
        i64 balance = 0;
        for (u32 t = 0; t < tris.size(); ++t) {
            const f32 area = areaOf(t);
            balance += area > 0.0f ? 1 : (area < 0.0f ? -1 : 0);
        }
        const f32 sign = balance >= 0 ? 1.0f : -1.0f;
        const auto folded = [&](u32 t) { return areaOf(t) * sign < 0.0f; };
        std::vector<std::vector<u32>> trisOf(wedges.size());
        for (u32 t = 0; t < tris.size(); ++t) {
            for (const u32 w : tris[t]) {
                trisOf[w].push_back(t);
            }
        }
        std::vector<u8> was;
        bool moved = true;
        for (u32 pass = 0; pass < 16 && moved; ++pass) {
            moved = false;
            for (u32 t = 0; t < tris.size(); ++t) {
                if (!folded(t)) {
                    continue;
                }
                for (const u32 w : tris[t]) {
                    if (pinned[w] != 0) {
                        continue;
                    }
                    was.clear();
                    u32 before = 0;
                    for (const u32 u : trisOf[w]) {
                        was.push_back(folded(u) ? 1 : 0);
                        before += was.back();
                    }
                    Vector2f mean{0.0f, 0.0f};
                    u32 count = 0;
                    for (const u32 u : trisOf[w]) {
                        for (const u32 v : tris[u]) {
                            if (v != w) {
                                mean = mean + solved[v];
                                ++count;
                            }
                        }
                    }
                    if (before == 0 || count == 0) {
                        continue;
                    }
                    mean = mean * (1.0f / static_cast<f32>(count));
                    const Vector2f start = solved[w];
                    for (const f32 step : {1.0f, 0.5f, 0.25f}) {
                        solved[w] = start + (mean - start) * step;
                        u32 after = 0;
                        bool made = false;
                        for (u32 k = 0; k < trisOf[w].size(); ++k) {
                            const bool now = folded(trisOf[w][k]);
                            after += now ? 1 : 0;
                            made = made || (now && was[k] == 0);
                        }
                        if (after < before && !made) {
                            moved = true;
                            break;
                        }
                        solved[w] = start;
                    }
                }
            }
        }
    }
    for (u32 i = 0; i < wedges.size(); ++i) {
        writeWedge(islands, uvs, wedges[i], solved[i]);
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

/// One triangle as Minimum stretch reads it: its three wedges (the island's
/// local numbering), its rest shape laid flat, and -- once the rest is scaled
/// -- the gradient of each corner's hat function and the rest area.
struct StretchTri {
    u32 w[3];
    f64 x[3][2];
    f64 g[3][2];
    f64 area;
};

/// Conjugate gradients on a symmetric positive definite operator, in doubles,
/// warm-started from @p x, Jacobi-preconditioned by @p diagonal when given.
template <class Apply>
void SolveSpd(const Apply& apply, const std::vector<f64>& b, std::vector<f64>& x, f64 tolerance, u32 cap,
              const std::vector<f64>& diagonal = {}) {
    const std::size_t n = b.size();
    std::vector<f64> r(n);
    std::vector<f64> z(n);
    std::vector<f64> ap(n);
    apply(x, ap);
    for (std::size_t i = 0; i < n; ++i) {
        r[i] = b[i] - ap[i];
    }
    const auto precondition = [&]() {
        f64 rz = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            z[i] = i < diagonal.size() && diagonal[i] > 0.0 ? r[i] / diagonal[i] : r[i];
            rz += r[i] * z[i];
        }
        return rz;
    };
    f64 rz = precondition();
    std::vector<f64> p = z;
    const f64 bb = std::max(norm2(b), 1e-300);
    for (u32 k = 0; k < cap && norm2(r) > tolerance * tolerance * bb; ++k) {
        apply(p, ap);
        f64 pap = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            pap += p[i] * ap[i];
        }
        if (!(pap > 0.0)) {
            break;
        }
        const f64 alpha = rz / pap;
        for (std::size_t i = 0; i < n; ++i) {
            x[i] += alpha * p[i];
            r[i] -= alpha * ap[i];
        }
        const f64 next = precondition();
        const f64 beta = next / rz;
        for (std::size_t i = 0; i < n; ++i) {
            p[i] = z[i] + beta * p[i];
        }
        rz = next;
    }
}

/// A 2x2 map's signed singular values and turns: `j = Rot(phi) diag(s) Rot(theta)`,
/// `s[1]` negative when the map mirrors.
struct Svd2 {
    f64 s[2];
    f64 phi;
    f64 theta;
};

Svd2 SignedSvd(const f64 (&j)[2][2]) {
    const f64 e = 0.5 * (j[0][0] + j[1][1]);
    const f64 f = 0.5 * (j[0][0] - j[1][1]);
    const f64 g = 0.5 * (j[1][0] + j[0][1]);
    const f64 h = 0.5 * (j[1][0] - j[0][1]);
    const f64 big = std::sqrt(e * e + h * h) + std::sqrt(f * f + g * g);
    const f64 a1 = std::atan2(g, f);
    const f64 a2 = std::atan2(h, e);
    // The small one through the determinant: q - r cancels as it nears zero.
    const f64 det = j[0][0] * j[1][1] - j[0][1] * j[1][0];
    return {{big, big > 0.0 ? det / big : 0.0}, 0.5 * (a2 + a1), 0.5 * (a2 - a1)};
}

f64 SignedArea2(const f64 (&a)[2], const f64 (&b)[2], const f64 (&c)[2]) {
    return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
}

} // namespace

FlattenResult MinimumStretch(Mesh& mesh, const UvIslands& islands, u32 island, u32 set,
                             std::span<const u32> alsoPinned, const LscmOptions& options) {
    FlattenResult result = LscmPinning(mesh, islands, island, set, alsoPinned, options);
    if (!result.ok()) {
        return result;
    }
    const Topology& topology = std::as_const(mesh).topology();
    const std::span<const Vector3f> positions =
        std::as_const(mesh).attributes.get<const Vector3f>(names::kPosition, Domain::Vertex);
    const std::span<Vector2f> uvs =
        mesh.attributes.getOrCreate<Vector2f>(names::uv(set), Domain::Halfedge, AttrType::F32x2);
    const std::span<const u8> pins =
        std::as_const(mesh).attributes.get<const u8>(names::uvPin(set), Domain::Halfedge);
    const std::vector<u32> wedges = IslandWedges(islands, mesh, island);
    std::vector<u32> localOf(islands.wedgeCount, kInvalidId);
    for (u32 i = 0; i < wedges.size(); ++i) {
        localOf[wedges[i]] = i;
    }
    // Held: the layer's pins and the caller's, which hold here as in LSCM.
    std::vector<u8> fixed(wedges.size(), 0);
    bool userPins = false;
    for (u32 i = 0; i < wedges.size(); ++i) {
        for (const u32 corner : islands.cornersOf(wedges[i])) {
            if (corner < pins.size() && pins[corner] != 0) {
                fixed[i] = 1;
            }
        }
    }
    for (const u32 wedge : alsoPinned) {
        if (wedge < localOf.size() && localOf[wedge] != kInvalidId) {
            fixed[localOf[wedge]] = 1;
        }
    }
    for (const u8 f : fixed) {
        userPins = userPins || f != 0;
    }
    const std::vector<u8> held = fixed;

    // --- the triangles and their rest shapes ----------------------------------
    std::vector<StretchTri> tris;
    f64 worldArea = 0.0;
    f64 uvArea = 0.0;
    std::vector<std::array<f64, 2>> u(wedges.size());
    for (u32 i = 0; i < wedges.size(); ++i) {
        const Vector2f p = readWedge(islands, uvs, wedges[i]);
        u[i] = {p.x, p.y};
    }
    for (const u32 face : islands.facesOf(island)) {
        for (const Tri& tri : detail::TrianglesOf(mesh, FaceId(face))) {
            StretchTri t{};
            bool ok = true;
            for (u32 k = 0; k < 3 && ok; ++k) {
                const u32 wedge = islands.wedgeOf(tri.corner[k]);
                ok = wedge != kInvalidId && localOf[wedge] != kInvalidId;
                if (ok) {
                    t.w[k] = localOf[wedge];
                }
            }
            if (!ok || t.w[0] == t.w[1] || t.w[1] == t.w[2] || t.w[0] == t.w[2] ||
                !detail::LocalTriangle(positions[topology.from(tri.corner[0]).index()],
                                       positions[topology.from(tri.corner[1]).index()],
                                       positions[topology.from(tri.corner[2]).index()], t.x)) {
                continue;
            }
            worldArea += 0.5 * t.x[1][0] * t.x[2][1];
            const f64 ua[2] = {u[t.w[0]][0], u[t.w[0]][1]};
            const f64 ub[2] = {u[t.w[1]][0], u[t.w[1]][1]};
            const f64 uc[2] = {u[t.w[2]][0], u[t.w[2]][1]};
            uvArea += 0.5 * std::abs(SignedArea2(ua, ub, uc));
            tris.push_back(t);
        }
    }
    if (tris.empty() || !(worldArea > 0.0) || !(uvArea > 0.0)) {
        return result;
    }
    const std::vector<std::array<f64, 2>> lscm = u;
    // Folds: LSCM's own minimum can turn a region over, and the rounds below
    // need a start that folds nothing. An island that folds starts instead
    // from Tutte's embedding -- its outer border on a circle, each other wedge
    // (a hole's too) at its neighbours' mean -- which folds nothing.
    if (!userPins && island < islands.loops.size() && islands.loops[island] >= 1) {
        i64 votes = 0;
        std::vector<f64> areas;
        for (const StretchTri& t : tris) {
            const f64 a[2] = {u[t.w[0]][0], u[t.w[0]][1]};
            const f64 b[2] = {u[t.w[1]][0], u[t.w[1]][1]};
            const f64 c[2] = {u[t.w[2]][0], u[t.w[2]][1]};
            areas.push_back(SignedArea2(a, b, c));
            votes += areas.back() > 0.0 ? 1 : (areas.back() < 0.0 ? -1 : 0);
        }
        const bool folds = std::any_of(areas.begin(), areas.end(),
                                       [&](f64 area) { return area * (votes >= 0 ? 1.0 : -1.0) <= 0.0; });
        // The loops lie end to end, each entry starting at the wedge the one
        // before reached; the longest is the outer border.
        const std::span<const HalfedgeId> loopsOf =
            folds ? islands.boundaryOf(island) : std::span<const HalfedgeId>{};
        const auto lengthOf = [&](HalfedgeId h) {
            return static_cast<f64>((positions[topology.to(h).index()] - positions[topology.from(h).index()]).length());
        };
        std::size_t outerBegin = 0;
        std::size_t outerEnd = 0;
        f64 outerLength = -1.0;
        for (std::size_t begin = 0; begin < loopsOf.size();) {
            std::size_t end = begin + 1;
            f64 length = lengthOf(loopsOf[begin]);
            while (end < loopsOf.size() &&
                   islands.wedgeOf(loopsOf[end]) == islands.wedgeOf(topology.next(loopsOf[end - 1]))) {
                length += lengthOf(loopsOf[end]);
                ++end;
            }
            if (length > outerLength) {
                outerBegin = begin;
                outerEnd = end;
                outerLength = length;
            }
            begin = end;
        }
        const std::span<const HalfedgeId> border = loopsOf.subspan(outerBegin, outerEnd - outerBegin);
        std::vector<u32> ring;
        std::vector<f64> along;
        std::vector<u8> onBorder(wedges.size(), 0);
        f64 total = 0.0;
        bool simple = folds && border.size() >= 3;
        for (const HalfedgeId h : border) {
            if (!simple) {
                break;
            }
            const u32 wedge = islands.wedgeOf(h);
            const u32 at = wedge != kInvalidId ? localOf[wedge] : kInvalidId;
            // A border that passes a wedge twice is no circle's.
            simple = at != kInvalidId && onBorder[at] == 0;
            if (simple) {
                onBorder[at] = 1;
                ring.push_back(at);
                along.push_back(total);
                total += lengthOf(h);
            }
        }
        if (simple && total > 0.0) {
            constexpr f64 kTurn = 6.283185307179586;
            f64 centre[2] = {0.0, 0.0};
            for (const auto& p : lscm) {
                centre[0] += p[0];
                centre[1] += p[1];
            }
            centre[0] /= static_cast<f64>(lscm.size());
            centre[1] /= static_cast<f64>(lscm.size());
            const f64 radius = std::sqrt(uvArea / (0.5 * kTurn));
            std::vector<std::array<f64, 2>> tutte(wedges.size(), std::array<f64, 2>{centre[0], centre[1]});
            for (std::size_t k = 0; k < ring.size(); ++k) {
                const f64 angle = kTurn * along[k] / total;
                tutte[ring[k]] = {centre[0] + radius * std::cos(angle), centre[1] + radius * std::sin(angle)};
            }
            std::vector<std::vector<u32>> adjacent(wedges.size());
            for (const StretchTri& t : tris) {
                for (u32 k = 0; k < 3; ++k) {
                    adjacent[t.w[k]].push_back(t.w[(k + 1) % 3]);
                    adjacent[t.w[(k + 1) % 3]].push_back(t.w[k]);
                }
            }
            std::vector<u32> column(wedges.size(), kInvalidId);
            std::vector<u32> inner;
            for (u32 i = 0; i < wedges.size(); ++i) {
                std::sort(adjacent[i].begin(), adjacent[i].end());
                adjacent[i].erase(std::unique(adjacent[i].begin(), adjacent[i].end()), adjacent[i].end());
                if (onBorder[i] == 0 && !adjacent[i].empty()) {
                    column[i] = static_cast<u32>(inner.size());
                    inner.push_back(i);
                }
            }
            const auto laplace = [&](const std::vector<f64>& y, std::vector<f64>& out) {
                out.assign(y.size(), 0.0);
                for (u32 c = 0; c < inner.size(); ++c) {
                    const u32 i = inner[c];
                    f64 sum = static_cast<f64>(adjacent[i].size()) * y[c];
                    for (const u32 j : adjacent[i]) {
                        if (column[j] != kInvalidId) {
                            sum -= y[column[j]];
                        }
                    }
                    out[c] = sum;
                }
            };
            for (u32 axis = 0; axis < 2 && !inner.empty(); ++axis) {
                std::vector<f64> b(inner.size(), 0.0);
                std::vector<f64> x(inner.size(), centre[axis]);
                for (u32 c = 0; c < inner.size(); ++c) {
                    for (const u32 j : adjacent[inner[c]]) {
                        if (column[j] == kInvalidId) {
                            b[c] += tutte[j][axis];
                        }
                    }
                }
                SolveSpd(laplace, b, x, 1e-12, std::max<u32>(128u, 8u * static_cast<u32>(inner.size())));
                for (u32 c = 0; c < inner.size(); ++c) {
                    tutte[inner[c]][axis] = x[c];
                }
            }
            // The circle runs one way round; LSCM's map may be the mirror of
            // it (refitted to a mirrored map), which the landing's turn cannot
            // undo, so the start is mirrored to match.
            i64 turn = 0;
            for (const StretchTri& t : tris) {
                const f64 a[2] = {tutte[t.w[0]][0], tutte[t.w[0]][1]};
                const f64 b[2] = {tutte[t.w[1]][0], tutte[t.w[1]][1]};
                const f64 c[2] = {tutte[t.w[2]][0], tutte[t.w[2]][1]};
                const f64 area = SignedArea2(a, b, c);
                turn += area > 0.0 ? 1 : (area < 0.0 ? -1 : 0);
            }
            if ((turn >= 0) != (votes >= 0)) {
                for (auto& p : tutte) {
                    p[0] = 2.0 * centre[0] - p[0];
                }
            }
            u = std::move(tutte);
        }
    }
    // Which way round the map is: LSCM may lay an island mirrored whole, which
    // is no fault, and a rotation cannot mirror -- so the rest shapes are
    // mirrored to match, or the rounds would read the whole map as folded.
    i64 balance = 0;
    for (const StretchTri& t : tris) {
        const f64 a[2] = {u[t.w[0]][0], u[t.w[0]][1]};
        const f64 b[2] = {u[t.w[1]][0], u[t.w[1]][1]};
        const f64 c[2] = {u[t.w[2]][0], u[t.w[2]][1]};
        const f64 area = SignedArea2(a, b, c);
        balance += area > 0.0 ? 1 : (area < 0.0 ? -1 : 0);
    }
    const f64 sign = balance >= 0 ? 1.0 : -1.0;
    // Rest shapes at the map's own scale, so the pins and the landing LSCM
    // made still fit, and each corner's gradient over its rest triangle.
    const f64 scale = std::sqrt(uvArea / worldArea);
    for (StretchTri& t : tris) {
        for (auto& corner : t.x) {
            corner[0] *= scale;
            corner[1] *= scale * sign;
        }
        const f64 twice = SignedArea2(t.x[0], t.x[1], t.x[2]);
        for (u32 k = 0; k < 3; ++k) {
            const f64* a = t.x[(k + 1) % 3];
            const f64* b = t.x[(k + 2) % 3];
            t.g[k][0] = (a[1] - b[1]) / twice;
            t.g[k][1] = (b[0] - a[0]) / twice;
        }
        t.area = 0.5 * std::abs(twice);
    }
    const auto jacobianOf = [&](const StretchTri& t, const std::vector<std::array<f64, 2>>& at, f64 (&j)[2][2]) {
        j[0][0] = j[0][1] = j[1][0] = j[1][1] = 0.0;
        for (u32 k = 0; k < 3; ++k) {
            const std::array<f64, 2>& p = at[t.w[k]];
            j[0][0] += p[0] * t.g[k][0];
            j[0][1] += p[0] * t.g[k][1];
            j[1][0] += p[1] * t.g[k][0];
            j[1][1] += p[1] * t.g[k][1];
        }
    };
    // Symmetric Dirichlet: s^2 + 1/s^2 per singular value, least at no
    // stretch and endless as a triangle loses its area, so no step that the
    // energy accepts can crush one.
    const auto energyOf = [&](const std::vector<std::array<f64, 2>>& at) {
        f64 total = 0.0;
        for (const StretchTri& t : tris) {
            f64 j[2][2];
            jacobianOf(t, at, j);
            const Svd2 d = SignedSvd(j);
            if (!(d.s[1] > 0.0)) {
                return std::numeric_limits<f64>::infinity();
            }
            total += t.area * (d.s[0] * d.s[0] + 1.0 / (d.s[0] * d.s[0]) + d.s[1] * d.s[1] + 1.0 / (d.s[1] * d.s[1]));
        }
        return total;
    };
    f64 energy = energyOf(u);
    // A start that still folds -- pins that hold a fold, a border Tutte could
    // not use -- keeps LSCM's map, as Conformal would.
    if (!std::isfinite(energy)) {
        return result;
    }
    // A wedge no triangle reaches stays; with nothing held at all, one wedge
    // holds the map in place, the rest of the gauge being the rotations'.
    std::vector<u8> touched(wedges.size(), 0);
    for (const StretchTri& t : tris) {
        touched[t.w[0]] = touched[t.w[1]] = touched[t.w[2]] = 1;
    }
    for (u32 i = 0; i < wedges.size(); ++i) {
        fixed[i] = fixed[i] != 0 || touched[i] == 0 ? 1 : 0;
    }
    if (!userPins) {
        for (u32 i = 0; i < wedges.size(); ++i) {
            if (touched[i] != 0) {
                fixed[i] = 1;
                break;
            }
        }
    }
    std::vector<u32> columnOf(wedges.size(), kInvalidId);
    std::vector<u32> wedgeOf;
    for (u32 i = 0; i < wedges.size(); ++i) {
        if (fixed[i] == 0) {
            columnOf[i] = static_cast<u32>(wedgeOf.size());
            wedgeOf.push_back(i);
        }
    }
    if (wedgeOf.empty()) {
        return result;
    }
    const std::size_t unknowns = wedgeOf.size();

    // --- rounds (SLIM: Rabinovich et al. 2017) ------------------------------
    // Each round weighs every triangle by how its energy bends at its current
    // stretch and solves for the positions nearest each one's rotation under
    // those weights -- one system, u and v coupled. The step then goes no
    // farther than the first triangle would lose its area, and back until the
    // energy falls.
    std::vector<std::array<f64, 5>> weigh(tris.size()); // m00, m01, m11, cos, sin
    const auto apply = [&](const std::vector<f64>& y, std::vector<f64>& out) {
        out.assign(y.size(), 0.0);
        for (std::size_t ti = 0; ti < tris.size(); ++ti) {
            const StretchTri& t = tris[ti];
            const std::array<f64, 5>& m = weigh[ti];
            f64 a[2] = {0.0, 0.0};
            f64 b[2] = {0.0, 0.0};
            for (u32 k = 0; k < 3; ++k) {
                const u32 c = columnOf[t.w[k]];
                if (c != kInvalidId) {
                    a[0] += y[c] * t.g[k][0];
                    a[1] += y[c] * t.g[k][1];
                    b[0] += y[unknowns + c] * t.g[k][0];
                    b[1] += y[unknowns + c] * t.g[k][1];
                }
            }
            const f64 pa[2] = {m[0] * a[0] + m[1] * b[0], m[0] * a[1] + m[1] * b[1]};
            const f64 pb[2] = {m[1] * a[0] + m[2] * b[0], m[1] * a[1] + m[2] * b[1]};
            for (u32 k = 0; k < 3; ++k) {
                const u32 c = columnOf[t.w[k]];
                if (c != kInvalidId) {
                    out[c] += t.area * (pa[0] * t.g[k][0] + pa[1] * t.g[k][1]);
                    out[unknowns + c] += t.area * (pb[0] * t.g[k][0] + pb[1] * t.g[k][1]);
                }
            }
        }
    };
    constexpr u32 kRounds = 25;
    std::vector<f64> solved(unknowns * 2);
    std::vector<f64> rhs(unknowns * 2);
    std::vector<f64> diagonal(unknowns * 2);
    std::vector<std::array<f64, 2>> trial(u.size());
    for (u32 round = 0; round < kRounds; ++round) {
        // Local: each triangle's weights and nearest rotation.
        std::fill(rhs.begin(), rhs.end(), 0.0);
        std::fill(diagonal.begin(), diagonal.end(), 0.0);
        for (std::size_t ti = 0; ti < tris.size(); ++ti) {
            const StretchTri& t = tris[ti];
            f64 j[2][2];
            jacobianOf(t, u, j);
            const Svd2 d = SignedSvd(j);
            f64 w[2];
            for (u32 k = 0; k < 2; ++k) {
                const f64 sv = d.s[k];
                w[k] = (sv + 1.0) * (sv * sv + 1.0) / (sv * sv * sv);
            }
            const f64 c = std::cos(d.phi);
            const f64 sn = std::sin(d.phi);
            std::array<f64, 5>& m = weigh[ti];
            m = {c * c * w[0] + sn * sn * w[1], c * sn * (w[0] - w[1]), sn * sn * w[0] + c * c * w[1],
                 std::cos(d.phi + d.theta), std::sin(d.phi + d.theta)};
            // The target's rows less what the held corners already give.
            f64 r0[2] = {m[3], -m[4]};
            f64 r1[2] = {m[4], m[3]};
            for (u32 k = 0; k < 3; ++k) {
                if (columnOf[t.w[k]] == kInvalidId) {
                    const std::array<f64, 2>& p = u[t.w[k]];
                    r0[0] -= p[0] * t.g[k][0];
                    r0[1] -= p[0] * t.g[k][1];
                    r1[0] -= p[1] * t.g[k][0];
                    r1[1] -= p[1] * t.g[k][1];
                }
            }
            const f64 pa[2] = {m[0] * r0[0] + m[1] * r1[0], m[0] * r0[1] + m[1] * r1[1]};
            const f64 pb[2] = {m[1] * r0[0] + m[2] * r1[0], m[1] * r0[1] + m[2] * r1[1]};
            for (u32 k = 0; k < 3; ++k) {
                const u32 col = columnOf[t.w[k]];
                if (col != kInvalidId) {
                    const f64 gg = t.g[k][0] * t.g[k][0] + t.g[k][1] * t.g[k][1];
                    rhs[col] += t.area * (pa[0] * t.g[k][0] + pa[1] * t.g[k][1]);
                    rhs[unknowns + col] += t.area * (pb[0] * t.g[k][0] + pb[1] * t.g[k][1]);
                    diagonal[col] += t.area * m[0] * gg;
                    diagonal[unknowns + col] += t.area * m[2] * gg;
                }
            }
        }
        // Global: warm-started from where the map is.
        for (std::size_t c = 0; c < unknowns; ++c) {
            solved[c] = u[wedgeOf[c]][0];
            solved[unknowns + c] = u[wedgeOf[c]][1];
        }
        SolveSpd(apply, rhs, solved, 1e-10, std::clamp<u32>(4u * static_cast<u32>(unknowns), 64u, 1000u), diagonal);
        std::vector<std::array<f64, 2>> next = u;
        for (std::size_t c = 0; c < unknowns; ++c) {
            next[wedgeOf[c]] = {solved[c], solved[unknowns + c]};
        }
        // Short of the first step at which a triangle would reach no area
        // (area(s) = a s^2 + b s + c, each one positive now).
        f64 step = 1.0;
        for (const StretchTri& t : tris) {
            const f64 e1[2] = {u[t.w[1]][0] - u[t.w[0]][0], u[t.w[1]][1] - u[t.w[0]][1]};
            const f64 e2[2] = {u[t.w[2]][0] - u[t.w[0]][0], u[t.w[2]][1] - u[t.w[0]][1]};
            const f64 d0[2] = {next[t.w[0]][0] - u[t.w[0]][0], next[t.w[0]][1] - u[t.w[0]][1]};
            const f64 d1[2] = {next[t.w[1]][0] - u[t.w[1]][0] - d0[0], next[t.w[1]][1] - u[t.w[1]][1] - d0[1]};
            const f64 d2[2] = {next[t.w[2]][0] - u[t.w[2]][0] - d0[0], next[t.w[2]][1] - u[t.w[2]][1] - d0[1]};
            const f64 c = (e1[0] * e2[1] - e1[1] * e2[0]) * sign;
            const f64 b = (e1[0] * d2[1] - e1[1] * d2[0] + d1[0] * e2[1] - d1[1] * e2[0]) * sign;
            const f64 a = (d1[0] * d2[1] - d1[1] * d2[0]) * sign;
            f64 root = std::numeric_limits<f64>::infinity();
            if (std::abs(a) < 1e-300) {
                if (b < 0.0) {
                    root = -c / b;
                }
            } else if (const f64 disc = b * b - 4.0 * a * c; disc >= 0.0) {
                // q and c / q, not -b +- sqrt: no cancellation when a is small.
                const f64 q = -0.5 * (b + std::copysign(std::sqrt(disc), b));
                for (const f64 r : {q / a, q != 0.0 ? c / q : std::numeric_limits<f64>::infinity()}) {
                    if (r > 0.0) {
                        root = std::min(root, r);
                    }
                }
            }
            step = std::min(step, 0.8 * root);
        }
        f64 after = energy;
        for (; step > 1e-8; step *= 0.5) {
            for (std::size_t i = 0; i < u.size(); ++i) {
                trial[i] = {u[i][0] + step * (next[i][0] - u[i][0]), u[i][1] + step * (next[i][1] - u[i][1])};
            }
            after = energyOf(trial);
            if (after < energy) {
                break;
            }
        }
        if (!(after < energy)) {
            break;
        }
        u.swap(trial);
        ++result.rounds;
        // Done once a round buys under a ten-thousandth of the energy.
        const bool settled = energy - after < 1e-4 * energy;
        energy = after;
        if (settled) {
            break;
        }
    }

    // Where LSCM landed it: with nothing held, the turn and place that best
    // lays the result over LSCM's map, which the caller's landing already
    // chose; with pins, they placed it.
    if (!userPins) {
        f64 ca[2] = {0.0, 0.0};
        f64 cl[2] = {0.0, 0.0};
        f64 reached = 0.0;
        for (std::size_t i = 0; i < u.size(); ++i) {
            if (touched[i] == 0) {
                continue;
            }
            ca[0] += u[i][0];
            ca[1] += u[i][1];
            cl[0] += lscm[i][0];
            cl[1] += lscm[i][1];
            reached += 1.0;
        }
        ca[0] /= reached;
        ca[1] /= reached;
        cl[0] /= reached;
        cl[1] /= reached;
        f64 dotSum = 0.0;
        f64 crossSum = 0.0;
        for (std::size_t i = 0; i < u.size(); ++i) {
            if (touched[i] == 0) {
                continue;
            }
            const f64 px = u[i][0] - ca[0], py = u[i][1] - ca[1];
            const f64 qx = lscm[i][0] - cl[0], qy = lscm[i][1] - cl[1];
            dotSum += px * qx + py * qy;
            crossSum += px * qy - py * qx;
        }
        const f64 theta = std::atan2(crossSum, dotSum);
        const f64 c = std::cos(theta);
        const f64 s = std::sin(theta);
        for (auto& p : u) {
            const f64 x = p[0] - ca[0];
            const f64 y = p[1] - ca[1];
            p = {cl[0] + c * x - s * y, cl[1] + s * x + c * y};
        }
    }
    // A wedge only triangles with no area touch followed none of the rounds
    // (a Tutte start left it at the centre): unless pinned, it goes to the
    // mean of the wedges it shares a face with, as LSCM's own pass puts it.
    if (std::find(touched.begin(), touched.end(), u8{0}) != touched.end()) {
        std::vector<std::array<f64, 2>> sum(wedges.size(), std::array<f64, 2>{0.0, 0.0});
        std::vector<u32> count(wedges.size(), 0);
        std::vector<u32> ring;
        for (const u32 face : islands.facesOf(island)) {
            ring.clear();
            for (const HalfedgeId h : topology.fh(FaceId(face))) {
                const u32 w = islands.wedgeOf(h);
                if (w != kInvalidId && localOf[w] != kInvalidId) {
                    ring.push_back(localOf[w]);
                }
            }
            for (const u32 i : ring) {
                if (touched[i] != 0 || held[i] != 0) {
                    continue;
                }
                for (const u32 j : ring) {
                    if (touched[j] != 0) {
                        sum[i][0] += u[j][0];
                        sum[i][1] += u[j][1];
                        ++count[i];
                    }
                }
            }
        }
        for (u32 i = 0; i < wedges.size(); ++i) {
            if (count[i] != 0) {
                u[i] = {sum[i][0] / count[i], sum[i][1] / count[i]};
            }
        }
    }
    for (u32 i = 0; i < wedges.size(); ++i) {
        writeWedge(islands, uvs, wedges[i], Vector2f{static_cast<f32>(u[i][0]), static_cast<f32>(u[i][1])});
    }
    // Flips, counted against the island's majority as LSCM counts them.
    result.flipped = 0;
    i32 votes = 0;
    std::vector<f32> signs;
    for (const u32 face : islands.facesOf(island)) {
        for (const Tri& tri : detail::TrianglesOf(mesh, FaceId(face))) {
            const f32 area = detail::TriAreaUv(uvs[tri.corner[0].index()], uvs[tri.corner[1].index()],
                                               uvs[tri.corner[2].index()]);
            if (area != 0.0f) {
                signs.push_back(area);
                votes += area > 0.0f ? 1 : -1;
            }
        }
    }
    for (const f32 area : signs) {
        if ((area > 0.0f) != (votes >= 0)) {
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
        result.capped = result.capped || solved.capped;
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
    // What the island covered before: the rectangle is laid in world units and
    // fitted back over it, as any re-solve lands, not left a model wide.
    const f32 areaBefore = detail::AreasOf(mesh, islands, island, positions, uvs).uv;
    Vector2f centreBefore{0.0f, 0.0f};
    {
        Vector2f low{1e30f, 1e30f};
        Vector2f high{-1e30f, -1e30f};
        for (const u32 face : islands.facesOf(island)) {
            for (const HalfedgeId h : topology.fh(FaceId(face))) {
                low = Vector2f{std::min(low.x, uvs[h.index()].x), std::min(low.y, uvs[h.index()].y)};
                high = Vector2f{std::max(high.x, uvs[h.index()].x), std::max(high.y, uvs[h.index()].y)};
            }
        }
        centreBefore = (low + high) * 0.5f;
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
    const f32 areaAfter = detail::AreasOf(mesh, islands, island, positions, uvs).uv;
    if (out.solve.ok() && areaBefore > 1e-12f && areaAfter > 1e-12f) {
        const f32 scale = std::sqrt(areaBefore / areaAfter);
        const Vector2f centreAfter{width * 0.5f, height * 0.5f};
        for (const u32 face : islands.facesOf(island)) {
            for (const HalfedgeId h : topology.fh(FaceId(face))) {
                uvs[h.index()] = centreBefore + (uvs[h.index()] - centreAfter) * scale;
            }
        }
    }
    return out;
}

namespace {

/// World units to tiles, when the frame says how many make one.
void toExtent(Mesh& mesh, std::span<const FaceId> faces, u32 set, f32 extent) {
    if (extent <= 0.0f) {
        return;
    }
    const Topology& topology = std::as_const(mesh).topology();
    const std::span<Vector2f> uvs =
        mesh.attributes.getOrCreate<Vector2f>(names::uv(set), Domain::Halfedge, AttrType::F32x2);
    const f32 scale = 1.0f / extent;
    for (const FaceId face : faces) {
        if (!face.valid() || topology.isDeleted(face)) {
            continue;
        }
        for (const HalfedgeId h : topology.fh(face)) {
            uvs[h.index()] = uvs[h.index()] * scale;
        }
    }
}

/// Box (EDIT_MODE_UV_REDESIGN.md §8): each face to the frame axis its normal is
/// nearest, projected on that plane the way round that keeps its winding, and
/// a cut wherever two planes meet.
FlattenResult projectBox(Mesh& mesh, std::span<const FaceId> faces, u32 set, const ProjectFrame& frame) {
    FlattenResult result;
    const Topology& topology = std::as_const(mesh).topology();
    const std::span<const Vector3f> positions =
        std::as_const(mesh).attributes.get<const Vector3f>(names::kPosition, Domain::Vertex);
    const std::span<Vector2f> uvs =
        mesh.attributes.getOrCreate<Vector2f>(names::uv(set), Domain::Halfedge, AttrType::F32x2);
    constexpr u8 kNone = 0xFF;
    std::vector<u8> side(topology.faceCount(), kNone);
    for (const FaceId face : faces) {
        if (!face.valid() || topology.isDeleted(face)) {
            continue;
        }
        // Newell's normal: right for any polygon, planar or not.
        Vector3f n{0.0f, 0.0f, 0.0f};
        for (const HalfedgeId h : topology.fh(face)) {
            const Vector3f& p = positions[topology.from(h).index()];
            const Vector3f& q = positions[topology.from(topology.next(h)).index()];
            n.x += (p.y - q.y) * (p.z + q.z);
            n.y += (p.z - q.z) * (p.x + q.x);
            n.z += (p.x - q.x) * (p.y + q.y);
        }
        const f32 along[3] = {n.dot(frame.axisU), n.dot(frame.axisV), n.dot(frame.axisN)};
        u32 axis = 0;
        for (u32 k = 1; k < 3; ++k) {
            if (std::abs(along[k]) > std::abs(along[axis])) {
                axis = k;
            }
        }
        side[face.index()] = static_cast<u8>(axis * 2 + (along[axis] < 0.0f ? 1 : 0));
        for (const HalfedgeId h : topology.fh(face)) {
            const Vector3f d = positions[topology.from(h).index()] - frame.origin;
            const f32 u = d.dot(frame.axisU);
            const f32 v = d.dot(frame.axisV);
            const f32 w = d.dot(frame.axisN);
            // Each side as a camera outside it sees it: (right, down) with
            // right x down along the look, -V up, and a top's or a bottom's
            // front (-N) at its top.
            switch (side[face.index()]) {
            case 0: uvs[h.index()] = Vector2f{w, v}; break;   // +U, the right side
            case 1: uvs[h.index()] = Vector2f{-w, v}; break;  // -U, the left side
            case 2: uvs[h.index()] = Vector2f{u, w}; break;   // +V, the bottom
            case 3: uvs[h.index()] = Vector2f{-u, w}; break;  // -V, the top
            case 4: uvs[h.index()] = Vector2f{-u, v}; break;  // +N, the back
            default: uvs[h.index()] = Vector2f{u, v}; break;  // -N, the front
            }
        }
    }
    std::vector<EdgeId> cuts;
    for (const FaceId face : faces) {
        if (!face.valid() || face.index() >= side.size() || side[face.index()] == kNone) {
            continue;
        }
        for (const HalfedgeId h : topology.fh(face)) {
            const FaceId other = topology.face(topology.opposite(h));
            if (other.valid() && side[other.index()] != kNone && side[other.index()] != side[face.index()] &&
                other.index() > face.index()) {
                cuts.push_back(Topology::edge(h));
            }
        }
    }
    if (!cuts.empty()) {
        ApplyMarks(mesh, set, std::span<const EdgeId>(cuts.data(), cuts.size()), true);
    }
    return result;
}

} // namespace

FlattenResult Project(Mesh& mesh, std::span<const FaceId> faces, u32 set, ProjectShape shape,
                      const ProjectFrame& frame, const ProjectOptions& options) {
    FlattenResult result;
    if (!mesh.hasConnectivity() || faces.empty()) {
        result.refusal = FlattenResult::Refusal::NoFaces;
        return result;
    }
    if (shape == ProjectShape::Box) {
        result = projectBox(mesh, faces, set, frame);
        toExtent(mesh, faces, set, frame.extent);
        return result;
    }
    if (shape == ProjectShape::Cylinder && options.cap) {
        // The caps first, as Box's top and bottom, then the side without them;
        // the rim between the two is a cut.
        const std::span<const Vector3f> at =
            std::as_const(mesh).attributes.get<const Vector3f>(names::kPosition, Domain::Vertex);
        const Topology& topo = std::as_const(mesh).topology();
        std::vector<FaceId> caps;
        std::vector<FaceId> side;
        for (const FaceId face : faces) {
            if (!face.valid() || topo.isDeleted(face)) {
                continue;
            }
            Vector3f n{0.0f, 0.0f, 0.0f};
            for (const HalfedgeId h : topo.fh(face)) {
                n = n + cross(at[topo.from(h).index()], at[topo.to(h).index()]);
            }
            const f32 length = n.length();
            (length > 0.0f && std::abs(n.dot(frame.axisV)) >= 0.70710678f * length ? caps : side).push_back(face);
        }
        if (!caps.empty()) {
            projectBox(mesh, caps, set, frame);
            std::vector<u8> isSide(topo.faceCount(), 0);
            for (const FaceId face : side) {
                isSide[face.index()] = 1;
            }
            std::vector<EdgeId> rim;
            for (const FaceId face : caps) {
                for (const HalfedgeId h : topo.fh(face)) {
                    const FaceId other = topo.face(topo.opposite(h));
                    if (other.valid() && isSide[other.index()]) {
                        rim.push_back(Topology::edge(h));
                    }
                }
            }
            if (!rim.empty()) {
                ApplyMarks(mesh, set, std::span<const EdgeId>(rim.data(), rim.size()), true);
            }
            if (!side.empty()) {
                result = Project(mesh, side, set, shape, frame);
            }
            toExtent(mesh, caps, set, frame.extent);
            return result;
        }
    }
    const Topology& topology = std::as_const(mesh).topology();
    const std::span<const Vector3f> positions =
        std::as_const(mesh).attributes.get<const Vector3f>(names::kPosition, Domain::Vertex);
    const std::span<Vector2f> uvs =
        mesh.attributes.getOrCreate<Vector2f>(names::uv(set), Domain::Halfedge, AttrType::F32x2);

    // Round the V axis: 0 at the front (-N), a quarter turn at the right (+U),
    // the wrap at the back, so the front reads as a planar map of it would.
    const auto turn = [&](const Vector3f& d) {
        return std::atan2(d.dot(frame.axisU), -d.dot(frame.axisN)) / (2.0f * kPi) + 0.5f;
    };
    const auto project = [&](const Vector3f& world) {
        const Vector3f d = world - frame.origin;
        switch (shape) {
        case ProjectShape::Planar:
            return Vector2f{d.dot(frame.axisU), d.dot(frame.axisV)};
        case ProjectShape::Cylinder:
            return Vector2f{turn(d), d.dot(frame.axisV)};
        case ProjectShape::Sphere:
        default: {
            const f32 length = d.length();
            const f32 up = length > 0.0f ? -d.dot(frame.axisV) / length : 0.0f;
            return Vector2f{turn(d), std::acos(std::clamp(up, -1.0f, 1.0f)) / kPi};
        }
        }
    };

    std::vector<u8> projected(topology.faceCount(), 0);
    std::vector<FaceId> turned;
    f64 radius = 0.0;
    u32 points = 0;
    for (const FaceId face : faces) {
        if (!face.valid() || topology.isDeleted(face)) {
            continue;
        }
        projected[face.index()] = 1;
        f32 lowest = 2.0f;
        f32 highest = -1.0f;
        for (const HalfedgeId h : topology.fh(face)) {
            const Vector3f d = positions[topology.from(h).index()] - frame.origin;
            const Vector2f value = project(positions[topology.from(h).index()]);
            uvs[h.index()] = value;
            lowest = std::min(lowest, value.x);
            highest = std::max(highest, value.x);
            // The cylinder's radius is off its axis, the sphere's from its centre.
            radius += shape == ProjectShape::Cylinder ? (d - frame.axisV * d.dot(frame.axisV)).length() : d.length();
            ++points;
        }
        // A round projection wraps somewhere. A face that spans the turn takes
        // its low side round by one turn, so it stays whole and joins the face
        // beyond it; the edges where it then parts from its neighbours are the
        // cut.
        if (shape != ProjectShape::Planar && highest - lowest > 0.5f) {
            for (const HalfedgeId h : topology.fh(face)) {
                if (uvs[h.index()].x < 0.5f) {
                    uvs[h.index()].x += 1.0f;
                }
            }
            turned.push_back(face);
        }
    }
    std::vector<EdgeId> wrapped;
    const auto same = [&](HalfedgeId a, HalfedgeId b) {
        return uvs[a.index()].x == uvs[b.index()].x && uvs[a.index()].y == uvs[b.index()].y;
    };
    for (const FaceId face : turned) {
        for (const HalfedgeId h : topology.fh(face)) {
            const HalfedgeId across = topology.opposite(h);
            const FaceId other = topology.face(across);
            if (!other.valid() || projected[other.index()] == 0) {
                continue;
            }
            if (!same(h, topology.next(across)) || !same(topology.next(h), across)) {
                wrapped.push_back(Topology::edge(h));
            }
        }
    }
    // A turn in arc length, so the map keeps the surface's proportions: round
    // the cylinder at its mean radius against its height, and both ways on the
    // sphere.
    if (shape != ProjectShape::Planar && points != 0) {
        const f32 r = static_cast<f32>(radius / points);
        const f32 across = 2.0f * kPi * r;
        const f32 down = shape == ProjectShape::Sphere ? kPi * r : 1.0f;
        for (const FaceId face : faces) {
            if (!face.valid() || topology.isDeleted(face)) {
                continue;
            }
            for (const HalfedgeId h : topology.fh(face)) {
                uvs[h.index()] = Vector2f{uvs[h.index()].x * across, uvs[h.index()].y * down};
            }
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
    toExtent(mesh, faces, set, frame.extent);
    return result;
}

namespace {

Vector3f unitOr(const Vector3f& v, const Vector3f& fallback) {
    const f32 length = v.length();
    return length > 1e-6f ? v * (1.0f / length) : fallback;
}

/// @p v with its part along unit @p axis taken out.
Vector3f across(const Vector3f& v, const Vector3f& axis) {
    return v - axis * v.dot(axis);
}

/// Any unit vector square to unit @p axis.
Vector3f squareTo(const Vector3f& axis) {
    const Vector3f seed = std::abs(axis.x) < 0.9f ? Vector3f{1.0f, 0.0f, 0.0f} : Vector3f{0.0f, 1.0f, 0.0f};
    return unitOr(cross(axis, seed), Vector3f{0.0f, 0.0f, 1.0f});
}

} // namespace

ProjectFrame UprightFrame(const Vector3f& origin, const Vector3f& look, const Vector3f& up,
                          const Vector3f& front) {
    ProjectFrame frame;
    frame.origin = origin;
    const Vector3f n = unitOr(look, Vector3f{-1.0f, 0.0f, 0.0f});
    // The image's up: the world's up laid on the view; looking along it, the
    // front is up instead, so a top view has the front at its top.
    Vector3f top = across(up, n);
    if (top.length() < 1e-3f) {
        top = across(front, n);
    }
    top = unitOr(top, squareTo(n));
    frame.axisN = n;
    frame.axisV = top * -1.0f;
    frame.axisU = cross(frame.axisV, frame.axisN);
    return frame;
}

ProjectFrame PoleFrame(const Vector3f& origin, const Vector3f& pole, const Vector3f& up,
                       const Vector3f& front) {
    ProjectFrame frame;
    frame.origin = origin;
    Vector3f v = unitOr(pole, Vector3f{0.0f, 0.0f, -1.0f});
    // Down the image from the end nearer up; a level pole from the end nearer
    // the front.
    const f32 rise = v.dot(up);
    const f32 ahead = v.dot(front);
    if (std::abs(rise) > 1e-3f ? rise > 0.0f : ahead > 0.0f) {
        v = v * -1.0f;
    }
    // Looked at from the front, so the wrap -- at +N -- is on the back; a pole
    // along the front is looked at from above, and wraps underneath.
    Vector3f n = across(front, v) * -1.0f;
    if (n.length() < 1e-3f) {
        n = across(up, v) * -1.0f;
    }
    n = unitOr(n, squareTo(v));
    frame.axisV = v;
    frame.axisN = n;
    frame.axisU = cross(v, n);
    return frame;
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
