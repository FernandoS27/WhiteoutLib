// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/geometry/ops.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace whiteout {
namespace models {
namespace wem {
namespace geom {

namespace {

// ============================================================================
// Element allocation — topology and attributes grow together
// ============================================================================

VertexId allocVertex(Mesh& mesh, u32 copySkinFrom) {
    const VertexId v = mesh.topology().addVertex();
    mesh.attributes.appendElement(Domain::Vertex);
    if (!mesh.skin.empty()) {
        if (copySkinFrom < mesh.skin.vertexCount()) {
            mesh.skin.appendCopyOf(copySkinFrom);
        } else {
            mesh.skin.appendVertex({});
        }
    }
    return v;
}

HalfedgeId allocEdge(Mesh& mesh) {
    const HalfedgeId h = mesh.topology().addEdgePair();
    mesh.attributes.appendElement(Domain::Halfedge);
    mesh.attributes.appendElement(Domain::Halfedge);
    mesh.attributes.appendElement(Domain::Edge);
    return h;
}

FaceId allocFace(Mesh& mesh, u32 copyFrom) {
    const FaceId f = mesh.topology().addFaceSlot();
    const u32 index = mesh.attributes.appendElement(Domain::Face);
    if (copyFrom != kInvalidId) {
        mesh.attributes.copyElement(Domain::Face, copyFrom, index);
    }
    return f;
}

// ============================================================================
// Attribute interpolation
// ============================================================================

void lerpElement(AttributeSet& attributes, Domain domain, u32 dst, u32 a, u32 b, f32 t) {
    for (const AttrLayer& layer : attributes.layers()) {
        if (layer.domain != domain) {
            continue;
        }
        AttrLayer& target = *attributes.layer(layer.name, domain);
        const std::size_t stride = AttrTypeSize(target.type);
        if (stride == 0) {
            continue;
        }
        const std::size_t offA = stride * a;
        const std::size_t offB = stride * b;
        const std::size_t offD = stride * dst;
        if (offA + stride > target.data.size() || offB + stride > target.data.size() ||
            offD + stride > target.data.size()) {
            continue;
        }
        switch (target.type) {
        case AttrType::F32:
        case AttrType::F32x2:
        case AttrType::F32x3:
        case AttrType::F32x4:
        case AttrType::Quat: {
            const u32 components = AttrTypeComponents(target.type);
            for (u32 c = 0; c < components; ++c) {
                f32 va = 0.0f;
                f32 vb = 0.0f;
                std::memcpy(&va, target.data.data() + offA + sizeof(f32) * c, sizeof(f32));
                std::memcpy(&vb, target.data.data() + offB + sizeof(f32) * c, sizeof(f32));
                const f32 mixed = va + (vb - va) * t;
                std::memcpy(target.data.data() + offD + sizeof(f32) * c, &mixed, sizeof(f32));
            }
            break;
        }
        case AttrType::U8x4: {
            for (u32 c = 0; c < 4; ++c) {
                const f32 va = static_cast<f32>(target.data[offA + c]);
                const f32 vb = static_cast<f32>(target.data[offB + c]);
                const f32 mixed = va + (vb - va) * t;
                target.data[offD + c] = static_cast<u8>(std::clamp(mixed + 0.5f, 0.0f, 255.0f));
            }
            break;
        }
        default:
            // Discrete values do not interpolate; the nearer end wins.
            std::memcpy(target.data.data() + offD, target.data.data() + (t < 0.5f ? offA : offB),
                        stride);
            break;
        }
    }
}

void copyHalfedgeAttrs(AttributeSet& attributes, u32 from, u32 to) {
    attributes.copyElement(Domain::Halfedge, from, to);
}

// ============================================================================
// Rebuild machinery, shared by the rebuilding ops
// ============================================================================

/// For each corner in `faceSet()` order, the halfedge it currently sits on.
/// Corner order is face-major starting at `halfedge(f)`, which is exactly the
/// order `Topology::toFaceSet()` writes, so the two index alike.
std::vector<u32> snapshotCorners(const Mesh& mesh) {
    std::vector<u32> out;
    if (!mesh.hasConnectivity()) {
        return out;
    }
    const Topology& topology = mesh.topology();
    out.reserve(topology.halfedgeCount());
    for (u32 f = 0; f < topology.faceCount(); ++f) {
        const FaceId face(f);
        if (topology.isDeleted(face)) {
            continue;
        }
        for (HalfedgeId h : topology.fh(face)) {
            out.push_back(static_cast<u32>(h.index()));
        }
    }
    return out;
}

struct RebuildMapping {
    FaceSet faces;
    std::vector<u32> vertexSource; ///< new vertex -> old vertex, or kInvalidId.
    std::vector<u32> faceSource;   ///< new face   -> old face.
    std::vector<u32> cornerSource; ///< new corner -> old corner (into the snapshot).
};

/// Replaces @p mesh's geometry with @p mapping, carrying attribute *data* across
/// by correspondence even though every handle changes.
bool rebuild(Mesh& mesh, RebuildMapping mapping, const std::vector<u32>& oldCornerHalfedge) {
    AttributeSet old = mesh.attributes;
    SkinBinding oldSkin = mesh.skin;

    // The old topology is needed for the Edge-layer lookup below, so take a copy
    // before `setFaceSet` clears it.
    Topology oldTopology;
    if (mesh.hasConnectivity()) {
        oldTopology = static_cast<const Mesh&>(mesh).topology();
    }
    const bool hadConnectivity = !oldTopology.empty();

    // A rebuilding op can perfectly well produce a non-manifold face set — a
    // destructive weld is the obvious way — so the target goes through the same
    // §5.3 repair the importer uses rather than through a bare build that would
    // fail and leave the mesh unusable.
    {
        const std::span<const Vector3f> oldPositions =
            old.get<const Vector3f>(names::kPosition, Domain::Vertex);
        const std::span<const u32> oldSections = old.get<const u32>(names::kSection, Domain::Face);
        std::vector<Vector3f> positions;
        positions.reserve(mapping.vertexSource.size());
        for (u32 v : mapping.vertexSource) {
            positions.push_back(v < oldPositions.size() ? oldPositions[v]
                                                        : Vector3f{0.0f, 0.0f, 0.0f});
        }
        std::vector<u32> sections;
        sections.reserve(mapping.faceSource.size());
        for (u32 f : mapping.faceSource) {
            sections.push_back(f < oldSections.size() ? oldSections[f] : 0u);
        }

        RepairResult repaired = Repair(mapping.faces, sections, positions);
        if (repaired.changed) {
            std::vector<u32> vertexSource(repaired.faces.vertexCount, kInvalidId);
            for (std::size_t i = 0; i < mapping.vertexSource.size() && i < vertexSource.size();
                 ++i) {
                vertexSource[i] = mapping.vertexSource[i];
            }
            for (const VertexSplit& split : repaired.log.splits) {
                if (split.created < vertexSource.size() && split.original < vertexSource.size()) {
                    vertexSource[split.created] = vertexSource[split.original];
                }
            }

            std::vector<u32> faceSource;
            std::vector<u32> cornerSource;
            std::vector<bool> dropped(mapping.faceSource.size(), false);
            for (const FaceRecord& record : repaired.log.droppedFaces) {
                if (record.index < dropped.size()) {
                    dropped[record.index] = true;
                }
            }
            std::size_t corner = 0;
            for (std::size_t f = 0; f < mapping.faceSource.size(); ++f) {
                const u32 valence = mapping.faces.faceValence[f];
                if (!dropped[f]) {
                    faceSource.push_back(mapping.faceSource[f]);
                    for (u32 i = 0; i < valence; ++i) {
                        cornerSource.push_back(mapping.cornerSource[corner + i]);
                    }
                }
                corner += valence;
            }

            mapping.faces = std::move(repaired.faces);
            mapping.vertexSource = std::move(vertexSource);
            mapping.faceSource = std::move(faceSource);
            mapping.cornerSource = std::move(cornerSource);
            // The import-time log described a face set that no longer exists, so
            // `Unrepair` cannot reach the source through it any more. Replacing
            // it is the honest record of what the mesh is now.
            mesh.repairLog = std::move(repaired.log);
        }
    }

    mesh.attributes.clear();
    mesh.setFaceSet(std::move(mapping.faces));

    for (const AttrLayer& layer : old.layers()) {
        if (layer.domain == Domain::Mesh) {
            AttrLayer& target =
                mesh.attributes.create(layer.name, layer.domain, layer.type, layer.storage);
            target.data = layer.data;
            continue;
        }
        mesh.attributes.create(layer.name, layer.domain, layer.type, layer.storage);
    }

    const auto remapDomain = [&](Domain domain, const std::vector<u32>& source) {
        for (const AttrLayer& layer : old.layers()) {
            if (layer.domain != domain) {
                continue;
            }
            AttrLayer& target = *mesh.attributes.layer(layer.name, domain);
            const std::size_t stride = AttrTypeSize(layer.type);
            for (std::size_t i = 0; i < source.size(); ++i) {
                const u32 from = source[i];
                if (from == kInvalidId) {
                    continue;
                }
                const std::size_t offFrom = stride * from;
                const std::size_t offTo = stride * i;
                if (offFrom + stride <= layer.data.size() && offTo + stride <= target.data.size()) {
                    std::memcpy(target.data.data() + offTo, layer.data.data() + offFrom, stride);
                }
            }
        }
    };
    remapDomain(Domain::Vertex, mapping.vertexSource);
    remapDomain(Domain::Face, mapping.faceSource);

    if (!oldSkin.empty()) {
        mesh.skin.reset(0);
        for (u32 v : mapping.vertexSource) {
            if (v < oldSkin.vertexCount()) {
                mesh.skin.appendVertex(oldSkin.forVertex(v));
            } else {
                mesh.skin.appendVertex({});
            }
        }
    }

    const BuildResult built = mesh.ensureConnectivity();
    if (!built.ok()) {
        return false;
    }

    // Halfedge layers, by corner correspondence.
    const Topology& topology = mesh.topology();
    std::vector<u32> newCornerHalfedge;
    newCornerHalfedge.reserve(topology.halfedgeCount());
    for (u32 f = 0; f < topology.faceCount(); ++f) {
        for (HalfedgeId h : topology.fh(FaceId(f))) {
            newCornerHalfedge.push_back(static_cast<u32>(h.index()));
        }
    }
    for (const AttrLayer& layer : old.layers()) {
        if (layer.domain != Domain::Halfedge) {
            continue;
        }
        AttrLayer& target = *mesh.attributes.layer(layer.name, Domain::Halfedge);
        const std::size_t stride = AttrTypeSize(layer.type);
        for (std::size_t k = 0; k < newCornerHalfedge.size() && k < mapping.cornerSource.size();
             ++k) {
            const u32 sourceCorner = mapping.cornerSource[k];
            if (sourceCorner == kInvalidId || sourceCorner >= oldCornerHalfedge.size()) {
                continue;
            }
            const std::size_t offFrom = stride * oldCornerHalfedge[sourceCorner];
            const std::size_t offTo = stride * newCornerHalfedge[k];
            if (offFrom + stride <= layer.data.size() && offTo + stride <= target.data.size()) {
                std::memcpy(target.data.data() + offTo, layer.data.data() + offFrom, stride);
            }
        }
    }

    // Edge layers, by endpoint pair — the only correspondence a rebuild leaves.
    if (hadConnectivity) {
        for (const AttrLayer& layer : old.layers()) {
            if (layer.domain != Domain::Edge) {
                continue;
            }
            AttrLayer& target = *mesh.attributes.layer(layer.name, Domain::Edge);
            const std::size_t stride = AttrTypeSize(layer.type);
            for (u32 e = 0; e < topology.edgeCount(); ++e) {
                const HalfedgeId h = Topology::halfedge(EdgeId(e), 0);
                const std::size_t a = topology.from(h).index();
                const std::size_t b = topology.to(h).index();
                if (a >= mapping.vertexSource.size() || b >= mapping.vertexSource.size()) {
                    continue;
                }
                const u32 oldA = mapping.vertexSource[a];
                const u32 oldB = mapping.vertexSource[b];
                if (oldA == kInvalidId || oldB == kInvalidId || oldA >= oldTopology.vertexCount() ||
                    oldB >= oldTopology.vertexCount()) {
                    continue;
                }
                const HalfedgeId found = oldTopology.findHalfedge(VertexId(oldA), VertexId(oldB));
                if (!found.valid()) {
                    continue;
                }
                const std::size_t offFrom = stride * Topology::edge(found).index();
                const std::size_t offTo = stride * e;
                if (offFrom + stride <= layer.data.size() && offTo + stride <= target.data.size()) {
                    std::memcpy(target.data.data() + offTo, layer.data.data() + offFrom, stride);
                }
            }
        }
    }

    return true;
}

/// Rebuilds from the mesh's own current face set, so an op that only rewrote
/// corner *vertices* (weld, unify, vertex split) does not have to build a
/// mapping for anything it did not touch.
RebuildMapping identityMapping(const Mesh& mesh) {
    RebuildMapping mapping;
    mapping.faces = mesh.faceSet();
    mapping.vertexSource.resize(mapping.faces.vertexCount);
    for (u32 i = 0; i < mapping.faces.vertexCount; ++i) {
        mapping.vertexSource[i] = i;
    }
    mapping.faceSource.resize(mapping.faces.faceCount());
    for (std::size_t i = 0; i < mapping.faceSource.size(); ++i) {
        mapping.faceSource[i] = static_cast<u32>(i);
    }
    mapping.cornerSource.resize(mapping.faces.cornerVertex.size());
    for (std::size_t i = 0; i < mapping.cornerSource.size(); ++i) {
        mapping.cornerSource[i] = static_cast<u32>(i);
    }
    return mapping;
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

    // `voh` steps h -> opposite(prev(h)), so the edge shared by consecutive
    // corners h and step(h) is edge(prev(h)).
    breakBefore.assign(ring.size(), false);
    for (std::size_t i = 0; i < ring.size(); ++i) {
        const HalfedgeId current = ring[i];
        const HalfedgeId previous = ring[(i + ring.size() - 1) % ring.size()];
        const HalfedgeId shared = topology.prev(current);
        bool split = false;
        if (topology.opposite(shared) != previous && ring.size() > 1) {
            split = true; // a boundary gap sits between them
        }
        const std::size_t edgeIndex = Topology::edge(shared).index();
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

} // namespace

// ============================================================================
// SplitEdge
// ============================================================================

VertexId SplitEdge(Mesh& mesh, EdgeId edge, f32 t) {
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        return VertexId();
    }
    Topology& topology = mesh.topology();
    if (edge.index() >= topology.edgeCount() || topology.isDeleted(edge)) {
        return VertexId();
    }
    t = std::clamp(t, 0.0f, 1.0f);

    const HalfedgeId h0 = Topology::halfedge(edge, 0);
    const HalfedgeId h1 = Topology::halfedge(edge, 1);
    const VertexId a = topology.from(h0);
    const VertexId b = topology.to(h0);
    const HalfedgeId n0 = topology.next(h0);
    const HalfedgeId n1 = topology.next(h1);
    const HalfedgeId p1 = topology.prev(h1);
    const FaceId f0 = topology.face(h0);
    const FaceId f1 = topology.face(h1);

    const VertexId v =
        allocVertex(mesh, t < 0.5f ? static_cast<u32>(a.index()) : static_cast<u32>(b.index()));
    {
        // Every Vertex layer — mergeGroup included — takes the nearer end, so a
        // split vertex stays in its group and the render view regroups it. This
        // has to come first: `copyElement` writes *every* layer of the domain,
        // position among them, and would undo the interpolation below.
        mesh.attributes.copyElement(Domain::Vertex, t < 0.5f ? a.index() : b.index(),
                                    static_cast<u32>(v.index()));
        const std::span<Vector3f> positions =
            mesh.attributes.get<Vector3f>(names::kPosition, Domain::Vertex);
        if (v.index() < positions.size() && a.index() < positions.size() &&
            b.index() < positions.size()) {
            const Vector3f& pa = positions[a.index()];
            const Vector3f& pb = positions[b.index()];
            positions[v.index()] = Vector3f{pa.x + (pb.x - pa.x) * t, pa.y + (pb.y - pa.y) * t,
                                            pa.z + (pb.z - pa.z) * t};
        }
    }

    const HalfedgeId g0 = allocEdge(mesh); // v -> b, joins h0's loop
    const HalfedgeId g1 = Topology::opposite(g0);

    topology.setTo(h0, v); // h0: a -> v, so h1 is v -> a
    topology.setTo(g0, b); // g0: v -> b
    topology.setTo(g1, v); // g1: b -> v

    topology.setNext(g0, n0);
    topology.setNext(h0, g0);
    topology.setFace(g0, f0);

    topology.setNext(p1, g1);
    topology.setNext(g1, h1);
    topology.setFace(g1, f1);

    // The corner at v in h0's loop lies at `t` between the corners at a and b;
    // in h1's loop the same point lies at `1 - t` from b.
    copyHalfedgeAttrs(mesh.attributes, static_cast<u32>(h1.index()), static_cast<u32>(g1.index()));
    lerpElement(mesh.attributes, Domain::Halfedge, static_cast<u32>(g0.index()),
                static_cast<u32>(h0.index()), static_cast<u32>(n0.index()), t);
    lerpElement(mesh.attributes, Domain::Halfedge, static_cast<u32>(h1.index()),
                static_cast<u32>(g1.index()), static_cast<u32>(n1.index()), 1.0f - t);

    if (f0.valid()) {
        topology.setFaceHalfedge(f0, h0);
    }
    if (f1.valid()) {
        topology.setFaceHalfedge(f1, h1);
    }
    topology.setOutgoing(v, h1);
    if (topology.outgoing(b) == h1) {
        topology.setOutgoing(b, g1);
    }
    topology.adjustOutgoing(v);
    topology.adjustOutgoing(a);
    topology.adjustOutgoing(b);
    return v;
}

// ============================================================================
// SplitFace
// ============================================================================

FaceId SplitFaceAt(Mesh& mesh, HalfedgeId a, HalfedgeId b) {
    Topology& topology = mesh.topology();
    if (!a.valid() || !b.valid() || a == b) {
        return FaceId();
    }
    const FaceId face = topology.face(a);
    if (!face.valid() || topology.face(b) != face) {
        return FaceId();
    }
    if (topology.next(a) == b || topology.next(b) == a) {
        return FaceId(); // adjacent corners — the diagonal is an existing edge
    }

    const HalfedgeId pa = topology.prev(a);
    const HalfedgeId pb = topology.prev(b);
    const VertexId va = topology.from(a);
    const VertexId vb = topology.from(b);

    const FaceId newFace = allocFace(mesh, static_cast<u32>(face.index()));
    const HalfedgeId g0 = allocEdge(mesh); // va -> vb, on the new face
    const HalfedgeId g1 = Topology::opposite(g0);
    topology.setTo(g0, vb);
    topology.setTo(g1, va);

    // Loop kept by `face`: a .. pb, then g1 (vb -> va).
    topology.setNext(pb, g1);
    topology.setNext(g1, a);
    topology.setFace(g1, face);
    topology.setFaceHalfedge(face, a);

    // Loop taken by `newFace`: b .. pa, then g0 (va -> vb).
    topology.setNext(pa, g0);
    topology.setNext(g0, b);
    topology.setFaceHalfedge(newFace, b);
    HalfedgeId walk = b;
    do {
        topology.setFace(walk, newFace);
        walk = topology.next(walk);
    } while (walk != b);

    copyHalfedgeAttrs(mesh.attributes, static_cast<u32>(a.index()), static_cast<u32>(g0.index()));
    copyHalfedgeAttrs(mesh.attributes, static_cast<u32>(b.index()), static_cast<u32>(g1.index()));

    topology.setOutgoing(va, g0);
    topology.setOutgoing(vb, g1);
    topology.adjustOutgoing(va);
    topology.adjustOutgoing(vb);
    return newFace;
}

FaceId SplitFace(Mesh& mesh, FaceId face, u32 cornerA, u32 cornerB) {
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        return FaceId();
    }
    const Topology& topology = mesh.topology();
    if (face.index() >= topology.faceCount() || topology.isDeleted(face)) {
        return FaceId();
    }
    const u32 valence = topology.valence(face);
    if (cornerA >= valence || cornerB >= valence) {
        return FaceId();
    }
    HalfedgeId a = topology.halfedge(face);
    for (u32 i = 0; i < cornerA; ++i) {
        a = topology.next(a);
    }
    HalfedgeId b = topology.halfedge(face);
    for (u32 i = 0; i < cornerB; ++i) {
        b = topology.next(b);
    }
    return SplitFaceAt(mesh, a, b);
}

// ============================================================================
// Triangulate
// ============================================================================

u32 Triangulate(Mesh& mesh, FaceId face) {
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        return 0;
    }
    if (face.index() >= mesh.topology().faceCount() || mesh.topology().isDeleted(face)) {
        return 0;
    }
    u32 added = 0;
    // `apex` always leaves the fan's first vertex, and after each cut it is the
    // new diagonal on the remainder — which is what keeps this a fan and not a
    // zigzag.
    const Mesh& readable = mesh;
    HalfedgeId apex = readable.topology().halfedge(face);
    while (readable.topology().valence(readable.topology().face(apex)) > 3) {
        const HalfedgeId b = readable.topology().next(readable.topology().next(apex));
        const FaceId remainder = SplitFaceAt(mesh, apex, b);
        if (!remainder.valid()) {
            break;
        }
        ++added;
        apex = readable.topology().prev(b);
    }
    return added;
}

u32 TriangulateAll(Mesh& mesh) {
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        return 0;
    }
    u32 added = 0;
    const u32 faceCount = mesh.topology().faceCount();
    for (u32 f = 0; f < faceCount; ++f) {
        if (!mesh.topology().isDeleted(FaceId(f))) {
            added += Triangulate(mesh, FaceId(f));
        }
    }
    return added;
}

// ============================================================================
// FlipEdge
// ============================================================================

bool FlipEdge(Mesh& mesh, EdgeId edge) {
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        return false;
    }
    Topology& topology = mesh.topology();
    if (edge.index() >= topology.edgeCount() || topology.isDeleted(edge) ||
        topology.isBoundary(edge)) {
        return false;
    }
    const HalfedgeId h0 = Topology::halfedge(edge, 0);
    const HalfedgeId h1 = Topology::halfedge(edge, 1);
    const FaceId f0 = topology.face(h0);
    const FaceId f1 = topology.face(h1);
    if (topology.valence(f0) != 3 || topology.valence(f1) != 3) {
        return false;
    }

