// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/skinning/deform.h>

#include <algorithm>

namespace whiteout {
namespace models {
namespace wem {
namespace skinning {

Matrix44f BlendSkinMatrix(std::span<const geom::Influence> influences,
                          std::span<const Matrix44f> skin) {
    if (influences.empty()) {
        return Matrix44f::identity();
    }
    Matrix44f sum;
    for (std::size_t r = 0; r < 4; ++r) {
        for (std::size_t c = 0; c < 4; ++c) {
            sum.data[r][c] = 0.0f;
        }
    }
    for (const geom::Influence& influence : influences) {
        const Matrix44f& bone =
            influence.bone < skin.size() ? skin[influence.bone] : Matrix44f::identity();
        for (std::size_t r = 0; r < 4; ++r) {
            for (std::size_t c = 0; c < 4; ++c) {
                sum.data[r][c] += bone.data[r][c] * influence.weight;
            }
        }
    }
    return sum;
}

Vector3f DeformPosition(const Vector3f& rest, std::span<const geom::Influence> influences,
                        std::span<const Matrix44f> skin) {
    const Matrix44f m = BlendSkinMatrix(influences, skin);
    // Row vector, which is how the renderer multiplies.
    return Vector3f{rest.x * m.data[0][0] + rest.y * m.data[1][0] + rest.z * m.data[2][0] +
                        m.data[3][0],
                    rest.x * m.data[0][1] + rest.y * m.data[1][1] + rest.z * m.data[2][1] +
                        m.data[3][1],
                    rest.x * m.data[0][2] + rest.y * m.data[1][2] + rest.z * m.data[2][2] +
                        m.data[3][2]};
}

std::vector<Vector3f> DeformMesh(const Mesh& mesh, std::span<const Matrix44f> skin,
                                 std::span<const std::vector<geom::Influence>> written) {
    const std::span<const Vector3f> positions =
        mesh.attributes.get<Vector3f>(geom::names::kPosition, geom::Domain::Vertex);
    std::vector<Vector3f> out(positions.begin(), positions.end());
    if (skin.empty()) {
        return out;
    }
    const bool useWritten = written.size() == positions.size();

    // A rigid section's node, per vertex, for the vertices that bind nothing:
    // §5.6's explicit rigid case, which never reaches the skin array.
    std::vector<u32> rigidOf;
    if (!useWritten) {
        bool anyRigid = false;
        for (const MeshSection& section : mesh.sections) {
            anyRigid = anyRigid || section.rigidNode.has_value();
        }
        if (anyRigid) {
            rigidOf.assign(positions.size(), kInvalidNode);
            const geom::FaceSet& faces = mesh.faceSet();
            const std::span<const u32> sectionOf = mesh.faceSections();
            std::size_t corner = 0;
            for (std::size_t f = 0; f < faces.faceCount(); ++f) {
                const u32 valence = faces.faceValence[f];
                const u32 section = f < sectionOf.size() ? sectionOf[f] : 0;
                const std::optional<u32> rigid =
                    section < mesh.sections.size() ? mesh.sections[section].rigidNode : std::nullopt;
                if (rigid.has_value()) {
                    for (u32 k = 0; k < valence; ++k) {
                        const u32 vertex = faces.cornerVertex[corner + k];
                        if (vertex < rigidOf.size()) {
                            rigidOf[vertex] = *rigid;
                        }
                    }
                }
                corner += valence;
            }
        }
    }

    std::vector<geom::Influence> scratch;
    for (std::size_t v = 0; v < positions.size(); ++v) {
        std::span<const geom::Influence> influences;
        if (useWritten) {
            influences = written[v];
        } else {
            scratch.clear();
            f32 total = 0.0f;
            if (!mesh.skin.empty() && v < mesh.skin.vertexCount()) {
                for (const geom::Influence& influence :
                     mesh.skin.forVertex(static_cast<u32>(v))) {
                    if (influence.weight > 0.0f) {
                        scratch.push_back(influence);
                        total += influence.weight;
                    }
                }
            }
            if (scratch.empty()) {
                if (!rigidOf.empty() && rigidOf[v] != kInvalidNode) {
                    scratch.push_back({rigidOf[v], 1.0f});
                }
            } else if (total > 0.0f && std::abs(total - 1.0f) > 1e-6f) {
                for (geom::Influence& influence : scratch) {
                    influence.weight /= total;
                }
            }
            influences = scratch;
        }
        out[v] = DeformPosition(positions[v], influences, skin);
    }
    return out;
}

} // namespace skinning
} // namespace wem
} // namespace models
} // namespace whiteout
