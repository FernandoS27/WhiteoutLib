// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/geometry/ops.h>

#include "rebuild.h"

#include <whiteout/models/wem/geometry/interpolate.h>
#include <whiteout/models/wem/geometry/modelling.h>
#include <whiteout/models/wem/geometry/triangulation.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <optional>
#include <unordered_map>
#include <utility>

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
    mesh.triangulation.appendFace();
    return f;
}

/// A face whose corner cycle changed drops its row (§2.3): the tool's commit
/// materialises a fresh one.
void clearRow(Mesh& mesh, FaceId face) {
    if (face.valid() && !mesh.triangulation.row(face.value()).empty()) {
        mesh.triangulation.setRow(face.value(), {}, std::as_const(mesh).topology().faceCount());
    }
}

/// Face @p face's loop from its first halfedge, as vertex ids.
std::vector<u32> loopOf(const Topology& topology, FaceId face) {
    std::vector<u32> loop;
    for (const HalfedgeId h : topology.fh(face)) {
        loop.push_back(topology.from(h).value());
    }
    return loop;
}

/// Face @p face as it is drawn (`TriangulateFace`): vertex ids, three per triangle.
std::vector<u32> drawnTriangles(const Mesh& mesh, FaceId face) {
    const std::vector<u32> loop = loopOf(mesh.topology(), face);
    std::vector<u32> cut;
    TriangulateFace(loop, mesh.attributes.get<const Vector3f>(names::kPosition, Domain::Vertex),
                    mesh.triangulation.row(face.value()), cut);
    for (u32& corner : cut) {
        corner = loop[corner];
    }
    return cut;
}

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

void copyHalfedgeAttrs(AttributeSet& attributes, u32 from, u32 to) {
    attributes.copyElement(Domain::Halfedge, from, to);
}