    const HalfedgeId n0 = topology.next(h0);
    const HalfedgeId nn0 = topology.next(n0);
    const HalfedgeId n1 = topology.next(h1);
    const HalfedgeId nn1 = topology.next(n1);
    const VertexId a = topology.from(h0);
    const VertexId b = topology.to(h0);
    const VertexId c = topology.to(n0);
    const VertexId d = topology.to(n1);
    if (c == d || topology.findHalfedge(c, d).valid()) {
        return false; // the new diagonal already exists
    }

    topology.setTo(h0, d); // h0: c -> d
    topology.setTo(h1, c); // h1: d -> c

    topology.setNext(h0, nn1);
    topology.setNext(nn1, n0);
    topology.setNext(n0, h0);
    topology.setFace(h0, f0);
    topology.setFace(nn1, f0);
    topology.setFace(n0, f0);
    topology.setFaceHalfedge(f0, h0);

    topology.setNext(h1, nn0);
    topology.setNext(nn0, n1);
    topology.setNext(n1, h1);
    topology.setFace(h1, f1);
    topology.setFace(nn0, f1);
    topology.setFace(n1, f1);
    topology.setFaceHalfedge(f1, h1);

    topology.setOutgoing(a, n1);
    topology.setOutgoing(b, n0);
    topology.setOutgoing(c, h0);
    topology.setOutgoing(d, h1);
    topology.adjustOutgoing(a);
    topology.adjustOutgoing(b);
    topology.adjustOutgoing(c);
    topology.adjustOutgoing(d);
    return true;
}

