// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/geometry/fracture/cut.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <unordered_map>
#include <utility>

#include <whiteout/models/wem/geometry/cdt.h>
#include <whiteout/models/wem/geometry/interpolate.h>
#include <whiteout/models/wem/geometry/repair.h>
#include <whiteout/models/wem/geometry/triangulation.h>

#include "parallel.h"

namespace whiteout {
namespace models {
namespace wem {
namespace geom {
namespace fracture {

namespace {

constexpr u32 kNone = 0xFFFFFFFFu;

f64 Dot(const Vector3d& a, const Vector3d& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vector3d Times(const Vector3d& a, f64 s) {
    return Vector3d(a.x * s, a.y * s, a.z * s);
}

f64 Length(const Vector3d& a) {
    return std::sqrt(Dot(a, a));
}

Vector3d Lerp(const Vector3d& a, const Vector3d& b, f64 s) {
    return Vector3d(a.x + s * (b.x - a.x), a.y + s * (b.y - a.y), a.z + s * (b.z - a.z));
}

bool PositionLess(const Vector3d& a, const Vector3d& b) {
    if (a.x != b.x) {
        return a.x < b.x;
    }
    if (a.y != b.y) {
        return a.y < b.y;
    }
    return a.z < b.z;
}

struct PositionKey {
    u64 x;
    u64 y;
    u64 z;
    bool operator==(const PositionKey& o) const {
        return x == o.x && y == o.y && z == o.z;
    }
};

/// SplitMix64's finalizer: a widened f32 has 29 zero bits at the bottom, and
/// the tables index by the bottom bits, so every bit is mixed into them.
u64 Mix(u64 z) {
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

struct PositionHash {
    std::size_t operator()(const PositionKey& k) const {
        return static_cast<std::size_t>(Mix(Mix(Mix(k.x) ^ k.y) ^ k.z));
    }
};

PositionKey PositionBits(const Vector3d& p) {
    PositionKey k{};
    // +0.0 for -0.0, so the two compare equal as bits too.
    const f64 x = p.x + 0.0;
    const f64 y = p.y + 0.0;
    const f64 z = p.z + 0.0;
    std::memcpy(&k.x, &x, 8);
    std::memcpy(&k.y, &y, 8);
    std::memcpy(&k.z, &z, 8);
    return k;
}

// ============================================================================
// Point keys (cut.h, "Shared points")
// ============================================================================

using Key = std::array<u32, 6>;

struct KeyHash {
    std::size_t operator()(const Key& key) const {
        u64 h = 1469598103934665603ull;
        for (u32 w : key) {
            h ^= w;
            h *= 1099511628211ull;
        }
        return static_cast<std::size_t>(h ^ (h >> 29));
    }
};

enum : u32 { kSourceKey = 0, kEdgeKey = 1, kFaceKey = 2, kCornerKey = 3, kCrossingKey = 4 };

Key SourceKey(u32 target, u32 vertex) {
    return {kSourceKey, target, vertex, 0, 0, 0};
}

Key EdgeKey(u32 target, u32 u, u32 v, u64 plane) {
    return {kEdgeKey, target, std::min(u, v), std::max(u, v), static_cast<u32>(plane >> 32),
            static_cast<u32>(plane)};
}

Key FaceKey(u32 target, u32 triangle, const std::array<u32, 3>& sites) {
    return {kFaceKey, target, triangle, sites[0], sites[1], sites[2]};
}

Key CornerKey(const std::array<u32, 4>& sites) {
    return {kCornerKey, sites[0], sites[1], sites[2], sites[3], 0};
}

/// @p owner tells apart the cell faces one slice plane holds, each with
/// crossings of its own: its low cell + 1, or 0 where the plane is one face's.
Key CrossingKey(u64 plane, u32 index, u32 owner) {
    return {kCrossingKey, static_cast<u32>(plane >> 32), static_cast<u32>(plane), index, owner, 0};
}

/// The key of the plane two sites of a cell edge share: a fixed site's own face.
u64 PairPlane(u32 low, u32 high) {
    return IsFixedSite(high) ? PlaneKey(high, high) : PlaneKey(low, high);
}

/// The two planes whose line is the cell edge of @p sites (ascending), from
/// the sites alone, so every cell round the edge names the same two. Seeds
/// come first: a Voronoi edge is its lowest seed's planes to the other two
/// sites, a slice edge its two lowest planes. False when there are too few.
bool EdgePlanes(const std::array<u32, 3>& sites, u64& first, u64& second) {
    if (IsFixedSite(sites[0])) {
        if (sites[1] == kNoSite) {
            return false;
        }
        first = PlaneKey(sites[0], sites[0]);
        second = PlaneKey(sites[1], sites[1]);
        return true;
    }
    if (sites[2] == kNoSite) {
        return false;
    }
    first = PairPlane(sites[0], sites[1]);
    second = PairPlane(sites[0], sites[2]);
    return true;
}

// ============================================================================
// A target, prepared
// ============================================================================

struct Target {
    Mesh mesh; ///< A copy with connectivity, so corners have halfedges.
    bool usable = false;
    bool whole = false;
    const WindingMesh* winding = nullptr;
    std::vector<Vector3d> positions;
    std::vector<u32> loopOffset; ///< Per face + 1, into the two below.
    std::vector<u32> loopVertex;
    std::vector<HalfedgeId> loopHalfedge;
    std::vector<u32> triFirst; ///< Per face + 1, into the triangles.
    std::vector<u32> tri;      ///< Three vertex ids each.
    std::vector<HalfedgeId> triCorner;
    std::vector<Vector3d> faceLow;
    std::vector<Vector3d> faceHigh;
    std::vector<std::vector<Influence>> skin;
    std::vector<u32> merge;
    /// Past this, the triangles that close the target's holes (`FillHoles`):
    /// cut for the segments they leave, never kept.
    u32 realTriangles = 0;
    std::vector<Vector3d> fillLow;
    std::vector<Vector3d> fillHigh;
    /// `Parts::Touch`: per vertex, the bones it rides, ascending; and those
    /// with their parents.
    std::vector<std::vector<u32>> rides;
    std::vector<std::vector<u32>> ridesWide;
};

void Prepare(Target& t, const CutTarget& input, const CutOptions& options) {
    t.whole = input.whole;
    t.winding = input.winding;
    if (input.mesh == nullptr) {
        return;
    }
    t.mesh = *input.mesh;
    t.mesh.invalidateConnectivity();
    if (!t.mesh.ensureConnectivity().ok()) {
        return;
    }
    t.usable = true;
    const auto positions = t.mesh.attributes.get<Vector3f>(names::kPosition, Domain::Vertex);
    t.positions.reserve(positions.size());
    for (const Vector3f& p : positions) {
        t.positions.push_back(Vector3d(p.x, p.y, p.z));
    }
    const FaceSet& faces = t.mesh.faceSet();
    const Topology& topology = std::as_const(t.mesh).topology();
    const u32 faceCount = static_cast<u32>(faces.faceCount());
    t.loopOffset.assign(1, 0);
    for (u32 f = 0; f < faceCount; ++f) {
        const u32 base = t.loopOffset.back();
        const u32 valence = faces.faceValence[f];
        HalfedgeId h = topology.halfedge(FaceId(f));
        for (u32 k = 0; k < valence; ++k) {
            t.loopVertex.push_back(faces.cornerVertex[base + k]);
            t.loopHalfedge.push_back(h);
            h = topology.next(h);
        }
        t.loopOffset.push_back(base + valence);
    }
    std::vector<u32> triangles;
    std::vector<u32> faceOf;
    TriangulateMesh(t.mesh, triangles, &faceOf);
    t.triFirst.assign(faceCount + 1, 0);
    for (u32 f : faceOf) {
        ++t.triFirst[f + 1];
    }
    for (u32 f = 0; f < faceCount; ++f) {
        t.triFirst[f + 1] += t.triFirst[f];
    }
    t.tri = std::move(triangles);
    t.realTriangles = static_cast<u32>(t.tri.size() / 3);
    t.triCorner.resize(t.tri.size());
    for (std::size_t i = 0; i < faceOf.size(); ++i) {
        const u32 f = faceOf[i];
        for (u32 k = 0; k < 3; ++k) {
            const u32 v = t.tri[3 * i + k];
            for (u32 c = t.loopOffset[f]; c < t.loopOffset[f + 1]; ++c) {
                if (t.loopVertex[c] == v) {
                    t.triCorner[3 * i + k] = t.loopHalfedge[c];
                    break;
                }
            }
        }
    }
    t.faceLow.resize(faceCount);
    t.faceHigh.resize(faceCount);
    for (u32 f = 0; f < faceCount; ++f) {
        Vector3d low = t.positions[t.loopVertex[t.loopOffset[f]]];
        Vector3d high = low;
        for (u32 c = t.loopOffset[f]; c < t.loopOffset[f + 1]; ++c) {
            const Vector3d& p = t.positions[t.loopVertex[c]];
            low = Vector3d(std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z));
            high = Vector3d(std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z));
        }
        t.faceLow[f] = low;
        t.faceHigh[f] = high;
    }
    // The source skin, a rigid section's spelled out per vertex.
    const u32 vertexCount = static_cast<u32>(t.positions.size());
    t.skin.assign(vertexCount, {});
    const auto sections = t.mesh.faceSections();
    std::vector<u32> sectionOf(vertexCount, kNone);
    for (u32 f = 0; f < faceCount; ++f) {
        for (u32 c = t.loopOffset[f]; c < t.loopOffset[f + 1]; ++c) {
            if (sectionOf[t.loopVertex[c]] == kNone) {
                sectionOf[t.loopVertex[c]] = f < sections.size() ? sections[f] : 0;
            }
        }
    }
    for (u32 v = 0; v < vertexCount; ++v) {
        if (!t.mesh.skin.empty() && v < t.mesh.skin.vertexCount()) {
            const auto list = std::as_const(t.mesh.skin).forVertex(v);
            t.skin[v].assign(list.begin(), list.end());
        }
        const u32 s = sectionOf[v];
        if (t.skin[v].empty() && s < t.mesh.sections.size() && t.mesh.sections[s].rigidNode) {
            t.skin[v].push_back(Influence{*t.mesh.sections[s].rigidNode, 1.0f});
        }
    }
    const auto groups = t.mesh.attributes.get<u32>(names::kMergeGroup, Domain::Vertex);
    t.merge.resize(vertexCount);
    for (u32 v = 0; v < vertexCount; ++v) {
        t.merge[v] = v < groups.size() ? groups[v] : v;
    }
    if (options.parts == Parts::Touch) {
        // A tenth of a point's weight is a bone it rides.
        t.rides.resize(vertexCount);
        t.ridesWide.resize(vertexCount);
        for (u32 v = 0; v < vertexCount; ++v) {
            for (const Influence& influence : t.skin[v]) {
                if (influence.weight < 0.1f) {
                    continue;
                }
                t.rides[v].push_back(influence.bone);
                t.ridesWide[v].push_back(influence.bone);
                if (influence.bone < options.parents.size() &&
                    options.parents[influence.bone] < options.parents.size()) {
                    t.ridesWide[v].push_back(options.parents[influence.bone]);
                }
            }
            for (std::vector<u32>* list : {&t.rides[v], &t.ridesWide[v]}) {
                std::sort(list->begin(), list->end());
                list->erase(std::unique(list->begin(), list->end()), list->end());
            }
        }
    }
}

// ============================================================================
// Phase 1: each cell clips the targets
// ============================================================================

struct Point {
    Key key{};
    Vector3d position{0, 0, 0};
    u32 target = kNone;
    std::array<u32, 3> from{kNone, kNone, kNone}; ///< Source vertices it blends.
    std::array<f64, 3> weight{0, 0, 0};
};

/// What a fragment's edge lies on: a source edge, or a cell plane.
struct Carrier {
    bool onPlane = false;
    u64 plane = 0;
    u32 u = kNone;
    u32 v = kNone;
};

struct Fragment {
    u32 target = 0;
    u32 face = 0;
    u32 triangle = kNone; ///< None: the face, whole.
    u32 rank = 0;         ///< Its first source triangle, for the numbering.
    std::vector<u32> points;
    std::vector<std::array<f64, 3>> weights; ///< Per corner, on the triangle's corners.
    std::vector<Carrier> carriers;
};

struct Segment {
    u64 plane = 0;
    u32 a = 0;
    u32 b = 0;
};

struct CellPlane {
    u64 key = 0;
    HalfSpace canonical;
    bool low = true; ///< This cell keeps the negative side, and the tie.

    bool inside(const Vector3d& p) const {
        const f64 d = canonical.distance(p);
        return low ? d <= 0.0 : d > 0.0;
    }
};

struct Cell {
    std::vector<Point> points;
    std::unordered_map<Key, u32, KeyHash> index;
    std::vector<Fragment> fragments;
    std::vector<Segment> segments;

    u32 add(Point point) {
        const auto found = index.find(point.key);
        if (found != index.end()) {
            return found->second;
        }
        const u32 at = static_cast<u32>(points.size());
        index.emplace(point.key, at);
        points.push_back(std::move(point));
        return at;
    }
};

class Clipper {
public:
    Clipper(const CellComplex& diagram, std::vector<Target>& targets, u32 site, Cell& cell)
        : diagram_(diagram), targets_(targets), site_(site), cell_(cell) {
        const ConvexCell& convex = diagram.cells[site];
        for (const CellFace& face : convex.faces) {
            CellPlane plane;
            plane.key = face.plane;
            plane.canonical = diagram.plane(face.plane);
            plane.low = face.low;
            planes_.push_back(plane);
        }
        // A region of slice planes has no seed to key its edges by: each is
        // keyed by every plane through it, which its two ends share.
        if (diagram.sliced) {
            // Every region clips by its planes in one order. Where two planes'
            // line runs through a mesh edge, which triangle it pierces is a
            // tie the first plane's cut point decides, and the regions round
            // the line must all decide it the same way.
            std::sort(planes_.begin(), planes_.end(),
                      [](const CellPlane& l, const CellPlane& r) { return l.key < r.key; });
            std::map<std::pair<u32, u32>, u64> faceOf;
            for (const CellFace& face : convex.faces) {
                const std::size_t n = face.loop.size();
                for (std::size_t k = 0; k < n; ++k) {
                    const u32 a = face.loop[k];
                    const u32 b = face.loop[(k + 1) % n];
                    const auto [at, fresh] = faceOf.emplace(std::minmax(a, b), face.plane);
                    if (fresh) {
                        continue;
                    }
                    std::array<u32, 3> shared{kNoSite, kNoSite, kNoSite};
                    u32 count = 0;
                    for (u32 s : convex.sites[a]) {
                        if (s != kNoSite && count < 3 &&
                            std::find(convex.sites[b].begin(), convex.sites[b].end(), s) !=
                                convex.sites[b].end()) {
                            shared[count++] = s;
                        }
                    }
                    edges_.emplace(std::minmax(at->second, face.plane), shared);
                }
            }
        }
        low_ = convex.vertices.front();
        high_ = low_;
        for (const Vector3d& v : convex.vertices) {
            low_ = Vector3d(std::min(low_.x, v.x), std::min(low_.y, v.y), std::min(low_.z, v.z));
            high_ = Vector3d(std::max(high_.x, v.x), std::max(high_.y, v.y), std::max(high_.z, v.z));
        }
    }