/// Edge @p from's values folded into edge @p into's by type, the rule for an
/// edge two old ones became: Bool ORs, F32 takes the maximum, anything else
/// keeps @p into's.
void combineEdgeInto(AttributeSet& attributes, u32 from, u32 into) {
    for (const AttrLayer& layer : attributes.layers()) {
        if (layer.domain != Domain::Edge) {
            continue;
        }
        AttrLayer& target = *attributes.layer(layer.name, Domain::Edge);
        const std::size_t stride = AttrTypeSize(target.type);
        if (stride * (std::max(from, into) + 1) > target.data.size()) {
            continue;
        }
        u8* out = target.data.data() + stride * into;
        const u8* in = target.data.data() + stride * from;
        if (target.type == AttrType::Bool) {
            out[0] = (out[0] != 0 || in[0] != 0) ? 1 : 0;
        } else if (target.type == AttrType::F32) {
            f32 a = 0.0f;
            f32 b = 0.0f;
            std::memcpy(&a, out, sizeof(f32));
            std::memcpy(&b, in, sizeof(f32));
            const f32 larger = std::max(a, b);
            std::memcpy(out, &larger, sizeof(f32));
        }
    }
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
// Rebuild machinery (rebuild.h), shared by the rebuilding ops
// ============================================================================

namespace detail {

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

std::vector<u32> snapshotCornersBuilt(const Mesh& mesh) {
    if (mesh.hasConnectivity()) {
        return snapshotCorners(mesh);
    }
    // The build is deterministic, so a local one numbers halfedges exactly as
    // the mesh's Halfedge layers are indexed (render_view.cpp does the same).
    Topology local;
    std::vector<u32> out;
    if (!local.build(mesh.faceSet()).ok()) {
        return out;
    }
    out.reserve(local.halfedgeCount());
    for (u32 f = 0; f < local.faceCount(); ++f) {
        for (HalfedgeId h : local.fh(FaceId(f))) {
            out.push_back(static_cast<u32>(h.index()));
        }
    }
    return out;
}

namespace {

/// The rows a rebuild's new faces take from their source faces (§2.3): each
/// vertex mapped through that face's own corners, which is exact where the
/// repair split a vertex, and each triangle turned round where the new loop runs
/// the old one backwards. A face whose cycle changed takes none.
FaceTriangulation carryRows(const FaceTriangulation& old, const FaceSet& oldFaces,
                            const RebuildMapping& mapping) {
    if (old.empty()) {
        return {};
    }
    std::vector<u32> oldBase(oldFaces.faceCount() + 1, 0);
    for (std::size_t f = 0; f < oldFaces.faceCount(); ++f) {
        oldBase[f + 1] = oldBase[f] + oldFaces.faceValence[f];
    }
    FaceTriangulationBuilder builder(static_cast<u32>(mapping.faces.faceCount()));
    std::vector<std::pair<u32, u32>> renames;
    std::vector<u32> mapped;
    std::size_t base = 0;
    for (std::size_t k = 0; k < mapping.faces.faceCount(); base += mapping.faces.faceValence[k++]) {
        const u32 valence = mapping.faces.faceValence[k];
        const u32 source = k < mapping.faceSource.size() ? mapping.faceSource[k] : kInvalidId;
        if (valence < 4 || source >= oldFaces.faceCount() ||
            oldFaces.faceValence[source] != valence || old.row(source).empty()) {
            continue;
        }
        renames.clear();
        std::vector<u32> ordinals;
        bool whole = true;
        for (u32 i = 0; i < valence && whole; ++i) {
            const u32 corner = base + i < mapping.cornerSource.size() ? mapping.cornerSource[base + i]
                                                                      : kInvalidId;
            whole = corner >= oldBase[source] && corner < oldBase[source + 1];
            if (whole) {
                ordinals.push_back(corner - oldBase[source]);
                renames.push_back(
                    {oldFaces.cornerVertex[corner], mapping.faces.cornerVertex[base + i]});
            }
        }
        if (!whole) {
            continue;
        }
        const bool forward = ordinals[1] == (ordinals[0] + 1) % valence;
        const bool backward = ordinals[0] == (ordinals[1] + 1) % valence;
        if (!forward && !backward) {
            continue;
        }
        mapped.clear();
        for (const u32 vertex : old.row(source)) {
            const auto found = std::find_if(renames.begin(), renames.end(),
                                            [&](const auto& pair) { return pair.first == vertex; });
            if (found == renames.end()) {
                whole = false;
                break;
            }
            mapped.push_back(found->second);
        }
        if (!whole) {
            continue;
        }
        if (backward) {
            for (std::size_t t = 0; t + 2 < mapped.size(); t += 3) {
                std::swap(mapped[t + 1], mapped[t + 2]);
            }
        }
        builder.set(static_cast<u32>(k), mapped);
    }
    return builder.build();
}

} // namespace

RebuildResult rebuild(Mesh& mesh, RebuildMapping mapping, const std::vector<u32>& oldCornerHalfedge) {
    RebuildResult result;
    AttributeSet old = mesh.attributes;
    SkinBinding oldSkin = mesh.skin;
    const FaceTriangulation oldRows = mesh.triangulation;
    const FaceSet oldFaces = oldRows.empty() ? FaceSet{} : mesh.faceSet();

    // The old topology is needed for the Edge-layer carry below, so take a copy
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

        result.repair = Repair(mapping.faces, sections,
                               mapping.mayHaveZeroArea ? std::span<const Vector3f>{}
                                                       : std::span<const Vector3f>(positions));
        if (result.repair.changed) {
            const RepairResult& repaired = result.repair;
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

            mapping.faces = repaired.faces;
            mapping.vertexSource = std::move(vertexSource);
            mapping.faceSource = std::move(faceSource);
            mapping.cornerSource = std::move(cornerSource);
            // The import-time log described a face set that no longer exists, so
            // `Unrepair` cannot reach the source through it any more. Replacing
            // it is the honest record of what the mesh is now.
            mesh.repairLog = repaired.log;
        }
    }

    FaceTriangulation rows = carryRows(oldRows, oldFaces, mapping);
    mesh.attributes.clear();
    mesh.setFaceSet(std::move(mapping.faces));
    mesh.triangulation = std::move(rows);

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
        return result;
    }
    result.ok = true;

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

    // Edge layers, by halfedge correspondence. A new edge's side runs from
    // corner k to corner k+1 of its face; its old edge is the one between those
    // corners' sources, whichever way round the old face ran them (a face
    // UnifyWinding reversed runs them backwards). A side with no such pair --
    // a new diagonal, or a corner nothing made -- has no old edge.
    if (!hadConnectivity) {
        return result;
    }
    std::vector<std::array<u32, 2>> sidesOf(topology.edgeCount(), {kInvalidId, kInvalidId});
    {
        std::size_t base = 0;
        std::size_t k = 0;
        for (u32 f = 0; f < topology.faceCount(); ++f) {
            const u32 valence = topology.valence(FaceId(f));
            for (u32 i = 0; i < valence; ++i, ++k) {
                const std::size_t next = base + (i + 1) % valence;
                if (k >= mapping.cornerSource.size() || next >= mapping.cornerSource.size()) {
                    continue;
                }
                const u32 a = mapping.cornerSource[k];
                const u32 b = mapping.cornerSource[next];
                if (a >= oldCornerHalfedge.size() || b >= oldCornerHalfedge.size()) {
                    continue;
                }
                const HalfedgeId ha(oldCornerHalfedge[a]);
                const HalfedgeId hb(oldCornerHalfedge[b]);
                u32 oldEdge = kInvalidId;
                if (oldTopology.next(ha) == hb) {
                    oldEdge = static_cast<u32>(Topology::edge(ha).index());
                } else if (oldTopology.next(hb) == ha) {
                    oldEdge = static_cast<u32>(Topology::edge(hb).index());
                }
                const std::size_t e = Topology::edge(HalfedgeId(newCornerHalfedge[k])).index();
                std::array<u32, 2>& sides = sidesOf[e];
                if (sides[0] == kInvalidId) {
                    sides[0] = oldEdge;
                } else if (sides[0] != oldEdge) {
                    sides[1] = oldEdge;
                }
            }
            base += valence;
        }
    }
    for (u32 e = 0; e < sidesOf.size(); ++e) {
        std::array<u32, 2>& sides = sidesOf[e];
        if (sides[1] != kInvalidId && sides[0] != kInvalidId) {
            result.closedEdges.push_back(e);
            if (sides[1] < sides[0]) {
                std::swap(sides[0], sides[1]);
            }
        } else if (sides[0] == kInvalidId) {
            sides[0] = sides[1];
            sides[1] = kInvalidId;
        }
    }
    for (const AttrLayer& layer : old.layers()) {
        if (layer.domain != Domain::Edge) {
            continue;
        }
        AttrLayer& target = *mesh.attributes.layer(layer.name, Domain::Edge);
        const std::size_t stride = AttrTypeSize(layer.type);
        const auto valueAt = [&](u32 edge) -> const u8* {
            const std::size_t at = stride * edge;
            return edge != kInvalidId && at + stride <= layer.data.size() ? layer.data.data() + at
                                                                          : nullptr;
        };
        for (u32 e = 0; e < sidesOf.size(); ++e) {
            const u8* first = valueAt(sidesOf[e][0]);
            const u8* second = valueAt(sidesOf[e][1]);
            const std::size_t offTo = stride * e;
            if (first == nullptr || offTo + stride > target.data.size()) {
                continue;
            }
            u8* out = target.data.data() + offTo;
            std::memcpy(out, first, stride);
            if (second == nullptr) {
                continue;
            }
            // A weld closed this edge: its two old sides combine by type.
            if (layer.type == AttrType::Bool) {
                out[0] = (first[0] != 0 || second[0] != 0) ? 1 : 0;
            } else if (layer.type == AttrType::F32) {
                f32 a = 0.0f;
                f32 b = 0.0f;
                std::memcpy(&a, first, sizeof(f32));
                std::memcpy(&b, second, sizeof(f32));
                const f32 larger = std::max(a, b);
                std::memcpy(out, &larger, sizeof(f32));
            }
        }
    }
    return result;
}

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

} // namespace detail

