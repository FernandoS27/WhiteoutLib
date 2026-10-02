// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "layout.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <queue>
#include <string>
#include <unordered_map>

#include "patch_map.h"

namespace whiteout {
namespace models {
namespace wem {
namespace geom {
namespace retopo {

namespace {

constexpr f64 kQuarter = kPi * 0.5;
/// A trace edge's `curves` value: its path, past every source curve id.
constexpr u32 kTracePathBase = 0x40000000u;

/// Refits triangle @p t's frame to its corners and sets its cross from
/// @p direction, or along its feature edge when it has one.
void RefreshTriangle(CrossField& field, const WorkMesh& mesh, const Surface& surface, u32 t,
                     const V3& direction) {
    const u32 triangles = mesh.triangleCount();
    if (field.angles.size() < triangles) {
        field.axisX.resize(triangles);
        field.axisY.resize(triangles);
        field.normals.resize(triangles);
        field.angles.resize(triangles, 0.0);
        field.constrained.resize(triangles, 0);
    }
    SetFrame(mesh, surface, field, t);
    f64 angle = field.angleOf(t, direction);
    field.constrained[t] = 0;
    for (u32 i = 0; i < 3; ++i) {
        const u32 h = 3 * t + i;
        if (mesh.isFeature(h)) {
            angle = field.angleOf(t, FeatureDirection(mesh, surface, h));
            field.constrained[t] = 1;
            break;
        }
    }
    angle = std::fmod(angle, kQuarter);
    if (angle < 0.0) {
        angle += kQuarter;
    }
    field.angles[t] = angle;
}

f64 SignedAngle(const V3& from, const V3& to, const V3& normal) {
    return std::atan2(Dot(Cross(from, to), normal), Dot(from, to));
}

} // namespace

u32 SplitWithField(WorkMesh& mesh, CrossField& field, const Surface& surface, u32 h, const V3& point) {
    const u32 a = mesh.from(h);
    const u32 b = mesh.to(h);
    const u32 twin = mesh.twins[h];
    const V3 d0 = field.direction(h / 3, 0);
    const V3 d1 = twin != kNone ? field.direction(twin / 3, 0) : d0;
    const f64 reach = Distance(mesh.positions[a], mesh.positions[b]);
    u32 m = kNone;
    FeaturePoint start;
    if (mesh.isFeature(h)) {
        start = CurveStart(mesh, surface, a, mesh.curves[h], point);
        if (!start.valid()) {
            start = CurveStart(mesh, surface, b, mesh.curves[h], point);
        }
    }
    if (start.valid()) {
        const u32 curve = mesh.curves[h];
        const FeaturePoint at = surface.locateOnCurve(start, point, reach);
        m = mesh.addVertex(surface.position(at), surface.toSurface(at), VertexKind::Feature);
        mesh.onCurve[m] = at;
        mesh.curve[m] = curve;
    } else {
        const u32 start[2] = {mesh.homes[a].triangle, mesh.homes[b].triangle};
        SurfacePoint home = surface.locate(start, point, reach);
        if (!home.valid()) {
            home = mesh.homes[a];
        }
        m = mesh.addVertex(surface.position(home), home, VertexKind::Free);
    }
    const u32 before = mesh.triangleCount();
    mesh.split(h, m);
    RefreshTriangle(field, mesh, surface, h / 3, d0);
    RefreshTriangle(field, mesh, surface, before, d0);
    if (twin != kNone) {
        RefreshTriangle(field, mesh, surface, twin / 3, d1);
        RefreshTriangle(field, mesh, surface, before + 1, d1);
    }
    field.index.resize(mesh.vertexCount(), 0);
    return m;
}

namespace {

/// A trace to start. Either from a fresh point with no layout edge yet (a
/// singularity's separatrix, arm `offset` of `arms`), or from a layout vertex
/// into the sector that begins at `sector`, `offset` quarter turns past it.
struct Emission {
    u32 vertex = kNone;
    V3 direction{0.0, 0.0, 0.0};
    u32 sector = kNone;
    u32 offset = 0;
    u32 arms = 0; ///< A fresh point's quarter turns; 0 for a sector emission.
};

/// One streamline and the edge path it is snapped to.
struct Trace {
    Emission emission;
    std::vector<u32> path;
    // Where the streamline is: at a vertex, or on halfedge `entry` (of the
    // triangle it is entering) at `point`.
    u32 atVertex = kNone;
    u32 entry = kNone;
    V3 point{0.0, 0.0, 0.0};
    V3 direction{0.0, 0.0, 0.0};
    f64 length = 0.0;
    u32 steps = 0;
    bool done = false;
    bool collided = false;
    u32 finished = 0; ///< The order it stopped in, for the arrivals.
};

/// A patch's map corner by corner: per work triangle, its corners' points in
/// slot order, kept through the splits a spoke makes. A vertex on a seam
/// through the patch, or where its border pinches, is a point per side.
using MapCorners = std::unordered_map<u32, std::array<P2, 3>>;

class Builder {
public:
    Builder(WorkMesh& mesh, CrossField& field, const Surface& surface, const LayoutOptions& options,
            Layout& out)
        : mesh_(mesh), field_(field), surface_(surface), options_(options), out_(out) {}

    void run();

private:
    void grow();
    bool isLayoutVertex(u32 v) const {
        return onLayout_[v] != 0;
    }
    void note(const std::string& text) {
        if (options_.verbose) {
            out_.notes.push_back(text);
        }
    }
    /// `SplitWithField`, keeping the sector book: the halfedges a split
    /// renumbers carry their sectors, and a vertex split into a layout edge is
    /// flat on both sides.
    u32 split(u32 h, const V3& point);

    // --- the field round a vertex ---
    struct FanStep {
        u32 face = kNone;
        u32 out = kNone;
        f64 nearLabel = 0.0;
        f64 farLabel = 0.0;
        f64 offset = 0.0;
    };
    void walkFan(u32 v, u32 fromOut, std::vector<FanStep>& steps) const;
    V3 armAt(const FanStep& step, f64 label) const;
    /// The combed quarter turns from outgoing halfedge @p from to @p to,
    /// counter-clockwise round @p v.
    f64 turnsBetween(u32 v, u32 from, u32 to) const;
    /// The arm @p offset whole quarter turns past outgoing halfedge @p from.
    bool armPast(u32 v, u32 from, u32 offset, V3& direction) const;
    /// Arm @p k of a point with no layout edge, from its first ring edge.
    bool armAround(u32 v, u32 k, V3& direction) const;
    V3 chord(u32 v, u32 next) const;

    // --- sectors ---
    /// The outgoing layout halfedges at @p v counter-clockwise; on a border the
    /// first is the border's.
    void layoutOut(u32 v, std::vector<u32>& ring, std::vector<u32>& out, bool& closed) const;
    /// Measures @p v's sectors from the field: feature vertices before any
    /// trace touches them, and any layout vertex a sector of which is unknown.
    void measure(u32 v);
    void ensureSectors();
    /// The field's own quarter turns of each sector at @p v (`outs` from
    /// `layoutOut`), from the combed field between short chords.
    void fieldTurns(u32 v, const std::vector<u32>& ring, const std::vector<u32>& outs, bool closed,
                    std::vector<f64>& q) const;
    /// Notes, when verbose, how many points of each kind disagree with it.
    void auditSectors();

    // --- tracing ---
    u32 startVertexFor(const Emission& emission);
    /// The separatrices of singular point @p v. A point of index two or more
    /// would be a vertex of valence two or less, so it is passed straight
    /// through instead, two turns each side, and the patches round it make
    /// up the difference with points of their own.
    void singularArms(u32 v, std::vector<Emission>& emissions) const;
    void traceAll(const std::vector<Emission>& emissions);
    bool advance(Trace& trace, u32 id);
    /// Whether layout vertex @p w lies on a path running nearly along the
    /// trace's way (either sense), other than its own and a head-on meeting.
    bool runsAlongside(const Trace& trace, u32 id, u32 w) const;
    bool append(Trace& trace, u32 id, u32 vertex);
    u32 bestFanArm(u32 v, const V3& want, V3& arm, u32& face) const;
    bool castInFace(u32 face, const V3& origin, const V3& direction, u32 skipA, u32 skipB, u32& exitHalfedge,
                    f64& along, V3& at) const;
    void straighten(Trace& trace, u32 id);
    /// The shortest way along inside edges from @p from to a vertex of a
    /// settled path other than trace @p id's own, both ends included; empty
    /// when there is none.
    std::vector<u32> pathToLayout(u32 from, u32 id) const;
    /// The vertices now between @p a and @p b where a split cut the edge they
    /// shared; empty when none are found.
    std::vector<u32> bridge(u32 a, u32 b) const;
    void markTrace(u32 h, u32 pathId);
    /// Two traces that ran past each other on one field line, each stopping on
    /// the other, are one line: the later is cut back to where the earlier met
    /// it. Returns the traces whose ends are already settled.
    std::vector<u8> mergeHeadOn(const std::vector<u32>& ids);
    /// A path that ends on @p w coming from @p previous splits the sector it
    /// arrives in: one turn on its first side at least, and never none.
    void arrive(u32 w, u32 previous);
    /// The layout halfedge that begins the sector outgoing @p out lies in.
    u32 sectorStart(u32 w, u32 out) const;
    /// A trace that ends in a corner (a sector of one turn) is moved one edge,
    /// onto the flat point beside the corner on either of its paths, when its
    /// last vertex reaches it.
    bool retarget(Trace& trace);

    // --- patches ---
    void computePatches();
    bool repairRound();
    /// Cuts the straight spoke from inside vertex @p from to @p target on the
    /// border, both in map coordinates: every edge the segment crosses is
    /// split where it crosses. The work vertices from @p from to the landing,
    /// never through one in @p avoid; empty when the walk loses its way.
    std::vector<u32> cutSpoke(u32 from, const P2& target, MapCorners& corners,
                              const std::unordered_map<u32, u8>& avoid);
    /// `split` at parameter @p t along @p h, carrying @p corners onto the
    /// four triangles it leaves.
    u32 splitCarrying(u32 h, f64 t, MapCorners& corners);
    bool centreSplit(u32 patch);
    /// Cuts straight across the patch from the concave corner at loop
    /// @p position, sharing its turns between the two sides by angle.
    bool concaveCut(u32 patch, u32 position);
    /// Makes flat points of the border corners until there are four, or
    /// splits the longest border edge for one. False when it can do neither.
    bool forceCorners(u32 patch);
    void buildArcs();

    WorkMesh& mesh_;
    CrossField& field_;
    const Surface& surface_;
    LayoutOptions options_;
    Layout& out_;