    void run() {
        for (u32 ti = 0; ti < targets_.size(); ++ti) {
            const Target& t = targets_[ti];
            if (!t.usable || t.whole) {
                continue;
            }
            const u32 faceCount = static_cast<u32>(t.faceLow.size());
            for (u32 f = 0; f < faceCount; ++f) {
                if (t.faceHigh[f].x < low_.x || t.faceLow[f].x > high_.x ||
                    t.faceHigh[f].y < low_.y || t.faceLow[f].y > high_.y ||
                    t.faceHigh[f].z < low_.z || t.faceLow[f].z > high_.z) {
                    continue;
                }
                clipFace(ti, f);
            }
            const u32 triCount = static_cast<u32>(t.tri.size() / 3);
            for (u32 i = t.realTriangles; i < triCount; ++i) {
                const Vector3d& lo = t.fillLow[i - t.realTriangles];
                const Vector3d& hi = t.fillHigh[i - t.realTriangles];
                if (hi.x < low_.x || lo.x > high_.x || hi.y < low_.y || lo.y > high_.y ||
                    hi.z < low_.z || lo.z > high_.z) {
                    continue;
                }
                clipTriangle(ti, kNone, i, true);
            }
        }
    }

private:
    u32 sourcePoint(u32 target, u32 v) {
        Point p;
        p.key = SourceKey(target, v);
        p.position = targets_[target].positions[v];
        p.target = target;
        p.from = {v, kNone, kNone};
        p.weight = {1.0, 0.0, 0.0};
        return cell_.add(p);
    }

    void clipFace(u32 ti, u32 f) {
        const Target& t = targets_[ti];
        const u32 first = t.loopOffset[f];
        const u32 last = t.loopOffset[f + 1];
        bool whole = true;
        for (const CellPlane& plane : planes_) {
            u32 in = 0;
            for (u32 c = first; c < last; ++c) {
                in += plane.inside(t.positions[t.loopVertex[c]]) ? 1 : 0;
            }
            if (in == 0) {
                return;
            }
            whole = whole && in == last - first;
        }
        if (whole) {
            Fragment fragment;
            fragment.target = ti;
            fragment.face = f;
            fragment.rank = t.triFirst[f];
            for (u32 c = first; c < last; ++c) {
                fragment.points.push_back(sourcePoint(ti, t.loopVertex[c]));
                Carrier carrier;
                carrier.u = t.loopVertex[c];
                carrier.v = t.loopVertex[c + 1 < last ? c + 1 : first];
                fragment.carriers.push_back(carrier);
            }
            cell_.fragments.push_back(std::move(fragment));
            return;
        }
        for (u32 i = t.triFirst[f]; i < t.triFirst[f + 1]; ++i) {
            clipTriangle(ti, f, i, false);
        }
    }

    struct Corner {
        u32 point = 0;
        Vector3d position{0, 0, 0};
        std::array<f64, 3> weight{0, 0, 0};
    };

    std::array<f64, 3> cornerWeights(u32 ti, u32 triangle, const Point& p) const {
        std::array<f64, 3> w{0, 0, 0};
        const u32* v = &targets_[ti].tri[3 * triangle];
        for (u32 i = 0; i < 3; ++i) {
            for (u32 k = 0; k < 3; ++k) {
                if (p.from[i] != kNone && p.from[i] == v[k]) {
                    w[k] += p.weight[i];
                }
            }
        }
        return w;
    }

    /// The point where a cut by @p plane crosses a mesh edge.
    u32 edgePoint(u32 ti, u32 u, u32 v, const CellPlane& plane) {
        const Key key = EdgeKey(ti, u, v, plane.key);
        const auto found = cell_.index.find(key);
        if (found != cell_.index.end()) {
            return found->second;
        }
        const Target& t = targets_[ti];
        // Ordered by position, so a seam's two copies of an edge agree.
        u32 a = u;
        u32 b = v;
        if (PositionLess(t.positions[b], t.positions[a]) ||
            (!PositionLess(t.positions[a], t.positions[b]) && b < a)) {
            std::swap(a, b);
        }
        const f64 da = plane.canonical.distance(t.positions[a]);
        const f64 db = plane.canonical.distance(t.positions[b]);
        const f64 s = da != db ? std::clamp(da / (da - db), 0.0, 1.0) : 0.5;
        Point p;
        p.key = key;
        p.position = Lerp(t.positions[a], t.positions[b], s);
        p.target = ti;
        p.from = {a, b, kNone};
        p.weight = {1.0 - s, s, 0.0};
        return cell_.add(p);
    }

    /// The point where the cell edge of planes @p p and @p q pierces a triangle.
    u32 facePoint(u32 ti, u32 triangle, u64 p, u64 q, const Corner& from, const Corner& to,
                  const CellPlane& plane) {
        std::array<u32, 8> all{};
        u32 count = 0;
        const auto addSite = [&](u32 s) {
            if (std::find(all.begin(), all.begin() + count, s) == all.begin() + count) {
                all[count++] = s;
            }
        };
        if (!diagram_.sliced) {
            addSite(site_);
        }
        addSite(PlaneLow(p));
        addSite(PlaneHigh(p));
        addSite(PlaneLow(q));
        addSite(PlaneHigh(q));
        std::sort(all.begin(), all.begin() + count);
        std::array<u32, 3> sites{all[0], count > 1 ? all[1] : kNoSite, count > 2 ? all[2] : kNoSite};
        // A region's edge, by every plane through it.
        const auto edge = edges_.find(std::minmax(p, q));
        if (edge != edges_.end()) {
            sites = edge->second;
        }
        const Key key = FaceKey(ti, triangle, sites);
        const auto found = cell_.index.find(key);
        if (found != cell_.index.end()) {
            return found->second;
        }
        const Target& t = targets_[ti];
        const u32* tv = &t.tri[3 * triangle];
        Vector3d position;
        bool placed = false;
        u64 first = 0;
        u64 second = 0;
        if (EdgePlanes(sites, first, second)) {
            const HalfSpace a = diagram_.plane(first);
            const HalfSpace b = diagram_.plane(second);
            // The chord of a across the triangle, from its corners in position order.
            std::array<u32, 3> order{tv[0], tv[1], tv[2]};
            std::sort(order.begin(), order.end(), [&](u32 l, u32 r) {
                if (PositionLess(t.positions[l], t.positions[r])) {
                    return true;
                }
                if (PositionLess(t.positions[r], t.positions[l])) {
                    return false;
                }
                return l < r;
            });
            f64 d[3];
            for (u32 k = 0; k < 3; ++k) {
                d[k] = a.distance(t.positions[order[k]]);
            }
            std::array<Vector3d, 4> chord;
            u32 points = 0;
            for (u32 k = 0; k < 3; ++k) {
                if (d[k] == 0.0) {
                    chord[points++] = t.positions[order[k]];
                }
            }
            const u32 pairs[3][2] = {{0, 1}, {0, 2}, {1, 2}};
            for (const auto& pair : pairs) {
                const f64 d0 = d[pair[0]];
                const f64 d1 = d[pair[1]];
                if ((d0 < 0.0 && d1 > 0.0) || (d0 > 0.0 && d1 < 0.0)) {
                    chord[points++] = Lerp(t.positions[order[pair[0]]],
                                           t.positions[order[pair[1]]], d0 / (d0 - d1));
                }
            }
            if (points >= 2) {
                u32 e0 = 0;
                u32 e1 = 1;
                f64 widest = -1.0;
                for (u32 i = 0; i < points; ++i) {
                    for (u32 j = i + 1; j < points; ++j) {
                        const Vector3d r = chord[j] - chord[i];
                        if (Dot(r, r) > widest) {
                            widest = Dot(r, r);
                            e0 = i;
                            e1 = j;
                        }
                    }
                }
                Vector3d c0 = chord[e0];
                Vector3d c1 = chord[e1];
                if (PositionLess(c1, c0)) {
                    std::swap(c0, c1);
                }
                const f64 b0 = b.distance(c0);
                const f64 b1 = b.distance(c1);
                if (b0 != b1) {
                    position = Lerp(c0, c1, std::clamp(b0 / (b0 - b1), 0.0, 1.0));
                    placed = true;
                }
            }
        }
        if (!placed) {
            // Degenerate: where this polygon's edge crosses the plane.
            const f64 d0 = plane.canonical.distance(from.position);
            const f64 d1 = plane.canonical.distance(to.position);
            position = Lerp(from.position, to.position,
                            d0 != d1 ? std::clamp(d0 / (d0 - d1), 0.0, 1.0) : 0.5);
        }
        // Barycentric in the triangle.
        const Vector3d& pa = t.positions[tv[0]];
        const Vector3d v0 = t.positions[tv[1]] - pa;
        const Vector3d v1 = t.positions[tv[2]] - pa;
        const Vector3d v2 = position - pa;
        const f64 d00 = Dot(v0, v0);
        const f64 d01 = Dot(v0, v1);
        const f64 d11 = Dot(v1, v1);
        const f64 d20 = Dot(v2, v0);
        const f64 d21 = Dot(v2, v1);
        const f64 denominator = d00 * d11 - d01 * d01;
        f64 wv = 0.0;
        f64 ww = 0.0;
        if (denominator != 0.0) {
            wv = (d11 * d20 - d01 * d21) / denominator;
            ww = (d00 * d21 - d01 * d20) / denominator;
        }
        Point point;
        point.key = key;
        point.position = position;
        point.target = ti;
        point.from = {tv[0], tv[1], tv[2]};
        point.weight = {1.0 - wv - ww, wv, ww};
        return cell_.add(point);
    }

    void clipTriangle(u32 ti, u32 f, u32 triangle, bool fill) {
        const Target& t = targets_[ti];
        const u32* tv = &t.tri[3 * triangle];
        std::vector<Corner> corners(3);
        std::vector<Carrier> carriers(3);
        for (u32 k = 0; k < 3; ++k) {
            corners[k].point = sourcePoint(ti, tv[k]);
            corners[k].position = t.positions[tv[k]];
            corners[k].weight = {k == 0 ? 1.0 : 0.0, k == 1 ? 1.0 : 0.0, k == 2 ? 1.0 : 0.0};
            carriers[k].u = tv[k];
            carriers[k].v = tv[(k + 1) % 3];
        }
        std::vector<Corner> nextCorners;
        std::vector<Carrier> nextCarriers;
        std::vector<u8> in;
        for (const CellPlane& plane : planes_) {
            const std::size_t n = corners.size();
            in.resize(n);
            u32 inCount = 0;
            for (std::size_t i = 0; i < n; ++i) {
                in[i] = plane.inside(corners[i].position) ? 1 : 0;
                inCount += in[i];
            }
            if (inCount == n) {
                continue;
            }
            if (inCount == 0) {
                return;
            }
            nextCorners.clear();
            nextCarriers.clear();
            for (std::size_t i = 0; i < n; ++i) {
                const std::size_t j = (i + 1) % n;
                if (in[i] != 0) {
                    nextCorners.push_back(corners[i]);
                    nextCarriers.push_back(carriers[i]);
                }
                if (in[i] == in[j]) {
                    continue;
                }
                const Carrier& c = carriers[i];
                const u32 point = c.onPlane ? facePoint(ti, triangle, plane.key, c.plane,
                                                        corners[i], corners[j], plane)
                                            : edgePoint(ti, c.u, c.v, plane);
                Corner x;
                x.point = point;
                x.position = cell_.points[point].position;
                x.weight = cornerWeights(ti, triangle, cell_.points[point]);
                nextCorners.push_back(x);
                if (in[i] != 0) {
                    Carrier along;
                    along.onPlane = true;
                    along.plane = plane.key;
                    nextCarriers.push_back(along);
                } else {
                    nextCarriers.push_back(c);
                }
            }
            corners.swap(nextCorners);
            carriers.swap(nextCarriers);
        }
        // Drop repeated points: a corner a cut met exactly.
        Fragment fragment;
        fragment.target = ti;
        fragment.face = f;
        fragment.triangle = triangle;
        fragment.rank = triangle;
        for (std::size_t i = 0; i < corners.size(); ++i) {
            const std::size_t j = (i + 1) % corners.size();
            if (corners[i].point == corners[j].point) {
                continue;
            }
            fragment.points.push_back(corners[i].point);
            fragment.weights.push_back(corners[i].weight);
            fragment.carriers.push_back(carriers[i]);
        }
        if (fragment.points.size() < 3) {
            return;
        }
        const std::size_t n = fragment.points.size();
        for (std::size_t i = 0; i < n; ++i) {
            const Carrier& c = fragment.carriers[i];
            if (c.onPlane && !IsBoxSite(PlaneLow(c.plane))) {
                cell_.segments.push_back(
                    Segment{c.plane, fragment.points[i], fragment.points[(i + 1) % n]});
            }
        }
        if (fill) {
            return;
        }
        // A plane through the mesh's own points cuts an edge at its end: the
        // cut point and the source point are one place. A fragment left with
        // no area there has given its segment, and is no face.
        if (diagram_.sliced) {
            for (std::size_t i = 0; fragment.points.size() >= 3 && i < fragment.points.size();) {
                const std::size_t j = (i + 1) % fragment.points.size();
                const Point& a = cell_.points[fragment.points[i]];
                const Point& b = cell_.points[fragment.points[j]];
                if (!(PositionBits(a.position) == PositionBits(b.position))) {
                    ++i;
                    continue;
                }
                // The source point stays, with the edge that leaves the place.
                if (a.key[0] == kSourceKey && b.key[0] != kSourceKey) {
                    fragment.points[j] = fragment.points[i];
                    fragment.weights[j] = fragment.weights[i];
                }
                fragment.points.erase(fragment.points.begin() + static_cast<std::ptrdiff_t>(i));
                fragment.weights.erase(fragment.weights.begin() + static_cast<std::ptrdiff_t>(i));
                fragment.carriers.erase(fragment.carriers.begin() + static_cast<std::ptrdiff_t>(i));
                i = 0;
            }
            if (fragment.points.size() < 3) {
                return;
            }
        }
        cell_.fragments.push_back(std::move(fragment));
    }

