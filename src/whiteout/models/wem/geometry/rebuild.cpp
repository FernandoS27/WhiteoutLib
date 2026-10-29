// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "rebuild.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace whiteout {
namespace models {
namespace wem {
namespace geom {

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

} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
