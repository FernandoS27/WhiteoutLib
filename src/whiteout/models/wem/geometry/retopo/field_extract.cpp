// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "field_extract.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

namespace whiteout {
namespace models {
namespace wem {
namespace geom {
namespace retopo {

namespace {

/// A cell of more corners than this keeps its work triangles.
constexpr std::size_t kMaxCorners = 6;
/// A face with less area than this share of a lattice cell, or two corners
/// nearer than this share of a step, is crushed.
constexpr f64 kCrushed = 0.02;
/// Leftover faces merge only into quads whose worst corner is nearer square
/// than this, in radians.
constexpr f64 kMergeTurn = 1.0;

/// No corner twice.
bool Distinct(const std::vector<u32>& corners) {
    for (std::size_t i = 0; i < corners.size(); ++i) {
        for (std::size_t j = i + 1; j < corners.size(); ++j) {
            if (corners[i] == corners[j]) {
                return false;
            }
        }
    }
    return true;
}

u32 Rank(VertexKind kind) {
    return kind == VertexKind::Corner ? 2u : (kind == VertexKind::Feature ? 1u : 0u);
}

} // namespace

QuadMesh ExtractFieldQuads(WorkMesh& mesh, const Surface& surface, const PositionField& field, LatticeSteps steps,
                           bool split, FieldExtractStats& stats) {
    const f64 h = field.scale;
    // Each vertex's lattice, carried to whichever vertex survives it, and
    // each halfedge's steps in its start's frame.
    PositionField lattice = field;
    std::vector<V3>& origins = lattice.origins;
    auto shiftOf = [&](u32 he) {
        LatticeShift s;
        s.x = steps.x[he];
        s.y = steps.y[he];
        s.gap = Distance(origins[mesh.from(he)], origins[mesh.to(he)]) / h;
        return s;
    };
    std::vector<u32> outgoing;
    auto turnOutgoing = [&](u32 v, u32 quarters) {
        mesh.ring(v, outgoing);
        for (const u32 o : outgoing) {
            TurnSteps(steps.x[o], steps.y[o], quarters);
        }
    };

    // Every zero shift collapsed, the nearest lattices first; a refused one
    // may pass once its neighbours have gone.
    struct Candidate {
        f64 gap;
        u32 halfedge;
    };
    std::vector<Candidate> candidates;
    for (u32 pass = 0; pass < 64; ++pass) {
        candidates.clear();
        for (u32 t = 0; t < mesh.triangleCount(); ++t) {
            if (mesh.dead[t]) {
                continue;
            }
            for (u32 i = 0; i < 3; ++i) {
                const u32 he = 3 * t + i;
                if (mesh.twins[he] != kNone && mesh.twins[he] < he) {
                    continue;
                }
                const LatticeShift s = shiftOf(he);
                if (s.zero()) {
                    candidates.push_back({s.gap, he});
                }
            }
        }
        std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
            return a.gap != b.gap ? a.gap < b.gap : a.halfedge < b.halfedge;
        });
        u32 done = 0;
        for (const Candidate& c : candidates) {
            if (mesh.dead[c.halfedge / 3]) {
                continue;
            }
            const u32 a = mesh.from(c.halfedge);
            const u32 b = mesh.to(c.halfedge);
            if (!shiftOf(c.halfedge).zero() || !mesh.canCollapse(c.halfedge)) {
                continue;
            }
            // The collapse keeps b's slot; a keeps its place in it when it is
            // the stronger kind, or as strong and nearer the lattice point.
            const bool keepA = Rank(mesh.kinds[a]) != Rank(mesh.kinds[b])
                                   ? Rank(mesh.kinds[a]) > Rank(mesh.kinds[b])
                                   : Distance(mesh.positions[a], origins[a]) < Distance(mesh.positions[b], origins[b]);
            // The survivor's frame is the one kept: the other's outgoing steps
            // turn into it. Steps into either end stand, the two being one
            // lattice point.
            if (keepA) {
                turnOutgoing(b, FrameQuarter(lattice, a, b));
            } else {
                turnOutgoing(a, FrameQuarter(lattice, b, a));
            }
            mesh.collapse(c.halfedge);
            if (keepA) {
                mesh.positions[b] = mesh.positions[a];
                mesh.homes[b] = mesh.homes[a];
                mesh.onCurve[b] = mesh.onCurve[a];
                mesh.curve[b] = mesh.curve[a];
                mesh.kinds[b] = mesh.kinds[a];
                mesh.sourceVertex[b] = mesh.sourceVertex[a];
                lattice.normals[b] = lattice.normals[a];
                lattice.axes[b] = lattice.axes[a];
                origins[b] = origins[a];
            }
            ++done;
        }
        stats.collapses += done;
        if (done == 0) {
            break;
        }
    }