    const CellComplex& diagram_;
    std::vector<Target>& targets_;
    u32 site_;
    Cell& cell_;
    std::vector<CellPlane> planes_;
    /// Slices: per pair of the cell's planes that share an edge, its sites.
    std::map<std::pair<u64, u64>, std::array<u32, 3>> edges_;
    Vector3d low_{0, 0, 0};
    Vector3d high_{0, 0, 0};
};

// ============================================================================
// Phase 2: the inside faces, one cell face at a time
// ============================================================================

struct InsidePoint {
    Key key{};
    Vector3d position{0, 0, 0};
};

struct InsideSet {
    u64 plane = 0;
    u32 low = 0;  ///< The cell the faces face out of.
    u32 high = 0; ///< The cell across, which gets them reversed.
    u32 face = 0; ///< The face of `low`'s cell.
    Vector3d normal{0, 0, 0};
    std::vector<InsidePoint> points;
    std::vector<u32> triangles; ///< Counter-clockwise about `normal`.
    std::vector<u32> owner;     ///< Per triangle, its target.
    std::vector<f64> area;      ///< Per triangle.
    /// Per triangle, 1 where its plane does not reach: the pieces either side
    /// are one there.
    std::vector<u8> open;
    /// Points the triangulation took for one: a point, then the earlier one
    /// it is.
    std::vector<std::array<u32, 2>> merged;
    bool failed = false;
};

void BuildInside(InsideSet& set, const CellComplex& diagram, const Cell& work,
                 std::span<const Target> targets) {
    const ConvexCell& convex = diagram.cells[set.low];
    const CellFace& face = convex.faces[set.face];
    const HalfSpace plane = diagram.plane(set.plane);
    set.normal = plane.normal;

    std::unordered_map<Key, u32, KeyHash> index;
    const auto add = [&](const Key& key, const Vector3d& position) {
        const auto found = index.find(key);
        if (found != index.end()) {
            return found->second;
        }
        const u32 at = static_cast<u32>(set.points.size());
        index.emplace(key, at);
        set.points.push_back(InsidePoint{key, position});
        return at;
    };

    const std::size_t corners = face.loop.size();
    std::vector<u32> corner(corners);
    for (std::size_t k = 0; k < corners; ++k) {
        corner[k] = add(CornerKey(convex.sites[face.loop[k]]), convex.vertices[face.loop[k]]);
    }
    std::vector<u32> segments;
    std::vector<std::vector<std::pair<f64, u32>>> onEdge(corners);
    std::vector<std::array<u32, 3>> edgeSites(corners);
    for (std::size_t k = 0; k < corners; ++k) {
        const auto& a = convex.sites[face.loop[k]];
        const auto& b = convex.sites[face.loop[(k + 1) % corners]];
        std::array<u32, 3> shared{kNoSite, kNoSite, kNoSite};
        u32 count = 0;
        for (u32 s : a) {
            if (s != kNoSite && count < 3 && std::find(b.begin(), b.end(), s) != b.end()) {
                shared[count++] = s;
            }
        }
        edgeSites[k] = shared;
    }
    const auto place = [&](u32 point) {
        const Key& key = set.points[point].key;
        if (key[0] != kFaceKey) {
            return;
        }
        for (std::size_t k = 0; k < corners; ++k) {
            if (edgeSites[k][0] == key[3] && edgeSites[k][1] == key[4] &&
                edgeSites[k][2] == key[5]) {
                const Vector3d& a = set.points[corner[k]].position;
                const Vector3d& b = set.points[corner[(k + 1) % corners]].position;
                const f64 along = Dot(set.points[point].position - a, b - a);
                auto& list = onEdge[k];
                if (std::find_if(list.begin(), list.end(),
                                 [&](const auto& e) { return e.second == point; }) == list.end()) {
                    list.emplace_back(along, point);
                }
                return;
            }
        }
    };
    for (const Segment& s : work.segments) {
        if (s.plane != set.plane) {
            continue;
        }
        const u32 a = add(work.points[s.a].key, work.points[s.a].position);
        const u32 b = add(work.points[s.b].key, work.points[s.b].position);
        place(a);
        place(b);
        segments.push_back(a);
        segments.push_back(b);
    }
    for (std::size_t k = 0; k < corners; ++k) {
        auto& list = onEdge[k];
        std::sort(list.begin(), list.end());
        u32 from = corner[k];
        for (const auto& e : list) {
            segments.push_back(from);
            segments.push_back(e.second);
            from = e.second;
        }
        segments.push_back(from);
        segments.push_back(corner[(k + 1) % corners]);
    }

    // The plane's own basis, the same as the cell's cap.
    const Vector3d& n = plane.normal;
    const Vector3d axis = std::fabs(n.x) < 0.6 ? Vector3d(1, 0, 0) : Vector3d(0, 1, 0);
    Vector3d u = cross(axis, n);
    u = Times(u, 1.0 / Length(u));
    const Vector3d v = cross(n, u);
    Vector3d origin(0, 0, 0);
    for (u32 c : corner) {
        origin += set.points[c].position;
    }
    origin = Times(origin, 1.0 / static_cast<f64>(corners));
    const u32 inputCount = static_cast<u32>(set.points.size());
    std::vector<Vector2<f64>> flat(inputCount);
    for (u32 i = 0; i < inputCount; ++i) {
        const Vector3d r = set.points[i].position - origin;
        flat[i] = Vector2<f64>(Dot(r, u), Dot(r, v));
    }
    const Cdt2d cdt = ConstrainedTriangulation2d(flat, segments);
    if (!cdt.ok()) {
        set.failed = true;
        return;
    }
    for (u32 i = inputCount; i < cdt.points.size(); ++i) {
        const Vector2<f64>& p = cdt.points[i];
        set.points.push_back(InsidePoint{CrossingKey(set.plane, i - inputCount, diagram.sliced ? set.low + 1 : 0),
                                         origin + Times(u, p.x) + Times(v, p.y)});
    }
    for (u32 i = 0; i < inputCount && i < cdt.same.size(); ++i) {
        if (cdt.same[i] != i && cdt.same[i] < inputCount) {
            set.merged.push_back({i, cdt.same[i]});
        }
    }

    // Each region by the winding at the middle of its largest triangle.
    const std::size_t triCount = cdt.regions.size();
    std::vector<f64> area(triCount);
    std::vector<u32> largest(cdt.regionCount, kNone);
    for (std::size_t t = 0; t < triCount; ++t) {
        const Vector2<f64>& a = cdt.points[cdt.triangles[3 * t]];
        const Vector2<f64>& b = cdt.points[cdt.triangles[3 * t + 1]];
        const Vector2<f64>& c = cdt.points[cdt.triangles[3 * t + 2]];
        area[t] = 0.5 * ((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x));
        const u32 r = cdt.regions[t];
        if (largest[r] == kNone || area[t] > area[largest[r]]) {
            largest[r] = static_cast<u32>(t);
        }
    }
    std::vector<u32> owner(cdt.regionCount, kNone);
    for (u32 r = 0; r < cdt.regionCount; ++r) {
        if (largest[r] == kNone) {
            continue;
        }
        const u32 t = largest[r];
        const Vector3d middle =
            Times(set.points[cdt.triangles[3 * t]].position +
                      set.points[cdt.triangles[3 * t + 1]].position +
                      set.points[cdt.triangles[3 * t + 2]].position,
                  1.0 / 3.0);
        f64 best = 0.5;
        for (u32 ti = 0; ti < targets.size(); ++ti) {
            const Target& target = targets[ti];
            if (!target.usable || target.whole || target.winding == nullptr) {
                continue;
            }
            const Extent& box = target.winding->bounds;
            if (middle.x < box.minimum.x || middle.x > box.maximum.x ||
                middle.y < box.minimum.y || middle.y > box.maximum.y ||
                middle.z < box.minimum.z || middle.z > box.maximum.z) {
                continue;
            }
            const f64 w = WindingNumber(*target.winding, middle);
            if (w >= best && (owner[r] == kNone || w > best)) {
                best = w;
                owner[r] = ti;
            }
        }
    }
    // A plane cuts within its reach alone: a region not wholly inside one of
    // its rectangles joins the pieces either side of it again.
    std::vector<u8> reached(cdt.regionCount, 1);
    const u32 site = PlaneLow(set.plane);
    if (IsPlaneSite(site) && site - kPlaneSite < diagram.reach.size() &&
        !diagram.reach[site - kPlaneSite].empty()) {
        const std::vector<SliceReach>& reach = diagram.reach[site - kPlaneSite];
        std::vector<u8> within(static_cast<std::size_t>(cdt.regionCount) * reach.size(), 1);
        for (std::size_t t = 0; t < triCount; ++t) {
            for (u32 k = 0; k < 3; ++k) {
                const Vector3d& p = set.points[cdt.triangles[3 * t + k]].position;
                for (std::size_t q = 0; q < reach.size(); ++q) {
                    if (!reach[q].holds(p)) {
                        within[cdt.regions[t] * reach.size() + q] = 0;
                    }
                }
            }
        }
        for (u32 r = 0; r < cdt.regionCount; ++r) {
            reached[r] = 0;
            for (std::size_t q = 0; q < reach.size(); ++q) {
                reached[r] = reached[r] != 0 || within[r * reach.size() + q] != 0 ? 1 : 0;
            }
        }
    }
    for (std::size_t t = 0; t < triCount; ++t) {
        const u32 o = owner[cdt.regions[t]];
        if (o == kNone) {
            continue;
        }
        set.open.push_back(reached[cdt.regions[t]] != 0 ? 0 : 1);
        // A target wound inward gets its inside faces wound inward too.
        const bool inward = targets[o].winding->sign < 0.0;
        set.triangles.insert(set.triangles.end(),
                             {cdt.triangles[3 * t], cdt.triangles[3 * t + (inward ? 2 : 1)],
                              cdt.triangles[3 * t + (inward ? 1 : 2)]});
        set.owner.push_back(o);
        const Vector3d e1 = set.points[cdt.triangles[3 * t + 1]].position -
                            set.points[cdt.triangles[3 * t]].position;
        const Vector3d e2 = set.points[cdt.triangles[3 * t + 2]].position -
                            set.points[cdt.triangles[3 * t]].position;
        set.area.push_back(0.5 * Length(cross(e1, e2)));
    }
}

// ============================================================================
// Phase 3: the pieces of each cell
// ============================================================================

struct FaceRef {
    u32 cell = 0;
    bool inside = false;
    u32 index = 0; ///< A fragment of `cell`, or a triangle of `set`.
    u32 set = 0;
    bool reversed = false;
};

struct PieceWork {
    std::vector<FaceRef> faces;
    std::vector<u32> cells;
    u32 whole = kNone;
    u64 rank = 0;
};

struct UnionFind {
    std::vector<u32> parent;
    u32 add() {
        parent.push_back(static_cast<u32>(parent.size()));
        return static_cast<u32>(parent.size() - 1);
    }
    u32 find(u32 x) {
        while (parent[x] != x) {
            parent[x] = parent[parent[x]];
            x = parent[x];
        }
        return x;
    }
    void join(u32 a, u32 b) {
        a = find(a);
        b = find(b);
        if (a != b) {
            parent[std::max(a, b)] = std::min(a, b);
        }
    }
};

/// An outside face of a part, for `Parts::Touch`: its box, and the source
/// points whose bones it rides.
struct Shard {
    Vector3d low{0, 0, 0};
    Vector3d high{0, 0, 0};
    u32 target = 0;
    std::span<const u32> vertices;
};

bool Shares(const std::vector<u32>& a, const std::vector<u32>& b) {
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < a.size() && j < b.size()) {
        if (a[i] == b[j]) {
            return true;
        }
        a[i] < b[j] ? ++i : ++j;
    }
    return false;
}

/// Whether two faces ride together: a point of each on one bone, or on a bone
/// and its parent. A point bound to nothing rides with anything.
bool Rides(const Shard& a, const Shard& b, std::span<const Target> targets) {
    const Target& ta = targets[a.target];
    const Target& tb = targets[b.target];
    for (const u32 va : a.vertices) {
        if (va >= ta.rides.size() || ta.rides[va].empty()) {
            return true;
        }
        for (const u32 vb : b.vertices) {
            if (vb >= tb.rides.size() || tb.rides[vb].empty() || Shares(ta.rides[va], tb.ridesWide[vb]) ||
                Shares(ta.ridesWide[va], tb.rides[vb])) {
                return true;
            }
        }
    }
    return false;
}

/// `Parts::Touch`: whether parts with faces @p a and @p b (by `low.x`) are
/// one piece. Faces within @p reach of each other touch; the parts are one
/// when at least half of the touching pairs ride together.
bool Together(const std::vector<Shard>& a, const std::vector<Shard>& b, f64 reach, std::span<const Target> targets) {
    constexpr u32 kEnough = 4096;
    u32 pairs = 0;
    u32 riding = 0;
    for (const Shard& s : a) {
        const auto end = std::upper_bound(b.begin(), b.end(), s.high.x + reach,
                                          [](f64 x, const Shard& other) { return x < other.low.x; });
        for (auto other = b.begin(); other != end; ++other) {
            if (other->high.x < s.low.x - reach || other->low.y > s.high.y + reach || other->high.y < s.low.y - reach ||
                other->low.z > s.high.z + reach || other->high.z < s.low.z - reach) {
                continue;
            }
            ++pairs;
            riding += Rides(s, *other, targets) ? 1 : 0;
        }
        if (pairs >= kEnough) {
            break;
        }
    }
    return pairs > 0 && 2 * riding >= pairs;
}

void CellPieces(u32 c, const Cell& work, const std::vector<InsideSet>& sets,
                const std::vector<std::vector<u32>>& setsOf, std::vector<PieceWork>& out,
                const Vector3d& cellLow, const Vector3d& cellHigh, std::span<const Target> targets,
                const CutOptions& options) {
    std::vector<FaceRef> faces;
    for (u32 i = 0; i < work.fragments.size(); ++i) {
        faces.push_back(FaceRef{c, false, i, 0, false});
    }
    for (u32 s : setsOf[c]) {
        const InsideSet& set = sets[s];
        const bool reversed = set.high == c;
        for (u32 t = 0; t < set.owner.size(); ++t) {
            faces.push_back(FaceRef{c, true, t, s, reversed});
        }
    }
    if (faces.empty()) {
        return;
    }
    UnionFind nodes;
    std::unordered_map<Key, u32, KeyHash> byKey;
    std::unordered_map<PositionKey, u32, PositionHash> byPosition;
    const auto node = [&](const Key& key, const Vector3d& position) {
        auto found = byKey.find(key);
        u32 id;
        if (found == byKey.end()) {
            id = nodes.add();
            byKey.emplace(key, id);
        } else {
            id = found->second;
        }
        const auto [at, fresh] = byPosition.emplace(PositionBits(position), id);
        if (!fresh) {
            nodes.join(at->second, id);
        }
        return id;
    };
    std::vector<u32> faceNode(faces.size());
    for (std::size_t i = 0; i < faces.size(); ++i) {
        const FaceRef& ref = faces[i];
        u32 first = kNone;
        if (!ref.inside) {
            for (u32 p : work.fragments[ref.index].points) {
                const u32 id = node(work.points[p].key, work.points[p].position);
                if (first == kNone) {
                    first = id;
                } else {
                    nodes.join(first, id);
                }
            }
        } else {
            const InsideSet& set = sets[ref.set];
            for (u32 k = 0; k < 3; ++k) {
                const InsidePoint& p = set.points[set.triangles[3 * ref.index + k]];
                const u32 id = node(p.key, p.position);
                if (first == kNone) {
                    first = id;
                } else {
                    nodes.join(first, id);
                }
            }
        }
        faceNode[i] = first;
    }

    // Parts, numbered by first face.
    struct Part {
        std::vector<u32> faces;
        std::vector<u32> targets;
        Vector3d low{0, 0, 0};
        Vector3d high{0, 0, 0};
        bool any = false;
        u64 rank = ~0ull;
    };
    std::vector<Part> parts;
    std::unordered_map<u32, u32> partOf;
    const auto grow = [](Part& part, const Vector3d& p) {
        if (!part.any) {
            part.low = p;
            part.high = p;
            part.any = true;
            return;
        }
        part.low = Vector3d(std::min(part.low.x, p.x), std::min(part.low.y, p.y),
                            std::min(part.low.z, p.z));
        part.high = Vector3d(std::max(part.high.x, p.x), std::max(part.high.y, p.y),
                             std::max(part.high.z, p.z));
    };
    for (std::size_t i = 0; i < faces.size(); ++i) {
        const u32 root = nodes.find(faceNode[i]);
        auto [at, fresh] = partOf.emplace(root, static_cast<u32>(parts.size()));
        if (fresh) {
            parts.emplace_back();
        }
        Part& part = parts[at->second];
        part.faces.push_back(static_cast<u32>(i));
        const FaceRef& ref = faces[i];
        u32 target;
        if (!ref.inside) {
            const Fragment& fragment = work.fragments[ref.index];
            target = fragment.target;
            for (u32 p : fragment.points) {
                grow(part, work.points[p].position);
            }
            const u64 rank = (static_cast<u64>(target) << 32) | fragment.rank;
            part.rank = std::min(part.rank, rank);
        } else {
            const InsideSet& set = sets[ref.set];
            target = set.owner[ref.index];
            for (u32 k = 0; k < 3; ++k) {
                grow(part, set.points[set.triangles[3 * ref.index + k]].position);
            }
            const u64 rank = (u64{1} << 63) | (static_cast<u64>(ref.set) << 24) | ref.index;
            part.rank = std::min(part.rank, rank);
        }
        if (std::find(part.targets.begin(), part.targets.end(), target) == part.targets.end()) {
            part.targets.push_back(target);
        }
    }

    // Parts that touch or overlap break together (§5.6): a wall and its trim,
    // and the shells one mesh is built of, its shingles or planks. Parts apart,
    // two pillars, stay two pieces.
    const Vector3d size = cellHigh - cellLow;
    const f64 reach = 0.01 * Length(size);
    UnionFind joined;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        joined.add();
    }
    // `Parts::Touch`: each part's outside faces, made when first asked for.
    std::vector<std::vector<Shard>> shards(parts.size());
    std::vector<u8> listed(parts.size(), 0);
    const auto shardsOf = [&](std::size_t part) -> const std::vector<Shard>& {
        if (listed[part] != 0) {
            return shards[part];
        }
        listed[part] = 1;
        for (const u32 f : parts[part].faces) {
            const FaceRef& ref = faces[f];
            if (ref.inside) {
                continue;
            }
            const Fragment& fragment = work.fragments[ref.index];
            const Target& t = targets[fragment.target];
            Shard shard;
            shard.target = fragment.target;
            shard.vertices = fragment.triangle != kNone
                                 ? std::span<const u32>(&t.tri[3 * fragment.triangle], 3)
                                 : std::span<const u32>(&t.loopVertex[t.loopOffset[fragment.face]],
                                                        t.loopOffset[fragment.face + 1] - t.loopOffset[fragment.face]);
            shard.low = work.points[fragment.points[0]].position;
            shard.high = shard.low;
            for (const u32 p : fragment.points) {
                const Vector3d& at = work.points[p].position;
                shard.low = Vector3d(std::min(shard.low.x, at.x), std::min(shard.low.y, at.y),
                                     std::min(shard.low.z, at.z));
                shard.high = Vector3d(std::max(shard.high.x, at.x), std::max(shard.high.y, at.y),
                                      std::max(shard.high.z, at.z));
            }
            shards[part].push_back(shard);
        }
        std::sort(shards[part].begin(), shards[part].end(),
                  [](const Shard& l, const Shard& r) { return l.low.x < r.low.x; });
        return shards[part];
    };
    for (std::size_t i = 0; i < parts.size(); ++i) {
        for (std::size_t j = i + 1; j < parts.size(); ++j) {
            const Part& a = parts[i];
            const Part& b = parts[j];
            const bool apart = a.low.x > b.high.x + reach || b.low.x > a.high.x + reach ||
                               a.low.y > b.high.y + reach || b.low.y > a.high.y + reach ||
                               a.low.z > b.high.z + reach || b.low.z > a.high.z + reach;
            if (apart) {
                continue;
            }
            if (options.parts == Parts::Touch) {
                if (joined.find(static_cast<u32>(i)) == joined.find(static_cast<u32>(j)) ||
                    !Together(shardsOf(i), shardsOf(j), reach, targets)) {
                    continue;
                }
            }
            joined.join(static_cast<u32>(i), static_cast<u32>(j));
        }
    }
    std::map<u32, PieceWork> pieces;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        PieceWork& piece = pieces[joined.find(static_cast<u32>(i))];
        piece.cells = {c};
        piece.rank = piece.faces.empty() ? parts[i].rank : std::min(piece.rank, parts[i].rank);
        for (u32 f : parts[i].faces) {
            piece.faces.push_back(faces[f]);
        }
    }
    std::vector<PieceWork> ordered;
    for (auto& [root, piece] : pieces) {
        std::sort(piece.faces.begin(), piece.faces.end(), [](const FaceRef& l, const FaceRef& r) {
            if (l.inside != r.inside) {
                return !l.inside;
            }
            if (l.set != r.set) {
                return l.set < r.set;
            }
            return l.index < r.index;
        });
        ordered.push_back(std::move(piece));
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const PieceWork& l, const PieceWork& r) { return l.rank < r.rank; });
    for (PieceWork& piece : ordered) {
        out.push_back(std::move(piece));
    }
}

/**
 * @brief Closes each hole of @p t with triangles past its own.
 *
 * A cell face's regions are bounded by the segments the cut leaves on it. On
 * an open mesh, a building with no floor, the region inside the walls would
 * run out through the hole into the one outside, and be classified with it.
 * The fill's segments bound it instead; its fragments are dropped, so the
 * pieces are open exactly along the hole the source had.
 */
void FillHoles(Target& t) {
    const u32 faceCount = static_cast<u32>(t.faceLow.size());
    const u32 vertexCount = static_cast<u32>(t.positions.size());
    std::vector<u32> point(vertexCount);
    {
        std::unordered_map<PositionKey, u32, PositionHash> byPosition;
        u32 count = 0;
        for (u32 v = 0; v < vertexCount; ++v) {
            const auto [found, fresh] = byPosition.emplace(PositionBits(t.positions[v]), count);
            point[v] = found->second;
            count += fresh ? 1 : 0;
        }
    }
    // Point edges used once, each as the face runs it: a -> b.
    std::map<std::pair<u32, u32>, u32> uses;
    for (u32 f = 0; f < faceCount; ++f) {
        for (u32 c = t.loopOffset[f]; c < t.loopOffset[f + 1]; ++c) {
            const u32 a = point[t.loopVertex[c]];
            const u32 b = point[t.loopVertex[c + 1 < t.loopOffset[f + 1] ? c + 1 : t.loopOffset[f]]];
            ++uses[{std::min(a, b), std::max(a, b)}];
        }
    }
    // The fill runs each border edge the other way: b -> a, from b's vertex.
    std::multimap<u32, std::pair<u32, u32>> border; // point b -> (point a, vertex of b)
    for (u32 f = 0; f < faceCount; ++f) {
        for (u32 c = t.loopOffset[f]; c < t.loopOffset[f + 1]; ++c) {
            const u32 next = c + 1 < t.loopOffset[f + 1] ? c + 1 : t.loopOffset[f];
            const u32 a = point[t.loopVertex[c]];
            const u32 b = point[t.loopVertex[next]];
            if (a != b && uses[{std::min(a, b), std::max(a, b)}] == 1) {
                border.emplace(b, std::make_pair(a, t.loopVertex[next]));
            }
        }
    }
    const auto addFill = [&](u32 a, u32 b, u32 c) {
        t.tri.insert(t.tri.end(), {a, b, c});
        t.triCorner.insert(t.triCorner.end(), 3, HalfedgeId());
        const Vector3d pa = t.positions[a];
        const Vector3d pb = t.positions[b];
        const Vector3d pc = t.positions[c];
        t.fillLow.push_back(Vector3d(std::min({pa.x, pb.x, pc.x}), std::min({pa.y, pb.y, pc.y}),
                                     std::min({pa.z, pb.z, pc.z})));
        t.fillHigh.push_back(Vector3d(std::max({pa.x, pb.x, pc.x}), std::max({pa.y, pb.y, pc.y}),
                                      std::max({pa.z, pb.z, pc.z})));
    };
    std::vector<u32> loop;
    while (!border.empty()) {
        auto at = border.begin();
        const u32 start = at->first;
        loop.clear();
        u32 current = start;
        for (std::size_t guard = 0; guard <= vertexCount && at != border.end(); ++guard) {
            const auto [toPoint, vertex] = at->second;
            loop.push_back(vertex);
            border.erase(at);
            current = toPoint;
            if (current == start) {
                break;
            }
            at = border.find(current);
        }
        if (current != start || loop.size() < 3) {
            continue;
        }
        if (loop.size() == 3) {
            addFill(loop[0], loop[1], loop[2]);
            continue;
        }
        // A fan from the loop's middle, a point of the fill's own: linear in
        // the loop, where ear clipping a long crack's loop is not. The fill
        // only bounds the regions; nothing draws it.
        Vector3d middle(0, 0, 0);
        for (u32 v : loop) {
            middle += t.positions[v];
        }
        const u32 centre = static_cast<u32>(t.positions.size());
        t.positions.push_back(middle * (1.0 / static_cast<f64>(loop.size())));
        t.skin.emplace_back();
        t.merge.push_back(centre);
        for (std::size_t k = 0; k < loop.size(); ++k) {
            addFill(centre, loop[k], loop[(k + 1) % loop.size()]);
        }
    }
}

// ============================================================================
// Phase 4: measures, and the Smallest merges
// ============================================================================

struct Measure {
    f64 volume = 0.0;
    f64 area = 0.0;
    f64 insideArea = 0.0;
    Vector3d volumeMoment{0, 0, 0};
    Vector3d areaMoment{0, 0, 0};
    Vector3d low{0, 0, 0};
    Vector3d high{0, 0, 0};
    bool any = false;
};

void Accumulate(Measure& m, const Vector3d& r, const Vector3d& a, const Vector3d& b,
                const Vector3d& c, bool inside) {
    const Vector3d e = cross(b - a, c - a);
    const f64 area = 0.5 * Length(e);
    if (inside) {
        m.insideArea += area;
    } else {
        m.area += area;
        m.areaMoment += Times(a + b + c, area / 3.0);
    }
    const f64 v = Dot(a - r, cross(b - r, c - r)) / 6.0;
    m.volume += v;
    m.volumeMoment += Times(r + a + b + c, v / 4.0);
}

template <class Each>
void ForEachTriangle(const PieceWork& piece, const std::vector<Cell>& cells,
                     const std::vector<InsideSet>& sets, Each&& each) {
    for (const FaceRef& ref : piece.faces) {
        if (!ref.inside) {
            const Cell& cell = cells[ref.cell];
            const Fragment& fragment = cell.fragments[ref.index];
            const Vector3d& a = cell.points[fragment.points[0]].position;
            for (std::size_t k = 1; k + 1 < fragment.points.size(); ++k) {
                each(a, cell.points[fragment.points[k]].position,
                     cell.points[fragment.points[k + 1]].position, false);
            }
        } else {
            const InsideSet& set = sets[ref.set];
            const u32* t = &set.triangles[3 * ref.index];
            if (ref.reversed) {
                each(set.points[t[0]].position, set.points[t[2]].position,
                     set.points[t[1]].position, true);
            } else {
                each(set.points[t[0]].position, set.points[t[1]].position,
                     set.points[t[2]].position, true);
            }
        }
    }
}

Measure MeasurePiece(const PieceWork& piece, const std::vector<Cell>& cells,
                     const std::vector<InsideSet>& sets) {
    Measure m;
    ForEachTriangle(piece, cells, sets, [&](const Vector3d& a, const Vector3d& b,
                                            const Vector3d& c, bool) {
        for (const Vector3d* p : {&a, &b, &c}) {
            if (!m.any) {
                m.low = *p;
                m.high = *p;
                m.any = true;
            }
            m.low = Vector3d(std::min(m.low.x, p->x), std::min(m.low.y, p->y),
                             std::min(m.low.z, p->z));
            m.high = Vector3d(std::max(m.high.x, p->x), std::max(m.high.y, p->y),
                              std::max(m.high.z, p->z));
        }
    });
    const Vector3d r = Times(m.low + m.high, 0.5);
    ForEachTriangle(piece, cells, sets, [&](const Vector3d& a, const Vector3d& b,
                                            const Vector3d& c, bool inside) {
        Accumulate(m, r, a, b, c, inside);
    });
    return m;
}

void Describe(CutPiece& out, const Measure& m, f64 band) {
    out.volume = std::fabs(m.volume);
    out.area = m.area;
    out.insideArea = m.insideArea;
    const f64 sheet = m.area * band;
    out.solid = out.volume > 0.5 * sheet;
    out.measure = out.solid ? out.volume : sheet;
    if (out.solid && m.volume != 0.0) {
        out.centroid = Times(m.volumeMoment, 1.0 / m.volume);
    } else if (m.area > 0.0) {
        out.centroid = Times(m.areaMoment, 1.0 / m.area);
    } else {
        out.centroid = Times(m.low + m.high, 0.5);
    }
}

// ============================================================================
// Phase 5: the meshes
// ============================================================================

struct OutVertex {
    Vector3d position{0, 0, 0};
    u32 piece = 0;
    std::array<u32, 3> from{kNone, kNone, kNone};
    std::array<f64, 3> weight{0, 0, 0};
    bool cut = false;
    u32 weld = 0; ///< Source merge group + 1, or 0.
    u32 group = 0;
};

enum class CornerKind : u8 { Whole, Fragment, Inside };

struct OutFace {
    u32 piece = 0;
    CornerKind kind = CornerKind::Whole;
    u32 sourceFace = kNone;
    u32 triangle = kNone;
    u32 section = 0;
    Vector3d normal{0, 0, 0};
    u64 plane = 0; ///< Inside: the cell face it lies on.
    std::vector<u32> vertices;
    std::vector<HalfedgeId> sourceCorner;      ///< Whole: each corner's source halfedge.
    std::vector<std::array<f64, 3>> weights;   ///< Fragment: each corner on the triangle.
    std::vector<std::pair<u32, u32>> edgeFrom; ///< Per edge, the source edge it lies on.
};

/// Blends Vertex layer @p layer of @p source at @p from by @p weight into
/// @p out at @p to: F32 kinds and colours by weight, the rest the heaviest's.
void BlendVertexLayer(const AttrLayer& source, AttrLayer& out, const OutVertex& v, u32 to) {
    const u32 stride = AttrTypeSize(source.type);
    if (stride == 0 || (to + 1) * stride > out.data.size()) {
        return;
    }
    u32 heaviest = 0;
    for (u32 i = 1; i < 3; ++i) {
        if (v.from[i] != kNone && v.weight[i] > v.weight[heaviest]) {
            heaviest = i;
        }
    }
    u8* target = out.data.data() + static_cast<std::size_t>(to) * stride;
    const auto at = [&](u32 vertex) {
        return source.data.data() + static_cast<std::size_t>(vertex) * stride;
    };
    const bool floats = source.type == AttrType::F32 || source.type == AttrType::F32x2 ||
                        source.type == AttrType::F32x3 || source.type == AttrType::F32x4;
    if (floats) {
        const u32 components = AttrTypeComponents(source.type);
        for (u32 c = 0; c < components; ++c) {
            f64 sum = 0.0;
            for (u32 i = 0; i < 3; ++i) {
                if (v.from[i] != kNone && (v.from[i] + 1) * stride <= source.data.size()) {
                    f32 value;
                    std::memcpy(&value, at(v.from[i]) + 4 * c, 4);
                    sum += v.weight[i] * value;
                }
            }
            const f32 value = static_cast<f32>(sum);
            std::memcpy(target + 4 * c, &value, 4);
        }
        return;
    }
    if (source.type == AttrType::U8x4) {
        for (u32 c = 0; c < 4; ++c) {
            f64 sum = 0.0;
            for (u32 i = 0; i < 3; ++i) {
                if (v.from[i] != kNone && (v.from[i] + 1) * stride <= source.data.size()) {
                    sum += v.weight[i] * at(v.from[i])[c];
                }
            }
            target[c] = static_cast<u8>(std::clamp(std::lround(sum), 0l, 255l));
        }
        return;
    }
    const u32 h = v.from[heaviest];
    if (h != kNone && (h + 1) * stride <= source.data.size()) {
        std::memcpy(target, at(h), stride);
    }
}

bool IsFractureLayer(const std::string& name) {
    return name.rfind("fracture.", 0) == 0;
}

Mesh Assemble(const Target& t, std::vector<OutVertex>& vertices, std::vector<OutFace>& faces,
              std::vector<u32>& survivor) {
    const Mesh& source = t.mesh;
    FaceSet set;
    set.vertexCount = static_cast<u32>(vertices.size());
    for (const OutFace& f : faces) {
        set.addFace(f.vertices);
    }
    // A cut can leave overlapping shells' faces meeting on one edge: the repair
    // splits rather than refuses, and drops no face that repeats no corner.
    RepairResult repaired = Repair(set);
    for (const VertexSplit& split : repaired.log.splits) {
        if (split.created >= vertices.size()) {
            vertices.resize(split.created + 1);
        }
        vertices[split.created] = vertices[split.original];
    }
    survivor.clear();
    {
        std::size_t dropped = 0;
        for (u32 f = 0; f < faces.size(); ++f) {
            if (dropped < repaired.log.droppedFaces.size() &&
                repaired.log.droppedFaces[dropped].index == f) {
                ++dropped;
                continue;
            }
            survivor.push_back(f);
        }
    }

    Mesh mesh;
    mesh.name = source.name;
    mesh.lodLevel = source.lodLevel;
    mesh.sections = source.sections;
    mesh.setFaceSet(repaired.faces);
    if (!mesh.ensureConnectivity().ok()) {
        return Mesh{};
    }
    const u32 vertexCount = static_cast<u32>(vertices.size());
    for (const AttrLayer& layer : source.attributes.layers()) {
        if (IsFractureLayer(layer.name)) {
            continue;
        }
        mesh.attributes.create(layer.name, layer.domain, layer.type, layer.storage);
    }
    // Mesh-domain layers come across whole.
    for (const AttrLayer& layer : source.attributes.layers()) {
        if (layer.domain == Domain::Mesh) {
            AttrLayer* out = mesh.attributes.layer(layer.name, Domain::Mesh);
            if (out != nullptr && out->data.size() == layer.data.size()) {
                out->data = layer.data;
            }
        }
    }

    // --- vertices ---
    {
        auto position = mesh.attributes.getOrCreate<Vector3f>(names::kPosition, Domain::Vertex,
                                                              AttrType::F32x3);
        for (u32 v = 0; v < vertexCount; ++v) {
            const Vector3d& p = vertices[v].position;
            position[v] = Vector3f(static_cast<f32>(p.x), static_cast<f32>(p.y),
                                   static_cast<f32>(p.z));
        }
    }
    for (const AttrLayer& layer : source.attributes.layers()) {
        if (layer.domain != Domain::Vertex || layer.name == names::kPosition ||
            layer.name == names::kMergeGroup || IsFractureLayer(layer.name)) {
            continue;
        }
        AttrLayer* out = mesh.attributes.layer(layer.name, Domain::Vertex);
        if (out == nullptr) {
            continue;
        }
        for (u32 v = 0; v < vertexCount; ++v) {
            BlendVertexLayer(layer, *out, vertices[v], v);
        }
    }
    {
        auto groups = MergeGroupsOf(mesh);
        auto cut = mesh.attributes.getOrCreate<u8>(names::kFractureCut, Domain::Vertex,
                                                   AttrType::Bool);
        auto weld = mesh.attributes.getOrCreate<u32>(names::kFractureWeld, Domain::Vertex,
                                                     AttrType::U32);
        for (u32 v = 0; v < vertexCount; ++v) {
            groups[v] = vertices[v].group;
            cut[v] = vertices[v].cut ? 1 : 0;
            weld[v] = vertices[v].weld;
        }
    }
    bool skinned = false;
    std::vector<std::vector<Influence>> skin(vertexCount);
    for (u32 v = 0; v < vertexCount; ++v) {
        const OutVertex& o = vertices[v];
        if (o.from[0] == kNone) {
            continue;
        }
        if (o.from[1] == kNone) {
            skin[v] = t.skin[o.from[0]];
        } else {
            std::vector<std::vector<Influence>> lists;
            std::vector<f32> w;
            for (u32 i = 0; i < 3; ++i) {
                if (o.from[i] != kNone) {
                    lists.push_back(t.skin[o.from[i]]);
                    w.push_back(static_cast<f32>(o.weight[i]));
                }
            }
            skin[v] = BlendInfluences(lists, w);
        }
        skinned = skinned || !skin[v].empty();
    }
    if (skinned) {
        mesh.skin.reset(0);
        mesh.skin.offsets.assign(1, 0);
        for (const auto& list : skin) {
            mesh.skin.appendVertex(list);
        }
        mesh.skin.sortByWeight();
    }

    // --- faces and corners ---
    const Topology& topology = std::as_const(mesh).topology();
    const Topology& sourceTopology = source.topology();
    auto sections = mesh.faceSections();
    auto made = mesh.attributes.getOrCreate<u8>(names::kFractureMade, Domain::Face, AttrType::Bool);
    auto from = mesh.attributes.getOrCreate<u32>(names::kFractureSource, Domain::Face,
                                                 AttrType::U32);
    auto normal = mesh.attributes.get<Vector3f>(names::kNormal, Domain::Halfedge);
    // Every layer is made by now, so these pointers hold.
    using LayerPair = std::pair<const AttrLayer*, AttrLayer*>;
    std::vector<LayerPair> faceLayers;
    std::vector<LayerPair> cornerLayers;
    std::vector<LayerPair> edgeLayers;
    for (const AttrLayer& layer : source.attributes.layers()) {
        if (IsFractureLayer(layer.name)) {
            continue;
        }
        AttrLayer* out = mesh.attributes.layer(layer.name, layer.domain);
        if (out == nullptr) {
            continue;
        }
        if (layer.domain == Domain::Face && layer.name != names::kSection) {
            faceLayers.emplace_back(&layer, out);
        } else if (layer.domain == Domain::Halfedge) {
            cornerLayers.emplace_back(&layer, out);
        } else if (layer.domain == Domain::Edge) {
            edgeLayers.emplace_back(&layer, out);
        }
    }
    const auto copyElement = [](const AttrLayer* sourceLayer, AttrLayer* outLayer, u32 fromIndex,
                                u32 toIndex) {
        const AttrLayer& s = *sourceLayer;
        AttrLayer& o = *outLayer;
        const u32 stride = AttrTypeSize(s.type);
        if ((fromIndex + 1) * static_cast<std::size_t>(stride) <= s.data.size() &&
            (toIndex + 1) * static_cast<std::size_t>(stride) <= o.data.size()) {
            std::memcpy(o.data.data() + static_cast<std::size_t>(toIndex) * stride,
                        s.data.data() + static_cast<std::size_t>(fromIndex) * stride, stride);
        }
    };
    const auto sourceMade = source.attributes.get<u8>(names::kFractureMade, Domain::Face);
    std::unordered_map<u32, SourcePolygon> captured;
    FaceTriangulationBuilder rows(static_cast<u32>(survivor.size()));
    bool anyRow = false;
    std::vector<u32> outRow;
    for (u32 outFace = 0; outFace < survivor.size(); ++outFace) {
        const OutFace& f = faces[survivor[outFace]];
        sections[outFace] = f.section;
        made[outFace] = f.kind == CornerKind::Inside ||
                                (f.sourceFace < sourceMade.size() && sourceMade[f.sourceFace] != 0)
                            ? 1
                            : 0;
        from[outFace] = f.sourceFace != kNone ? f.sourceFace + 1 : 0;
        if (f.sourceFace != kNone) {
            for (const auto& [s, o] : faceLayers) {
                copyElement(s, o, f.sourceFace, outFace);
            }
        }
        HalfedgeId h = topology.halfedge(FaceId(outFace));
        const std::size_t valence = f.vertices.size();
        for (std::size_t k = 0; k < valence; ++k) {
            if (f.kind == CornerKind::Whole) {
                for (const auto& [s, o] : cornerLayers) {
                    copyElement(s, o, f.sourceCorner[k].value(), h.value());
                }
            } else if (f.kind == CornerKind::Fragment) {
                // A corner on a source corner takes its bytes, so a rejoin is exact.
                u32 exact = kNone;
                for (u32 j = 0; j < 3; ++j) {
                    if (f.weights[k][j] == 1.0 && f.weights[k][(j + 1) % 3] == 0.0 &&
                        f.weights[k][(j + 2) % 3] == 0.0) {
                        exact = j;
                    }
                }
                if (exact != kNone) {
                    for (const auto& [s, o] : cornerLayers) {
                        copyElement(s, o, t.triCorner[3 * f.triangle + exact].value(), h.value());
                    }
                } else {
                    auto found = captured.find(f.triangle);
                    if (found == captured.end()) {
                        const HalfedgeId corners[3] = {t.triCorner[3 * f.triangle],
                                                       t.triCorner[3 * f.triangle + 1],
                                                       t.triCorner[3 * f.triangle + 2]};
                        found = captured.emplace(f.triangle, CaptureCorners(source, corners)).first;
                    }
                    const f32 w[3] = {static_cast<f32>(f.weights[k][0]),
                                      static_cast<f32>(f.weights[k][1]),
                                      static_cast<f32>(f.weights[k][2])};
                    const HalfedgeId target[1] = {h};
                    BlendCorners(mesh, found->second, w, target);
                }
            } else if (h.index() < normal.size()) {
                normal[h.index()] = Vector3f(static_cast<f32>(f.normal.x),
                                             static_cast<f32>(f.normal.y),
                                             static_cast<f32>(f.normal.z));
            }
            const auto [u, v] = f.edgeFrom[k];
            if (u != kNone && !edgeLayers.empty()) {
                const HalfedgeId sh = sourceTopology.findHalfedge(VertexId(u), VertexId(v));
                if (sh.valid()) {
                    const u32 se = Topology::edge(sh).value();
                    const u32 oe = Topology::edge(h).value();
                    for (const auto& [s, o] : edgeLayers) {
                        copyElement(s, o, se, oe);
                    }
                }
            }
            h = topology.next(h);
        }
        // A whole polygon keeps the triangles it was drawn as.
        if (f.kind == CornerKind::Whole && valence > 3) {
            const auto row = source.triangulation.row(f.sourceFace);
            if (!row.empty()) {
                outRow.clear();
                for (u32 sv : row) {
                    for (std::size_t k = 0; k < valence; ++k) {
                        if (t.loopVertex[t.loopOffset[f.sourceFace] + k] == sv) {
                            outRow.push_back(f.vertices[k]);
                            break;
                        }
                    }
                }
                if (outRow.size() == row.size()) {
                    rows.set(outFace, outRow);
                    anyRow = true;
                }
            }
        }
    }
    if (anyRow) {
        mesh.triangulation = rows.build();
    }
    MaterialiseRows(mesh);
    mesh.recomputeBounds();
    return mesh;
}

/**
 * @brief Points the triangulation took for one, made one everywhere.
 *
 * Slices put planes along a mesh's own symmetry: two planes' line through a
 * mesh edge, where the edge's two cut points and the line's own come out a
 * hair apart. A cell face's triangulation snaps such points together, so its
 * inside faces would part from the outside there. Every point of a key the
 * snap joined takes one place, in every cell and on every face, and a
 * fragment or triangle left with no area goes.
 */
void UniteClosePoints(std::vector<Cell>& cells, std::vector<InsideSet>& sets) {
    std::unordered_map<Key, u32, KeyHash> ids;
    std::vector<Vector3d> at;
    UnionFind classes;
    const auto id = [&](const InsidePoint& p) {
        const auto [found, fresh] = ids.emplace(p.key, static_cast<u32>(at.size()));
        if (fresh) {
            at.push_back(p.position);
            classes.add();
        }
        return found->second;
    };
    for (const InsideSet& set : sets) {
        for (const std::array<u32, 2>& pair : set.merged) {
            const u32 a = id(set.points[pair[0]]);
            const u32 b = id(set.points[pair[1]]);
            classes.join(a, b);
        }
    }
    if (ids.empty()) {
        return;
    }
    const auto move = [&](const Key& key, Vector3d& position) {
        const auto found = ids.find(key);
        if (found != ids.end()) {
            position = at[classes.find(found->second)];
        }
    };
    const auto same = [](const Vector3d& a, const Vector3d& b) { return PositionBits(a) == PositionBits(b); };
    for (Cell& cell : cells) {
        for (Point& p : cell.points) {
            move(p.key, p.position);
        }
        std::vector<Fragment> kept;
        for (Fragment& f : cell.fragments) {
            for (std::size_t i = 0; f.points.size() >= 3 && i < f.points.size();) {
                const std::size_t j = (i + 1) % f.points.size();
                if (!same(cell.points[f.points[i]].position, cell.points[f.points[j]].position)) {
                    ++i;
                    continue;
                }
                const auto offset = static_cast<std::ptrdiff_t>(i);
                f.points.erase(f.points.begin() + offset);
                f.carriers.erase(f.carriers.begin() + offset);
                if (!f.weights.empty()) {
                    f.weights.erase(f.weights.begin() + offset);
                }
                i = 0;
            }
            if (f.points.size() >= 3) {
                kept.push_back(std::move(f));
            }
        }
        cell.fragments = std::move(kept);
    }
    for (InsideSet& set : sets) {
        for (InsidePoint& p : set.points) {
            move(p.key, p.position);
        }
        std::vector<u32> triangles;
        std::vector<u32> owner;
        std::vector<f64> area;
        std::vector<u8> open;
        for (std::size_t t = 0; t < set.owner.size(); ++t) {
            const Vector3d& a = set.points[set.triangles[3 * t]].position;
            const Vector3d& b = set.points[set.triangles[3 * t + 1]].position;
            const Vector3d& c = set.points[set.triangles[3 * t + 2]].position;
            if (same(a, b) || same(b, c) || same(c, a)) {
                continue;
            }
            triangles.insert(triangles.end(), {set.triangles[3 * t], set.triangles[3 * t + 1], set.triangles[3 * t + 2]});
            owner.push_back(set.owner[t]);
            area.push_back(set.area[t]);
            open.push_back(t < set.open.size() ? set.open[t] : 0);
        }
        set.triangles = std::move(triangles);
        set.owner = std::move(owner);
        set.area = std::move(area);
        set.open = std::move(open);
    }
}

/**
 * @brief The pieces either side of where a plane does not reach, made one.
 *
 * Every inside triangle its plane does not reach joins the two pieces that
 * hold its sides. Then the inside faces between a piece and itself go: the
 * unreached ones, and those of a cut that ends inside the solid, which parts
 * nothing. Returns how many pieces fewer there are.
 */
u32 JoinUnreached(std::vector<PieceWork>& pieces, const std::vector<InsideSet>& sets) {
    std::vector<std::vector<u32>> side(sets.size());
    for (std::size_t s = 0; s < sets.size(); ++s) {
        side[s].assign(2 * sets[s].owner.size(), kNone);
    }
    for (u32 p = 0; p < pieces.size(); ++p) {
        for (const FaceRef& ref : pieces[p].faces) {
            if (ref.inside) {
                side[ref.set][2 * ref.index + (ref.reversed ? 1 : 0)] = p;
            }
        }
    }
    UnionFind one;
    for (std::size_t p = 0; p < pieces.size(); ++p) {
        one.add();
    }
    for (std::size_t s = 0; s < sets.size(); ++s) {
        for (std::size_t t = 0; t < sets[s].open.size(); ++t) {
            const u32 a = side[s][2 * t];
            const u32 b = side[s][2 * t + 1];
            if (sets[s].open[t] != 0 && a != kNone && b != kNone) {
                one.join(a, b);
            }
        }
    }
    // A class's lowest piece is its root, and comes first.
    std::vector<PieceWork> kept;
    std::vector<u32> place(pieces.size(), kNone);
    for (u32 p = 0; p < pieces.size(); ++p) {
        const u32 root = one.find(p);
        if (root == p) {
            place[p] = static_cast<u32>(kept.size());
            kept.push_back(std::move(pieces[p]));
            continue;
        }
        PieceWork& into = kept[place[root]];
        PieceWork& gone = pieces[p];
        into.faces.insert(into.faces.end(), gone.faces.begin(), gone.faces.end());
        for (const u32 c : gone.cells) {
            if (std::find(into.cells.begin(), into.cells.end(), c) == into.cells.end()) {
                into.cells.push_back(c);
            }
        }
        into.rank = std::min(into.rank, gone.rank);
    }
    for (PieceWork& piece : kept) {
        std::erase_if(piece.faces, [&](const FaceRef& ref) {
            if (!ref.inside) {
                return false;
            }
            if (ref.index < sets[ref.set].open.size() && sets[ref.set].open[ref.index] != 0) {
                return true;
            }
            const u32 a = side[ref.set][2 * ref.index];
            const u32 b = side[ref.set][2 * ref.index + 1];
            return a != kNone && b != kNone && one.find(a) == one.find(b);
        });
    }
    const u32 joined = static_cast<u32>(pieces.size() - kept.size());
    pieces = std::move(kept);
    return joined;
}

/**
 * @brief What a join left in fragments, made whole again.
 *
 * The regions are the whole planes', so a plane cuts every face it crosses,
 * also where it does not reach and its two sides are one piece. Each source
 * triangle's fragments in one piece are made one face again: their union's
 * outline, less the cut points no other piece holds. Such a point lies on a
 * source edge, between its neighbours, and every face round it drops it. A
 * union that is not one loop (a hole cut out of a triangle) stays in its
 * fragments, and keeps their points in the faces beside it. A face left with
 * each of its triangles whole is the source face again.
 *
 * The faces it makes go in a cell of their own, past @p cells'.
 */
void HealJoined(std::vector<PieceWork>& pieces, const std::vector<bool>& alive, std::vector<Cell>& cells,
                std::span<const Target> targets) {
    constexpr u32 kMany = 0xFFFFFFFEu;
    // Where the points of more than one piece stand: the cracks.
    std::unordered_map<PositionKey, u32, PositionHash> holder;
    for (u32 p = 0; p < pieces.size(); ++p) {
        if (!alive[p]) {
            continue;
        }
        for (const FaceRef& ref : pieces[p].faces) {
            if (ref.inside) {
                continue;
            }
            const Cell& cell = cells[ref.cell];
            for (const u32 point : cell.fragments[ref.index].points) {
                const auto [found, fresh] = holder.emplace(PositionBits(cell.points[point].position), p);
                if (!fresh && found->second != p) {
                    found->second = kMany;
                }
            }
        }
    }

    struct Corner {
        Point point;
        std::array<f64, 3> weight{0, 0, 0};
        Carrier carrier; ///< Of the edge that leaves it.
    };
    struct Group {
        u32 target = 0;
        u32 triangle = 0;
        u32 face = 0;
        std::vector<u32> refs; ///< Into the piece's faces.
        std::vector<Corner> loop;
        bool single = false;
    };
    const auto isSource = [](const Point& point) { return point.key[0] == kSourceKey; };
    std::vector<std::vector<Group>> groupsOf(pieces.size());
    std::unordered_map<PositionKey, u8, PositionHash> pinned;
    for (u32 p = 0; p < pieces.size(); ++p) {
        if (!alive[p]) {
            continue;
        }
        std::map<std::pair<u32, u32>, std::size_t> index;
        std::vector<Group>& groups = groupsOf[p];
        for (u32 r = 0; r < pieces[p].faces.size(); ++r) {
            const FaceRef& ref = pieces[p].faces[r];
            if (ref.inside) {
                continue;
            }
            const Fragment& fragment = cells[ref.cell].fragments[ref.index];
            if (fragment.triangle == kNone) {
                continue;
            }
            const auto [at, fresh] = index.emplace(std::make_pair(fragment.target, fragment.triangle), groups.size());
            if (fresh) {
                groups.emplace_back();
                groups.back().target = fragment.target;
                groups.back().triangle = fragment.triangle;
                groups.back().face = fragment.face;
            }
            groups[at->second].refs.push_back(r);
        }
        for (Group& group : groups) {
            // The fragments' points by place, a source point standing for its place.
            std::unordered_map<PositionKey, u32, PositionHash> local;
            std::vector<Corner> points;
            struct Edge {
                u32 from;
                u32 to;
                Carrier carrier;
            };
            std::vector<Edge> edges;
            std::map<std::pair<u32, u32>, i32> count;
            std::vector<u32> ids;
            for (const u32 r : group.refs) {
                const FaceRef& ref = pieces[p].faces[r];
                const Cell& cell = cells[ref.cell];
                const Fragment& fragment = cell.fragments[ref.index];
                const std::size_t n = fragment.points.size();
                ids.resize(n);
                for (std::size_t k = 0; k < n; ++k) {
                    const Point& at = cell.points[fragment.points[k]];
                    const auto [found, fresh] = local.emplace(PositionBits(at.position), static_cast<u32>(points.size()));
                    if (fresh) {
                        points.push_back(Corner{at, fragment.weights[k], Carrier{}});
                    } else if (isSource(at) && !isSource(points[found->second].point)) {
                        points[found->second] = Corner{at, fragment.weights[k], Carrier{}};
                    }
                    ids[k] = found->second;
                }
                for (std::size_t k = 0; k < n; ++k) {
                    const u32 a = ids[k];
                    const u32 b = ids[(k + 1) % n];
                    if (a != b) {
                        edges.push_back(Edge{a, b, fragment.carriers[k]});
                        ++count[{a, b}];
                    }
                }
            }
            // Their union's outline: each edge its reverse does not cancel.
            std::map<u32, std::vector<u32>> leaving;
            std::map<std::pair<u32, u32>, i32> used;
            for (u32 e = 0; e < edges.size(); ++e) {
                const auto key = std::make_pair(edges[e].from, edges[e].to);
                const auto back = count.find({edges[e].to, edges[e].from});
                const i32 net = count[key] - (back == count.end() ? 0 : back->second);
                if (net > 0 && used[key] < net) {
                    ++used[key];
                    leaving[edges[e].from].push_back(e);
                }
            }
            bool single = !leaving.empty();
            for (const auto& [from, list] : leaving) {
                single = single && list.size() == 1;
            }
            std::vector<u32> walk;
            if (single) {
                u32 e = leaving.begin()->second.front();
                const u32 start = edges[e].from;
                for (std::size_t guard = 0; guard <= leaving.size(); ++guard) {
                    walk.push_back(e);
                    const auto next = leaving.find(edges[e].to);
                    if (next == leaving.end()) {
                        single = false;
                        break;
                    }
                    e = next->second.front();
                    if (edges[e].from == start) {
                        break;
                    }
                }
                single = single && walk.size() == leaving.size() && walk.size() >= 3;
            }
            group.single = single;
            if (!single) {
                for (const Corner& corner : points) {
                    pinned.emplace(PositionBits(corner.point.position), u8{1});
                }
                continue;
            }
            for (const u32 e : walk) {
                Corner corner = points[edges[e].from];
                corner.carrier = edges[e].carrier;
                group.loop.push_back(std::move(corner));
            }
        }
    }

    Cell healed;
    const u32 healedCell = static_cast<u32>(cells.size());
    for (u32 p = 0; p < pieces.size(); ++p) {
        if (!alive[p] || groupsOf[p].empty()) {
            continue;
        }
        // Each source triangle's face in this piece now: a fragment it had, or
        // one made here.
        struct Now {
            u32 target = 0;
            u32 face = 0;
            bool whole = false; ///< The triangle itself.
            bool made = false;
            Fragment fragment;       ///< When made.
            std::vector<u32> refs;   ///< When not: the fragments it stays in.
        };
        std::vector<Now> now;
        std::vector<u8> consumed(pieces[p].faces.size(), 0);
        for (const Group& group : groupsOf[p]) {
            Now entry;
            entry.target = group.target;
            entry.face = group.face;
            std::vector<Corner> loop;
            if (group.single) {
                for (const Corner& corner : group.loop) {
                    const PositionKey at = PositionBits(corner.point.position);
                    const auto held = holder.find(at);
                    if (isSource(corner.point) || (held != holder.end() && held->second == kMany) ||
                        pinned.find(at) != pinned.end()) {
                        loop.push_back(corner);
                    }
                }
            }
            const bool changed = group.single && loop.size() >= 3 &&
                                 (group.refs.size() > 1 || loop.size() != group.loop.size());
            if (!changed) {
                entry.refs = group.refs;
                if (group.refs.size() == 1) {
                    const FaceRef& ref = pieces[p].faces[group.refs[0]];
                    const Cell& cell = cells[ref.cell];
                    const Fragment& fragment = cell.fragments[ref.index];
                    entry.whole = fragment.points.size() == 3 &&
                                  std::all_of(fragment.points.begin(), fragment.points.end(),
                                              [&](u32 point) { return isSource(cell.points[point]); });
                }
                now.push_back(std::move(entry));
                continue;
            }
            for (const u32 r : group.refs) {
                consumed[r] = 1;
            }
            entry.made = true;
            entry.fragment.target = group.target;
            entry.fragment.face = group.face;
            entry.fragment.triangle = group.triangle;
            entry.fragment.rank = group.triangle;
            entry.whole = loop.size() == 3;
            for (const Corner& corner : loop) {
                entry.whole = entry.whole && isSource(corner.point);
                entry.fragment.points.push_back(healed.add(corner.point));
                entry.fragment.weights.push_back(corner.weight);
                entry.fragment.carriers.push_back(corner.carrier);
            }
            now.push_back(std::move(entry));
        }
        // A face whose every triangle is whole here is the source face again.
        std::map<std::pair<u32, u32>, std::vector<std::size_t>> byFace;
        for (std::size_t i = 0; i < now.size(); ++i) {
            byFace[{now[i].target, now[i].face}].push_back(i);
        }
        std::vector<FaceRef> made;
        for (const auto& [key, list] : byFace) {
            const Target& t = targets[key.first];
            const u32 f = key.second;
            const bool whole = list.size() == t.triFirst[f + 1] - t.triFirst[f] &&
                               std::all_of(list.begin(), list.end(), [&](std::size_t i) { return now[i].whole; });
            if (whole) {
                Fragment fragment;
                fragment.target = key.first;
                fragment.face = f;
                fragment.rank = t.triFirst[f];
                for (u32 c = t.loopOffset[f]; c < t.loopOffset[f + 1]; ++c) {
                    const u32 v = t.loopVertex[c];
                    Point point;
                    point.key = SourceKey(key.first, v);
                    point.position = t.positions[v];
                    point.target = key.first;
                    point.from = {v, kNone, kNone};
                    point.weight = {1.0, 0.0, 0.0};
                    fragment.points.push_back(healed.add(point));
                    Carrier carrier;
                    carrier.u = v;
                    carrier.v = t.loopVertex[c + 1 < t.loopOffset[f + 1] ? c + 1 : t.loopOffset[f]];
                    fragment.carriers.push_back(carrier);
                }
                for (const std::size_t i : list) {
                    for (const u32 r : now[i].refs) {
                        consumed[r] = 1;
                    }
                }
                made.push_back(FaceRef{healedCell, false, static_cast<u32>(healed.fragments.size()), 0, false});
                healed.fragments.push_back(std::move(fragment));
                continue;
            }
            for (const std::size_t i : list) {
                if (now[i].made) {
                    made.push_back(FaceRef{healedCell, false, static_cast<u32>(healed.fragments.size()), 0, false});
                    healed.fragments.push_back(std::move(now[i].fragment));
                }
            }
        }
        if (made.empty()) {
            continue;
        }
        // The outside it keeps, what was made, then the inside.
        std::vector<FaceRef> faces;
        for (u32 r = 0; r < pieces[p].faces.size(); ++r) {
            if (!pieces[p].faces[r].inside && consumed[r] == 0) {
                faces.push_back(pieces[p].faces[r]);
            }
        }
        faces.insert(faces.end(), made.begin(), made.end());
        for (const FaceRef& ref : pieces[p].faces) {
            if (ref.inside) {
                faces.push_back(ref);
            }
        }
        pieces[p].faces = std::move(faces);
    }
    cells.push_back(std::move(healed));
}

} // namespace

// ============================================================================
// CutPieces
// ============================================================================

CutResult CutPieces(std::span<const CutTarget> inputs, const CellComplex& diagram,
                    const CutOptions& options) {
    CutResult result;
    std::vector<Target> targets(inputs.size());
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        Prepare(targets[i], inputs[i], options);
        if (targets[i].usable && !targets[i].whole) {
            FillHoles(targets[i]);
        }
    }
    const u32 cellCount = static_cast<u32>(diagram.cells.size());
    const f64 band = 0.01 * Length(diagram.high - diagram.low);

