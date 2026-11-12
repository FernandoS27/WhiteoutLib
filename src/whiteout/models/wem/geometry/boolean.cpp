// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/geometry/boolean.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <whiteout/models/wem/geometry/bvh.h>
#include <whiteout/models/wem/geometry/cdt.h>
#include <whiteout/models/wem/geometry/fracture/winding.h>
#include <whiteout/models/wem/geometry/interpolate.h>
#include <whiteout/models/wem/geometry/merge.h>
#include <whiteout/models/wem/geometry/modelling.h>
#include <whiteout/models/wem/geometry/repair.h>
#include <whiteout/models/wem/geometry/triangulation.h>

namespace whiteout {
namespace models {
namespace wem {
namespace geom {

const char* ToString(BooleanRefusal refusal) {
    switch (refusal) {
    case BooleanRefusal::None:
        return "None";
    case BooleanRefusal::NoTarget:
        return "NoTarget";
    case BooleanRefusal::NoOperand:
        return "NoOperand";
    case BooleanRefusal::OperandHoldsNothing:
        return "OperandHoldsNothing";
    case BooleanRefusal::NothingToSubtract:
        return "NothingToSubtract";
    case BooleanRefusal::NothingLeft:
        return "NothingLeft";
    case BooleanRefusal::CutFailed:
        return "CutFailed";
    }
    return "?";
}

namespace {

constexpr u32 kNone = 0xFFFFFFFFu;
/// The grid every decision is made on: a difference is 27 bits, so a 2x2
/// minor fits 64 and an orientation, a minor times a difference summed
/// thrice, fits 128 (the CDT's resolution, `cdt.cpp`).
constexpr f64 kGrid = 67108864.0; // 2^26
/// Readings a patch takes, its largest fragments first.
constexpr u32 kReadings = 8;

using Grid = std::array<i64, 3>;

// ============================================================================
// Exact signs
// ============================================================================

struct Wide {
    u64 lo = 0;
    u64 hi = 0;
};

Wide Multiply(i64 a, i64 b) {
    const bool negative = (a < 0) != (b < 0);
    const u64 ua = a < 0 ? 0 - static_cast<u64>(a) : static_cast<u64>(a);
    const u64 ub = b < 0 ? 0 - static_cast<u64>(b) : static_cast<u64>(b);
    const u64 a0 = ua & 0xFFFFFFFFu;
    const u64 a1 = ua >> 32;
    const u64 b0 = ub & 0xFFFFFFFFu;
    const u64 b1 = ub >> 32;
    const u64 p00 = a0 * b0;
    const u64 p01 = a0 * b1;
    const u64 p10 = a1 * b0;
    const u64 p11 = a1 * b1;
    const u64 middle = (p00 >> 32) + (p01 & 0xFFFFFFFFu) + (p10 & 0xFFFFFFFFu);
    Wide out;
    out.lo = (p00 & 0xFFFFFFFFu) | (middle << 32);
    out.hi = p11 + (p01 >> 32) + (p10 >> 32) + (middle >> 32);
    if (negative) {
        out.lo = ~out.lo + 1;
        out.hi = ~out.hi + (out.lo == 0 ? 1 : 0);
    }
    return out;
}

Wide Add(Wide a, Wide b) {
    Wide out;
    out.lo = a.lo + b.lo;
    out.hi = a.hi + b.hi + (out.lo < a.lo ? 1 : 0);
    return out;
}

int SignOf(Wide a) {
    if (static_cast<i64>(a.hi) < 0) {
        return -1;
    }
    return (a.hi != 0 || a.lo != 0) ? 1 : 0;
}

f64 ToDouble(Wide a) {
    const bool negative = static_cast<i64>(a.hi) < 0;
    if (negative) {
        a.lo = ~a.lo + 1;
        a.hi = ~a.hi + (a.lo == 0 ? 1 : 0);
    }
    const f64 value = static_cast<f64>(a.hi) * 18446744073709551616.0 + static_cast<f64>(a.lo);
    return negative ? -value : value;
}

/// ((b - a) x (c - a)) . (d - a): positive when d lies on the side the plane
/// through a, b, c faces.
Wide Orient3(const Grid& a, const Grid& b, const Grid& c, const Grid& d) {
    const i64 bx = b[0] - a[0];
    const i64 by = b[1] - a[1];
    const i64 bz = b[2] - a[2];
    const i64 cx = c[0] - a[0];
    const i64 cy = c[1] - a[1];
    const i64 cz = c[2] - a[2];
    const i64 dx = d[0] - a[0];
    const i64 dy = d[1] - a[1];
    const i64 dz = d[2] - a[2];
    const i64 m0 = cy * dz - cz * dy;
    const i64 m1 = cz * dx - cx * dz;
    const i64 m2 = cx * dy - cy * dx;
    return Add(Add(Multiply(bx, m0), Multiply(by, m1)), Multiply(bz, m2));
}

int Orient3Sign(const Grid& a, const Grid& b, const Grid& c, const Grid& d) {
    return SignOf(Orient3(a, b, c, d));
}

/// Grid steps within which a corner lies on another triangle's plane. A copy
/// turned and turned back holds its positions an `f32` rounding apart, so
/// faces meant to be flush are not, on the grid; without this they cross at a
/// grazing angle and leave slivers (§10).
constexpr f64 kFlat = 16.0;

Grid NormalOf(const Grid& a, const Grid& b, const Grid& c) {
    const i64 bx = b[0] - a[0];
    const i64 by = b[1] - a[1];
    const i64 bz = b[2] - a[2];
    const i64 cx = c[0] - a[0];
    const i64 cy = c[1] - a[1];
    const i64 cz = c[2] - a[2];
    return Grid{by * cz - bz * cy, bz * cx - bx * cz, bx * cy - by * cx};
}

u32 DominantAxis(const Grid& n) {
    const i64 x = n[0] < 0 ? -n[0] : n[0];
    const i64 y = n[1] < 0 ? -n[1] : n[1];
    const i64 z = n[2] < 0 ? -n[2] : n[2];
    if (x >= y && x >= z) {
        return 0;
    }
    return y >= z ? 1 : 2;
}

/// The turn a, b, c in the plane that drops @p axis, kept in cyclic order so a
/// triangle whose normal points along +axis turns counter-clockwise.
int Orient2(const Grid& a, const Grid& b, const Grid& c, u32 axis) {
    const u32 i = (axis + 1) % 3;
    const u32 j = (axis + 2) % 3;
    const i64 v = (b[i] - a[i]) * (c[j] - a[j]) - (b[j] - a[j]) * (c[i] - a[i]);
    return v > 0 ? 1 : (v < 0 ? -1 : 0);
}

enum class Place : u8 { Outside, Inside, OnEdge, OnCorner };

struct Where {
    Place place = Place::Outside;
    u32 index = 0; ///< The edge (k to k + 1) or the corner.
};

/// Where @p p lies against triangle @p t, in its plane, exactly.
Where Locate2(const std::array<Grid, 3>& t, const Grid& p, u32 axis) {
    const int o = Orient2(t[0], t[1], t[2], axis);
    std::array<int, 3> s{};
    u32 zeros = 0;
    for (u32 k = 0; k < 3; ++k) {
        s[k] = Orient2(t[k], t[(k + 1) % 3], p, axis) * o;
        if (s[k] < 0) {
            return {};
        }
        zeros += s[k] == 0 ? 1 : 0;
    }
    if (zeros == 0) {
        return {Place::Inside, 0};
    }
    if (zeros == 1) {
        for (u32 k = 0; k < 3; ++k) {
            if (s[k] == 0) {
                return {Place::OnEdge, k};
            }
        }
    }
    for (u32 m = 0; m < 3; ++m) {
        if (s[m] == 0 && s[(m + 2) % 3] == 0) {
            return {Place::OnCorner, m};
        }
    }
    return {};
}

// ============================================================================
// Doubles
// ============================================================================

Vector3d Sub(const Vector3d& a, const Vector3d& b) {
    return Vector3d(a.x - b.x, a.y - b.y, a.z - b.z);
}
Vector3d AddV(const Vector3d& a, const Vector3d& b) {
    return Vector3d(a.x + b.x, a.y + b.y, a.z + b.z);
}
Vector3d Scale(const Vector3d& a, f64 s) {
    return Vector3d(a.x * s, a.y * s, a.z * s);
}
f64 Dot(const Vector3d& a, const Vector3d& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
Vector3d Cross(const Vector3d& a, const Vector3d& b) {
    return Vector3d(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
f64 Length(const Vector3d& a) {
    return std::sqrt(Dot(a, a));
}
f64 Axis(const Vector3d& a, u32 axis) {
    return axis == 0 ? a.x : (axis == 1 ? a.y : a.z);
}
Vector2<f64> Project(const Vector3d& p, u32 axis) {
    return Vector2<f64>(Axis(p, (axis + 1) % 3), Axis(p, (axis + 2) % 3));
}

/// Barycentric weights of @p p on triangle a, b, c, clamped and summing to one.
std::array<f64, 3> WeightsOn(const Vector3d& a, const Vector3d& b, const Vector3d& c, const Vector3d& p) {
    const Vector3d v0 = Sub(b, a);
    const Vector3d v1 = Sub(c, a);
    const Vector3d v2 = Sub(p, a);
    const f64 d00 = Dot(v0, v0);
    const f64 d01 = Dot(v0, v1);
    const f64 d11 = Dot(v1, v1);
    const f64 d20 = Dot(v2, v0);
    const f64 d21 = Dot(v2, v1);
    const f64 den = d00 * d11 - d01 * d01;
    std::array<f64, 3> w{1.0, 0.0, 0.0};
    if (!(std::fabs(den) > 0.0)) {
        return w;
    }
    w[1] = (d11 * d20 - d01 * d21) / den;
    w[2] = (d00 * d21 - d01 * d20) / den;
    w[0] = 1.0 - w[1] - w[2];
    f64 sum = 0.0;
    for (f64& x : w) {
        x = x < 1e-12 ? 0.0 : x;
        sum += x;
    }
    if (sum > 0.0) {
        for (f64& x : w) {
            x /= sum;
        }
    }
    return w;
}

// ============================================================================
// Keys and points
// ============================================================================

enum class Kind : u8 { Vertex, EdgeFace, EdgeEdge, Triple, Local };

struct Key {
    Kind kind = Kind::Vertex;
    std::array<u32, 6> v{};

    bool operator==(const Key& other) const {
        return kind == other.kind && v == other.v;
    }
};

u64 Mix(u64 z) {
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

struct KeyHash {
    std::size_t operator()(const Key& key) const {
        u64 h = Mix(static_cast<u64>(key.kind) + 1);
        for (u32 x : key.v) {
            h = Mix(h ^ x);
        }
        return static_cast<std::size_t>(h);
    }
};

u64 PairKey(u32 a, u32 b) {
    return a < b ? (static_cast<u64>(a) << 32) | b : (static_cast<u64>(b) << 32) | a;
}

/// Every point a run knows: the inputs' vertices by grid position, and the
/// points the cut makes, each placed once.
struct Points {
    std::vector<Vector3d> position;
    std::vector<Grid> grid; ///< Vertex points only.
    std::unordered_map<Key, u32, KeyHash> index;
    std::vector<u32> parent;

    u32 find(const Key& key) const {
        const auto found = index.find(key);
        return found == index.end() ? kNone : found->second;
    }

    u32 add(const Key& key, const Vector3d& at, const Grid& g = Grid{}) {
        const auto [it, fresh] = index.emplace(key, static_cast<u32>(position.size()));
        if (fresh) {
            position.push_back(at);
            grid.push_back(g);
            parent.push_back(it->second);
        }
        return it->second;
    }

    u32 root(u32 p) {
        while (parent[p] != p) {
            parent[p] = parent[parent[p]];
            p = parent[p];
        }
        return p;
    }

    void unite(u32 a, u32 b) {
        a = root(a);
        b = root(b);
        if (a != b) {
            parent[std::max(a, b)] = std::min(a, b);
        }
    }
};

// ============================================================================
// An input, prepared
// ============================================================================

struct Part {
    Mesh mesh; ///< A copy with connectivity, so corners have halfedges.
    bool operand = false;
    bool usable = false;
    std::vector<Vector3d> positions;
    std::vector<Vector3f> positionsF;
    std::vector<u32> loopOffset;
    std::vector<u32> loopVertex;
    std::vector<HalfedgeId> loopHalfedge;
    std::vector<u32> tri;     ///< Three source vertices each.
    std::vector<u32> triFace; ///< Per triangle, its face slot.
    std::vector<HalfedgeId> triCorner;
    std::vector<std::vector<Influence>> skin;
    std::vector<u32> merge;
    fracture::WindingMesh winding;
    bool holds = false;
    TriangleBvh bvh;
    Extent box;
};

bool Prepare(Part& part, const Mesh& source) {
    part.mesh = source;
    part.mesh.invalidateConnectivity();
    if (!part.mesh.ensureConnectivity().ok()) {
        return false;
    }
    const auto positions = part.mesh.attributes.get<Vector3f>(names::kPosition, Domain::Vertex);
    part.positionsF.assign(positions.begin(), positions.end());
    part.positions.reserve(positions.size());
    ResetExtent(part.box);
    for (const Vector3f& p : positions) {
        part.positions.push_back(Vector3d(p.x, p.y, p.z));
        GrowExtent(part.box, p);
    }
    const FaceSet& faces = part.mesh.faceSet();
    const Topology& topology = std::as_const(part.mesh).topology();
    const u32 faceCount = static_cast<u32>(faces.faceCount());
    part.loopOffset.assign(1, 0);
    for (u32 f = 0; f < faceCount; ++f) {
        const u32 base = part.loopOffset.back();
        const u32 valence = faces.faceValence[f];
        HalfedgeId h = topology.halfedge(FaceId(f));
        for (u32 k = 0; k < valence; ++k) {
            part.loopVertex.push_back(faces.cornerVertex[base + k]);
            part.loopHalfedge.push_back(h);
            h = topology.next(h);
        }
        part.loopOffset.push_back(base + valence);
    }
    TriangulateMesh(part.mesh, part.tri, &part.triFace);
    part.triCorner.resize(part.tri.size());
    for (std::size_t i = 0; i < part.triFace.size(); ++i) {
        const u32 f = part.triFace[i];
        for (u32 k = 0; k < 3; ++k) {
            const u32 v = part.tri[3 * i + k];
            for (u32 c = part.loopOffset[f]; c < part.loopOffset[f + 1]; ++c) {
                if (part.loopVertex[c] == v) {
                    part.triCorner[3 * i + k] = part.loopHalfedge[c];
                    break;
                }
            }
        }
    }
    // The skin, a rigid section's spelled out per vertex (the Fracture's rule).
    const u32 vertexCount = static_cast<u32>(part.positions.size());
    part.skin.assign(vertexCount, {});
    const auto sections = part.mesh.faceSections();
    std::vector<u32> sectionOf(vertexCount, kNone);
    for (u32 f = 0; f < faceCount; ++f) {
        for (u32 c = part.loopOffset[f]; c < part.loopOffset[f + 1]; ++c) {
            if (sectionOf[part.loopVertex[c]] == kNone) {
                sectionOf[part.loopVertex[c]] = f < sections.size() ? sections[f] : 0;
            }
        }
    }
    for (u32 v = 0; v < vertexCount; ++v) {
        if (!part.mesh.skin.empty() && v < part.mesh.skin.vertexCount()) {
            const auto list = std::as_const(part.mesh.skin).forVertex(v);
            part.skin[v].assign(list.begin(), list.end());
        }
        const u32 s = sectionOf[v];
        if (part.skin[v].empty() && s < part.mesh.sections.size() && part.mesh.sections[s].rigidNode) {
            part.skin[v].push_back(Influence{*part.mesh.sections[s].rigidNode, 1.0f});
        }
    }
    const auto groups = part.mesh.attributes.get<u32>(names::kMergeGroup, Domain::Vertex);
    part.merge.resize(vertexCount);
    for (u32 v = 0; v < vertexCount; ++v) {
        part.merge[v] = v < groups.size() ? groups[v] : v;
    }
    part.winding = fracture::WindingMeshOf(part.mesh);
    part.bvh.build(part.tri, part.positionsF);
    part.usable = !part.tri.empty();
    return true;
}

/// §2.4: whether @p part holds a volume, and which way it is wound. A probe a
/// little either side of a face reads about 1 on the inside of a closed mesh
/// and at most ½ beside a sheet; the side that reads more is the inside.
void ReadVolume(Part& part) {
    part.holds = false;
    part.winding.sign = 1.0;
    const u32 triangles = static_cast<u32>(part.tri.size() / 3);
    if (triangles == 0) {
        return;
    }
    const Vector3d low(part.box.minimum.x, part.box.minimum.y, part.box.minimum.z);
    const Vector3d high(part.box.maximum.x, part.box.maximum.y, part.box.maximum.z);
    const f64 eps = 1e-4 * Length(Sub(high, low));
    const u32 step = std::max<u32>(1, triangles / 1024);
    f64 total = 0.0;
    f64 held = 0.0;
    f64 signedHeld = 0.0;
    for (u32 t = 0; t < triangles; t += step) {
        const Vector3d& a = part.positions[part.tri[3 * t]];
        const Vector3d& b = part.positions[part.tri[3 * t + 1]];
        const Vector3d& c = part.positions[part.tri[3 * t + 2]];
        const Vector3d n = Cross(Sub(b, a), Sub(c, a));
        const f64 twice = Length(n);
        if (!(twice > 0.0)) {
            continue;
        }
        const Vector3d unit = Scale(n, 1.0 / twice);
        const Vector3d centre = Scale(AddV(AddV(a, b), c), 1.0 / 3.0);
        const f64 inner = fracture::WindingNumber(part.winding, Sub(centre, Scale(unit, eps)));
        const f64 outer = fracture::WindingNumber(part.winding, AddV(centre, Scale(unit, eps)));
        const f64 strong = std::fabs(inner) >= std::fabs(outer) ? inner : outer;
        const f64 area = 0.5 * twice;
        total += area;
        if (std::fabs(strong) >= 0.75) {
            held += area;
            signedHeld += strong > 0.0 ? area : -area;
        }
    }
    part.holds = total > 0.0 && held >= 0.5 * total;
    part.winding.sign = signedHeld < 0.0 ? -1.0 : 1.0;
}

// ============================================================================
// A run: one set of inputs cut against each other
// ============================================================================

enum class Class : u8 { Outside, Inside, Same, Opposite };

struct Segment {
    u32 a = kNone;
    u32 b = kNone;
    u32 faceA = kNone; ///< Flat triangle ids.
    u32 faceB = kNone;
    std::vector<std::pair<f64, u32>> inner; ///< Points it is split at, by parameter.
};

struct Partner {
    u32 face = kNone;
    bool same = false;
};

struct TriWork {
    std::vector<u32> interior;
    std::vector<u32> segments;
    std::vector<Partner> coplanar;
    bool cut = false;
    std::vector<std::array<u32, 3>> fragments; ///< Point ids, wound as the source.
    std::vector<u32> region;                   ///< Per fragment.
    u32 regionCount = 1;
};

struct EdgeWork {
    std::vector<std::pair<f64, u32>> points; ///< Strictly inside the edge, from its lower point.
    std::vector<std::pair<u32, u32>> spans;  ///< Stretches a cut runs along.
};

/// What a run decides for one of its inputs.
struct PartOutcome {
    std::vector<u8> wholeKept;   ///< Per uncut triangle.
    std::vector<u8> wholeTurned;
    std::vector<std::vector<u8>> fragmentKept; ///< Per cut triangle, per fragment.
    std::vector<std::vector<u8>> fragmentTurned;
    bool changed = false; ///< A target dropped something, or an operand gave something.
    u32 keptFaces = 0;
};

class Run {
public:
    Run(std::vector<Part*> parts, std::vector<u8> isTarget, const BooleanOptions& options, const Vector3d& low,
        f64 step)
        : parts_(std::move(parts)), isTarget_(std::move(isTarget)), options_(options), low_(low), step_(step) {}

    void cut(BooleanResult& result);
    void classify(BooleanResult& result);

    std::vector<PartOutcome> outcomes;
    std::vector<Part*> parts_;
    std::vector<u8> isTarget_;
    Points points_;
    std::vector<std::vector<u32>> pointOf_; ///< Per part, per vertex.
    std::vector<u32> triBase_;               ///< Per part, its first flat triangle.
    std::vector<TriWork> tris_;
    std::vector<Segment> segments_;
    std::unordered_map<u64, EdgeWork> edges_;
    u32 failed_ = 0;

    u32 partOf(u32 face) const {
        return static_cast<u32>(std::upper_bound(triBase_.begin(), triBase_.end(), face) - triBase_.begin()) - 1;
    }
    std::array<u32, 3> cornersOf(u32 face) const {
        const u32 p = partOf(face);
        const u32 t = face - triBase_[p];
        const Part& part = *parts_[p];
        return {pointOf_[p][part.tri[3 * t]], pointOf_[p][part.tri[3 * t + 1]], pointOf_[p][part.tri[3 * t + 2]]};
    }
    /// A cut triangle whose one fragment is itself: the cut only ran along its edges.
    bool alongOnly(u32 face) {
        const TriWork& work = tris_[face];
        if (work.fragments.size() != 1) {
            return false;
        }
        std::array<u32, 3> c = cornersOf(face);
        for (u32& x : c) {
            x = points_.root(x);
        }
        const auto& f = work.fragments.front();
        for (u32 k = 0; k < 3; ++k) {
            if (f[0] == c[k] && f[1] == c[(k + 1) % 3] && f[2] == c[(k + 2) % 3]) {
                return true;
            }
        }
        return false;
    }

    bool cutAgainst(u32 a, u32 b) const;
    void test(u32 faceA, u32 faceB);
    void coplanar(u32 faceA, u32 faceB, const std::array<u32, 3>& pa, const std::array<u32, 3>& pb);
    u32 edgeFacePoint(u32 e0, u32 e1, u32 face);
    u32 edgeEdgePoint(u32 a0, u32 a1, u32 b0, u32 b1);
    void onEdge(u32 e0, u32 e1, u32 point);
    void interior(u32 face, u32 point);
    void addSegment(u32 a, u32 b, u32 faceA, u32 faceB, bool alongA, bool alongB, u32 edgeA0, u32 edgeA1,
                    u32 edgeB0, u32 edgeB1);
    void triples();
    void triangulate(u32 face);
    std::vector<u32> chain(u32 c0, u32 c1);

    const BooleanOptions& options_;
    Vector3d low_;
    f64 step_ = 1.0;
};

bool Run::cutAgainst(u32 a, u32 b) const {
    if (options_.op == BooleanOp::Union) {
        return true;
    }
    // Two targets are never cut against each other (§3.2).
    return !(isTarget_[a] && isTarget_[b]);
}

void Run::onEdge(u32 e0, u32 e1, u32 point) {
    if (point == e0 || point == e1) {
        return;
    }
    const u32 lo = std::min(e0, e1);
    const u32 hi = std::max(e0, e1);
    EdgeWork& edge = edges_[PairKey(lo, hi)];
    for (const auto& entry : edge.points) {
        if (entry.second == point) {
            return;
        }
    }
    const Vector3d d = Sub(points_.position[hi], points_.position[lo]);
    const f64 length2 = Dot(d, d);
    const f64 t = length2 > 0.0 ? Dot(Sub(points_.position[point], points_.position[lo]), d) / length2 : 0.5;
    edge.points.emplace_back(t, point);
}

void Run::interior(u32 face, u32 point) {
    std::vector<u32>& list = tris_[face].interior;
    if (std::find(list.begin(), list.end(), point) == list.end()) {
        list.push_back(point);
    }
}

u32 Run::edgeFacePoint(u32 e0, u32 e1, u32 face) {
    const u32 lo = std::min(e0, e1);
    const u32 hi = std::max(e0, e1);
    Key key{Kind::EdgeFace, {lo, hi, face, 0, 0, 0}};
    if (const u32 found = points_.find(key); found != kNone) {
        return found;
    }
    const std::array<u32, 3> c = cornersOf(face);
    const f64 dl = ToDouble(Orient3(points_.grid[c[0]], points_.grid[c[1]], points_.grid[c[2]], points_.grid[lo]));
    const f64 dh = ToDouble(Orient3(points_.grid[c[0]], points_.grid[c[1]], points_.grid[c[2]], points_.grid[hi]));
    const f64 t = dl / (dl - dh);
    const Vector3d& a = points_.position[lo];
    const Vector3d& b = points_.position[hi];
    return points_.add(key, AddV(a, Scale(Sub(b, a), t)));
}

u32 Run::edgeEdgePoint(u32 a0, u32 a1, u32 b0, u32 b1) {
    std::array<u32, 2> ea{std::min(a0, a1), std::max(a0, a1)};
    std::array<u32, 2> eb{std::min(b0, b1), std::max(b0, b1)};
    if (eb < ea) {
        std::swap(ea, eb);
    }
    Key key{Kind::EdgeEdge, {ea[0], ea[1], eb[0], eb[1], 0, 0}};
    if (const u32 found = points_.find(key); found != kNone) {
        return found;
    }
    // The point of the first line nearest the second: the two meet there.
    const Vector3d& p = points_.position[ea[0]];
    const Vector3d u = Sub(points_.position[ea[1]], p);
    const Vector3d& q = points_.position[eb[0]];
    const Vector3d v = Sub(points_.position[eb[1]], q);
    const Vector3d w = Sub(p, q);
    const f64 a = Dot(u, u);
    const f64 b = Dot(u, v);
    const f64 c = Dot(v, v);
    const f64 d = Dot(u, w);
    const f64 e = Dot(v, w);
    const f64 den = a * c - b * b;
    f64 s = den > 0.0 ? (b * e - c * d) / den : 0.0;
    s = std::clamp(s, 0.0, 1.0);
    return points_.add(key, AddV(p, Scale(u, s)));
}

void Run::addSegment(u32 a, u32 b, u32 faceA, u32 faceB, bool alongA, bool alongB, u32 edgeA0, u32 edgeA1,
                     u32 edgeB0, u32 edgeB1) {
    const u32 id = static_cast<u32>(segments_.size());
    segments_.push_back(Segment{a, b, faceA, faceB, {}});
    if (alongA) {
        edges_[PairKey(edgeA0, edgeA1)].spans.emplace_back(a, b);
    } else {
        tris_[faceA].segments.push_back(id);
    }
    if (alongB) {
        edges_[PairKey(edgeB0, edgeB1)].spans.emplace_back(a, b);
    } else {
        tris_[faceB].segments.push_back(id);
    }
    tris_[faceA].cut = true;
    tris_[faceB].cut = true;
}

void Run::test(u32 faceA, u32 faceB) {
    const std::array<u32, 3> pa = cornersOf(faceA);
    const std::array<u32, 3> pb = cornersOf(faceB);
    const std::array<Grid, 3> ga{points_.grid[pa[0]], points_.grid[pa[1]], points_.grid[pa[2]]};
    const std::array<Grid, 3> gb{points_.grid[pb[0]], points_.grid[pb[1]], points_.grid[pb[2]]};
    // Each corner against the other's plane, a corner within `kFlat` steps of
    // it on it. Decided here, once for the pair, and only for the planes.
    const auto plane = [](const std::array<Grid, 3>& t, const Grid& d) {
        const Grid n = NormalOf(t[0], t[1], t[2]);
        const f64 length = std::sqrt(static_cast<f64>(n[0]) * static_cast<f64>(n[0]) +
                                     static_cast<f64>(n[1]) * static_cast<f64>(n[1]) +
                                     static_cast<f64>(n[2]) * static_cast<f64>(n[2]));
        const Wide o = Orient3(t[0], t[1], t[2], d);
        if (std::fabs(ToDouble(o)) <= kFlat * length) {
            return 0;
        }
        return SignOf(o);
    };
    std::array<int, 3> sa{};
    std::array<int, 3> sb{};
    for (u32 k = 0; k < 3; ++k) {
        sa[k] = plane(gb, ga[k]);
        sb[k] = plane(ga, gb[k]);
    }
    // A snap is a pair's: one side flat on the other's plane is both flat.
    const bool flatA = sa[0] == 0 && sa[1] == 0 && sa[2] == 0;
    const bool flatB = sb[0] == 0 && sb[1] == 0 && sb[2] == 0;
    if (flatA != flatB) {
        if (flatA) {
            sb = {0, 0, 0};
        } else {
            sa = {0, 0, 0};
        }
    }
    const auto apart = [](const std::array<int, 3>& s) {
        return (s[0] > 0 && s[1] > 0 && s[2] > 0) || (s[0] < 0 && s[1] < 0 && s[2] < 0);
    };
    if (apart(sa) || apart(sb)) {
        return;
    }
    if (sa[0] == 0 && sa[1] == 0 && sa[2] == 0) {
        coplanar(faceA, faceB, pa, pb);
        return;
    }
    // The points of each triangle on the two planes' line that lie in the
    // other triangle: its edges through the other's plane, and its corners on
    // it. The crossing is the stretch between the outermost of them.
    std::vector<u32> found;
    const auto side = [&](u32 self, u32 other, const std::array<u32, 3>& ps, const std::array<u32, 3>& po,
                          const std::array<Grid, 3>& gs, const std::array<Grid, 3>& go,
                          const std::array<int, 3>& s) {
        for (u32 i = 0; i < 3; ++i) {
            const u32 j = (i + 1) % 3;
            if (s[i] * s[j] >= 0) {
                continue;
            }
            std::array<int, 3> t{};
            u32 zeros = 0;
            bool positive = false;
            bool negative = false;
            for (u32 k = 0; k < 3; ++k) {
                t[k] = Orient3Sign(gs[i], gs[j], go[k], go[(k + 1) % 3]);
                zeros += t[k] == 0 ? 1 : 0;
                positive = positive || t[k] > 0;
                negative = negative || t[k] < 0;
            }
            if (positive && negative) {
                continue;
            }
            if (zeros == 0) {
                const u32 x = edgeFacePoint(ps[i], ps[j], other);
                onEdge(ps[i], ps[j], x);
                interior(other, x);
                found.push_back(x);
            } else if (zeros == 1) {
                for (u32 k = 0; k < 3; ++k) {
                    if (t[k] == 0) {
                        const u32 x = edgeEdgePoint(ps[i], ps[j], po[k], po[(k + 1) % 3]);
                        onEdge(ps[i], ps[j], x);
                        onEdge(po[k], po[(k + 1) % 3], x);
                        found.push_back(x);
                    }
                }
            } else {
                for (u32 m = 0; m < 3; ++m) {
                    if (t[m] == 0 && t[(m + 2) % 3] == 0) {
                        onEdge(ps[i], ps[j], po[m]);
                        found.push_back(po[m]);
                    }
                }
            }
        }
        const u32 axis = DominantAxis(NormalOf(go[0], go[1], go[2]));
        for (u32 i = 0; i < 3; ++i) {
            if (s[i] != 0) {
                continue;
            }
            const Where where = Locate2(go, gs[i], axis);
            if (where.place == Place::Inside) {
                interior(other, ps[i]);
                found.push_back(ps[i]);
            } else if (where.place == Place::OnEdge) {
                onEdge(po[where.index], po[(where.index + 1) % 3], ps[i]);
                found.push_back(ps[i]);
            } else if (where.place == Place::OnCorner) {
                found.push_back(ps[i]);
            }
        }
        (void)self;
    };
    side(faceA, faceB, pa, pb, ga, gb, sa);
    side(faceB, faceA, pb, pa, gb, ga, sb);
    std::sort(found.begin(), found.end());
    found.erase(std::unique(found.begin(), found.end()), found.end());
    if (found.size() < 2) {
        return;
    }
    // Along the line: the direction the two normals' cross product gives.
    const Vector3d& a0 = points_.position[pa[0]];
    const Vector3d& b0 = points_.position[pb[0]];
    const Vector3d na = Cross(Sub(points_.position[pa[1]], a0), Sub(points_.position[pa[2]], a0));
    const Vector3d nb = Cross(Sub(points_.position[pb[1]], b0), Sub(points_.position[pb[2]], b0));
    const Vector3d dir = Cross(na, nb);
    u32 first = found[0];
    u32 last = found[0];
    f64 lowest = Dot(points_.position[first], dir);
    f64 highest = lowest;
    for (u32 x : found) {
        const f64 at = Dot(points_.position[x], dir);
        if (at < lowest) {
            lowest = at;
            first = x;
        }
        if (at > highest) {
            highest = at;
            last = x;
        }
    }
    if (first == last) {
        return;
    }
    // A stretch along one of a triangle's own edges is that edge's, not a
    // segment across the triangle.
    const auto along = [](const std::array<int, 3>& s, const std::array<u32, 3>& p, u32& e0, u32& e1) {
        for (u32 i = 0; i < 3; ++i) {
            const u32 j = (i + 1) % 3;
            if (s[i] == 0 && s[j] == 0 && s[(i + 2) % 3] != 0) {
                e0 = p[i];
                e1 = p[j];
                return true;
            }
        }
        return false;
    };
    u32 ea0 = kNone;
    u32 ea1 = kNone;
    u32 eb0 = kNone;
    u32 eb1 = kNone;
    const bool alongA = along(sa, pa, ea0, ea1);
    const bool alongB = along(sb, pb, eb0, eb1);
    addSegment(first, last, faceA, faceB, alongA, alongB, ea0, ea1, eb0, eb1);
}

void Run::coplanar(u32 faceA, u32 faceB, const std::array<u32, 3>& pa, const std::array<u32, 3>& pb) {
    const std::array<Grid, 3> ga{points_.grid[pa[0]], points_.grid[pa[1]], points_.grid[pa[2]]};
    const std::array<Grid, 3> gb{points_.grid[pb[0]], points_.grid[pb[1]], points_.grid[pb[2]]};
    const u32 axis = DominantAxis(NormalOf(ga[0], ga[1], ga[2]));
    const int oa = Orient2(ga[0], ga[1], ga[2], axis);
    const int ob = Orient2(gb[0], gb[1], gb[2], axis);
    if (oa == 0 || ob == 0) {
        return;
    }
    bool overlap = false;
    // Corners of each inside, or on an edge of, the other.
    std::array<Where, 3> bInA{};
    std::array<Where, 3> aInB{};
    for (u32 k = 0; k < 3; ++k) {
        bInA[k] = Locate2(ga, gb[k], axis);
        aInB[k] = Locate2(gb, ga[k], axis);
        if (bInA[k].place == Place::Inside) {
            interior(faceA, pb[k]);
            overlap = true;
        } else if (bInA[k].place == Place::OnEdge) {
            onEdge(pa[bInA[k].index], pa[(bInA[k].index + 1) % 3], pb[k]);
        }
        if (aInB[k].place == Place::Inside) {
            interior(faceB, pa[k]);
            overlap = true;
        } else if (aInB[k].place == Place::OnEdge) {
            onEdge(pb[aInB[k].index], pb[(aInB[k].index + 1) % 3], pa[k]);
        }
    }
    // Proper crossings of an edge of each.
    std::array<std::array<u32, 3>, 3> crossing{};
    for (auto& row : crossing) {
        row.fill(kNone);
    }
    for (u32 i = 0; i < 3; ++i) {
        const Grid& p = ga[i];
        const Grid& q = ga[(i + 1) % 3];
        for (u32 k = 0; k < 3; ++k) {
            const Grid& r = gb[k];
            const Grid& s = gb[(k + 1) % 3];
            const int o1 = Orient2(p, q, r, axis);
            const int o2 = Orient2(p, q, s, axis);
            const int o3 = Orient2(r, s, p, axis);
            const int o4 = Orient2(r, s, q, axis);
            if (o1 * o2 < 0 && o3 * o4 < 0) {
                const u32 x = edgeEdgePoint(pa[i], pa[(i + 1) % 3], pb[k], pb[(k + 1) % 3]);
                onEdge(pa[i], pa[(i + 1) % 3], x);
                onEdge(pb[k], pb[(k + 1) % 3], x);
                crossing[i][k] = x;
                overlap = true;
            }
        }
    }
    if (!overlap) {
        // Identical, or one inside the other with its corners on the edges.
        const auto centre = [](const std::array<Grid, 3>& g) {
            return Grid{(g[0][0] + g[1][0] + g[2][0]) / 3, (g[0][1] + g[1][1] + g[2][1]) / 3,
                        (g[0][2] + g[1][2] + g[2][2]) / 3};
        };
        overlap = Locate2(ga, centre(gb), axis).place == Place::Inside ||
                  Locate2(gb, centre(ga), axis).place == Place::Inside;
    }
    if (!overlap) {
        return;
    }
    // Each triangle's edges, clipped to the other, are segments of the other.
    const auto clip = [&](u32 self, u32 other, const std::array<u32, 3>& ps, const std::array<Grid, 3>& gs,
                          const std::array<u32, 3>& po, const std::array<Grid, 3>& go,
                          const std::array<Where, 3>& selfInOther, bool selfIsA) {
        for (u32 k = 0; k < 3; ++k) {
            const u32 e0 = ps[k];
            const u32 e1 = ps[(k + 1) % 3];
            std::vector<std::pair<f64, u32>> along;
            const Vector3d& from = points_.position[e0];
            const Vector3d d = Sub(points_.position[e1], from);
            const f64 length2 = Dot(d, d);
            const auto param = [&](u32 x) {
                return length2 > 0.0 ? Dot(Sub(points_.position[x], from), d) / length2 : 0.0;
            };
            along.emplace_back(0.0, e0);
            along.emplace_back(1.0, e1);
            for (u32 i = 0; i < 3; ++i) {
                const u32 x = selfIsA ? crossing[k][i] : crossing[i][k];
                if (x != kNone) {
                    along.emplace_back(param(x), x);
                }
                // The other's corners lying on this edge.
                const Where w = Locate2(gs, go[i], axis);
                if (w.place == Place::OnEdge && w.index == k) {
                    along.emplace_back(param(po[i]), po[i]);
                }
            }
            std::sort(along.begin(), along.end());
            for (std::size_t n = 0; n + 1 < along.size(); ++n) {
                const u32 x = along[n].second;
                const u32 y = along[n + 1].second;
                if (x == y) {
                    continue;
                }
                // The piece is a segment of the other when its middle is
                // strictly inside it; on its boundary it is the other's edge.
                const Grid& gx = points_.grid[x];
                const Grid& gy = points_.grid[y];
                const bool xVertex = points_.find(Key{Kind::Vertex, {static_cast<u32>(gx[0]), static_cast<u32>(gx[1]),
                                                                     static_cast<u32>(gx[2]), 0, 0, 0}}) == x;
                const bool yVertex = points_.find(Key{Kind::Vertex, {static_cast<u32>(gy[0]), static_cast<u32>(gy[1]),
                                                                     static_cast<u32>(gy[2]), 0, 0, 0}}) == y;
                bool inside = false;
                if (xVertex && yVertex) {
                    const Grid mid{gx[0] + gy[0], gx[1] + gy[1], gx[2] + gy[2]};
                    const std::array<Grid, 3> doubled{
                        Grid{2 * go[0][0], 2 * go[0][1], 2 * go[0][2]},
                        Grid{2 * go[1][0], 2 * go[1][1], 2 * go[1][2]},
                        Grid{2 * go[2][0], 2 * go[2][1], 2 * go[2][2]}};
                    inside = Locate2(doubled, mid, axis).place == Place::Inside;
                } else {
                    // A crossing has no grid point: its middle is tested in doubles.
                    const Vector3d m = Scale(AddV(points_.position[x], points_.position[y]), 0.5);
                    const Vector2<f64> p = Project(m, axis);
                    std::array<Vector2<f64>, 3> t{};
                    for (u32 i = 0; i < 3; ++i) {
                        t[i] = Project(points_.position[po[i]], axis);
                    }
                    const f64 o = (t[1].x - t[0].x) * (t[2].y - t[0].y) - (t[1].y - t[0].y) * (t[2].x - t[0].x);
                    inside = true;
                    for (u32 i = 0; i < 3; ++i) {
                        const Vector2<f64>& u = t[i];
                        const Vector2<f64>& v = t[(i + 1) % 3];
                        const f64 s = ((v.x - u.x) * (p.y - u.y) - (v.y - u.y) * (p.x - u.x)) * o;
                        inside = inside && s > 0.0;
                    }
                }
                if (inside) {
                    const u32 id = static_cast<u32>(segments_.size());
                    segments_.push_back(Segment{x, y, other, self, {}});
                    tris_[other].segments.push_back(id);
                }
            }
            (void)selfInOther;
        }
    };
    clip(faceA, faceB, pa, ga, pb, gb, aInB, true);
    clip(faceB, faceA, pb, gb, pa, ga, bInA, false);
    const bool same = oa == ob;
    tris_[faceA].coplanar.push_back(Partner{faceB, same});
    tris_[faceB].coplanar.push_back(Partner{faceA, same});
    tris_[faceA].cut = true;
    tris_[faceB].cut = true;
}

/// Two cuts crossing inside a third triangle: where three inputs meet, or two
/// and one crossing itself. The point is decided once, by its key, and given
/// to all three, so none of them leaves a T-junction on the cut.
void Run::triples() {
    std::unordered_map<u64, std::vector<u32>> byPair;
    for (u32 s = 0; s < segments_.size(); ++s) {
        byPair[PairKey(segments_[s].faceA, segments_[s].faceB)].push_back(s);
    }
    std::unordered_set<u64> decided;
    for (u32 face = 0; face < tris_.size(); ++face) {
        const std::vector<u32>& list = tris_[face].segments;
        if (list.size() < 2) {
            continue;
        }
        const std::array<u32, 3> c = cornersOf(face);
        const Vector3d& c0 = points_.position[c[0]];
        const Vector3d n = Cross(Sub(points_.position[c[1]], c0), Sub(points_.position[c[2]], c0));
        const u32 axis = std::fabs(n.x) >= std::fabs(n.y) && std::fabs(n.x) >= std::fabs(n.z)
                             ? 0
                             : (std::fabs(n.y) >= std::fabs(n.z) ? 1 : 2);
        for (std::size_t i = 0; i < list.size(); ++i) {
            for (std::size_t j = i + 1; j < list.size(); ++j) {
                const Segment s1 = segments_[list[i]];
                const Segment s2 = segments_[list[j]];
                const u32 f1 = s1.faceA == face ? s1.faceB : s1.faceA;
                const u32 f2 = s2.faceA == face ? s2.faceB : s2.faceA;
                if (f1 == f2 || s1.a == s2.a || s1.a == s2.b || s1.b == s2.a || s1.b == s2.b) {
                    continue;
                }
                std::array<std::pair<u32, u32>, 3> faces{std::pair<u32, u32>{partOf(face), face},
                                                         std::pair<u32, u32>{partOf(f1), f1},
                                                         std::pair<u32, u32>{partOf(f2), f2}};
                std::sort(faces.begin(), faces.end());
                const Key key{Kind::Triple,
                              {faces[0].first, faces[0].second, faces[1].first, faces[1].second, faces[2].first,
                               faces[2].second}};
                const u64 tag = KeyHash{}(key);
                if (points_.find(key) != kNone || decided.count(tag) != 0) {
                    continue;
                }
                decided.insert(tag);
                const Vector2<f64> p = Project(points_.position[s1.a], axis);
                const Vector2<f64> q = Project(points_.position[s1.b], axis);
                const Vector2<f64> r = Project(points_.position[s2.a], axis);
                const Vector2<f64> s = Project(points_.position[s2.b], axis);
                const f64 d1 = (q.x - p.x) * (r.y - p.y) - (q.y - p.y) * (r.x - p.x);
                const f64 d2 = (q.x - p.x) * (s.y - p.y) - (q.y - p.y) * (s.x - p.x);
                const f64 d3 = (s.x - r.x) * (p.y - r.y) - (s.y - r.y) * (p.x - r.x);
                const f64 d4 = (s.x - r.x) * (q.y - r.y) - (s.y - r.y) * (q.x - r.x);
                if (!(d1 * d2 < 0.0 && d3 * d4 < 0.0)) {
                    continue;
                }
                const f64 t = d3 / (d3 - d4);
                const Vector3d at =
                    AddV(points_.position[s1.a], Scale(Sub(points_.position[s1.b], points_.position[s1.a]), t));
                const u32 x = points_.add(key, at);
                const auto split = [&](u32 seg) {
                    Segment& g = segments_[seg];
                    const Vector3d d = Sub(points_.position[g.b], points_.position[g.a]);
                    const f64 length2 = Dot(d, d);
                    const f64 u = length2 > 0.0 ? Dot(Sub(at, points_.position[g.a]), d) / length2 : 0.5;
                    g.inner.emplace_back(u, x);
                };
                split(list[i]);
                split(list[j]);
                interior(face, x);
                interior(f1, x);
                interior(f2, x);
                if (const auto third = byPair.find(PairKey(f1, f2)); third != byPair.end()) {
                    for (u32 seg : third->second) {
                        split(seg);
                    }
                }
            }
        }
    }
}

/// The points along the edge from @p c0 to @p c1, both ends included.
std::vector<u32> Run::chain(u32 c0, u32 c1) {
    std::vector<u32> out{c0};
    const auto found = edges_.find(PairKey(c0, c1));
    if (found != edges_.end()) {
        std::vector<std::pair<f64, u32>> list = found->second.points;
        std::sort(list.begin(), list.end());
        if (c0 > c1) {
            std::reverse(list.begin(), list.end());
        }
        for (const auto& entry : list) {
            out.push_back(entry.second);
        }
    }
    out.push_back(c1);
    return out;
}

void Run::triangulate(u32 face) {
    TriWork& work = tris_[face];
    const std::array<u32, 3> c = cornersOf(face);
    const Vector3d& c0 = points_.position[c[0]];
    const Vector3d n = Cross(Sub(points_.position[c[1]], c0), Sub(points_.position[c[2]], c0));
    const u32 axis =
        std::fabs(n.x) >= std::fabs(n.y) && std::fabs(n.x) >= std::fabs(n.z) ? 0 : (std::fabs(n.y) >= std::fabs(n.z) ? 1 : 2);
    const bool flip = Axis(n, axis) < 0.0;
    std::vector<u32> local;
    std::unordered_map<u32, u32> localOf;
    const auto use = [&](u32 point) {
        const auto [it, fresh] = localOf.emplace(point, static_cast<u32>(local.size()));
        if (fresh) {
            local.push_back(point);
        }
        return it->second;
    };
    std::vector<u32> segments;
    for (u32 k = 0; k < 3; ++k) {
        const std::vector<u32> along = chain(c[k], c[(k + 1) % 3]);
        for (std::size_t i = 0; i + 1 < along.size(); ++i) {
            const u32 a = use(along[i]);
            const u32 b = use(along[i + 1]);
            if (a != b) {
                segments.push_back(a);
                segments.push_back(b);
            }
        }
    }
    for (u32 p : work.interior) {
        use(p);
    }
    for (u32 s : work.segments) {
        Segment& g = segments_[s];
        std::sort(g.inner.begin(), g.inner.end());
        u32 previous = use(g.a);
        for (const auto& entry : g.inner) {
            const u32 x = use(entry.second);
            if (x != previous) {
                segments.push_back(previous);
                segments.push_back(x);
            }
            previous = x;
        }
        const u32 last = use(g.b);
        if (last != previous) {
            segments.push_back(previous);
            segments.push_back(last);
        }
    }
    std::vector<Vector2<f64>> flat;
    flat.reserve(local.size());
    for (u32 p : local) {
        flat.push_back(Project(points_.position[p], axis));
    }
    const Cdt2d cdt = ConstrainedTriangulation2d(flat, segments);
    if (!cdt.ok()) {
        ++failed_;
    }
    const u32 inputs = static_cast<u32>(local.size());
    // The snap's merges are the run's: one point everywhere.
    for (u32 i = 0; i < cdt.same.size() && i < inputs; ++i) {
        if (cdt.same[i] != i && cdt.same[i] < inputs) {
            points_.unite(local[i], local[cdt.same[i]]);
        }
    }
    // Crossings the triangulation added lie in this triangle alone.
    const Vector2<f64> f0 = Project(points_.position[c[0]], axis);
    const Vector2<f64> f1 = Project(points_.position[c[1]], axis);
    const Vector2<f64> f2 = Project(points_.position[c[2]], axis);
    for (u32 i = inputs; i < cdt.points.size(); ++i) {
        const Vector2<f64>& p = cdt.points[i];
        const f64 den = (f1.y - f2.y) * (f0.x - f2.x) + (f2.x - f1.x) * (f0.y - f2.y);
        f64 w0 = 1.0 / 3.0;
        f64 w1 = 1.0 / 3.0;
        if (std::fabs(den) > 0.0) {
            w0 = ((f1.y - f2.y) * (p.x - f2.x) + (f2.x - f1.x) * (p.y - f2.y)) / den;
            w1 = ((f2.y - f0.y) * (p.x - f2.x) + (f0.x - f2.x) * (p.y - f2.y)) / den;
        }
        const f64 w2 = 1.0 - w0 - w1;
        const Vector3d at = AddV(AddV(Scale(points_.position[c[0]], w0), Scale(points_.position[c[1]], w1)),
                                 Scale(points_.position[c[2]], w2));
        local.push_back(points_.add(Key{Kind::Local, {partOf(face), face, i, 0, 0, 0}}, at));
    }
    work.fragments.clear();
    work.region.clear();
    for (std::size_t t = 0; t < cdt.regions.size(); ++t) {
        std::array<u32, 3> f{local[cdt.triangles[3 * t]], local[cdt.triangles[3 * t + 1]],
                             local[cdt.triangles[3 * t + 2]]};
        if (flip) {
            std::swap(f[1], f[2]);
        }
        work.fragments.push_back(f);
        work.region.push_back(cdt.regions[t]);
    }
    work.regionCount = std::max<u32>(1, cdt.regionCount);
}

void Run::cut(BooleanResult& result) {
    // Points of the inputs' vertices, by grid position.
    pointOf_.resize(parts_.size());
    triBase_.assign(1, 0);
    for (u32 p = 0; p < parts_.size(); ++p) {
        const Part& part = *parts_[p];
        pointOf_[p].resize(part.positions.size());
        for (u32 v = 0; v < part.positions.size(); ++v) {
            const Vector3d& at = part.positions[v];
            const Grid g{static_cast<i64>(std::llround((at.x - low_.x) / step_)),
                         static_cast<i64>(std::llround((at.y - low_.y) / step_)),
                         static_cast<i64>(std::llround((at.z - low_.z) / step_))};
            pointOf_[p][v] = points_.add(
                Key{Kind::Vertex, {static_cast<u32>(g[0]), static_cast<u32>(g[1]), static_cast<u32>(g[2]), 0, 0, 0}},
                at, g);
        }
        triBase_.push_back(triBase_.back() + static_cast<u32>(part.tri.size() / 3));
    }
    triBase_.pop_back();
    const u32 faces = parts_.empty() ? 0 : triBase_.back() + static_cast<u32>(parts_.back()->tri.size() / 3);
    tris_.assign(faces, {});

    // Every pair of triangles from two inputs cut against each other.
    std::vector<u32> candidates;
    for (u32 a = 0; a < parts_.size(); ++a) {
        for (u32 b = a + 1; b < parts_.size(); ++b) {
            if (!cutAgainst(a, b)) {
                continue;
            }
            const Part& pa = *parts_[a];
            const Part& pb = *parts_[b];
            const f32 pad = static_cast<f32>(step_ * 4.0);
            for (u32 t = 0; t < pa.tri.size() / 3; ++t) {
                const Vector3f& p0 = pa.positionsF[pa.tri[3 * t]];
                const Vector3f& p1 = pa.positionsF[pa.tri[3 * t + 1]];
                const Vector3f& p2 = pa.positionsF[pa.tri[3 * t + 2]];
                const Vector3f lo(std::min({p0.x, p1.x, p2.x}) - pad, std::min({p0.y, p1.y, p2.y}) - pad,
                                  std::min({p0.z, p1.z, p2.z}) - pad);
                const Vector3f hi(std::max({p0.x, p1.x, p2.x}) + pad, std::max({p0.y, p1.y, p2.y}) + pad,
                                  std::max({p0.z, p1.z, p2.z}) + pad);
                candidates.clear();
                pb.bvh.overlapping(lo, hi, candidates);
                std::sort(candidates.begin(), candidates.end());
                const u32 fa = triBase_[a] + t;
                const std::array<u32, 3> ca = cornersOf(fa);
                if (ca[0] == ca[1] || ca[1] == ca[2] || ca[0] == ca[2]) {
                    continue;
                }
                for (u32 u : candidates) {
                    const u32 fb = triBase_[b] + u;
                    const std::array<u32, 3> cb = cornersOf(fb);
                    if (cb[0] == cb[1] || cb[1] == cb[2] || cb[0] == cb[2]) {
                        continue;
                    }
                    test(fa, fb);
                }
            }
        }
    }
    result.met = result.met || !segments_.empty();
    triples();
    // A point inside a triangle that its triangulation would snap onto an edge
    // is put on the edge for both triangles, or the neighbour leaves a sliver
    // open beside it (the CDT's snap is 16 steps of a 2^26 grid on the
    // triangle; twice that is taken here).
    for (u32 face = 0; face < tris_.size(); ++face) {
        if (tris_[face].interior.empty()) {
            continue;
        }
        const std::array<u32, 3> c = cornersOf(face);
        f64 longest = 0.0;
        for (u32 k = 0; k < 3; ++k) {
            longest = std::max(longest, Length(Sub(points_.position[c[(k + 1) % 3]], points_.position[c[k]])));
        }
        const f64 near = 32.0 * longest / kGrid;
        for (const u32 x : std::vector<u32>(tris_[face].interior)) {
            const Vector3d& p = points_.position[x];
            for (u32 k = 0; k < 3; ++k) {
                const Vector3d& a = points_.position[c[k]];
                const Vector3d d = Sub(points_.position[c[(k + 1) % 3]], a);
                const f64 length2 = Dot(d, d);
                if (!(length2 > 0.0)) {
                    continue;
                }
                const f64 t = std::clamp(Dot(Sub(p, a), d) / length2, 0.0, 1.0);
                if (Length(Sub(p, AddV(a, Scale(d, t)))) <= near) {
                    onEdge(c[k], c[(k + 1) % 3], x);
                }
            }
        }
    }

    // A triangle a cut reaches, through itself or through a point on an edge.
    for (u32 face = 0; face < tris_.size(); ++face) {
        TriWork& work = tris_[face];
        if (!work.cut) {
            const std::array<u32, 3> c = cornersOf(face);
            for (u32 k = 0; k < 3 && !work.cut; ++k) {
                const auto found = edges_.find(PairKey(c[k], c[(k + 1) % 3]));
                work.cut = found != edges_.end() && !found->second.points.empty();
            }
            work.cut = work.cut || !work.interior.empty();
        }
        if (work.cut) {
            const std::array<u32, 3> c = cornersOf(face);
            if (c[0] == c[1] || c[1] == c[2] || c[0] == c[2]) {
                work.cut = false;
                continue;
            }
            triangulate(face);
        }
    }
    // Points the snap made one, one in every triangle.
    for (TriWork& work : tris_) {
        if (!work.cut) {
            continue;
        }
        std::vector<std::array<u32, 3>> kept;
        std::vector<u32> regions;
        for (std::size_t i = 0; i < work.fragments.size(); ++i) {
            std::array<u32, 3> f = work.fragments[i];
            for (u32& p : f) {
                p = points_.root(p);
            }
            if (f[0] == f[1] || f[1] == f[2] || f[0] == f[2]) {
                continue;
            }
            kept.push_back(f);
            regions.push_back(work.region[i]);
        }
        work.fragments = std::move(kept);
        work.region = std::move(regions);
    }
    for (u32 p = 0; p < parts_.size(); ++p) {
        std::unordered_set<u32> cutFaces;
        for (u32 t = 0; t < parts_[p]->tri.size() / 3; ++t) {
            if (tris_[triBase_[p] + t].cut) {
                cutFaces.insert(parts_[p]->triFace[t]);
            }
        }
        result.cutFaces += static_cast<u32>(cutFaces.size());
    }
    for (const Segment& s : segments_) {
        result.cutLines.push_back(Vector3f(static_cast<f32>(points_.position[s.a].x),
                                           static_cast<f32>(points_.position[s.a].y),
                                           static_cast<f32>(points_.position[s.a].z)));
        result.cutLines.push_back(Vector3f(static_cast<f32>(points_.position[s.b].x),
                                           static_cast<f32>(points_.position[s.b].y),
                                           static_cast<f32>(points_.position[s.b].z)));
    }
    result.failedFaces += failed_;
}

// ============================================================================
// Patches and what each operation keeps
// ============================================================================

struct UnionFind {
    std::vector<u32> parent;
    explicit UnionFind(u32 n) : parent(n) {
        std::iota(parent.begin(), parent.end(), 0u);
    }
    u32 find(u32 x) {
        while (parent[x] != x) {
            parent[x] = parent[parent[x]];
            x = parent[x];
        }
        return x;
    }
    void unite(u32 a, u32 b) {
        a = find(a);
        b = find(b);
        if (a != b) {
            parent[std::max(a, b)] = std::min(a, b);
        }
    }
};

/// Per input, the faces of another it lies on: bit 1 the same way, bit 2 the
/// opposite way.
using OnSignature = std::vector<std::pair<u32, u8>>;

bool Keeps(BooleanOp op, bool closeCut, u32 self, std::span<const u8> isTarget, std::span<const Class> classes,
           bool& turn) {
    turn = false;
    const u32 n = static_cast<u32>(classes.size());
    if (op == BooleanOp::Union) {
        for (u32 j = 0; j < n; ++j) {
            if (j == self) {
                continue;
            }
            const Class c = classes[j];
            if (c == Class::Inside || c == Class::Opposite || (c == Class::Same && j < self)) {
                return false;
            }
        }
        return true;
    }
    if (isTarget[self]) {
        for (u32 j = 0; j < n; ++j) {
            if (j == self || isTarget[j]) {
                continue;
            }
            const Class c = classes[j];
            if (op == BooleanOp::Intersect && !(c == Class::Inside || c == Class::Same)) {
                return false;
            }
            if (op == BooleanOp::Subtract && !(c == Class::Outside || c == Class::Opposite)) {
                return false;
            }
        }
        return true;
    }
    if (!closeCut) {
        return false;
    }
    for (u32 j = 0; j < n; ++j) {
        if (j == self) {
            continue;
        }
        const Class c = classes[j];
        if (isTarget[j]) {
            if (c != Class::Inside) {
                return false;
            }
            continue;
        }
        if (op == BooleanOp::Intersect) {
            if (!(c == Class::Inside || (c == Class::Same && self < j))) {
                return false;
            }
        } else if (!(c == Class::Outside || (c == Class::Same && self < j))) {
            return false;
        }
    }
    turn = op == BooleanOp::Subtract;
    return true;
}

void Run::classify(BooleanResult& result) {
    outcomes.assign(parts_.size(), {});
    for (u32 p = 0; p < parts_.size(); ++p) {
        const Part& part = *parts_[p];
        const u32 triangles = static_cast<u32>(part.tri.size() / 3);
        // Nodes: a whole triangle is one, a cut one a node per region.
        std::vector<u32> nodeBase(triangles + 1, 0);
        for (u32 t = 0; t < triangles; ++t) {
            const TriWork& work = tris_[triBase_[p] + t];
            nodeBase[t + 1] = nodeBase[t] + (work.cut ? work.regionCount : 1);
        }
        const u32 nodes = nodeBase[triangles];
        UnionFind find(nodes);
        std::vector<OnSignature> on(nodes);
        std::vector<u8> onDone(nodes, 0);

        // Regions lying on a coplanar partner.
        for (u32 t = 0; t < triangles; ++t) {
            const u32 face = triBase_[p] + t;
            const TriWork& work = tris_[face];
            if (!work.cut || work.coplanar.empty()) {
                continue;
            }
            const std::array<u32, 3> c = cornersOf(face);
            const Vector3d& c0 = points_.position[c[0]];
            const Vector3d n = Cross(Sub(points_.position[c[1]], c0), Sub(points_.position[c[2]], c0));
            const u32 axis = std::fabs(n.x) >= std::fabs(n.y) && std::fabs(n.x) >= std::fabs(n.z)
                                 ? 0
                                 : (std::fabs(n.y) >= std::fabs(n.z) ? 1 : 2);
            std::vector<f64> best(work.regionCount, -1.0);
            std::vector<Vector3d> sample(work.regionCount);
            for (std::size_t i = 0; i < work.fragments.size(); ++i) {
                const auto& f = work.fragments[i];
                const Vector3d& a = points_.position[f[0]];
                const Vector3d& b = points_.position[f[1]];
                const Vector3d& d = points_.position[f[2]];
                const f64 area = Length(Cross(Sub(b, a), Sub(d, a)));
                const u32 r = work.region[i];
                if (r < best.size() && area > best[r]) {
                    best[r] = area;
                    sample[r] = Scale(AddV(AddV(a, b), d), 1.0 / 3.0);
                }
            }
            for (u32 r = 0; r < work.regionCount; ++r) {
                if (best[r] < 0.0) {
                    continue;
                }
                const Vector2<f64> q = Project(sample[r], axis);
                OnSignature sig;
                for (const Partner& partner : work.coplanar) {
                    const std::array<u32, 3> pc = cornersOf(partner.face);
                    std::array<Vector2<f64>, 3> tp{};
                    for (u32 k = 0; k < 3; ++k) {
                        tp[k] = Project(points_.position[pc[k]], axis);
                    }
                    const f64 o = (tp[1].x - tp[0].x) * (tp[2].y - tp[0].y) - (tp[1].y - tp[0].y) * (tp[2].x - tp[0].x);
                    bool inside = o != 0.0;
                    for (u32 k = 0; k < 3 && inside; ++k) {
                        const Vector2<f64>& u = tp[k];
                        const Vector2<f64>& v = tp[(k + 1) % 3];
                        inside = ((v.x - u.x) * (q.y - u.y) - (v.y - u.y) * (q.x - u.x)) * o > 0.0;
                    }
                    if (!inside) {
                        continue;
                    }
                    const u32 other = partOf(partner.face);
                    // A mesh read as wound inward has its faces turned.
                    const bool flipped = (parts_[other]->winding.sign < 0.0) != (part.winding.sign < 0.0);
                    const u8 bit = (partner.same != flipped) ? u8{1} : u8{2};
                    auto it = std::find_if(sig.begin(), sig.end(), [&](const auto& e) { return e.first == other; });
                    if (it == sig.end()) {
                        sig.emplace_back(other, bit);
                    } else {
                        it->second |= bit;
                    }
                }
                std::sort(sig.begin(), sig.end());
                on[nodeBase[t] + r] = std::move(sig);
            }
        }

        // Joined across every stretch of a triangle's edge that no cut runs along.
        std::unordered_map<u64, std::vector<u32>> trianglesOf;
        for (u32 t = 0; t < triangles; ++t) {
            const std::array<u32, 3> c = cornersOf(triBase_[p] + t);
            for (u32 k = 0; k < 3; ++k) {
                trianglesOf[PairKey(c[k], c[(k + 1) % 3])].push_back(t);
            }
        }
        std::vector<std::unordered_map<u64, u32>> regionByEdge(triangles);
        for (u32 t = 0; t < triangles; ++t) {
            const TriWork& work = tris_[triBase_[p] + t];
            if (!work.cut) {
                continue;
            }
            for (std::size_t i = 0; i < work.fragments.size(); ++i) {
                const auto& f = work.fragments[i];
                for (u32 k = 0; k < 3; ++k) {
                    regionByEdge[t].emplace(PairKey(f[k], f[(k + 1) % 3]), work.region[i]);
                }
            }
        }
        const auto regionOf = [&](u32 t, u32 x, u32 y) -> u32 {
            const TriWork& work = tris_[triBase_[p] + t];
            if (!work.cut) {
                return 0;
            }
            const auto found = regionByEdge[t].find(PairKey(x, y));
            return found == regionByEdge[t].end() ? kNone : found->second;
        };
        for (const auto& [edgeKey, list] : trianglesOf) {
            if (list.size() < 2) {
                continue;
            }
            const u32 e0 = static_cast<u32>(edgeKey >> 32);
            const u32 e1 = static_cast<u32>(edgeKey & 0xFFFFFFFFu);
            std::vector<u32> along = chain(e0, e1);
            for (u32& x : along) {
                x = points_.root(x);
            }
            along.erase(std::unique(along.begin(), along.end()), along.end());
            // Stretches a cut runs along, by their ends' places on the chain.
            std::vector<u8> cutPiece(along.size(), 0);
            if (const auto found = edges_.find(edgeKey); found != edges_.end()) {
                for (const auto& [sa, sb] : found->second.spans) {
                    const auto ia = std::find(along.begin(), along.end(), points_.root(sa));
                    const auto ib = std::find(along.begin(), along.end(), points_.root(sb));
                    if (ia == along.end() || ib == along.end()) {
                        continue;
                    }
                    std::size_t lo = static_cast<std::size_t>(std::min(ia, ib) - along.begin());
                    std::size_t hi = static_cast<std::size_t>(std::max(ia, ib) - along.begin());
                    for (std::size_t i = lo; i < hi; ++i) {
                        cutPiece[i] = 1;
                    }
                }
            }
            for (std::size_t i = 0; i + 1 < along.size(); ++i) {
                if (cutPiece[i] != 0) {
                    continue;
                }
                const u32 x = along[i];
                const u32 y = along[i + 1];
                const u32 first = list[0];
                const u32 r0 = regionOf(first, x, y);
                if (r0 == kNone) {
                    continue;
                }
                for (std::size_t k = 1; k < list.size(); ++k) {
                    const u32 r1 = regionOf(list[k], x, y);
                    if (r1 == kNone) {
                        continue;
                    }
                    const u32 n0 = nodeBase[first] + r0;
                    const u32 n1 = nodeBase[list[k]] + r1;
                    if (on[n0] == on[n1]) {
                        find.unite(n0, n1);
                    }
                }
            }
        }

        // Each patch read against each other input.
        struct Sample {
            f64 area = 0.0;
            Vector3d centre{0, 0, 0};
        };
        std::unordered_map<u32, std::vector<Sample>> samples;
        std::unordered_map<u32, u32> patchOn;
        for (u32 t = 0; t < triangles; ++t) {
            const TriWork& work = tris_[triBase_[p] + t];
            const auto add = [&](u32 node, const Vector3d& a, const Vector3d& b, const Vector3d& d) {
                const f64 area = 0.5 * Length(Cross(Sub(b, a), Sub(d, a)));
                const u32 root = find.find(node);
                samples[root].push_back(Sample{area, Scale(AddV(AddV(a, b), d), 1.0 / 3.0)});
                patchOn.emplace(root, node);
            };
            if (!work.cut) {
                add(nodeBase[t], part.positions[part.tri[3 * t]], part.positions[part.tri[3 * t + 1]],
                    part.positions[part.tri[3 * t + 2]]);
                continue;
            }
            for (std::size_t i = 0; i < work.fragments.size(); ++i) {
                const auto& f = work.fragments[i];
                add(nodeBase[t] + work.region[i], points_.position[f[0]], points_.position[f[1]],
                    points_.position[f[2]]);
            }
        }
        std::unordered_map<u32, std::pair<u8, u8>> decision; // root -> keep, turn
        std::vector<u32> roots;
        roots.reserve(samples.size());
        for (const auto& entry : samples) {
            roots.push_back(entry.first);
        }
        std::sort(roots.begin(), roots.end());
        bool anyDropped = false;
        bool anyKept = false;
        for (u32 root : roots) {
            std::vector<Sample>& list = samples[root];
            std::sort(list.begin(), list.end(), [](const Sample& l, const Sample& r) { return l.area > r.area; });
            if (list.size() > kReadings) {
                list.resize(kReadings);
            }
            const OnSignature& sig = on[patchOn[root]];
            std::vector<Class> classes(parts_.size(), Class::Outside);
            for (u32 j = 0; j < parts_.size(); ++j) {
                if (j == p) {
                    continue;
                }
                const auto it = std::find_if(sig.begin(), sig.end(), [&](const auto& e) { return e.first == j; });
                if (it != sig.end()) {
                    classes[j] = (it->second & 1) != 0 ? Class::Same : Class::Opposite;
                    continue;
                }
                if (!parts_[j]->holds) {
                    continue;
                }
                f64 sum = 0.0;
                f64 weight = 0.0;
                for (const Sample& s : list) {
                    sum += s.area * fracture::WindingNumber(parts_[j]->winding, s.centre);
                    weight += s.area;
                }
                const f64 reading = weight > 0.0 ? sum / weight
                                                 : fracture::WindingNumber(parts_[j]->winding, list.front().centre);
                classes[j] = reading >= 0.5 ? Class::Inside : Class::Outside;
                if (!isTarget_[p] && isTarget_[j] && reading >= 0.25 && reading < 0.75) {
                    ++result.nearOpening;
                }
            }
            if (!sig.empty()) {
                ++result.coplanar;
            }
            bool turn = false;
            const bool keep = Keeps(options_.op, options_.closeCut, p, isTarget_, classes, turn);
            // A mesh read as wound inward is turned once before anything.
            if (part.winding.sign < 0.0) {
                turn = !turn;
            }
            decision[root] = {keep ? u8{1} : u8{0}, turn ? u8{1} : u8{0}};
            anyDropped = anyDropped || !keep;
            anyKept = anyKept || keep;
        }
        result.patches += static_cast<u32>(roots.size());

        PartOutcome& out = outcomes[p];
        out.wholeKept.assign(triangles, 0);
        out.wholeTurned.assign(triangles, 0);
        out.fragmentKept.resize(triangles);
        out.fragmentTurned.resize(triangles);
        for (u32 t = 0; t < triangles; ++t) {
            const TriWork& work = tris_[triBase_[p] + t];
            if (!work.cut) {
                const auto& d = decision[find.find(nodeBase[t])];
                out.wholeKept[t] = d.first;
                out.wholeTurned[t] = d.second;
                out.keptFaces += d.first;
                continue;
            }
            out.fragmentKept[t].resize(work.fragments.size());
            out.fragmentTurned[t].resize(work.fragments.size());
            for (std::size_t i = 0; i < work.fragments.size(); ++i) {
                const auto& d = decision[find.find(nodeBase[t] + work.region[i])];
                out.fragmentKept[t][i] = d.first;
                out.fragmentTurned[t][i] = d.second;
                out.keptFaces += d.first;
            }
        }
        // What counts as a change: a target that lost something, an operand
        // that gave something. A Union changes whatever it drops.
        if (options_.op == BooleanOp::Union) {
            out.changed = anyDropped;
        } else {
            out.changed = isTarget_[p] ? anyDropped : anyKept;
        }
    }
}

// ============================================================================
// Assembly
// ============================================================================

/// One input's kept faces, as they go into an output mesh.
struct Contribution {
    u32 runPart = 0;
    bool wall = false;   ///< An operand's faces joining a target: its section and skin are the target's.
    bool joined = false; ///< Another input's faces in the first's mesh: its section is the target's (§3.11).
};

struct OutVertex {
    u32 part = 0; ///< Run part.
    u32 vertex = kNone;
    u32 triangle = kNone; ///< A cut point: the triangle it blends from.
    std::array<f64, 3> weight{0, 0, 0};
    Vector3d position{0, 0, 0};
    bool cut = false;
};

struct OutFace {
    u32 part = 0;
    u32 sourceFace = kNone;
    u32 triangle = kNone; ///< None: the source face, whole.
    bool turned = false;
    bool fragment = false;
    u32 section = 0;
    std::vector<u32> vertices;
    std::vector<HalfedgeId> sourceCorner;       ///< Whole: each corner's source halfedge.
    std::vector<std::array<f64, 3>> weights;    ///< Fragment: each corner on its triangle.
    std::vector<std::pair<u32, u32>> edgeFrom;  ///< Per edge, the source vertices of the edge it lies on.
};

bool IsBooleanLayer(const std::string& name) {
    return name.rfind("boolean.", 0) == 0;
}

void CopyElement(const AttrLayer& s, AttrLayer& o, u32 from, u32 to) {
    if (s.type != o.type) {
        return;
    }
    const u32 stride = AttrTypeSize(s.type);
    if ((from + 1) * static_cast<std::size_t>(stride) <= s.data.size() &&
        (to + 1) * static_cast<std::size_t>(stride) <= o.data.size()) {
        std::memcpy(o.data.data() + static_cast<std::size_t>(to) * stride,
                    s.data.data() + static_cast<std::size_t>(from) * stride, stride);
    }
}

/// Vertex layer @p s blended at @p v's weights into @p o at @p to: floats and
/// colours by weight, the rest the heaviest's (the Fracture's rule).
void BlendVertexLayer(const AttrLayer& s, AttrLayer& o, const std::array<u32, 3>& from,
                      const std::array<f64, 3>& weight, u32 to) {
    if (s.type != o.type) {
        return;
    }
    const u32 stride = AttrTypeSize(s.type);
    if (stride == 0 || (to + 1) * static_cast<std::size_t>(stride) > o.data.size()) {
        return;
    }
    u8* target = o.data.data() + static_cast<std::size_t>(to) * stride;
    const auto at = [&](u32 vertex) { return s.data.data() + static_cast<std::size_t>(vertex) * stride; };
    const auto valid = [&](u32 vertex) {
        return vertex != kNone && (vertex + 1) * static_cast<std::size_t>(stride) <= s.data.size();
    };
    const bool floats = s.type == AttrType::F32 || s.type == AttrType::F32x2 || s.type == AttrType::F32x3 ||
                        s.type == AttrType::F32x4;
    if (floats) {
        const u32 components = AttrTypeComponents(s.type);
        for (u32 c = 0; c < components; ++c) {
            f64 sum = 0.0;
            for (u32 i = 0; i < 3; ++i) {
                if (valid(from[i])) {
                    f32 value;
                    std::memcpy(&value, at(from[i]) + 4 * c, 4);
                    sum += weight[i] * value;
                }
            }
            const f32 value = static_cast<f32>(sum);
            std::memcpy(target + 4 * c, &value, 4);
        }
        return;
    }
    if (s.type == AttrType::U8x4) {
        for (u32 c = 0; c < 4; ++c) {
            f64 sum = 0.0;
            for (u32 i = 0; i < 3; ++i) {
                if (valid(from[i])) {
                    sum += weight[i] * at(from[i])[c];
                }
            }
            target[c] = static_cast<u8>(std::clamp(std::lround(sum), 0l, 255l));
        }
        return;
    }
    u32 heaviest = 0;
    for (u32 i = 1; i < 3; ++i) {
        if (weight[i] > weight[heaviest]) {
            heaviest = i;
        }
    }
    if (valid(from[heaviest])) {
        std::memcpy(target, at(from[heaviest]), stride);
    }
}

class Assembler {
public:
    Assembler(Run& run, const BooleanOptions& options) : run_(run), options_(options) {}

    /// The faces of @p contributions as one mesh: the first's name, sections and
    /// Mesh layers. @p wallSection is the section a wall's faces take.
    Mesh build(std::span<const Contribution> contributions, u32 wallSection);

private:
    u32 sourceVertex(u32 part, u32 vertex);
    u32 cutPoint(u32 point, u32 part, u32 triangle);

    Run& run_;
    const BooleanOptions& options_;
    std::vector<OutVertex> vertices_;
    std::vector<OutFace> faces_;
    std::vector<std::unordered_map<u32, u32>> sourceMap_;
    std::unordered_map<u32, u32> pointMap_;
};

u32 Assembler::sourceVertex(u32 part, u32 vertex) {
    auto& map = sourceMap_[part];
    if (const auto found = map.find(vertex); found != map.end()) {
        return found->second;
    }
    // Where another input already put a vertex on this point, the two meet
    // there: the cut is one vertex. An input's own coincident vertices stay
    // apart, as it had them.
    const u32 point = run_.points_.root(run_.pointOf_[part][vertex]);
    if (const auto at = pointMap_.find(point); at != pointMap_.end() && vertices_[at->second].part != part) {
        map.emplace(vertex, at->second);
        return at->second;
    }
    const u32 id = static_cast<u32>(vertices_.size());
    OutVertex out;
    out.part = part;
    out.vertex = vertex;
    out.position = run_.parts_[part]->positions[vertex];
    vertices_.push_back(out);
    map.emplace(vertex, id);
    pointMap_.emplace(point, id);
    return id;
}

u32 Assembler::cutPoint(u32 point, u32 part, u32 triangle) {
    const auto [it, fresh] = pointMap_.emplace(point, static_cast<u32>(vertices_.size()));
    if (fresh) {
        const Part& p = *run_.parts_[part];
        OutVertex out;
        out.part = part;
        out.triangle = triangle;
        out.position = run_.points_.position[point];
        out.weight = WeightsOn(p.positions[p.tri[3 * triangle]], p.positions[p.tri[3 * triangle + 1]],
                               p.positions[p.tri[3 * triangle + 2]], out.position);
        out.cut = true;
        vertices_.push_back(out);
    }
    return it->second;
}

Mesh Assembler::build(std::span<const Contribution> contributions, u32 wallSection) {
    vertices_.clear();
    faces_.clear();
    pointMap_.clear();
    sourceMap_.assign(run_.parts_.size(), {});
    const Part& primary = *run_.parts_[contributions.front().runPart];

    std::unordered_set<u32> wallParts;
    for (const Contribution& c : contributions) {
        if (c.wall) {
            wallParts.insert(c.runPart);
        }
        const u32 p = c.runPart;
        const Part& part = *run_.parts_[p];
        const PartOutcome& outcome = run_.outcomes[p];
        const auto sections = std::as_const(part.mesh).faceSections();
        const u32 faceCount = static_cast<u32>(part.loopOffset.size() - 1);
        // Triangles per face, in order.
        std::vector<u32> first(faceCount + 1, 0);
        for (u32 f : part.triFace) {
            ++first[f + 1];
        }
        for (u32 f = 0; f < faceCount; ++f) {
            first[f + 1] += first[f];
        }
        for (u32 f = 0; f < faceCount; ++f) {
            const u32 section = c.wall || c.joined ? wallSection : (f < sections.size() ? sections[f] : 0);
            // A face no cut reached, kept whole and turned alike, is written whole.
            bool whole = first[f + 1] > first[f];
            bool kept = true;
            bool turned = false;
            for (u32 t = first[f]; t < first[f + 1] && whole; ++t) {
                const u32 at = run_.triBase_[p] + t;
                const TriWork& work = run_.tris_[at];
                bool tk = outcome.wholeKept[t] != 0;
                bool tt = outcome.wholeTurned[t] != 0;
                if (work.cut) {
                    // A cut that only ran along the triangle's edges left it one
                    // fragment, itself: it is whole (BOO-16).
                    if (!run_.alongOnly(at)) {
                        whole = false;
                        break;
                    }
                    tk = outcome.fragmentKept[t][0] != 0;
                    tt = outcome.fragmentTurned[t][0] != 0;
                }
                if (t == first[f]) {
                    kept = tk;
                    turned = tt;
                } else if (tk != kept || tt != turned) {
                    whole = false;
                }
            }
            if (whole) {
                if (!kept) {
                    continue;
                }
                OutFace out;
                out.part = p;
                out.sourceFace = f;
                out.turned = turned;
                out.section = section;
                for (u32 k = part.loopOffset[f]; k < part.loopOffset[f + 1]; ++k) {
                    out.vertices.push_back(sourceVertex(p, part.loopVertex[k]));
                    out.sourceCorner.push_back(part.loopHalfedge[k]);
                }
                const u32 valence = static_cast<u32>(out.vertices.size());
                for (u32 k = 0; k < valence; ++k) {
                    out.edgeFrom.emplace_back(part.loopVertex[part.loopOffset[f] + k],
                                              part.loopVertex[part.loopOffset[f] + (k + 1) % valence]);
                }
                faces_.push_back(std::move(out));
                continue;
            }
            for (u32 t = first[f]; t < first[f + 1]; ++t) {
                const u32 face = run_.triBase_[p] + t;
                const TriWork& work = run_.tris_[face];
                const std::array<u32, 3> sv{part.tri[3 * t], part.tri[3 * t + 1], part.tri[3 * t + 2]};
                if (!work.cut) {
                    if (outcome.wholeKept[t] == 0) {
                        continue;
                    }
                    OutFace out;
                    out.part = p;
                    out.sourceFace = f;
                    out.triangle = t;
                    out.fragment = false;
                    out.turned = outcome.wholeTurned[t] != 0;
                    out.section = section;
                    for (u32 k = 0; k < 3; ++k) {
                        out.vertices.push_back(sourceVertex(p, sv[k]));
                        out.sourceCorner.push_back(part.triCorner[3 * t + k]);
                        out.edgeFrom.emplace_back(sv[k], sv[(k + 1) % 3]);
                    }
                    faces_.push_back(std::move(out));
                    continue;
                }
                // Which of the triangle's corners and edges each point is on.
                std::array<u32, 3> cornerPoint{};
                for (u32 k = 0; k < 3; ++k) {
                    cornerPoint[k] = run_.points_.root(run_.pointOf_[p][sv[k]]);
                }
                std::array<std::unordered_set<u32>, 3> onEdge;
                for (u32 k = 0; k < 3; ++k) {
                    const std::vector<u32> along =
                        run_.chain(run_.pointOf_[p][sv[k]], run_.pointOf_[p][sv[(k + 1) % 3]]);
                    for (u32 x : along) {
                        onEdge[k].insert(run_.points_.root(x));
                    }
                }
                for (std::size_t i = 0; i < work.fragments.size(); ++i) {
                    if (outcome.fragmentKept[t][i] == 0) {
                        continue;
                    }
                    const auto& frag = work.fragments[i];
                    OutFace out;
                    out.part = p;
                    out.sourceFace = f;
                    out.triangle = t;
                    out.fragment = true;
                    out.turned = outcome.fragmentTurned[t][i] != 0;
                    out.section = section;
                    for (u32 k = 0; k < 3; ++k) {
                        const u32 x = frag[k];
                        u32 corner = kNone;
                        for (u32 j = 0; j < 3; ++j) {
                            if (cornerPoint[j] == x) {
                                corner = j;
                            }
                        }
                        std::array<f64, 3> w{0, 0, 0};
                        if (corner != kNone) {
                            out.vertices.push_back(sourceVertex(p, sv[corner]));
                            w[corner] = 1.0;
                        } else {
                            out.vertices.push_back(cutPoint(x, p, t));
                            w = WeightsOn(part.positions[sv[0]], part.positions[sv[1]], part.positions[sv[2]],
                                          run_.points_.position[x]);
                        }
                        out.weights.push_back(w);
                    }
                    for (u32 k = 0; k < 3; ++k) {
                        const u32 x = frag[k];
                        const u32 y = frag[(k + 1) % 3];
                        std::pair<u32, u32> from{kNone, kNone};
                        for (u32 e = 0; e < 3; ++e) {
                            if (onEdge[e].count(x) != 0 && onEdge[e].count(y) != 0) {
                                from = {sv[e], sv[(e + 1) % 3]};
                            }
                        }
                        out.edgeFrom.push_back(from);
                    }
                    faces_.push_back(std::move(out));
                }
            }
        }
    }

    // A turned face runs its corners the other way, taking their values along.
    for (OutFace& f : faces_) {
        if (!f.turned) {
            continue;
        }
        std::reverse(f.vertices.begin(), f.vertices.end());
        std::reverse(f.sourceCorner.begin(), f.sourceCorner.end());
        std::reverse(f.weights.begin(), f.weights.end());
        // Edge k ran from corner k to k + 1; reversed, edge k runs between the
        // corners that were n-1-k and n-2-k.
        const std::size_t n = f.edgeFrom.size();
        std::vector<std::pair<u32, u32>> edges(n);
        for (std::size_t k = 0; k < n; ++k) {
            edges[k] = f.edgeFrom[(2 * n - 2 - k) % n];
        }
        f.edgeFrom = std::move(edges);
    }

    FaceSet set;
    set.vertexCount = static_cast<u32>(vertices_.size());
    for (const OutFace& f : faces_) {
        set.addFace(f.vertices);
    }
    RepairResult repaired = Repair(set);
    for (const VertexSplit& split : repaired.log.splits) {
        if (split.created >= vertices_.size()) {
            vertices_.resize(split.created + 1);
        }
        vertices_[split.created] = vertices_[split.original];
    }
    std::vector<u32> survivor;
    {
        std::size_t dropped = 0;
        for (u32 f = 0; f < faces_.size(); ++f) {
            if (dropped < repaired.log.droppedFaces.size() && repaired.log.droppedFaces[dropped].index == f) {
                ++dropped;
                continue;
            }
            survivor.push_back(f);
        }
    }

    Mesh mesh;
    mesh.name = primary.mesh.name;
    mesh.lodLevel = primary.mesh.lodLevel;
    mesh.sections = primary.mesh.sections;
    mesh.sc2Bounds = primary.mesh.sc2Bounds;
    // As `rebuild` keeps it: the import's log, unless this face set needed its own repair.
    mesh.repairLog = repaired.log.empty() ? primary.mesh.repairLog : repaired.log;
    mesh.setFaceSet(repaired.faces);
    if (!mesh.ensureConnectivity().ok()) {
        return Mesh{};
    }
    const u32 vertexCount = static_cast<u32>(vertices_.size());
    // Every contribution's layers, the primary's first.
    for (const Contribution& c : contributions) {
        for (const AttrLayer& layer : run_.parts_[c.runPart]->mesh.attributes.layers()) {
            if (IsBooleanLayer(layer.name) || mesh.attributes.layer(layer.name, layer.domain) != nullptr) {
                continue;
            }
            if (layer.domain == Domain::Mesh && c.runPart != contributions.front().runPart) {
                continue;
            }
            mesh.attributes.create(layer.name, layer.domain, layer.type, layer.storage);
        }
    }
    for (const AttrLayer& layer : primary.mesh.attributes.layers()) {
        if (layer.domain == Domain::Mesh) {
            AttrLayer* out = mesh.attributes.layer(layer.name, Domain::Mesh);
            if (out != nullptr && out->data.size() == layer.data.size()) {
                out->data = layer.data;
            }
        }
    }

    // --- vertices ---
    {
        auto position =
            mesh.attributes.getOrCreate<Vector3f>(names::kPosition, Domain::Vertex, AttrType::F32x3);
        for (u32 v = 0; v < vertexCount; ++v) {
            const Vector3d& p = vertices_[v].position;
            position[v] = Vector3f(static_cast<f32>(p.x), static_cast<f32>(p.y), static_cast<f32>(p.z));
        }
    }
    for (u32 v = 0; v < vertexCount; ++v) {
        const OutVertex& o = vertices_[v];
        const Part& part = *run_.parts_[o.part];
        for (const AttrLayer& layer : part.mesh.attributes.layers()) {
            if (layer.domain != Domain::Vertex || layer.name == names::kPosition ||
                layer.name == names::kMergeGroup || IsBooleanLayer(layer.name)) {
                continue;
            }
            AttrLayer* out = mesh.attributes.layer(layer.name, Domain::Vertex);
            if (out == nullptr) {
                continue;
            }
            if (!o.cut) {
                CopyElement(layer, *out, o.vertex, v);
            } else {
                const std::array<u32, 3> from{part.tri[3 * o.triangle], part.tri[3 * o.triangle + 1],
                                              part.tri[3 * o.triangle + 2]};
                BlendVertexLayer(layer, *out, from, o.weight, v);
            }
        }
    }
    // A reference layer's values name source vertices: renumbered as the vertices
    // were, and a vertex the cut dropped named by none (BOO-6).
    for (const AttrLayer& layer : std::as_const(mesh.attributes).layers()) {
        if (!IsVertexReferenceLayer(layer)) {
            continue;
        }
        const auto lanes = mesh.attributes.get<std::array<u32, 4>>(layer.name, Domain::Vertex);
        for (u32 v = 0; v < vertexCount && v < lanes.size(); ++v) {
            const auto& map = sourceMap_[vertices_[v].part];
            for (u32& named : lanes[v]) {
                const auto found = map.find(named);
                named = found != map.end() ? found->second : kInvalidId;
            }
        }
    }
    {
        // Merge groups: each contribution's own apart from another's, a cut
        // point a fresh one.
        auto groups = MergeGroupsOf(mesh);
        std::vector<u32> base(run_.parts_.size(), 0);
        u32 next = 0;
        for (const Contribution& c : contributions) {
            base[c.runPart] = next;
            u32 highest = 0;
            for (u32 g : run_.parts_[c.runPart]->merge) {
                highest = std::max(highest, g + 1);
            }
            next += highest;
        }
        for (u32 v = 0; v < vertexCount; ++v) {
            const OutVertex& o = vertices_[v];
            groups[v] = o.cut ? next++ : base[o.part] + run_.parts_[o.part]->merge[o.vertex];
        }
    }
    // The skin: a wall's from the target's surface (D6), else blended.
    bool skinned = false;
    std::vector<std::vector<Influence>> skin(vertexCount);
    const Part& target = primary;
    for (u32 v = 0; v < vertexCount; ++v) {
        const OutVertex& o = vertices_[v];
        const Part& part = *run_.parts_[o.part];
        if (wallParts.count(o.part) != 0 && !target.tri.empty()) {
            const Vector3f at(static_cast<f32>(o.position.x), static_cast<f32>(o.position.y),
                              static_cast<f32>(o.position.z));
            const PointHit hit = target.bvh.closestPoint(at);
            if (hit.hit()) {
                const u32 t = hit.triangle;
                const std::vector<Influence> lists[3] = {target.skin[target.tri[3 * t]],
                                                         target.skin[target.tri[3 * t + 1]],
                                                         target.skin[target.tri[3 * t + 2]]};
                const f32 w[3] = {1.0f - hit.u - hit.v, hit.u, hit.v};
                skin[v] = BlendInfluences(lists, w);
                skinned = skinned || !skin[v].empty();
                continue;
            }
        }
        if (!o.cut) {
            skin[v] = part.skin[o.vertex];
        } else {
            const std::vector<Influence> lists[3] = {part.skin[part.tri[3 * o.triangle]],
                                                     part.skin[part.tri[3 * o.triangle + 1]],
                                                     part.skin[part.tri[3 * o.triangle + 2]]};
            const f32 w[3] = {static_cast<f32>(o.weight[0]), static_cast<f32>(o.weight[1]),
                              static_cast<f32>(o.weight[2])};
            skin[v] = BlendInfluences(lists, w);
        }
        skinned = skinned || !skin[v].empty();
    }
    // An input with no skin joined to skinned ones takes the nearest skinned
    // input's surface, as a wall does, so no vertex is left unbound (P6, BOO-15).
    if (skinned && contributions.size() > 1) {
        std::vector<u8> bound(run_.parts_.size(), 0);
        for (const Contribution& c : contributions) {
            for (const auto& list : run_.parts_[c.runPart]->skin) {
                if (!list.empty()) {
                    bound[c.runPart] = 1;
                    break;
                }
            }
        }
        for (u32 v = 0; v < vertexCount; ++v) {
            if (!skin[v].empty() || bound[vertices_[v].part] != 0) {
                continue;
            }
            const Vector3d& p = vertices_[v].position;
            const Vector3f at(static_cast<f32>(p.x), static_cast<f32>(p.y), static_cast<f32>(p.z));
            f32 nearest = std::numeric_limits<f32>::max();
            for (const Contribution& c : contributions) {
                const Part& from = *run_.parts_[c.runPart];
                if (bound[c.runPart] == 0 || from.tri.empty()) {
                    continue;
                }
                const PointHit hit = from.bvh.closestPoint(at);
                if (!hit.hit() || hit.distance >= nearest) {
                    continue;
                }
                nearest = hit.distance;
                const u32 t = hit.triangle;
                const std::vector<Influence> lists[3] = {from.skin[from.tri[3 * t]], from.skin[from.tri[3 * t + 1]],
                                                         from.skin[from.tri[3 * t + 2]]};
                const f32 w[3] = {1.0f - hit.u - hit.v, hit.u, hit.v};
                skin[v] = BlendInfluences(lists, w);
            }
        }
    }
    if (skinned) {
        mesh.skin.reset(0);
        mesh.skin.offsets.assign(1, 0);
        for (const auto& list : skin) {
            mesh.skin.appendVertex(list);
        }
        mesh.skin.sortByWeight();
        // Spelled out per vertex: a section's rigid node would bind faces from
        // another input to it.
        if (contributions.size() > 1) {
            for (MeshSection& s : mesh.sections) {
                s.rigidNode.reset();
            }
        }
    }

    // --- faces and corners ---
    const Topology& topology = std::as_const(mesh).topology();
    auto sectionsOut = mesh.faceSections();
    auto normal = mesh.attributes.get<Vector3f>(names::kNormal, Domain::Halfedge);
    auto tangent = mesh.attributes.get<Vector4f>(names::kTangent, Domain::Halfedge);
    std::unordered_map<u64, SourcePolygon> captured;
    FaceTriangulationBuilder rows(static_cast<u32>(survivor.size()));
    bool anyRow = false;
    std::vector<u32> outRow;
    std::vector<u32> touched;
    // Colour layers another input brings: a face without one is white, not black
    // and transparent (BOO-5).
    std::vector<std::string> colours;
    for (const AttrLayer& layer : std::as_const(mesh.attributes).layers()) {
        if (layer.domain == Domain::Halfedge && layer.type == AttrType::U8x4 && layer.name.rfind("color", 0) == 0) {
            colours.push_back(layer.name);
        }
    }
    for (u32 outFace = 0; outFace < survivor.size(); ++outFace) {
        const OutFace& f = faces_[survivor[outFace]];
        const Part& part = *run_.parts_[f.part];
        const Topology& sourceTopology = std::as_const(part.mesh).topology();
        sectionsOut[outFace] = f.section;
        for (const std::string& name : colours) {
            if (part.mesh.attributes.layer(name, Domain::Halfedge) != nullptr) {
                continue;
            }
            const auto white = mesh.attributes.get<std::array<u8, 4>>(name, Domain::Halfedge);
            for (const HalfedgeId c : topology.fh(FaceId(outFace))) {
                if (c.index() < white.size()) {
                    white[c.index()] = {255, 255, 255, 255};
                }
            }
        }
        for (const AttrLayer& layer : part.mesh.attributes.layers()) {
            if (layer.domain != Domain::Face || layer.name == names::kSection || IsBooleanLayer(layer.name)) {
                continue;
            }
            if (AttrLayer* out = mesh.attributes.layer(layer.name, Domain::Face)) {
                CopyElement(layer, *out, f.sourceFace, outFace);
            }
        }
        HalfedgeId h = topology.halfedge(FaceId(outFace));
        const std::size_t valence = f.vertices.size();
        for (std::size_t k = 0; k < valence; ++k) {
            if (!f.fragment) {
                for (const AttrLayer& layer : part.mesh.attributes.layers()) {
                    if (layer.domain != Domain::Halfedge || IsBooleanLayer(layer.name)) {
                        continue;
                    }
                    if (AttrLayer* out = mesh.attributes.layer(layer.name, Domain::Halfedge)) {
                        CopyElement(layer, *out, f.sourceCorner[k].value(), h.value());
                    }
                }
            } else {
                u32 exact = kNone;
                for (u32 j = 0; j < 3; ++j) {
                    if (f.weights[k][j] == 1.0) {
                        exact = j;
                    }
                }
                if (exact != kNone) {
                    for (const AttrLayer& layer : part.mesh.attributes.layers()) {
                        if (layer.domain != Domain::Halfedge || IsBooleanLayer(layer.name)) {
                            continue;
                        }
                        if (AttrLayer* out = mesh.attributes.layer(layer.name, Domain::Halfedge)) {
                            CopyElement(layer, *out, part.triCorner[3 * f.triangle + exact].value(), h.value());
                        }
                    }
                } else {
                    const u64 key = (static_cast<u64>(f.part) << 32) | f.triangle;
                    auto found = captured.find(key);
                    if (found == captured.end()) {
                        const HalfedgeId corners[3] = {part.triCorner[3 * f.triangle],
                                                       part.triCorner[3 * f.triangle + 1],
                                                       part.triCorner[3 * f.triangle + 2]};
                        found = captured.emplace(key, CaptureCorners(part.mesh, corners)).first;
                    }
                    const f32 w[3] = {static_cast<f32>(f.weights[k][0]), static_cast<f32>(f.weights[k][1]),
                                      static_cast<f32>(f.weights[k][2])};
                    const HalfedgeId target[1] = {h};
                    BlendCorners(mesh, found->second, w, target);
                }
            }
            if (f.turned) {
                if (h.index() < normal.size()) {
                    normal[h.index()] = Vector3f(-normal[h.index()].x, -normal[h.index()].y, -normal[h.index()].z);
                }
                if (h.index() < tangent.size()) {
                    tangent[h.index()].w = -tangent[h.index()].w;
                }
            }
            const auto [u, v] = f.edgeFrom[k];
            const u32 oe = Topology::edge(h).value();
            if (u != kNone) {
                const HalfedgeId sh = sourceTopology.findHalfedge(VertexId(u), VertexId(v));
                if (sh.valid()) {
                    const u32 se = Topology::edge(sh).value();
                    for (const AttrLayer& layer : part.mesh.attributes.layers()) {
                        if (layer.domain != Domain::Edge || IsBooleanLayer(layer.name)) {
                            continue;
                        }
                        if (AttrLayer* out = mesh.attributes.layer(layer.name, Domain::Edge)) {
                            CopyElement(layer, *out, se, oe);
                        }
                    }
                }
            }
            if (f.fragment) {
                touched.push_back(oe);
            }
            h = topology.next(h);
        }
        // A whole polygon keeps the triangles it was drawn as.
        if (!f.fragment && f.triangle == kNone && valence > 3) {
            const auto row = part.mesh.triangulation.row(f.sourceFace);
            if (!row.empty()) {
                outRow.clear();
                std::vector<u32> loop(f.vertices);
                std::vector<u32> sourceLoop(part.loopVertex.begin() + part.loopOffset[f.sourceFace],
                                            part.loopVertex.begin() + part.loopOffset[f.sourceFace + 1]);
                if (f.turned) {
                    std::reverse(sourceLoop.begin(), sourceLoop.end());
                }
                for (u32 sv : row) {
                    for (std::size_t k = 0; k < valence; ++k) {
                        if (sourceLoop[k] == sv) {
                            outRow.push_back(loop[k]);
                            break;
                        }
                    }
                }
                if (outRow.size() == row.size()) {
                    if (f.turned) {
                        for (std::size_t i = 0; i + 2 < outRow.size(); i += 3) {
                            std::swap(outRow[i + 1], outRow[i + 2]);
                        }
                    }
                    rows.set(outFace, outRow);
                    anyRow = true;
                }
            }
        }
    }
    if (anyRow) {
        mesh.triangulation = rows.build();
    }
    // Faces that came from another input than the primary's are not changed by
    // the same `FinishTool` rows, but the crease rule runs along the cut.
    ModelPlan plan;
    std::sort(touched.begin(), touched.end());
    touched.erase(std::unique(touched.begin(), touched.end()), touched.end());
    plan.touchedEdges = std::move(touched);
    FinishTool(mesh, plan, options_.creaseAngle);
    return mesh;
}

/// The section of the target its walls take: the one most of the target's cut
/// faces are in (§3.11).
u32 WallSection(const Run& run, u32 targetPart) {
    const Part& part = *run.parts_[targetPart];
    const auto sections = std::as_const(part.mesh).faceSections();
    std::unordered_map<u32, u32> count;
    for (u32 t = 0; t < part.tri.size() / 3; ++t) {
        if (run.tris_[run.triBase_[targetPart] + t].cut) {
            const u32 f = part.triFace[t];
            ++count[f < sections.size() ? sections[f] : 0];
        }
    }
    u32 best = 0;
    u32 most = 0;
    for (const auto& [s, n] : count) {
        if (n > most || (n == most && s < best)) {
            best = s;
            most = n;
        }
    }
    return best;
}

u32 FaceCount(const Mesh& mesh) {
    return static_cast<u32>(mesh.faceSet().faceCount());
}

Mesh EmptyLike(const Mesh& source) {
    Mesh out;
    out.name = source.name;
    out.lodLevel = source.lodLevel;
    out.sections = source.sections;
    out.setFaceSet(FaceSet{});
    for (const AttrLayer& layer : source.attributes.layers()) {
        if (layer.domain == Domain::Mesh) {
            out.attributes.create(layer.name, layer.domain, layer.type, layer.storage);
            if (AttrLayer* o = out.attributes.layer(layer.name, Domain::Mesh)) {
                o->data = layer.data;
            }
        }
    }
    return out;
}

} // namespace

BooleanResult BooleanMeshes(std::span<const BooleanInput> inputs, const BooleanOptions& options) {
    BooleanResult result;
    const u32 n = static_cast<u32>(inputs.size());
    result.meshes.resize(n);
    result.consumed.assign(n, 0);
    result.holds.assign(n, 0);
    std::vector<u32> targets;
    std::vector<u32> operands;
    for (u32 i = 0; i < n; ++i) {
        if (inputs[i].mesh == nullptr) {
            continue;
        }
        (inputs[i].operand ? operands : targets).push_back(i);
    }
    if (targets.empty()) {
        result.refusal = BooleanRefusal::NoTarget;
        return result;
    }
    if (operands.empty()) {
        result.refusal = BooleanRefusal::NoOperand;
        return result;
    }

    std::vector<Part> parts(n);
    Extent joint;
    ResetExtent(joint);
    for (u32 i = 0; i < n; ++i) {
        if (inputs[i].mesh == nullptr) {
            continue;
        }
        parts[i].operand = inputs[i].operand;
        Prepare(parts[i], *inputs[i].mesh);
        ReadVolume(parts[i]);
        result.holds[i] = parts[i].holds ? 1 : 0;
        if (parts[i].usable) {
            GrowExtent(joint, parts[i].box.minimum);
            GrowExtent(joint, parts[i].box.maximum);
        }
    }
    if (options.op != BooleanOp::Union) {
        for (u32 o : operands) {
            if (parts[o].usable && !parts[o].holds) {
                result.refusal = BooleanRefusal::OperandHoldsNothing;
                result.refusedInput = o;
                return result;
            }
        }
    }
    const Vector3d low(joint.minimum.x, joint.minimum.y, joint.minimum.z);
    const f64 side = std::max({static_cast<f64>(joint.maximum.x) - joint.minimum.x,
                               static_cast<f64>(joint.maximum.y) - joint.minimum.y,
                               static_cast<f64>(joint.maximum.z) - joint.minimum.z});
    const f64 step = side > 0.0 ? side / kGrid : 1.0;

    const auto usable = [&](u32 i) { return parts[i].usable; };

    if (options.op == BooleanOp::Union) {
        std::vector<u32> members;
        for (u32 i : targets) {
            if (usable(i)) {
                members.push_back(i);
            }
        }
        for (u32 i : operands) {
            if (usable(i)) {
                members.push_back(i);
            }
        }
        if (members.empty()) {
            // Every input is empty: there is no face for the union to keep.
            result.refusal = BooleanRefusal::NothingLeft;
            result.refusedInput = targets.front();
            return result;
        }
        std::vector<Part*> runParts;
        std::vector<u8> isTarget;
        for (u32 i : members) {
            runParts.push_back(&parts[i]);
            isTarget.push_back(inputs[i].operand ? 0 : 1);
        }
        Run run(runParts, isTarget, options, low, step);
        run.cut(result);
        if (result.failedFaces > 0) {
            result.refusal = BooleanRefusal::CutFailed;
            return result;
        }
        run.classify(result);
        Assembler assembler(run, options);
        if (options.faces == OperandFaces::JoinTarget) {
            std::vector<Contribution> all;
            for (u32 r = 0; r < members.size(); ++r) {
                all.push_back(Contribution{r, false, r != 0});
            }
            result.meshes[members.front()] = assembler.build(all, WallSection(run, 0));
            for (u32 i = 0; i < n; ++i) {
                if (i != members.front() && inputs[i].mesh != nullptr) {
                    result.meshes[i] = EmptyLike(*inputs[i].mesh);
                    result.consumed[i] = 1;
                }
            }
        } else {
            for (u32 r = 0; r < members.size(); ++r) {
                const Contribution own[1] = {Contribution{r, false}};
                result.meshes[members[r]] = assembler.build(own, 0);
                if (FaceCount(result.meshes[members[r]]) == 0) {
                    result.consumed[members[r]] = 1;
                }
            }
        }
        for (u32 i = 0; i < n; ++i) {
            if (inputs[i].mesh != nullptr && !usable(i) && !result.consumed[i]) {
                result.meshes[i] = *inputs[i].mesh;
            }
        }
        return result;
    }

    // Subtract and Intersect: each target on its own, against every operand.
    std::vector<std::vector<Mesh>> walls(n);
    bool anyChange = false;
    for (u32 t : targets) {
        if (!usable(t)) {
            result.meshes[t] = *inputs[t].mesh;
            continue;
        }
        std::vector<Part*> runParts{&parts[t]};
        std::vector<u8> isTarget{1};
        std::vector<u32> runOperand;
        for (u32 o : operands) {
            if (usable(o)) {
                runParts.push_back(&parts[o]);
                isTarget.push_back(0);
                runOperand.push_back(o);
            }
        }
        Run run(runParts, isTarget, options, low, step);
        run.cut(result);
        if (result.failedFaces > 0) {
            result.refusal = BooleanRefusal::CutFailed;
            result.refusedInput = t;
            return result;
        }
        run.classify(result);
        for (const PartOutcome& outcome : run.outcomes) {
            anyChange = anyChange || outcome.changed;
        }
        Assembler assembler(run, options);
        const u32 wallSection = WallSection(run, 0);
        if (options.faces == OperandFaces::JoinTarget) {
            std::vector<Contribution> all{Contribution{0, false}};
            for (u32 r = 1; r < runParts.size(); ++r) {
                if (run.outcomes[r].keptFaces > 0) {
                    all.push_back(Contribution{r, true});
                }
            }
            result.meshes[t] = assembler.build(all, wallSection);
        } else {
            const Contribution own[1] = {Contribution{0, false}};
            result.meshes[t] = assembler.build(own, 0);
            bool walled = false;
            for (u32 r = 1; r < runParts.size(); ++r) {
                if (run.outcomes[r].keptFaces == 0) {
                    continue;
                }
                const Contribution wall[1] = {Contribution{r, false}};
                walls[runOperand[r - 1]].push_back(assembler.build(wall, 0));
                walled = walled || FaceCount(walls[runOperand[r - 1]].back()) != 0;
            }
            // The whole result in an operand's own mesh (an Intersect with the
            // operand inside): the target is used up, not refused (BOO-1).
            if (walled && FaceCount(result.meshes[t]) == 0) {
                result.meshes[t] = EmptyLike(*inputs[t].mesh);
                result.consumed[t] = 1;
                continue;
            }
        }
        if (FaceCount(result.meshes[t]) == 0) {
            result.refusal = BooleanRefusal::NothingLeft;
            result.refusedInput = t;
            return result;
        }
    }
    if (options.op == BooleanOp::Subtract && !anyChange) {
        result.refusal = BooleanRefusal::NothingToSubtract;
        result.refusedInput = operands.front();
        return result;
    }
    for (u32 o : operands) {
        if (inputs[o].mesh == nullptr) {
            continue;
        }
        if (options.faces == OperandFaces::OwnMesh && !walls[o].empty()) {
            if (walls[o].size() == 1) {
                result.meshes[o] = std::move(walls[o].front());
            } else {
                // One wall per target, each with the operand's sections: one
                // mesh with the operand's name, level and sections (BOO-7).
                Mesh merged = MergeMeshes(walls[o]);
                const Mesh& first = walls[o].front();
                merged.name = first.name;
                merged.lodLevel = first.lodLevel;
                merged.sc2Bounds = first.sc2Bounds;
                const u32 own = static_cast<u32>(first.sections.size());
                if (own > 0 && merged.sections.size() > own) {
                    for (u32& s : merged.faceSections()) {
                        s %= own;
                    }
                    merged.sections.erase(merged.sections.begin() + own, merged.sections.end());
                }
                result.meshes[o] = std::move(merged);
            }
        } else {
            result.meshes[o] = EmptyLike(*inputs[o].mesh);
            result.consumed[o] = 1;
        }
    }
    return result;
}

} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
