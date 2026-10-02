// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "surface.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

#include <whiteout/models/wem/geometry/attributes.h>
#include <whiteout/models/wem/geometry/triangulation.h>

namespace whiteout {
namespace models {
namespace wem {
namespace geom {
namespace retopo {

namespace {

u64 EdgeKey(u32 a, u32 b) {
    return (static_cast<u64>(a) << 32) | b;
}

/// Two corner values differ: any component by more than a relative 1e-6.
bool Differs(const Vector2f& a, const Vector2f& b) {
    const f32 scale = std::max({1.0f, std::abs(a.x), std::abs(a.y), std::abs(b.x), std::abs(b.y)});
    return std::abs(a.x - b.x) > 1e-6f * scale || std::abs(a.y - b.y) > 1e-6f * scale;
}

} // namespace

// ============================================================================
// Points
// ============================================================================

SurfacePoint Surface::Make(u32 triangle, f64 w0, f64 w1, f64 w2) {
    const f64 sum = w0 + w1 + w2;
    SurfacePoint point;
    point.triangle = triangle;
    point.u = sum > 0.0 ? w1 / sum : 0.0;
    point.v = sum > 0.0 ? w2 / sum : 0.0;
    return point;
}

V3 Surface::position(const SurfacePoint& point) const {
    const u32 t = point.triangle;
    return positions[corners[3 * t]] * point.weight(0) + positions[corners[3 * t + 1]] * point.u +
           positions[corners[3 * t + 2]] * point.v;
}

V3 Surface::normal(const SurfacePoint& point) const {
    const u32 t = point.triangle;
    const V3 blended = cornerNormals[3 * t] * point.weight(0) + cornerNormals[3 * t + 1] * point.u +
                       cornerNormals[3 * t + 2] * point.v;
    const V3 n = Unit(blended);
    return Length2(n) > 0.0 ? n : triangleNormals[t];
}

V3 Surface::position(const FeaturePoint& point) const {
    return Lerp(positions[from(point.halfedge)], positions[to(point.halfedge)], point.t);
}

SurfacePoint Surface::toSurface(const FeaturePoint& point) const {
    const u32 t = point.halfedge / 3;
    const u32 i = point.halfedge % 3;
    f64 w[3] = {0.0, 0.0, 0.0};
    w[i] = 1.0 - point.t;
    w[(i + 1) % 3] = point.t;
    return Make(t, w[0], w[1], w[2]);
}

SurfacePoint Surface::atVertex(u32 vertex) const {
    const u32 t = vertexTriangles[vertex];
    f64 w[3] = {0.0, 0.0, 0.0};
    for (u32 i = 0; i < 3; ++i) {
        if (corners[3 * t + i] == vertex) {
            w[i] = 1.0;
        }
    }
    return Make(t, w[0], w[1], w[2]);
}

u32 Surface::nextStamp() const {
    if (stamp_.size() < std::max<std::size_t>(triangleCount(), corners.size())) {
        stamp_.assign(std::max<std::size_t>(triangleCount(), corners.size()), 0);
        stampValue_ = 0;
    }
    if (++stampValue_ == 0) {
        std::fill(stamp_.begin(), stamp_.end(), 0);
        stampValue_ = 1;
    }
    return stampValue_;
}

SurfacePoint Surface::locate(std::span<const u32> start, const V3& point, f64 radius) const {
    const u32 stamp = nextStamp();
    std::vector<u32> stack;
    SurfacePoint best;
    f64 bestDistance = std::numeric_limits<f64>::infinity();
    auto visit = [&](u32 t, bool always) {
        if (t == kNone || stamp_[t] == stamp) {
            return;
        }
        const Nearest nearest = NearestOnTriangle(point, positions[corners[3 * t]],
                                                  positions[corners[3 * t + 1]],
                                                  positions[corners[3 * t + 2]]);
        if (!always && nearest.distance > radius) {
            return;
        }
        stamp_[t] = stamp;
        if (nearest.distance < bestDistance) {
            bestDistance = nearest.distance;
            best = Make(t, nearest.w0, nearest.w1, nearest.w2);
        }
        stack.push_back(t);
    };
    for (u32 t : start) {
        visit(t, true);
    }
    while (!stack.empty()) {
        const u32 t = stack.back();
        stack.pop_back();
        for (u32 i = 0; i < 3; ++i) {
            const u32 twin = twins[3 * t + i];
            if (twin != kNone) {
                visit(twin / 3, false);
            }
        }
    }
    return best;
}

FeaturePoint Surface::locateOnCurve(const FeaturePoint& start, const V3& point, f64 radius) const {
    if (!start.valid()) {
        return start;
    }
    const u32 curve = curves[start.halfedge];
    const u32 stamp = nextStamp();
    FeaturePoint best = start;
    f64 bestDistance = std::numeric_limits<f64>::infinity();
    std::vector<u32> stack;
    auto visit = [&](u32 h, bool always) {
        if (stamp_[h] == stamp || curves[h] != curve) {
            return;
        }
        const V3 a = positions[from(h)];
        const V3 b = positions[to(h)];
        const f64 t = NearestOnSegment(point, a, b);
        const f64 d = Distance(point, Lerp(a, b, t));
        if (!always && d > radius) {
            return;
        }
        stamp_[h] = stamp;
        if (twins[h] != kNone) {
            stamp_[twins[h]] = stamp;
        }
        if (d < bestDistance) {
            bestDistance = d;
            best = FeaturePoint{h, t};
        }
        stack.push_back(h);
    };
    visit(start.halfedge, true);
    while (!stack.empty()) {
        const u32 h = stack.back();
        stack.pop_back();
        for (u32 end : {from(h), to(h)}) {
            if (kinds[end] == VertexKind::Corner) {
                continue;
            }
            for (u32 other : vertexFeatureEdges_[end]) {
                visit(other, false);
            }
        }
    }
    return best;
}

void Surface::fan(u32 vertex, std::vector<u32>& triangles) const {
    triangles.clear();
    const u32 t0 = vertexTriangles[vertex];
    u32 h0 = kNone;
    for (u32 i = 0; i < 3; ++i) {
        if (corners[3 * t0 + i] == vertex) {
            h0 = 3 * t0 + i;
        }
    }
    // Rewind to a border, if the fan has one, then walk forward; no fan holds
    // more triangles than there are, so twins that disagree never loop.
    u32 h = h0;
    for (u32 steps = 0; steps < triangleCount(); ++steps) {
        const u32 twin = twins[h];
        if (twin == kNone) {
            break;
        }
        const u32 back = Next(twin);
        if (back == h0) {
            break;
        }
        h = back;
    }
    const u32 first = h;
    for (;;) {
        triangles.push_back(h / 3);
        const u32 twin = twins[Prev(h)];
        if (twin == kNone || twin == first || triangles.size() >= triangleCount()) {
            break;
        }
        h = twin;
    }
}

// ============================================================================
// Building
// ============================================================================

void BuildCurves(Surface& s) {
    // Chains of feature edges between corners, then the closed loops.
    const u32 vertices = s.vertexCount();
    s.curves.assign(s.corners.size(), kNone);
    s.curveCount = 0;
    auto walk = [&](u32 startVertex, u32 startEdge, u32 curve) {
        u32 vertex = startVertex;
        u32 edge = startEdge;
        while (edge != kNone && s.curves[edge] == kNone) {
            s.curves[edge] = curve;
            if (s.twins[edge] != kNone) {
                s.curves[s.twins[edge]] = curve;
            }
            vertex = s.from(edge) == vertex ? s.to(edge) : s.from(edge);
            if (s.kinds[vertex] == VertexKind::Corner) {
                break;
            }
            u32 next = kNone;
            for (u32 other : s.vertexFeatureEdges_[vertex]) {
                if (s.curves[other] == kNone) {
                    next = other;
                }
            }
            edge = next;
        }
    };
    for (u32 v = 0; v < vertices; ++v) {
        if (s.kinds[v] != VertexKind::Corner) {
            continue;
        }
        for (u32 edge : s.vertexFeatureEdges_[v]) {
            if (s.curves[edge] == kNone) {
                walk(v, edge, s.curveCount++);
            }
        }
    }
    for (u32 v = 0; v < vertices; ++v) {
        for (u32 edge : s.vertexFeatureEdges_[v]) {
            if (s.curves[edge] == kNone) {
                walk(v, edge, s.curveCount++);
            }
        }
    }
}

Surface BuildSurface(const Mesh& welded, const FeatureOptions& options) {
    Surface s;
    const std::span<const Vector3f> positions =
        welded.attributes.get<const Vector3f>(names::kPosition, Domain::Vertex);
    s.positions.resize(positions.size());
    for (std::size_t v = 0; v < positions.size(); ++v) {
        s.positions[v] = ToV3(positions[v]);
    }
    std::vector<u32> faceOf;
    TriangulateMesh(welded, s.corners, &faceOf);
    const u32 triangles = s.triangleCount();
    s.faces = faceOf;

    V3 low = Make(1e300, 1e300, 1e300);
    V3 high = Make(-1e300, -1e300, -1e300);
    for (const V3& p : s.positions) {
        low = Make(std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z));
        high = Make(std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z));
    }
    s.diagonal = s.positions.empty() ? 0.0 : Distance(low, high);