    // --- phase 1 ---
    std::vector<Cell> cells(cellCount);
    Parallel(cellCount, options.threads, [&](std::size_t i) {
        if (!diagram.cells[i].empty()) {
            Clipper(diagram, targets, static_cast<u32>(i), cells[i]).run();
        }
    });

    // --- phase 2 ---
    std::vector<InsideSet> sets;
    for (u32 c = 0; c < cellCount; ++c) {
        const ConvexCell& convex = diagram.cells[c];
        for (u32 k = 0; k < convex.faces.size(); ++k) {
            const CellFace& face = convex.faces[k];
            // Once per pair, from the side the canonical plane faces out of.
            if (IsBoxSite(face.other) || !face.low || face.other >= cellCount ||
                diagram.cells[face.other].empty()) {
                continue;
            }
            InsideSet set;
            set.plane = face.plane;
            set.low = c;
            set.high = face.other;
            set.face = k;
            sets.push_back(std::move(set));
        }
    }
    Parallel(sets.size(), options.threads, [&](std::size_t i) {
        BuildInside(sets[i], diagram, cells[sets[i].low], targets);
    });
    if (diagram.sliced) {
        UniteClosePoints(cells, sets);
    }
    std::vector<std::vector<u32>> setsOf(cellCount);
    for (u32 s = 0; s < sets.size(); ++s) {
        if (sets[s].failed) {
            ++result.failedFaces;
        }
        setsOf[sets[s].low].push_back(s);
        setsOf[sets[s].high].push_back(s);
    }