using detail::identityMapping;
using detail::RebuildMapping;
using detail::rebuild;
using detail::snapshotCorners;
using detail::snapshotCornersBuilt;

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

    // Each loop's two corners at a and b, in that order, captured before the
    // relink moves them: both loops blend one ordered pair with one set of
    // weights, so a continuous edge splits to bit-equal corners.
    const std::array<f32, 2> weights{1.0f - t, t};
    const std::array<HalfedgeId, 2> cornersA{h0, n0}; // f0: at a, at b
    const std::array<HalfedgeId, 2> cornersB{n1, h1}; // f1: at a, at b
    const SourcePolygon loopA = CaptureCorners(mesh, cornersA);
    const SourcePolygon loopB = CaptureCorners(mesh, cornersB);

    const u32 group = FreshMergeGroup(mesh);
    const VertexId v = allocVertex(mesh, static_cast<u32>(a.index()));
    {
        const std::array<u32, 2> ends{static_cast<u32>(a.index()), static_cast<u32>(b.index())};
        BlendVertex(mesh, ends, weights, static_cast<u32>(v.index()));
        const std::span<Vector3f> positions =
            mesh.attributes.get<Vector3f>(names::kPosition, Domain::Vertex);
        if (v.index() < positions.size() && a.index() < positions.size() &&
            b.index() < positions.size()) {
            const Vector3f& pa = positions[a.index()];
            const Vector3f& pb = positions[b.index()];
            positions[v.index()] = Vector3f{pa.x + (pb.x - pa.x) * t, pa.y + (pb.y - pa.y) * t,
                                            pa.z + (pb.z - pa.z) * t};
        }
        const std::span<u32> groups = mesh.attributes.get<u32>(names::kMergeGroup, Domain::Vertex);
        if (v.index() < groups.size()) {
            groups[v.index()] = group;
        }
    }

    const HalfedgeId g0 = allocEdge(mesh); // v -> b, joins h0's loop
    const HalfedgeId g1 = Topology::opposite(g0);
    // Both halves lie along the old edge, so both keep its `sharp`, `crease`
    // and `seam` (EDIT_MODE_MODELLING_DESIGN.md §3.12).
    mesh.attributes.copyElement(Domain::Edge, edge.value(), Topology::edge(g0).value());

    topology.setTo(h0, v); // h0: a -> v, so h1 is v -> a
    topology.setTo(g0, b); // g0: v -> b
    topology.setTo(g1, v); // g1: b -> v

    topology.setNext(g0, n0);
    topology.setNext(h0, g0);
    topology.setFace(g0, f0);

    topology.setNext(p1, g1);
    topology.setNext(g1, h1);
    topology.setFace(g1, f1);

    // h1's loop: g1 now leaves b, so it takes the corner h1 had there; the
    // corner at v (h1, now v -> a) is the blend. h0's loop: g0 leaves v.
    copyHalfedgeAttrs(mesh.attributes, static_cast<u32>(h1.index()), static_cast<u32>(g1.index()));
    const std::array<HalfedgeId, 1> atA{g0};
    BlendCorners(mesh, loopA, weights, atA);
    if (f1.valid()) {
        const std::array<HalfedgeId, 1> atB{h1};
        BlendCorners(mesh, loopB, weights, atB);
    }

    if (f0.valid()) {
        topology.setFaceHalfedge(f0, h0);
    }
    if (f1.valid()) {
        topology.setFaceHalfedge(f1, h1);
    }
    clearRow(mesh, f0);
    clearRow(mesh, f1);
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
    // Joined elsewhere already (a cap over two triangles, split along the
    // diagonal they share): a second a-b edge is a duplicate no rebuild takes.
    if (topology.findHalfedge(va, vb).valid()) {
        return FaceId();
    }

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
    clearRow(mesh, face);

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
    const Mesh& readable = mesh;
    if (readable.topology().valence(face) < 4) {
        return 0;
    }
    // The face is cut along exactly the diagonals it is drawn with, so the
    // triangles it becomes are the ones it showed.
    const std::vector<u32> loop = loopOf(readable.topology(), face);
    const u32 valence = static_cast<u32>(loop.size());
    std::vector<u32> cut;
    TriangulateFace(loop, readable.attributes.get<const Vector3f>(names::kPosition, Domain::Vertex),
                    readable.triangulation.row(face.value()), cut);
    std::vector<std::pair<u32, u32>> diagonals;
    for (std::size_t t = 0; t + 2 < cut.size(); t += 3) {
        for (u32 side = 0; side < 3; ++side) {
            const u32 a = cut[t + side];
            const u32 b = cut[t + (side + 1) % 3];
            if (a < b && b != a + 1 && !(a == 0 && b == valence - 1)) {
                diagonals.push_back({a, b});
            }
        }
    }
    // Each diagonal lies inside one piece: the one holding both its ends.
    std::vector<FaceId> pieces{face};
    u32 added = 0;
    for (const auto& [a, b] : diagonals) {
        HalfedgeId at;
        HalfedgeId to;
        for (std::size_t p = 0; p < pieces.size() && !to.valid(); ++p) {
            at = HalfedgeId();
            for (const HalfedgeId h : readable.topology().fh(pieces[p])) {
                if (readable.topology().from(h).value() == loop[a]) {
                    at = h;
                }
            }
            for (const HalfedgeId h : readable.topology().fh(pieces[p])) {
                if (at.valid() && readable.topology().from(h).value() == loop[b]) {
                    to = h;
                }
            }
        }
        const FaceId piece = to.valid() ? SplitFaceAt(mesh, at, to) : FaceId();
        if (piece.valid()) {
            pieces.push_back(piece);
            ++added;
        }
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

    // The link condition for polygons: the one-rings of v0 and v1 meet exactly
    // at the apexes of the TRIANGULAR side faces. A quad or an n-gon beside the
    // edge has no apex -- it just loses a corner -- so counting one per side
    // face refused every collapse next to a quad.
    std::vector<VertexId> apexes;
    for (const HalfedgeId side : {h, o}) {
        if (topology.face(side).valid() && loopLength(topology, side) == 3) {
            apexes.push_back(topology.to(topology.next(side)));
        }
    }
    std::vector<VertexId> ring0;
    for (VertexId n : topology.vv(v0)) {
        ring0.push_back(n);
    }
    std::vector<VertexId> shared;
    for (VertexId n : topology.vv(v1)) {
        if (std::find(ring0.begin(), ring0.end(), n) != ring0.end() &&
            std::find(shared.begin(), shared.end(), n) == shared.end()) {
            shared.push_back(n);
        }
    }
    std::sort(apexes.begin(), apexes.end());
    std::sort(shared.begin(), shared.end());
    if (shared != apexes) {
        return false;
    }
    // No face but the two side faces may hold both ends: a hexagon
    // (v0, p, r, v1, s, t) passes the ring test and would repeat a vertex.
    const FaceId sideA = topology.face(h);
    const FaceId sideB = topology.face(o);
    for (const FaceId f : topology.vf(v0)) {
        if (!f.valid() || f == sideA || f == sideB) {
            continue;
        }
        for (const VertexId v : topology.fv(f)) {
            if (v == v1) {
                return false;
            }
        }
    }
    return true;
}