    // A zero shift still standing: its ends keep their own places, so the
    // face between them is not crushed to a point.
    std::vector<u8> ownPlace(mesh.vertexCount(), 0);
    for (u32 t = 0; t < mesh.triangleCount(); ++t) {
        if (mesh.dead[t]) {
            continue;
        }
        for (u32 i = 0; i < 3; ++i) {
            const u32 he = 3 * t + i;
            if ((mesh.twins[he] == kNone || mesh.twins[he] > he) && shiftOf(he).zero()) {
                ownPlace[mesh.from(he)] = 1;
                ownPlace[mesh.to(he)] = 1;
                ++stats.refused;
            }
        }
    }

    // The output vertices: each survivor at its lattice point, on the source.
    QuadMesh quads;
    std::vector<u32> outOf(mesh.vertexCount(), kNone);
    for (u32 v = 0; v < mesh.vertexCount(); ++v) {
        if (!mesh.alive(v)) {
            continue;
        }
        const V3 target = ownPlace[v] ? mesh.positions[v] : origins[v];
        const f64 radius = Distance(target, mesh.positions[v]) + 0.5 * h;
        SurfacePoint home = mesh.homes[v];
        V3 position = mesh.positions[v];
        FeaturePoint onCurve;
        if (mesh.kinds[v] == VertexKind::Feature && mesh.onCurve[v].valid()) {
            const FeaturePoint at = surface.locateOnCurve(mesh.onCurve[v], target, radius);
            if (at.valid()) {
                onCurve = at;
                home = surface.toSurface(at);
                position = surface.position(at);
            }
        } else if (mesh.kinds[v] == VertexKind::Free) {
            const u32 start = mesh.homes[v].triangle;
            const SurfacePoint at = surface.locate(std::span<const u32>(&start, 1), target, radius);
            if (at.valid()) {
                home = at;
                position = surface.position(at);
            }
        }
        outOf[v] = quads.vertexCount();
        quads.positions.push_back(position);
        quads.homes.push_back(home);
        quads.kinds.push_back(mesh.kinds[v]);
        quads.onCurve.push_back(mesh.kinds[v] == VertexKind::Feature ? onCurve : FeaturePoint{});
        quads.curve.push_back(mesh.kinds[v] == VertexKind::Feature ? mesh.curve[v] : kNone);
    }

