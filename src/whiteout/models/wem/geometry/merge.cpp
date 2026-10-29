// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/geometry/merge.h>
#include <whiteout/models/wem/geometry/kernel.h>
#include <whiteout/models/wem/geometry/prepare.h>

#include "rebuild.h"

#include <algorithm>
#include <optional>

namespace whiteout {
namespace models {
namespace wem {
namespace geom {

using detail::identityMapping;
using detail::RebuildMapping;
using detail::rebuild;
using detail::snapshotCorners;
using detail::snapshotCornersBuilt;

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

} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