bool CollapseEdge(Mesh& mesh, HalfedgeId h) {
    if (!IsCollapseLegal(mesh, h)) {
        return false;
    }
    {
        // Rows first, while the loops are whole: a side face longer than a
        // triangle loses a corner; every other face around v0 only renames it.
        const Topology& before = std::as_const(mesh).topology();
        const HalfedgeId other = Topology::opposite(h);
        const std::array<u32, 1> from{before.from(h).value()};
        const std::array<u32, 1> to{before.to(h).value()};
        std::vector<FaceId> around;
        for (const FaceId f : before.vf(before.from(h))) {
            around.push_back(f);
        }
        for (const FaceId f : around) {
            if (!f.valid()) {
                continue;
            }
            if (f == before.face(h) || f == before.face(other)) {
                clearRow(mesh, f);
            } else {
                mesh.triangulation.rewriteRow(f.value(), from, to);
            }
        }
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
    // v1's own fan, before the splice: where a live outgoing is found when the
    // one it had goes (below).
    std::vector<HalfedgeId> leaving;
    for (HalfedgeId oh : topology.voh(v1)) {
        leaving.push_back(oh);
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
            leaving.push_back(keep);
        } else {
            topology.setNext(sidePrev, sideNext);
            if (face.valid() && topology.halfedge(face) == side) {
                topology.setFaceHalfedge(face, sideNext);
            }
        }
    };
    handleSide(h);
    handleSide(o);

    // Any surviving halfedge leaving v1 will do; `adjustOutgoing` then prefers
    // a boundary one. The candidates are v0's fan and v1's own, and each
    // spliced triangle's surviving side: when v0 had valence 2 and its triangle
    // was spliced away, everything v0 brought is deleted, and only `keep` is
    // left to point at.
    const auto live = [&](HalfedgeId candidate) {
        return candidate.valid() && !topology.isDeleted(candidate) && candidate != h &&
               candidate != o && topology.from(candidate) == v1;
    };
    if (!live(topology.outgoing(v1))) {
        HalfedgeId replacement;
        for (HalfedgeId ih : incoming) {
            if (live(Topology::opposite(ih))) {
                replacement = Topology::opposite(ih);
                break;
            }
        }
        for (std::size_t i = 0; !replacement.valid() && i < leaving.size(); ++i) {
            if (live(leaving[i])) {
                replacement = leaving[i];
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

detail::Dissolved detail::DissolveEdgeDeferred(Mesh& mesh, EdgeId edge, FaceId survivor) {
    Dissolved out;
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        return out;
    }
    {
        // Every refusal asked of a const view, so nothing is marked edited.
        const Topology& topology = std::as_const(mesh).topology();
        if (edge.index() >= topology.edgeCount() || topology.isDeleted(edge) ||
            topology.isBoundary(edge)) {
            return out;
        }
        const FaceId fa = topology.face(Topology::halfedge(edge, 0));
        const FaceId fb = topology.face(Topology::halfedge(edge, 1));
        if (fa == fb || (survivor.valid() && survivor != fa && survivor != fb)) {
            return out;
        }
        const std::span<const u32> sections = std::as_const(mesh).faceSections();
        if (fa.index() < sections.size() && fb.index() < sections.size() &&
            sections[fa.index()] != sections[fb.index()]) {
            return out;
        }
        // The merged loop is both loops less the edge: it repeats a vertex
        // whenever the two faces share one besides the edge's ends.
        std::vector<VertexId> merged;
        for (const VertexId v : topology.fv(fa)) {
            merged.push_back(v);
        }
        u32 shared = 0;
        for (const VertexId v : topology.fv(fb)) {
            if (std::find(merged.begin(), merged.end(), v) != merged.end()) {
                ++shared;
            } else {
                merged.push_back(v);
            }
        }
        if (shared != 2) {
            return out;
        }
        // A face with these corners already there would be its duplicate.
        std::sort(merged.begin(), merged.end());
        const VertexId anchor = topology.from(Topology::halfedge(edge, 0));
        for (const FaceId g : topology.vf(anchor)) {
            if (!g.valid() || g == fa || g == fb || topology.valence(g) != merged.size()) {
                continue;
            }
            std::vector<VertexId> corners;
            for (const VertexId v : topology.fv(g)) {
                corners.push_back(v);
            }
            std::sort(corners.begin(), corners.end());
            if (corners == merged) {
                return out;
            }
        }
    }

    // The merged face is drawn as both faces were, so the file's diagonal is
    // kept: the union of their triangles, lower-numbered face first (§2.3).
    std::vector<u32> joined;
    {
        const Topology& before = std::as_const(mesh).topology();
        FaceId first = before.face(Topology::halfedge(edge, 0));
        FaceId second = before.face(Topology::halfedge(edge, 1));
        if (second.value() < first.value()) {
            std::swap(first, second);
        }
        joined = drawnTriangles(mesh, first);
        const std::vector<u32> rest = drawnTriangles(mesh, second);
        joined.insert(joined.end(), rest.begin(), rest.end());
    }

    Topology& topology = mesh.topology();
    // The survivor's side is `h0`: its loop is the one kept.
    const u32 keepSide = survivor.valid() && topology.face(Topology::halfedge(edge, 1)) == survivor
                             ? 1u
                             : 0u;
    const HalfedgeId h0 = Topology::halfedge(edge, keepSide);
    const HalfedgeId h1 = Topology::halfedge(edge, keepSide ^ 1u);
    const FaceId f0 = topology.face(h0);
    const FaceId f1 = topology.face(h1);
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
    out.ok = true;
    out.kept = f0;
    out.gone = f1;
    out.joined = std::move(joined);

    if (topology.outgoing(a) == h0) {
        topology.setOutgoing(a, n1);
    }
    if (topology.outgoing(b) == h1) {
        topology.setOutgoing(b, n0);
    }
    topology.adjustOutgoing(a);
    topology.adjustOutgoing(b);
    return out;
}

bool DissolveEdge(Mesh& mesh, EdgeId edge, FaceId survivor) {
    detail::Dissolved dissolved = detail::DissolveEdgeDeferred(mesh, edge, survivor);
    if (!dissolved.ok) {
        return false;
    }
    clearRow(mesh, dissolved.gone);
    mesh.triangulation.setRow(dissolved.kept.value(), dissolved.joined,
                              std::as_const(mesh).topology().faceCount());
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
    // A face beside it losing its third corner, or an a-b edge already there:
    // either leaves a degenerate face or a duplicate edge behind.
    for (const HalfedgeId side : {oa, ha}) {
        const FaceId face = topology.face(side);
        if (face.valid() && topology.valence(face) <= 3) {
            return false;
        }
    }
    if (topology.findHalfedge(topology.to(ha), b).valid()) {
        return false;
    }

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

    // The merged edge runs along both old ones: it keeps either's flag and the
    // larger crease, as a weld's closed edge does.
    combineEdgeInto(mesh.attributes, Topology::edge(hb).value(), Topology::edge(ha).value());
    clearRow(mesh, faceA);
    clearRow(mesh, faceB);

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
// WeldVertices
// ============================================================================

WeldResult WeldVertices(Mesh& mesh, f32 epsilon, bool respectMergeGroups) {
    WeldResult result;
    // Connectivity where the face set builds, so the corners and edges can be
    // carried; compacted, so a face-set index is a slot and no Face layer is
    // misread (§2.1). A face set that does not build is welded as it stands.
    if (mesh.hasConnectivity() || mesh.ensureConnectivity().ok()) {
        GarbageCollect(mesh);
    }
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
    GarbageCollect(mesh);
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
    // As WeldVertices: a face set wound against itself does not build, and is
    // exactly what this op is for.
    if (mesh.hasConnectivity() || mesh.ensureConnectivity().ok()) {
        GarbageCollect(mesh);
    }
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

namespace {

/// @p mesh itself when it has connectivity and nothing lazily deleted, else a
/// copy made so into @p store: a face-set index is then a slot, and its corners
/// and edges can be read (C26). With @p prepare, an unmodelled input is a
/// prepared copy (EDIT_MODE_MODELLING_DESIGN.md §2.7 item 4), so a merged mesh
/// is all file or all prepared, whoever calls it.
const Mesh& preparedInput(const Mesh& mesh, std::vector<Mesh>& store, bool prepare = false) {
    if (prepare && !IsModelled(mesh)) {
        Mesh& copy = store.emplace_back(mesh);
        PrepareForModelling(copy);
        if (copy.hasConnectivity() || copy.ensureConnectivity().ok()) {
            GarbageCollect(copy);
        }
        return copy;
    }
    if (mesh.hasConnectivity() && !mesh.topology().hasDeleted()) {
        return mesh;
    }
    Mesh& copy = store.emplace_back(mesh);
    if (copy.hasConnectivity() || copy.ensureConnectivity().ok()) {
        GarbageCollect(copy);
    }
    return copy;
}

} // namespace

Mesh MergeMeshes(std::span<const Mesh> inputs) {
    // Stable addresses: `meshes` points into `store`.
    std::vector<Mesh> store;
    store.reserve(inputs.size());
    std::vector<const Mesh*> prepared;
    bool anyModelled = false;
    for (const Mesh& input : inputs) {
        anyModelled = anyModelled || IsModelled(input);
    }
    for (const Mesh& input : inputs) {
        prepared.push_back(&preparedInput(input, store, anyModelled));
    }
    const auto meshAt = [&](std::size_t i) -> const Mesh& { return *prepared[i]; };
    const std::size_t meshCount = prepared.size();

    Mesh out;
    FaceSet faces;
    u32 vertexBase = 0;
    u32 sectionBase = 0;
    u32 groupBase = 0;

    // Declare the union of every input's layers first, so a layer only some
    // inputs carry still lands with zeroes where it was absent.
    for (std::size_t m = 0; m < meshCount; ++m) {
        for (const AttrLayer& layer : meshAt(m).attributes.layers()) {
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
    snapshots.reserve(meshCount);

    for (std::size_t m = 0; m < meshCount; ++m) {
        const Mesh& mesh = meshAt(m);
        snapshots.push_back(snapshotCorners(mesh));
        sectionOffsets.push_back(sectionBase);
        groupOffsets.push_back(groupBase);
        const FaceSet& source = mesh.faceSet();
        const u32 owner = static_cast<u32>(m);
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
    const u32 outFaces = static_cast<u32>(faces.faceCount());
    out.setFaceSet(std::move(faces));
    {
        // Rows concatenate, each input's vertex ids moved by its base.
        FaceTriangulationBuilder rows(outFaces);
        bool any = false;
        std::vector<u32> moved;
        u32 base = 0;
        u32 face = 0;
        for (std::size_t m = 0; m < meshCount; ++m) {
            const Mesh& mesh = meshAt(m);
            const u32 count = static_cast<u32>(mesh.faceSet().faceCount());
            for (u32 f = 0; f < count; ++f, ++face) {
                moved.clear();
                for (const u32 v : mesh.triangulation.row(f)) {
                    moved.push_back(v + base);
                }
                any = any || !moved.empty();
                rows.set(face, moved);
            }
            base += mesh.faceSet().vertexCount;
        }
        if (any) {
            out.triangulation = rows.build();
        }
    }

    const auto copyDomain = [&](Domain domain, const std::vector<u32>& source,
                                const std::vector<u32>& owner) {
        for (const AttrLayer& layer : out.attributes.layers()) {
            if (layer.domain != domain) {
                continue;
            }
            AttrLayer& target = *out.attributes.layer(layer.name, domain);
            const std::size_t stride = AttrTypeSize(target.type);
            for (std::size_t i = 0; i < source.size(); ++i) {
                const AttrLayer* from = meshAt(owner[i]).attributes.layer(layer.name, domain);
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
                meshAt(vertexOwner[i]).attributes.has(names::kMergeGroup, Domain::Vertex);
            groupValues[i] =
                (authored ? groupValues[i] : vertexSource[i]) + groupOffsets[vertexOwner[i]];
        }
    }

    // Left default-constructed when no input carries a skin: an *empty* binding
    // is legal, one sized for zero vertices in a mesh that has some is not.
    bool anySkin = false;
    for (std::size_t m = 0; m < meshCount; ++m) {
        anySkin = anySkin || !meshAt(m).skin.empty();
    }
    if (anySkin) {
        out.skin.reset(0);
        for (std::size_t i = 0; i < vertexSource.size(); ++i) {
            const SkinBinding& source = meshAt(vertexOwner[i]).skin;
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
                const AttributeSet& from = meshAt(cornerOwner[corner]).attributes;
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

    // Edge layers, by endpoint pair inside the input the edge came from: the
    // inputs share no vertex, so both ends name one input.
    for (const AttrLayer& layer : out.attributes.layers()) {
        if (layer.domain != Domain::Edge) {
            continue;
        }
        AttrLayer& target = *out.attributes.layer(layer.name, Domain::Edge);
        const std::size_t stride = AttrTypeSize(target.type);
        for (u32 e = 0; e < topology.edgeCount(); ++e) {
            const HalfedgeId h = Topology::halfedge(EdgeId(e), 0);
            const u32 a = static_cast<u32>(topology.from(h).index());
            const u32 b = static_cast<u32>(topology.to(h).index());
            if (a >= vertexOwner.size() || b >= vertexOwner.size() ||
                vertexOwner[a] != vertexOwner[b]) {
                continue;
            }
            const Mesh& source = meshAt(vertexOwner[a]);
            const AttrLayer* from = source.attributes.layer(layer.name, Domain::Edge);
            if (from == nullptr || !source.hasConnectivity()) {
                continue;
            }
            const HalfedgeId found =
                source.topology().findHalfedge(VertexId(vertexSource[a]), VertexId(vertexSource[b]));
            if (!found.valid()) {
                continue;
            }
            const std::size_t offFrom = stride * Topology::edge(found).index();
            const std::size_t offTo = stride * e;
            if (offFrom + stride <= from->data.size() && offTo + stride <= target.data.size()) {
                std::memcpy(target.data.data() + offTo, from->data.data() + offFrom, stride);
            }
        }
    }

    // The Mesh domain: the first input's value that has the layer, except
    // `modelled`, which a merge keeps only when every input carries it.
    for (const AttrLayer& layer : out.attributes.layers()) {
        if (layer.domain != Domain::Mesh) {
            continue;
        }
        AttrLayer& target = *out.attributes.layer(layer.name, Domain::Mesh);
        if (layer.name == names::kModelled) {
            bool all = meshCount > 0;
            for (std::size_t m = 0; m < meshCount; ++m) {
                const std::span<const u8> value =
                    meshAt(m).attributes.get<const u8>(names::kModelled, Domain::Mesh);
                all = all && !value.empty() && value[0] != 0;
            }
            if (!target.data.empty()) {
                target.data[0] = all ? 1 : 0;
            }
            continue;
        }
        for (std::size_t m = 0; m < meshCount; ++m) {
            const AttrLayer* from = meshAt(m).attributes.layer(layer.name, Domain::Mesh);
            if (from != nullptr && from->data.size() == target.data.size()) {
                target.data = from->data;
                break;
            }
        }
    }
    out.recomputeBounds();
    return out;
}

std::vector<Mesh> SplitMesh(const Mesh& input) {
    std::vector<Mesh> out;
    std::vector<Mesh> store;
    const Mesh& mesh = preparedInput(input, store);
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

std::vector<u32> MergeSections(Mesh& mesh, std::span<const u32> sections, u32 keep) {
    const u32 count = static_cast<u32>(mesh.sections.size());
    if (keep >= count) {
        return {};
    }
    std::vector<u8> merging(count, 0);
    merging[keep] = 1;
    for (const u32 s : sections) {
        if (s >= count) {
            return {};
        }
        merging[s] = 1;
    }
    // "No gate" is stored as well as omitted; the two mean the same.
    const auto gateOf = [](const MeshSection& section) -> std::optional<i64> {
        const NativeBag::Entry* entry = section.native.find(kSectionVisibilityNode);
        if (entry == nullptr || entry->value == kSectionAlwaysDrawn) {
            return std::nullopt;
        }
        return entry->value;
    };
    const MeshSection& kept = mesh.sections[keep];
    for (u32 s = 0; s < count; ++s) {
        if (merging[s] != 0 && (mesh.sections[s].rigidNode != kept.rigidNode ||
                                gateOf(mesh.sections[s]) != gateOf(kept))) {
            return {};
        }
    }

    std::vector<u32> remap(count, kInvalidId);
    for (u32 s = 0, next = 0; s < count; ++s) {
        if (merging[s] == 0 || s == keep) {
            remap[s] = next++;
        }
    }
    const u32 into = remap[keep];
    const auto target = [&](u32 s) {
        return s < count ? (merging[s] != 0 ? into : remap[s]) : s;
    };
    for (u32& s : mesh.faceSections()) {
        s = target(s);
    }
    for (FaceRecord& record : mesh.repairLog.droppedFaces) {
        record.section = target(record.section);
    }
    for (u32 s = count; s-- > 0;) {
        if (merging[s] != 0 && s != keep) {
            mesh.sections.erase(mesh.sections.begin() + s);
        }
    }
    mesh.recomputeBounds();
    return remap;
}

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
    // The UV-derived bitangent, kept only for its side: a mirrored island's
    // points against cross(normal, tangent), and that is the handedness.
    std::vector<Vector3f> faceBitangents(topology.faceCount(), Vector3f{0.0f, 0.0f, 0.0f});
    // A triangle's is its own; a polygon's the UV-area-weighted mean over the
    // triangles it is drawn as, never a fan that can cross a reflex corner.
    for (u32 f = 0; f < topology.faceCount(); ++f) {
        if (topology.isDeleted(FaceId(f))) {
            continue;
        }
        const std::vector<HalfedgeId> corners = drawnCorners(mesh, positions, FaceId(f));
        const bool triangle = corners.size() == 3;
        Vector3f tangent{0.0f, 0.0f, 0.0f};
        Vector3f bitangent{0.0f, 0.0f, 0.0f};
        f32 weight = 0.0f;
        for (std::size_t t = 0; t + 2 < corners.size(); t += 3) {
            const HalfedgeId h0 = corners[t];
            const HalfedgeId h1 = corners[t + 1];
            const HalfedgeId h2 = corners[t + 2];
            const std::size_t i0 = topology.from(h0).index();
            const std::size_t i1 = topology.from(h1).index();
            const std::size_t i2 = topology.from(h2).index();
            if (i0 >= positions.size() || i1 >= positions.size() || i2 >= positions.size() ||
                h0.index() >= uvs.size() || h1.index() >= uvs.size() || h2.index() >= uvs.size()) {
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
            // T/det per triangle, weighted by |det|: T·sign summed, over Σ|det|.
            const f32 scale = triangle ? 1.0f / determinant : (determinant < 0.0f ? -1.0f : 1.0f);
            tangent.x += (e1.x * dv2 - e2.x * dv1) * scale;
            tangent.y += (e1.y * dv2 - e2.y * dv1) * scale;
            tangent.z += (e1.z * dv2 - e2.z * dv1) * scale;
            bitangent.x += (e2.x * du1 - e1.x * du2) * scale;
            bitangent.y += (e2.y * du1 - e1.y * du2) * scale;
            bitangent.z += (e2.z * du1 - e1.z * du2) * scale;
            weight += triangle ? 1.0f : std::fabs(determinant);
        }
        if (weight <= 0.0f) {
            continue;
        }
        const f32 inverse = triangle ? 1.0f : 1.0f / weight;
        faceTangents[f] = Vector3f{tangent.x * inverse, tangent.y * inverse, tangent.z * inverse};
        faceBitangents[f] =
            Vector3f{bitangent.x * inverse, bitangent.y * inverse, bitangent.z * inverse};
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
            Vector3f bitangent{0.0f, 0.0f, 0.0f};
            for (HalfedgeId h : group) {
                const std::size_t f = topology.face(h).index();
                if (f < faceTangents.size()) {
                    sum.x += faceTangents[f].x;
                    sum.y += faceTangents[f].y;
                    sum.z += faceTangents[f].z;
                    bitangent.x += faceBitangents[f].x;
                    bitangent.y += faceBitangents[f].y;
                    bitangent.z += faceBitangents[f].z;
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
                const Vector3f nxt = whiteout::cross(normal, orthogonal);
                const f32 side = nxt.x * bitangent.x + nxt.y * bitangent.y + nxt.z * bitangent.z;
                tangents[h.index()] =
                    Vector4f{orthogonal.x, orthogonal.y, orthogonal.z, side < 0.0f ? -1.0f : 1.0f};
            }
        }
    }
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

void RecomputeNormals(Mesh& mesh, std::span<const FaceId> faces, f32 angleThreshold) {
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
    const Topology& topology = std::as_const(mesh).topology();
    for (const VertexId v : verticesOf(topology, faces)) {
        for (const std::vector<HalfedgeId>& group : cornerGroups(mesh, faceNormals, v, cosThreshold)) {
            Vector3f sum{0.0f, 0.0f, 0.0f};
            for (const HalfedgeId h : group) {
                const std::size_t f = topology.face(h).index();
                if (f < faceNormals.size()) {
                    sum.x += faceNormals[f].x;
                    sum.y += faceNormals[f].y;
                    sum.z += faceNormals[f].z;
                }
            }
            const Vector3f value = normalized(sum);
            for (const HalfedgeId h : group) {
                if (h.index() < normals.size()) {
                    normals[h.index()] = value;
                }
            }
        }
    }
}

void RecomputeTangents(Mesh& mesh, std::span<const FaceId> faces, u32 uvSet) {
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        return;
    }
    const Mesh& readable = mesh;
    const std::span<const Vector2f> uvs =
        readable.attributes.get<const Vector2f>(names::uv(uvSet), Domain::Halfedge);
    const std::span<const Vector3f> positions =
        readable.attributes.get<const Vector3f>(names::kPosition, Domain::Vertex);
    if (uvs.empty() || positions.empty()) {
        return;
    }
    const Topology& topology = readable.topology();
    const std::vector<Vector3f> faceNormals = allFaceNormals(readable);

    // Per face: its UV-space tangent and bitangent over the triangles it is
    // drawn as, area-weighted, and whether its UV map has any area at all.
    const u32 faceCount = topology.faceCount();
    std::vector<Vector3f> faceTangents(faceCount, Vector3f{0.0f, 0.0f, 0.0f});
    std::vector<Vector3f> faceBitangents(faceCount, Vector3f{0.0f, 0.0f, 0.0f});
    std::vector<u8> mapped(faceCount, 0);
    for (u32 f = 0; f < faceCount; ++f) {
        if (topology.isDeleted(FaceId(f))) {
            continue;
        }
        const std::vector<HalfedgeId> corners = drawnCorners(readable, positions, FaceId(f));
        for (std::size_t t = 0; t + 2 < corners.size(); t += 3) {
            const HalfedgeId h0 = corners[t];
            const HalfedgeId h1 = corners[t + 1];
            const HalfedgeId h2 = corners[t + 2];
            const std::size_t i0 = topology.from(h0).index();
            const std::size_t i1 = topology.from(h1).index();
            const std::size_t i2 = topology.from(h2).index();
            if (i0 >= positions.size() || i1 >= positions.size() || i2 >= positions.size() ||
                h2.index() >= uvs.size() || h1.index() >= uvs.size() || h0.index() >= uvs.size()) {
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
            // Weighted by the triangle's UV area, the determinant's own size.
            const f32 sign = determinant < 0.0f ? -1.0f : 1.0f;
            faceTangents[f].x += (e1.x * dv2 - e2.x * dv1) * sign;
            faceTangents[f].y += (e1.y * dv2 - e2.y * dv1) * sign;
            faceTangents[f].z += (e1.z * dv2 - e2.z * dv1) * sign;
            faceBitangents[f].x += (e2.x * du1 - e1.x * du2) * sign;
            faceBitangents[f].y += (e2.y * du1 - e1.y * du2) * sign;
            faceBitangents[f].z += (e2.z * du1 - e1.z * du2) * sign;
            mapped[f] = 1;
        }
    }
    // A face's handedness: its UV bitangent against cross(normal, tangent).
    const auto handedness = [&](u32 f) -> f32 {
        const Vector3f nxt = whiteout::cross(faceNormals[f], faceTangents[f]);
        return dot3(nxt, faceBitangents[f]) < 0.0f ? -1.0f : 1.0f;
    };

    const std::span<const Vector3f> normals =
        readable.attributes.get<const Vector3f>(names::kNormal, Domain::Halfedge);
    const std::span<Vector4f> tangents =
        mesh.attributes.getOrCreate<Vector4f>(names::kTangent, Domain::Halfedge, AttrType::F32x4);
    const f32 cosThreshold = std::cos(ShadingAngle(readable));
    for (const VertexId v : verticesOf(topology, faces)) {
        for (const std::vector<HalfedgeId>& normalGroup :
             cornerGroups(readable, faceNormals, v, cosThreshold)) {
            // Inside the normal group, one tangent per (UV, handedness) class.
            std::vector<u8> done(normalGroup.size(), 0);
            for (std::size_t i = 0; i < normalGroup.size(); ++i) {
                if (done[i] != 0) {
                    continue;
                }
                const HalfedgeId lead = normalGroup[i];
                const u32 leadFace = static_cast<u32>(topology.face(lead).index());
                const Vector2f leadUv = lead.index() < uvs.size() ? uvs[lead.index()] : Vector2f{};
                const f32 leadSide = mapped[leadFace] != 0 ? handedness(leadFace) : 0.0f;
                std::vector<HalfedgeId> members;
                for (std::size_t j = i; j < normalGroup.size(); ++j) {
                    const HalfedgeId h = normalGroup[j];
                    const u32 f = static_cast<u32>(topology.face(h).index());
                    const Vector2f uv = h.index() < uvs.size() ? uvs[h.index()] : Vector2f{};
                    const f32 side = mapped[f] != 0 ? handedness(f) : 0.0f;
                    // An unmapped face's corner joins whatever shares its UV.
                    const bool sameSide = side == 0.0f || leadSide == 0.0f || side == leadSide;
                    if (done[j] == 0 && uv.x == leadUv.x && uv.y == leadUv.y && sameSide) {
                        done[j] = 1;
                        members.push_back(h);
                    }
                }
                Vector3f sum{0.0f, 0.0f, 0.0f};
                Vector3f bitangent{0.0f, 0.0f, 0.0f};
                for (const HalfedgeId h : members) {
                    const u32 f = static_cast<u32>(topology.face(h).index());
                    if (mapped[f] == 0) {
                        continue;
                    }
                    sum.x += faceTangents[f].x;
                    sum.y += faceTangents[f].y;
                    sum.z += faceTangents[f].z;
                    bitangent.x += faceBitangents[f].x;
                    bitangent.y += faceBitangents[f].y;
                    bitangent.z += faceBitangents[f].z;
                }
                for (const HalfedgeId h : members) {
                    if (h.index() >= tangents.size()) {
                        continue;
                    }
                    const u32 f = static_cast<u32>(topology.face(h).index());
                    const Vector3f normal = h.index() < normals.size() ? normals[h.index()]
                                                                       : faceNormals[f];
                    Vector3f along = sum;
                    f32 w = 1.0f;
                    if (dot3(along, along) <= 1e-30f) {
                        // Nothing mapped here: along the face's first edge.
                        const HalfedgeId first = topology.halfedge(FaceId(f));
                        const Vector3f& a = positions[topology.from(first).index()];
                        const Vector3f& b = positions[topology.to(first).index()];
                        along = Vector3f{b.x - a.x, b.y - a.y, b.z - a.z};
                    } else {
                        const Vector3f nxt = whiteout::cross(normal, normalized(along));
                        w = dot3(nxt, bitangent) < 0.0f ? -1.0f : 1.0f;
                    }
                    const f32 d = dot3(along, normal);
                    const Vector3f orthogonal = normalized(Vector3f{
                        along.x - normal.x * d, along.y - normal.y * d, along.z - normal.z * d});
                    tangents[h.index()] = Vector4f{orthogonal.x, orthogonal.y, orthogonal.z, w};
                }
            }
        }
    }
}

} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