    // Faces: each lattice cell's triangles together -- every edge inside a cell
    // (a diagonal step, a long one, a zero one a collapse had to keep)
    // dissolved -- so a cell is one polygon however the work mesh cut it.
    struct Face {
        std::vector<u32> corners; ///< Survivors, counter-clockwise.
        std::vector<u32> sides;   ///< Per side, its work halfedge; kNone for one made here.
    };
    const u32 triangles = mesh.triangleCount();
    std::vector<u32> cell(triangles);
    for (u32 t = 0; t < triangles; ++t) {
        cell[t] = t;
    }
    auto root = [&](u32 t) {
        while (cell[t] != t) {
            cell[t] = cell[cell[t]];
            t = cell[t];
        }
        return t;
    };
    for (u32 t = 0; t < triangles; ++t) {
        if (mesh.dead[t]) {
            continue;
        }
        for (u32 i = 0; i < 3; ++i) {
            const u32 he = 3 * t + i;
            const u32 twin = mesh.twins[he];
            if (twin != kNone && twin > he && !shiftOf(he).unit()) {
                cell[root(twin / 3)] = root(t);
            }
        }
    }
    std::vector<std::vector<u32>> members(triangles);
    for (u32 t = 0; t < triangles; ++t) {
        if (!mesh.dead[t]) {
            members[root(t)].push_back(t);
        }
    }
    std::vector<Face> faces;
    auto addTriangle = [&](u32 t) {
        faces.push_back(Face{{mesh.corners[3 * t], mesh.corners[3 * t + 1], mesh.corners[3 * t + 2]},
                             {3 * t, 3 * t + 1, 3 * t + 2}});
    };
    std::unordered_map<u32, u32> boundaryFrom; // a cell's boundary, by its start
    for (u32 r = 0; r < triangles; ++r) {
        const std::vector<u32>& group = members[r];
        if (group.size() == 1) {
            addTriangle(group[0]);
            continue;
        }
        if (group.empty()) {
            continue;
        }
        // One loop round the cell, no corner twice, or its triangles stand.
        boundaryFrom.clear();
        bool simple = true;
        u32 count = 0;
        for (const u32 t : group) {
            for (u32 i = 0; i < 3; ++i) {
                const u32 he = 3 * t + i;
                const u32 twin = mesh.twins[he];
                if (twin != kNone && root(twin / 3) == r) {
                    continue;
                }
                simple = simple && boundaryFrom.emplace(mesh.from(he), he).second;
                ++count;
            }
        }
        Face face;
        if (simple && count >= 3 && count <= kMaxCorners) {
            u32 he = boundaryFrom.begin()->second;
            for (u32 k = 0; k < count; ++k) {
                face.corners.push_back(mesh.from(he));
                face.sides.push_back(he);
                const auto next = boundaryFrom.find(mesh.to(he));
                if (next == boundaryFrom.end()) {
                    simple = false;
                    break;
                }
                he = next->second;
            }
            simple = simple && he == face.sides.front();
        }
        // A closed piece all one cell has no loop at all.
        if (simple && count >= 3 && face.corners.size() == count) {
            faces.push_back(std::move(face));
        } else {
            for (const u32 t : group) {
                addTriangle(t);
            }
        }
    }

