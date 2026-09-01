// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/geometry/builder.h>

#include <algorithm>
#include <cstring>

namespace whiteout {
namespace models {
namespace wem {
namespace geom {

namespace {

/// The `AttrType` whose element is @p size bytes and which a caller most likely
/// meant. Only used for layers a caller creates implicitly by writing to them;
/// a reserved name overrides this from the table anyway.
AttrType guessType(std::size_t size) {
    switch (size) {
    case 1:
        return AttrType::Bool;
    case 2:
        return AttrType::U16;
    case 4:
        return AttrType::F32;
    case 8:
        return AttrType::F32x2;
    case 12:
        return AttrType::F32x3;
    case 16:
        return AttrType::F32x4;
    default:
        return AttrType::F32;
    }
}

} // namespace

// ============================================================================

VertexId MeshBuilder::addVertex(const Vector3f& position) {
    const u32 index = static_cast<u32>(positions_.size());
    positions_.push_back(position);
    influences_.emplace_back();
    pending_.setDomainCount(Domain::Vertex, index + 1);
    return VertexId(index);
}

void MeshBuilder::reserveVertices(u32 count) {
    positions_.resize(count, Vector3f{0, 0, 0});
    influences_.resize(count);
    pending_.setDomainCount(Domain::Vertex, count);
}

std::span<Vector3f> MeshBuilder::positions() {
    return std::span<Vector3f>(positions_.data(), positions_.size());
}

FaceId MeshBuilder::addFace(std::span<const VertexId> corners, u32 section) {
    const u32 index = static_cast<u32>(faceValence_.size());
    faceCornerBase_.push_back(static_cast<u32>(cornerVertex_.size()));
    faceValence_.push_back(static_cast<u32>(corners.size()));
    faceSection_.push_back(section);
    for (VertexId v : corners) {
        cornerVertex_.push_back(v.value());
    }
    pending_.setDomainCount(Domain::Halfedge, static_cast<u32>(cornerVertex_.size()));
    return FaceId(index);
}

FaceId MeshBuilder::addTriangle(VertexId a, VertexId b, VertexId c, u32 section) {
    const VertexId corners[] = {a, b, c};
    return addFace(std::span<const VertexId>(corners, 3), section);
}

u32 MeshBuilder::cornerIndex(FaceId face, u32 corner) const {
    if (!face.valid() || face.index() >= faceValence_.size()) {
        return kInvalidId;
    }
    if (corner >= faceValence_[face.index()]) {
        return kInvalidId;
    }
    return faceCornerBase_[face.index()] + corner;
}

void MeshBuilder::addInfluence(VertexId vertex, u32 bone, f32 weight) {
    if (!vertex.valid() || vertex.index() >= influences_.size()) {
        return;
    }
    influences_[vertex.index()].push_back(Influence{bone, weight});
}

u32 MeshBuilder::addSection(MeshSection section) {
    const u32 index = static_cast<u32>(sections_.size());
    sections_.push_back(std::move(section));
    return index;
}

AttrLayer& MeshBuilder::layerFor(Domain domain, const std::string& name, std::size_t size) {
    return pending_.create(name, domain, guessType(size));
}

void MeshBuilder::setAttr(Domain domain, u32 element, const std::string& name, const void* value,
                          std::size_t size) {
    AttrLayer& layer = layerFor(domain, name, size);
    const std::size_t stride = AttrTypeSize(layer.type);
    const std::size_t offset = stride * element;
    if (offset + stride > layer.data.size()) {
        return;
    }
    std::memcpy(layer.data.data() + offset, value, std::min(stride, size));
}

// ============================================================================

MeshBuilder::BuildOutcome MeshBuilder::build(RepairPolicy policy) {
    BuildOutcome outcome;

    FaceSet input;
    input.vertexCount = static_cast<u32>(positions_.size());
    input.faceValence = faceValence_;
    input.cornerVertex = cornerVertex_;

    RepairResult repaired =
        Repair(input, std::span<const u32>(faceSection_.data(), faceSection_.size()),
               std::span<const Vector3f>(positions_.data(), positions_.size()));

    if (repaired.changed && policy == RepairPolicy::Refuse) {
        outcome.refused = true;
        outcome.repairStats = repaired.log.stats();
        return outcome;
    }

    // --- expand vertex-domain data across the splits -------------------------
    //
    // A created vertex carries the original's position, attributes and skin —
    // that is what makes `mergeGroup` a real identity and `Unrepair` exact.

    Mesh& mesh = outcome.mesh;
    mesh.sections = std::move(sections_);

    positions_.resize(repaired.faces.vertexCount, Vector3f{0, 0, 0});
    pending_.setDomainCount(Domain::Vertex, repaired.faces.vertexCount);
    for (const VertexSplit& split : repaired.log.splits) {
        if (split.original < positions_.size() && split.created < positions_.size()) {
            positions_[split.created] = positions_[split.original];
            pending_.copyElement(Domain::Vertex, split.original, split.created);
        }
    }

    SkinBinding skin;
    bool anySkin = false;
    for (const auto& list : influences_) {
        if (!list.empty()) {
            anySkin = true;
            break;
        }
    }
    if (anySkin) {
        skin.offsets.assign(1, 0);
        for (const auto& list : influences_) {
            skin.appendVertex(std::span<const Influence>(list.data(), list.size()));
        }
        for (const VertexSplit& split : repaired.log.splits) {
            skin.appendCopyOf(split.original);
        }
        skin.sortByWeight();
    }
    mesh.skin = std::move(skin);

    // --- geometry -------------------------------------------------------------

    mesh.setFaceSet(repaired.faces);
    const BuildResult built = mesh.ensureConnectivity();
    if (!built.ok()) {
        // The repair is supposed to make this impossible; if it ever happens the
        // caller gets an empty mesh rather than one that violates C1-C7.
        outcome.refused = true;
        outcome.repairStats = repaired.log.stats();
        outcome.mesh = Mesh();
        return outcome;
    }

    // --- attributes ------------------------------------------------------------

    auto positionLayer =
        mesh.attributes.getOrCreate<Vector3f>(names::kPosition, Domain::Vertex, AttrType::F32x3);
    for (std::size_t v = 0; v < positionLayer.size() && v < positions_.size(); ++v) {
        positionLayer[v] = positions_[v];
    }

    const std::vector<u32> groups = BuildMergeGroups(repaired.faces.vertexCount, repaired.log);
    auto groupLayer =
        mesh.attributes.getOrCreate<u32>(names::kMergeGroup, Domain::Vertex, AttrType::U32);
    for (std::size_t v = 0; v < groupLayer.size() && v < groups.size(); ++v) {
        groupLayer[v] = groups[v];
    }

    auto sectionLayer = mesh.faceSections();
    for (std::size_t f = 0; f < sectionLayer.size() && f < repaired.sections.size(); ++f) {
        sectionLayer[f] = repaired.sections[f];
    }

    // Corner attributes reach the halfedge their corner became. The output face
    // order is the input order minus the dropped faces, and repair preserves
    // corner ordinals, so the two sides line up by walking both in step.
    std::vector<u32> survivingInputFace;
    survivingInputFace.reserve(repaired.faces.faceCount());
    {
        std::size_t dropped = 0;
        for (u32 f = 0; f < faceValence_.size(); ++f) {
            if (dropped < repaired.log.droppedFaces.size() &&
                repaired.log.droppedFaces[dropped].index == f) {
                ++dropped;
                continue;
            }
            survivingInputFace.push_back(f);
        }
    }

    // Create every halfedge layer up front and address them by index: `create`
    // can reallocate the layer vector, so a reference held across the face loop
    // would dangle.
    struct CornerLayer {
        u32 source;
        u32 target;
        std::size_t stride;
    };
    std::vector<CornerLayer> cornerLayers;
    for (u32 i = 0; i < pending_.layerCount(); ++i) {
        const AttrLayer& source = pending_.layers()[i];
        if (source.domain != Domain::Halfedge) {
            continue;
        }
        mesh.attributes.create(source.name, Domain::Halfedge, source.type, source.storage);
        cornerLayers.push_back(CornerLayer{i, mesh.attributes.find(source.name, Domain::Halfedge),
                                           AttrTypeSize(source.type)});
    }

    const Mesh& readable = mesh;
    const Topology& topology = readable.topology();
    for (std::size_t outFace = 0; outFace < survivingInputFace.size(); ++outFace) {
        const u32 inFace = survivingInputFace[outFace];
        const u32 valence = faceValence_[inFace];
        HalfedgeId h = topology.halfedge(FaceId(static_cast<u32>(outFace)));
        for (u32 i = 0; i < valence; ++i) {
            const u32 corner = faceCornerBase_[inFace] + i;
            for (const CornerLayer& pair : cornerLayers) {
                const AttrLayer& source = pending_.layers()[pair.source];
                AttrLayer& target = *mesh.attributes.layer(source.name, Domain::Halfedge);
                const std::size_t from = pair.stride * corner;
                const std::size_t to = pair.stride * h.index();
                if (from + pair.stride <= source.data.size() &&
                    to + pair.stride <= target.data.size()) {
                    std::memcpy(target.data.data() + to, source.data.data() + from, pair.stride);
                }
            }
            h = topology.next(h);
        }
    }

    // Any other vertex-domain layer the caller wrote comes across as-is.
    for (const AttrLayer& source : pending_.layers()) {
        if (source.domain != Domain::Vertex) {
            continue;
        }
        AttrLayer& target =
            mesh.attributes.create(source.name, Domain::Vertex, source.type, source.storage);
        const std::size_t copy = std::min(source.data.size(), target.data.size());
        std::memcpy(target.data.data(), source.data.data(), copy);
    }

    outcome.repairStats = repaired.log.stats();
    if (policy == RepairPolicy::RepairAndRecord) {
        mesh.repairLog = std::move(repaired.log);
    }
    mesh.recomputeBounds();

    // Leave the builder empty: a builder is for one mesh.
    positions_.clear();
    faceValence_.clear();
    faceCornerBase_.clear();
    cornerVertex_.clear();
    faceSection_.clear();
    influences_.clear();
    pending_.clear();
    return outcome;
}

} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