    // Twins by directed edge.
    std::unordered_map<u64, u32> directed;
    directed.reserve(s.corners.size() * 2);
    for (u32 h = 0; h < s.corners.size(); ++h) {
        directed[EdgeKey(s.from(h), s.to(h))] = h;
    }
    s.twins.assign(s.corners.size(), kNone);
    for (u32 h = 0; h < s.corners.size(); ++h) {
        const auto it = directed.find(EdgeKey(s.to(h), s.from(h)));
        if (it != directed.end()) {
            s.twins[h] = it->second;
        }
    }

    // Each triangle corner's WEM corner, and the normals.
    const Topology& topology = welded.topology();
    s.cornerHalfedges.assign(s.corners.size(), kInvalidId);
    for (u32 t = 0; t < triangles; ++t) {
        const FaceId face(s.faces[t]);
        for (const HalfedgeId h : topology.fh(face)) {
            const u32 from = topology.from(h).value();
            for (u32 i = 0; i < 3; ++i) {
                if (s.corners[3 * t + i] == from) {
                    s.cornerHalfedges[3 * t + i] = h.value();
                }
            }
        }
    }
    s.triangleNormals.resize(triangles);
    s.triangleAreas.resize(triangles);
    for (u32 t = 0; t < triangles; ++t) {
        const V3& a = s.positions[s.corners[3 * t]];
        const V3& b = s.positions[s.corners[3 * t + 1]];
        const V3& c = s.positions[s.corners[3 * t + 2]];
        s.triangleNormals[t] = TriangleNormal(a, b, c);
        s.triangleAreas[t] = TriangleArea(a, b, c);
    }
    const std::span<const Vector3f> normals =
        welded.attributes.get<const Vector3f>(names::kNormal, Domain::Halfedge);
    s.authoredNormals = !normals.empty();
    s.cornerNormals.resize(s.corners.size());
    for (u32 c = 0; c < s.corners.size(); ++c) {
        V3 n{0.0, 0.0, 0.0};
        const u32 h = s.cornerHalfedges[c];
        if (s.authoredNormals && h < normals.size()) {
            n = Unit(ToV3(normals[h]));
        }
        s.cornerNormals[c] = Length2(n) > 0.0 ? n : s.triangleNormals[c / 3];
    }