    // What is left over pairs up: two triangles side by side are a quad, and a
    // triangle beside a pentagon (a lattice dislocation) two, split across the
    // hexagon they make.
    auto at = [&](u32 v) -> const V3& { return quads.positions[outOf[v]]; };
    auto normalOf = [&](const std::vector<u32>& corners) {
        V3 n{0.0, 0.0, 0.0};
        for (const u32 v : corners) {
            n = n + surface.normal(quads.homes[outOf[v]]);
        }
        return Unit(n);
    };
    // The worst corner's turn from square, or infinity for a corner that turns
    // the wrong way round.
    auto squareness = [&](const std::vector<u32>& corners) {
        const V3 n = normalOf(corners);
        const std::size_t count = corners.size();
        f64 worst = 0.0;
        for (std::size_t i = 0; i < count; ++i) {
            const V3& p = at(corners[i]);
            const V3 out = at(corners[(i + 1) % count]) - p;
            const V3 in = at(corners[(i + count - 1) % count]) - p;
            if (Dot(Cross(out, in), n) <= 0.0) {
                return std::numeric_limits<f64>::infinity();
            }
            worst = std::max(worst, std::abs(Angle(out, in) - 0.5 * kPi));
        }
        return worst;
    };
    auto merge = [](const Face& a, std::size_t i, const Face& b, std::size_t j) {
        // a's side i is b's side j turned round: a from the side's end round to
        // its start, then b on from there.
        Face merged;
        const std::size_t na = a.corners.size();
        const std::size_t nb = b.corners.size();
        for (std::size_t k = 1; k < na; ++k) {
            merged.corners.push_back(a.corners[(i + k) % na]);
            merged.sides.push_back(a.sides[(i + k) % na]);
        }
        merged.corners.push_back(a.corners[i]);
        merged.sides.push_back(b.sides[(j + 1) % nb]);
        for (std::size_t k = 2; k < nb; ++k) {
            merged.corners.push_back(b.corners[(j + k) % nb]);
            merged.sides.push_back(b.sides[(j + k) % nb]);
        }
        return merged;
    };
    auto edgeKey = [](u32 a, u32 b) { return (static_cast<u64>(std::min(a, b)) << 32) | std::max(a, b); };
    for (u32 round = 0; round < 2; ++round) {
        // Each side's two faces.
        std::unordered_map<u64, std::pair<u32, u32>> sideFaces;
        for (u32 f = 0; f < faces.size(); ++f) {
            for (std::size_t i = 0; i < faces[f].corners.size(); ++i) {
                const u32 a = faces[f].corners[i];
                const u32 b = faces[f].corners[(i + 1) % faces[f].corners.size()];
                auto [it, fresh] = sideFaces.emplace(edgeKey(a, b), std::make_pair(f, kNone));
                if (!fresh) {
                    it->second.second = f;
                }
            }
        }
        struct Candidate {
            f64 worst;
            u32 a;
            u32 b;
            std::size_t i;
            std::size_t j;
            u32 diagonal; ///< The hexagon's split, 0..2; unused for two triangles.
        };
        std::vector<Candidate> candidates;
        const std::size_t partner = round == 0 ? 3 : 5;
        for (u32 f = 0; f < faces.size(); ++f) {
            if (faces[f].corners.size() != 3) {
                continue;
            }
            for (std::size_t i = 0; i < 3; ++i) {
                const u32 a = faces[f].corners[i];
                const u32 b = faces[f].corners[(i + 1) % 3];
                const auto [x, y] = sideFaces[edgeKey(a, b)];
                const u32 g = x == f ? y : x;
                if (g == kNone || g == f || faces[g].corners.size() != partner || (round == 0 && g < f)) {
                    continue;
                }
                std::size_t j = 0;
                while (j < partner && !(faces[g].corners[j] == b && faces[g].corners[(j + 1) % partner] == a)) {
                    ++j;
                }
                if (j == partner) {
                    continue;
                }
                const Face merged = merge(faces[f], i, faces[g], j);
                if (!Distinct(merged.corners)) {
                    continue; // the two share more than the one side
                }
                if (round == 0) {
                    const f64 worst = squareness(merged.corners);
                    if (worst < kMergeTurn) {
                        candidates.push_back({worst, f, g, i, j, 0});
                    }
                    continue;
                }
                // The hexagon's best split, along no edge it has elsewhere.
                for (u32 d = 0; d < 3; ++d) {
                    const u32 p = merged.corners[d];
                    const u32 q = merged.corners[d + 3];
                    if (sideFaces.count(edgeKey(p, q)) != 0) {
                        continue;
                    }
                    const std::vector<u32> first(merged.corners.begin() + d, merged.corners.begin() + d + 4);
                    const std::vector<u32> second = {merged.corners[d + 3], merged.corners[(d + 4) % 6],
                                                     merged.corners[(d + 5) % 6], merged.corners[d]};
                    const f64 worst = std::max(squareness(first), squareness(second));
                    if (worst < kMergeTurn) {
                        candidates.push_back({worst, f, g, i, j, d});
                    }
                }
            }
        }
        std::sort(candidates.begin(), candidates.end(), [](const Candidate& x, const Candidate& y) {
            return x.worst != y.worst ? x.worst < y.worst : (x.a != y.a ? x.a < y.a : x.b < y.b);
        });
        std::vector<u8> used(faces.size(), 0);
        std::vector<Face> made;
        for (const Candidate& c : candidates) {
            if (used[c.a] || used[c.b]) {
                continue;
            }
            used[c.a] = 1;
            used[c.b] = 1;
            const Face merged = merge(faces[c.a], c.i, faces[c.b], c.j);
            if (round == 0) {
                made.push_back(merged);
                continue;
            }
            const u32 d = c.diagonal;
            Face first;
            Face second;
            for (u32 k = 0; k < 4; ++k) {
                first.corners.push_back(merged.corners[(d + k) % 6]);
                first.sides.push_back(k < 3 ? merged.sides[(d + k) % 6] : kNone);
                second.corners.push_back(merged.corners[(d + 3 + k) % 6]);
                second.sides.push_back(k < 3 ? merged.sides[(d + 3 + k) % 6] : kNone);
            }
            made.push_back(std::move(first));
            made.push_back(std::move(second));
        }
        std::vector<Face> kept;
        for (u32 f = 0; f < faces.size(); ++f) {
            if (!used[f]) {
                kept.push_back(std::move(faces[f]));
            }
        }
        for (Face& f : made) {
            kept.push_back(std::move(f));
        }
        faces = std::move(kept);
    }
    // A corner with only two faces round it, both sides shared (a doublet),
    // goes, the two faces one: split as it stands it would leave two quads
    // folded round it.
    for (bool again = true; again;) {
        again = false;
        std::unordered_map<u32, std::vector<u32>> round;
        for (u32 f = 0; f < faces.size(); ++f) {
            for (const u32 v : faces[f].corners) {
                round[v].push_back(f);
            }
        }
        std::vector<u8> gone(faces.size(), 0);
        std::vector<Face> made;
        for (const auto& [v, list] : round) {
            if (list.size() != 2 || gone[list[0]] || gone[list[1]] || list[0] == list[1]) {
                continue;
            }
            const Face& a = faces[list[0]];
            const Face& b = faces[list[1]];
            const std::size_t na = a.corners.size();
            const std::size_t nb = b.corners.size();
            const std::size_t i = std::find(a.corners.begin(), a.corners.end(), v) - a.corners.begin();
            const std::size_t j = std::find(b.corners.begin(), b.corners.end(), v) - b.corners.begin();
            // Both of v's sides shared: a turns from p through v to q, b back.
            const u32 p = a.corners[(i + na - 1) % na];
            const u32 q = a.corners[(i + 1) % na];
            if (b.corners[(j + 1) % nb] != p || b.corners[(j + nb - 1) % nb] != q || na + nb < 7) {
                continue;
            }
            // a from q round to p, then b from p round to q, v left out.
            Face merged;
            for (std::size_t k = 1; k < na; ++k) {
                merged.corners.push_back(a.corners[(i + k) % na]);
                merged.sides.push_back(k + 1 < na ? a.sides[(i + k) % na] : b.sides[(j + 1) % nb]);
            }
            for (std::size_t k = 2; k + 1 < nb; ++k) {
                merged.corners.push_back(b.corners[(j + k) % nb]);
                merged.sides.push_back(b.sides[(j + k) % nb]);
            }
            if (!Distinct(merged.corners)) {
                continue;
            }
            gone[list[0]] = 1;
            gone[list[1]] = 1;
            made.push_back(std::move(merged));
            again = true;
        }
        if (!again) {
            break;
        }
        std::vector<Face> kept;
        for (u32 f = 0; f < faces.size(); ++f) {
            if (!gone[f]) {
                kept.push_back(std::move(faces[f]));
            }
        }
        for (Face& f : made) {
            kept.push_back(std::move(f));
        }
        faces = std::move(kept);
    }
    // A face the lattice crushed -- two corners on one point (a feature point
    // pressed to its curve's end, onto the corner there), or all on a line --
    // takes its corners' own places, which the work mesh keeps apart.
    for (const Face& face : faces) {
        const std::size_t n = face.corners.size();
        V3 newell{0.0, 0.0, 0.0};
        f64 shortest = std::numeric_limits<f64>::infinity();
        for (std::size_t i = 0; i < n; ++i) {
            newell = newell + Cross(at(face.corners[i]), at(face.corners[(i + 1) % n]));
            for (std::size_t j = i + 1; j < n; ++j) {
                shortest = std::min(shortest, Distance(at(face.corners[i]), at(face.corners[j])));
            }
        }
        if (0.5 * Length(newell) >= kCrushed * h * h && shortest >= kCrushed * h) {
            continue;
        }
        for (const u32 v : face.corners) {
            const u32 o = outOf[v];
            quads.positions[o] = mesh.positions[v];
            quads.homes[o] = mesh.homes[v];
            if (mesh.kinds[v] == VertexKind::Feature) {
                quads.onCurve[o] = mesh.onCurve[v];
            }
        }
        ++stats.crushed;
    }
    // What is still degenerate goes by topology. Two corners on one point (two
    // feature points pressed onto a corner) become one; a face with no area
    // left (three points along one straight feature) is taken into the face
    // across its longest side, which gains its other corners.
    for (bool again = true; again;) {
        again = false;
        for (u32 f = 0; f < faces.size() && !again; ++f) {
            const std::size_t n = faces[f].corners.size();
            for (std::size_t i = 0; i < n && !again; ++i) {
                const u32 a = faces[f].corners[i];
                const u32 b = faces[f].corners[(i + 1) % n];
                if (Distance(at(a), at(b)) > 1e-6 * h) {
                    continue;
                }
                // The stronger kind stays; only where every face keeps its
                // corners distinct.
                const u32 keep = Rank(mesh.kinds[a]) >= Rank(mesh.kinds[b]) ? a : b;
                const u32 drop = keep == a ? b : a;
                std::vector<Face> welded;
                bool clean = true;
                for (const Face& face : faces) {
                    Face c;
                    const std::size_t m = face.corners.size();
                    for (std::size_t k = 0; k < m && clean; ++k) {
                        const u32 v = face.corners[k] == drop ? keep : face.corners[k];
                        const u32 w = face.corners[(k + 1) % m] == drop ? keep : face.corners[(k + 1) % m];
                        if (v != w) {
                            c.corners.push_back(v);
                            c.sides.push_back(face.sides[k]);
                        }
                    }
                    if (c.corners.size() >= 3) {
                        clean = clean && Distinct(c.corners);
                        welded.push_back(std::move(c));
                    }
                }
                if (clean) {
                    faces = std::move(welded);
                    ++stats.welded;
                    again = true;
                }
            }
        }
    }
    for (bool again = true; again;) {
        again = false;
        std::unordered_map<u64, u32> sideOf; // directed side -> its face
        for (u32 f = 0; f < faces.size(); ++f) {
            const std::size_t n = faces[f].corners.size();
            for (std::size_t i = 0; i < n; ++i) {
                sideOf[(static_cast<u64>(faces[f].corners[i]) << 32) | faces[f].corners[(i + 1) % n]] = f;
            }
        }
        for (u32 f = 0; f < faces.size() && !again; ++f) {
            const Face& flat = faces[f];
            const std::size_t n = flat.corners.size();
            V3 newell{0.0, 0.0, 0.0};
            std::size_t span = 0;
            f64 longest = -1.0;
            for (std::size_t i = 0; i < n; ++i) {
                const V3& p = at(flat.corners[i]);
                const V3& q = at(flat.corners[(i + 1) % n]);
                newell = newell + Cross(p, q);
                if (Distance(p, q) > longest) {
                    longest = Distance(p, q);
                    span = i;
                }
            }
            if (Length(newell) > 1e-9 * h * h) {
                continue;
            }
            const u32 from = flat.corners[span];
            const u32 to = flat.corners[(span + 1) % n];
            const auto across = sideOf.find((static_cast<u64>(to) << 32) | from);
            if (across == sideOf.end() || across->second == f) {
                continue;
            }
            // Into the face across, between its side's ends: the flat face's
            // corners from past `to` round to before `from`.
            Face& host = faces[across->second];
            const std::size_t m = host.corners.size();
            std::size_t at0 = 0;
            while (at0 < m && !(host.corners[at0] == to && host.corners[(at0 + 1) % m] == from)) {
                ++at0;
            }
            bool fresh = at0 < m;
            for (std::size_t j = 1; j + 1 < n && fresh; ++j) {
                const u32 c = flat.corners[(span + 1 + j) % n];
                fresh = std::find(host.corners.begin(), host.corners.end(), c) == host.corners.end();
            }
            if (!fresh) {
                continue;
            }
            Face grown;
            for (std::size_t k = 0; k < m; ++k) {
                grown.corners.push_back(host.corners[k]);
                if (k != at0) {
                    grown.sides.push_back(host.sides[k]);
                    continue;
                }
                for (std::size_t j = 1; j + 1 < n; ++j) {
                    const std::size_t c = (span + 1 + j) % n;
                    grown.sides.push_back(flat.sides[(c + n - 1) % n]);
                    grown.corners.push_back(flat.corners[c]);
                }
                grown.sides.push_back(flat.sides[(span + n - 1) % n]);
            }
            host = std::move(grown);
            faces.erase(faces.begin() + f);
            ++stats.absorbed;
            again = true;
        }
    }
    for (const Face& face : faces) {
        const std::size_t n = face.corners.size();
        (n == 4 ? stats.quads : (n == 3 ? stats.triangles : stats.polygons)) += 1;
    }