// ============================================================================
// CollapseEdge
// ============================================================================

namespace {

/// The loop `h` belongs to, as a halfedge count. Boundary loops count too.
u32 loopLength(const Topology& topology, HalfedgeId h) {
    u32 count = 0;
    HalfedgeId walk = h;
    do {
        ++count;
        walk = topology.next(walk);
        if (count > 1u << 24) {
            return count; // corrupt; caller refuses
        }
    } while (walk != h);
    return count;
}

bool sideIsCollapsible(const Topology& topology, HalfedgeId h) {
    const u32 length = loopLength(topology, h);
    if (length < 3) {
        return false;
    }
    if (length == 3) {
        // A triangular *face* is spliced away; a boundary loop of three would be
        // left with two halfedges between one vertex pair, which is degenerate.
        return topology.face(h).valid();
    }
    return true;
}

} // namespace

bool IsCollapseLegal(const Mesh& mesh, HalfedgeId h) {
    if (!mesh.hasConnectivity()) {
        return false;
    }
    const Topology& topology = mesh.topology();
    if (!h.valid() || h.index() >= topology.halfedgeCount() || topology.isDeleted(h)) {
        return false;
    }
    const HalfedgeId o = Topology::opposite(h);
    const VertexId v0 = topology.from(h);
    const VertexId v1 = topology.to(h);
    if (v0 == v1) {
        return false;
    }
    if (!sideIsCollapsible(topology, h) || !sideIsCollapsible(topology, o)) {
        return false;
    }
    // A boundary vertex may only collapse along the boundary; otherwise the
    // collapse pinches two boundary loops together.
    if (topology.isBoundary(v0) && topology.isBoundary(v1) &&
        !topology.isBoundary(Topology::edge(h))) {
        return false;
    }

    // The link condition: the one-rings of v0 and v1 may meet only at the
    // vertices opposite the edge — one per incident face.
    std::vector<VertexId> ring0;
    for (VertexId n : topology.vv(v0)) {
        ring0.push_back(n);
    }
    u32 shared = 0;
    for (VertexId n : topology.vv(v1)) {
        if (std::find(ring0.begin(), ring0.end(), n) != ring0.end()) {
            ++shared;
        }
    }
    u32 expected = 0;
    if (topology.face(h).valid()) {
        ++expected;
    }
    if (topology.face(o).valid()) {
        ++expected;
    }
    return shared == expected;
}

