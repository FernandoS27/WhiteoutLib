// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file wem_geometry_ingest.h
 * @brief Test-only geometry ingest for the WEM v3 P1/P2 gates.
 *
 * P1's and P2's gates need real meshes, and the real WEM v3 converters do not
 * exist until P5. This runs the existing format parsers and feeds positions,
 * face lists and per-corner attributes straight into `MeshBuilder` — no
 * materials, no nodes, no profiles. It exists to answer two questions the design
 * asks and cannot answer from synthetic meshes:
 *
 * - How expensive is the §5.3 manifold repair on shipped content?
 * - Does the §5.8 identity property hold on shipped content?
 *
 * **This header is deleted at P5**, when the real converters take over. Nothing
 * outside `tests/` may include it.
 */

#include <string>
#include <vector>

#include <whiteout/models/m2/parser.h>
#include <whiteout/models/m3/parser.h>
#include <whiteout/models/mdx/parser.h>
#include <whiteout/models/wem/geometry/builder.h>
#include <whiteout/utils/vertex_buffer.h>

namespace whiteout::test {

/// One ingested mesh plus the source arrays the identity property is checked
/// against.
struct IngestedMesh {
    models::wem::Mesh mesh;
    models::wem::geom::RepairLog::Stats repairStats;

    /// The source's own vertex arrays, in source order — what a GPU buffer built
    /// from this file would hold.
    std::vector<Vector3f> sourcePositions;
    std::vector<Vector3f> sourceNormals;
    std::vector<Vector2f> sourceUv0;
    /// The source's index buffer, mesh-local.
    std::vector<u32> sourceIndices;
    bool allTriangles = true;
};

namespace detail {

/// Feeds one indexed triangle soup into a builder, attaching per-corner normals
/// and UVs so the halfedge layers are populated the way an importer would.
inline IngestedMesh ingestIndexed(const std::vector<Vector3f>& positions,
                                  const std::vector<Vector3f>& normals,
                                  const std::vector<Vector2f>& uv0,
                                  const std::vector<u32>& indices, const std::string& name) {
    using namespace models::wem;

    IngestedMesh out;
    out.sourcePositions = positions;
    out.sourceNormals = normals;
    out.sourceUv0 = uv0;
    out.sourceIndices = indices;
    out.allTriangles = (indices.size() % 3) == 0;

    geom::MeshBuilder builder;
    MeshSection section;
    section.name = name;
    builder.addSection(std::move(section));

    for (const Vector3f& position : positions) {
        builder.addVertex(position);
    }

    const std::size_t triangles = indices.size() / 3;
    for (std::size_t t = 0; t < triangles; ++t) {
        const u32 a = indices[t * 3 + 0];
        const u32 b = indices[t * 3 + 1];
        const u32 c = indices[t * 3 + 2];
        if (a >= positions.size() || b >= positions.size() || c >= positions.size()) {
            continue;
        }
        const auto face =
            builder.addTriangle(geom::VertexId(a), geom::VertexId(b), geom::VertexId(c), 0);
        const u32 corners[3] = {a, b, c};
        for (u32 i = 0; i < 3; ++i) {
            if (corners[i] < normals.size()) {
                builder.setCornerAttr(face, i, geom::names::kNormal, normals[corners[i]]);
            }
            if (corners[i] < uv0.size()) {
                builder.setCornerAttr(face, i, geom::names::uv(0), uv0[corners[i]]);
            }
        }
    }

    auto outcome = builder.build();
    out.mesh = std::move(outcome.mesh);
    out.mesh.name = name;
    out.repairStats = outcome.repairStats;
    return out;
}

} // namespace detail

// ============================================================================
// MDX — one Mesh per Geoset (§5.5's per-profile table)
// ============================================================================

inline std::vector<IngestedMesh> IngestMdx(std::span<const u8> bytes) {
    std::vector<IngestedMesh> out;
    mdx::Parser parser;
    const mdx::Model model = parser.parse(bytes);

    for (std::size_t g = 0; g < model.geosets.size(); ++g) {
        const mdx::Geoset& geoset = model.geosets[g];
        std::vector<u32> indices;
        indices.reserve(geoset.faces.size());
        for (u16 index : geoset.faces) {
            indices.push_back(index);
        }
        std::vector<Vector2f> uv0;
        if (!geoset.textureCoordinateSets.empty()) {
            uv0 = geoset.textureCoordinateSets[0];
        }
        out.push_back(detail::ingestIndexed(geoset.vertexPositions, geoset.vertexNormals, uv0,
                                            indices,
                                            geoset.lodName.empty()
                                                ? "geoset_" + std::to_string(g)
                                                : geoset.lodName));
    }
    return out;
}

// ============================================================================
// M2 — one Mesh per SkinProfile
// ============================================================================
//
// The skin's `vertices` array is an indirection into the model's global vertex
// list, and `indices` addresses *that* array, so the ingest flattens both into a
// mesh-local space. That flattening is exactly what the real converter will do.

inline std::vector<IngestedMesh> IngestM2(const m2::Model& model) {
    std::vector<IngestedMesh> out;
    for (std::size_t s = 0; s < model.skinProfiles.size(); ++s) {
        const m2::SkinProfile& skin = model.skinProfiles[s];
        std::vector<Vector3f> positions;
        std::vector<Vector3f> normals;
        std::vector<Vector2f> uv0;
        positions.reserve(skin.vertices.size());
        normals.reserve(skin.vertices.size());
        uv0.reserve(skin.vertices.size());
        for (u16 global : skin.vertices) {
            if (global < model.vertices.size()) {
                const m2::Vertex& vertex = model.vertices[global];
                positions.push_back(vertex.position);
                normals.push_back(vertex.normal);
                uv0.push_back(vertex.texCoords[0]);
            } else {
                positions.push_back(Vector3f{0, 0, 0});
                normals.push_back(Vector3f{0, 0, 1});
                uv0.push_back(Vector2f{0, 0});
            }
        }
        std::vector<u32> indices;
        indices.reserve(skin.indices.size());
        for (u16 index : skin.indices) {
            indices.push_back(index);
        }
        out.push_back(detail::ingestIndexed(positions, normals, uv0, indices,
                                            "skin_" + std::to_string(s)));
    }
    return out;
}

// ============================================================================
// M3 — one Mesh per MeshDivision
// ============================================================================

inline std::vector<IngestedMesh> IngestM3(const m3::Model& model) {
    std::vector<IngestedMesh> out;

    const std::vector<Vector3f> positions = model.vertices.getPositions();
    const std::vector<Vector3f> normals = model.vertices.getNormals();
    std::vector<Vector2f> uv0;
    if (model.vertices.UVsNum() > 0) {
        uv0 = model.vertices.getUVs(0);
    }

    for (std::size_t d = 0; d < model.divisions.size(); ++d) {
        const auto& division = model.divisions[d];

        // A division's regions select a range of the global buffer; rebase it so
        // the mesh is self-contained, the same as §5.5's "one Mesh per division".
        u32 lowest = 0xFFFFFFFFu;
        u32 highest = 0;
        for (const auto& region : division.regions) {
            lowest = std::min(lowest, region.firstVertex);
            highest = std::max(highest, region.firstVertex + region.vertexCount);
        }
        if (lowest > highest) {
            lowest = 0;
            highest = 0;
        }

        std::vector<Vector3f> localPositions;
        std::vector<Vector3f> localNormals;
        std::vector<Vector2f> localUv0;
        for (u32 v = lowest; v < highest; ++v) {
            localPositions.push_back(v < positions.size() ? positions[v] : Vector3f{0, 0, 0});
            localNormals.push_back(v < normals.size() ? normals[v] : Vector3f{0, 0, 1});
            localUv0.push_back(v < uv0.size() ? uv0[v] : Vector2f{0, 0});
        }

        // A region's face values are **region-local** — indices into that
        // region's own vertex slice, not into the division's buffer. Reading them
        // as global is what a first draft of this ingest did, and it reported the
        // repair splitting 99% of all vertices; the number was the bug, not the
        // content.
        std::vector<u32> indices;
        indices.reserve(division.faces.size());
        for (const auto& region : division.regions) {
            const std::size_t begin = region.firstIndex;
            const std::size_t end = begin + region.indexCount;
            const u32 base = region.firstVertex - lowest;
            for (std::size_t i = begin; i < end && i < division.faces.size(); ++i) {
                indices.push_back(static_cast<u32>(division.faces[i]) + base);
            }
        }

        out.push_back(detail::ingestIndexed(localPositions, localNormals, localUv0, indices,
                                            "division_" + std::to_string(d)));
    }
    return out;
}

} // namespace whiteout::test
