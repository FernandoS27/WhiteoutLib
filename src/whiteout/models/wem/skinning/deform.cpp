// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/skinning/deform.h>

#include <algorithm>
#include <cmath>

#include <whiteout/models/wem/geometry/topology.h>

namespace whiteout {
namespace models {
namespace wem {
namespace skinning {

namespace {

/// What each vertex skins by when no written set is given: its own influences,
/// normalised, because a file that ships weights summing to 0.998 keeps them
/// (WEM_DESIGN §5.6) and the renderer divides; and a vertex of a rigid section
/// that binds nothing follows that section's node whole (§5.6's explicit rigid
/// case, which never reaches the skin array).
class VertexInfluences {
public:
    explicit VertexInfluences(const Mesh& mesh) : mesh_(mesh) {
        bool anyRigid = false;
        for (const MeshSection& section : mesh.sections) {
            anyRigid = anyRigid || section.rigidNode.has_value();
        }
        if (!anyRigid) {
            return;
        }
        const geom::FaceSet& faces = mesh.faceSet();
        rigidOf_.assign(faces.vertexCount, kInvalidNode);
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
                    if (vertex < rigidOf_.size()) {
                        rigidOf_[vertex] = *rigid;
                    }
                }
            }
            corner += valence;
        }
    }

    std::span<const geom::Influence> of(std::size_t v) {
        scratch_.clear();
        f32 total = 0.0f;
        if (!mesh_.skin.empty() && v < mesh_.skin.vertexCount()) {
            for (const geom::Influence& influence : mesh_.skin.forVertex(static_cast<u32>(v))) {
                if (influence.weight > 0.0f) {
                    scratch_.push_back(influence);
                    total += influence.weight;
                }
            }
        }
        if (scratch_.empty()) {
            if (v < rigidOf_.size() && rigidOf_[v] != kInvalidNode) {
                scratch_.push_back({rigidOf_[v], 1.0f});
            }
        } else if (total > 0.0f && std::abs(total - 1.0f) > 1e-6f) {
            for (geom::Influence& influence : scratch_) {
                influence.weight /= total;
            }
        }
        return scratch_;
    }

private:
    const Mesh& mesh_;
    std::vector<u32> rigidOf_;
    std::vector<geom::Influence> scratch_;
};

} // namespace

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
    if (written.size() == positions.size()) {
        for (std::size_t v = 0; v < positions.size(); ++v) {
            out[v] = DeformPosition(positions[v], written[v], skin);
        }
        return out;
    }
    VertexInfluences influences(mesh);
    for (std::size_t v = 0; v < positions.size(); ++v) {
        out[v] = DeformPosition(positions[v], influences.of(v), skin);
    }
    return out;
}

std::vector<Vector3f> DeformNormals(const Mesh& mesh, std::span<const Matrix44f> skin) {
    const std::span<const Vector3f> normals =
        mesh.attributes.get<Vector3f>(geom::names::kNormal, geom::Domain::Halfedge);
    std::vector<Vector3f> out(normals.begin(), normals.end());
    if (skin.empty() || normals.empty()) {
        return out;
    }
    // Halfedge layers index the cached connectivity, or the local build that
    // reproduces its numbering when there is none.
    geom::Topology local;
    const geom::Topology* topology = nullptr;
    if (mesh.hasConnectivity()) {
        topology = &mesh.topology();
    } else {
        if (!local.build(mesh.faceSet()).ok()) {
            return out;
        }
        topology = &local;
    }

    // The row-vector normal matrix is (M^-1)^T = cofactor(M) / det(M); the
    // length is renormalised, so only the determinant's sign survives — it is
    // what keeps a mirrored bone's normals facing out.
    VertexInfluences influences(mesh);
    std::vector<Matrix44f> normalOf(topology->vertexCount());
    std::vector<u8> ready(topology->vertexCount(), 0);
    for (u32 h = 0; h < topology->halfedgeCount() && h < out.size(); ++h) {
        const geom::HalfedgeId halfedge(h);
        if (topology->isDeleted(halfedge)) {
            continue;
        }
        const u32 v = topology->from(halfedge).value();
        if (v >= normalOf.size()) {
            continue;
        }
        if (ready[v] == 0) {
            const Matrix44f m = BlendSkinMatrix(influences.of(v), skin);
            const auto at = [&](int r, int c) {
                return m.data[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)];
            };
            Matrix44f cofactor = Matrix44f::identity();
            for (int r = 0; r < 3; ++r) {
                for (int c = 0; c < 3; ++c) {
                    const int r1 = (r + 1) % 3;
                    const int r2 = (r + 2) % 3;
                    const int c1 = (c + 1) % 3;
                    const int c2 = (c + 2) % 3;
                    cofactor.data[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)] =
                        at(r1, c1) * at(r2, c2) - at(r1, c2) * at(r2, c1);
                }
            }
            const f32 det = at(0, 0) * cofactor.data[0][0] + at(0, 1) * cofactor.data[0][1] +
                            at(0, 2) * cofactor.data[0][2];
            if (det < 0.0f) {
                for (std::size_t r = 0; r < 3; ++r) {
                    for (std::size_t c = 0; c < 3; ++c) {
                        cofactor.data[r][c] = -cofactor.data[r][c];
                    }
                }
            }
            normalOf[v] = cofactor;
            ready[v] = 1;
        }
        const Matrix44f& n = normalOf[v];
        const Vector3f& rest = normals[h];
        Vector3f posed{rest.x * n.data[0][0] + rest.y * n.data[1][0] + rest.z * n.data[2][0],
                       rest.x * n.data[0][1] + rest.y * n.data[1][1] + rest.z * n.data[2][1],
                       rest.x * n.data[0][2] + rest.y * n.data[1][2] + rest.z * n.data[2][2]};
        const f32 length = std::sqrt(posed.x * posed.x + posed.y * posed.y + posed.z * posed.z);
        out[h] = length > 1e-12f ? posed * (1.0f / length) : rest;
    }
    return out;
}

} // namespace skinning
} // namespace wem
} // namespace models
} // namespace whiteout