    // Features, per halfedge.
    const std::span<const u8> sharp = welded.attributes.get<const u8>(names::kSharp, Domain::Edge);
    const std::span<const f32> crease = welded.attributes.get<const f32>(names::kCrease, Domain::Edge);
    const std::span<const u32> sections = welded.faceSections();
    struct UvSet {
        std::span<const Vector2f> values;
        std::span<const u8> cuts;
    };
    std::vector<UvSet> uvSets;
    for (u32 set = 0; set < UvSetCount(welded.attributes); ++set) {
        if ((options.uvSeamSets >> set) & 1u) {
            uvSets.push_back({welded.attributes.get<const Vector2f>(names::uv(set), Domain::Halfedge),
                              welded.attributes.get<const u8>(names::uvSeam(set), Domain::Edge)});
        }
    }
    f32 angle = options.angle;
    if (angle < 0.0f) {
        angle = s.authoredNormals ? 0.0f : 0.698131701f;
    }
    const f64 cosAngle = std::cos(static_cast<f64>(angle));

    s.features.assign(s.corners.size(), 0);
    for (u32 h = 0; h < s.corners.size(); ++h) {
        const u32 twin = s.twins[h];
        if (twin == kNone) {
            s.features[h] = kFeatureBorder;
            continue;
        }
        if (twin < h) {
            s.features[h] = s.features[twin];
            continue;
        }
        const u32 t0 = h / 3;
        const u32 t1 = twin / 3;
        if (s.faces[t0] == s.faces[t1]) {
            continue; // a diagonal inside one polygon
        }
        u8 bits = 0;
        const HalfedgeId wem = topology.findHalfedge(VertexId(s.from(h)), VertexId(s.to(h)));
        const u32 edge = wem.valid() ? Topology::edge(wem).value() : kInvalidId;
        if (s.faces[t0] < sections.size() && s.faces[t1] < sections.size() &&
            sections[s.faces[t0]] != sections[s.faces[t1]]) {
            bits |= kFeatureSection;
        }
        if (options.sharp && edge < sharp.size() && sharp[edge] != 0) {
            // The break at either end: each end's corner normal on this side
            // against the twin's on the other.
            const u32 next = 3 * t0 + (h % 3 + 1) % 3;
            const u32 twinNext = 3 * t1 + (twin % 3 + 1) % 3;
            const f64 shading = std::max(Angle(s.cornerNormals[h], s.cornerNormals[twinNext]),
                                         Angle(s.cornerNormals[next], s.cornerNormals[twin]));
            if (shading >= options.sharpAngle) {
                bits |= kFeatureSharp;
            }
        }
        if (options.crease && edge < crease.size() && crease[edge] > 0.0f) {
            bits |= kFeatureCrease;
        }
        if (wem.valid()) {
            // WEM keeps a corner on the halfedge leaving it: at from(h) the
            // corners are `wem` and next(opposite); at to(h), next(wem) and
            // opposite.
            const HalfedgeId other = Topology::opposite(wem);
            const HalfedgeId pairs[2][2] = {{wem, topology.next(other)},
                                            {topology.next(wem), other}};
            for (const UvSet& set : uvSets) {
                bool cut = edge < set.cuts.size() && set.cuts[edge] != 0;
                for (const auto& pair : pairs) {
                    if (!set.values.empty() && pair[0].index() < set.values.size() &&
                        pair[1].index() < set.values.size() &&
                        Differs(set.values[pair[0].index()], set.values[pair[1].index()])) {
                        cut = true;
                    }
                }
                if (cut) {
                    bits |= kFeatureUvSeam;
                }
            }
        }
        // A zero-area triangle has no normal, so no angle to its neighbour.
        if (angle > 0.0f && Length2(s.triangleNormals[t0]) > 0.0 && Length2(s.triangleNormals[t1]) > 0.0 &&
            Dot(s.triangleNormals[t0], s.triangleNormals[t1]) < cosAngle) {
            bits |= kFeatureAngle;
        }
        s.features[h] = bits;
    }