    // --- phase 3 ---
    std::vector<std::vector<PieceWork>> perCell(cellCount);
    Parallel(cellCount, options.threads, [&](std::size_t i) {
        const ConvexCell& convex = diagram.cells[i];
        if (convex.empty()) {
            return;
        }
        Vector3d low = convex.vertices.front();
        Vector3d high = low;
        for (const Vector3d& v : convex.vertices) {
            low = Vector3d(std::min(low.x, v.x), std::min(low.y, v.y), std::min(low.z, v.z));
            high = Vector3d(std::max(high.x, v.x), std::max(high.y, v.y), std::max(high.z, v.z));
        }
        CellPieces(static_cast<u32>(i), cells[i], sets, setsOf, perCell[i], low, high, targets, options);
    });
    std::vector<PieceWork> pieces;
    for (std::size_t c = 0; c < perCell.size(); ++c) {
        std::vector<PieceWork>& list = perCell[c];
        // A cell through parts that do not touch makes a piece of each; one
        // that holds nothing makes none.
        if (list.empty()) {
            ++result.empty;
        } else {
            result.split += static_cast<u32>(list.size() - 1);
        }
        for (PieceWork& piece : list) {
            pieces.push_back(std::move(piece));
        }
    }
    // A Whole target is one piece, in a cell of its own past the diagram's.
    for (u32 ti = 0; ti < targets.size(); ++ti) {
        const Target& t = targets[ti];
        if (!t.usable || !t.whole) {
            continue;
        }
        Cell cell;
        const u32 faceCount = static_cast<u32>(t.faceLow.size());
        for (u32 f = 0; f < faceCount; ++f) {
            Fragment fragment;
            fragment.target = ti;
            fragment.face = f;
            for (u32 c = t.loopOffset[f]; c < t.loopOffset[f + 1]; ++c) {
                const u32 v = t.loopVertex[c];
                Point p;
                p.key = SourceKey(ti, v);
                p.position = t.positions[v];
                p.target = ti;
                p.from = {v, kNone, kNone};
                p.weight = {1.0, 0.0, 0.0};
                fragment.points.push_back(cell.add(p));
                Carrier carrier;
                carrier.u = v;
                carrier.v = t.loopVertex[c + 1 < t.loopOffset[f + 1] ? c + 1 : t.loopOffset[f]];
                fragment.carriers.push_back(carrier);
            }
            cell.fragments.push_back(std::move(fragment));
        }
        PieceWork piece;
        piece.whole = ti;
        for (u32 f = 0; f < faceCount; ++f) {
            piece.faces.push_back(FaceRef{static_cast<u32>(cells.size()), false, f, 0, false});
        }
        cells.push_back(std::move(cell));
        pieces.push_back(std::move(piece));
    }
    // A plane parts the pieces only where it reaches.
    const bool bounded = diagram.bounded();
    if (bounded) {
        result.joined = JoinUnreached(pieces, sets);
    }

