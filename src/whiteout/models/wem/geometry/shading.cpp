// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/geometry/shading.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>
#include <tuple>
#include <unordered_map>
#include <utility>

#include <whiteout/models/wem/geometry/attributes.h>
#include <whiteout/models/wem/geometry/modelling.h>
#include <whiteout/models/wem/geometry/ops.h>
#include <whiteout/models/wem/skinning/points.h>

namespace whiteout {
namespace models {
namespace wem {
namespace geom {

namespace {

Vector3f sub(const Vector3f& a, const Vector3f& b) {
    return Vector3f{a.x - b.x, a.y - b.y, a.z - b.z};
}

Vector3f add(const Vector3f& a, const Vector3f& b) {
    return Vector3f{a.x + b.x, a.y + b.y, a.z + b.z};
}

Vector3f scaled(const Vector3f& a, f32 s) {
    return Vector3f{a.x * s, a.y * s, a.z * s};
}

f32 dot(const Vector3f& a, const Vector3f& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vector3f crossOf(const Vector3f& a, const Vector3f& b) {
    return Vector3f{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

f32 lengthOf(const Vector3f& a) {
    return std::sqrt(dot(a, a));
}

/// @p v at unit length, as `RecomputeNormals` leaves a sum: +z when it has none.
Vector3f unitOr(const Vector3f& v) {
    const f32 length = lengthOf(v);
    if (length <= 1e-20f) {
        return Vector3f{0.0f, 0.0f, 1.0f};
    }
    return Vector3f{v.x / length, v.y / length, v.z / length};
}

/// The angle between two directions, radians; 0 when either has no length.
f32 angleBetween(const Vector3f& a, const Vector3f& b) {
    const f32 la = lengthOf(a);
    const f32 lb = lengthOf(b);
    if (la <= 1e-20f || lb <= 1e-20f) {
        return 0.0f;
    }
    return std::acos(std::clamp(dot(a, b) / (la * lb), -1.0f, 1.0f));
}

std::span<const Vector3f> positionsOf(const Mesh& mesh) {
    return mesh.attributes.get<const Vector3f>(names::kPosition, Domain::Vertex);
}

/// Face @p f's Newell vector: twice its area along its normal.
Vector3f newell(const Topology& topology, std::span<const Vector3f> positions, FaceId f) {
    Vector3f sum{0.0f, 0.0f, 0.0f};
    for (const HalfedgeId h : topology.fh(f)) {
        const std::size_t a = topology.from(h).index();
        const std::size_t b = topology.to(h).index();
        if (a < positions.size() && b < positions.size()) {
            const Vector3f& pa = positions[a];
            const Vector3f& pb = positions[b];
            sum.x += (pa.y - pb.y) * (pa.z + pb.z);
            sum.y += (pa.z - pb.z) * (pa.x + pb.x);
            sum.z += (pa.x - pb.x) * (pa.y + pb.y);
        }
    }
    return sum;
}

/// The halfedge of @p e that has a face: its first, unless only its second has.
HalfedgeId faceSide(const Topology& topology, u32 e) {
    const HalfedgeId h = Topology::halfedge(EdgeId(e), 0);
    return topology.face(h).valid() ? h : Topology::opposite(h);
}

bool liveEdge(const Topology& topology, u32 e) {
    return e < topology.edgeCount() && !topology.isDeleted(EdgeId(e));
}

bool flagged(const Mesh& mesh, const char* layer, u32 e) {
    const std::span<const u8> flags = mesh.attributes.get<const u8>(layer, Domain::Edge);
    return e < flags.size() && flags[e] != 0;
}

/// A mesh's two shading layers, fetched once for a run of writes.
struct Shaded {
    std::span<Vector3f> normals;
    std::span<u8> custom;
};

Shaded shadedOf(Mesh& mesh, bool makeCustom) {
    Shaded out;
    out.normals = mesh.attributes.getOrCreate<Vector3f>(names::kNormal, Domain::Halfedge, AttrType::F32x3);
    out.custom = makeCustom ? mesh.attributes.getOrCreate<u8>(names::kNormalCustom, Domain::Halfedge, AttrType::Bool)
                            : mesh.attributes.get<u8>(names::kNormalCustom, Domain::Halfedge);
    // A layer made second may have moved the first.
    out.normals = mesh.attributes.get<Vector3f>(names::kNormal, Domain::Halfedge);
    return out;
}

std::vector<shading::Corner> sortedUnique(std::vector<shading::Corner> corners) {
    std::sort(corners.begin(), corners.end());
    corners.erase(std::unique(corners.begin(), corners.end()), corners.end());
    return corners;
}

} // namespace

namespace detail {

Vector3f WeightedFaceNormal(const Mesh& mesh, std::span<const Vector3f> faceNormals, HalfedgeId h,
                            shading::Weighting weighting) {
    const Topology& topology = mesh.topology();
    const FaceId face = topology.face(h);
    if (!face.valid() || face.index() >= faceNormals.size()) {
        return Vector3f{0.0f, 0.0f, 0.0f};
    }
    const Vector3f& normal = faceNormals[face.index()];
    if (weighting == shading::Weighting::Even) {
        return normal;
    }
    const std::span<const Vector3f> positions = positionsOf(mesh);
    f32 weight = 1.0f;
    if (weighting == shading::Weighting::Angle || weighting == shading::Weighting::AreaAngle) {
        const std::size_t at = topology.from(h).index();
        const std::size_t next = topology.to(h).index();
        const std::size_t previous = topology.from(topology.prev(h)).index();
        if (at < positions.size() && next < positions.size() && previous < positions.size()) {
            weight *= angleBetween(sub(positions[next], positions[at]), sub(positions[previous], positions[at]));
        }
    }
    if (weighting == shading::Weighting::Area || weighting == shading::Weighting::AreaAngle) {
        weight *= 0.5f * lengthOf(newell(topology, positions, face));
    }
    return scaled(normal, weight);
}

} // namespace detail

namespace shading {

// ============================================================================
// The weighting
// ============================================================================

Weighting WeightingOf(const Mesh& mesh) {
    const std::span<const u16> stored = mesh.attributes.get<const u16>(names::kShading, Domain::Mesh);
    return !stored.empty() && stored[0] < kWeightingCount ? static_cast<Weighting>(stored[0]) : Weighting::Even;
}

bool IsAdopted(const Mesh& mesh) {
    return mesh.attributes.has(names::kShading, Domain::Mesh);
}

void SetWeighting(Mesh& mesh, Weighting weighting) {
    const std::span<u16> stored = mesh.attributes.getOrCreate<u16>(names::kShading, Domain::Mesh, AttrType::U16);
    if (!stored.empty()) {
        stored[0] = static_cast<u16>(weighting);
    }
}

WeightingFit FitWeighting(const Mesh& source) {
    WeightingFit fit;
    if (!source.hasConnectivity()) {
        return fit;
    }
    const std::span<const Vector3f> normals = source.attributes.get<const Vector3f>(names::kNormal, Domain::Halfedge);
    if (normals.empty()) {
        return fit;
    }
    const std::span<const u8> custom = source.attributes.get<const u8>(names::kNormalCustom, Domain::Halfedge);
    const Topology& topology = source.topology();
    const std::vector<Vector3f> faceNormals = detail::FaceNormals(source);
    const f32 cosThreshold = std::cos(ShadingAngle(source));
    std::vector<f32> errors[kWeightingCount];
    for (u32 v = 0; v < topology.vertexCount(); ++v) {
        if (topology.isDeleted(VertexId(v))) {
            continue;
        }
        for (const std::vector<HalfedgeId>& fan : detail::RingFans(source, faceNormals, VertexId(v), cosThreshold)) {
            for (u32 w = 0; w < kWeightingCount; ++w) {
                Vector3f sum{0.0f, 0.0f, 0.0f};
                for (const HalfedgeId h : fan) {
                    sum = add(sum, detail::WeightedFaceNormal(source, faceNormals, h, static_cast<Weighting>(w)));
                }
                const Vector3f value = unitOr(sum);
                for (const HalfedgeId h : fan) {
                    if (h.index() < normals.size() && !(h.index() < custom.size() && custom[h.index()] != 0)) {
                        errors[w].push_back(angleBetween(value, normals[h.index()]));
                    }
                }
            }
        }
    }
    if (errors[0].empty()) {
        return fit;
    }
    u32 best = 0;
    for (u32 w = 0; w < kWeightingCount; ++w) {
        const auto middle = errors[w].begin() + static_cast<std::ptrdiff_t>(errors[w].size() / 2);
        std::nth_element(errors[w].begin(), middle, errors[w].end());
        fit.medians[w] = *middle;
        // Strictly better, by more than rounding: Even wins a tie.
        if (fit.medians[w] + 1e-6f < fit.medians[best]) {
            best = w;
        }
    }
    if (fit.medians[best] <= kWeightingFallback) {
        fit.weighting = static_cast<Weighting>(best);
        fit.fitted = true;
    }
    return fit;
}

// ============================================================================
// The surface
// ============================================================================

Surface::Surface(std::span<Mesh* const> meshes) : meshes_(meshes.begin(), meshes.end()) {
    partners_.resize(meshes_.size());
    pointOf_.resize(meshes_.size());
    // Each mesh's points, then the points of different meshes that coincide.
    f32 tolerance = std::numeric_limits<f32>::max();
    std::vector<u32> base(meshes_.size() + 1, 0);
    std::vector<Vector3f> places;
    std::vector<u32> meshOfPoint;
    for (u32 m = 0; m < meshes_.size(); ++m) {
        Mesh& mesh = *meshes_[m];
        if (!mesh.hasConnectivity()) {
            mesh.ensureConnectivity();
        }
        const skinning::PointTable table = skinning::BuildPointTable(mesh);
        const std::span<const Vector3f> positions = positionsOf(mesh);
        pointOf_[m].assign(table.pointOf.begin(), table.pointOf.end());
        for (u32 p = 0; p < table.pointCount; ++p) {
            const std::span<const u32> members = table.membersOf(p);
            places.push_back(!members.empty() && members[0] < positions.size() ? positions[members[0]]
                                                                             : Vector3f{0.0f, 0.0f, 0.0f});
            meshOfPoint.push_back(m);
        }
        base[m + 1] = base[m] + table.pointCount;
        if (!positions.empty()) {
            tolerance = std::min(tolerance, CoincidenceTolerance(mesh));
        }
    }
    std::vector<u32> parent(places.size());
    std::iota(parent.begin(), parent.end(), 0u);
    const auto root = [&](u32 p) {
        while (parent[p] != p) {
            parent[p] = parent[parent[p]];
            p = parent[p];
        }
        return p;
    };
    if (meshes_.size() > 1 && tolerance < std::numeric_limits<f32>::max()) {
        // The smaller of the tolerances: a larger one would pair what one mesh
        // keeps apart.
        const f64 cell = std::max(static_cast<f64>(tolerance), 1e-12);
        const auto cellOf = [&](f32 x) { return static_cast<i64>(std::floor(static_cast<f64>(x) / cell)); };
        const auto keyOf = [](i64 x, i64 y, i64 z) {
            return static_cast<u64>(x) * 0x9E3779B97F4A7C15ull ^ static_cast<u64>(y) * 0xC2B2AE3D27D4EB4Full ^
                   static_cast<u64>(z) * 0x165667B19E3779F9ull;
        };
        std::unordered_map<u64, std::vector<u32>> grid;
        grid.reserve(places.size());
        for (u32 p = 0; p < places.size(); ++p) {
            const i64 cx = cellOf(places[p].x);
            const i64 cy = cellOf(places[p].y);
            const i64 cz = cellOf(places[p].z);
            for (i64 dx = -1; dx <= 1; ++dx) {
                for (i64 dy = -1; dy <= 1; ++dy) {
                    for (i64 dz = -1; dz <= 1; ++dz) {
                        const auto found = grid.find(keyOf(cx + dx, cy + dy, cz + dz));
                        if (found == grid.end()) {
                            continue;
                        }
                        for (const u32 other : found->second) {
                            if (meshOfPoint[other] != meshOfPoint[p] &&
                                lengthOf(sub(places[other], places[p])) <= tolerance) {
                                parent[root(p)] = root(other);
                            }
                        }
                    }
                }
            }
            grid[keyOf(cx, cy, cz)].push_back(p);
        }
    }
    for (u32 m = 0; m < meshes_.size(); ++m) {
        for (u32& p : pointOf_[m]) {
            p = p == kInvalidIndex ? kInvalidId : root(base[m] + p);
        }
    }

    // Partners: two border edges on one pair of points, running opposite ways.
    struct Border {
        u32 mesh;
        u32 edge;
        bool forward;
    };
    std::unordered_map<u64, std::vector<Border>> borders;
    for (u32 m = 0; m < meshes_.size(); ++m) {
        const Mesh& mesh = *meshes_[m];
        if (!mesh.hasConnectivity()) {
            continue;
        }
        const Topology& topology = mesh.topology();
        partners_[m].assign(topology.edgeCount(), MeshEdge{});
        for (u32 e = 0; e < topology.edgeCount(); ++e) {
            if (topology.isDeleted(EdgeId(e)) || !topology.isBoundary(EdgeId(e))) {
                continue;
            }
            const HalfedgeId h = faceSide(topology, e);
            if (!topology.face(h).valid()) {
                continue;
            }
            const std::size_t from = topology.from(h).index();
            const std::size_t to = topology.to(h).index();
            if (from >= pointOf_[m].size() || to >= pointOf_[m].size()) {
                continue;
            }
            const u32 a = pointOf_[m][from];
            const u32 b = pointOf_[m][to];
            if (a == b || a == kInvalidId || b == kInvalidId) {
                continue;
            }
            borders[(static_cast<u64>(std::min(a, b)) << 32) | std::max(a, b)].push_back(Border{m, e, a < b});
        }
    }
    for (const auto& [key, found] : borders) {
        if (found.size() == 2 && found[0].forward != found[1].forward) {
            partners_[found[0].mesh][found[0].edge] = MeshEdge{found[1].mesh, found[1].edge};
            partners_[found[1].mesh][found[1].edge] = MeshEdge{found[0].mesh, found[0].edge};
        }
    }
}

Surface Surface::ReadOnly(std::span<const Mesh* const> meshes) {
    // The constructor writes only to build connectivity a mesh lacks, and
    // these have theirs.
    std::vector<Mesh*> held;
    held.reserve(meshes.size());
    for (const Mesh* mesh : meshes) {
        held.push_back(const_cast<Mesh*>(mesh));
    }
    return Surface(held);
}

std::optional<MeshEdge> Surface::partner(MeshEdge edge) const {
    if (edge.mesh >= partners_.size() || edge.edge >= partners_[edge.mesh].size() ||
        partners_[edge.mesh][edge.edge].edge == kInvalidId) {
        return std::nullopt;
    }
    return partners_[edge.mesh][edge.edge];
}

bool Surface::twoSided(MeshEdge edge) const {
    if (edge.mesh >= meshes_.size() || !meshes_[edge.mesh]->hasConnectivity()) {
        return false;
    }
    const Topology& topology = std::as_const(*meshes_[edge.mesh]).topology();
    if (!liveEdge(topology, edge.edge)) {
        return false;
    }
    return !topology.isBoundary(EdgeId(edge.edge)) || partner(edge).has_value();
}

bool Surface::hard(MeshEdge edge) const {
    if (edge.mesh >= meshes_.size()) {
        return false;
    }
    if (flagged(*meshes_[edge.mesh], names::kSharp, edge.edge)) {
        return true;
    }
    const std::optional<MeshEdge> twin = partner(edge);
    return twin && flagged(*meshes_[twin->mesh], names::kSharp, twin->edge);
}

std::vector<std::pair<Corner, Corner>> Surface::across(MeshEdge edge) const {
    std::vector<std::pair<Corner, Corner>> out;
    if (!twoSided(edge)) {
        return out;
    }
    const Topology& topology = std::as_const(*meshes_[edge.mesh]).topology();
    const HalfedgeId h = faceSide(topology, edge.edge);
    if (const std::optional<MeshEdge> twin = partner(edge)) {
        const Topology& other = std::as_const(*meshes_[twin->mesh]).topology();
        const HalfedgeId o = faceSide(other, twin->edge);
        out.emplace_back(Corner{edge.mesh, h.value()}, Corner{twin->mesh, other.next(o).value()});
        out.emplace_back(Corner{edge.mesh, topology.next(h).value()}, Corner{twin->mesh, o.value()});
        return out;
    }
    const HalfedgeId o = Topology::opposite(h);
    out.emplace_back(Corner{edge.mesh, h.value()}, Corner{edge.mesh, topology.next(o).value()});
    out.emplace_back(Corner{edge.mesh, topology.next(h).value()}, Corner{edge.mesh, o.value()});
    return out;
}

void Surface::build() const {
    built_ = true;
    fanOf_.assign(meshes_.size(), {});
    faceNormals_.assign(meshes_.size(), {});
    // Each vertex's ring first, in the order `RecomputeNormals` walks it.
    std::vector<std::vector<Corner>> rings;
    for (u32 m = 0; m < meshes_.size(); ++m) {
        const Mesh& mesh = *meshes_[m];
        if (!mesh.hasConnectivity()) {
            continue;
        }
        const Topology& topology = mesh.topology();
        fanOf_[m].assign(topology.halfedgeCount(), kInvalidId);
        faceNormals_[m] = detail::FaceNormals(mesh);
        const f32 cosThreshold = std::cos(ShadingAngle(mesh));
        for (u32 v = 0; v < topology.vertexCount(); ++v) {
            if (topology.isDeleted(VertexId(v))) {
                continue;
            }
            for (const std::vector<HalfedgeId>& ring : detail::RingFans(mesh, faceNormals_[m], VertexId(v), cosThreshold)) {
                const u32 id = static_cast<u32>(rings.size());
                rings.emplace_back();
                for (const HalfedgeId h : ring) {
                    rings.back().push_back(Corner{m, h.value()});
                    fanOf_[m][h.index()] = id;
                }
            }
        }
    }
    // Then joined across each soft partner, where both meshes are adopted: a
    // mesh the Normals workspace never wrote shades as its rings alone.
    std::vector<u32> parent(rings.size());
    std::iota(parent.begin(), parent.end(), 0u);
    const auto root = [&](u32 f) {
        while (parent[f] != f) {
            parent[f] = parent[parent[f]];
            f = parent[f];
        }
        return f;
    };
    for (u32 m = 0; m < meshes_.size(); ++m) {
        for (u32 e = 0; e < partners_[m].size(); ++e) {
            const MeshEdge twin = partners_[m][e];
            const MeshEdge edge{m, e};
            if (twin.edge == kInvalidId || !(edge < twin) || !IsAdopted(*meshes_[m]) ||
                !IsAdopted(*meshes_[twin.mesh]) || hard(edge)) {
                continue;
            }
            for (const auto& [here, there] : across(edge)) {
                const u32 a = fanOf_[here.mesh][here.halfedge];
                const u32 b = fanOf_[there.mesh][there.halfedge];
                if (a != kInvalidId && b != kInvalidId && root(a) != root(b)) {
                    // The lower ring leads, so a fan's corners keep one order.
                    const u32 low = std::min(root(a), root(b));
                    const u32 high = std::max(root(a), root(b));
                    parent[high] = low;
                }
            }
        }
    }
    std::vector<u32> number(rings.size(), kInvalidId);
    std::vector<u32> sizes;
    for (u32 r = 0; r < rings.size(); ++r) {
        const u32 lead = root(r);
        if (number[lead] == kInvalidId) {
            number[lead] = static_cast<u32>(sizes.size());
            sizes.push_back(0);
        }
        number[r] = number[lead];
        sizes[number[r]] += static_cast<u32>(rings[r].size());
    }
    offsets_.assign(sizes.size() + 1, 0);
    for (std::size_t f = 0; f < sizes.size(); ++f) {
        offsets_[f + 1] = offsets_[f] + sizes[f];
    }
    corners_.assign(offsets_.back(), Corner{});
    std::vector<u32> filled(sizes.size(), 0);
    for (u32 r = 0; r < rings.size(); ++r) {
        const u32 f = number[r];
        for (const Corner& corner : rings[r]) {
            corners_[offsets_[f] + filled[f]++] = corner;
            fanOf_[corner.mesh][corner.halfedge] = f;
        }
    }
}

u32 Surface::fanCount() const {
    if (!built_) {
        build();
    }
    return static_cast<u32>(offsets_.size() - 1);
}

u32 Surface::fanOf(Corner corner) const {
    if (!built_) {
        build();
    }
    if (corner.mesh >= fanOf_.size() || corner.halfedge >= fanOf_[corner.mesh].size()) {
        return kInvalidId;
    }
    return fanOf_[corner.mesh][corner.halfedge];
}

std::span<const Corner> Surface::fan(u32 fan) const {
    if (!built_) {
        build();
    }
    if (fan + 1 >= offsets_.size()) {
        return {};
    }
    return std::span<const Corner>(corners_.data() + offsets_[fan], offsets_[fan + 1] - offsets_[fan]);
}

Vector3f Surface::fanNormal(u32 fan) const {
    Vector3f sum{0.0f, 0.0f, 0.0f};
    for (const Corner& corner : this->fan(fan)) {
        const Mesh& mesh = *meshes_[corner.mesh];
        sum = add(sum, detail::WeightedFaceNormal(mesh, faceNormals_[corner.mesh], HalfedgeId(corner.halfedge),
                                                  WeightingOf(mesh)));
    }
    return unitOr(sum);
}

std::span<const Vector3f> Surface::faceNormals(u32 mesh) const {
    if (!built_) {
        build();
    }
    return faceNormals_[mesh];
}

std::vector<Corner> Surface::cornersAtPointOf(u32 mesh, u32 vertex) const {
    std::vector<Corner> out;
    if (mesh >= pointOf_.size() || vertex >= pointOf_[mesh].size()) {
        return out;
    }
    const u32 point = pointOf_[mesh][vertex];
    for (u32 m = 0; m < meshes_.size(); ++m) {
        if (!meshes_[m]->hasConnectivity()) {
            continue;
        }
        const Topology& topology = std::as_const(*meshes_[m]).topology();
        for (u32 v = 0; v < pointOf_[m].size() && v < topology.vertexCount(); ++v) {
            if (pointOf_[m][v] != point || topology.isDeleted(VertexId(v))) {
                continue;
            }
            for (const HalfedgeId h : topology.voh(VertexId(v))) {
                if (!topology.isBoundary(h)) {
                    out.push_back(Corner{m, h.value()});
                }
            }
        }
    }
    return out;
}

namespace {

/// The fans @p corners are in, once each, ascending.
std::vector<u32> fansOf(const Surface& surface, std::span<const Corner> corners) {
    std::vector<u32> fans;
    for (const Corner& corner : corners) {
        const u32 fan = surface.fanOf(corner);
        if (fan != kInvalidId) {
            fans.push_back(fan);
        }
    }
    std::sort(fans.begin(), fans.end());
    fans.erase(std::unique(fans.begin(), fans.end()), fans.end());
    return fans;
}

/// The tangents of @p written rebuilt, where a mesh carries the layer.
void retangent(Surface& surface, std::span<const Corner> written) {
    std::vector<std::vector<HalfedgeId>> perMesh(surface.meshCount());
    for (const Corner& corner : written) {
        perMesh[corner.mesh].push_back(HalfedgeId(corner.halfedge));
    }
    for (u32 m = 0; m < surface.meshCount(); ++m) {
        if (!perMesh[m].empty() && surface.mesh(m).attributes.has(names::kTangent, Domain::Halfedge)) {
            detail::RecomputeTangentsAt(surface.mesh(m), perMesh[m], 0);
        }
    }
}

/// Each of @p fans given its automatic normal. With @p clearCustom every corner
/// takes it and stops being custom; without, a custom corner keeps its own.
std::vector<Corner> writeFans(Surface& surface, std::span<const u32> fans, bool clearCustom) {
    std::vector<Shaded> layers(surface.meshCount());
    std::vector<u8> fetched(surface.meshCount(), 0);
    std::vector<Corner> written;
    for (const u32 fan : fans) {
        const Vector3f value = surface.fanNormal(fan);
        for (const Corner& corner : surface.fan(fan)) {
            if (fetched[corner.mesh] == 0) {
                layers[corner.mesh] = shadedOf(surface.mesh(corner.mesh), false);
                fetched[corner.mesh] = 1;
            }
            Shaded& layer = layers[corner.mesh];
            const bool custom = corner.halfedge < layer.custom.size() && layer.custom[corner.halfedge] != 0;
            if (custom && !clearCustom) {
                continue;
            }
            if (custom) {
                layer.custom[corner.halfedge] = 0;
            }
            if (corner.halfedge < layer.normals.size()) {
                layer.normals[corner.halfedge] = value;
                written.push_back(corner);
            }
        }
    }
    return written;
}

/// Per mesh, which faces @p picked names at @p level: the polygons, or every
/// face of a mesh that is picked at the Mesh level (all of them when none is).
std::vector<std::vector<u8>> pickedFaces(const Surface& surface, std::span<const ElementSet> picked, Level level) {
    bool none = true;
    for (const ElementSet& set : picked) {
        none = none && set.empty();
    }
    std::vector<std::vector<u8>> out(surface.meshCount());
    for (u32 m = 0; m < surface.meshCount(); ++m) {
        const Mesh& mesh = surface.mesh(m);
        if (!mesh.hasConnectivity()) {
            continue;
        }
        const u32 faceCount = mesh.topology().faceCount();
        const bool whole = level == Level::Mesh && (none || (m < picked.size() && !picked[m].empty()));
        out[m].assign(faceCount, whole ? 1 : 0);
        if (level == Level::Polygon && m < picked.size()) {
            for (const u32 f : picked[m].faces) {
                if (f < faceCount) {
                    out[m][f] = 1;
                }
            }
        }
    }
    return out;
}

/// The face on the far side of @p edge from @p h's, on this mesh or its partner's.
std::optional<std::pair<u32, u32>> faceAcross(const Surface& surface, u32 m, HalfedgeId h) {
    const Topology& topology = std::as_const(surface.mesh(m)).topology();
    const HalfedgeId o = Topology::opposite(h);
    if (topology.face(o).valid()) {
        return std::make_pair(m, static_cast<u32>(topology.face(o).index()));
    }
    const std::optional<MeshEdge> twin = surface.partner(MeshEdge{m, Topology::edge(h).value()});
    if (!twin) {
        return std::nullopt;
    }
    const Topology& other = std::as_const(surface.mesh(twin->mesh)).topology();
    return std::make_pair(twin->mesh, static_cast<u32>(other.face(faceSide(other, twin->edge)).index()));
}

/// @p edge and its partner set hard or soft; noted in @p change when that
/// changed how it shades.
void setEdge(Surface& surface, MeshEdge edge, bool hard, ShadingChange& change) {
    if (!surface.twoSided(edge)) {
        return; // a border with nothing on it: no fan crosses it either way
    }
    const bool was = surface.hard(edge);
    const auto write = [&](MeshEdge e) {
        Mesh& mesh = surface.mesh(e.mesh);
        std::span<u8> sharp = mesh.attributes.get<u8>(names::kSharp, Domain::Edge);
        if (sharp.empty() && hard) {
            sharp = mesh.attributes.getOrCreate<u8>(names::kSharp, Domain::Edge, AttrType::Bool);
        }
        if (e.edge < sharp.size()) {
            sharp[e.edge] = hard ? 1 : 0;
        }
    };
    write(edge);
    if (const std::optional<MeshEdge> twin = surface.partner(edge)) {
        write(*twin);
    }
    if (was == hard) {
        return;
    }
    ++change.edges;
    for (const auto& [here, there] : surface.across(edge)) {
        change.corners.push_back(here);
        change.corners.push_back(there);
    }
}

/// The edges a selection names for Hard, Soft and Auto smooth.
std::vector<MeshEdge> edgesNamed(const Surface& surface, std::span<const ElementSet> picked, Level level,
                                 bool betweenOnly) {
    std::vector<MeshEdge> out;
    const std::vector<std::vector<u8>> faces =
        level == Level::Polygon || level == Level::Mesh ? pickedFaces(surface, picked, level)
                                                        : std::vector<std::vector<u8>>{};
    for (u32 m = 0; m < surface.meshCount(); ++m) {
        const Mesh& mesh = surface.mesh(m);
        if (!mesh.hasConnectivity()) {
            continue;
        }
        const Topology& topology = mesh.topology();
        if (level == Level::Edge && m < picked.size()) {
            for (const u32 e : picked[m].edges) {
                if (liveEdge(topology, e)) {
                    out.push_back(MeshEdge{m, e});
                }
            }
        } else if (level == Level::Vertex && m < picked.size()) {
            for (const u32 v : picked[m].vertices) {
                if (v >= topology.vertexCount() || topology.isDeleted(VertexId(v))) {
                    continue;
                }
                for (const HalfedgeId h : topology.voh(VertexId(v))) {
                    out.push_back(MeshEdge{m, Topology::edge(h).value()});
                }
            }
        } else if (level == Level::Polygon || level == Level::Mesh) {
            for (u32 f = 0; f < faces[m].size(); ++f) {
                if (faces[m][f] == 0 || topology.isDeleted(FaceId(f))) {
                    continue;
                }
                for (const HalfedgeId h : topology.fh(FaceId(f))) {
                    if (betweenOnly) {
                        const std::optional<std::pair<u32, u32>> far = faceAcross(surface, m, h);
                        if (!far || faces[far->first][far->second] == 0) {
                            continue;
                        }
                    }
                    out.push_back(MeshEdge{m, Topology::edge(h).value()});
                }
            }
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

} // namespace

// ============================================================================
// The first write
// ============================================================================

AdoptReport Adopt(Surface& surface) {
    AdoptReport report;
    std::vector<u8> fresh(surface.meshCount(), 0);
    for (u32 m = 0; m < surface.meshCount(); ++m) {
        Mesh& mesh = surface.mesh(m);
        if (IsAdopted(mesh) || !mesh.hasConnectivity()) {
            continue;
        }
        fresh[m] = 1;
        ++report.meshes;
        // The weighting first: it is read from the fans the file's own flags
        // and groups make.
        const WeightingFit fit = FitWeighting(mesh);
        // `smoothGroup` is folded into the edges: one truth from here on.
        if (mesh.attributes.has(names::kSmoothGroup, Domain::Face)) {
            const Topology& topology = std::as_const(mesh).topology();
            std::vector<u32> between;
            {
                const std::span<const u32> groups =
                    std::as_const(mesh).attributes.get<const u32>(names::kSmoothGroup, Domain::Face);
                for (u32 e = 0; e < topology.edgeCount(); ++e) {
                    if (topology.isDeleted(EdgeId(e)) || topology.isBoundary(EdgeId(e))) {
                        continue;
                    }
                    const HalfedgeId h = Topology::halfedge(EdgeId(e), 0);
                    const std::size_t a = topology.face(h).index();
                    const std::size_t b = topology.face(Topology::opposite(h)).index();
                    if (a < groups.size() && b < groups.size() && groups[a] != groups[b] &&
                        !flagged(mesh, names::kSharp, e)) {
                        between.push_back(e);
                    }
                }
            }
            if (!between.empty()) {
                const std::span<u8> sharp = mesh.attributes.getOrCreate<u8>(names::kSharp, Domain::Edge, AttrType::Bool);
                for (const u32 e : between) {
                    sharp[e] = 1;
                }
            }
            report.groupEdges += static_cast<u32>(between.size());
            mesh.attributes.remove(names::kSmoothGroup, Domain::Face);
        }
        SetWeighting(mesh, fit.weighting);
    }
    // A partner is hard where the file drew two normals across it: the weld's
    // test for the edges it closes, made here for the ones it could not.
    for (u32 m = 0; m < surface.meshCount(); ++m) {
        const Mesh& mesh = surface.mesh(m);
        if (!mesh.hasConnectivity()) {
            continue;
        }
        const u32 edgeCount = std::as_const(mesh).topology().edgeCount();
        for (u32 e = 0; e < edgeCount; ++e) {
            const MeshEdge edge{m, e};
            const std::optional<MeshEdge> twin = surface.partner(edge);
            if (!twin || !(edge < *twin) || (fresh[m] == 0 && fresh[twin->mesh] == 0)) {
                continue;
            }
            bool breaks = surface.hard(edge);
            for (const auto& [here, there] : surface.across(edge)) {
                const std::span<const Vector3f> a =
                    std::as_const(surface.mesh(here.mesh)).attributes.get<const Vector3f>(names::kNormal, Domain::Halfedge);
                const std::span<const Vector3f> b =
                    std::as_const(surface.mesh(there.mesh)).attributes.get<const Vector3f>(names::kNormal, Domain::Halfedge);
                if (here.halfedge < a.size() && there.halfedge < b.size() &&
                    angleBetween(a[here.halfedge], b[there.halfedge]) > kWeldSharpAngle) {
                    breaks = true;
                }
            }
            if (breaks) {
                for (const MeshEdge& side : {edge, *twin}) {
                    surface.mesh(side.mesh).attributes.getOrCreate<u8>(names::kSharp, Domain::Edge, AttrType::Bool)[side.edge] = 1;
                }
                ++report.partnersHard;
            } else {
                ++report.partnersSoft;
            }
        }
    }
    surface.touch();
    return report;
}

// ============================================================================
// Smoothing
// ============================================================================

ShadingChange Sharpen(Surface& surface, std::span<const ElementSet> picked, Level level, bool hard) {
    ShadingChange change;
    // Polygons go soft only between two of them; everything else is the edges
    // the selection names.
    for (const MeshEdge& edge : edgesNamed(surface, picked, level, level == Level::Polygon && !hard)) {
        setEdge(surface, edge, hard, change);
    }
    change.corners = sortedUnique(std::move(change.corners));
    surface.touch();
    return change;
}

ShadingChange Group(Surface& surface, std::span<const ElementSet> picked, Level level) {
    ShadingChange change;
    if (level != Level::Polygon && level != Level::Mesh) {
        return change;
    }
    const std::vector<std::vector<u8>> faces = pickedFaces(surface, picked, level);
    for (u32 m = 0; m < surface.meshCount(); ++m) {
        const Mesh& mesh = surface.mesh(m);
        if (!mesh.hasConnectivity()) {
            continue;
        }
        const Topology& topology = mesh.topology();
        for (u32 f = 0; f < faces[m].size(); ++f) {
            if (faces[m][f] == 0 || topology.isDeleted(FaceId(f))) {
                continue;
            }
            for (const HalfedgeId h : topology.fh(FaceId(f))) {
                const std::optional<std::pair<u32, u32>> far = faceAcross(surface, m, h);
                if (far) {
                    // Soft inside the selection, hard round it.
                    setEdge(surface, MeshEdge{m, Topology::edge(h).value()}, faces[far->first][far->second] == 0, change);
                }
            }
        }
    }
    change.corners = sortedUnique(std::move(change.corners));
    surface.touch();
    return change;
}

ShadingChange AutoSmooth(Surface& surface, std::span<const ElementSet> picked, Level level,
                         const AutoSmoothOptions& options) {
    ShadingChange change;
    bool none = true;
    for (const ElementSet& set : picked) {
        none = none && set.empty();
    }
    const std::vector<MeshEdge> edges = edgesNamed(surface, picked, none ? Level::Mesh : level, false);
    const f32 cosAngle = std::cos(options.angle);
    for (const MeshEdge& edge : edges) {
        if (!surface.twoSided(edge)) {
            continue;
        }
        const Topology& topology = std::as_const(surface.mesh(edge.mesh)).topology();
        const HalfedgeId h = faceSide(topology, edge.edge);
        const std::optional<std::pair<u32, u32>> far = faceAcross(surface, edge.mesh, h);
        if (!far) {
            continue;
        }
        const Vector3f& here = surface.faceNormals(edge.mesh)[topology.face(h).index()];
        const Vector3f& there = surface.faceNormals(far->first)[far->second];
        bool hard = detail::HardAtAngle(here, there, cosAngle);
        if (options.keepHard && surface.hard(edge)) {
            hard = true;
        }
        if (options.hardAtUvSeams && flagged(surface.mesh(edge.mesh), names::kSeam, edge.edge)) {
            hard = true;
        }
        setEdge(surface, edge, hard, change);
    }
    change.corners = sortedUnique(std::move(change.corners));
    surface.touch();
    return change;
}

u32 FinishShading(Surface& surface, std::span<const Corner> corners) {
    surface.touch();
    const std::vector<u32> fans = fansOf(surface, corners);
    const std::vector<Corner> written = writeFans(surface, fans, true);
    retangent(surface, written);
    return static_cast<u32>(written.size());
}

u32 ReshadeAll(Surface& surface) {
    surface.touch();
    std::vector<u32> fans(surface.fanCount());
    std::iota(fans.begin(), fans.end(), 0u);
    const std::vector<Corner> written = writeFans(surface, fans, false);
    retangent(surface, written);
    return static_cast<u32>(written.size());
}

// ============================================================================
// Editing normals
// ============================================================================

std::vector<Corner> CornersOf(const Surface& surface, std::span<const ElementSet> picked, Level level) {
    std::vector<Corner> out;
    if (level == Level::Polygon || level == Level::Mesh) {
        const std::vector<std::vector<u8>> faces = pickedFaces(surface, picked, level);
        for (u32 m = 0; m < surface.meshCount(); ++m) {
            if (!surface.mesh(m).hasConnectivity()) {
                continue;
            }
            const Topology& topology = std::as_const(surface.mesh(m)).topology();
            for (u32 f = 0; f < faces[m].size(); ++f) {
                if (faces[m][f] != 0 && !topology.isDeleted(FaceId(f))) {
                    for (const HalfedgeId h : topology.fh(FaceId(f))) {
                        out.push_back(Corner{m, h.value()});
                    }
                }
            }
        }
        return sortedUnique(std::move(out));
    }
    for (u32 m = 0; m < surface.meshCount() && m < picked.size(); ++m) {
        if (!surface.mesh(m).hasConnectivity()) {
            continue;
        }
        const Topology& topology = std::as_const(surface.mesh(m)).topology();
        if (level == Level::Vertex) {
            for (const u32 v : picked[m].vertices) {
                const std::vector<Corner> at = surface.cornersAtPointOf(m, v);
                out.insert(out.end(), at.begin(), at.end());
            }
            continue;
        }
        for (const u32 e : picked[m].edges) {
            if (!liveEdge(topology, e)) {
                continue;
            }
            const MeshEdge edge{m, e};
            if (surface.twoSided(edge)) {
                for (const auto& [here, there] : surface.across(edge)) {
                    out.push_back(here);
                    out.push_back(there);
                }
            } else {
                const HalfedgeId h = faceSide(topology, e);
                if (topology.face(h).valid()) {
                    out.push_back(Corner{m, h.value()});
                    out.push_back(Corner{m, topology.next(h).value()});
                }
            }
        }
    }
    return sortedUnique(std::move(out));
}

void SetNormals(Surface& surface, std::span<const Corner> corners, std::span<const Vector3f> values) {
    std::vector<Shaded> layers(surface.meshCount());
    std::vector<u8> fetched(surface.meshCount(), 0);
    std::vector<Corner> written;
    for (std::size_t i = 0; i < corners.size() && i < values.size(); ++i) {
        const Corner& corner = corners[i];
        if (corner.mesh >= surface.meshCount() || lengthOf(values[i]) <= 1e-20f) {
            continue;
        }
        if (fetched[corner.mesh] == 0) {
            layers[corner.mesh] = shadedOf(surface.mesh(corner.mesh), true);
            fetched[corner.mesh] = 1;
        }
        Shaded& layer = layers[corner.mesh];
        if (corner.halfedge < layer.normals.size() && corner.halfedge < layer.custom.size()) {
            layer.normals[corner.halfedge] = unitOr(values[i]);
            layer.custom[corner.halfedge] = 1;
            written.push_back(corner);
        }
    }
    retangent(surface, written);
}

u32 KeepNormals(Surface& surface, std::span<const Corner> corners) {
    std::vector<Shaded> layers(surface.meshCount());
    std::vector<u8> fetched(surface.meshCount(), 0);
    u32 marked = 0;
    for (const Corner& corner : corners) {
        if (corner.mesh >= surface.meshCount()) {
            continue;
        }
        if (fetched[corner.mesh] == 0) {
            layers[corner.mesh] = shadedOf(surface.mesh(corner.mesh), true);
            fetched[corner.mesh] = 1;
        }
        Shaded& layer = layers[corner.mesh];
        if (corner.halfedge < layer.custom.size() && layer.custom[corner.halfedge] == 0) {
            layer.custom[corner.halfedge] = 1;
            ++marked;
        }
    }
    return marked;
}

u32 ResetNormals(Surface& surface, std::span<const Corner> corners) {
    return FinishShading(surface, corners);
}

u32 FavorFaces(Surface& surface, std::span<const ElementSet> picked) {
    const std::vector<std::vector<u8>> faces = pickedFaces(surface, picked, Level::Polygon);
    const std::vector<Corner> seeds = CornersOf(surface, picked, Level::Polygon);
    std::vector<Corner> corners;
    std::vector<Vector3f> values;
    for (const u32 fan : fansOf(surface, seeds)) {
        Vector3f sum{0.0f, 0.0f, 0.0f};
        for (const Corner& corner : surface.fan(fan)) {
            const Topology& topology = std::as_const(surface.mesh(corner.mesh)).topology();
            const std::size_t f = topology.face(HalfedgeId(corner.halfedge)).index();
            if (f < faces[corner.mesh].size() && faces[corner.mesh][f] != 0) {
                sum = add(sum, surface.faceNormals(corner.mesh)[f]);
            }
        }
        if (lengthOf(sum) <= 1e-20f) {
            continue;
        }
        for (const Corner& corner : surface.fan(fan)) {
            corners.push_back(corner);
            values.push_back(sum);
        }
    }
    SetNormals(surface, corners, values);
    return static_cast<u32>(corners.size());
}

void TurnCustomNormals(Mesh& mesh, std::span<const FaceId> faces, std::span<const Vector3f> before) {
    if (!mesh.hasConnectivity()) {
        return;
    }
    const std::span<const u8> custom = std::as_const(mesh).attributes.get<const u8>(names::kNormalCustom, Domain::Halfedge);
    const std::span<Vector3f> normals = mesh.attributes.get<Vector3f>(names::kNormal, Domain::Halfedge);
    if (custom.empty() || normals.empty()) {
        return;
    }
    const Topology& topology = std::as_const(mesh).topology();
    const std::span<const Vector3f> now = positionsOf(mesh);
    // A face's frame: its normal, and its first edge laid into its plane.
    struct Frame {
        Vector3f x, y, z;
        bool ok = false;
    };
    const auto frameOf = [&](FaceId f, std::span<const Vector3f> positions) {
        Frame frame;
        const HalfedgeId first = topology.halfedge(f);
        const std::size_t a = topology.from(first).index();
        const std::size_t b = topology.to(first).index();
        if (a >= positions.size() || b >= positions.size()) {
            return frame;
        }
        const Vector3f normal = newell(topology, positions, f);
        const f32 normalLength = lengthOf(normal);
        if (normalLength <= 1e-20f) {
            return frame;
        }
        frame.z = scaled(normal, 1.0f / normalLength);
        Vector3f along = sub(positions[b], positions[a]);
        along = sub(along, scaled(frame.z, dot(along, frame.z)));
        const f32 alongLength = lengthOf(along);
        if (alongLength <= 1e-20f) {
            return frame;
        }
        frame.x = scaled(along, 1.0f / alongLength);
        frame.y = crossOf(frame.z, frame.x);
        frame.ok = true;
        return frame;
    };
    struct Turned {
        u32 vertex;
        Vector3f was;
        u32 halfedge;
        Vector3f now;
    };
    std::vector<Turned> turned;
    for (const FaceId f : faces) {
        if (f.index() >= topology.faceCount() || topology.isDeleted(f)) {
            continue;
        }
        bool any = false;
        for (const HalfedgeId h : topology.fh(f)) {
            any = any || (h.index() < custom.size() && custom[h.index()] != 0);
        }
        if (!any) {
            continue;
        }
        const Frame from = frameOf(f, before);
        const Frame to = frameOf(f, now);
        if (!from.ok || !to.ok) {
            continue;
        }
        for (const HalfedgeId h : topology.fh(f)) {
            if (h.index() >= custom.size() || custom[h.index()] == 0 || h.index() >= normals.size()) {
                continue;
            }
            const Vector3f& n = normals[h.index()];
            // Into the face's frame as it was, out of it as it is.
            const Vector3f value = add(add(scaled(to.x, dot(n, from.x)), scaled(to.y, dot(n, from.y))),
                                       scaled(to.z, dot(n, from.z)));
            turned.push_back(Turned{static_cast<u32>(topology.from(h).index()), n, h.value(), value});
        }
    }
    // Corners of one vertex that held one normal hold the mean of what they
    // became: a fan stays a fan through a bend.
    std::sort(turned.begin(), turned.end(), [](const Turned& a, const Turned& b) {
        return std::tie(a.vertex, a.was.x, a.was.y, a.was.z, a.halfedge) <
               std::tie(b.vertex, b.was.x, b.was.y, b.was.z, b.halfedge);
    });
    for (std::size_t i = 0; i < turned.size();) {
        std::size_t j = i;
        Vector3f sum{0.0f, 0.0f, 0.0f};
        while (j < turned.size() && turned[j].vertex == turned[i].vertex && turned[j].was.x == turned[i].was.x &&
               turned[j].was.y == turned[i].was.y && turned[j].was.z == turned[i].was.z) {
            sum = add(sum, turned[j].now);
            ++j;
        }
        const Vector3f value = lengthOf(sum) > 1e-20f ? unitOr(sum) : turned[i].was;
        for (std::size_t k = i; k < j; ++k) {
            normals[turned[k].halfedge] = value;
        }
        i = j;
    }
}

// ============================================================================
// Smooth groups
// ============================================================================

namespace {

/// The surface's faces joined across every soft two-sided edge: per mesh, per
/// face, its group's lead (a global face number), and the hard edges met.
struct FaceGroups {
    std::vector<u32> base;
    std::vector<u32> parent;
    u32 hardEdges = 0;

    u32 root(u32 f) {
        while (parent[f] != f) {
            parent[f] = parent[parent[f]];
            f = parent[f];
        }
        return f;
    }
};

FaceGroups faceGroups(const Surface& surface) {
    FaceGroups groups;
    groups.base.assign(surface.meshCount() + 1, 0);
    for (u32 m = 0; m < surface.meshCount(); ++m) {
        const Mesh& mesh = surface.mesh(m);
        groups.base[m + 1] = groups.base[m] + (mesh.hasConnectivity() ? mesh.topology().faceCount() : 0);
    }
    groups.parent.resize(groups.base.back());
    std::iota(groups.parent.begin(), groups.parent.end(), 0u);
    for (u32 m = 0; m < surface.meshCount(); ++m) {
        const Mesh& mesh = surface.mesh(m);
        if (!mesh.hasConnectivity()) {
            continue;
        }
        const Topology& topology = mesh.topology();
        for (u32 e = 0; e < topology.edgeCount(); ++e) {
            const MeshEdge edge{m, e};
            if (!surface.twoSided(edge)) {
                continue;
            }
            const std::optional<MeshEdge> twin = surface.partner(edge);
            if (twin && !(edge < *twin)) {
                continue; // counted from its lower side
            }
            if (surface.hard(edge)) {
                ++groups.hardEdges;
                continue;
            }
            const HalfedgeId h = faceSide(topology, e);
            const std::optional<std::pair<u32, u32>> far = faceAcross(surface, m, h);
            if (far) {
                const u32 a = groups.root(groups.base[m] + static_cast<u32>(topology.face(h).index()));
                const u32 b = groups.root(groups.base[far->first] + far->second);
                groups.parent[std::max(a, b)] = std::min(a, b);
            }
        }
    }
    return groups;
}

} // namespace

SmoothGroups GroupsOf(const Surface& surface) {
    FaceGroups groups = faceGroups(surface);
    std::vector<u32> size(groups.parent.size(), 0);
    for (u32 f = 0; f < groups.parent.size(); ++f) {
        ++size[groups.root(f)];
    }
    SmoothGroups out;
    out.hardEdges = groups.hardEdges;
    out.groupOf.resize(surface.meshCount());
    std::vector<u32> number(groups.parent.size(), 0);
    for (u32 m = 0; m < surface.meshCount(); ++m) {
        const Mesh& mesh = surface.mesh(m);
        const u32 faceCount = groups.base[m + 1] - groups.base[m];
        out.groupOf[m].assign(faceCount, 0);
        for (u32 f = 0; f < faceCount; ++f) {
            if (mesh.topology().isDeleted(FaceId(f))) {
                continue;
            }
            const u32 lead = groups.root(groups.base[m] + f);
            if (size[lead] < 2) {
                continue; // a polygon alone: untinted, so the groups stand out
            }
            if (number[lead] == 0) {
                number[lead] = ++out.groups;
            }
            out.groupOf[m][f] = number[lead];
        }
    }
    return out;
}

std::vector<ElementSet> GroupFaces(const Surface& surface, std::span<const ElementSet> picked) {
    FaceGroups groups = faceGroups(surface);
    std::vector<u8> wanted(groups.parent.size(), 0);
    for (u32 m = 0; m < surface.meshCount() && m < picked.size(); ++m) {
        for (const u32 f : picked[m].faces) {
            if (groups.base[m] + f < groups.base[m + 1]) {
                wanted[groups.root(groups.base[m] + f)] = 1;
            }
        }
    }
    std::vector<ElementSet> out(surface.meshCount());
    for (u32 m = 0; m < surface.meshCount(); ++m) {
        for (u32 f = groups.base[m]; f < groups.base[m + 1]; ++f) {
            if (wanted[groups.root(f)] != 0 && !surface.mesh(m).topology().isDeleted(FaceId(f - groups.base[m]))) {
                out[m].faces.push_back(f - groups.base[m]);
            }
        }
    }
    return out;
}

// ============================================================================
// Check
// ============================================================================

CheckReport CheckNormals(const Surface& surface, const CheckOptions& options) {
    CheckReport report;
    for (CheckRow* row : {&report.broken, &report.seams, &report.insideOut, &report.tangentsBroken,
                          &report.tangentsMirrored, &report.faintHard, &report.backwards, &report.custom}) {
        row->found.resize(surface.meshCount());
    }
    const auto note = [](CheckRow& row, u32 m, u32 face) {
        ++row.count;
        row.found[m].faces.push_back(face);
    };
    for (u32 m = 0; m < surface.meshCount(); ++m) {
        const Mesh& mesh = surface.mesh(m);
        if (!mesh.hasConnectivity()) {
            continue;
        }
        const Topology& topology = mesh.topology();
        const std::span<const Vector3f> positions = positionsOf(mesh);
        const std::span<const Vector3f> normals = mesh.attributes.get<const Vector3f>(names::kNormal, Domain::Halfedge);
        const std::span<const Vector4f> tangents = mesh.attributes.get<const Vector4f>(names::kTangent, Domain::Halfedge);
        const std::span<const u8> custom = mesh.attributes.get<const u8>(names::kNormalCustom, Domain::Halfedge);
        const std::span<const Vector2f> uvs = mesh.attributes.get<const Vector2f>(names::uv(options.uvSet), Domain::Halfedge);
        const std::span<const Vector3f> faceNormals = surface.faceNormals(m);
        const u32 faceCount = topology.faceCount();

        // Each face's UV winding, by a fan from its first corner: the side its
        // tangents should have. Its size is only told from rounding.
        std::vector<f32> uvArea(faceCount, 0.0f);
        if (!uvs.empty()) {
            for (u32 f = 0; f < faceCount; ++f) {
                if (topology.isDeleted(FaceId(f))) {
                    continue;
                }
                std::vector<Vector2f> loop;
                for (const HalfedgeId h : topology.fh(FaceId(f))) {
                    loop.push_back(h.index() < uvs.size() ? uvs[h.index()] : Vector2f{});
                }
                for (std::size_t i = 1; i + 1 < loop.size(); ++i) {
                    uvArea[f] += (loop[i].x - loop[0].x) * (loop[i + 1].y - loop[0].y) -
                                 (loop[i + 1].x - loop[0].x) * (loop[i].y - loop[0].y);
                }
            }
        }
        f32 smallArea = 0.0f;
        {
            std::vector<f32> sizes;
            for (const f32 area : uvArea) {
                if (area != 0.0f) {
                    sizes.push_back(std::fabs(area));
                }
            }
            if (!sizes.empty()) {
                const auto middle = sizes.begin() + static_cast<std::ptrdiff_t>(sizes.size() / 2);
                std::nth_element(sizes.begin(), middle, sizes.end());
                smallArea = *middle * options.smallUvArea;
            }
        }

        for (u32 f = 0; f < faceCount; ++f) {
            if (topology.isDeleted(FaceId(f))) {
                continue;
            }
            for (const HalfedgeId h : topology.fh(FaceId(f))) {
                const std::size_t c = h.index();
                if (c < custom.size() && custom[c] != 0) {
                    note(report.custom, m, f);
                }
                if (c >= normals.size()) {
                    continue;
                }
                const Vector3f& n = normals[c];
                const f32 length = lengthOf(n);
                const bool sound = std::isfinite(n.x) && std::isfinite(n.y) && std::isfinite(n.z) &&
                                   std::fabs(length - 1.0f) <= 0.01f;
                if (!sound) {
                    note(report.broken, m, f);
                    continue;
                }
                if (f < faceNormals.size() && dot(n, faceNormals[f]) < 0.0f) {
                    note(report.backwards, m, f);
                }
                if (c >= tangents.size()) {
                    continue;
                }
                const Vector3f t{tangents[c].x, tangents[c].y, tangents[c].z};
                const f32 tangentLength = lengthOf(t);
                if (!(tangentLength > 1e-6f) || std::fabs(dot(t, n)) > 0.01f * tangentLength) {
                    note(report.tangentsBroken, m, f);
                    continue;
                }
                if (std::fabs(uvArea[f]) > smallArea && (uvArea[f] > 0.0f) != (tangents[c].w < 0.0f)) {
                    note(report.tangentsMirrored, m, f);
                }
            }
        }

        // The edges: a soft one whose sides disagree, a hard one that barely does.
        for (u32 e = 0; e < topology.edgeCount(); ++e) {
            const MeshEdge edge{m, e};
            if (!surface.twoSided(edge)) {
                continue;
            }
            const std::optional<MeshEdge> twin = surface.partner(edge);
            if (twin && (!(edge < *twin) || !IsAdopted(mesh) || !IsAdopted(surface.mesh(twin->mesh)))) {
                continue;
            }
            f32 widest = 0.0f;
            bool measured = false;
            for (const auto& [here, there] : surface.across(edge)) {
                const Mesh& a = surface.mesh(here.mesh);
                const Mesh& b = surface.mesh(there.mesh);
                const std::span<const Vector3f> na = a.attributes.get<const Vector3f>(names::kNormal, Domain::Halfedge);
                const std::span<const Vector3f> nb = b.attributes.get<const Vector3f>(names::kNormal, Domain::Halfedge);
                const std::span<const u8> ca = a.attributes.get<const u8>(names::kNormalCustom, Domain::Halfedge);
                const std::span<const u8> cb = b.attributes.get<const u8>(names::kNormalCustom, Domain::Halfedge);
                if (here.halfedge >= na.size() || there.halfedge >= nb.size() ||
                    (here.halfedge < ca.size() && ca[here.halfedge] != 0) ||
                    (there.halfedge < cb.size() && cb[there.halfedge] != 0)) {
                    continue; // a custom corner is allowed to differ
                }
                widest = std::max(widest, angleBetween(na[here.halfedge], nb[there.halfedge]));
                measured = true;
            }
            if (!measured) {
                continue;
            }
            CheckRow* row = nullptr;
            if (!surface.hard(edge) && widest > kWeldSharpAngle) {
                row = &report.seams;
            } else if (surface.hard(edge) && widest <= options.faintAngle) {
                row = &report.faintHard;
            }
            if (row != nullptr) {
                ++row->count;
                row->found[m].edges.push_back(e);
                if (twin) {
                    row->found[twin->mesh].edges.push_back(twin->edge);
                }
            }
        }

        // A closed part that holds no volume the right way round is inside out.
        std::vector<u32> parent(faceCount);
        std::iota(parent.begin(), parent.end(), 0u);
        const auto root = [&](u32 f) {
            while (parent[f] != f) {
                parent[f] = parent[parent[f]];
                f = parent[f];
            }
            return f;
        };
        for (u32 f = 0; f < faceCount; ++f) {
            if (topology.isDeleted(FaceId(f))) {
                continue;
            }
            for (const HalfedgeId h : topology.fh(FaceId(f))) {
                if (const FaceId other = topology.face(Topology::opposite(h)); other.valid()) {
                    parent[root(f)] = root(static_cast<u32>(other.index()));
                }
            }
        }
        std::vector<Vector3f> centre(faceCount, Vector3f{0.0f, 0.0f, 0.0f});
        std::vector<u32> corners(faceCount, 0);
        std::vector<u8> open(faceCount, 0);
        for (u32 f = 0; f < faceCount; ++f) {
            if (topology.isDeleted(FaceId(f))) {
                continue;
            }
            for (const HalfedgeId h : topology.fh(FaceId(f))) {
                centre[root(f)] = add(centre[root(f)], positions[topology.from(h).index()]);
                ++corners[root(f)];
                open[root(f)] |= topology.face(Topology::opposite(h)).valid() ? 0 : 1;
            }
        }
        std::vector<f32> volume(faceCount, 0.0f);
        for (u32 f = 0; f < faceCount; ++f) {
            if (topology.isDeleted(FaceId(f))) {
                continue;
            }
            const u32 part = root(f);
            const Vector3f c = scaled(centre[part], 1.0f / static_cast<f32>(std::max(corners[part], 1u)));
            std::vector<Vector3f> loop;
            for (const VertexId v : topology.fv(FaceId(f))) {
                loop.push_back(sub(positions[v.index()], c));
            }
            for (std::size_t i = 1; i + 1 < loop.size(); ++i) {
                volume[part] += dot(loop[0], crossOf(loop[i], loop[i + 1]));
            }
        }
        for (u32 f = 0; f < faceCount; ++f) {
            if (!topology.isDeleted(FaceId(f)) && open[root(f)] == 0 && volume[root(f)] < 0.0f) {
                note(report.insideOut, m, f);
            }
        }
    }
    for (CheckRow* row : {&report.broken, &report.seams, &report.insideOut, &report.tangentsBroken,
                          &report.tangentsMirrored, &report.faintHard, &report.backwards, &report.custom}) {
        for (ElementSet& set : row->found) {
            set.normalise();
        }
    }
    return report;
}

} // namespace shading
} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
