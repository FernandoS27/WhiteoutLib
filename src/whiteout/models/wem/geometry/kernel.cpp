// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/geometry/kernel.h>
#include <whiteout/models/wem/geometry/shading.h>
#include <whiteout/models/wem/geometry/triangulation.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace whiteout {
namespace models {
namespace wem {
namespace geom {

namespace {

/// Face @p face as it is drawn, as the halfedges its triangles' corners sit on.
std::vector<HalfedgeId> drawnCorners(const Mesh& mesh, std::span<const Vector3f> positions,
                                     FaceId face) {
    const Topology& topology = mesh.topology();
    std::vector<HalfedgeId> halfedges;
    std::vector<u32> loop;
    for (const HalfedgeId h : topology.fh(face)) {
        halfedges.push_back(h);
        loop.push_back(topology.from(h).value());
    }
    std::vector<u32> cut;
    TriangulateFace(loop, positions, mesh.triangulation.row(face.value()), cut);
    std::vector<HalfedgeId> out;
    out.reserve(cut.size());
    for (const u32 corner : cut) {
        out.push_back(halfedges[corner]);
    }
    return out;
}

// ============================================================================
// Corner groups around a vertex — shared by the two derived-data ops
// ============================================================================

Vector3f faceNormal(const Topology& topology, std::span<const Vector3f> positions, FaceId f) {
    // Newell, so an n-gon gets a sane normal without assuming planarity.
    Vector3f normal{0.0f, 0.0f, 0.0f};
    HalfedgeId h = topology.halfedge(f);
    const HalfedgeId start = h;
    do {
        const std::size_t a = topology.from(h).index();
        const std::size_t b = topology.to(h).index();
        if (a < positions.size() && b < positions.size()) {
            const Vector3f& pa = positions[a];
            const Vector3f& pb = positions[b];
            normal.x += (pa.y - pb.y) * (pa.z + pb.z);
            normal.y += (pa.z - pb.z) * (pa.x + pb.x);
            normal.z += (pa.x - pb.x) * (pa.y + pb.y);
        }
        h = topology.next(h);
    } while (h != start);
    return normal;
}

Vector3f normalized(const Vector3f& v) {
    const f32 length = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (length <= 1e-20f) {
        return Vector3f{0.0f, 0.0f, 1.0f};
    }
    return Vector3f{v.x / length, v.y / length, v.z / length};
}

/// The interior corners around @p v, split into smoothing groups. Each group is
/// a run of the circulation; a break is a sharp edge, a smoothing-group change,
/// or a face-normal angle over @p cosThreshold.
std::vector<std::vector<HalfedgeId>> cornerGroups(const Mesh& mesh,
                                                  std::span<const Vector3f> faceNormals, VertexId v,
                                                  f32 cosThreshold) {
    const Topology& topology = mesh.topology();
    const std::span<const u8> sharp = mesh.attributes.get<const u8>(names::kSharp, Domain::Edge);
    const std::span<const u32> smoothing =
        mesh.attributes.get<const u32>(names::kSmoothGroup, Domain::Face);

    std::vector<HalfedgeId> ring;
    std::vector<bool> breakBefore;
    for (HalfedgeId h : topology.voh(v)) {
        if (topology.isBoundary(h)) {
            continue;
        }
        ring.push_back(h);
    }
    if (ring.empty()) {
        return {};
    }

    // `voh` steps h -> opposite(prev(h)), so the edge between a corner and the
    // one before it is the corner's own: edge(current) == edge(prev(previous)).
    breakBefore.assign(ring.size(), false);
    for (std::size_t i = 0; i < ring.size(); ++i) {
        const HalfedgeId current = ring[i];
        const HalfedgeId previous = ring[(i + ring.size() - 1) % ring.size()];
        bool split = false;
        if (topology.prev(previous) != topology.opposite(current) && ring.size() > 1) {
            split = true; // a boundary gap sits between them
        }
        const std::size_t edgeIndex = Topology::edge(current).index();
        if (edgeIndex < sharp.size() && sharp[edgeIndex] != 0) {
            split = true;
        }
        const std::size_t fa = topology.face(current).index();
        const std::size_t fb = topology.face(previous).index();
        if (fa < smoothing.size() && fb < smoothing.size() && smoothing[fa] != smoothing[fb]) {
            split = true;
        }
        if (fa < faceNormals.size() && fb < faceNormals.size()) {
            const Vector3f& na = faceNormals[fa];
            const Vector3f& nb = faceNormals[fb];
            if (na.x * nb.x + na.y * nb.y + na.z * nb.z < cosThreshold) {
                split = true;
            }
        }
        breakBefore[i] = split;
    }

    std::size_t start = 0;
    bool anyBreak = false;
    for (std::size_t i = 0; i < ring.size(); ++i) {
        if (breakBefore[i]) {
            start = i;
            anyBreak = true;
            break;
        }
    }
    std::vector<std::vector<HalfedgeId>> groups;
    if (!anyBreak) {
        groups.push_back(ring);
        return groups;
    }
    std::vector<HalfedgeId> current;
    for (std::size_t k = 0; k < ring.size(); ++k) {
        const std::size_t i = (start + k) % ring.size();
        if (breakBefore[i] && !current.empty()) {
            groups.push_back(current);
            current.clear();
        }
        current.push_back(ring[i]);
    }
    if (!current.empty()) {
        groups.push_back(current);
    }
    return groups;
}

/// The normal of every face round @p vertices, at the size of the whole list;
/// the faces no corner there reads are left at +Z, unmeasured. What a re-shade
/// of those corners reads, and nothing more.
std::vector<Vector3f> ringFaceNormals(const Mesh& mesh, std::span<const VertexId> vertices) {
    const Topology& topology = mesh.topology();
    const std::span<const Vector3f> positions =
        mesh.attributes.get<const Vector3f>(names::kPosition, Domain::Vertex);
    std::vector<Vector3f> normals(topology.faceCount(), Vector3f{0.0f, 0.0f, 1.0f});
    std::vector<u8> done(topology.faceCount(), 0);
    for (const VertexId v : vertices) {
        if (v.index() >= topology.vertexCount() || topology.isDeleted(v)) {
            continue;
        }
        for (const HalfedgeId h : topology.voh(v)) {
            const FaceId f = topology.face(h);
            if (!f.valid() || done[f.index()] != 0 || topology.isDeleted(f)) {
                continue;
            }
            done[f.index()] = 1;
            normals[f.index()] = normalized(faceNormal(topology, positions, f));
        }
    }
    return normals;
}

std::vector<Vector3f> allFaceNormals(const Mesh& mesh) {
    const Topology& topology = mesh.topology();
    const std::span<const Vector3f> positions =
        mesh.attributes.get<const Vector3f>(names::kPosition, Domain::Vertex);
    std::vector<Vector3f> normals(topology.faceCount(), Vector3f{0.0f, 0.0f, 1.0f});
    for (u32 f = 0; f < topology.faceCount(); ++f) {
        if (topology.isDeleted(FaceId(f))) {
            continue;
        }
        normals[f] = normalized(faceNormal(topology, positions, FaceId(f)));
    }
    return normals;
}

/// How one fan's normal is made and who takes it: the mesh's weighting
/// (`shading`), and every corner but the ones marked custom, whose faces still
/// count in the sum (EDIT_MODE_NORMALS_DESIGN.md §5.1, §5.3).
struct ShadingRule {
    explicit ShadingRule(const Mesh& mesh)
        : weighting(shading::WeightingOf(mesh)),
          custom(mesh.attributes.get<const u8>(names::kNormalCustom, Domain::Halfedge)) {}