    // --- phase 4: Smallest ---
    std::vector<Measure> measures(pieces.size());
    for (std::size_t p = 0; p < pieces.size(); ++p) {
        measures[p] = MeasurePiece(pieces[p], cells, sets);
    }
    // Which piece holds each side of each inside triangle.
    std::map<std::pair<u32, u32>, f64> shared; // (low piece, high piece) -> area
    std::vector<std::vector<u32>> sidePiece(sets.size());
    for (u32 s = 0; s < sets.size(); ++s) {
        sidePiece[s].assign(2 * sets[s].owner.size(), kNone);
    }
    for (u32 p = 0; p < pieces.size(); ++p) {
        for (const FaceRef& ref : pieces[p].faces) {
            if (ref.inside) {
                sidePiece[ref.set][2 * ref.index + (ref.reversed ? 1 : 0)] = p;
            }
        }
    }
    for (u32 s = 0; s < sets.size(); ++s) {
        for (u32 t = 0; t < sets[s].owner.size(); ++t) {
            const u32 a = sidePiece[s][2 * t];
            const u32 b = sidePiece[s][2 * t + 1];
            if (a != kNone && b != kNone && a != b) {
                shared[{std::min(a, b), std::max(a, b)}] += sets[s].area[t];
            }
        }
    }
    std::vector<bool> alive(pieces.size(), true);
    std::vector<CutPiece> described(pieces.size());
    for (std::size_t p = 0; p < pieces.size(); ++p) {
        Describe(described[p], measures[p], band);
    }
    if (options.smallest > 0.0 && pieces.size() > 1) {
        f64 total = 0.0;
        u32 counted = 0;
        for (std::size_t p = 0; p < pieces.size(); ++p) {
            if (pieces[p].whole == kNone) {
                total += described[p].measure;
                ++counted;
            }
        }
        const f64 threshold = counted > 0 ? options.smallest * total / counted : 0.0;
        for (;;) {
            u32 small = kNone;
            for (u32 p = 0; p < pieces.size(); ++p) {
                if (!alive[p] || pieces[p].whole != kNone) {
                    continue;
                }
                if (described[p].measure < threshold &&
                    (small == kNone || described[p].measure < described[small].measure)) {
                    small = p;
                }
            }
            if (small == kNone) {
                break;
            }
            u32 into = kNone;
            f64 most = 0.0;
            for (const auto& [pair, area] : shared) {
                if (pair.first != small && pair.second != small) {
                    continue;
                }
                const u32 other = pair.first == small ? pair.second : pair.first;
                if (alive[other] && pieces[other].whole == kNone &&
                    (area > most || (area == most && into != kNone && other < into))) {
                    most = area;
                    into = other;
                }
            }
            if (into == kNone) {
                f64 nearest = 0.0;
                for (u32 p = 0; p < pieces.size(); ++p) {
                    if (p == small || !alive[p] || pieces[p].whole != kNone) {
                        continue;
                    }
                    const Vector3d r = described[p].centroid - described[small].centroid;
                    if (into == kNone || Dot(r, r) < nearest) {
                        nearest = Dot(r, r);
                        into = p;
                    }
                }
            }
            if (into == kNone) {
                break;
            }
            // Join, dropping the inside faces the two shared.
            PieceWork& keep = pieces[into];
            const PieceWork& gone = pieces[small];
            std::vector<FaceRef> faces;
            const auto sharedFace = [&](const FaceRef& ref) {
                if (!ref.inside) {
                    return false;
                }
                const u32 a = sidePiece[ref.set][2 * ref.index];
                const u32 b = sidePiece[ref.set][2 * ref.index + 1];
                return (a == into && b == small) || (a == small && b == into);
            };
            for (const FaceRef& ref : keep.faces) {
                if (!sharedFace(ref)) {
                    faces.push_back(ref);
                }
            }
            for (const FaceRef& ref : gone.faces) {
                if (!sharedFace(ref)) {
                    faces.push_back(ref);
                }
            }
            keep.faces = std::move(faces);
            for (u32 c : gone.cells) {
                if (std::find(keep.cells.begin(), keep.cells.end(), c) == keep.cells.end()) {
                    keep.cells.push_back(c);
                }
            }
            keep.rank = std::min(keep.rank, gone.rank);
            alive[small] = false;
            ++result.merged;
            for (const FaceRef& ref : keep.faces) {
                if (ref.inside) {
                    sidePiece[ref.set][2 * ref.index + (ref.reversed ? 1 : 0)] = into;
                }
            }
            std::map<std::pair<u32, u32>, f64> next;
            for (const auto& [pair, area] : shared) {
                u32 a = pair.first == small ? into : pair.first;
                u32 b = pair.second == small ? into : pair.second;
                if (a != b) {
                    next[{std::min(a, b), std::max(a, b)}] += area;
                }
            }
            shared = std::move(next);
            measures[into] = MeasurePiece(keep, cells, sets);
            Describe(described[into], measures[into], band);
        }
    }