bool CollapseEdge(Mesh& mesh, HalfedgeId h) {
    if (!IsCollapseLegal(mesh, h)) {
        return false;
    }
    Topology& topology = mesh.topology();
    const HalfedgeId o = Topology::opposite(h);
    const VertexId v0 = topology.from(h);
    const VertexId v1 = topology.to(h);

    // Everything that pointed at v0 points at v1 from here on.
    std::vector<HalfedgeId> incoming;
    for (HalfedgeId oh : topology.voh(v0)) {
        incoming.push_back(Topology::opposite(oh));
    }
    for (HalfedgeId ih : incoming) {
        topology.setTo(ih, v1);
    }

    // Each side is either spliced (a triangle collapsing to one edge) or simply
    // unlinked (a longer loop that just loses a corner).
    const auto handleSide = [&](HalfedgeId side) {
        const HalfedgeId sideNext = topology.next(side);
        const HalfedgeId sidePrev = topology.prev(side);
        const FaceId face = topology.face(side);
        if (face.valid() && loopLength(topology, side) == 3) {
            const HalfedgeId keep = sideNext;                    // v1 -> w
            const HalfedgeId drop = topology.next(sideNext);     // w  -> v1
            const HalfedgeId dropOpp = Topology::opposite(drop); // v1 -> w, other side
            const VertexId w = topology.to(keep);

            // `keep` takes over `dropOpp`'s place, so the two surviving
            // neighbours meet across one edge — attributes ride along, because
            // `keep` now occupies that loop's corner.
            const HalfedgeId dropOppPrev = topology.prev(dropOpp);
            const HalfedgeId dropOppNext = topology.next(dropOpp);
            const FaceId dropOppFace = topology.face(dropOpp);
            copyHalfedgeAttrs(mesh.attributes, static_cast<u32>(dropOpp.index()),
                              static_cast<u32>(keep.index()));
            topology.setNext(dropOppPrev, keep);
            topology.setNext(keep, dropOppNext);
            topology.setFace(keep, dropOppFace);
            if (dropOppFace.valid()) {
                topology.setFaceHalfedge(dropOppFace, keep);
            }
            topology.setTo(keep, w);

            topology.setStatus(face, topology.status(face) | Status::Deleted);
            topology.setStatus(Topology::edge(drop),
                               topology.status(Topology::edge(drop)) | Status::Deleted);
            topology.setStatus(drop, topology.status(drop) | Status::Deleted);
            topology.setStatus(dropOpp, topology.status(dropOpp) | Status::Deleted);

            if (topology.outgoing(w) == drop || topology.outgoing(w) == dropOpp) {
                topology.setOutgoing(w, Topology::opposite(keep));
            }
            if (topology.outgoing(v1) == dropOpp || topology.outgoing(v1) == drop) {
                topology.setOutgoing(v1, keep);
            }
        } else {
            topology.setNext(sidePrev, sideNext);
            if (face.valid() && topology.halfedge(face) == side) {
                topology.setFaceHalfedge(face, sideNext);
            }
        }
    };
    handleSide(h);
    handleSide(o);

    if (topology.outgoing(v1) == o || topology.outgoing(v1) == h) {
        // Any surviving outgoing halfedge will do; `adjustOutgoing` then prefers
        // a boundary one.
        HalfedgeId replacement;
        for (HalfedgeId ih : incoming) {
            const HalfedgeId candidate = Topology::opposite(ih);
            if (!topology.isDeleted(candidate) && candidate != h && candidate != o) {
                replacement = candidate;
                break;
            }
        }
        topology.setOutgoing(v1, replacement);
    }
    topology.setStatus(Topology::edge(h), topology.status(Topology::edge(h)) | Status::Deleted);
    topology.setStatus(h, topology.status(h) | Status::Deleted);
    topology.setStatus(o, topology.status(o) | Status::Deleted);
    topology.setStatus(v0, topology.status(v0) | Status::Deleted);
    if (topology.outgoing(v1).valid()) {
        topology.adjustOutgoing(v1);
    }
    return true;
}