    void write(const Mesh& mesh, std::span<const Vector3f> faceNormals, std::span<const HalfedgeId> fan,
               std::span<Vector3f> normals) const {
        const Topology& topology = mesh.topology();
        Vector3f sum{0.0f, 0.0f, 0.0f};
        for (const HalfedgeId h : fan) {
            if (weighting == shading::Weighting::Even) {
                const std::size_t f = topology.face(h).index();
                if (f < faceNormals.size()) {
                    sum.x += faceNormals[f].x;
                    sum.y += faceNormals[f].y;
                    sum.z += faceNormals[f].z;
                }
            } else {
                const Vector3f part = detail::WeightedFaceNormal(mesh, faceNormals, h, weighting);
                sum.x += part.x;
                sum.y += part.y;
                sum.z += part.z;
            }
        }
        const Vector3f value = normalized(sum);
        for (const HalfedgeId h : fan) {
            if (h.index() < normals.size() && !(h.index() < custom.size() && custom[h.index()] != 0)) {
                normals[h.index()] = value;
            }
        }
    }

    shading::Weighting weighting;
    std::span<const u8> custom;
};

} // namespace

// ============================================================================
// GarbageCollect
// ============================================================================

Topology::Remap GarbageCollect(Mesh& mesh) {
    if (!mesh.hasConnectivity()) {
        return Topology::Remap{};
    }
    const Topology::Remap remap = mesh.topology().garbageCollect();
    mesh.attributes.remapDomain(Domain::Vertex, remap.vertices, remap.newVertexCount);
    mesh.attributes.remapDomain(Domain::Halfedge, remap.halfedges, remap.newHalfedgeCount);
    mesh.attributes.remapDomain(Domain::Edge, remap.edges, remap.newEdgeCount);
    mesh.attributes.remapDomain(Domain::Face, remap.faces, remap.newFaceCount);
    if (!mesh.skin.empty()) {
        mesh.skin.remapVertices(remap.vertices, remap.newVertexCount);
    }
    mesh.triangulation.remapFaces(remap.faces, remap.newFaceCount);
    mesh.triangulation.remapVertices(remap.vertices);
    return remap;
}

// ============================================================================
// Canonical numbering (EDIT_MODE_MODELLING_DESIGN.md §2.1)
// ============================================================================

namespace {

/// Each corner's halfedge, face-major from `halfedge(f)`: the order
/// `toFaceSet` writes corners in, so two topologies of one face set line up.
std::vector<u32> cornersOf(const Topology& topology) {
    std::vector<u32> out;
    out.reserve(topology.halfedgeCount());
    for (u32 f = 0; f < topology.faceCount(); ++f) {
        if (topology.isDeleted(FaceId(f)) || !topology.halfedge(FaceId(f)).valid()) {
            continue;
        }
        for (const HalfedgeId h : topology.fh(FaceId(f))) {
            out.push_back(static_cast<u32>(h.index()));
        }
    }
    return out;
}

/// Every layer of @p domain rewritten through @p map (`map[old]` = new, or
/// `kInvalidId`), into @p count elements.
void permuteDomain(AttributeSet& attributes, Domain domain, std::span<const u32> map, u32 count) {
    for (const AttrLayer& layer : attributes.layers()) {
        if (layer.domain != domain) {
            continue;
        }
        AttrLayer& target = *attributes.layer(layer.name, domain);
        const std::size_t stride = AttrTypeSize(target.type);
        std::vector<u8> moved(stride * count, 0);
        for (std::size_t i = 0; i < map.size(); ++i) {
            const std::size_t from = stride * i;
            if (map[i] == kInvalidId || map[i] >= count || from + stride > target.data.size()) {
                continue;
            }
            std::memcpy(moved.data() + stride * map[i], target.data.data() + from, stride);
        }
        target.data = std::move(moved);
    }
    attributes.setDomainCount(domain, count);
}

/// `second[first[i]]`, carried through `kInvalidId`.
std::vector<u32> compose(std::span<const u32> first, std::span<const u32> second) {
    std::vector<u32> out(first.size(), kInvalidId);
    for (std::size_t i = 0; i < first.size(); ++i) {
        if (first[i] != kInvalidId && first[i] < second.size()) {
            out[i] = second[first[i]];
        }
    }
    return out;
}

bool sameArrays(const Topology& a, const Topology& b) {
    const auto equal = [](auto x, auto y) {
        return x.size() == y.size() && std::equal(x.begin(), x.end(), y.begin());
    };
    return equal(a.vertexOutgoingArray(), b.vertexOutgoingArray()) &&
           equal(a.halfedgeToArray(), b.halfedgeToArray()) &&
           equal(a.halfedgeFaceArray(), b.halfedgeFaceArray()) &&
           equal(a.halfedgeNextArray(), b.halfedgeNextArray()) &&
           equal(a.halfedgePrevArray(), b.halfedgePrevArray()) &&
           equal(a.faceHalfedgeArray(), b.faceHalfedgeArray());
}

} // namespace

bool NumberingDirty(const Mesh& mesh) {
    return mesh.connectivity_ && mesh.numberingDirty_;
}

CanonicalRemap Canonicalize(Mesh& mesh) {
    CanonicalRemap out;
    if (!NumberingDirty(mesh)) {
        mesh.numberingDirty_ = false;
        return out;
    }
    const Topology::Remap compacted = GarbageCollect(mesh);

    const Topology& old = mesh.topology_;
    FaceSet faces = old.toFaceSet();
    Topology fresh;
    if (!fresh.build(faces).ok()) {
        // A manifold mesh always rebuilds; one that does not is left as it is,
        // compacted, and still dirty, for `Validate` to name.
        out.vertices = compacted.vertices;
        out.faces = compacted.faces;
        out.halfedges = compacted.halfedges;
        out.edges = compacted.edges;
        return out;
    }

    // Corner k of face f is corner k in both, so each face side maps directly
    // and its opposite with it. A side with no face on either half would have
    // no corner, and a face set cannot hold one.
    const std::vector<u32> oldCorners = cornersOf(old);
    const std::vector<u32> newCorners = cornersOf(fresh);
    std::vector<u32> halfedges(old.halfedgeCount(), kInvalidId);
    for (std::size_t k = 0; k < oldCorners.size() && k < newCorners.size(); ++k) {
        halfedges[oldCorners[k]] = newCorners[k];
        halfedges[oldCorners[k] ^ 1u] = newCorners[k] ^ 1u;
    }
    std::vector<u32> edges(old.edgeCount(), kInvalidId);
    for (u32 e = 0; e < edges.size(); ++e) {
        const u32 h = halfedges[e << 1u];
        edges[e] = h == kInvalidId ? kInvalidId : h >> 1u;
    }
    permuteDomain(mesh.attributes, Domain::Halfedge, halfedges, fresh.halfedgeCount());
    permuteDomain(mesh.attributes, Domain::Edge, edges, fresh.edgeCount());

    mesh.topology_ = std::move(fresh);
    mesh.faces_ = std::move(faces);
    mesh.facesStale_ = false;
    mesh.connectivity_ = true;
    mesh.numberingDirty_ = false;

    out.vertices = compacted.vertices;
    out.faces = compacted.faces;
    out.halfedges = compacted.halfedges.empty() ? halfedges : compose(compacted.halfedges, halfedges);
    out.edges = compacted.edges.empty() ? edges : compose(compacted.edges, edges);
    return out;
}

bool IsCanonical(const Mesh& mesh) {
    if (!mesh.hasConnectivity()) {
        return true;
    }
    const Topology& topology = mesh.topology();
    if (topology.hasDeleted()) {
        return false;
    }
    Topology fresh;
    return fresh.build(topology.toFaceSet()).ok() && sameArrays(topology, fresh);
}

// ============================================================================
// Derived data
// ============================================================================

bool BakeRigidNode(Mesh& mesh, u32 section) {
    if (section >= mesh.sections.size() || !mesh.sections[section].rigidNode) {
        return false;
    }
    const FaceSet& faces = mesh.faceSet();
    const std::span<const u32> faceSections = std::as_const(mesh).faceSections();
    const u32 vertexCount = faces.vertexCount;
    std::vector<u8> inRigid(vertexCount, 0);
    std::vector<u8> inOther(vertexCount, 0);
    std::size_t corner = 0;
    for (std::size_t f = 0; f < faces.faceCount(); ++f) {
        const bool mine = (f < faceSections.size() ? faceSections[f] : 0u) == section;
        for (u32 i = 0; i < faces.faceValence[f]; ++i) {
            const u32 v = faces.cornerVertex[corner + i];
            if (v < vertexCount) {
                (mine ? inRigid : inOther)[v] = 1;
            }
        }
        corner += faces.faceValence[f];
    }
    for (u32 v = 0; v < vertexCount; ++v) {
        if (inRigid[v] != 0 && inOther[v] != 0) {
            return false;
        }
    }

    // CSR is append-only, so the binding is rebuilt in vertex order.
    const Influence rigid{*mesh.sections[section].rigidNode, 1.0f};
    SkinBinding baked;
    baked.reset(0);
    for (u32 v = 0; v < vertexCount; ++v) {
        if (inRigid[v] != 0) {
            baked.appendVertex(std::span<const Influence>(&rigid, 1));
        } else if (v < mesh.skin.vertexCount()) {
            baked.appendVertex(std::as_const(mesh.skin).forVertex(v));
        } else {
            baked.appendVertex({});
        }
    }
    mesh.skin = std::move(baked);
    mesh.sections[section].rigidNode.reset();
    return true;
}

void RecomputeNormals(Mesh& mesh, f32 angleThreshold) {
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        return;
    }
    const std::vector<Vector3f> faceNormals = allFaceNormals(mesh);
    const std::span<Vector3f> normals =
        mesh.attributes.getOrCreate<Vector3f>(names::kNormal, Domain::Halfedge, AttrType::F32x3);
    if (normals.empty()) {
        return;
    }
    const f32 cosThreshold = std::cos(angleThreshold);
    const Topology& topology = mesh.topology();
    const ShadingRule rule(mesh);
    for (u32 v = 0; v < topology.vertexCount(); ++v) {
        if (topology.isDeleted(VertexId(v))) {
            continue;
        }
        for (const std::vector<HalfedgeId>& group :
             cornerGroups(mesh, faceNormals, VertexId(v), cosThreshold)) {
            rule.write(mesh, faceNormals, group, normals);
        }
    }
}

void RecomputeTangents(Mesh& mesh, u32 uvSet) {
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        return;
    }
    // One rule, so a whole mesh and a face set of it never disagree.
    const Topology& topology = std::as_const(mesh).topology();
    std::vector<FaceId> faces;
    faces.reserve(topology.faceCount());
    for (u32 f = 0; f < topology.faceCount(); ++f) {
        if (!topology.isDeleted(FaceId(f))) {
            faces.push_back(FaceId(f));
        }
    }
    RecomputeTangents(mesh, faces, uvSet);
}

// ============================================================================
// The face-set re-shade (EDIT_MODE_MODELLING_DESIGN.md §2.7.10)
// ============================================================================

namespace {

/// The vertices of @p faces, once each.
std::vector<VertexId> verticesOf(const Topology& topology, std::span<const FaceId> faces) {
    std::vector<VertexId> out;
    for (const FaceId f : faces) {
        if (f.index() >= topology.faceCount() || topology.isDeleted(f)) {
            continue;
        }
        for (const VertexId v : topology.fv(f)) {
            out.push_back(v);
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

f32 dot3(const Vector3f& a, const Vector3f& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

} // namespace

f32 ShadingAngle(const Mesh& mesh) {
    const std::span<const u8> modelled =
        mesh.attributes.get<const u8>(names::kModelled, Domain::Mesh);
    return !modelled.empty() && modelled[0] != 0 ? 3.14159265358979f : kDefaultShadingAngle;
}

namespace detail {

std::vector<Vector3f> FaceNormals(const Mesh& mesh) {
    return allFaceNormals(mesh);
}

std::vector<std::vector<HalfedgeId>> RingFans(const Mesh& mesh, std::span<const Vector3f> faceNormals,
                                              VertexId v, f32 cosThreshold) {
    return cornerGroups(mesh, faceNormals, v, cosThreshold);
}

bool HardAtAngle(const Vector3f& a, const Vector3f& b, f32 cosAngle) {
    // A hair under the angle's cosine, so two faces exactly at it read soft
    // whichever way they round: a six-sided cylinder's sides meet at 60 degrees.
    return dot3(a, b) < cosAngle - 1e-5f;
}

} // namespace detail

namespace {

u32 markSharpByAngle(Mesh& mesh, std::span<const EdgeId> edges, bool every, f32 angle,
                     bool keepHard) {
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        return 0;
    }
    const std::vector<Vector3f> faceNormals = allFaceNormals(mesh);
    const Topology& topology = std::as_const(mesh).topology();
    const f32 cosAngle = std::cos(angle);
    // Made only when there is a hard edge to hold: a sphere gains no layer.
    std::span<u8> sharp = mesh.attributes.get<u8>(names::kSharp, Domain::Edge);
    u32 changed = 0;
    const auto mark = [&](u32 e) {
        if (e >= topology.edgeCount() || topology.isDeleted(EdgeId(e))) {
            return;
        }
        const HalfedgeId h = Topology::halfedge(EdgeId(e), 0);
        const FaceId a = topology.face(h);
        const FaceId b = topology.face(Topology::opposite(h));
        if (!a.valid() || !b.valid()) {
            return; // a border: nothing to shade across
        }
        const bool was = e < sharp.size() && sharp[e] != 0;
        const bool hard =
            detail::HardAtAngle(faceNormals[a.index()], faceNormals[b.index()], cosAngle) || (keepHard && was);
        if (hard == was) {
            return;
        }
        if (sharp.empty()) {
            sharp = mesh.attributes.getOrCreate<u8>(names::kSharp, Domain::Edge, AttrType::Bool);
        }
        if (e < sharp.size()) {
            sharp[e] = hard ? 1 : 0;
            ++changed;
        }
    };
    if (every) {
        for (u32 e = 0; e < topology.edgeCount(); ++e) {
            mark(e);
        }
    } else {
        for (const EdgeId e : edges) {
            mark(static_cast<u32>(e.index()));
        }
    }
    return changed;
}

} // namespace

u32 MarkSharpByAngle(Mesh& mesh, std::span<const EdgeId> edges, f32 angle, bool keepHard) {
    return markSharpByAngle(mesh, edges, false, angle, keepHard);
}

u32 MarkSharpByAngle(Mesh& mesh, f32 angle, bool keepHard) {
    return markSharpByAngle(mesh, {}, true, angle, keepHard);
}

void RecomputeNormals(Mesh& mesh, std::span<const FaceId> faces, f32 angleThreshold) {
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        return;
    }
    RecomputeNormalsAt(mesh, verticesOf(std::as_const(mesh).topology(), faces), angleThreshold);
}

void RecomputeNormalsAt(Mesh& mesh, std::span<const VertexId> vertices, f32 angleThreshold) {
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        return;
    }
    const std::vector<Vector3f> faceNormals = ringFaceNormals(mesh, vertices);
    const std::span<Vector3f> normals =
        mesh.attributes.getOrCreate<Vector3f>(names::kNormal, Domain::Halfedge, AttrType::F32x3);
    if (normals.empty()) {
        return;
    }
    const f32 cosThreshold = std::cos(angleThreshold);
    const Topology& topology = std::as_const(mesh).topology();
    const ShadingRule rule(mesh);
    for (const VertexId v : vertices) {
        if (v.index() >= topology.vertexCount() || topology.isDeleted(v)) {
            continue;
        }
        for (const std::vector<HalfedgeId>& group : cornerGroups(mesh, faceNormals, v, cosThreshold)) {
            rule.write(mesh, faceNormals, group, normals);
        }
    }
}

namespace {

/// The one tangent rule (`RecomputeTangents`' words), over the corners at
/// @p vertices. With @p only, a corner is written when its entry is set; the
/// others are read, as members of its class, and left as they are.
void recomputeTangents(Mesh& mesh, std::span<const VertexId> vertices, std::span<const u8> only, u32 uvSet) {
    const Mesh& readable = mesh;
    const std::span<const Vector2f> uvs =
        readable.attributes.get<const Vector2f>(names::uv(uvSet), Domain::Halfedge);
    const std::span<const Vector3f> positions =
        readable.attributes.get<const Vector3f>(names::kPosition, Domain::Vertex);
    if (uvs.empty() || positions.empty()) {
        return;
    }
    const Topology& topology = readable.topology();
    const std::vector<Vector3f> faceNormals = ringFaceNormals(readable, vertices);
    const std::span<const Vector3f> normals =
        readable.attributes.get<const Vector3f>(names::kNormal, Domain::Halfedge);
    const auto normalAt = [&](HalfedgeId h) {
        return h.index() < normals.size() ? normals[h.index()]
                                          : faceNormals[topology.face(h).index()];
    };
    // @p v laid into the plane across @p n, at unit length; false when nothing is left.
    const auto intoPlane = [](Vector3f& v, const Vector3f& n) {
        const f32 d = dot3(v, n);
        v = Vector3f{v.x - n.x * d, v.y - n.y * d, v.z - n.z * d};
        const f32 length = std::sqrt(dot3(v, v));
        if (length <= 1e-20f) {
            return false;
        }
        v = Vector3f{v.x / length, v.y / length, v.z / length};
        return true;
    };

    // MikkTSpace's rule, which the files follow and the bake tools use. Per
    // corner, what the triangles it is drawn in give it: each one's unit dP/du,
    // laid into the corner's normal plane and weighted by the corner's angle
    // there. Per face, which way its UVs wind; 0 where the map has no area.
    std::vector<Vector3f> given(topology.halfedgeCount(), Vector3f{0.0f, 0.0f, 0.0f});
    std::vector<f32> winding(topology.faceCount(), 0.0f);
    std::vector<u8> measured(topology.faceCount(), 0);
    const auto measure = [&](u32 f) {
        if (measured[f] != 0) {
            return;
        }
        measured[f] = 1;
        const std::vector<HalfedgeId> corners = drawnCorners(readable, positions, FaceId(f));
        f32 area = 0.0f;
        for (std::size_t t = 0; t + 2 < corners.size(); t += 3) {
            const HalfedgeId h[3] = {corners[t], corners[t + 1], corners[t + 2]};
            const std::size_t i[3] = {topology.from(h[0]).index(), topology.from(h[1]).index(),
                                      topology.from(h[2]).index()};
            if (i[0] >= positions.size() || i[1] >= positions.size() || i[2] >= positions.size() ||
                h[0].index() >= uvs.size() || h[1].index() >= uvs.size() || h[2].index() >= uvs.size()) {
                continue;
            }
            const Vector3f p[3] = {positions[i[0]], positions[i[1]], positions[i[2]]};
            const Vector3f e1{p[1].x - p[0].x, p[1].y - p[0].y, p[1].z - p[0].z};
            const Vector3f e2{p[2].x - p[0].x, p[2].y - p[0].y, p[2].z - p[0].z};
            const f32 dv1 = uvs[h[1].index()].y - uvs[h[0].index()].y;
            const f32 dv2 = uvs[h[2].index()].y - uvs[h[0].index()].y;
            const f32 determinant = (uvs[h[1].index()].x - uvs[h[0].index()].x) * dv2 -
                                    (uvs[h[2].index()].x - uvs[h[0].index()].x) * dv1;
            if (std::fabs(determinant) < 1e-20f) {
                continue; // no UV area: it says nothing
            }
            area += determinant;
            const f32 sign = determinant < 0.0f ? -1.0f : 1.0f;
            const Vector3f along{(e1.x * dv2 - e2.x * dv1) * sign, (e1.y * dv2 - e2.y * dv1) * sign,
                                 (e1.z * dv2 - e2.z * dv1) * sign};
            for (u32 k = 0; k < 3; ++k) {
                const Vector3f normal = normalAt(h[k]);
                const Vector3f& a = p[(k + 1) % 3];
                const Vector3f& b = p[(k + 2) % 3];
                Vector3f toNext{a.x - p[k].x, a.y - p[k].y, a.z - p[k].z};
                Vector3f toPrevious{b.x - p[k].x, b.y - p[k].y, b.z - p[k].z};
                Vector3f tangent = along;
                if (!intoPlane(toNext, normal) || !intoPlane(toPrevious, normal) ||
                    !intoPlane(tangent, normal)) {
                    continue;
                }
                const f32 angle = std::acos(std::clamp(dot3(toNext, toPrevious), -1.0f, 1.0f));
                Vector3f& sum = given[h[k].index()];
                sum = Vector3f{sum.x + tangent.x * angle, sum.y + tangent.y * angle,
                               sum.z + tangent.z * angle};
            }
        }
        winding[f] = area > 0.0f ? 1.0f : (area < 0.0f ? -1.0f : 0.0f);
    };

    const std::span<Vector4f> tangents =
        mesh.attributes.getOrCreate<Vector4f>(names::kTangent, Domain::Halfedge, AttrType::F32x4);
    std::vector<HalfedgeId> ring;
    std::vector<HalfedgeId> members;
    std::vector<u8> done;
    for (const VertexId v : vertices) {
        ring.clear();
        for (const HalfedgeId h : topology.voh(v)) {
            if (!topology.isBoundary(h)) {
                ring.push_back(h);
                measure(static_cast<u32>(topology.face(h).index()));
            }
        }
        // One tangent for the corners here that share a normal, a UV and a
        // winding: a UV seam or a mirror line keeps two at one vertex, and a
        // crease smoothed into one normal gets one.
        done.assign(ring.size(), 0);
        for (std::size_t i = 0; i < ring.size(); ++i) {
            if (done[i] != 0) {
                continue;
            }
            const Vector3f leadNormal = normalAt(ring[i]);
            const Vector2f leadUv = ring[i].index() < uvs.size() ? uvs[ring[i].index()] : Vector2f{};
            f32 side = winding[topology.face(ring[i]).index()];
            members.clear();
            Vector3f sum{0.0f, 0.0f, 0.0f};
            for (std::size_t j = i; j < ring.size(); ++j) {
                const HalfedgeId h = ring[j];
                const Vector3f normal = normalAt(h);
                const Vector2f uv = h.index() < uvs.size() ? uvs[h.index()] : Vector2f{};
                const f32 own = winding[topology.face(h).index()];
                // An unmapped face's corner joins whatever shares its UV.
                if (done[j] != 0 || uv.x != leadUv.x || uv.y != leadUv.y ||
                    normal.x != leadNormal.x || normal.y != leadNormal.y ||
                    normal.z != leadNormal.z || (side != 0.0f && own != 0.0f && own != side)) {
                    continue;
                }
                side = side != 0.0f ? side : own;
                done[j] = 1;
                members.push_back(h);
                const Vector3f& part = given[h.index()];
                sum = Vector3f{sum.x + part.x, sum.y + part.y, sum.z + part.z};
            }
            for (const HalfedgeId h : members) {
                if (h.index() >= tangents.size() || (!only.empty() && (h.index() >= only.size() || only[h.index()] == 0))) {
                    continue;
                }
                const Vector3f normal = normalAt(h);
                Vector3f along = sum;
                // The files' side (`names::kTangent`): +1 where the UVs wind
                // against the face.
                f32 w = side > 0.0f ? -1.0f : 1.0f;
                if (dot3(along, along) <= 1e-30f) {
                    // Nothing mapped here: along the face's first edge.
                    const HalfedgeId first = topology.halfedge(topology.face(h));
                    const Vector3f& a = positions[topology.from(first).index()];
                    const Vector3f& b = positions[topology.to(first).index()];
                    along = Vector3f{b.x - a.x, b.y - a.y, b.z - a.z};
                    w = 1.0f;
                }
                const f32 d = dot3(along, normal);
                const Vector3f orthogonal = normalized(Vector3f{
                    along.x - normal.x * d, along.y - normal.y * d, along.z - normal.z * d});
                tangents[h.index()] = Vector4f{orthogonal.x, orthogonal.y, orthogonal.z, w};
            }
        }
    }
}

} // namespace

void RecomputeTangents(Mesh& mesh, std::span<const FaceId> faces, u32 uvSet) {
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        return;
    }
    recomputeTangents(mesh, verticesOf(std::as_const(mesh).topology(), faces), {}, uvSet);
}

void RecomputeTangentsAt(Mesh& mesh, std::span<const VertexId> vertices, u32 uvSet) {
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        return;
    }
    recomputeTangents(mesh, vertices, {}, uvSet);
}

namespace detail {

void RecomputeTangentsAt(Mesh& mesh, std::span<const HalfedgeId> corners, u32 uvSet) {
    if (corners.empty() || (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok())) {
        return;
    }
    const Topology& topology = std::as_const(mesh).topology();
    std::vector<u8> only(topology.halfedgeCount(), 0);
    std::vector<VertexId> vertices;
    for (const HalfedgeId h : corners) {
        if (h.index() < only.size() && topology.face(h).valid()) {
            only[h.index()] = 1;
            vertices.push_back(topology.from(h));
        }
    }
    std::sort(vertices.begin(), vertices.end());
    vertices.erase(std::unique(vertices.begin(), vertices.end()), vertices.end());
    recomputeTangents(mesh, vertices, only, uvSet);
}

} // namespace detail

} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