    // The survivors, renumbered in order.
    std::vector<u32> number(pieces.size(), kNone);
    for (u32 p = 0; p < pieces.size(); ++p) {
        if (alive[p]) {
            number[p] = static_cast<u32>(result.pieces.size());
            CutPiece piece = described[p];
            piece.cells = pieces[p].cells;
            std::sort(piece.cells.begin(), piece.cells.end());
            piece.whole = pieces[p].whole;
            result.pieces.push_back(std::move(piece));
        }
    }
    for (const auto& [pair, area] : shared) {
        if (number[pair.first] != kNone && number[pair.second] != kNone) {
            PieceLink link;
            link.a = number[pair.first];
            link.b = number[pair.second];
            link.insideArea = area;
            result.links.push_back(link);
        }
    }

    // After the measures: a healed face need not be convex, and they fan.
    if (bounded) {
        HealJoined(pieces, alive, cells, targets);
    }

    // --- phase 5 ---
    result.meshes.resize(targets.size());
    result.facePiece.resize(targets.size());
    result.facePlane.resize(targets.size());
    std::unordered_map<PositionKey, std::vector<u32>, PositionHash> cutAt;
    for (u32 ti = 0; ti < targets.size(); ++ti) {
        const Target& t = targets[ti];
        if (!t.usable) {
            continue;
        }
        std::vector<OutVertex> vertices;
        std::vector<OutFace> outside;
        std::vector<OutFace> inside;
        struct VertexKey {
            u32 piece;
            bool inside;
            Key key;
            bool operator==(const VertexKey& o) const {
                return piece == o.piece && inside == o.inside && key == o.key;
            }
        };
        struct VertexKeyHash {
            std::size_t operator()(const VertexKey& k) const {
                return KeyHash{}(k.key) ^ (static_cast<std::size_t>(k.piece) * 0x9E3779B1u) ^
                       (k.inside ? 0x5bd1e995u : 0u);
            }
        };
        std::unordered_map<VertexKey, u32, VertexKeyHash> vertexOf;
        std::map<std::pair<u32, Key>, u32> groupOf;
        const auto mergeKey = [&](const Key& key) -> Key {
            if (key[0] == kSourceKey && key[1] == ti) {
                return {kSourceKey, ti, t.merge[key[2]], 0, 0, 0};
            }
            if (key[0] == kEdgeKey && key[1] == ti) {
                const u32 a = t.merge[key[2]];
                const u32 b = t.merge[key[3]];
                return {kEdgeKey, ti, std::min(a, b), std::max(a, b), key[4], key[5]};
            }
            return key;
        };
        const auto vertex = [&](u32 piece, bool isInside, const Key& key,
                                const Vector3d& position, const Point* recipe) {
            const VertexKey vk{piece, isInside, key};
            const auto found = vertexOf.find(vk);
            if (found != vertexOf.end()) {
                return found->second;
            }
            OutVertex v;
            v.position = position;
            v.piece = piece;
            v.cut = isInside || key[0] != kSourceKey;
            if (recipe != nullptr && !isInside) {
                v.from = recipe->from;
                v.weight = recipe->weight;
                if (key[0] == kSourceKey) {
                    v.weld = t.merge[key[2]] + 1;
                }
            }
            const auto [g, fresh] = groupOf.emplace(std::make_pair(piece, mergeKey(key)),
                                                    static_cast<u32>(groupOf.size()));
            v.group = g->second;
            const u32 at = static_cast<u32>(vertices.size());
            vertices.push_back(v);
            vertexOf.emplace(vk, at);
            return at;
        };
        for (u32 p = 0; p < pieces.size(); ++p) {
            if (!alive[p]) {
                continue;
            }
            const u32 piece = number[p];
            u32 section = kNone;
            for (const FaceRef& ref : pieces[p].faces) {
                if (ref.inside) {
                    const InsideSet& set = sets[ref.set];
                    if (set.owner[ref.index] != ti) {
                        continue;
                    }
                    OutFace f;
                    f.piece = piece;
                    f.kind = CornerKind::Inside;
                    f.normal = ref.reversed ? Times(set.normal, -1.0) : set.normal;
                    const u32* tri = &set.triangles[3 * ref.index];
                    const u32 order[3] = {tri[0], ref.reversed ? tri[2] : tri[1],
                                          ref.reversed ? tri[1] : tri[2]};
                    for (u32 k : order) {
                        const InsidePoint& ip = set.points[k];
                        f.vertices.push_back(vertex(piece, true, ip.key, ip.position, nullptr));
                    }
                    f.edgeFrom.assign(3, {kNone, kNone});
                    f.section = section != kNone ? section : 0;
                    f.plane = set.plane;
                    inside.push_back(std::move(f));
                    continue;
                }
                const Cell& cell = cells[ref.cell];
                const Fragment& fragment = cell.fragments[ref.index];
                if (fragment.target != ti) {
                    continue;
                }
                OutFace f;
                f.piece = piece;
                f.sourceFace = fragment.face;
                f.triangle = fragment.triangle;
                const auto sections = t.mesh.faceSections();
                f.section = fragment.face < sections.size() ? sections[fragment.face] : 0;
                if (section == kNone) {
                    section = f.section;
                }
                f.kind = fragment.triangle == kNone ? CornerKind::Whole : CornerKind::Fragment;
                const std::size_t n = fragment.points.size();
                for (std::size_t k = 0; k < n; ++k) {
                    const Point& point = cell.points[fragment.points[k]];
                    f.vertices.push_back(vertex(piece, false, point.key, point.position, &point));
                    if (f.kind == CornerKind::Whole) {
                        f.sourceCorner.push_back(
                            t.loopHalfedge[t.loopOffset[fragment.face] + static_cast<u32>(k)]);
                    } else {
                        f.weights.push_back(fragment.weights[k]);
                    }
                    const Carrier& c = fragment.carriers[k];
                    f.edgeFrom.emplace_back(c.onPlane ? kNone : c.u, c.onPlane ? kNone : c.v);
                    if (point.key[0] != kSourceKey) {
                        auto& list = cutAt[PositionBits(point.position)];
                        if (std::find(list.begin(), list.end(), piece) == list.end()) {
                            list.push_back(piece);
                        }
                    }
                }
                outside.push_back(std::move(f));
            }
        }
        std::vector<OutFace> faces = std::move(outside);
        for (OutFace& f : inside) {
            faces.push_back(std::move(f));
        }
        if (faces.empty()) {
            continue;
        }
        std::vector<u32> survivor;
        result.meshes[ti] = Assemble(t, vertices, faces, survivor);
        for (u32 f : survivor) {
            result.facePiece[ti].push_back(faces[f].piece);
            result.facePlane[ti].push_back(faces[f].plane);
        }
        for (u32 piece : result.facePiece[ti]) {
            CutPiece& out = result.pieces[piece];
            if (std::find(out.targets.begin(), out.targets.end(), ti) == out.targets.end()) {
                out.targets.push_back(ti);
            }
        }
    }