// ============================================================================
// Dissolve
// ============================================================================

bool DissolveEdge(Mesh& mesh, EdgeId edge) {
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        return false;
    }
    Topology& topology = mesh.topology();
    if (edge.index() >= topology.edgeCount() || topology.isDeleted(edge) ||
        topology.isBoundary(edge)) {
        return false;
    }
    const HalfedgeId h0 = Topology::halfedge(edge, 0);
    const HalfedgeId h1 = Topology::halfedge(edge, 1);
    const FaceId f0 = topology.face(h0);
    const FaceId f1 = topology.face(h1);
    if (f0 == f1) {
        return false;
    }
    const HalfedgeId p0 = topology.prev(h0);
    const HalfedgeId n0 = topology.next(h0);
    const HalfedgeId p1 = topology.prev(h1);
    const HalfedgeId n1 = topology.next(h1);
    const VertexId a = topology.from(h0);
    const VertexId b = topology.from(h1);

    topology.setNext(p0, n1);
    topology.setNext(p1, n0);
    HalfedgeId walk = n0;
    do {
        topology.setFace(walk, f0);
        walk = topology.next(walk);
    } while (walk != n0);
    topology.setFaceHalfedge(f0, n0);

    topology.setStatus(f1, topology.status(f1) | Status::Deleted);
    topology.setStatus(edge, topology.status(edge) | Status::Deleted);
    topology.setStatus(h0, topology.status(h0) | Status::Deleted);
    topology.setStatus(h1, topology.status(h1) | Status::Deleted);

    if (topology.outgoing(a) == h0) {
        topology.setOutgoing(a, n1);
    }
    if (topology.outgoing(b) == h1) {
        topology.setOutgoing(b, n0);
    }
    topology.adjustOutgoing(a);
    topology.adjustOutgoing(b);
    return true;
}

bool DissolveVertex(Mesh& mesh, VertexId vertex) {
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        return false;
    }
    Topology& topology = mesh.topology();
    if (vertex.index() >= topology.vertexCount() || topology.isDeleted(vertex)) {
        return false;
    }
    if (topology.valence(vertex) != 2) {
        return false;
    }
    const HalfedgeId ha = topology.outgoing(vertex); // v -> a
    const HalfedgeId oa = Topology::opposite(ha);    // a -> v
    const HalfedgeId hb = topology.next(oa);         // v -> b
    const HalfedgeId ob = Topology::opposite(hb);    // b -> v
    if (hb == ha) {
        return false;
    }
    const VertexId b = topology.to(hb);

    // Re-purpose ha's edge as a <-> b: `oa` becomes a -> b, and `ha`, being its
    // opposite, is b -> a without touching its `to`.
    const HalfedgeId obPrev = topology.prev(ob);
    const HalfedgeId hbNext = topology.next(hb);
    topology.setTo(oa, b);
    topology.setNext(obPrev, ha);
    topology.setNext(oa, hbNext);

    const FaceId faceA = topology.face(oa);
    const FaceId faceB = topology.face(ha);
    if (faceA.valid() && topology.halfedge(faceA) == hb) {
        topology.setFaceHalfedge(faceA, oa);
    }
    if (faceB.valid() && topology.halfedge(faceB) == ob) {
        topology.setFaceHalfedge(faceB, ha);
    }

    topology.setStatus(Topology::edge(hb), topology.status(Topology::edge(hb)) | Status::Deleted);
    topology.setStatus(hb, topology.status(hb) | Status::Deleted);
    topology.setStatus(ob, topology.status(ob) | Status::Deleted);
    topology.setStatus(vertex, topology.status(vertex) | Status::Deleted);

    topology.setOutgoing(b, ha);
    topology.setOutgoing(topology.from(oa), oa);
    topology.adjustOutgoing(b);
    topology.adjustOutgoing(topology.from(oa));
    return true;
}

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
    return remap;
}

// ============================================================================
// WeldVertices
// ============================================================================

WeldResult WeldVertices(Mesh& mesh, f32 epsilon, bool respectMergeGroups) {
    WeldResult result;
    const std::vector<u32> snapshot = snapshotCorners(mesh);
    const FaceSet& faces = mesh.faceSet();
    const std::span<const Vector3f> positions =
        mesh.attributes.get<const Vector3f>(names::kPosition, Domain::Vertex);
    const std::span<const u32> groups =
        mesh.attributes.get<const u32>(names::kMergeGroup, Domain::Vertex);
    if (positions.empty()) {
        return result;
    }

    // Quantise to an epsilon lattice, then confirm with a real distance test
    // against the bucket's representative, so the result does not depend on
    // which of two nearly equal points was seen first.
    const f32 cell = epsilon > 0.0f ? epsilon : 1e-6f;
    struct Key {
        i64 x, y, z;
        u32 group;
        bool operator==(const Key& other) const {
            return x == other.x && y == other.y && z == other.z && group == other.group;
        }
    };
    struct KeyHash {
        std::size_t operator()(const Key& k) const {
            u64 hash = 1469598103934665603ull;
            const auto mix = [&hash](u64 value) {
                hash ^= value;
                hash *= 1099511628211ull;
            };
            mix(static_cast<u64>(k.x));
            mix(static_cast<u64>(k.y));
            mix(static_cast<u64>(k.z));
            mix(k.group);
            return static_cast<std::size_t>(hash);
        }
    };
    std::unordered_map<Key, std::vector<u32>, KeyHash> buckets; // looked up, never iterated

    std::vector<u32> newIndex(positions.size(), kInvalidId);
    std::vector<u32> vertexSource;
    const f32 epsilonSq = epsilon * epsilon;
    for (u32 v = 0; v < positions.size(); ++v) {
        const Vector3f& p = positions[v];
        const u32 group = respectMergeGroups ? (v < groups.size() ? groups[v] : v) : 0u;
        u32 match = kInvalidId;
        for (i64 dx = -1; dx <= 1 && match == kInvalidId; ++dx) {
            for (i64 dy = -1; dy <= 1 && match == kInvalidId; ++dy) {
                for (i64 dz = -1; dz <= 1 && match == kInvalidId; ++dz) {
                    const Key key{static_cast<i64>(std::floor(p.x / cell)) + dx,
                                  static_cast<i64>(std::floor(p.y / cell)) + dy,
                                  static_cast<i64>(std::floor(p.z / cell)) + dz, group};
                    const auto bucket = buckets.find(key);
                    if (bucket == buckets.end()) {
                        continue;
                    }
                    for (u32 candidate : bucket->second) {
                        const Vector3f& q = positions[candidate];
                        const f32 ddx = p.x - q.x;
                        const f32 ddy = p.y - q.y;
                        const f32 ddz = p.z - q.z;
                        if (ddx * ddx + ddy * ddy + ddz * ddz <= epsilonSq) {
                            match = candidate;
                            break;
                        }
                    }
                }
            }
        }
        if (match != kInvalidId) {
            newIndex[v] = newIndex[match];
            ++result.verticesMerged;
            continue;
        }
        const Key key{static_cast<i64>(std::floor(p.x / cell)),
                      static_cast<i64>(std::floor(p.y / cell)),
                      static_cast<i64>(std::floor(p.z / cell)), group};
        buckets[key].push_back(v);
        newIndex[v] = static_cast<u32>(vertexSource.size());
        vertexSource.push_back(v);
    }
    if (result.verticesMerged == 0) {
        return result;
    }

    RebuildMapping mapping;
    mapping.vertexSource = std::move(vertexSource);
    mapping.faces.vertexCount = static_cast<u32>(mapping.vertexSource.size());
    std::size_t corner = 0;
    for (std::size_t f = 0; f < faces.faceCount(); ++f) {
        const u32 valence = faces.faceValence[f];
        std::vector<u32> rewritten;
        rewritten.reserve(valence);
        bool degenerate = false;
        for (u32 i = 0; i < valence; ++i) {
            const u32 mapped = newIndex[faces.cornerVertex[corner + i]];
            if (std::find(rewritten.begin(), rewritten.end(), mapped) != rewritten.end()) {
                degenerate = true;
                break;
            }
            rewritten.push_back(mapped);
        }
        if (degenerate || rewritten.size() < 3) {
            ++result.facesDropped;
            corner += valence;
            continue;
        }
        mapping.faceSource.push_back(static_cast<u32>(f));
        mapping.faces.faceValence.push_back(valence);
        for (u32 i = 0; i < valence; ++i) {
            mapping.faces.cornerVertex.push_back(rewritten[i]);
            mapping.cornerSource.push_back(static_cast<u32>(corner + i));
        }
        corner += valence;
    }

    rebuild(mesh, std::move(mapping), snapshot);
    return result;
}