    // Vertex kinds: a corner where the feature valence is not two, or where two
    // feature edges turn sharply.
    const u32 vertices = s.vertexCount();
    s.vertexTriangles.assign(vertices, kNone);
    s.vertexFeatureEdges_.assign(vertices, {});
    for (u32 h = 0; h < s.corners.size(); ++h) {
        if (s.vertexTriangles[s.from(h)] == kNone) {
            s.vertexTriangles[s.from(h)] = h / 3;
        }
        if (s.features[h] != 0 && (s.twins[h] == kNone || h < s.twins[h])) {
            s.vertexFeatureEdges_[s.from(h)].push_back(h);
            s.vertexFeatureEdges_[s.to(h)].push_back(h);
        }
    }
    s.kinds.assign(vertices, VertexKind::Free);
    const f64 cosCorner = std::cos(static_cast<f64>(options.cornerAngle));
    for (u32 v = 0; v < vertices; ++v) {
        const std::vector<u32>& edges = s.vertexFeatureEdges_[v];
        if (edges.empty()) {
            continue;
        }
        if (edges.size() != 2) {
            s.kinds[v] = VertexKind::Corner;
            continue;
        }
        auto away = [&](u32 h) {
            const u32 other = s.from(h) == v ? s.to(h) : s.from(h);
            return Unit(s.positions[other] - s.positions[v]);
        };
        // Straight on is 1; a turn of `cornerAngle` or more is a corner.
        const f64 straightness = -Dot(away(edges[0]), away(edges[1]));
        s.kinds[v] = straightness < cosCorner ? VertexKind::Corner : VertexKind::Feature;
    }

    BuildCurves(s);