    std::vector<u8> onLayout_;
    std::vector<u32> owner_;  ///< Per vertex: the trace that claimed it.
    std::vector<u8> pinned_;  ///< Per vertex: a path ends here, so none may skip it.
    std::vector<u32> arrivals_; ///< Per vertex: how many traces stopped on it.
    struct Join {
        u32 at;
        u32 first;  ///< Trace ids: their paths are read once straightened.
        u32 second;
    };
    std::vector<Join> joined_; ///< Straight joins made by `mergeHeadOn`, to set flat.
    std::vector<u8> sectors_; ///< Per halfedge starting a sector: its quarter turns.
    std::unordered_map<u32, u32> refinements_; ///< Per patch, by a triangle of it.
    std::unordered_map<u32, u32> concaveTries_; ///< Per concave corner: traces sent from it.
    std::vector<Trace> traces_;
    /// Emissions waiting to be traced, and the first trace of the batch: each
    /// names its sector by the halfedge it begins at, which a split renumbers.
    std::vector<Emission>* pending_ = nullptr;
    u32 batchStart_ = 0;
    u32 nextPathId_ = 0;
    u32 finishedCount_ = 0;
};

void Builder::grow() {
    const u32 vertices = mesh_.vertexCount();
    onLayout_.resize(vertices, 0);
    owner_.resize(vertices, kNone);
    pinned_.resize(vertices, 0);
    arrivals_.resize(vertices, 0);
    out_.turnsOverride.resize(vertices, 0);
    sectors_.resize(mesh_.corners.size(), 0);
    field_.index.resize(vertices, 0);
}

u32 Builder::split(u32 h, const V3& point) {
    const u32 twin = mesh_.twins[h];
    const u32 h0n = WorkMesh::Next(h);
    const u32 h1n = twin != kNone ? WorkMesh::Next(twin) : kNone;
    const u8 s0 = sectors_[h0n];
    const u8 s1 = h1n != kNone ? sectors_[h1n] : 0;
    const bool layout = mesh_.isLayout(h);
    const u32 before = mesh_.triangleCount();
    const u32 m = SplitWithField(mesh_, field_, surface_, h, point);
    grow();
    // `split` gives b->c the new halfedge 3 * before + 1 and a->d
    // 3 * (before + 1) + 1; their old slots now leave the new vertex.
    sectors_[3 * before + 1] = s0;
    sectors_[h0n] = 0;
    if (twin != kNone) {
        sectors_[3 * (before + 1) + 1] = s1;
        sectors_[h1n] = 0;
    }
    auto carry = [&](u32& sector) {
        if (sector == h0n) {
            sector = 3 * before + 1;
        } else if (twin != kNone && sector == h1n) {
            sector = 3 * (before + 1) + 1;
        }
    };
    for (u32 i = batchStart_; i < traces_.size(); ++i) {
        carry(traces_[i].emission.sector);
    }
    if (pending_ != nullptr) {
        for (Emission& emission : *pending_) {
            carry(emission.sector);
        }
    }
    if (layout) {
        onLayout_[m] = 1;
        sectors_[3 * before] = 2;
        if (twin != kNone) {
            sectors_[3 * (before + 1)] = 2;
        }
    }
    return m;
}

// ============================================================================
// The field round a vertex
// ============================================================================

void Builder::walkFan(u32 v, u32 fromOut, std::vector<FanStep>& steps) const {
    steps.clear();
    std::vector<u32> ring;
    const bool closed = mesh_.ring(v, ring);
    const std::size_t n = ring.size();
    std::size_t index = 0;
    while (index < n && ring[index] != fromOut) {
        ++index;
    }
    if (index == n) {
        return;
    }
    auto raw = [&](u32 face, u32 toward) {
        const V3 e = mesh_.positions[toward] - mesh_.positions[v];
        return (field_.angleOf(face, e) - field_.angles[face]) / kQuarter;
    };
    f64 offset = 0.0;
    for (std::size_t step = 0; step < n; ++step) {
        const std::size_t at = index + step;
        if (!closed && at >= n) {
            break;
        }
        const u32 o = ring[at % n];
        FanStep s;
        s.face = o / 3;
        s.out = o;
        s.offset = offset;
        s.nearLabel = raw(s.face, mesh_.to(o)) + offset;
        s.farLabel = raw(s.face, mesh_.to(WorkMesh::Next(o))) + offset;
        // A triangle's corner is under half a turn, so its far edge is less
        // than two quarter turns past its near one.
        while (s.farLabel < s.nearLabel) {
            s.farLabel += 4.0;
        }
        while (s.farLabel > s.nearLabel + 4.0) {
            s.farLabel -= 4.0;
        }
        steps.push_back(s);
        if (step + 1 == n || (!closed && at + 1 >= n)) {
            break;
        }
        const u32 nextFace = ring[(at + 1) % n] / 3;
        offset = std::round(s.farLabel - raw(nextFace, mesh_.to(WorkMesh::Next(o))));
    }
}

V3 Builder::armAt(const FanStep& step, f64 label) const {
    const f64 angle = field_.angles[step.face] + (label - step.offset) * kQuarter;
    return field_.axisX[step.face] * std::cos(angle) + field_.axisY[step.face] * std::sin(angle);
}

f64 Builder::turnsBetween(u32 v, u32 from, u32 to) const {
    std::vector<FanStep> steps;
    walkFan(v, from, steps);
    for (const FanStep& step : steps) {
        if (step.out == to) {
            return step.nearLabel - steps.front().nearLabel;
        }
    }
    // `to` was not met before a border: it closes the fan from the far side.
    return steps.empty() ? 0.0 : steps.back().farLabel - steps.front().nearLabel;
}

bool Builder::armPast(u32 v, u32 from, u32 offset, V3& direction) const {
    std::vector<FanStep> steps;
    walkFan(v, from, steps);
    if (steps.empty()) {
        return false;
    }
    // The arm nearest `offset` turns past the path's own direction, read from
    // its chord rather than its zig-zagging first edge.
    const V3 edge = mesh_.positions[mesh_.to(from)] - mesh_.positions[v];
    const f64 bend = std::clamp(SignedAngle(edge, chord(v, mesh_.to(from)), field_.normals[steps.front().face]),
                                -kQuarter * 0.5, kQuarter * 0.5);
    const f64 wanted = std::round(steps.front().nearLabel + bend / kQuarter + static_cast<f64>(offset));
    for (const FanStep& step : steps) {
        if (wanted >= step.nearLabel - 1e-9 && wanted <= step.farLabel + 1e-9) {
            direction = armAt(step, wanted);
            return true;
        }
    }
    return false;
}

bool Builder::armAround(u32 v, u32 k, V3& direction) const {
    std::vector<FanStep> steps;
    walkFan(v, mesh_.out[v], steps);
    if (steps.empty()) {
        return false;
    }
    const f64 wanted = std::ceil(steps.front().nearLabel - 1e-9) + static_cast<f64>(k);
    for (const FanStep& step : steps) {
        if (wanted >= step.nearLabel - 1e-9 && wanted <= step.farLabel + 1e-9) {
            direction = armAt(step, wanted);
            return true;
        }
    }
    return false;
}

V3 Builder::chord(u32 v, u32 next) const {
    // Follow the layout path through `next` while it runs on unbranched. Never
    // round a corner or a junction: the chord is a direction, not an average.
    std::vector<u32> ring;
    u32 previous = v;
    u32 current = next;
    f64 walked = Distance(mesh_.positions[v], mesh_.positions[next]);
    for (u32 step = 0; step < 64 && walked < options_.chord; ++step) {
        if (out_.turnsOverride[current] != 0 || mesh_.kinds[current] == VertexKind::Corner ||
            field_.index[current] != 0) {
            break;
        }
        const bool closed = mesh_.ring(current, ring);
        u32 found = kNone;
        u32 count = 0;
        auto consider = [&](u32 h, u32 other) {
            if (mesh_.isLayout(h)) {
                ++count;
                if (other != previous) {
                    found = other;
                }
            }
        };
        for (u32 o : ring) {
            consider(o, mesh_.to(o));
        }
        if (!closed && !ring.empty()) {
            const u32 in = WorkMesh::Prev(ring.back());
            consider(in, mesh_.from(in));
        }
        if (count != 2 || found == kNone) {
            break;
        }
        walked += Distance(mesh_.positions[current], mesh_.positions[found]);
        previous = current;
        current = found;
    }
    return mesh_.positions[current] - mesh_.positions[v];
}

// ============================================================================
// Sectors
// ============================================================================

void Builder::layoutOut(u32 v, std::vector<u32>& ring, std::vector<u32>& out, bool& closed) const {
    out.clear();
    closed = mesh_.ring(v, ring);
    for (u32 o : ring) {
        if (mesh_.isLayout(o)) {
            out.push_back(o);
        }
    }
}

void Builder::measure(u32 v) {
    std::vector<u32> ring;
    std::vector<u32> outs;
    bool closed = true;
    layoutOut(v, ring, outs, closed);
    if (outs.empty()) {
        return;
    }
    // The middle of one feature curve is flat by construction.
    if (mesh_.kinds[v] != VertexKind::Corner && field_.index[v] == 0 && out_.turnsOverride[v] == 0) {
        u32 other = kNone;
        if (!closed && !ring.empty() && outs.size() == 1) {
            other = WorkMesh::Prev(ring.back());
        } else if (closed && outs.size() == 2) {
            other = outs[1];
        }
        if (other != kNone && mesh_.curves[outs[0]] != kNone && mesh_.curves[outs[0]] == mesh_.curves[other]) {
            for (u32 o : outs) {
                sectors_[o] = 2;
            }
            return;
        }
    }
    // Each sector's turns from the combed field between its chords, then the
    // vertex's total shared by largest remainder, each at least one.
    std::vector<f64> q;
    fieldTurns(v, ring, outs, closed, q);
    f64 sum = 0.0;
    for (f64 value : q) {
        sum += value;
    }
    i32 total = 0;
    if (out_.turnsOverride[v] != 0) {
        total = out_.turnsOverride[v];
    } else if (closed) {
        total = field_.index[v] >= 2 ? 4 : 4 - field_.index[v];
    } else {
        total = std::max<i32>(1, static_cast<i32>(std::llround(sum)));
    }
    std::vector<i32> turns(outs.size(), 0);
    i32 left = total;
    std::vector<std::pair<f64, std::size_t>> remainders;
    for (std::size_t i = 0; i < outs.size(); ++i) {
        const f64 value = std::max(q[i], 0.0);
        turns[i] = std::max<i32>(1, static_cast<i32>(std::floor(value)));
        left -= turns[i];
        remainders.push_back({value - std::floor(value), i});
    }
    std::sort(remainders.begin(), remainders.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (std::size_t r = 0; left > 0 && !remainders.empty(); r = (r + 1) % remainders.size()) {
        turns[remainders[r].second] += 1;
        --left;
    }
    for (std::size_t i = 0; i < outs.size(); ++i) {
        sectors_[outs[i]] = static_cast<u8>(std::clamp(turns[i], 1, 255));
    }
}

void Builder::fieldTurns(u32 v, const std::vector<u32>& ring, const std::vector<u32>& outs, bool closed,
                         std::vector<f64>& q) const {
    q.assign(outs.size(), 0.0);
    for (std::size_t i = 0; i < outs.size(); ++i) {
        const u32 from = outs[i];
        std::vector<FanStep> steps;
        walkFan(v, from, steps);
        if (steps.empty()) {
            continue;
        }
        const u32 endVertex = i + 1 < outs.size() ? mesh_.to(outs[i + 1])
                                                  : (closed ? mesh_.to(outs[0]) : mesh_.from(WorkMesh::Prev(ring.back())));
        const V3 startEdge = mesh_.positions[mesh_.to(from)] - mesh_.positions[v];
        const f64 startBend = std::clamp(SignedAngle(startEdge, chord(v, mesh_.to(from)), field_.normals[steps.front().face]),
                                         -kQuarter * 0.5, kQuarter * 0.5);
        for (const FanStep& step : steps) {
            const u32 far = mesh_.to(WorkMesh::Next(step.out));
            if (far != endVertex) {
                continue;
            }
            const V3 farEdge = mesh_.positions[far] - mesh_.positions[v];
            const f64 endBend = std::clamp(SignedAngle(farEdge, chord(v, far), field_.normals[step.face]),
                                           -kQuarter * 0.5, kQuarter * 0.5);
            q[i] = step.farLabel - steps.front().nearLabel + (endBend - startBend) / kQuarter;
            break;
        }
    }
}

void Builder::auditSectors() {
    // Where the turns the layout gave a sector and the field's own disagree,
    // by what kind of point it is: a patch with no singular point inside
    // turns four quarters round its border, so each such sector is a patch
    // with a corner too many or too few.
    std::map<std::string, std::pair<u32, u32>> tally; // kind -> (points, points off)
    std::vector<u32> ring;
    std::vector<u32> outs;
    std::vector<f64> q;
    for (u32 v = 0; v < mesh_.vertexCount(); ++v) {
        if (!mesh_.alive(v) || !isLayoutVertex(v)) {
            continue;
        }
        bool closed = true;
        layoutOut(v, ring, outs, closed);
        if (outs.empty()) {
            continue;
        }
        bool feature = false;
        for (u32 o : outs) {
            feature = feature || mesh_.isFeature(o);
        }
        std::string kind = field_.index[v] != 0      ? "singular"
                           : out_.turnsOverride[v] != 0 ? "forced"
                           : feature                     ? (mesh_.kinds[v] == VertexKind::Corner ? "feature corner" : "feature")
                           : outs.size() == 2            ? "path middle"
                           : outs.size() == 3            ? "T"
                           : outs.size() == 4            ? "X"
                                                         : "other";
        fieldTurns(v, ring, outs, closed, q);
        bool off = false;
        for (std::size_t i = 0; i < outs.size(); ++i) {
            off = off || std::llround(q[i]) != static_cast<i64>(sectors_[outs[i]]);
        }
        auto& [points, wrong] = tally[kind];
        ++points;
        wrong += off ? 1 : 0;
        if (off && wrong <= 6 && (kind == "T" || kind == "singular")) {
            std::string text = "audit " + kind + " at " + std::to_string(v) + ": field";
            char number[32];
            for (std::size_t i = 0; i < outs.size(); ++i) {
                std::snprintf(number, sizeof(number), " %.2f", q[i]);
                text += number;
            }
            text += " given";
            for (u32 o : outs) {
                text += " " + std::to_string(sectors_[o]);
            }
            note(text);
        }
    }
    for (const auto& [kind, counts] : tally) {
        note("audit " + kind + ": " + std::to_string(counts.second) + " of " + std::to_string(counts.first) +
             " points off the field");
    }
}

void Builder::ensureSectors() {
    std::vector<u32> ring;
    std::vector<u32> outs;
    for (u32 v = 0; v < mesh_.vertexCount(); ++v) {
        if (!mesh_.alive(v) || !isLayoutVertex(v)) {
            continue;
        }
        bool closed = true;
        layoutOut(v, ring, outs, closed);
        bool known = !outs.empty();
        for (u32 o : outs) {
            known = known && sectors_[o] != 0;
        }
        if (!known && !outs.empty()) {
            measure(v);
        }
    }
}

u32 Builder::sectorStart(u32 w, u32 out) const {
    std::vector<u32> ring;
    const bool closed = mesh_.ring(w, ring);
    const std::size_t n = ring.size();
    std::size_t index = 0;
    while (index < n && ring[index] != out) {
        ++index;
    }
    if (index == n) {
        return kNone;
    }
    for (std::size_t back = 1; back <= n; ++back) {
        if (!closed && back > index) {
            break;
        }
        const u32 o = ring[(index + n - back) % n];
        if (mesh_.isLayout(o) && o != out) {
            return o;
        }
    }
    return kNone;
}

bool Builder::retarget(Trace& trace) {
    std::vector<u32>& path = trace.path;
    const u32 w = path.back();
    const u32 p = path[path.size() - 2];
    const u32 out = mesh_.find(w, p);
    if (out == kNone) {
        return false;
    }
    const u32 start = sectorStart(w, out);
    if (start == kNone || sectors_[start] >= 2) {
        return false;
    }
    // The sector's two paths: the one it begins at, and the next one round.
    std::vector<u32> ring;
    mesh_.ring(w, ring);
    u32 next = kNone;
    for (std::size_t i = 0; i < ring.size(); ++i) {
        if (ring[i] == out) {
            for (std::size_t k = 1; k < ring.size(); ++k) {
                const u32 o = ring[(i + k) % ring.size()];
                if (mesh_.isLayout(o)) {
                    next = o;
                    break;
                }
            }
        }
    }
    for (u32 candidate : {start, next}) {
        if (candidate == kNone) {
            continue;
        }
        const u32 x = mesh_.to(candidate);
        if (x == p || mesh_.kinds[x] == VertexKind::Corner || out_.turnsOverride[x] != 0 || field_.index[x] != 0) {
            continue;
        }
        u32 edge = mesh_.find(p, x);
        if (edge == kNone) {
            edge = mesh_.find(x, p);
        }
        if (edge == kNone || mesh_.isLayout(edge)) {
            continue;
        }
        // x must be the middle of one path: two layout edges, both flat.
        std::vector<u32> outs;
        std::vector<u32> xring;
        bool closed = true;
        layoutOut(x, xring, outs, closed);
        if (!closed || outs.size() != 2 || sectors_[outs[0]] != 2 || sectors_[outs[1]] != 2) {
            continue;
        }
        u32 old = mesh_.find(p, w);
        if (old == kNone) {
            old = mesh_.find(w, p);
        }
        // p's sector toward the end, a middle's flat one or a start's own.
        const u8 carried = mesh_.find(p, w) != kNone ? sectors_[mesh_.find(p, w)] : 0;
        const u32 pathId = mesh_.curves[old] >= kTracePathBase ? mesh_.curves[old] - kTracePathBase : nextPathId_++;
        for (u32 h : {old, mesh_.twins[old]}) {
            if (h != kNone && !mesh_.isFeature(h)) {
                mesh_.features[h] &= static_cast<u8>(~kLayoutTrace);
                mesh_.curves[h] = kNone;
                sectors_[h] = 0;
            }
        }
        markTrace(edge, pathId);
        path.back() = x;
        pinned_[x] = 1;
        if (arrivals_[w] > 0) {
            --arrivals_[w];
        }
        ++arrivals_[x];
        const u32 back = mesh_.find(p, x);
        if (back != kNone) {
            sectors_[back] = carried != 0 ? carried : 2;
        }
        note("moved an arrival off a corner at vertex " + std::to_string(w));
        return true;
    }
    return false;
}

void Builder::arrive(u32 w, u32 previous) {
    u32 out = mesh_.find(w, previous);
    if (out == kNone) {
        return;
    }
    // The arriving edge is itself a layout edge now; the sector it lies in
    // begins at the layout edge before it.
    const u32 start = sectorStart(w, out);
    if (start == kNone) {
        // The only layout edge here: the whole turn is its sector.
        std::vector<u32> ring;
        const bool closed = mesh_.ring(w, ring);
        if (sectors_[out] == 0) {
            const i32 index = field_.index[w];
            sectors_[out] = static_cast<u8>(closed ? (index >= 2 ? 4 : 4 - index) : 2);
        }
        return;
    }
    const i32 k = sectors_[start];
    if (k >= 2) {
        const f64 measured = turnsBetween(w, start, out);
        const i32 j = std::clamp<i32>(static_cast<i32>(std::llround(measured)), 1, k - 1);
        sectors_[start] = static_cast<u8>(j);
        sectors_[out] = static_cast<u8>(k - j);
    } else {
        // Into a corner: the point gains a turn and both halves are corners.
        sectors_[start] = 1;
        sectors_[out] = 1;
        note("arrival into a corner at vertex " + std::to_string(w));
    }
}

// ============================================================================
// Tracing
// ============================================================================

bool Builder::castInFace(u32 face, const V3& origin, const V3& direction, u32 skipA, u32 skipB,
                         u32& exitHalfedge, f64& along, V3& at) const {
    const V3& x = field_.axisX[face];
    const V3& y = field_.axisY[face];
    const f64 dx = Dot(direction, x);
    const f64 dy = Dot(direction, y);
    f64 best = std::numeric_limits<f64>::infinity();
    exitHalfedge = kNone;
    for (u32 i = 0; i < 3; ++i) {
        const u32 h = 3 * face + i;
        if (h == skipA || h == skipB) {
            continue;
        }
        const V3 a = mesh_.positions[mesh_.from(h)] - origin;
        const V3 b = mesh_.positions[mesh_.to(h)] - origin;
        const f64 ax = Dot(a, x);
        const f64 ay = Dot(a, y);
        const f64 ex = Dot(b, x) - ax;
        const f64 ey = Dot(b, y) - ay;
        const f64 det = dx * (-ey) - dy * (-ex);
        if (std::abs(det) < 1e-18) {
            continue;
        }
        const f64 t = (ax * (-ey) - ay * (-ex)) / det;
        const f64 s = (dx * ay - dy * ax) / det;
        if (t <= 1e-12 || s < -1e-6 || s > 1.0 + 1e-6) {
            continue;
        }
        if (t < best) {
            best = t;
            exitHalfedge = h;
            along = std::clamp(s, 0.0, 1.0);
        }
    }
    if (exitHalfedge == kNone) {
        return false;
    }
    at = Lerp(mesh_.positions[mesh_.from(exitHalfedge)], mesh_.positions[mesh_.to(exitHalfedge)], along);
    return true;
}

u32 Builder::bestFanArm(u32 v, const V3& want, V3& arm, u32& face) const {
    // The field arm leaving v that is nearest the wanted direction, among the
    // arms that point into the triangle they belong to.
    std::vector<u32> ring;
    mesh_.ring(v, ring);
    f64 best = std::numeric_limits<f64>::infinity();
    u32 found = kNone;
    for (u32 o : ring) {
        const u32 t = o / 3;
        const V3 e1 = mesh_.positions[mesh_.to(o)] - mesh_.positions[v];
        const V3 e2 = mesh_.positions[mesh_.to(WorkMesh::Next(o))] - mesh_.positions[v];
        const V3& n = field_.normals[t];
        for (u32 k = 0; k < 4; ++k) {
            const V3 d = field_.direction(t, k);
            if (Dot(Cross(e1, d), n) < 0.0 || Dot(Cross(d, e2), n) < 0.0) {
                continue;
            }
            const f64 deviation = Angle(d, Tangent(want, n));
            if (deviation < best) {
                best = deviation;
                found = o;
                arm = d;
                face = t;
            }
        }
    }
    return found;
}

bool Builder::append(Trace& trace, u32 id, u32 vertex) {
    std::vector<u32>& path = trace.path;
    if (vertex == path.back()) {
        return true;
    }
    if (path.size() >= 2 && vertex == path[path.size() - 2]) {
        // The snap stepped back where the streamline doubled round a vertex.
        const u32 dropped = path.back();
        if (arrivals_[dropped] > 0) {
            // Another trace stopped on it: it stays, and this trace ends there.
            trace.done = true;
            trace.finished = finishedCount_++;
            return false;
        }
        if (owner_[dropped] == id) {
            owner_[dropped] = kNone;
            onLayout_[dropped] = 0;
        }
        path.pop_back();
        if (path.size() == 1) {
            trace.done = true; // it turned straight back: no trace
            trace.finished = finishedCount_++;
        }
        return !trace.done;
    }
    if (isLayoutVertex(vertex)) {
        path.push_back(vertex);
        pinned_[vertex] = 1;
        ++arrivals_[vertex];
        trace.done = true;
        trace.collided = true;
        trace.finished = finishedCount_++;
        return false;
    }
    path.push_back(vertex);
    onLayout_[vertex] = 1;
    owner_[vertex] = id;
    return true;
}

bool Builder::runsAlongside(const Trace& trace, u32 id, u32 w) const {
    if (owner_[w] == id) {
        return false; // its own path: a loop closing
    }
    std::vector<u32> ring;
    const bool closed = mesh_.ring(w, ring);
    // Both run in the field, so they meet square or alongside, and 45 degrees
    // is the line between; the path's way is its chord, not its first edge.
    auto check = [&](u32 other) {
        const V3 along = Unit(chord(w, other));
        return std::abs(Dot(along, trace.direction)) > 0.7;
    };
    bool parallel = false;
    bool edges = false;
    for (u32 o : ring) {
        if (mesh_.isLayout(o)) {
            edges = true;
            parallel = parallel || check(mesh_.to(o));
        }
    }
    if (!closed && !ring.empty()) {
        const u32 in = WorkMesh::Prev(ring.back());
        if (mesh_.isLayout(in)) {
            edges = true;
            parallel = parallel || check(mesh_.from(in));
        }
    }
    if (edges || owner_[w] == kNone) {
        return parallel;
    }
    // A path of this batch has no layout edges yet: its own course, a few
    // points each way, is its way. Running the same way is passed; head-on
    // is a meeting, which the merge makes one line.
    const std::vector<u32>& path = traces_[owner_[w]].path;
    const auto at = std::find(path.begin(), path.end(), w);
    if (at == path.end()) {
        return false;
    }
    const std::size_t i = static_cast<std::size_t>(at - path.begin());
    const std::size_t lo = i >= 3 ? i - 3 : 0;
    const std::size_t hi = std::min(path.size() - 1, i + 3);
    return hi > lo && Dot(Unit(mesh_.positions[path[hi]] - mesh_.positions[path[lo]]), trace.direction) > 0.7;
}

bool Builder::advance(Trace& trace, u32 id) {
    if (++trace.steps > 4 * mesh_.triangleCount() + 64) {
        trace.done = true;
        trace.finished = finishedCount_++;
        return false;
    }
    u32 face = kNone;
    V3 arm{0.0, 0.0, 0.0};
    V3 origin{0.0, 0.0, 0.0};
    u32 skipA = kNone;
    u32 skipB = kNone;
    if (trace.atVertex != kNone) {
        const u32 out = bestFanArm(trace.atVertex, trace.direction, arm, face);
        if (out == kNone) {
            trace.done = true;
            trace.finished = finishedCount_++;
            return false;
        }
        origin = mesh_.positions[trace.atVertex];
        skipA = out;
        skipB = WorkMesh::Prev(out);
    } else {
        face = trace.entry / 3;
        const V3& n = field_.normals[face];
        const V3 edge = mesh_.positions[mesh_.to(trace.entry)] - mesh_.positions[mesh_.from(trace.entry)];
        const V3 inward = Cross(n, edge);
        f64 best = -2.0;
        for (u32 k = 0; k < 4; ++k) {
            const V3 d = field_.direction(face, k);
            if (Dot(d, inward) <= 1e-9 * Length(inward)) {
                continue;
            }
            const f64 score = Dot(d, trace.direction);
            if (score > best) {
                best = score;
                arm = d;
            }
        }
        if (best <= -2.0) {
            trace.done = true;
            trace.finished = finishedCount_++;
            return false;
        }
        origin = trace.point;
        skipA = trace.entry;
    }
    u32 exit = kNone;
    f64 along = 0.0;
    V3 at{0.0, 0.0, 0.0};
    if (!castInFace(face, origin, arm, skipA, skipB, exit, along, at)) {
        trace.done = true;
        trace.finished = finishedCount_++;
        return false;
    }
    trace.length += Distance(origin, at);
    trace.direction = arm;
    constexpr f64 kVertexSnap = 0.02;
    u32 snapped = along < 0.5 ? mesh_.from(exit) : mesh_.to(exit);
    const u32 other = along < 0.5 ? mesh_.to(exit) : mesh_.from(exit);
    // A path running alongside is not one to stop on: snap to the edge's other
    // end and keep the two apart. A meeting at a shallow angle would read as a
    // right-angled junction where there is none.
    bool sidestepped = false;
    if (isLayoutVertex(snapped) && !isLayoutVertex(other) && runsAlongside(trace, id, snapped)) {
        snapped = other;
        sidestepped = true;
    }
    if (!sidestepped && (along < kVertexSnap || along > 1.0 - kVertexSnap)) {
        trace.atVertex = snapped;
        trace.entry = kNone;
        trace.point = mesh_.positions[snapped];
    } else {
        const u32 twin = mesh_.twins[exit];
        if (twin == kNone) {
            append(trace, id, snapped);
            if (!trace.done) {
                trace.done = true;
                trace.finished = finishedCount_++;
            }
            return false;
        }
        trace.atVertex = kNone;
        trace.entry = twin;
        trace.point = at;
        trace.direction = Unit(Tangent(arm, field_.normals[twin / 3]));
    }
    return append(trace, id, snapped);
}

void Builder::singularArms(u32 v, std::vector<Emission>& emissions) const {
    const i32 index = field_.index[v];
    const u32 arms = static_cast<u32>(std::max(1, 4 - index));
    const u32 spacing = arms < 3 ? 4 / arms : 1;
    for (u32 k = 0; k < arms; ++k) {
        V3 d{0.0, 0.0, 0.0};
        if (armAround(v, k, d)) {
            emissions.push_back({v, d, kNone, k * spacing, arms * spacing});
        }
    }
}

u32 Builder::startVertexFor(const Emission& emission) {
    // A trace's first edge is made to lie on its direction: the edge it would
    // cross first is split where it crosses, unless it passes near an end.
    const u32 v = emission.vertex;
    V3 arm{0.0, 0.0, 0.0};
    u32 face = kNone;
    const u32 out = bestFanArm(v, emission.direction, arm, face);
    if (out == kNone) {
        return kNone;
    }
    u32 exit = kNone;
    f64 along = 0.0;
    V3 at{0.0, 0.0, 0.0};
    if (!castInFace(face, mesh_.positions[v], arm, out, WorkMesh::Prev(out), exit, along, at)) {
        return kNone;
    }
    if (along < 0.2 && !isLayoutVertex(mesh_.from(exit))) {
        return mesh_.from(exit);
    }
    if (along > 0.8 && !isLayoutVertex(mesh_.to(exit))) {
        return mesh_.to(exit);
    }
    return split(exit, at);
}

std::vector<u32> Builder::pathToLayout(u32 from, u32 id) const {
    // Dijkstra over the inside edges from @p from to the nearest layout
    // vertex that is not the trace's own.
    using Entry = std::pair<f64, u32>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> open;
    std::unordered_map<u32, f64> distance;
    std::unordered_map<u32, u32> previous;
    distance[from] = 0.0;
    open.push({0.0, from});
    std::vector<u32> ring;
    for (u32 visited = 0; !open.empty() && visited < 20000; ++visited) {
        const auto [d, v] = open.top();
        open.pop();
        if (d > distance[v]) {
            continue;
        }
        const u32 owner = owner_[v];
        const bool settled = owner == kNone || traces_[owner].collided;
        if (v != from && isLayoutVertex(v) && owner != id && settled && v != traces_[id].path.front()) {
            std::vector<u32> path{v};
            for (u32 at = v; at != from;) {
                at = previous[at];
                path.push_back(at);
            }
            std::reverse(path.begin(), path.end());
            return path;
        }
        if (v != from && isLayoutVertex(v)) {
            continue; // its own path: not through it
        }
        const bool closed = mesh_.ring(v, ring);
        std::vector<std::pair<u32, u32>> steps; // (halfedge, neighbour)
        for (u32 o : ring) {
            steps.push_back({o, mesh_.to(o)});
        }
        if (!closed && !ring.empty()) {
            const u32 in = WorkMesh::Prev(ring.back());
            steps.push_back({in, mesh_.from(in)});
        }
        for (const auto& [h, w] : steps) {
            if (mesh_.isLayout(h)) {
                continue;
            }
            const f64 next = d + Distance(mesh_.positions[v], mesh_.positions[w]);
            const auto it = distance.find(w);
            if (it == distance.end() || next < it->second) {
                distance[w] = next;
                previous[w] = v;
                open.push({next, w});
            }
        }
    }
    return {};
}

std::vector<u32> Builder::bridge(u32 a, u32 b) const {
    // The vertices a split left on the edge a, b once was: the shortest way
    // between them through points near that segment.
    const V3& pa = mesh_.positions[a];
    const V3& pb = mesh_.positions[b];
    const f64 reach = 0.25 * Distance(pa, pb) + 1e-12;
    using Entry = std::pair<f64, u32>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> open;
    std::unordered_map<u32, f64> distance;
    std::unordered_map<u32, u32> previous;
    distance[a] = 0.0;
    open.push({0.0, a});
    std::vector<u32> ring;
    for (u32 visited = 0; !open.empty() && visited < 64; ++visited) {
        const auto [d, v] = open.top();
        open.pop();
        if (d > distance[v]) {
            continue;
        }
        if (v == b) {
            std::vector<u32> middle;
            for (u32 at = previous[b]; at != a; at = previous[at]) {
                middle.push_back(at);
            }
            std::reverse(middle.begin(), middle.end());
            return middle;
        }
        mesh_.ring(v, ring);
        std::vector<u32> neighbours;
        for (u32 o : ring) {
            neighbours.push_back(mesh_.to(o));
            neighbours.push_back(mesh_.from(WorkMesh::Prev(o)));
        }
        for (u32 w : neighbours) {
            if (w != b && Distance(mesh_.positions[w], Lerp(pa, pb, NearestOnSegment(mesh_.positions[w], pa, pb))) > reach) {
                continue;
            }
            const f64 next = d + Distance(mesh_.positions[v], mesh_.positions[w]);
            const auto it = distance.find(w);
            if (it == distance.end() || next < it->second) {
                distance[w] = next;
                previous[w] = v;
                open.push({next, w});
            }
        }
    }
    return {};
}

void Builder::traceAll(const std::vector<Emission>& emissions) {
    ensureSectors();
    // Emissions into one sector of one vertex must start in their own order,
    // so each finds the fan as the one before left it.
    std::vector<Emission> pending = emissions;
    std::vector<Emission>* const outer = pending_;
    pending_ = &pending;
    batchStart_ = static_cast<u32>(traces_.size());
    std::vector<u32> ids;
    for (std::size_t e = 0; e < pending.size(); ++e) {
        const u32 first = startVertexFor(pending[e]);
        const Emission emission = pending[e];
        if (first == kNone) {
            note("an emission found no first edge at vertex " + std::to_string(emission.vertex));
            continue;
        }
        Trace trace;
        trace.emission = emission;
        trace.path = {emission.vertex};
        const u32 id = static_cast<u32>(traces_.size());
        traces_.push_back(trace);
        Trace& t = traces_.back();
        t.atVertex = first;
        t.point = mesh_.positions[first];
        t.direction = Unit(mesh_.positions[first] - mesh_.positions[emission.vertex]);
        t.length = Distance(mesh_.positions[first], mesh_.positions[emission.vertex]);
        onLayout_[emission.vertex] = 1;
        pinned_[emission.vertex] = 1;
        append(t, id, first);
        ids.push_back(id);
    }
    using Entry = std::pair<f64, u32>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> open;
    for (u32 id : ids) {
        if (!traces_[id].done) {
            open.push({traces_[id].length, id});
        }
    }
    while (!open.empty()) {
        const u32 id = open.top().second;
        open.pop();
        Trace& trace = traces_[id];
        if (trace.done) {
            continue;
        }
        if (advance(trace, id) && !trace.done) {
            open.push({trace.length, id});
        }
    }

    const std::vector<u8> settled = mergeHeadOn(ids);
    // A trace that stopped short of every path would leave a loose end in its
    // patch: it is carried on along edges to the nearest one, or dropped.
    // Only onto a path that is settled itself, so passes run till none moves;
    // and a trace dropped takes away points others stopped on, which then
    // stopped short themselves, so the whole runs till none is dropped.
    for (bool again = true; again;) {
        again = false;
        for (bool moved = true; moved;) {
            moved = false;
            for (u32 id : ids) {
                Trace& trace = traces_[id];
                if (trace.collided || trace.path.size() < 2 || settled[id]) {
                    continue;
                }
                const std::vector<u32> rest = pathToLayout(trace.path.back(), id);
                if (rest.size() >= 2) {
                    for (std::size_t k = 1; k < rest.size(); ++k) {
                        append(trace, id, rest[k]);
                    }
                    note("carried a trace that stopped short on to vertex " + std::to_string(trace.path.back()));
                    moved = true;
                }
            }
        }
        for (u32 id : ids) {
            Trace& trace = traces_[id];
            if (trace.collided || trace.path.size() < 2 || settled[id]) {
                continue;
            }
            for (std::size_t k = 1; k < trace.path.size(); ++k) {
                const u32 v = trace.path[k];
                if (owner_[v] != id) {
                    continue;
                }
                owner_[v] = kNone;
                onLayout_[v] = 0;
                for (u32 other : ids) {
                    Trace& stopped = traces_[other];
                    if (other != id && stopped.collided && !settled[other] && stopped.path.size() >= 2 &&
                        stopped.path.back() == v) {
                        // It stopped short now, on a point of its own.
                        stopped.collided = false;
                        owner_[v] = other;
                        onLayout_[v] = 1;
                        arrivals_[v] -= arrivals_[v] > 0 ? 1 : 0;
                        again = true;
                    }
                }
            }
            trace.path.resize(1);
            note("dropped a trace that stopped short of every path");
        }
    }
    // A fresh point whose every trace was dropped, and that none stopped on,
    // is no layout point: a later round sees it again.
    std::map<u32, u8> kept;
    for (u32 id : ids) {
        const Emission& emission = traces_[id].emission;
        if (emission.arms != 0) {
            kept[emission.vertex] |= traces_[id].path.size() >= 2 ? 1 : 0;
        }
    }
    for (const auto& [v, any] : kept) {
        if (!any && arrivals_[v] == 0) {
            onLayout_[v] = 0;
            pinned_[v] = 0;
        }
    }
    // Commit in three passes, so every sector is set from what made it: the
    // paths and their flat middles, then each start's sectors, then each end
    // where it arrived, in the order the traces stopped.
    for (u32 id : ids) {
        Trace& trace = traces_[id];
        if (trace.path.size() < 2) {
            continue;
        }
        straighten(trace, id);
        // A later start's split may have cut an edge of the path: the
        // vertices it left there join the path.
        for (std::size_t i = 1; i < trace.path.size(); ++i) {
            const u32 a = trace.path[i - 1];
            const u32 b = trace.path[i];
            if (mesh_.find(a, b) != kNone || mesh_.find(b, a) != kNone) {
                continue;
            }
            const std::vector<u32> middle = bridge(a, b);
            if (middle.empty()) {
                note("a trace stepped between two vertices with no edge");
                continue;
            }
            for (u32 m : middle) {
                onLayout_[m] = 1;
                owner_[m] = id;
            }
            trace.path.insert(trace.path.begin() + static_cast<std::ptrdiff_t>(i), middle.begin(), middle.end());
            i += middle.size();
        }
        const u32 pathId = nextPathId_++;
        for (std::size_t i = 1; i < trace.path.size(); ++i) {
            const u32 a = trace.path[i - 1];
            const u32 b = trace.path[i];
            u32 h = mesh_.find(a, b);
            if (h == kNone) {
                h = mesh_.find(b, a);
            }
            if (h == kNone) {
                continue;
            }
            markTrace(h, pathId);
        }
        for (std::size_t i = 1; i + 1 < trace.path.size(); ++i) {
            const u32 v = trace.path[i];
            const u32 forward = mesh_.find(v, trace.path[i + 1]);
            const u32 backward = mesh_.find(v, trace.path[i - 1]);
            if (forward != kNone) {
                sectors_[forward] = 2;
            }
            if (backward != kNone) {
                sectors_[backward] = 2;
            }
        }
    }
    // Starts: a fresh point's arms a quarter turn apart in arm order; a sector
    // emission's paths cut its sector at their offsets.
    std::map<u32, std::vector<u32>> byVertex;
    for (u32 id : ids) {
        if (traces_[id].path.size() >= 2) {
            byVertex[traces_[id].emission.vertex].push_back(id);
        }
    }
    for (auto& [v, list] : byVertex) {
        if (traces_[list.front()].emission.arms != 0) {
            const u32 arms = traces_[list.front()].emission.arms;
            std::sort(list.begin(), list.end(),
                      [&](u32 a, u32 b) { return traces_[a].emission.offset < traces_[b].emission.offset; });
            for (std::size_t i = 0; i < list.size(); ++i) {
                const Trace& t = traces_[list[i]];
                const u32 out = mesh_.find(v, t.path[1]);
                const u32 nextArm = i + 1 < list.size() ? traces_[list[i + 1]].emission.offset
                                                        : traces_[list.front()].emission.offset + arms;
                if (out != kNone) {
                    sectors_[out] = static_cast<u8>(nextArm - t.emission.offset);
                }
            }
            continue;
        }
        std::map<u32, std::vector<u32>> bySector;
        for (u32 id : list) {
            bySector[traces_[id].emission.sector].push_back(id);
        }
        for (auto& [sector, members] : bySector) {
            std::sort(members.begin(), members.end(),
                      [&](u32 a, u32 b) { return traces_[a].emission.offset < traces_[b].emission.offset; });
            const i32 k = sectors_[sector];
            i32 previous = 0;
            u32 previousOut = sector;
            for (u32 id : members) {
                const Trace& t = traces_[id];
                const u32 out = mesh_.find(v, t.path[1]);
                const i32 j = std::clamp<i32>(static_cast<i32>(t.emission.offset), previous + 1, std::max(previous + 1, k - 1));
                if (out == kNone) {
                    continue;
                }
                sectors_[previousOut] = static_cast<u8>(std::max(1, j - previous));
                previousOut = out;
                previous = j;
            }
            sectors_[previousOut] = static_cast<u8>(std::max(1, k - previous));
        }
    }
    // Ends.
    std::vector<u32> ended;
    for (u32 id : ids) {
        if (traces_[id].collided && traces_[id].path.size() >= 2 && !settled[id]) {
            ended.push_back(id);
        }
    }
    std::sort(ended.begin(), ended.end(), [&](u32 a, u32 b) { return traces_[a].finished < traces_[b].finished; });
    for (u32 id : ended) {
        Trace& t = traces_[id];
        retarget(t);
        arrive(t.path.back(), t.path[t.path.size() - 2]);
    }
    for (const Join& join : joined_) {
        for (u32 id : {join.first, join.second}) {
            const std::vector<u32>& path = traces_[id].path;
            if (path.size() < 2) {
                continue;
            }
            const u32 out = mesh_.find(join.at, path[path.size() - 2]);
            if (out != kNone) {
                sectors_[out] = 2;
            }
        }
    }
    joined_.clear();
    pending_ = outer;
}

std::vector<u8> Builder::mergeHeadOn(const std::vector<u32>& ids) {
    std::vector<u8> settled(traces_.size(), 0);
    std::vector<u32> candidates;
    for (u32 a : ids) {
        Trace& first = traces_[a];
        if (!first.collided || first.path.size() < 2) {
            continue;
        }
        const u32 x = first.path.back();
        // x's trace, or at a singular point, which no trace claims, any of
        // the traces that began there.
        candidates.clear();
        if (owner_[x] != kNone) {
            candidates.push_back(owner_[x]);
        } else {
            for (u32 id : ids) {
                if (id != a && traces_[id].path.size() >= 2 && traces_[id].path.front() == x) {
                    candidates.push_back(id);
                }
            }
        }
        for (const u32 other : candidates) {
            if (other == a || settled[other] || settled[a]) {
                continue;
            }
            Trace& second = traces_[other];
            if (!second.collided || second.path.size() < 2 || second.finished < first.finished) {
                continue;
            }
            // The second must have stopped on the first's path.
            const u32 y = second.path.back();
            if (std::find(first.path.begin(), first.path.end(), y) == first.path.end()) {
                continue;
            }
            const auto at = std::find(second.path.begin(), second.path.end(), x);
            if (at == second.path.end()) {
                continue;
            }
            const std::size_t cut = static_cast<std::size_t>(at - second.path.begin());
            // Only the two of them may have stopped on the tail being dropped.
            bool alone = true;
            for (std::size_t k = cut + 1; k + 1 < second.path.size(); ++k) {
                alone = alone && arrivals_[second.path[k]] == 0;
            }
            if (!alone) {
                continue;
            }
            for (std::size_t k = cut + 1; k < second.path.size(); ++k) {
                const u32 v = second.path[k];
                if (owner_[v] == other) {
                    owner_[v] = kNone;
                    onLayout_[v] = 0;
                }
            }
            if (arrivals_[y] > 0) {
                --arrivals_[y];
            }
            if (arrivals_[y] == 0 && y != first.path.front()) {
                pinned_[y] = 0;
            }
            second.path.resize(cut + 1);
            if (cut == 0) {
                // It met the first at its own start: the first is the
                // connection, and arrives there like any other.
                second.path.clear();
                second.collided = false;
                settled[other] = 1;
                note("merged two separatrices into one connection at vertex " + std::to_string(x));
                break;
            }
            // Both now end at x, one line straight through it.
            settled[a] = 1;
            settled[other] = 1;
            joined_.push_back({x, a, other});
            note("merged two traces that ran past each other at vertex " + std::to_string(x));
            break;
        }
    }
    return settled;
}

void Builder::straighten(Trace& trace, u32 id) {
    // A vertex the path only visits to come back beside where it was (its
    // neighbours share an edge) is skipped, unless a path ends there.
    std::vector<u32>& path = trace.path;
    for (bool again = true; again;) {
        again = false;
        for (std::size_t i = 1; i + 1 < path.size(); ++i) {
            const u32 a = path[i - 1];
            const u32 b = path[i];
            const u32 c = path[i + 1];
            if (pinned_[b]) {
                continue;
            }
            u32 h = mesh_.find(a, c);
            if (h == kNone) {
                h = mesh_.find(c, a);
            }
            if (h == kNone || mesh_.isLayout(h)) {
                continue;
            }
            if (owner_[b] == id) {
                owner_[b] = kNone;
                onLayout_[b] = 0;
            }
            path.erase(path.begin() + static_cast<std::ptrdiff_t>(i));
            again = true;
            break;
        }
    }
}

void Builder::markTrace(u32 h, u32 pathId) {
    // A trace edge's `curves` names its path, so a split's halves keep it.
    if (mesh_.isFeature(h)) {
        return;
    }
    mesh_.features[h] |= kLayoutTrace;
    mesh_.curves[h] = kTracePathBase + pathId;
    if (mesh_.twins[h] != kNone) {
        mesh_.features[mesh_.twins[h]] |= kLayoutTrace;
        mesh_.curves[mesh_.twins[h]] = kTracePathBase + pathId;
    }
}

// ============================================================================
// Patches
// ============================================================================

void Builder::computePatches() {
    const u32 triangles = mesh_.triangleCount();
    out_.patchOf.assign(triangles, kNone);
    out_.patches.clear();
    std::vector<u32> stack;
    for (u32 seed = 0; seed < triangles; ++seed) {
        if (mesh_.dead[seed] || out_.patchOf[seed] != kNone) {
            continue;
        }
        const u32 id = static_cast<u32>(out_.patches.size());
        out_.patches.emplace_back();
        LayoutPatch& patch = out_.patches.back();
        out_.patchOf[seed] = id;
        stack.push_back(seed);
        while (!stack.empty()) {
            const u32 t = stack.back();
            stack.pop_back();
            patch.triangles.push_back(t);
            for (u32 i = 0; i < 3; ++i) {
                const u32 h = 3 * t + i;
                const u32 twin = mesh_.twins[h];
                if (twin == kNone || mesh_.isLayout(h) || out_.patchOf[twin / 3] != kNone) {
                    continue;
                }
                out_.patchOf[twin / 3] = id;
                stack.push_back(twin / 3);
            }
        }
    }
    // Border loops and Euler characteristics.
    std::vector<u8> seen(mesh_.corners.size(), 0);
    for (u32 id = 0; id < out_.patches.size(); ++id) {
        LayoutPatch& patch = out_.patches[id];
        // The Euler characteristic of the patch cut open along its own layout
        // edges (a seam through it is two sides of its border): each border
        // halfedge brings one edge and one vertex, so they cancel, leaving the
        // inside vertices less the inside edges plus the triangles.
        std::vector<u32> border;
        std::unordered_map<u32, u8> vertices;
        u32 insideHalfedges = 0;
        for (u32 t : patch.triangles) {
            for (u32 i = 0; i < 3; ++i) {
                const u32 h = 3 * t + i;
                const u32 twin = mesh_.twins[h];
                if (twin == kNone || mesh_.isLayout(h)) {
                    border.push_back(h);
                    vertices[mesh_.from(h)] = 0;
                    vertices[mesh_.to(h)] = 0;
                } else {
                    ++insideHalfedges;
                    vertices.emplace(mesh_.from(h), 1);
                }
            }
        }
        i32 inside = 0;
        for (const auto& [v, flag] : vertices) {
            (void)v;
            inside += flag;
        }
        patch.euler = inside - static_cast<i32>(insideHalfedges / 2) + static_cast<i32>(patch.triangles.size());
        u32 loops = 0;
        for (u32 first : border) {
            if (seen[first]) {
                continue;
            }
            ++loops;
            std::vector<u32> loop;
            u32 h = first;
            for (u32 guard = 0; guard < border.size() + 1; ++guard) {
                seen[h] = 1;
                loop.push_back(h);
                // Round to(h) inside the patch to the next border halfedge.
                u32 c = WorkMesh::Next(h);
                for (u32 spin = 0; spin < 512; ++spin) {
                    if (mesh_.twins[c] == kNone || mesh_.isLayout(c)) {
                        break;
                    }
                    c = WorkMesh::Next(mesh_.twins[c]);
                }
                h = c;
                if (h == first) {
                    break;
                }
            }
            if (patch.loop.empty() || loop.size() > patch.loop.size()) {
                patch.loop = loop;
            }
        }
        patch.loops = loops;
    }
}

bool Builder::repairRound() {
    computePatches();
    ensureSectors();
    if (options_.verbose && out_.stats.rounds == 1) {
        auditSectors();
    }
    note("round: " + std::to_string(out_.patches.size()) + " patches");
    std::vector<Emission> emissions;
    pending_ = &emissions;
    bool changed = false;
    // A repair edits its patch and may split an edge of its border, so the
    // patches beside it wait for the next round's fresh measure.
    std::vector<u8> touched(out_.patches.size(), 0);
    std::vector<u32> beside;
    auto touch = [&](u32 id) {
        touched[id] = 1;
        for (u32 other : beside) {
            touched[other] = 1;
        }
    };
    for (u32 id = 0; id < out_.patches.size(); ++id) {
        if (touched[id]) {
            continue;
        }
        const LayoutPatch& patch = out_.patches[id];
        beside.clear();
        for (u32 h : patch.loop) {
            const u32 twin = mesh_.twins[h];
            if (twin != kNone && twin / 3 < out_.patchOf.size() && out_.patchOf[twin / 3] != kNone) {
                beside.push_back(out_.patchOf[twin / 3]);
            }
        }
        // A singular point left inside: its separatrices.
        u32 singular = kNone;
        for (u32 t : patch.triangles) {
            for (u32 i = 0; i < 3 && singular == kNone; ++i) {
                const u32 v = mesh_.corners[3 * t + i];
                if (!isLayoutVertex(v) && field_.index[v] != 0) {
                    singular = v;
                }
            }
        }
        if (singular != kNone) {
            note("patch " + std::to_string(id) + ": a singular point inside");
            singularArms(singular, emissions);
            touch(id);
            continue;
        }
        if (patch.loop.empty()) {
            // Closed: no border at all. Cut it with a trace both ways.
            const u32 v = mesh_.corners[3 * patch.triangles.front()];
            note("patch " + std::to_string(id) + ": closed");
            for (u32 k : {0u, 2u}) {
                V3 d{0.0, 0.0, 0.0};
                if (armAround(v, k, d)) {
                    emissions.push_back({v, d, kNone, k, 4});
                }
            }
            touch(id);
            continue;
        }
        if (patch.loops != 1 || patch.euler != 1) {
            // Not a disk: cross it from a flat point of its border.
            note("patch " + std::to_string(id) + ": not a disk");
            const std::size_t size = patch.loop.size();
            for (std::size_t step = 0; step < size; ++step) {
                const u32 h = patch.loop[(size / 2 + step) % size];
                if (sectors_[h] < 2) {
                    continue;
                }
                V3 d{0.0, 0.0, 0.0};
                if (armPast(mesh_.from(h), h, 1, d)) {
                    emissions.push_back({mesh_.from(h), d, h, 1, 0});
                    break;
                }
            }
            touch(id);
            continue;
        }
        // A concave corner: split it with a trace into the patch, or with a
        // straight cut when no trace from it has done so.
        bool concave = false;
        u32 concaveAt = kNone;
        for (u32 i = 0; i < patch.loop.size(); ++i) {
            const u32 h = patch.loop[i];
            const u8 k = sectors_[h];
            if (k >= 3) {
                concaveAt = concaveAt == kNone ? i : concaveAt;
                if (concaveTries_[mesh_.from(h)] >= 2) {
                    continue;
                }
                const u32 j = k >= 4 ? 2 : 1;
                V3 d{0.0, 0.0, 0.0};
                if (armPast(mesh_.from(h), h, j, d)) {
                    note("patch " + std::to_string(id) + ": concave");
                    emissions.push_back({mesh_.from(h), d, h, j, 0});
                    ++concaveTries_[mesh_.from(h)];
                    concave = true;
                    break;
                }
            }
        }
        if (concave) {
            touch(id);
            continue;
        }
        if (concaveAt != kNone) {
            if (concaveCut(id, concaveAt)) {
                changed = true;
                touch(id);
            }
            continue;
        }
        u32 corners = 0;
        for (u32 h : patch.loop) {
            corners += sectors_[h] == 1 ? 1 : 0;
        }
        if (corners == 4) {
            continue;
        }
        note("patch " + std::to_string(id) + " (" + std::to_string(patch.triangles.size()) + " triangles): " +
             std::to_string(corners) + " corners");
        if (corners >= 3 ? centreSplit(id) : forceCorners(id)) {
            changed = true;
            touch(id);
        }
    }
    pending_ = nullptr;
    if (!emissions.empty()) {
        out_.stats.repairTraces += static_cast<u32>(emissions.size());
        traceAll(emissions);
        changed = true;
    }
    return changed;
}

u32 Builder::splitCarrying(u32 h, f64 t, MapCorners& corners) {
    const u32 t0 = h / 3;
    const u32 twin = mesh_.twins[h];
    const u32 sa = h % 3;
    const u32 sb = (sa + 1) % 3;
    const u32 sc = (sa + 2) % 3;
    auto lerp = [](const P2& p, const P2& q, f64 f) {
        return P2{p[0] + f * (q[0] - p[0]), p[1] + f * (q[1] - p[1])};
    };
    const auto near = corners.find(t0);
    const bool hasNear = near != corners.end();
    const std::array<P2, 3> c0 = hasNear ? near->second : std::array<P2, 3>{};
    const auto far = twin != kNone ? corners.find(twin / 3) : corners.end();
    const bool hasFar = far != corners.end();
    const std::array<P2, 3> c1 = hasFar ? far->second : std::array<P2, 3>{};
    const u32 before = mesh_.triangleCount();
    const u32 m = split(h, Lerp(mesh_.positions[mesh_.from(h)], mesh_.positions[mesh_.to(h)], t));
    // (a, b, c) became (a, m, c) and a new (m, b, c); across, (b, a, d)
    // became (b, m, d) and a new (m, a, d).
    if (hasNear) {
        const P2 middle = lerp(c0[sa], c0[sb], t);
        corners[t0][sb] = middle;
        corners[before] = {middle, c0[sb], c0[sc]};
    }
    if (hasFar) {
        const u32 s1b = twin % 3;
        const u32 s1a = (s1b + 1) % 3;
        const u32 s1d = (s1b + 2) % 3;
        const P2 middle = lerp(c1[s1a], c1[s1b], t);
        corners[twin / 3][s1a] = middle;
        corners[before + 1] = {middle, c1[s1a], c1[s1d]};
    }
    return m;
}

std::vector<u32> Builder::cutSpoke(u32 from, const P2& target, MapCorners& corners,
                                   const std::unordered_map<u32, u8>& avoid) {
    // The edge parameter within which a crossing lands on the vertex itself.
    constexpr f64 kSnap = 1e-3;
    std::vector<u32> path{from};
    u32 v = from;
    std::vector<u32> ring;
    const u32 guard = 4 * mesh_.triangleCount() + 64;
    for (u32 step = 0; step < guard; ++step) {
        mesh_.ring(v, ring);
        // The edge facing v that the ray toward the target leaves through,
        // each triangle read in its own corners.
        u32 hit = kNone;
        f64 t = 0.0;
        for (u32 o : ring) {
            const auto it = corners.find(o / 3);
            if (it == corners.end()) {
                continue;
            }
            const u32 slot = o % 3;
            const P2& at = it->second[slot];
            const P2& px = it->second[(slot + 1) % 3];
            const P2& py = it->second[(slot + 2) % 3];
            const P2 d{target[0] - at[0], target[1] - at[1]};
            const P2 e{py[0] - px[0], py[1] - px[1]};
            const P2 w{px[0] - at[0], px[1] - at[1]};
            const f64 denom = d[0] * e[1] - d[1] * e[0];
            if (std::abs(denom) < 1e-300) {
                continue;
            }
            const f64 s = (w[0] * e[1] - w[1] * e[0]) / denom;
            const f64 along = (w[0] * d[1] - w[1] * d[0]) / denom;
            if (s > 1e-12 && along >= -1e-9 && along <= 1.0 + 1e-9) {
                hit = WorkMesh::Next(o);
                t = std::clamp(along, 0.0, 1.0);
                break;
            }
        }
        if (hit == kNone) {
            return {};
        }
        const u32 x = mesh_.from(hit);
        const u32 y = mesh_.to(hit);
        const bool wall = mesh_.isLayout(hit) || mesh_.twins[hit] == kNone;
        u32 next = kNone;
        if (t <= kSnap) {
            next = avoid.count(x) ? kNone : x;
        } else if (t >= 1.0 - kSnap) {
            next = avoid.count(y) ? kNone : y;
        }
        if (next != kNone) {
            path.push_back(next);
            if (isLayoutVertex(next)) {
                return path; // landed on a flat point of the border
            }
            v = next;
            continue;
        }
        // A refused snap still keeps clear of the vertex it would have hit.
        if (t <= kSnap || t >= 1.0 - kSnap) {
            t = std::clamp(t, 0.1, 0.9);
        }
        const u32 m = splitCarrying(hit, t, corners);
        path.push_back(m);
        if (wall) {
            return path;
        }
        v = m;
    }
    return {};
}

bool Builder::centreSplit(u32 id) {
    const LayoutPatch& patch = out_.patches[id];
    const std::vector<u32>& loop = patch.loop;
    const u32 size = static_cast<u32>(loop.size());
    std::vector<u32> cornerAt;
    for (u32 i = 0; i < size; ++i) {
        if (sectors_[loop[i]] == 1) {
            cornerAt.push_back(i);
        }
    }
    const u32 n = static_cast<u32>(cornerAt.size());
    if (++refinements_[patch.triangles.front()] > 6) {
        note("centre split: the patch would not split");
        return false;
    }
    PatchMap map;
    if (!BuildPatchMap(mesh_, patch, CornerCircleBorder(mesh_, patch, cornerAt), map)) {
        note("centre split: the patch would not lay flat");
        return false;
    }
    if (map.vertices.size() == map.borderCount) {
        // No vertex inside: split the longest edge across the patch.
        u32 longest = kNone;
        f64 length = 0.0;
        for (u32 t : patch.triangles) {
            for (u32 i = 0; i < 3; ++i) {
                const u32 h = 3 * t + i;
                if (mesh_.isLayout(h) || mesh_.twins[h] == kNone) {
                    continue;
                }
                const f64 l = Distance(mesh_.positions[mesh_.from(h)], mesh_.positions[mesh_.to(h)]);
                if (l > length) {
                    length = l;
                    longest = h;
                }
            }
        }
        if (longest == kNone) {
            // Not even an edge inside: the longest border edge, then, whose
            // split draws one across.
            for (u32 h : loop) {
                const f64 l = Distance(mesh_.positions[mesh_.from(h)], mesh_.positions[mesh_.to(h)]);
                if (l > length) {
                    length = l;
                    longest = h;
                }
            }
        }
        split(longest, Lerp(mesh_.positions[mesh_.from(longest)], mesh_.positions[mesh_.to(longest)], 0.5));
        return true;
    }
    // The centre: the inside vertex nearest the middle of the circle.
    u32 centre = kNone;
    f64 nearest = std::numeric_limits<f64>::infinity();
    MapCorners corners;
    for (u32 k = 0; k < map.triangleCount(); ++k) {
        corners[map.triangles[k]] = {map.uv[map.corners[3 * k]], map.uv[map.corners[3 * k + 1]],
                                     map.uv[map.corners[3 * k + 2]]};
    }
    for (u32 k = map.borderCount; k < map.vertices.size(); ++k) {
        const f64 r = map.uv[k][0] * map.uv[k][0] + map.uv[k][1] * map.uv[k][1];
        if (r < nearest) {
            nearest = r;
            centre = k;
        }
    }
    const u32 c = map.vertices[centre];
    // Each spoke runs straight in the map to the middle of its side. The map
    // is convex and one to one, so the spokes never meet but at the centre,
    // and each is cut into the mesh edge by edge.
    std::unordered_map<u32, u8> avoid;
    for (u32 i : cornerAt) {
        avoid[mesh_.from(loop[i])] = 1;
    }
    // Read before any spoke: a spoke landing on the border splits its edge.
    std::vector<f64> lengths(size);
    for (u32 i = 0; i < size; ++i) {
        lengths[i] = Distance(mesh_.positions[mesh_.from(loop[i])], mesh_.positions[mesh_.to(loop[i])]);
    }
    auto edgeLength = [&](u32 i) {
        return lengths[i];
    };
    std::vector<std::vector<u32>> spokes;
    for (u32 s = 0; s < n; ++s) {
        const u32 begin = cornerAt[s];
        const u32 span = (cornerAt[(s + 1) % n] + size - begin) % size;
        const u32 count = span == 0 ? size : span;
        f64 side = 0.0;
        for (u32 k = 0; k < count; ++k) {
            side += edgeLength((begin + k) % size);
        }
        P2 target = map.uv[begin];
        f64 walked = 0.0;
        for (u32 k = 0; k < count; ++k) {
            const u32 i = (begin + k) % size;
            const f64 l = edgeLength(i);
            if (walked + l >= 0.5 * side || k + 1 == count) {
                const f64 f = l > 0.0 ? std::clamp((0.5 * side - walked) / l, 0.0, 1.0) : 0.5;
                const P2& a = map.uv[i];
                const P2& b = map.uv[(i + 1) % size];
                target = {a[0] + f * (b[0] - a[0]), a[1] + f * (b[1] - a[1])};
                break;
            }
            walked += l;
        }
        std::vector<u32> spoke = cutSpoke(c, target, corners, avoid);
        if (spoke.size() < 2) {
            note("centre split: a spoke lost its way");
            return true; // what was cut stays as refinement; the next round tries again
        }
        for (std::size_t k = 1; k < spoke.size(); ++k) {
            avoid[spoke[k]] = 1;
        }
        spokes.push_back(std::move(spoke));
    }
    onLayout_[c] = 1;
    pinned_[c] = 1;
    for (const std::vector<u32>& path : spokes) {
        const u32 pathId = nextPathId_++;
        pinned_[path.back()] = 1;
        for (std::size_t k = 1; k < path.size(); ++k) {
            u32 h = mesh_.find(path[k - 1], path[k]);
            if (h == kNone) {
                h = mesh_.find(path[k], path[k - 1]);
            }
            if (h != kNone) {
                markTrace(h, pathId);
            } else {
                note("centre split: a spoke stepped between two vertices with no edge");
            }
            onLayout_[path[k]] = 1;
        }
        for (std::size_t k = 1; k + 1 < path.size(); ++k) {
            const u32 forward = mesh_.find(path[k], path[k + 1]);
            const u32 backward = mesh_.find(path[k], path[k - 1]);
            if (forward != kNone) {
                sectors_[forward] = 2;
            }
            if (backward != kNone) {
                sectors_[backward] = 2;
            }
        }
        const u32 first = mesh_.find(c, path[1]);
        if (first != kNone) {
            sectors_[first] = 1;
        }
    }
    for (const std::vector<u32>& path : spokes) {
        arrive(path.back(), path[path.size() - 2]);
    }
    out_.turnsOverride[c] = static_cast<i32>(n);
    ++out_.stats.centreSplits;
    note("  centre split of " + std::to_string(n) + " spokes at vertex " + std::to_string(c));
    return true;
}

bool Builder::concaveCut(u32 id, u32 position) {
    const LayoutPatch& patch = out_.patches[id];
    const std::vector<u32>& loop = patch.loop;
    const u32 size = static_cast<u32>(loop.size());
    if (++refinements_[patch.triangles.front()] > 6) {
        note("concave cut: the patch would not split");
        return false;
    }
    PatchMap map;
    if (!BuildPatchMap(mesh_, patch, CircleBorder(mesh_, patch), map)) {
        note("concave cut: the patch would not lay flat");
        return false;
    }
    MapCorners corners;
    for (u32 k = 0; k < map.triangleCount(); ++k) {
        corners[map.triangles[k]] = {map.uv[map.corners[3 * k]], map.uv[map.corners[3 * k + 1]],
                                     map.uv[map.corners[3 * k + 2]]};
    }
    // Straight across, to the border half the loop's length on.
    std::vector<f64> at(size + 1, 0.0);
    for (u32 i = 0; i < size; ++i) {
        at[i + 1] = at[i] + Distance(mesh_.positions[mesh_.from(loop[i])], mesh_.positions[mesh_.to(loop[i])]);
    }
    const f64 half = 0.5 * at[size];
    P2 target = map.uv[(position + size / 2) % size];
    f64 walked = 0.0;
    for (u32 k = 0; k < size; ++k) {
        const u32 i = (position + k) % size;
        const f64 l = at[i + 1] - at[i];
        if (walked + l >= half) {
            const f64 f = l > 0.0 ? std::clamp((half - walked) / l, 0.0, 1.0) : 0.5;
            const P2& a = map.uv[i];
            const P2& b = map.uv[(i + 1) % size];
            target = {a[0] + f * (b[0] - a[0]), a[1] + f * (b[1] - a[1])};
            break;
        }
        walked += l;
    }
    const u32 q = mesh_.from(loop[position]);
    std::unordered_map<u32, u8> avoid{{q, 1}};
    for (u32 i = 0; i < size; ++i) {
        if (sectors_[loop[i]] != 2) {
            avoid[mesh_.from(loop[i])] = 1;
        }
    }
    const u32 start = loop[position];
    // The corner's neighbour before it on the loop: no halfedge from q to it
    // where the loop runs along the mesh's own border.
    const u32 before = mesh_.from(loop[(position + size - 1) % size]);
    const u32 k = sectors_[start];
    const std::vector<u32> path = cutSpoke(q, target, corners, avoid);
    if (path.size() < 2) {
        note("concave cut: the cut lost its way");
        return true; // what was cut stays as refinement
    }
    const u32 pathId = nextPathId_++;
    pinned_[q] = 1;
    pinned_[path.back()] = 1;
    for (std::size_t i = 1; i < path.size(); ++i) {
        u32 h = mesh_.find(path[i - 1], path[i]);
        if (h == kNone) {
            h = mesh_.find(path[i], path[i - 1]);
        }
        if (h != kNone) {
            markTrace(h, pathId);
        }
        onLayout_[path[i]] = 1;
    }
    for (std::size_t i = 1; i + 1 < path.size(); ++i) {
        const u32 forward = mesh_.find(path[i], path[i + 1]);
        const u32 backward = mesh_.find(path[i], path[i - 1]);
        if (forward != kNone) {
            sectors_[forward] = 2;
        }
        if (backward != kNone) {
            sectors_[backward] = 2;
        }
    }
    // q's sector shared by angle between the two sides of the cut.
    const u32 cut = mesh_.find(q, path[1]);
    if (cut != kNone) {
        const V3 p = mesh_.positions[q];
        const f64 first = Angle(mesh_.positions[mesh_.to(start)] - p, mesh_.positions[path[1]] - p);
        const f64 second = Angle(mesh_.positions[path[1]] - p, mesh_.positions[before] - p);
        const i32 j = std::clamp<i32>(static_cast<i32>(std::lround(k * first / std::max(first + second, 1e-12))), 1,
                                      static_cast<i32>(k) - 1);
        sectors_[start] = static_cast<u8>(j);
        sectors_[cut] = static_cast<u8>(k - j);
    }
    arrive(path.back(), path[path.size() - 2]);
    note("  cut a concave corner across at vertex " + std::to_string(q));
    return true;
}

bool Builder::forceCorners(u32 id) {
    const LayoutPatch& patch = out_.patches[id];
    const std::vector<u32>& loop = patch.loop;
    std::vector<u32> cornerAt;
    for (u32 i = 0; i < loop.size(); ++i) {
        if (sectors_[loop[i]] == 1) {
            cornerAt.push_back(i);
        }
    }
    // Spread the missing corners evenly by length over the loop, between the
    // corners it has.
    const u32 missing = 4 - static_cast<u32>(cornerAt.size());
    std::vector<f64> at(loop.size() + 1, 0.0);
    for (u32 i = 0; i < loop.size(); ++i) {
        at[i + 1] = at[i] + Distance(mesh_.positions[mesh_.from(loop[i])], mesh_.positions[mesh_.to(loop[i])]);
    }
    const f64 total = at[loop.size()];
    const u32 start = cornerAt.empty() ? 0 : cornerAt[0];
    std::vector<u8> taken(loop.size(), 0);
    for (u32 c : cornerAt) {
        taken[c] = 1;
    }
    u32 forced = 0;
    for (u32 k = 1; k <= missing; ++k) {
        const f64 want = std::fmod(at[start] + total * k / (missing + 1), total);
        u32 best = kNone;
        f64 bestOff = std::numeric_limits<f64>::infinity();
        for (u32 i = 0; i < loop.size(); ++i) {
            if (taken[i] || sectors_[loop[i]] != 2 || out_.turnsOverride[mesh_.from(loop[i])] != 0) {
                continue;
            }
            // Its turns less the one taken must leave it three: a valence-3
            // point, never fewer.
            std::vector<u32> outs;
            std::vector<u32> ring;
            bool closed = true;
            layoutOut(mesh_.from(loop[i]), ring, outs, closed);
            u32 turns = 0;
            for (u32 o : outs) {
                turns += sectors_[o];
            }
            if (closed && turns < 4) {
                continue;
            }
            f64 off = std::abs(at[i] - want);
            off = std::min(off, total - off);
            if (off < bestOff) {
                bestOff = off;
                best = i;
            }
        }
        if (best == kNone) {
            break;
        }
        taken[best] = 1;
        // A flat point made a corner of this patch alone: its sector here is
        // one turn and the vertex loses that turn, a valence-3 point, rather
        // than giving the patch beside it a concave corner to repair.
        const u32 h = loop[best];
        std::vector<u32> outs;
        std::vector<u32> ring;
        bool closed = true;
        layoutOut(mesh_.from(h), ring, outs, closed);
        i32 turns = 0;
        for (u32 o : outs) {
            turns += sectors_[o];
        }
        sectors_[h] = 1;
        out_.turnsOverride[mesh_.from(h)] = std::max(1, turns - 1);
        ++out_.stats.forcedCorners;
        ++forced;
    }
    if (forced > 0) {
        return true;
    }
    // No flat point left to take: split the longest border edge for one, at
    // most a few times for one patch.
    if (loop.empty() || ++refinements_[patch.triangles.front()] > 6) {
        note("forced corners: no flat point on the border");
        return false;
    }
    u32 longest = 0;
    for (u32 i = 1; i < loop.size(); ++i) {
        if (at[i + 1] - at[i] > at[longest + 1] - at[longest]) {
            longest = i;
        }
    }
    const u32 h = loop[longest];
    split(h, Lerp(mesh_.positions[mesh_.from(h)], mesh_.positions[mesh_.to(h)], 0.5));
    return true;
}

// ============================================================================
// The T-mesh
// ============================================================================

void Builder::buildArcs() {
    computePatches();
    ensureSectors();
    out_.sectorTurns = sectors_;
    const u32 vertices = mesh_.vertexCount();
    out_.nodeOf.assign(vertices, kNone);
    out_.nodeVertex.clear();
    out_.arcs.clear();
    std::vector<u32> ring;
    // Layout edges at each vertex, both directions, as (halfedge, other end).
    auto neighbours = [&](u32 v, std::vector<std::pair<u32, u32>>& outEdges) {
        outEdges.clear();
        const bool closed = mesh_.ring(v, ring);
        for (u32 o : ring) {
            if (mesh_.isLayout(o)) {
                outEdges.push_back({o, mesh_.to(o)});
            }
        }
        if (!closed && !ring.empty()) {
            const u32 in = WorkMesh::Prev(ring.back());
            if (mesh_.isLayout(in)) {
                outEdges.push_back({kNone, mesh_.from(in)});
            }
        }
    };
    std::vector<std::pair<u32, u32>> edges;
    for (u32 v = 0; v < vertices; ++v) {
        if (!mesh_.alive(v) || !isLayoutVertex(v)) {
            continue;
        }
        neighbours(v, edges);
        if (edges.empty()) {
            continue;
        }
        bool node = edges.size() != 2 || out_.turnsOverride[v] != 0;
        for (const auto& [h, w] : edges) {
            (void)w;
            if (h != kNone && out_.sectorTurns[h] != 2) {
                node = true;
            }
        }
        if (node) {
            out_.nodeOf[v] = static_cast<u32>(out_.nodeVertex.size());
            out_.nodeVertex.push_back(v);
        }
    }
    // Walk every layout edge once, node to node.
    std::unordered_map<u64, u8> walked;
    auto key = [](u32 a, u32 b) { return (static_cast<u64>(std::min(a, b)) << 32) | std::max(a, b); };
    auto walkFrom = [&](u32 start, u32 next) {
        // Its own list: the callers are iterating theirs.
        std::vector<std::pair<u32, u32>> along;
        LayoutArc arc;
        arc.vertices = {start};
        u32 previous = start;
        u32 current = next;
        for (u32 guard = 0; guard < vertices + 1; ++guard) {
            walked[key(previous, current)] = 1;
            arc.vertices.push_back(current);
            if (out_.nodeOf[current] != kNone) {
                break;
            }
            neighbours(current, along);
            u32 following = kNone;
            for (const auto& [h, w] : along) {
                (void)h;
                if (w != previous && !walked.count(key(current, w))) {
                    following = w;
                }
            }
            if (following == kNone) {
                break;
            }
            previous = current;
            current = following;
        }
        for (std::size_t i = 1; i < arc.vertices.size(); ++i) {
            const u32 a = arc.vertices[i - 1];
            const u32 b = arc.vertices[i];
            arc.length += Distance(mesh_.positions[a], mesh_.positions[b]);
            const u32 any = mesh_.find(a, b) != kNone ? mesh_.find(a, b) : mesh_.find(b, a);
            if (any != kNone) {
                arc.features |= mesh_.features[any] & kSourceFeatures;
            }
        }
        out_.arcs.push_back(arc);
    };
    for (u32 node = 0; node < out_.nodeVertex.size(); ++node) {
        const u32 v = out_.nodeVertex[node];
        neighbours(v, edges);
        for (const auto& [h, w] : edges) {
            (void)h;
            if (!walked.count(key(v, w))) {
                walkFrom(v, w);
            }
        }
    }
    // Loops with no node at all: start one anywhere on them.
    for (u32 v = 0; v < vertices; ++v) {
        if (!mesh_.alive(v) || !isLayoutVertex(v)) {
            continue;
        }
        neighbours(v, edges);
        for (const auto& [h, w] : edges) {
            (void)h;
            if (!walked.count(key(v, w))) {
                out_.nodeOf[v] = static_cast<u32>(out_.nodeVertex.size());
                out_.nodeVertex.push_back(v);
                walkFrom(v, w);
            }
        }
    }

    // Each arc's patches and sides, from the patches' loops.
    std::unordered_map<u64, std::pair<u32, bool>> arcOfEdge; // (from, to) -> arc, forward
    for (u32 a = 0; a < out_.arcs.size(); ++a) {
        const LayoutArc& arc = out_.arcs[a];
        for (std::size_t i = 1; i < arc.vertices.size(); ++i) {
            const u32 x = arc.vertices[i - 1];
            const u32 y = arc.vertices[i];
            arcOfEdge[(static_cast<u64>(x) << 32) | y] = {a, true};
            arcOfEdge[(static_cast<u64>(y) << 32) | x] = {a, false};
        }
    }
    for (u32 id = 0; id < out_.patches.size(); ++id) {
        LayoutPatch& patch = out_.patches[id];
        patch.corners.clear();
        for (auto& side : patch.sides) {
            side.clear();
        }
        patch.rectangle = false;
        if (patch.loop.empty() || patch.loops != 1 || patch.euler != 1) {
            continue;
        }
        for (u32 i = 0; i < patch.loop.size(); ++i) {
            const u8 k = out_.sectorTurns[patch.loop[i]];
            if (k == 1) {
                patch.corners.push_back(i);
            } else if (k != 2) {
                patch.corners.clear();
                break;
            }
        }
        if (patch.corners.size() != 4) {
            patch.corners.clear();
            continue;
        }
        bool ok = true;
        for (u32 s = 0; s < 4 && ok; ++s) {
            const u32 begin = patch.corners[s];
            const u32 end = patch.corners[(s + 1) % 4];
            const u32 size = static_cast<u32>(patch.loop.size());
            const u32 span = (end + size - begin) % size;
            for (u32 k = 0; k < (span == 0 ? size : span); ++k) {
                const u32 h = patch.loop[(begin + k) % size];
                const auto it = arcOfEdge.find((static_cast<u64>(mesh_.from(h)) << 32) | mesh_.to(h));
                if (it == arcOfEdge.end()) {
                    ok = false;
                    break;
                }
                const auto [arcId, forward] = it->second;
                std::vector<SideArc>& side = patch.sides[s];
                if (side.empty() || side.back().arc != arcId) {
                    side.push_back({arcId, forward});
                    LayoutArc& arc = out_.arcs[arcId];
                    if (forward) {
                        arc.left = id;
                        arc.leftSide = static_cast<u8>(s);
                    } else {
                        arc.right = id;
                        arc.rightSide = static_cast<u8>(s);
                    }
                }
            }
        }
        patch.rectangle = ok;
    }
}

void Builder::run() {
    grow();
    for (u32 h = 0; h < mesh_.corners.size(); ++h) {
        if (!mesh_.dead[h / 3] && mesh_.isLayout(h)) {
            onLayout_[mesh_.from(h)] = 1;
            onLayout_[mesh_.to(h)] = 1;
        }
    }
    ensureSectors();
    // Every separatrix of every singular point inside, then every concave
    // corner of the features, all traced together.
    std::vector<Emission> emissions;
    for (u32 v = 0; v < mesh_.vertexCount(); ++v) {
        if (!mesh_.alive(v) || isLayoutVertex(v) || field_.index[v] == 0) {
            continue;
        }
        singularArms(v, emissions);
    }
    out_.stats.separatrices = static_cast<u32>(emissions.size());
    std::vector<u32> ring;
    std::vector<u32> outs;
    for (u32 v = 0; v < mesh_.vertexCount(); ++v) {
        if (!mesh_.alive(v) || !isLayoutVertex(v)) {
            continue;
        }
        bool closed = true;
        layoutOut(v, ring, outs, closed);
        for (u32 h : outs) {
            const u8 k = sectors_[h];
            for (u32 j = 1; k >= 3 && j < k; ++j) {
                V3 d{0.0, 0.0, 0.0};
                if (armPast(v, h, j, d)) {
                    emissions.push_back({v, d, h, j, 0});
                }
            }
        }
    }
    traceAll(emissions);
    for (u32 round = 0; round < options_.repairRounds; ++round) {
        out_.stats.rounds = round + 1;
        if (!repairRound()) {
            break;
        }
    }
    buildArcs();
    for (const LayoutPatch& patch : out_.patches) {
        out_.stats.unfilled += patch.rectangle ? 0 : 1;
    }
}

} // namespace

Layout BuildLayout(WorkMesh& mesh, CrossField& field, const Surface& surface, const LayoutOptions& options) {
    Layout layout;
    Builder builder(mesh, field, surface, options, layout);
    builder.run();
    return layout;
}

} // namespace retopo
} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