// ============================================================================
// SplitVertexByHalfedgeAttr
// ============================================================================

u32 SplitVertexByHalfedgeAttr(Mesh& mesh, std::span<const std::string> layers) {
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        return 0;
    }
    const std::vector<u32> snapshot = snapshotCorners(mesh);
    const FaceSet& faces = mesh.faceSet();

    std::vector<const AttrLayer*> resolved;
    for (const std::string& name : layers) {
        if (const AttrLayer* layer = mesh.attributes.layer(name, Domain::Halfedge)) {
            resolved.push_back(layer);
        }
    }
    if (resolved.empty()) {
        return 0;
    }

    // One new vertex per (vertex, attribute tuple) — the same grouping the render
    // view does, just materialised into the document.
    struct KeyHash {
        std::size_t operator()(const std::string& s) const {
            return std::hash<std::string>()(s);
        }
    };
    std::unordered_map<std::string, u32, KeyHash> seen; // looked up, never iterated
    std::vector<u32> vertexSource;
    RebuildMapping mapping;
    mapping.faces.faceValence = faces.faceValence;
    mapping.faceSource.resize(faces.faceCount());
    for (std::size_t i = 0; i < mapping.faceSource.size(); ++i) {
        mapping.faceSource[i] = static_cast<u32>(i);
    }
    mapping.cornerSource.resize(faces.cornerVertex.size());
    for (std::size_t i = 0; i < mapping.cornerSource.size(); ++i) {
        mapping.cornerSource[i] = static_cast<u32>(i);
    }
    mapping.faces.cornerVertex.resize(faces.cornerVertex.size());

    // Pass one assigns provisional ids in corner order; pass two renumbers them
    // source-vertex-major, so a vertex's copies sit together and right where the
    // original was. Corner order alone is just as deterministic but permutes the
    // whole array for the sake of one seam.
    std::vector<u32> provisional(faces.cornerVertex.size(), kInvalidId);
    std::string key;
    for (std::size_t c = 0; c < faces.cornerVertex.size(); ++c) {
        const u32 vertex = faces.cornerVertex[c];
        const u32 halfedge = c < snapshot.size() ? snapshot[c] : 0;
        key.clear();
        key.append(reinterpret_cast<const char*>(&vertex), sizeof(vertex));
        for (const AttrLayer* layer : resolved) {
            const std::size_t stride = AttrTypeSize(layer->type);
            const std::size_t offset = stride * halfedge;
            if (offset + stride <= layer->data.size()) {
                key.append(reinterpret_cast<const char*>(layer->data.data() + offset), stride);
            } else {
                key.append(stride, '\0');
            }
        }
        const auto found = seen.find(key);
        if (found != seen.end()) {
            provisional[c] = found->second;
            continue;
        }
        const u32 index = static_cast<u32>(vertexSource.size());
        vertexSource.push_back(vertex);
        seen.emplace(key, index);
        provisional[c] = index;
    }

    if (vertexSource.size() == faces.vertexCount) {
        return 0;
    }

    std::vector<u32> order(vertexSource.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = static_cast<u32>(i);
    }
    std::stable_sort(order.begin(), order.end(),
                     [&](u32 lhs, u32 rhs) { return vertexSource[lhs] < vertexSource[rhs]; });
    std::vector<u32> finalOf(vertexSource.size(), kInvalidId);
    mapping.vertexSource.resize(order.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        finalOf[order[i]] = static_cast<u32>(i);
        mapping.vertexSource[i] = vertexSource[order[i]];
    }
    for (std::size_t c = 0; c < provisional.size(); ++c) {
        mapping.faces.cornerVertex[c] = finalOf[provisional[c]];
    }

    mapping.faces.vertexCount = static_cast<u32>(mapping.vertexSource.size());
    const u32 before = mesh.vertexCount();
    rebuild(mesh, std::move(mapping), snapshot);
    // Not `vertexSource.size() - faces.vertexCount`: separating a vertex's fans
    // can leave two faces meeting at a single vertex, and the rebuild's repair
    // then splits that bowtie too. Report what actually happened.
    return mesh.vertexCount() - before;
}

// ============================================================================
// UnifyWinding
// ============================================================================

u32 UnifyWinding(Mesh& mesh) {
    const std::vector<u32> snapshot = snapshotCorners(mesh);
    const FaceSet& faces = mesh.faceSet();
    const std::size_t faceCount = faces.faceCount();
    if (faceCount == 0) {
        return 0;
    }

    std::vector<u32> base(faceCount + 1, 0);
    for (std::size_t f = 0; f < faceCount; ++f) {
        base[f + 1] = base[f] + faces.faceValence[f];
    }

    // Directed corner pairs, so two faces agree exactly when they traverse a
    // shared edge in opposite directions.
    std::unordered_map<u64, std::vector<u32>> byEdge; // looked up, never iterated
    const auto undirected = [](u32 a, u32 b) {
        const u64 lo = std::min(a, b);
        const u64 hi = std::max(a, b);
        return (lo << 32) | hi;
    };
    for (std::size_t f = 0; f < faceCount; ++f) {
        const u32 valence = faces.faceValence[f];
        for (u32 i = 0; i < valence; ++i) {
            const u32 a = faces.cornerVertex[base[f] + i];
            const u32 b = faces.cornerVertex[base[f] + (i + 1) % valence];
            byEdge[undirected(a, b)].push_back(static_cast<u32>(f));
        }
    }

    std::vector<i8> orientation(faceCount, 0); // 0 unvisited, 1 keep, -1 flip
    std::vector<u32> stack;
    u32 flipped = 0;
    const auto traversesForward = [&](std::size_t f, u32 a, u32 b) {
        const u32 valence = faces.faceValence[f];
        for (u32 i = 0; i < valence; ++i) {
            const u32 x = faces.cornerVertex[base[f] + i];
            const u32 y = faces.cornerVertex[base[f] + (i + 1) % valence];
            if (x == a && y == b) {
                return true;
            }
        }
        return false;
    };
    for (std::size_t seed = 0; seed < faceCount; ++seed) {
        if (orientation[seed] != 0) {
            continue;
        }
        orientation[seed] = 1;
        stack.push_back(static_cast<u32>(seed));
        while (!stack.empty()) {
            const u32 f = stack.back();
            stack.pop_back();
            const u32 valence = faces.faceValence[f];
            for (u32 i = 0; i < valence; ++i) {
                const u32 a = faces.cornerVertex[base[f] + i];
                const u32 b = faces.cornerVertex[base[f] + (i + 1) % valence];
                const auto bucket = byEdge.find(undirected(a, b));
                if (bucket == byEdge.end()) {
                    continue;
                }
                for (u32 other : bucket->second) {
                    if (other == f || orientation[other] != 0) {
                        continue;
                    }
                    // `f` traverses a->b in its *effective* winding; a neighbour
                    // agrees when it traverses b->a in its own.
                    const bool fReversed = orientation[f] < 0;
                    const bool otherAgrees =
                        traversesForward(other, fReversed ? a : b, fReversed ? b : a);
                    orientation[other] = otherAgrees ? static_cast<i8>(1) : static_cast<i8>(-1);
                    if (!otherAgrees) {
                        ++flipped;
                    }
                    stack.push_back(other);
                }
            }
        }
    }
    if (flipped == 0) {
        return 0;
    }

    RebuildMapping mapping = identityMapping(mesh);
    for (std::size_t f = 0; f < faceCount; ++f) {
        if (orientation[f] >= 0) {
            continue;
        }
        const u32 valence = faces.faceValence[f];
        for (u32 i = 0; i < valence / 2; ++i) {
            std::swap(mapping.faces.cornerVertex[base[f] + i],
                      mapping.faces.cornerVertex[base[f] + valence - 1 - i]);
            std::swap(mapping.cornerSource[base[f] + i],
                      mapping.cornerSource[base[f] + valence - 1 - i]);
        }
    }
    rebuild(mesh, std::move(mapping), snapshot);
    return flipped;
}