    // Pieces that meet at cut points.
    std::map<std::pair<u32, u32>, u32> touching;
    for (const auto& [position, list] : cutAt) {
        for (std::size_t i = 0; i < list.size(); ++i) {
            for (std::size_t j = i + 1; j < list.size(); ++j) {
                ++touching[{std::min(list[i], list[j]), std::max(list[i], list[j])}];
            }
        }
    }
    for (PieceLink& link : result.links) {
        const auto found = touching.find({std::min(link.a, link.b), std::max(link.a, link.b)});
        if (found != touching.end()) {
            link.sharedPoints = found->second;
            touching.erase(found);
        }
    }
    for (const auto& [pair, count] : touching) {
        PieceLink link;
        link.a = pair.first;
        link.b = pair.second;
        link.sharedPoints = count;
        result.links.push_back(link);
    }
    std::sort(result.links.begin(), result.links.end(), [](const PieceLink& l, const PieceLink& r) {
        return l.a != r.a ? l.a < r.a : l.b < r.b;
    });
    return result;
}

// ============================================================================
// Thicken (§5.7)
// ============================================================================

u32 ThickenOpen(Mesh& mesh, f64 thickness) {
    if (!(thickness > 0.0)) {
        return 0;
    }
    Mesh source = mesh;
    source.invalidateConnectivity();
    if (!source.ensureConnectivity().ok()) {
        return 0;
    }
    const auto positions = source.attributes.get<Vector3f>(names::kPosition, Domain::Vertex);
    const FaceSet& faces = source.faceSet();
    const Topology& topology = std::as_const(source).topology();
    const u32 vertexCount = static_cast<u32>(positions.size());
    const u32 faceCount = static_cast<u32>(faces.faceCount());
    std::vector<u32> offset(faceCount + 1, 0);
    for (u32 f = 0; f < faceCount; ++f) {
        offset[f + 1] = offset[f] + faces.faceValence[f];
    }
    const auto next = [&](u32 f, u32 c) { return c + 1 < offset[f + 1] ? c + 1 : offset[f]; };
    std::vector<Vector3d> place(vertexCount);
    for (u32 v = 0; v < vertexCount; ++v) {
        place[v] = Vector3d(positions[v].x, positions[v].y, positions[v].z);
    }

    // Points: vertices welded by position, which closes the seams of an import.
    std::vector<u32> point(vertexCount);
    u32 pointCount = 0;
    {
        std::unordered_map<PositionKey, u32, PositionHash> byPosition;
        for (u32 v = 0; v < vertexCount; ++v) {
            const auto [found, fresh] = byPosition.emplace(PositionBits(place[v]), pointCount);
            point[v] = found->second;
            pointCount += fresh ? 1 : 0;
        }
    }
    // Parts, and the point edges used once.
    UnionFind parts;
    for (u32 p = 0; p < pointCount; ++p) {
        parts.add();
    }
    std::map<std::pair<u32, u32>, u32> uses;
    for (u32 f = 0; f < faceCount; ++f) {
        for (u32 c = offset[f]; c < offset[f + 1]; ++c) {
            const u32 a = point[faces.cornerVertex[c]];
            const u32 b = point[faces.cornerVertex[next(f, c)]];
            parts.join(a, b);
            ++uses[{std::min(a, b), std::max(a, b)}];
        }
    }
    std::vector<bool> open(pointCount, false);
    u32 thickened = 0;
    for (const auto& [edge, count] : uses) {
        if (count == 1 && !open[parts.find(edge.first)]) {
            open[parts.find(edge.first)] = true;
            ++thickened;
        }
    }
    if (thickened == 0) {
        return 0;
    }
    const auto isOpen = [&](u32 f) { return open[parts.find(point[faces.cornerVertex[offset[f]]])]; };

    // Each point's normal, area-weighted, and its offset inward along it.
    const auto newell = [&](u32 f, const std::vector<Vector3d>& at) {
        Vector3d n(0, 0, 0);
        for (u32 c = offset[f]; c < offset[f + 1]; ++c) {
            const Vector3d& a = at[faces.cornerVertex[c]];
            const Vector3d& b = at[faces.cornerVertex[next(f, c)]];
            n += Vector3d((a.y - b.y) * (a.z + b.z), (a.z - b.z) * (a.x + b.x),
                          (a.x - b.x) * (a.y + b.y));
        }
        return n;
    };
    std::vector<Vector3d> pointNormal(pointCount, Vector3d(0, 0, 0));
    std::vector<Vector3d> faceNormal(faceCount);
    for (u32 f = 0; f < faceCount; ++f) {
        faceNormal[f] = newell(f, place);
        for (u32 c = offset[f]; c < offset[f + 1]; ++c) {
            pointNormal[point[faces.cornerVertex[c]]] += faceNormal[f];
        }
    }
    std::vector<f64> depth(pointCount, thickness);
    std::vector<Vector3d> inner(vertexCount);
    for (u32 round = 0; round < 5; ++round) {
        for (u32 v = 0; v < vertexCount; ++v) {
            const Vector3d& n = pointNormal[point[v]];
            const f64 length = Length(n);
            inner[v] = length > 0.0 ? place[v] - Times(n, depth[point[v]] / length) : place[v];
        }
        bool turned = false;
        for (u32 f = 0; f < faceCount; ++f) {
            if (isOpen(f) && Dot(newell(f, inner), faceNormal[f]) <= 0.0) {
                turned = true;
                for (u32 c = offset[f]; c < offset[f + 1]; ++c) {
                    depth[point[faces.cornerVertex[c]]] *= 0.5;
                }
            }
        }
        if (!turned) {
            break;
        }
    }

    // The new face set: the source's, each open face's copy turned inward, and
    // a strip of quads along every border edge.
    struct Corner {
        HalfedgeId from;   ///< The source corner whose values it takes.
        bool flip = false; ///< Its normal turned round.
        bool flat = false; ///< A strip's: the strip's own normal.
    };
    std::vector<u32> copyOf(vertexCount, kNone);
    std::vector<u32> copied; // per vertex past the source's, the source vertex
    const auto copy = [&](u32 v) {
        if (copyOf[v] == kNone) {
            copyOf[v] = vertexCount + static_cast<u32>(copied.size());
            copied.push_back(v);
        }
        return copyOf[v];
    };
    std::vector<std::vector<HalfedgeId>> loopHalfedge(faceCount);
    for (u32 f = 0; f < faceCount; ++f) {
        HalfedgeId h = topology.halfedge(FaceId(f));
        for (u32 c = offset[f]; c < offset[f + 1]; ++c) {
            loopHalfedge[f].push_back(h);
            h = topology.next(h);
        }
    }
    FaceSet out = faces;
    std::vector<u32> faceFrom(faceCount);
    std::vector<std::vector<Corner>> corners(faceCount);
    std::vector<Vector3d> stripNormal(faceCount, Vector3d(0, 0, 0));
    for (u32 f = 0; f < faceCount; ++f) {
        faceFrom[f] = f;
        for (HalfedgeId h : loopHalfedge[f]) {
            corners[f].push_back(Corner{h, false, false});
        }
    }
    std::vector<u32> loop;
    for (u32 f = 0; f < faceCount; ++f) {
        if (!isOpen(f)) {
            continue;
        }
        const u32 n = offset[f + 1] - offset[f];
        loop.clear();
        std::vector<Corner> turned;
        for (u32 k = 0; k < n; ++k) {
            const u32 j = (n - k) % n;
            loop.push_back(copy(faces.cornerVertex[offset[f] + j]));
            turned.push_back(Corner{loopHalfedge[f][j], true, false});
        }
        out.addFace(loop);
        faceFrom.push_back(f);
        corners.push_back(std::move(turned));
        stripNormal.push_back(Vector3d(0, 0, 0));
        for (u32 k = 0; k < n; ++k) {
            const u32 u = faces.cornerVertex[offset[f] + k];
            const u32 v = faces.cornerVertex[offset[f] + (k + 1) % n];
            const u32 pu = point[u];
            const u32 pv = point[v];
            if (uses[{std::min(pu, pv), std::max(pu, pv)}] != 1) {
                continue;
            }
            const u32 strip[4] = {v, u, copy(u), copy(v)};
            out.addFace(strip);
            faceFrom.push_back(f);
            const HalfedgeId hu = loopHalfedge[f][k];
            const HalfedgeId hv = loopHalfedge[f][(k + 1) % n];
            corners.push_back({Corner{hv, false, true}, Corner{hu, false, true},
                               Corner{hu, false, true}, Corner{hv, false, true}});
            stripNormal.push_back(cross(place[u] - place[v], inner[u] - place[v]));
        }
    }
    out.vertexCount = vertexCount + static_cast<u32>(copied.size());
    // Every vertex of the new set by the source vertex it copies, and where it is.
    std::vector<u32> vertexFrom(out.vertexCount);
    std::vector<Vector3d> vertexAt(out.vertexCount);
    for (u32 v = 0; v < out.vertexCount; ++v) {
        vertexFrom[v] = v < vertexCount ? v : copied[v - vertexCount];
        vertexAt[v] = v < vertexCount ? place[v] : inner[vertexFrom[v]];
    }

    RepairResult repaired = Repair(out);
    vertexFrom.resize(repaired.faces.vertexCount);
    vertexAt.resize(repaired.faces.vertexCount);
    std::vector<bool> isCopy(repaired.faces.vertexCount, false);
    for (u32 v = vertexCount; v < out.vertexCount; ++v) {
        isCopy[v] = true;
    }
    for (const VertexSplit& split : repaired.log.splits) {
        if (split.created < vertexFrom.size() && split.original < vertexFrom.size()) {
            vertexFrom[split.created] = vertexFrom[split.original];
            vertexAt[split.created] = vertexAt[split.original];
            isCopy[split.created] = isCopy[split.original];
        }
    }
    std::vector<u32> survivor;
    {
        std::size_t dropped = 0;
        for (u32 f = 0; f < out.faceCount(); ++f) {
            if (dropped < repaired.log.droppedFaces.size() &&
                repaired.log.droppedFaces[dropped].index == f) {
                ++dropped;
                continue;
            }
            survivor.push_back(f);
        }
    }

    Mesh result;
    result.name = source.name;
    result.lodLevel = source.lodLevel;
    result.sections = source.sections;
    result.setFaceSet(repaired.faces);
    if (!result.ensureConnectivity().ok()) {
        return 0;
    }
    for (const AttrLayer& layer : source.attributes.layers()) {
        result.attributes.create(layer.name, layer.domain, layer.type, layer.storage);
    }
    result.attributes.create(names::kFractureMade, Domain::Face, AttrType::Bool);
    MergeGroupsOf(result);
    const auto copyBytes = [](const AttrLayer& s, AttrLayer& o, u32 from, u32 to) {
        const u32 stride = AttrTypeSize(s.type);
        if ((from + 1) * static_cast<std::size_t>(stride) <= s.data.size() &&
            (to + 1) * static_cast<std::size_t>(stride) <= o.data.size()) {
            std::memcpy(o.data.data() + static_cast<std::size_t>(to) * stride,
                        s.data.data() + static_cast<std::size_t>(from) * stride, stride);
        }
    };
    const Topology& built = std::as_const(result).topology();
    for (const AttrLayer& layer : source.attributes.layers()) {
        AttrLayer* target = result.attributes.layer(layer.name, layer.domain);
        if (target == nullptr) {
            continue;
        }
        if (layer.domain == Domain::Vertex) {
            for (u32 v = 0; v < vertexFrom.size(); ++v) {
                copyBytes(layer, *target, vertexFrom[v], v);
            }
        } else if (layer.domain == Domain::Face) {
            for (u32 f = 0; f < survivor.size(); ++f) {
                copyBytes(layer, *target, faceFrom[survivor[f]], f);
            }
        } else if (layer.domain == Domain::Halfedge) {
            for (u32 f = 0; f < survivor.size(); ++f) {
                HalfedgeId h = built.halfedge(FaceId(f));
                for (const Corner& c : corners[survivor[f]]) {
                    copyBytes(layer, *target, c.from.value(), h.value());
                    h = built.next(h);
                }
            }
        } else if (layer.domain == Domain::Edge) {
            // The source faces keep their edges' marks.
            for (u32 f = 0; f < survivor.size(); ++f) {
                if (survivor[f] >= faceCount) {
                    continue;
                }
                HalfedgeId h = built.halfedge(FaceId(f));
                for (const Corner& c : corners[survivor[f]]) {
                    copyBytes(layer, *target, Topology::edge(c.from).value(),
                              Topology::edge(h).value());
                    h = built.next(h);
                }
            }
        } else if (layer.domain == Domain::Mesh) {
            target->data = layer.data;
        }
    }
    {
        auto position = result.attributes.get<Vector3f>(names::kPosition, Domain::Vertex);
        auto groups = result.attributes.get<u32>(names::kMergeGroup, Domain::Vertex);
        // The copies are points of their own, one group per welded point.
        std::vector<u32> pointGroup(pointCount, kNone);
        u32 fresh = FreshMergeGroup(source);
        for (u32 v = 0; v < vertexAt.size(); ++v) {
            const Vector3d& p = vertexAt[v];
            position[v] = Vector3f(static_cast<f32>(p.x), static_cast<f32>(p.y),
                                   static_cast<f32>(p.z));
            if (isCopy[v] && v < groups.size()) {
                u32& group = pointGroup[point[vertexFrom[v]]];
                if (group == kNone) {
                    group = fresh++;
                }
                groups[v] = group;
            }
        }
    }
    auto made = result.attributes.get<u8>(names::kFractureMade, Domain::Face);
    auto normal = result.attributes.get<Vector3f>(names::kNormal, Domain::Halfedge);
    for (u32 f = 0; f < survivor.size(); ++f) {
        const u32 was = survivor[f];
        made[f] = was >= faceCount ? 1 : 0;
        if (was < faceCount || normal.empty()) {
            continue;
        }
        Vector3d flat = stripNormal[was];
        const f64 length = Length(flat);
        flat = length > 0.0 ? Times(flat, 1.0 / length) : flat;
        HalfedgeId h = built.halfedge(FaceId(f));
        for (const Corner& c : corners[was]) {
            if (h.index() < normal.size()) {
                if (c.flat) {
                    normal[h.index()] = Vector3f(static_cast<f32>(flat.x),
                                                 static_cast<f32>(flat.y),
                                                 static_cast<f32>(flat.z));
                } else if (c.flip) {
                    const Vector3f n = normal[h.index()];
                    normal[h.index()] = Vector3f(-n.x, -n.y, -n.z);
                }
            }
            h = built.next(h);
        }
    }
    if (!source.skin.empty()) {
        result.skin.offsets.assign(1, 0);
        for (u32 v = 0; v < vertexFrom.size(); ++v) {
            result.skin.appendVertex(std::as_const(source.skin).forVertex(vertexFrom[v]));
        }
    }
    // The source's polygons keep their rows; the slab's are cut by the rule.
    {
        FaceTriangulationBuilder rows(static_cast<u32>(survivor.size()));
        bool any = false;
        for (u32 f = 0; f < survivor.size(); ++f) {
            if (survivor[f] < faceCount) {
                const auto row = source.triangulation.row(survivor[f]);
                if (!row.empty()) {
                    rows.set(f, row);
                    any = true;
                }
            }
        }
        if (any) {
            result.triangulation = rows.build();
        }
    }
    MaterialiseRows(result);
    result.recomputeBounds();
    mesh = std::move(result);
    return thickened;
}

} // namespace fracture
} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