    // Regions (no feature crossed) and components (anything crossed).
    auto flood = [&](std::vector<u32>& label, bool stopAtFeatures) {
        label.assign(triangles, kNone);
        u32 count = 0;
        std::vector<u32> stack;
        for (u32 seed = 0; seed < triangles; ++seed) {
            if (label[seed] != kNone) {
                continue;
            }
            label[seed] = count;
            stack.push_back(seed);
            while (!stack.empty()) {
                const u32 t = stack.back();
                stack.pop_back();
                for (u32 i = 0; i < 3; ++i) {
                    const u32 h = 3 * t + i;
                    const u32 twin = s.twins[h];
                    if (twin == kNone || (stopAtFeatures && s.features[h] != 0)) {
                        continue;
                    }
                    if (label[twin / 3] == kNone) {
                        label[twin / 3] = count;
                        stack.push_back(twin / 3);
                    }
                }
            }
            ++count;
        }
        return count;
    };
    s.regionCount = flood(s.regions, true);
    s.componentCount = flood(s.components, false);
    return s;
}

void ScaleFeatures(Surface& s, f64 scale, f32 cornerAngle) {
    const u32 vertices = s.vertexCount();
    const f64 reach = 0.5 * scale;
    auto other = [&](u32 h, u32 v) {
        return s.from(h) == v ? s.to(h) : s.from(h);
    };
    // From @p v along feature edge @p h, through points where only two
    // feature edges meet, until @p length or a stop: the point reached and
    // the points passed.
    auto walk = [&](u32 v, u32 h, f64 length, bool stopAtCorners, std::vector<u32>* passed) {
        u32 at = v;
        u32 edge = h;
        f64 walked = 0.0;
        for (u32 step = 0; step < vertices && edge != kNone; ++step) {
            const u32 next = other(edge, at);
            walked += Distance(s.positions[at], s.positions[next]);
            at = next;
            if (at == v || walked >= length || s.vertexFeatureEdges_[at].size() != 2 ||
                (stopAtCorners && s.kinds[at] == VertexKind::Corner)) {
                break;
            }
            if (passed != nullptr) {
                passed->push_back(at);
            }
            const std::vector<u32>& edges = s.vertexFeatureEdges_[at];
            edge = edges[0] == edge ? edges[1] : edges[0];
        }
        return s.positions[at];
    };
    const f64 cosCorner = std::cos(static_cast<f64>(cornerAngle));
    // How straight each two-edge point runs, at itself and over the reach.
    std::vector<f64> straight(vertices, -1.0);
    std::vector<u8> candidate(vertices, 0);
    for (u32 v = 0; v < vertices; ++v) {
        const std::vector<u32>& edges = s.vertexFeatureEdges_[v];
        if (edges.size() != 2) {
            continue;
        }
        const V3& p = s.positions[v];
        straight[v] = -Dot(Unit(s.positions[other(edges[0], v)] - p), Unit(s.positions[other(edges[1], v)] - p));
        const f64 chord = -Dot(Unit(walk(v, edges[0], reach, false, nullptr) - p),
                               Unit(walk(v, edges[1], reach, false, nullptr) - p));
        candidate[v] = straight[v] < cosCorner && chord < cosCorner ? 1 : 0;
    }
    for (u32 v = 0; v < vertices; ++v) {
        const std::vector<u32>& edges = s.vertexFeatureEdges_[v];
        if (s.kinds[v] == VertexKind::Free) {
            continue;
        }
        if (edges.size() != 2) {
            s.kinds[v] = VertexKind::Corner;
            continue;
        }
        bool corner = candidate[v] != 0;
        for (u32 side = 0; side < 2 && corner; ++side) {
            std::vector<u32> passed;
            walk(v, edges[side], reach, false, &passed);
            for (u32 w : passed) {
                if (candidate[w] && (straight[w] < straight[v] || (straight[w] == straight[v] && w < v))) {
                    corner = false;
                }
            }
        }
        s.kinds[v] = corner ? VertexKind::Corner : VertexKind::Feature;
    }
    BuildCurves(s);
    // Each feature edge's direction: between the points half a quad edge
    // behind and ahead of it on its curve.
    s.tangents.assign(s.corners.size(), V3{0.0, 0.0, 0.0});
    for (u32 h = 0; h < s.corners.size(); ++h) {
        if (s.features[h] == 0 || (s.twins[h] != kNone && s.twins[h] < h)) {
            continue;
        }
        const u32 a = s.from(h);
        const u32 b = s.to(h);
        auto beyond = [&](u32 end, u32 back) {
            if (s.kinds[end] == VertexKind::Corner || s.vertexFeatureEdges_[end].size() != 2) {
                return s.positions[end];
            }
            const std::vector<u32>& edges = s.vertexFeatureEdges_[end];
            const u32 next = other(edges[0], end) == back ? edges[1] : edges[0];
            return walk(end, next, reach, true, nullptr);
        };
        V3 direction = beyond(b, a) - beyond(a, b);
        if (Length2(direction) == 0.0 || Dot(direction, s.positions[b] - s.positions[a]) <= 0.0) {
            direction = s.positions[b] - s.positions[a];
        }
        s.tangents[h] = Unit(direction);
        if (s.twins[h] != kNone) {
            s.tangents[s.twins[h]] = s.tangents[h] * -1.0;
        }
    }
}

} // namespace retopo
} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