// ============================================================================
// MergeMeshes / SplitMesh
// ============================================================================

Mesh MergeMeshes(std::span<const Mesh> meshes) {
    Mesh out;
    FaceSet faces;
    u32 vertexBase = 0;
    u32 sectionBase = 0;
    u32 groupBase = 0;

    // Declare the union of every input's layers first, so a layer only some
    // inputs carry still lands with zeroes where it was absent.
    for (const Mesh& mesh : meshes) {
        for (const AttrLayer& layer : mesh.attributes.layers()) {
            out.attributes.create(layer.name, layer.domain, layer.type, layer.storage);
        }
    }

    std::vector<u32> vertexSource;
    std::vector<u32> vertexOwner;
    std::vector<u32> faceSource;
    std::vector<u32> faceOwner;
    std::vector<u32> cornerSource;
    std::vector<u32> cornerOwner;
    std::vector<std::vector<u32>> snapshots;
    std::vector<u32> sectionOffsets;
    std::vector<u32> groupOffsets;
    snapshots.reserve(meshes.size());

    for (const Mesh& mesh : meshes) {
        snapshots.push_back(snapshotCorners(mesh));
        sectionOffsets.push_back(sectionBase);
        groupOffsets.push_back(groupBase);
        const FaceSet& source = mesh.faceSet();
        const u32 owner = static_cast<u32>(&mesh - meshes.data());
        for (u32 v = 0; v < source.vertexCount; ++v) {
            vertexSource.push_back(v);
            vertexOwner.push_back(owner);
        }
        std::size_t corner = 0;
        for (std::size_t f = 0; f < source.faceCount(); ++f) {
            const u32 valence = source.faceValence[f];
            faces.faceValence.push_back(valence);
            faceSource.push_back(static_cast<u32>(f));
            faceOwner.push_back(owner);
            for (u32 i = 0; i < valence; ++i) {
                faces.cornerVertex.push_back(source.cornerVertex[corner + i] + vertexBase);
                cornerSource.push_back(static_cast<u32>(corner + i));
                cornerOwner.push_back(owner);
            }
            corner += valence;
        }
        vertexBase += source.vertexCount;
        sectionBase += static_cast<u32>(mesh.sections.size());

        const std::span<const u32> groups =
            mesh.attributes.get<const u32>(names::kMergeGroup, Domain::Vertex);
        u32 highest = 0;
        for (u32 value : groups) {
            highest = std::max(highest, value + 1);
        }
        groupBase += std::max(highest, source.vertexCount);

        for (const MeshSection& section : mesh.sections) {
            out.sections.push_back(section);
        }
    }
    faces.vertexCount = vertexBase;
    out.setFaceSet(std::move(faces));

    const auto copyDomain = [&](Domain domain, const std::vector<u32>& source,
                                const std::vector<u32>& owner) {
        for (const AttrLayer& layer : out.attributes.layers()) {
            if (layer.domain != domain) {
                continue;
            }
            AttrLayer& target = *out.attributes.layer(layer.name, domain);
            const std::size_t stride = AttrTypeSize(target.type);
            for (std::size_t i = 0; i < source.size(); ++i) {
                const AttrLayer* from = meshes[owner[i]].attributes.layer(layer.name, domain);
                if (from == nullptr) {
                    continue;
                }
                const std::size_t offFrom = stride * source[i];
                const std::size_t offTo = stride * i;
                if (offFrom + stride <= from->data.size() && offTo + stride <= target.data.size()) {
                    std::memcpy(target.data.data() + offTo, from->data.data() + offFrom, stride);
                }
            }
        }
    };
    copyDomain(Domain::Vertex, vertexSource, vertexOwner);
    copyDomain(Domain::Face, faceSource, faceOwner);

    // Sections and merge groups are per-mesh numbers; both move into the
    // combined space or the result draws the wrong material and welds the wrong
    // vertices.
    {
        const std::span<u32> sectionValues = out.faceSections();
        for (std::size_t i = 0; i < sectionValues.size() && i < faceOwner.size(); ++i) {
            sectionValues[i] += sectionOffsets[faceOwner[i]];
        }
        const std::span<u32> groupValues =
            out.attributes.get<u32>(names::kMergeGroup, Domain::Vertex);
        for (std::size_t i = 0; i < groupValues.size() && i < vertexOwner.size(); ++i) {
            // An input with no layer of its own would otherwise contribute every
            // vertex to one group, and the render view would weld the lot.
            const bool authored =
                meshes[vertexOwner[i]].attributes.has(names::kMergeGroup, Domain::Vertex);
            groupValues[i] =
                (authored ? groupValues[i] : vertexSource[i]) + groupOffsets[vertexOwner[i]];
        }
    }

    // Left default-constructed when no input carries a skin: an *empty* binding
    // is legal, one sized for zero vertices in a mesh that has some is not.
    bool anySkin = false;
    for (const Mesh& mesh : meshes) {
        anySkin = anySkin || !mesh.skin.empty();
    }
    if (anySkin) {
        out.skin.reset(0);
        for (std::size_t i = 0; i < vertexSource.size(); ++i) {
            const SkinBinding& source = meshes[vertexOwner[i]].skin;
            if (vertexSource[i] < source.vertexCount()) {
                out.skin.appendVertex(source.forVertex(vertexSource[i]));
            } else {
                out.skin.appendVertex({});
            }
        }
    }

    if (!out.ensureConnectivity().ok()) {
        return out;
    }
    const Topology& topology = out.topology();
    std::size_t corner = 0;
    for (u32 f = 0; f < topology.faceCount(); ++f) {
        for (HalfedgeId h : topology.fh(FaceId(f))) {
            if (corner >= cornerSource.size()) {
                break;
            }
            const std::vector<u32>& snapshot = snapshots[cornerOwner[corner]];
            const u32 sourceCorner = cornerSource[corner];
            if (sourceCorner < snapshot.size()) {
                const AttributeSet& from = meshes[cornerOwner[corner]].attributes;
                for (const AttrLayer& layer : out.attributes.layers()) {
                    if (layer.domain != Domain::Halfedge) {
                        continue;
                    }
                    const AttrLayer* sourceLayer = from.layer(layer.name, Domain::Halfedge);
                    if (sourceLayer == nullptr) {
                        continue;
                    }
                    AttrLayer& target = *out.attributes.layer(layer.name, Domain::Halfedge);
                    const std::size_t stride = AttrTypeSize(target.type);
                    const std::size_t offFrom = stride * snapshot[sourceCorner];
                    const std::size_t offTo = stride * h.index();
                    if (offFrom + stride <= sourceLayer->data.size() &&
                        offTo + stride <= target.data.size()) {
                        std::memcpy(target.data.data() + offTo, sourceLayer->data.data() + offFrom,
                                    stride);
                    }
                }
            }
            ++corner;
        }
    }
    out.recomputeBounds();
    return out;
}

