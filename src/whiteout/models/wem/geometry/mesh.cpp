// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/geometry/mesh.h>

#include <cmath>
#include <limits>

namespace whiteout {
namespace models {
namespace wem {

void ResetExtent(Extent& extent) {
    const f32 infinity = std::numeric_limits<f32>::infinity();
    extent.minimum = Vector3f{infinity, infinity, infinity};
    extent.maximum = Vector3f{-infinity, -infinity, -infinity};
    extent.sphereRadius = 0;
}

void GrowExtent(Extent& extent, const Vector3f& point) {
    extent.minimum.x = std::min(extent.minimum.x, point.x);
    extent.minimum.y = std::min(extent.minimum.y, point.y);
    extent.minimum.z = std::min(extent.minimum.z, point.z);
    extent.maximum.x = std::max(extent.maximum.x, point.x);
    extent.maximum.y = std::max(extent.maximum.y, point.y);
    extent.maximum.z = std::max(extent.maximum.z, point.z);
}

void FinishExtent(Extent& extent) {
    if (!extent.valid()) {
        extent.minimum = Vector3f{0, 0, 0};
        extent.maximum = Vector3f{0, 0, 0};
        extent.sphereRadius = 0;
        return;
    }
    const f32 dx = (extent.maximum.x - extent.minimum.x) * 0.5f;
    const f32 dy = (extent.maximum.y - extent.minimum.y) * 0.5f;
    const f32 dz = (extent.maximum.z - extent.minimum.z) * 0.5f;
    extent.sphereRadius = std::sqrt(dx * dx + dy * dy + dz * dz);
}

// ============================================================================
// Mesh
// ============================================================================

void Mesh::syncFaceSet() const {
    if (!facesStale_) {
        return;
    }
    faces_ = topology_.toFaceSet();
    facesStale_ = false;
}

const geom::FaceSet& Mesh::faceSet() const {
    syncFaceSet();
    return faces_;
}

void Mesh::setFaceSet(geom::FaceSet faces) {
    faces_ = std::move(faces);
    facesStale_ = false;
    connectivity_ = false;
    topology_.clear();
    attributes.setDomainCount(geom::Domain::Vertex, faces_.vertexCount);
    attributes.setDomainCount(geom::Domain::Face, static_cast<u32>(faces_.faceCount()));
}

geom::BuildResult Mesh::ensureConnectivity() {
    if (connectivity_) {
        return geom::BuildResult{};
    }
    syncFaceSet();
    const geom::BuildResult result = topology_.build(faces_);
    if (!result.ok()) {
        return result;
    }
    connectivity_ = true;
    facesStale_ = false;
    // Only the build knows how many halfedges and edges there are, so these two
    // domains are sized here and nowhere else.
    attributes.setDomainCount(geom::Domain::Halfedge, topology_.halfedgeCount());
    attributes.setDomainCount(geom::Domain::Edge, topology_.edgeCount());
    return result;
}

void Mesh::invalidateConnectivity() {
    syncFaceSet();
    topology_.clear();
    connectivity_ = false;
}

geom::Topology& Mesh::topology() {
    facesStale_ = true;
    return topology_;
}

u32 Mesh::vertexCount() const {
    return connectivity_ ? topology_.vertexCount() : faces_.vertexCount;
}

u32 Mesh::faceCount() const {
    return connectivity_ ? topology_.faceCount() : static_cast<u32>(faces_.faceCount());
}

std::span<u32> Mesh::faceSections() {
    return attributes.getOrCreate<u32>(geom::names::kSection, geom::Domain::Face,
                                       geom::AttrType::U32);
}

std::span<const u32> Mesh::faceSections() const {
    return attributes.get<const u32>(geom::names::kSection, geom::Domain::Face);
}

std::vector<u32> Mesh::facesOfSection(u32 section) const {
    std::vector<u32> out;
    const auto values = faceSections();
    for (u32 f = 0; f < values.size(); ++f) {
        if (values[f] == section) {
            out.push_back(f);
        }
    }
    return out;
}

void Mesh::recomputeBounds() {
    const auto positions =
        attributes.get<const Vector3f>(geom::names::kPosition, geom::Domain::Vertex);
    ResetExtent(bounds);
    for (MeshSection& section : sections) {
        ResetExtent(section.bounds);
    }
    if (positions.empty()) {
        FinishExtent(bounds);
        for (MeshSection& section : sections) {
            FinishExtent(section.bounds);
        }
        return;
    }

    for (const Vector3f& position : positions) {
        GrowExtent(bounds, position);
    }
    FinishExtent(bounds);

    // Section bounds follow the faces that name the section, which is the only
    // definition that survives editing: a section is a face set, not a range.
    const auto sectionOf = faceSections();
    const geom::FaceSet& faces = faceSet();
    std::size_t cursor = 0;
    for (std::size_t f = 0; f < faces.faceCount(); ++f) {
        const u32 valence = faces.faceValence[f];
        const u32 index = f < sectionOf.size() ? sectionOf[f] : 0;
        if (index < sections.size()) {
            for (u32 i = 0; i < valence; ++i) {
                const u32 vertex = faces.cornerVertex[cursor + i];
                if (vertex < positions.size()) {
                    GrowExtent(sections[index].bounds, positions[vertex]);
                }
            }
        }
        cursor += valence;
    }
    for (MeshSection& section : sections) {
        FinishExtent(section.bounds);
    }
}

} // namespace wem
} // namespace models
} // namespace whiteout