    auto addFace = [&](std::initializer_list<u32> corners) {
        quads.faceVertices.insert(quads.faceVertices.end(), corners.begin(), corners.end());
        quads.faceOffsets.push_back(static_cast<u32>(quads.faceVertices.size()));
        quads.facePatch.push_back(0);
    };
    if (!split) {
        for (const Face& face : faces) {
            for (const u32 v : face.corners) {
                quads.faceVertices.push_back(outOf[v]);
            }
            quads.faceOffsets.push_back(static_cast<u32>(quads.faceVertices.size()));
            quads.facePatch.push_back(0);
        }
    } else {
        // Each face of n corners as n quads: a point mid-side, shared with the
        // face across, and one in the middle.
        auto addVertex = [&](const V3& position, const SurfacePoint& home, VertexKind kind, const FeaturePoint& point,
                             u32 curve) {
            quads.positions.push_back(position);
            quads.homes.push_back(home);
            quads.kinds.push_back(kind);
            quads.onCurve.push_back(point);
            quads.curve.push_back(curve);
            return quads.vertexCount() - 1;
        };
        std::unordered_map<u64, u32> middles;
        auto middle = [&](u32 a, u32 b, u32 he) {
            const u32 oa = outOf[a];
            const u32 ob = outOf[b];
            const u64 key = edgeKey(oa, ob);
            const auto found = middles.find(key);
            if (found != middles.end()) {
                return found->second;
            }
            const V3 mid = Lerp(quads.positions[oa], quads.positions[ob], 0.5);
            const f64 reach = Distance(quads.positions[oa], quads.positions[ob]) + 0.5 * h;
            u32 made = kNone;
            if (he != kNone && mesh.isFeature(he) && mesh.curves[he] != kNone && mesh.kinds[a] != VertexKind::Free &&
                mesh.kinds[b] != VertexKind::Free) {
                const u32 curve = mesh.curves[he];
                const FeaturePoint start = CurveStart(mesh, surface, a, curve, mid);
                const FeaturePoint point = start.valid() ? surface.locateOnCurve(start, mid, reach) : FeaturePoint{};
                if (point.valid()) {
                    made = addVertex(surface.position(point), surface.toSurface(point), VertexKind::Feature, point, curve);
                }
            }
            if (made == kNone) {
                const u32 starts[2] = {quads.homes[oa].triangle, quads.homes[ob].triangle};
                SurfacePoint home = surface.locate(starts, mid, reach);
                if (!home.valid()) {
                    home = quads.homes[oa];
                }
                made = addVertex(surface.position(home), home, VertexKind::Free, FeaturePoint{}, kNone);
            }
            middles.emplace(key, made);
            return made;
        };
        for (const Face& face : faces) {
            const std::size_t n = face.corners.size();
            std::vector<u32> corner(n);
            std::vector<u32> mids(n);
            V3 centroid{0.0, 0.0, 0.0};
            std::vector<u32> starts;
            for (std::size_t i = 0; i < n; ++i) {
                corner[i] = outOf[face.corners[i]];
                mids[i] = middle(face.corners[i], face.corners[(i + 1) % n], face.sides[i]);
                centroid = centroid + quads.positions[corner[i]];
                starts.push_back(quads.homes[corner[i]].triangle);
            }
            centroid = centroid * (1.0 / static_cast<f64>(n));
            f64 reach = 0.0;
            for (std::size_t i = 0; i < n; ++i) {
                reach = std::max(reach, Distance(centroid, quads.positions[corner[i]]));
            }
            SurfacePoint home = surface.locate(starts, centroid, reach + 0.5 * h);
            if (!home.valid()) {
                home = quads.homes[corner[0]];
            }
            const u32 centre = addVertex(surface.position(home), home, VertexKind::Free, FeaturePoint{}, kNone);
            for (std::size_t i = 0; i < n; ++i) {
                addFace({corner[i], mids[i], centre, mids[(i + n - 1) % n]});
            }
        }
    }

    // Faces that turn from the surface where they were placed.
    for (u32 f = 0; f < quads.faceCount(); ++f) {
        const std::span<const u32> face = quads.face(f);
        V3 newell{0.0, 0.0, 0.0};
        V3 normal{0.0, 0.0, 0.0};
        for (std::size_t i = 0; i < face.size(); ++i) {
            newell = newell + Cross(quads.positions[face[i]], quads.positions[face[(i + 1) % face.size()]]);
            normal = normal + surface.normal(quads.homes[face[i]]);
        }
        stats.flipped += Dot(newell, normal) < 0.0 ? 1 : 0;
    }
    return quads;
}

} // namespace retopo
} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