std::vector<Mesh> SplitMesh(const Mesh& mesh) {
    std::vector<Mesh> out;
    const u32 sectionCount = mesh.sections.empty() ? 1u : static_cast<u32>(mesh.sections.size());
    const std::vector<u32> snapshot = snapshotCorners(mesh);
    const FaceSet& faces = mesh.faceSet();
    const std::span<const u32> sections = mesh.faceSections();

    std::vector<u32> base(faces.faceCount() + 1, 0);
    for (std::size_t f = 0; f < faces.faceCount(); ++f) {
        base[f + 1] = base[f] + faces.faceValence[f];
    }

    for (u32 s = 0; s < sectionCount; ++s) {
        Mesh part = mesh; // carries name, sections metadata, layer declarations
        part.sections.clear();
        if (s < mesh.sections.size()) {
            part.sections.push_back(mesh.sections[s]);
        }

        RebuildMapping mapping;
        std::vector<u32> remap(faces.vertexCount, kInvalidId);
        for (std::size_t f = 0; f < faces.faceCount(); ++f) {
            const u32 value = f < sections.size() ? sections[f] : 0;
            if (value != s) {
                continue;
            }
            const u32 valence = faces.faceValence[f];
            mapping.faceSource.push_back(static_cast<u32>(f));
            mapping.faces.faceValence.push_back(valence);
            for (u32 i = 0; i < valence; ++i) {
                const u32 vertex = faces.cornerVertex[base[f] + i];
                if (remap[vertex] == kInvalidId) {
                    remap[vertex] = static_cast<u32>(mapping.vertexSource.size());
                    mapping.vertexSource.push_back(vertex);
                }
                mapping.faces.cornerVertex.push_back(remap[vertex]);
                mapping.cornerSource.push_back(base[f] + i);
            }
        }
        mapping.faces.vertexCount = static_cast<u32>(mapping.vertexSource.size());
        rebuild(part, std::move(mapping), snapshot);
        // One section per part, so every face names section 0.
        for (u32& value : part.faceSections()) {
            value = 0;
        }
        part.recomputeBounds();
        out.push_back(std::move(part));
    }
    return out;
}

// ============================================================================
// Derived data
// ============================================================================

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
    for (u32 v = 0; v < topology.vertexCount(); ++v) {
        if (topology.isDeleted(VertexId(v))) {
            continue;
        }
        for (const std::vector<HalfedgeId>& group :
             cornerGroups(mesh, faceNormals, VertexId(v), cosThreshold)) {
            Vector3f sum{0.0f, 0.0f, 0.0f};
            for (HalfedgeId h : group) {
                const std::size_t f = topology.face(h).index();
                if (f < faceNormals.size()) {
                    sum.x += faceNormals[f].x;
                    sum.y += faceNormals[f].y;
                    sum.z += faceNormals[f].z;
                }
            }
            const Vector3f value = normalized(sum);
            for (HalfedgeId h : group) {
                if (h.index() < normals.size()) {
                    normals[h.index()] = value;
                }
            }
        }
    }
}

void RecomputeTangents(Mesh& mesh, u32 uvSet) {
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        return;
    }
    const std::span<const Vector2f> uvs =
        mesh.attributes.get<const Vector2f>(names::uv(uvSet), Domain::Halfedge);
    const std::span<const Vector3f> positions =
        mesh.attributes.get<const Vector3f>(names::kPosition, Domain::Vertex);
    if (uvs.empty() || positions.empty()) {
        return;
    }
    const std::vector<Vector3f> faceNormals = allFaceNormals(mesh);
    const Topology& topology = mesh.topology();

    // Per-face tangent, then averaged over the same smoothing groups the normals
    // use — so a UV seam keeps two tangents at one vertex, which is the point.
    std::vector<Vector3f> faceTangents(topology.faceCount(), Vector3f{1.0f, 0.0f, 0.0f});
    for (u32 f = 0; f < topology.faceCount(); ++f) {
        if (topology.isDeleted(FaceId(f))) {
            continue;
        }
        const HalfedgeId h0 = topology.halfedge(FaceId(f));
        const HalfedgeId h1 = topology.next(h0);
        const HalfedgeId h2 = topology.next(h1);
        const std::size_t i0 = topology.from(h0).index();
        const std::size_t i1 = topology.from(h1).index();
        const std::size_t i2 = topology.from(h2).index();
        if (i0 >= positions.size() || i1 >= positions.size() || i2 >= positions.size() ||
            h2.index() >= uvs.size()) {
            continue;
        }
        const Vector3f e1{positions[i1].x - positions[i0].x, positions[i1].y - positions[i0].y,
                          positions[i1].z - positions[i0].z};
        const Vector3f e2{positions[i2].x - positions[i0].x, positions[i2].y - positions[i0].y,
                          positions[i2].z - positions[i0].z};
        const f32 du1 = uvs[h1.index()].x - uvs[h0.index()].x;
        const f32 dv1 = uvs[h1.index()].y - uvs[h0.index()].y;
        const f32 du2 = uvs[h2.index()].x - uvs[h0.index()].x;
        const f32 dv2 = uvs[h2.index()].y - uvs[h0.index()].y;
        const f32 determinant = du1 * dv2 - du2 * dv1;
        if (std::fabs(determinant) < 1e-20f) {
            continue;
        }
        const f32 r = 1.0f / determinant;
        faceTangents[f] = Vector3f{(e1.x * dv2 - e2.x * dv1) * r, (e1.y * dv2 - e2.y * dv1) * r,
                                   (e1.z * dv2 - e2.z * dv1) * r};
    }

    const std::span<const Vector3f> normals =
        mesh.attributes.get<const Vector3f>(names::kNormal, Domain::Halfedge);
    const std::span<Vector4f> tangents =
        mesh.attributes.getOrCreate<Vector4f>(names::kTangent, Domain::Halfedge, AttrType::F32x4);
    const f32 cosThreshold = std::cos(1.047197551f);
    for (u32 v = 0; v < topology.vertexCount(); ++v) {
        if (topology.isDeleted(VertexId(v))) {
            continue;
        }
        for (const std::vector<HalfedgeId>& group :
             cornerGroups(mesh, faceNormals, VertexId(v), cosThreshold)) {
            Vector3f sum{0.0f, 0.0f, 0.0f};
            for (HalfedgeId h : group) {
                const std::size_t f = topology.face(h).index();
                if (f < faceTangents.size()) {
                    sum.x += faceTangents[f].x;
                    sum.y += faceTangents[f].y;
                    sum.z += faceTangents[f].z;
                }
            }
            for (HalfedgeId h : group) {
                if (h.index() >= tangents.size()) {
                    continue;
                }
                Vector3f normal{0.0f, 0.0f, 1.0f};
                if (h.index() < normals.size()) {
                    normal = normals[h.index()];
                } else {
                    const std::size_t f = topology.face(h).index();
                    if (f < faceNormals.size()) {
                        normal = faceNormals[f];
                    }
                }
                const f32 dot = sum.x * normal.x + sum.y * normal.y + sum.z * normal.z;
                const Vector3f orthogonal = normalized(Vector3f{
                    sum.x - normal.x * dot, sum.y - normal.y * dot, sum.z - normal.z * dot});
                tangents[h.index()] = Vector4f{orthogonal.x, orthogonal.y, orthogonal.z, 1.0f};
            }
        }
    }
}

} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
