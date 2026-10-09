// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/geometry/fracture/fracture.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <numeric>
#include <set>
#include <unordered_map>
#include <utility>

#include <whiteout/models/wem/geometry/bvh.h>
#include <whiteout/models/wem/geometry/fracture/seeds.h>
#include <whiteout/models/wem/geometry/fracture/winding.h>
#include <whiteout/models/wem/geometry/interpolate.h>
#include <whiteout/models/wem/geometry/ops.h>
#include <whiteout/models/wem/geometry/repair.h>
#include <whiteout/models/wem/geometry/triangulation.h>
#include <whiteout/models/wem/geometry/uv/flatten.h>
#include <whiteout/models/wem/geometry/uv/islands.h>
#include <whiteout/models/wem/geometry/uv/layout.h>
#include <whiteout/models/wem/physics/cloth_cage.h>
#include <whiteout/models/wem/scene_ops.h>

namespace whiteout {
namespace models {
namespace wem {
namespace geom {
namespace fracture {

namespace {

constexpr u32 kNone = 0xFFFFFFFFu;

f64 Dot(const Vector3d& a, const Vector3d& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vector3d D(const Vector3f& v) {
    return Vector3d(v.x, v.y, v.z);
}

Vector3f F(const Vector3d& v) {
    return Vector3f(static_cast<f32>(v.x), static_cast<f32>(v.y), static_cast<f32>(v.z));
}

f64 Length(const Vector3d& v) {
    return std::sqrt(Dot(v, v));
}

/// The probes the flipped-target test reads (§5.3), drawn in the target's box.
constexpr u64 kProbeStream = 1ull << 40;
/// The hash target the slice planes' jitter and tilt are drawn under, past
/// any target's own.
constexpr u32 kSliceStream = 0x51CE0000u;

/// A node named @p name of @p kind under @p parent, standing at model-space
/// @p rest -- its pivot on a pivot rig, its local and its bind poses on an
/// explicit one. As the cloth bake adds its bones.
u32 AddNodeAt(NodeTree& tree, std::string name, NodeKind kind, u32 parent, const Matrix44f& rest) {
    Node node;
    node.name = std::move(name);
    node.kind = kind;
    node.resetPayloadForKind();
    node.parent = parent;
    const Matrix44f parentRest =
        parent < tree.size() ? ToMatrix(tree.worldBind(parent)) : Matrix44f::identity();
    if (tree.rig == RigConvention::PivotRelative) {
        node.pivot = Vector3f{rest.data[3][0], rest.data[3][1], rest.data[3][2]};
        node.local = Transform::identity();
        node.local.translation =
            node.pivot - (parent < tree.size() ? tree.nodes[parent].pivot : Vector3f{0, 0, 0});
    } else {
        node.local = FromMatrix(rest * Matrix44f::inverse(parentRest));
    }
    if (kind == NodeKind::Bone) {
        const bool matrices = std::any_of(tree.nodes.begin(), tree.nodes.end(),
                                          [](const Node& other) { return !other.poseMatrices.empty(); });
        for (const PoseSchema& entry : tree.poseSchema) {
            Matrix44f value =
                entry.space == PoseSpace::Model ? rest : rest * Matrix44f::inverse(parentRest);
            if (entry.inverse) {
                value = Matrix44f::inverse(value);
            }
            node.poses.push_back(FromMatrix(value));
            if (matrices) {
                node.poseMatrices.push_back(entry.storage == PoseStorage::Matrix ? value
                                                                                 : Matrix44f::identity());
            }
        }
    }
    return tree.add(std::move(node));
}

Matrix44f Translation(const Vector3f& at) {
    Matrix44f m = Matrix44f::identity();
    m.data[3][0] = at.x;
    m.data[3][1] = at.y;
    m.data[3][2] = at.z;
    return m;
}

/// @p base, or @p base with " 2", " 3", ... when a node already has it.
std::string UniqueName(const NodeTree& tree, const std::string& base) {
    const auto taken = [&](const std::string& name) {
        return std::any_of(tree.nodes.begin(), tree.nodes.end(),
                           [&](const Node& n) { return n.name == name; });
    };
    if (!taken(base)) {
        return base;
    }
    for (u32 i = 2;; ++i) {
        const std::string name = base + " " + std::to_string(i);
        if (!taken(name)) {
            return name;
        }
    }
}

std::string PieceName(const std::string& name, u32 index, u32 count) {
    char digits[16];
    std::snprintf(digits, sizeof digits, count > 99 ? "%03u" : "%02u", index + 1);
    return name + "_Piece" + digits;
}

f64 Diagonal(const Extent& box) {
    if (!box.valid()) {
        return 0.0;
    }
    return Length(D(box.maximum) - D(box.minimum));
}

u32 TriangleCount(const Mesh& mesh) {
    u32 count = 0;
    for (u32 valence : mesh.faceSet().faceValence) {
        count += valence - 2;
    }
    return count;
}

struct Loop {
    std::vector<u32> vertices;
    std::vector<HalfedgeId> halfedges;
};

/// Face @p f of @p mesh with its corners' halfedges. Needs connectivity.
Loop LoopOf(const Mesh& mesh, u32 f) {
    Loop out;
    const Topology& topology = mesh.topology();
    const HalfedgeId first = topology.halfedge(FaceId(f));
    HalfedgeId h = first;
    do {
        out.vertices.push_back(topology.from(h).value());
        out.halfedges.push_back(h);
        h = topology.next(h);
    } while (h != first && out.halfedges.size() < 1024);
    return out;
}

Vector3d Newell(std::span<const Vector3d> points) {
    Vector3d n(0, 0, 0);
    for (std::size_t k = 0; k < points.size(); ++k) {
        const Vector3d& a = points[k];
        const Vector3d& b = points[(k + 1) % points.size()];
        n += Vector3d((a.y - b.y) * (a.z + b.z), (a.z - b.z) * (a.x + b.x), (a.x - b.x) * (a.y + b.y));
    }
    return n;
}

/// UV units per model unit over @p mesh's outside faces in set 0, or 0.
f64 UvDensity(const Mesh& mesh) {
    const auto positions = mesh.attributes.get<Vector3f>(names::kPosition, Domain::Vertex);
    const auto uv = mesh.attributes.get<Vector2f>(names::uv(0), Domain::Halfedge);
    const auto made = mesh.attributes.get<u8>(names::kFractureMade, Domain::Face);
    if (uv.empty() || !mesh.hasConnectivity()) {
        return 0.0;
    }
    f64 uvArea = 0.0;
    f64 worldArea = 0.0;
    for (u32 f = 0; f < mesh.faceCount(); ++f) {
        if (f < made.size() && made[f] != 0) {
            continue;
        }
        const Loop loop = LoopOf(mesh, f);
        for (std::size_t k = 1; k + 1 < loop.vertices.size(); ++k) {
            const Vector3d a = D(positions[loop.vertices[0]]);
            const Vector3d b = D(positions[loop.vertices[k]]);
            const Vector3d c = D(positions[loop.vertices[k + 1]]);
            worldArea += 0.5 * Length(cross(b - a, c - a));
            const Vector2f& ua = uv[loop.halfedges[0].index()];
            const Vector2f& ub = uv[loop.halfedges[k].index()];
            const Vector2f& uc = uv[loop.halfedges[k + 1].index()];
            uvArea += 0.5 * std::fabs(static_cast<f64>((ub.x - ua.x) * (uc.y - ua.y) -
                                                       (ub.y - ua.y) * (uc.x - ua.x)));
        }
    }
    return worldArea > 0.0 && uvArea > 0.0 ? std::sqrt(uvArea / worldArea) : 0.0;
}

/// The area of @p mesh's inside faces.
f64 InsideArea(const Mesh& mesh, std::span<const u64> facePlane) {
    const auto positions = mesh.attributes.get<Vector3f>(names::kPosition, Domain::Vertex);
    const FaceSet& faces = mesh.faceSet();
    f64 area = 0.0;
    u32 corner = 0;
    for (u32 f = 0; f < faces.faceCount(); ++f) {
        const u32 valence = faces.faceValence[f];
        if (f < facePlane.size() && facePlane[f] != 0) {
            std::vector<Vector3d> loop;
            for (u32 k = 0; k < valence; ++k) {
                loop.push_back(D(positions[faces.cornerVertex[corner + k]]));
            }
            area += 0.5 * Length(Newell(loop));
        }
        corner += valence;
    }
    return area;
}

/// A flat cell face has one tangent. `RecomputeTangents` gives each smoothing
/// run of it its own, a rounding apart, and a file writes a point once per
/// value it carries: five times over, on a face of five triangles. So the
/// faces of @p faces that share a point and its normal take one tangent,
/// their mean by area.
void LevelTangents(Mesh& mesh, std::span<const FaceId> faces) {
    const auto positions = std::as_const(mesh).attributes.get<Vector3f>(names::kPosition, Domain::Vertex);
    const auto normals = std::as_const(mesh).attributes.get<Vector3f>(names::kNormal, Domain::Halfedge);
    const auto tangents = mesh.attributes.get<Vector4f>(names::kTangent, Domain::Halfedge);
    if (tangents.empty() || normals.empty() || faces.empty()) {
        return;
    }
    std::vector<u32> run(faces.size());
    std::iota(run.begin(), run.end(), 0u);
    const auto runOf = [&](u32 i) {
        while (run[i] != i) {
            run[i] = run[run[i]];
            i = run[i];
        }
        return i;
    };
    // Each corner as its point and the normal it has there, then its face.
    std::vector<Loop> loops(faces.size());
    std::vector<std::pair<std::array<u32, 4>, u32>> corners;
    for (u32 i = 0; i < faces.size(); ++i) {
        loops[i] = LoopOf(mesh, faces[i].value());
        for (std::size_t k = 0; k < loops[i].vertices.size(); ++k) {
            const std::size_t h = loops[i].halfedges[k].index();
            if (h >= normals.size()) {
                continue;
            }
            // + 0: -0 is 0 here.
            const f32 normal[3] = {normals[h].x + 0.0f, normals[h].y + 0.0f, normals[h].z + 0.0f};
            std::array<u32, 4> key{loops[i].vertices[k], 0, 0, 0};
            std::memcpy(&key[1], normal, sizeof(normal));
            corners.emplace_back(key, i);
        }
    }
    std::sort(corners.begin(), corners.end());
    for (std::size_t c = 1; c < corners.size(); ++c) {
        if (corners[c].first == corners[c - 1].first) {
            run[runOf(corners[c].second)] = runOf(corners[c - 1].second);
        }
    }
    struct Mean {
        Vector3d along{0, 0, 0};
        Vector3d normal{0, 0, 0};
        f64 side = 0.0;
    };
    std::vector<Mean> means(faces.size());
    for (u32 i = 0; i < faces.size(); ++i) {
        std::vector<Vector3d> points;
        for (const u32 v : loops[i].vertices) {
            points.push_back(D(positions[v]));
        }
        const f64 area = 0.5 * Length(Newell(points));
        Mean& mean = means[runOf(i)];
        for (const HalfedgeId h : loops[i].halfedges) {
            if (h.index() < tangents.size() && h.index() < normals.size()) {
                const Vector4f& tangent = tangents[h.index()];
                mean.along += Vector3d(tangent.x, tangent.y, tangent.z) * area;
                mean.normal += D(normals[h.index()]) * area;
                mean.side += tangent.w * area;
            }
        }
    }
    for (u32 i = 0; i < faces.size(); ++i) {
        const Mean& mean = means[runOf(i)];
        const f64 length = Length(mean.normal);
        if (!(length > 0.0)) {
            continue;
        }
        const Vector3d normal = mean.normal * (1.0 / length);
        const Vector3d flat = mean.along - normal * Dot(mean.along, normal);
        const f64 size = Length(flat);
        // A run with no area, or none mapped, keeps what it has.
        if (!(size > 0.0)) {
            continue;
        }
        const Vector3f along = F(flat * (1.0 / size));
        const Vector4f value{along.x, along.y, along.z, mean.side < 0.0 ? -1.0f : 1.0f};
        for (const HalfedgeId h : loops[i].halfedges) {
            if (h.index() < tangents.size()) {
                tangents[h.index()] = value;
            }
        }
    }
}

/// The inside faces' UVs, colours and tangents (§6.3). With @p square above
/// 0, each cell face is laid flat at that many UV units per model unit and
/// left where it falls, for `PackFilling`.
void DressInside(Mesh& mesh, std::span<const u32> facePiece, std::span<const u64> facePlane,
                 f64 density, f32 uvScale, u32 seed, f64 size, f64 square) {
    if (!mesh.hasConnectivity()) {
        mesh.ensureConnectivity();
    }
    const auto positions = mesh.attributes.get<Vector3f>(names::kPosition, Domain::Vertex);
    std::map<u64, std::vector<FaceId>> byPlane;
    std::vector<FaceId> inside;
    for (u32 f = 0; f < mesh.faceCount() && f < facePlane.size(); ++f) {
        if (facePlane[f] != 0) {
            byPlane[facePlane[f]].push_back(FaceId(f));
            inside.push_back(FaceId(f));
        }
    }
    if (inside.empty()) {
        return;
    }
    // UVs: each cell face projected flat, at the outside's density, shifted by
    // a hash of the plane so neighbouring faces do not repeat one patch.
    const u32 sets = UvSetCount(mesh.attributes);
    if (sets > 0) {
        const f64 extent = square > 0.0    ? 1.0 / square
                           : density > 0.0 ? 1.0 / (density * std::max(uvScale, 1e-6f))
                                           : std::max(size, 1e-6) / std::max(uvScale, 1e-6f);
        for (const auto& [plane, faces] : byPlane) {
            const Loop first = LoopOf(mesh, faces.front().value());
            std::vector<Vector3d> corners;
            for (u32 v : first.vertices) {
                corners.push_back(D(positions[v]));
            }
            Vector3d n = Newell(corners);
            const f64 length = Length(n);
            if (!(length > 0.0)) {
                continue;
            }
            n = n * (1.0 / length);
            Vector3d centre(0, 0, 0);
            u32 count = 0;
            for (FaceId f : faces) {
                for (u32 v : LoopOf(mesh, f.value()).vertices) {
                    centre += D(positions[v]);
                    ++count;
                }
            }
            centre = centre * (1.0 / static_cast<f64>(count));
            const Vector3d look = n * -1.0;
            const Vector3d axis = std::fabs(n.x) < 0.6 ? Vector3d(1, 0, 0) : Vector3d(0, 1, 0);
            Vector3d u = cross(axis, n);
            u = u * (1.0 / Length(u));
            const Vector3d v = cross(look, u);
            // One square has no patch to repeat: its faces are packed after.
            const f64 h0 = square > 0.0 ? 0.0
                                        : HashUnit(FractureHash(seed, static_cast<u32>(plane >> 32),
                                                                plane & 0xFFFFFFFFu));
            const f64 h1 = square > 0.0 ? 0.0
                                        : HashUnit(FractureHash(seed, static_cast<u32>(plane >> 32),
                                                                (plane & 0xFFFFFFFFu) | (1ull << 33)));
            uv::ProjectFrame frame;
            frame.origin = F(centre - u * (h0 * extent) - v * (h1 * extent));
            frame.axisU = F(u);
            frame.axisV = F(v);
            frame.axisN = F(look);
            frame.extent = static_cast<f32>(extent);
            uv::Project(mesh, faces, 0, uv::ProjectShape::Planar, frame);
        }
        const auto uv0 = mesh.attributes.get<Vector2f>(names::uv(0), Domain::Halfedge);
        for (u32 s = 1; s < sets; ++s) {
            auto uvs = mesh.attributes.get<Vector2f>(names::uv(s), Domain::Halfedge);
            for (FaceId f : inside) {
                for (HalfedgeId h : LoopOf(mesh, f.value()).halfedges) {
                    if (h.index() < uvs.size() && h.index() < uv0.size()) {
                        uvs[h.index()] = uv0[h.index()];
                    }
                }
            }
        }
    }
    // Colours: the mean of the piece's outside corners.
    for (u32 c = 0;; ++c) {
        AttrLayer* layer = mesh.attributes.layer(names::color(c), Domain::Halfedge);
        if (layer == nullptr) {
            break;
        }
        const u32 components = AttrTypeComponents(layer->type);
        const bool bytes = layer->type == AttrType::U8x4;
        const bool floats = layer->type == AttrType::F32x3 || layer->type == AttrType::F32x4;
        if (!bytes && !floats) {
            continue;
        }
        const u32 stride = AttrTypeSize(layer->type);
        std::map<u32, std::pair<std::array<f64, 4>, u32>> mean;
        for (u32 f = 0; f < mesh.faceCount() && f < facePlane.size(); ++f) {
            if (facePlane[f] != 0) {
                continue;
            }
            auto& [sum, count] = mean[facePiece[f]];
            for (HalfedgeId h : LoopOf(mesh, f).halfedges) {
                const u8* at = layer->data.data() + static_cast<std::size_t>(h.value()) * stride;
                for (u32 k = 0; k < components; ++k) {
                    if (bytes) {
                        sum[k] += at[k];
                    } else {
                        f32 value;
                        std::memcpy(&value, at + 4 * k, 4);
                        sum[k] += value;
                    }
                }
                ++count;
            }
        }
        for (FaceId f : inside) {
            const auto found = mean.find(facePiece[f.value()]);
            if (found == mean.end() || found->second.second == 0) {
                continue;
            }
            const auto& [sum, count] = found->second;
            for (HalfedgeId h : LoopOf(mesh, f.value()).halfedges) {
                u8* at = layer->data.data() + static_cast<std::size_t>(h.value()) * stride;
                for (u32 k = 0; k < components; ++k) {
                    const f64 value = sum[k] / count;
                    if (bytes) {
                        at[k] = static_cast<u8>(std::clamp(std::lround(value), 0l, 255l));
                    } else {
                        const f32 v32 = static_cast<f32>(value);
                        std::memcpy(at + 4 * k, &v32, 4);
                    }
                }
            }
        }
    }
    if (mesh.attributes.has(names::kTangent, Domain::Halfedge)) {
        RecomputeTangents(mesh, inside, 0);
        LevelTangents(mesh, inside);
    }
}

/// Removes the faces @p drop names from @p mesh, and what only they used.
void DropFaces(Mesh& mesh, const std::vector<bool>& drop) {
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        return;
    }
    for (u32 f = 0; f < drop.size(); ++f) {
        if (drop[f]) {
            mesh.topology().deleteFace(FaceId(f), true);
        }
    }
    GarbageCollect(mesh);
    mesh.invalidateConnectivity();
    mesh.recomputeBounds();
}

/// Every geoset track of mesh @p from, copied onto mesh @p to in every clip of
/// the model, so a fade fades both.
void CopySectionTracks(Document& document, u32 model, u32 from, u32 to) {
    AnimChannelTable& table = document.models[model].animChannels;
    std::vector<std::pair<u32, u32>> copies;
    const std::size_t count = table.channels.size();
    for (std::size_t i = 0; i < count; ++i) {
        const AnimChannel channel = table.channels[i];
        if (channel.target.kind != TrackTarget::Kind::Section || channel.target.mesh != from) {
            continue;
        }
        AnimChannel copy = channel;
        copy.id = table.nextFreeId();
        copy.target.mesh = to;
        table.add(copy);
        copies.emplace_back(channel.id, copy.id);
    }
    if (copies.empty()) {
        return;
    }
    for (Clip& clip : document.clips) {
        if (clip.model != model) {
            continue;
        }
        for (SubTrackContainer& container : clip.containers) {
            const std::size_t tracks = container.subTracks.size();
            for (std::size_t t = 0; t < tracks; ++t) {
                for (const auto& [old, made] : copies) {
                    if (container.subTracks[t].channel == old) {
                        SubTrack track = container.subTracks[t];
                        track.channel = made;
                        container.subTracks.push_back(std::move(track));
                    }
                }
            }
        }
    }
}

/// *One square* (EDIT_MODE_FRACTURE_NEXT_DESIGN.md §5.3): every island of the
/// inside meshes packed into the tile at one scale. A crack's two faces share
/// their UV points, so they move as one. Returns the scale it took and how
/// many islands it placed.
f32 PackFilling(std::vector<std::pair<u32, Mesh>>& meshes, u32 mapSize, u32& placed) {
    std::vector<uv::UvIslands> islands(meshes.size());
    std::vector<std::vector<u8>> movable(meshes.size());
    std::vector<uv::PackInput> inputs;
    for (std::size_t i = 0; i < meshes.size(); ++i) {
        Mesh& mesh = meshes[i].second;
        if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
            continue;
        }
        islands[i] = uv::BuildUvIslands(mesh, 0);
        movable[i].assign(islands[i].count, 1);
        inputs.push_back(uv::PackInput{&mesh, &islands[i], 0, movable[i], {}});
    }
    uv::PackOptions options;
    options.resolution = std::clamp<u32>(mapSize, 64, 8192);
    options.leavePinned = false;
    options.refuseTiling = false;
    const uv::PackResult packed = uv::PackMeshes(inputs, options);
    placed = packed.placed;
    for (auto& [target, mesh] : meshes) {
        if (!mesh.hasConnectivity()) {
            continue;
        }
        // The other sets follow the first, and the tangents the new layout.
        const u32 sets = UvSetCount(mesh.attributes);
        const auto uv0 = mesh.attributes.get<Vector2f>(names::uv(0), Domain::Halfedge);
        for (u32 s = 1; s < sets; ++s) {
            auto uvs = mesh.attributes.get<Vector2f>(names::uv(s), Domain::Halfedge);
            for (std::size_t h = 0; h < uvs.size() && h < uv0.size(); ++h) {
                uvs[h] = uv0[h];
            }
        }
        if (mesh.attributes.has(names::kTangent, Domain::Halfedge)) {
            RecomputeTangents(mesh, 0);
            std::vector<FaceId> all(mesh.faceCount());
            for (u32 f = 0; f < all.size(); ++f) {
                all[f] = FaceId(f);
            }
            LevelTangents(mesh, all);
        }
        mesh.invalidateConnectivity();
    }
    return packed.placed > 0 ? packed.scaled : 0.0f;
}

/// A file indexes a section's vertices in 16 bits, and writes a point once per
/// set of corner values it carries; one more only truncates the index. A
/// section of @p mesh past @p most of those is divided into sections like it,
/// whole pieces to each: a piece is the faces of one bone, or of the bones
/// @p pieceOfNode gives one piece, and one alone past @p most stays as it is.
/// `fracture.section` keeps where a moved face was. Returns each section
/// added, with the one it came from.
std::vector<std::pair<u32, u32>> DivideSections(Mesh& mesh, u32 most, std::span<const u32> pieceOfNode) {
    std::vector<std::pair<u32, u32>> added;
    const u32 faceCount = mesh.faceCount();
    if (most == 0 || mesh.sections.empty() || faceCount == 0) {
        return added;
    }
    const bool built = !mesh.hasConnectivity();
    if (built && !mesh.ensureConnectivity().ok()) {
        return added;
    }
    // Before the layers are listed: it makes the layer when there is none.
    std::span<u32> sectionOf = mesh.faceSections();
    std::vector<const AttrLayer*> layers;
    for (const AttrLayer& layer : std::as_const(mesh).attributes.layers()) {
        // A mark is not a value: it parts no vertex in any file.
        if (layer.domain == Domain::Halfedge && !names::IsCornerMark(layer.name)) {
            layers.push_back(&layer);
        }
    }
    // A corner's values over every corner layer as one number: no file reads
    // more of them than all, so the count is never under what it writes.
    const auto valueOf = [&](HalfedgeId h) {
        u64 value = 0xcbf29ce484222325ull;
        for (const AttrLayer* layer : layers) {
            const u32 stride = AttrTypeSize(layer->type);
            const std::size_t at = static_cast<std::size_t>(h.value()) * stride;
            for (u32 k = 0; k < stride && at + k < layer->data.size(); ++k) {
                value = (value ^ layer->data[at + k]) * 0x100000001b3ull;
            }
        }
        return value;
    };
    struct Written {
        u32 section = 0;
        u32 piece = 0;
        u32 vertex = 0;
        u64 value = 0;
        auto operator<=>(const Written&) const = default;
    };
    using Part = std::pair<u32, u32>; ///< A section and a piece in it.
    std::vector<Written> written;
    std::vector<u32> pieceOf(faceCount, kNone);
    std::vector<std::vector<u32>> order(mesh.sections.size());
    std::set<Part> met;
    for (u32 f = 0; f < faceCount; ++f) {
        const u32 section = f < sectionOf.size() ? sectionOf[f] : 0;
        const Loop loop = LoopOf(mesh, f);
        if (section >= order.size() || loop.vertices.empty()) {
            continue;
        }
        if (loop.vertices[0] < mesh.skin.vertexCount()) {
            const auto bound = std::as_const(mesh.skin).forVertex(loop.vertices[0]);
            pieceOf[f] = bound.empty() ? kNone : bound[0].bone;
            if (pieceOf[f] < pieceOfNode.size()) {
                pieceOf[f] = pieceOfNode[pieceOf[f]];
            }
        }
        if (met.emplace(section, pieceOf[f]).second) {
            order[section].push_back(pieceOf[f]);
        }
        for (std::size_t k = 0; k < loop.vertices.size(); ++k) {
            written.push_back(Written{section, pieceOf[f], loop.vertices[k], valueOf(loop.halfedges[k])});
        }
    }
    std::sort(written.begin(), written.end());
    written.erase(std::unique(written.begin(), written.end()), written.end());
    std::map<Part, u32> counts;
    for (const Written& w : written) {
        ++counts[Part(w.section, w.piece)];
    }

    std::map<Part, u32> moved;
    const u32 before = static_cast<u32>(mesh.sections.size());
    for (u32 s = 0; s < before; ++s) {
        u64 total = 0;
        for (const u32 piece : order[s]) {
            total += counts[Part(s, piece)];
        }
        if (total <= most) {
            continue;
        }
        u32 into = s;
        u32 held = 0;
        for (const u32 piece : order[s]) {
            const u32 count = counts[Part(s, piece)];
            if (held != 0 && held + count > most) {
                MeshSection like = mesh.sections[s];
                into = static_cast<u32>(mesh.sections.size());
                mesh.sections.push_back(std::move(like));
                added.emplace_back(into, s);
                held = 0;
            }
            held += count;
            if (into != s) {
                moved[Part(s, piece)] = into;
            }
        }
    }
    if (!added.empty()) {
        const auto origin = mesh.attributes.getOrCreate<u32>(names::kFractureSection, Domain::Face, AttrType::U32);
        sectionOf = mesh.faceSections();
        for (u32 f = 0; f < faceCount && f < sectionOf.size() && f < origin.size(); ++f) {
            const auto to = moved.find(Part(sectionOf[f], pieceOf[f]));
            if (to != moved.end()) {
                origin[f] = sectionOf[f] + 1;
                sectionOf[f] = to->second;
            }
        }
    }
    if (built) {
        mesh.invalidateConnectivity();
    }
    if (!added.empty()) {
        mesh.recomputeBounds();
    }
    return added;
}

/// Where @p plane cuts, as a rectangle on it. False when it cuts everywhere:
/// it has no size, or its rectangle holds all the plane has of the box.
bool ReachOf(const SlicePlane& plane, const Vector3d& low, const Vector3d& high, SliceReach& out) {
    Vector3d n = D(plane.normal);
    const f64 length = Length(n);
    if (!(plane.width > 0.0f) || !(plane.height > 0.0f) || !(length > 0.0)) {
        return false;
    }
    n = n * (1.0 / length);
    Vector3d u = D(plane.along);
    u = u - n * Dot(u, n);
    if (!(Length(u) > 1e-9)) {
        u = cross(std::fabs(n.x) < 0.6 ? Vector3d(1, 0, 0) : Vector3d(0, 1, 0), n);
    }
    out.u = u * (1.0 / Length(u));
    out.v = cross(n, out.u);
    const Vector3d centre = D(plane.centre);
    out.centre = centre - n * (Dot(centre, n) - plane.offset / length);
    out.halfU = plane.width;
    out.halfV = plane.height;
    // The box's corners, laid on the plane, hold all it has of the box.
    for (u32 k = 0; k < 8; ++k) {
        const Vector3d corner((k & 1) != 0 ? high.x : low.x, (k & 2) != 0 ? high.y : low.y,
                              (k & 4) != 0 ? high.z : low.z);
        if (!out.holds(corner - n * (Dot(corner, n) - plane.offset / length))) {
            return true;
        }
    }
    return false;
}

/// A piece's cut skeleton: its bone, and each source node's copy under it.
struct CutSkeleton {
    u32 root = kInvalidNode;
    std::map<u32, u32> copies;
};

/// @p node and the nodes above it, to its root.
std::vector<u32> ChainOf(const NodeTree& tree, u32 node) {
    std::vector<u32> chain;
    for (u32 n = node; n < tree.size() && chain.size() <= tree.size(); n = tree.nodes[n].parent) {
        chain.push_back(n);
    }
    return chain;
}

/// A copy of node @p source under @p parent: its rest, its flags and its
/// poses, so that under a copy of its parent it stands and moves as its
/// source does. A bone stays a bone and a helper a helper; any other kind is
/// a helper, since a second emitter is not what was asked for. @p asBone
/// makes it a bone whatever it was: a piece's own.
u32 CopyNode(NodeTree& tree, u32 source, u32 parent, std::string name, bool asBone) {
    const bool bone = tree.nodes[source].kind == NodeKind::Bone;
    if (asBone && !bone) {
        const Node from = tree.nodes[source];
        const u32 made = AddNodeAt(tree, std::move(name), NodeKind::Bone, parent, ToMatrix(tree.worldBind(source)));
        tree.nodes[made].pivot = from.pivot;
        tree.nodes[made].local = from.local;
        tree.nodes[made].flags = from.flags;
        tree.nodes[made].profiles = from.profiles;
        return made;
    }
    Node node = tree.nodes[source];
    node.name = std::move(name);
    node.parent = parent;
    node.skin = {};
    node.rig = {};
    node.removed = false;
    if (!bone && node.kind != NodeKind::Helper) {
        node.kind = NodeKind::Helper;
        node.resetPayloadForKind();
        node.native = {};
    }
    return tree.add(std::move(node));
}

/**
 * @brief The skeleton cut for one piece, whose points rode the bones of
 *        @p rides by those shares of it.
 *
 * The piece's bone is a copy of the node that carries it: the lowest one whose
 * bones, with those under it, hold three quarters of the piece. It hangs from
 * that node's parent, and the nodes below it down to the bones the piece rode
 * are copied under it. So what moves the whole piece is one bone, and what is
 * under it is the piece's own limbs. A bone the piece rode outside that node
 * (the far side's, at the rim of a cut) is not copied: its share goes to the
 * piece's bone.
 *
 * With no such node (two roots that share the piece), or a point bound to
 * nothing (@p loose), the piece's bone is a new one at the root, and every
 * bone it rode is copied under it, from the roots down.
 */
CutSkeleton CutPieceSkeleton(NodeTree& tree, const std::map<u32, f64>& rides, bool loose, const std::string& name,
                             const Vector3f& centroid) {
    CutSkeleton made;
    // Each node's share: its own, and that of every node under it.
    std::map<u32, f64> held;
    f64 all = 0.0;
    for (const auto& [bone, share] : rides) {
        all += share;
        for (const u32 n : ChainOf(tree, bone)) {
            held[n] += share;
        }
    }
    // From the top down, while one node holds three quarters of it: lower,
    // and too much of the piece would be rigid with its bone; higher, and the
    // piece would still play the clip under a bone that no longer does.
    constexpr f64 kCarries = 0.75;
    u32 top = kInvalidNode;
    for (u32 guard = 0; !loose && all > 0.0 && guard <= tree.size(); ++guard) {
        u32 next = kInvalidNode;
        for (const auto& [n, share] : held) {
            if (tree.nodes[n].parent == top && share >= kCarries * all) {
                next = n;
            }
        }
        if (next == kInvalidNode) {
            break;
        }
        top = next;
    }
    std::set<u32> nodes;
    for (const auto& [bone, share] : rides) {
        const std::vector<u32> chain = ChainOf(tree, bone);
        // A bone outside the carrier is left to it.
        if (top != kInvalidNode && std::find(chain.begin(), chain.end(), top) == chain.end()) {
            continue;
        }
        for (const u32 n : chain) {
            nodes.insert(n);
            if (n == top) {
                break;
            }
        }
    }
    if (top == kInvalidNode) {
        // On an explicit rig a child's rest is in its parent's frame: a bone
        // with copies under it stands where the roots did.
        const bool atOrigin = tree.rig != RigConvention::PivotRelative && !nodes.empty();
        made.root = AddNodeAt(tree, UniqueName(tree, name), NodeKind::Bone, kInvalidNode,
                              atOrigin ? Matrix44f::identity() : Translation(centroid));
    }
    // By depth, a parent before its children: an imported tree need not list
    // them so (a file's bones come before its helpers), and a copy made before
    // its parent's would hang from nothing.
    std::vector<std::pair<std::size_t, u32>> order;
    for (const u32 n : nodes) {
        order.emplace_back(ChainOf(tree, n).size(), n);
    }
    std::sort(order.begin(), order.end());
    for (const auto& [depth, n] : order) {
        const u32 parent = tree.nodes[n].parent;
        if (n == top) {
            made.root = CopyNode(tree, n, parent, UniqueName(tree, name), true);
            made.copies[n] = made.root;
            continue;
        }
        const auto found = made.copies.find(parent);
        const u32 under = found != made.copies.end() ? found->second : made.root;
        made.copies[n] = CopyNode(tree, n, under, UniqueName(tree, name + "_" + tree.nodes[n].name), false);
    }
    return made;
}

/// The transform tracks of each node of @p copies, stated again for each of
/// its copies in every clip of the model: a copy moves as its source does.
void CopyNodeTracks(Document& document, u32 model, const std::multimap<u32, u32>& copies) {
    if (copies.empty()) {
        return;
    }
    AnimChannelTable& table = document.models[model].animChannels;
    std::unordered_map<u32, std::vector<u32>> made;
    const std::size_t count = table.channels.size();
    for (std::size_t i = 0; i < count; ++i) {
        const AnimChannel channel = table.channels[i];
        const bool transform = channel.target.channel == Channel::Translation ||
                               channel.target.channel == Channel::Rotation ||
                               channel.target.channel == Channel::Scale;
        if (channel.target.kind != TrackTarget::Kind::Node || !transform) {
            continue;
        }
        const auto range = copies.equal_range(channel.target.node);
        for (auto at = range.first; at != range.second; ++at) {
            AnimChannel copy = channel;
            copy.id = table.nextFreeId();
            copy.target.node = at->second;
            table.add(copy);
            made[channel.id].push_back(copy.id);
        }
    }
    if (made.empty()) {
        return;
    }
    for (Clip& clip : document.clips) {
        if (clip.model != model) {
            continue;
        }
        for (SubTrackContainer& container : clip.containers) {
            const std::size_t tracks = container.subTracks.size();
            for (std::size_t t = 0; t < tracks; ++t) {
                const auto found = made.find(container.subTracks[t].channel);
                if (found == made.end()) {
                    continue;
                }
                for (const u32 id : found->second) {
                    SubTrack track = container.subTracks[t];
                    track.channel = id;
                    container.subTracks.push_back(std::move(track));
                }
            }
        }
    }
}

} // namespace

std::vector<SlicePlane> SlicePlanes(std::span<const SliceSet> sets, u32 seed, const Vector3f& low,
                                    const Vector3f& high) {
    std::vector<SlicePlane> planes;
    const Vector3d centre = (D(low) + D(high)) * 0.5;
    for (u32 s = 0; s < sets.size(); ++s) {
        const SliceSet& set = sets[s];
        Vector3d n = D(set.direction);
        const f64 length = Length(n);
        if (!(length > 0.0) || set.count == 0) {
            continue;
        }
        n = n * (1.0 / length);
        // The box's reach along the direction.
        f64 from = 0.0;
        f64 to = 0.0;
        for (u32 k = 0; k < 8; ++k) {
            const Vector3d corner((k & 1) != 0 ? high.x : low.x, (k & 2) != 0 ? high.y : low.y,
                                  (k & 4) != 0 ? high.z : low.z);
            const f64 along = Dot(corner, n);
            from = k == 0 ? along : std::min(from, along);
            to = k == 0 ? along : std::max(to, along);
        }
        const f64 gap = (to - from) / (set.count + 1);
        const Vector3d axis = std::fabs(n.x) < 0.6 ? Vector3d(1, 0, 0) : Vector3d(0, 1, 0);
        Vector3d u = cross(axis, n);
        u = u * (1.0 / Length(u));
        const Vector3d v = cross(n, u);
        for (u32 k = 0; k < set.count; ++k) {
            const auto unit = [&](u32 j) { return HashUnit(FractureHash(seed, kSliceStream + s, 4ull * k + j)); };
            f64 offset = from + gap * (k + 1) + set.shift;
            offset += (unit(0) - 0.5) * gap * std::clamp(set.jitter, 0.0f, 1.0f);
            Vector3d normal = n;
            if (set.tilt > 0.0f) {
                // It leans about its own point on the box's axis.
                const f64 lean = unit(1) * set.tilt;
                const f64 round = unit(2) * 6.283185307179586;
                const Vector3d side = u * std::cos(round) + v * std::sin(round);
                const Vector3d at = centre + n * (offset - Dot(centre, n));
                normal = n * std::cos(lean) + side * std::sin(lean);
                offset = Dot(normal, at);
            }
            planes.push_back(SlicePlane{F(normal), static_cast<f32>(offset)});
        }
    }
    return planes;
}

const char* ToString(Refusal refusal) {
    switch (refusal) {
    case Refusal::None:
        return "none";
    case Refusal::Missing:
        return "missing";
    case Refusal::Empty:
        return "empty";
    case Refusal::Cloth:
        return "cloth";
    case Refusal::Billboard:
        return "billboard";
    case Refusal::Repeated:
        return "repeated";
    case Refusal::NoPieces:
        return "no pieces";
    case Refusal::TooMany:
        return "too many";
    }
    return "?";
}

Refusal RefuseTarget(const Model& model, u32 mesh) {
    if (mesh >= model.meshes.size()) {
        return Refusal::Missing;
    }
    const Mesh& m = model.meshes[mesh];
    if (m.faceCount() == 0) {
        return Refusal::Empty;
    }
    for (const MeshSection& section : m.sections) {
        if (hasFlag(section.flags, SectionFlags::ClothSimulated) ||
            hasFlag(section.flags, SectionFlags::ClothInfluenced)) {
            return Refusal::Cloth;
        }
        if (hasFlag(section.flags, SectionFlags::Billboard)) {
            return Refusal::Billboard;
        }
    }
    return Refusal::None;
}

FractureResult BreakModel(Document& document, u32 modelIndex, const FractureSpec& spec) {
    FractureResult result;
    if (modelIndex >= document.models.size() || spec.targets.empty()) {
        result.refusal = Refusal::Missing;
        return result;
    }
    Model& model = document.models[modelIndex];
    const u32 count = static_cast<u32>(spec.targets.size());
    for (u32 i = 0; i < count; ++i) {
        Refusal refusal = RefuseTarget(model, spec.targets[i].mesh);
        for (u32 j = 0; j < i && refusal == Refusal::None; ++j) {
            if (spec.targets[j].mesh == spec.targets[i].mesh) {
                refusal = Refusal::Repeated;
            }
        }
        if (refusal != Refusal::None) {
            result.refusal = refusal;
            result.refusedTarget = i;
            return result;
        }
    }

    // --- the meshes to cut, thickened when asked ---
    const bool sliced = spec.method == Method::Slices;
    std::vector<Mesh> sources(count);
    std::vector<Mesh> work(count);
    for (u32 i = 0; i < count; ++i) {
        sources[i] = model.meshes[spec.targets[i].mesh];
        sources[i].recomputeBounds();
        work[i] = sources[i];
        result.trianglesBefore += TriangleCount(sources[i]);
        if (spec.openParts == OpenParts::Thicken && spec.targets[i].pieces != 1) {
            const f64 thickness =
                spec.thickness > 0.0f ? spec.thickness : 0.02 * Diagonal(work[i].bounds);
            ThickenOpen(work[i], thickness);
        }
    }

    // --- winding, surfaces and the box ---
    std::vector<WindingMesh> winding(count);
    std::vector<TriangleBvh> surfaces(count);
    Vector3d low(0, 0, 0);
    Vector3d high(0, 0, 0);
    bool any = false;
    for (u32 i = 0; i < count; ++i) {
        winding[i] = WindingMeshOf(work[i]);
        const Extent& box = winding[i].bounds;
        if (!box.valid()) {
            continue;
        }
        std::vector<Vector3d> probes;
        for (u32 k = 0; k < 64; ++k) {
            const auto unit = [&](u32 j) {
                return HashUnit(FractureHash(spec.seed, i, kProbeStream + 3 * k + j));
            };
            probes.push_back(Vector3d(box.minimum.x + unit(0) * (box.maximum.x - box.minimum.x),
                                      box.minimum.y + unit(1) * (box.maximum.y - box.minimum.y),
                                      box.minimum.z + unit(2) * (box.maximum.z - box.minimum.z)));
        }
        OrientWinding(winding[i], probes);
        std::vector<u32> triangles;
        TriangulateMesh(work[i], triangles);
        surfaces[i].build(triangles,
                          work[i].attributes.get<Vector3f>(names::kPosition, Domain::Vertex));
        const Vector3d a = D(box.minimum);
        const Vector3d b = D(box.maximum);
        low = any ? Vector3d(std::min(low.x, a.x), std::min(low.y, a.y), std::min(low.z, a.z)) : a;
        high = any ? Vector3d(std::max(high.x, b.x), std::max(high.y, b.y), std::max(high.z, b.z)) : b;
        any = true;
    }
    if (!any) {
        result.refusal = Refusal::Empty;
        result.refusedTarget = 0;
        return result;
    }
    const Vector3d centre = (low + high) * 0.5;
    const f64 grow = 0.01 * Length(high - low);
    const Vector3d margin(grow, grow, grow);

    // --- each target's count: its own, or its share of the total by size ---
    std::vector<SeedTarget> seedTargets(count);
    result.shares.resize(count);
    bool sharing = false;
    for (u32 i = 0; i < count; ++i) {
        seedTargets[i].winding = &winding[i];
        seedTargets[i].surface = &surfaces[i];
        result.shares[i] = spec.targets[i].pieces;
        sharing = sharing || spec.targets[i].pieces == 0;
    }
    if (sharing && !sliced) {
        result.shares =
            SharePieces(result.shares, TargetMeasures(seedTargets, spec.seed, spec.threads), spec.total);
    }
    u32 wholes = 0;
    for (u32 i = 0; i < count; ++i) {
        seedTargets[i].pieces = result.shares[i];
        wholes += spec.targets[i].pieces == 1 ? 1 : 0;
    }

    // --- seeds and their cells, or the planes' regions; then the cut ---
    std::vector<Vector3d> seeds;
    CellComplex diagram;
    std::vector<u32> slots; ///< *Slices*: per plane asked for, the plane of the regions it is.
    if (sliced) {
        result.planes = spec.planes.empty() ? SlicePlanes(spec.slices, spec.seed, F(low), F(high)) : spec.planes;
        std::vector<HalfSpace> planes;
        for (const SlicePlane& plane : result.planes) {
            planes.push_back(HalfSpace{D(plane.normal), plane.offset});
        }
        diagram = PlaneCells(planes, low - margin, high + margin, kMostSliceRegions, &slots);
        if (diagram.cells.empty()) {
            result.refusal = Refusal::TooMany;
            return result;
        }
        // Each plane's reach. Planes that are one plane cut within any of
        // their rectangles, and everywhere when one of them does.
        std::vector<u8> everywhere(diagram.slices.size(), 0);
        for (std::size_t i = 0; i < result.planes.size(); ++i) {
            if (slots[i] >= diagram.slices.size()) {
                continue;
            }
            SliceReach reach;
            if (ReachOf(result.planes[i], low - margin, high + margin, reach)) {
                diagram.reach[slots[i]].push_back(reach);
            } else {
                everywhere[slots[i]] = 1;
            }
        }
        for (std::size_t k = 0; k < everywhere.size(); ++k) {
            if (everywhere[k] != 0) {
                diagram.reach[k].clear();
            }
        }
        // A region's middle stands for its seed: what a static place is kept as.
        for (const ConvexCell& cell : diagram.cells) {
            Vector3d middle(0, 0, 0);
            for (const Vector3d& v : cell.vertices) {
                middle += v;
            }
            seeds.push_back(middle * (1.0 / static_cast<f64>(std::max<std::size_t>(cell.vertices.size(), 1))));
        }
    } else {
        if (!spec.seeds.empty()) {
            for (const Vector3f& s : spec.seeds) {
                seeds.push_back(D(s));
            }
        } else {
            SeedOptions options;
            options.seed = spec.seed;
            options.nearBlast = spec.nearBlast;
            options.blastCentre = D(spec.blastCentre);
            options.blastRadius = spec.blastRadius;
            options.even = spec.even;
            options.grain = spec.grain;
            options.stretch = spec.stretch;
            options.threads = spec.threads;
            seeds = FractureSeeds(seedTargets, options);
        }
        VoronoiOptions voronoi;
        voronoi.grain = spec.grain;
        voronoi.stretch = spec.stretch;
        voronoi.threads = spec.threads;
        diagram = VoronoiCells(seeds, low - margin, high + margin, voronoi);
    }
    std::vector<CutTarget> targets(count);
    for (u32 i = 0; i < count; ++i) {
        targets[i].mesh = &work[i];
        targets[i].winding = &winding[i];
        targets[i].whole = spec.targets[i].pieces == 1;
    }
    CutOptions cutOptions;
    cutOptions.smallest = spec.smallest;
    cutOptions.threads = spec.threads;
    // A figure's parts are told apart by what they ride: an arm that rests on
    // a hip is not the hip's. So where the skin is kept, and where a plane is
    // placed to cut one part and not another.
    std::vector<u32> parents;
    if (spec.cutSkeleton || diagram.bounded()) {
        // A bone's parent, for this, is the nearest node above it that a point
        // rides: a helmet's bone hangs from a helper no point is bound to, and
        // is the chest's all the same.
        const NodeTree& nodes = model.nodes;
        std::vector<u8> ridden(nodes.size(), 0);
        for (u32 i = 0; i < count; ++i) {
            for (const u32 owner : VertexOwners(work[i])) {
                if (owner < ridden.size()) {
                    ridden[owner] = 1;
                }
            }
            for (u32 v = 0; v < work[i].skin.vertexCount(); ++v) {
                for (const Influence& influence : std::as_const(work[i].skin).forVertex(v)) {
                    if (influence.weight >= 0.1f && influence.bone < ridden.size()) {
                        ridden[influence.bone] = 1;
                    }
                }
            }
        }
        parents.assign(nodes.size(), kInvalidNode);
        for (u32 n = 0; n < nodes.size(); ++n) {
            u32 above = nodes.nodes[n].parent;
            for (u32 guard = 0; above < nodes.size() && ridden[above] == 0 && guard < nodes.size(); ++guard) {
                above = nodes.nodes[above].parent;
            }
            parents[n] = above < nodes.size() ? above : kInvalidNode;
        }
        cutOptions.parts = Parts::Touch;
        cutOptions.parents = parents;
    }
    CutResult cut = CutPieces(targets, diagram, cutOptions);
    if (cut.pieces.empty()) {
        result.refusal = Refusal::NoPieces;
        return result;
    }
    if (sliced && cut.pieces.size() > kMostSlicePieces) {
        result.refusal = Refusal::TooMany;
        return result;
    }
    result.asked = static_cast<u32>(seeds.size()) + wholes;
    result.split = cut.split;
    result.empty = cut.empty;
    result.joined = cut.joined;
    // A plane parted something when a crack lies on it.
    if (sliced) {
        std::vector<u8> parts(diagram.slices.size(), 0);
        for (u32 i = 0; i < count; ++i) {
            for (const u64 key : cut.facePlane[i]) {
                const u32 site = PlaneLow(key);
                if (key != 0 && IsPlaneSite(site) && site - kPlaneSite < parts.size()) {
                    parts[site - kPlaneSite] = 1;
                }
            }
        }
        for (const u32 slot : slots) {
            result.planeParts.push_back(slot < parts.size() ? parts[slot] : u8{0});
        }
    }

    // --- each piece's parent: the node its outside rode most, by area ---
    const u32 pieceCount = static_cast<u32>(cut.pieces.size());
    std::vector<std::map<u32, f64>> votes(pieceCount);
    for (u32 i = 0; i < count; ++i) {
        const Mesh& mesh = cut.meshes[i];
        if (mesh.faceCount() == 0) {
            continue;
        }
        const std::vector<u32> owners = VertexOwners(mesh);
        const auto positions = mesh.attributes.get<Vector3f>(names::kPosition, Domain::Vertex);
        const FaceSet& faces = mesh.faceSet();
        u32 corner = 0;
        for (u32 f = 0; f < faces.faceCount(); ++f) {
            const u32 valence = faces.faceValence[f];
            if (cut.facePlane[i][f] == 0) {
                std::vector<Vector3d> loop;
                for (u32 k = 0; k < valence; ++k) {
                    loop.push_back(D(positions[faces.cornerVertex[corner + k]]));
                }
                const f64 area = 0.5 * Length(Newell(loop));
                for (u32 k = 0; k < valence; ++k) {
                    const u32 owner = owners[faces.cornerVertex[corner + k]];
                    votes[cut.facePiece[i][f]][owner] += area / valence;
                }
            }
            corner += valence;
        }
    }
    std::vector<u32> parent(pieceCount, kInvalidNode);
    for (u32 p = 0; p < pieceCount; ++p) {
        f64 best = -1.0;
        for (const auto& [node, weight] : votes[p]) {
            if (weight > best) {
                best = weight;
                parent[p] = node;
            }
        }
    }
    // A piece with no outside of its own (deep inside a solid) rides the most
    // common parent.
    {
        std::map<u32, u32> common;
        for (u32 p = 0; p < pieceCount; ++p) {
            if (!votes[p].empty()) {
                ++common[parent[p]];
            }
        }
        u32 most = kInvalidNode;
        u32 times = 0;
        for (const auto& [node, n] : common) {
            if (n > times) {
                times = n;
                most = node;
            }
        }
        for (u32 p = 0; p < pieceCount; ++p) {
            if (votes[p].empty()) {
                parent[p] = most;
            }
        }
    }
    const bool shared = !spec.cutSkeleton &&
                        std::all_of(parent.begin(), parent.end(), [&](u32 n) { return n == parent[0]; });

    // --- nodes: the helper, then a bone per piece ---
    std::string name = spec.name.empty() ? sources[0].name : spec.name;
    if (name.empty()) {
        name = "Fracture";
    }
    NodeTree& tree = model.nodes;
    if (shared) {
        result.helper = AddNodeAt(tree, UniqueName(tree, name), NodeKind::Helper, parent[0],
                                  Translation(F(centre)));
    }
    // The skeleton cut: what each piece's points rode, on the outside, where
    // the cut carried the source's skin, and how much of the piece by area.
    std::vector<std::map<u32, f64>> rides(spec.cutSkeleton ? pieceCount : 0);
    std::vector<u8> loose(rides.size(), 0);
    for (u32 i = 0; i < count && spec.cutSkeleton; ++i) {
        const Mesh& mesh = cut.meshes[i];
        const auto positions = mesh.attributes.get<Vector3f>(names::kPosition, Domain::Vertex);
        const FaceSet& faces = mesh.faceSet();
        u32 corner = 0;
        for (u32 f = 0; f < faces.faceCount(); ++f) {
            const u32 valence = faces.faceValence[f];
            std::vector<Vector3d> loop;
            for (u32 k = 0; k < valence; ++k) {
                loop.push_back(D(positions[faces.cornerVertex[corner + k]]));
            }
            const f64 area = 0.5 * Length(Newell(loop)) / valence;
            for (u32 k = 0; k < valence && cut.facePlane[i][f] == 0; ++k) {
                const u32 v = faces.cornerVertex[corner + k];
                bool bound = false;
                if (v < mesh.skin.vertexCount()) {
                    for (const Influence& influence : mesh.skin.forVertex(v)) {
                        if (influence.weight > 0.0f && influence.bone < tree.size()) {
                            rides[cut.facePiece[i][f]][influence.bone] += area * influence.weight;
                            bound = true;
                        }
                    }
                }
                loose[cut.facePiece[i][f]] = loose[cut.facePiece[i][f]] != 0 || !bound ? 1 : 0;
            }
            corner += valence;
        }
    }
    std::vector<CutSkeleton> skeletons(rides.size());
    std::multimap<u32, u32> copied;
    result.pieces.resize(pieceCount);
    for (u32 p = 0; p < pieceCount; ++p) {
        const CutPiece& piece = cut.pieces[p];
        FracturePiece& out = result.pieces[p];
        out.centroid = F(piece.centroid);
        out.volume = static_cast<f32>(piece.volume);
        out.area = static_cast<f32>(piece.area);
        out.solid = piece.solid;
        out.cells = piece.cells;
        out.targets = piece.targets;
        if (spec.cutSkeleton) {
            skeletons[p] = CutPieceSkeleton(tree, rides[p], loose[p] != 0, PieceName(name, p, pieceCount), out.centroid);
            out.node = skeletons[p].root;
            out.parent = tree.nodes[out.node].parent;
            for (const auto& [source, copy] : skeletons[p].copies) {
                copied.emplace(source, copy);
                if (copy != out.node) {
                    out.bones.push_back(copy);
                    out.sources.push_back(source);
                } else {
                    out.carrier = source;
                }
            }
            continue;
        }
        out.parent = shared ? result.helper : parent[p];
        out.node = AddNodeAt(tree, UniqueName(tree, PieceName(name, p, pieceCount)), NodeKind::Bone,
                             out.parent, Translation(out.centroid));
    }
    CopyNodeTracks(document, modelIndex, copied);
    const std::vector<u32> pieceOfNode = PieceOfNode(result, tree.size());

    // --- each target becomes its pieces ---
    std::map<u32, u32> skinIndex;
    result.made.assign(2 * count, kInvalidIndex);
    result.sources.assign(count, kInvalidIndex);
    std::vector<std::pair<u32, Mesh>> insideMeshes;
    // One square: every crack's two faces are one island, so half the inside
    // area fills the tile before the pack makes room for the padding.
    f64 square = 0.0;
    if (spec.fillingUvs == FillingUvs::Square && spec.inside != kInvalidIndex) {
        f64 area = 0.0;
        for (u32 i = 0; i < count; ++i) {
            area += InsideArea(cut.meshes[i], cut.facePlane[i]);
        }
        square = area > 0.0 ? std::sqrt(2.0 / area) : 0.0;
    }
    for (u32 i = 0; i < count; ++i) {
        const u32 target = spec.targets[i].mesh;
        Mesh out = std::move(cut.meshes[i]);
        if (out.faceCount() == 0) {
            continue;
        }
        for (MeshSection& section : out.sections) {
            section.rigidNode.reset();
        }
        // The source skin goes to the layers; every vertex rides its piece.
        const u32 vertices = out.vertexCount();
        std::vector<u32> pieceOf(vertices, kNone);
        {
            const FaceSet& faces = out.faceSet();
            u32 corner = 0;
            for (u32 f = 0; f < faces.faceCount(); ++f) {
                for (u32 k = 0; k < faces.faceValence[f]; ++k) {
                    pieceOf[faces.cornerVertex[corner + k]] = cut.facePiece[i][f];
                }
                corner += faces.faceValence[f];
            }
        }
        auto lanes = out.attributes.getOrCreate<std::array<u32, 4>>(names::kFractureSkinNode,
                                                                    Domain::Vertex, AttrType::U32x4);
        auto weights = out.attributes.getOrCreate<std::array<f32, 4>>(
            names::kFractureSkinWeight, Domain::Vertex, AttrType::F32x4);
        SkinBinding bound;
        bound.offsets.assign(1, 0);
        std::vector<std::vector<Influence>> kept(spec.cutSkeleton ? vertices : 0);
        for (u32 v = 0; v < vertices; ++v) {
            if (!out.skin.empty() && v < out.skin.vertexCount()) {
                const auto list = std::as_const(out.skin).forVertex(v);
                for (std::size_t k = 0; k < list.size() && k < 4; ++k) {
                    const auto [at, fresh] =
                        skinIndex.emplace(list[k].bone, static_cast<u32>(result.skinNodes.size()));
                    if (fresh) {
                        result.skinNodes.push_back(list[k].bone);
                    }
                    lanes[v][k] = at->second + 1;
                    weights[v][k] = list[k].weight;
                }
            }
            const u32 piece = pieceOf[v] != kNone ? pieceOf[v] : 0;
            const Influence whole{result.pieces[piece].node, 1.0f};
            if (!spec.cutSkeleton) {
                bound.appendVertex(std::span<const Influence>(&whole, 1));
                continue;
            }
            // Its own skin, on its piece's copies of the bones. What it rode
            // of a bone the piece has no copy of, the far side's at the rim of
            // the cut, goes to the piece's own bone.
            f32 own = 0.0f;
            if (!out.skin.empty() && v < out.skin.vertexCount()) {
                for (const Influence& influence : std::as_const(out.skin).forVertex(v)) {
                    if (!(influence.weight > 0.0f)) {
                        continue;
                    }
                    const auto copy = skeletons[piece].copies.find(influence.bone);
                    if (copy == skeletons[piece].copies.end() || copy->second == whole.bone) {
                        own += influence.weight;
                        if (copy == skeletons[piece].copies.end()) {
                            ++result.rebound;
                        }
                    } else {
                        kept[v].push_back(Influence{copy->second, influence.weight});
                    }
                }
            }
            if (own > 0.0f || kept[v].empty()) {
                kept[v].push_back(Influence{whole.bone, kept[v].empty() ? 1.0f : own});
            }
        }
        if (spec.cutSkeleton) {
            // An inside face's point was made by the cut and rode nothing: it
            // takes the skin of its piece's nearest point on the outside,
            // which at the crack's rim is the point it stands on.
            std::vector<u8> outside(vertices, 0);
            {
                const FaceSet& faces = out.faceSet();
                u32 corner = 0;
                for (u32 f = 0; f < faces.faceCount(); ++f) {
                    for (u32 k = 0; k < faces.faceValence[f] && cut.facePlane[i][f] == 0; ++k) {
                        outside[faces.cornerVertex[corner + k]] = 1;
                    }
                    corner += faces.faceValence[f];
                }
            }
            const auto positions = out.attributes.get<Vector3f>(names::kPosition, Domain::Vertex);
            const auto cutPoint = out.attributes.get<u8>(names::kFractureCut, Domain::Vertex);
            std::vector<std::vector<u32>> rim(pieceCount);
            for (u32 v = 0; v < vertices; ++v) {
                if (outside[v] != 0 && pieceOf[v] != kNone && v < cutPoint.size() && cutPoint[v] != 0) {
                    rim[pieceOf[v]].push_back(v);
                }
            }
            for (u32 v = 0; v < vertices; ++v) {
                if (outside[v] != 0 || pieceOf[v] == kNone) {
                    continue;
                }
                u32 nearest = kNone;
                f64 best = 0.0;
                for (const u32 other : rim[pieceOf[v]]) {
                    const Vector3d r = D(positions[other]) - D(positions[v]);
                    if (nearest == kNone || Dot(r, r) < best) {
                        best = Dot(r, r);
                        nearest = other;
                    }
                }
                if (nearest != kNone) {
                    kept[v] = kept[nearest];
                }
            }
            for (const std::vector<Influence>& list : kept) {
                bound.appendVertex(list);
            }
            bound.sortByWeight();
        }
        out.skin = std::move(bound);

        out.ensureConnectivity();
        DressInside(out, cut.facePiece[i], cut.facePlane[i], UvDensity(out), spec.uvScale, spec.seed,
                    Diagonal(sources[i].bounds), square);
        result.trianglesAfter += TriangleCount(out);

        // Another material's inside faces go in a mesh of their own.
        const bool separate =
            spec.inside != kInvalidIndex &&
            std::any_of(cut.facePlane[i].begin(), cut.facePlane[i].end(), [](u64 p) { return p != 0; });
        if (separate) {
            Mesh inside = out;
            inside.name = out.name + " Inside";
            std::vector<bool> dropOutside(out.faceCount());
            std::vector<bool> dropInside(out.faceCount());
            for (u32 f = 0; f < out.faceCount(); ++f) {
                dropOutside[f] = cut.facePlane[i][f] == 0;
                dropInside[f] = !dropOutside[f];
            }
            DropFaces(inside, dropOutside);
            DropFaces(out, dropInside);
            for (MeshSection& section : inside.sections) {
                section.materialSlot = spec.inside;
            }
            insideMeshes.emplace_back(i, std::move(inside));
        } else {
            out.invalidateConnectivity();
        }
        model.meshes[target] = std::move(out);
        result.made[2 * i] = target;
    }
    if (square > 0.0 && !insideMeshes.empty()) {
        const f32 scaled = PackFilling(insideMeshes, spec.mapSize, result.fillingIslands);
        result.fillingDensity = static_cast<f32>(square) * scaled;
    }
    for (auto& [i, inside] : insideMeshes) {
        const u32 index = static_cast<u32>(model.meshes.size());
        model.meshes.push_back(std::move(inside));
        result.made[2 * i + 1] = index;
        CopySectionTracks(document, modelIndex, spec.targets[i].mesh, index);
    }
    // After the copies above, which take a target's tracks section for section.
    for (const u32 made : result.made) {
        if (made == kInvalidIndex) {
            continue;
        }
        for (const auto& [to, from] : DivideSections(model.meshes[made], spec.mostVertices, pieceOfNode)) {
            CloneSectionChannels(document, modelIndex, made, from, to);
            ++result.sections;
        }
    }
    if (spec.keepWhole) {
        for (u32 i = 0; i < count; ++i) {
            Mesh whole = std::move(sources[i]);
            whole.name += " (whole)";
            for (MeshSection& section : whole.sections) {
                section.profiles = 0;
            }
            result.sources[i] = static_cast<u32>(model.meshes.size());
            model.meshes.push_back(std::move(whole));
        }
    }
    for (const Vector3d& s : seeds) {
        result.seeds.push_back(F(s));
    }
    result.links = std::move(cut.links);
    result.failedFaces = cut.failedFaces;
    result.merged = cut.merged;
    return result;
}

// ============================================================================
// RejoinPieces
// ============================================================================

RejoinReport RejoinPieces(Mesh& mesh, std::span<const u32> skinNodes) {
    RejoinReport report;
    if (!mesh.attributes.has(names::kFractureSource, Domain::Face)) {
        return report;
    }
    Mesh m = mesh;
    m.invalidateConnectivity();
    if (!m.ensureConnectivity().ok()) {
        return report;
    }
    const auto positions = m.attributes.get<Vector3f>(names::kPosition, Domain::Vertex);
    const auto made = m.attributes.get<u8>(names::kFractureMade, Domain::Face);
    const auto source = m.attributes.get<u32>(names::kFractureSource, Domain::Face);
    const auto cut = m.attributes.get<u8>(names::kFractureCut, Domain::Vertex);
    const auto weld = m.attributes.get<u32>(names::kFractureWeld, Domain::Vertex);
    const auto lanes = m.attributes.get<std::array<u32, 4>>(names::kFractureSkinNode, Domain::Vertex);
    const auto laneWeights =
        m.attributes.get<std::array<f32, 4>>(names::kFractureSkinWeight, Domain::Vertex);
    const u32 vertexCount = m.vertexCount();
    const u32 faceCount = m.faceCount();
    m.recomputeBounds();
    const f64 tolerance = 1e-5 * Diagonal(m.bounds);
    const auto isCut = [&](u32 v) { return v < cut.size() && cut[v] != 0; };

    // Points: a cut point is one with every cut point at its place.
    std::vector<u32> point(vertexCount);
    {
        std::map<std::array<f32, 3>, u32> first;
        for (u32 v = 0; v < vertexCount; ++v) {
            if (isCut(v)) {
                const auto [at, fresh] =
                    first.emplace(std::array<f32, 3>{positions[v].x + 0.0f, positions[v].y + 0.0f,
                                                     positions[v].z + 0.0f},
                                  v);
                point[v] = at->second;
            } else {
                point[v] = v;
            }
        }
    }

    struct Corner {
        u32 vertex = 0;
        HalfedgeId from; ///< Whose corner values it takes, and whose edge.
    };
    struct OutFace {
        u32 from = 0; ///< The face whose face values it takes.
        std::vector<Corner> corners;
    };
    std::vector<OutFace> faces;
    std::map<u32, std::vector<u32>> groups;
    std::vector<u32> added;
    for (u32 f = 0; f < faceCount; ++f) {
        if (f < made.size() && made[f] != 0) {
            continue;
        }
        const u32 s = f < source.size() ? source[f] : 0;
        if (s == 0) {
            added.push_back(f);
        } else {
            groups[s].push_back(f);
        }
    }
    const auto asItIs = [&](u32 f) {
        OutFace face;
        face.from = f;
        const Loop loop = LoopOf(m, f);
        for (std::size_t k = 0; k < loop.vertices.size(); ++k) {
            face.corners.push_back(Corner{point[loop.vertices[k]], loop.halfedges[k]});
        }
        return face;
    };

    // The float and colour corner layers, for the linear test.
    std::vector<const AttrLayer*> linear;
    for (const AttrLayer& layer : m.attributes.layers()) {
        if (layer.domain == Domain::Halfedge &&
            (layer.type == AttrType::F32 || layer.type == AttrType::F32x2 ||
             layer.type == AttrType::F32x3 || layer.type == AttrType::F32x4 ||
             layer.type == AttrType::U8x4)) {
            linear.push_back(&layer);
        }
    }
    const auto linearAt = [&](HalfedgeId p, HalfedgeId x, HalfedgeId n, f64 t) {
        for (const AttrLayer* layer : linear) {
            const u32 stride = AttrTypeSize(layer->type);
            const u32 components = AttrTypeComponents(layer->type);
            const u8* a = layer->data.data() + static_cast<std::size_t>(p.value()) * stride;
            const u8* b = layer->data.data() + static_cast<std::size_t>(x.value()) * stride;
            const u8* c = layer->data.data() + static_cast<std::size_t>(n.value()) * stride;
            f64 held[4] = {0.0, 0.0, 0.0, 0.0};
            f64 blend[4] = {0.0, 0.0, 0.0, 0.0};
            f64 slack[4] = {0.0, 0.0, 0.0, 0.0};
            for (u32 k = 0; k < components && k < 4; ++k) {
                f64 va;
                f64 vc;
                if (layer->type == AttrType::U8x4) {
                    va = a[k];
                    held[k] = b[k];
                    vc = c[k];
                    slack[k] = 1.5;
                } else {
                    f32 fa;
                    f32 fb;
                    f32 fc;
                    std::memcpy(&fa, a + 4 * k, 4);
                    std::memcpy(&fb, b + 4 * k, 4);
                    std::memcpy(&fc, c + 4 * k, 4);
                    va = fa;
                    held[k] = fb;
                    vc = fc;
                    slack[k] = 1e-3 * (1.0 + std::fabs(va) + std::fabs(vc));
                }
                blend[k] = va + t * (vc - va);
            }
            // A direction is blended and then brought back to unit length
            // (`BlendCorners`), so that is what a cut point across a smooth
            // surface holds: against the bare line it would never dissolve.
            if (layer->type != AttrType::U8x4 && components >= 3 &&
                (layer->name == names::kNormal || layer->name == names::kBinormal ||
                 layer->name == names::kTangent)) {
                const f64 length =
                    std::sqrt(blend[0] * blend[0] + blend[1] * blend[1] + blend[2] * blend[2]);
                if (length > 1e-20) {
                    blend[0] /= length;
                    blend[1] /= length;
                    blend[2] /= length;
                }
            }
            for (u32 k = 0; k < components && k < 4; ++k) {
                if (std::fabs(held[k] - blend[k]) > slack[k]) {
                    return false;
                }
            }
        }
        return true;
    };

    for (const auto& [s, fragments] : groups) {
        bool anyCut = false;
        for (u32 f : fragments) {
            for (u32 v : LoopOf(m, f).vertices) {
                anyCut = anyCut || isCut(v);
            }
        }
        if (fragments.size() == 1 && !anyCut) {
            faces.push_back(asItIs(fragments[0]));
            continue;
        }
        // The fragments' union: their edges, each cancelled by its reverse.
        struct Edge {
            u32 from;
            u32 to;
            HalfedgeId halfedge;
            u32 vertex;
        };
        std::vector<Edge> edges;
        std::map<std::pair<u32, u32>, i32> count;
        for (u32 f : fragments) {
            const Loop loop = LoopOf(m, f);
            const std::size_t n = loop.vertices.size();
            for (std::size_t k = 0; k < n; ++k) {
                const u32 a = point[loop.vertices[k]];
                const u32 b = point[loop.vertices[(k + 1) % n]];
                if (a == b) {
                    continue;
                }
                edges.push_back(Edge{a, b, loop.halfedges[k], loop.vertices[k]});
                ++count[{a, b}];
            }
        }
        std::map<u32, std::vector<u32>> leaving;
        std::map<std::pair<u32, u32>, i32> used;
        for (u32 e = 0; e < edges.size(); ++e) {
            const auto key = std::make_pair(edges[e].from, edges[e].to);
            const auto back = count.find({edges[e].to, edges[e].from});
            const i32 net = count[key] - (back == count.end() ? 0 : back->second);
            if (net > 0 && used[key] < net) {
                ++used[key];
                leaving[edges[e].from].push_back(e);
            }
        }
        bool single = !leaving.empty();
        for (const auto& [p, list] : leaving) {
            single = single && list.size() == 1;
        }
        std::vector<u32> walk;
        if (single) {
            u32 e = leaving.begin()->second.front();
            const u32 start = edges[e].from;
            for (std::size_t guard = 0; guard <= leaving.size(); ++guard) {
                walk.push_back(e);
                const auto next = leaving.find(edges[e].to);
                if (next == leaving.end()) {
                    single = false;
                    break;
                }
                e = next->second.front();
                if (edges[e].from == start) {
                    break;
                }
            }
            single = single && walk.size() == leaving.size();
        }
        if (!single || walk.size() < 3) {
            report.fragments += static_cast<u32>(fragments.size());
            for (u32 f : fragments) {
                faces.push_back(asItIs(f));
            }
            continue;
        }
        // Dissolve the cut points left on a straight, linear edge.
        const auto between = [&](const Edge& p, const Edge& x, const Edge& n) {
            const Vector3d pp = D(positions[p.vertex]);
            const Vector3d px = D(positions[x.vertex]);
            const Vector3d pn = D(positions[n.vertex]);
            const Vector3d along = pn - pp;
            const f64 length2 = Dot(along, along);
            if (!(length2 > 0.0)) {
                return false;
            }
            const f64 t = Dot(px - pp, along) / length2;
            const f64 off = Length(cross(px - pp, along)) / std::sqrt(length2);
            return t > 0.0 && t < 1.0 && off <= tolerance && linearAt(p.halfedge, x.halfedge, n.halfedge, t);
        };
        bool changed = true;
        while (changed && walk.size() > 3) {
            changed = false;
            for (std::size_t i = 0; i < walk.size() && walk.size() > 3; ++i) {
                const Edge& x = edges[walk[i]];
                if (!isCut(x.vertex)) {
                    continue;
                }
                const std::size_t size = walk.size();
                std::size_t before = (i + size - 1) % size;
                std::size_t after = (i + 1) % size;
                bool straight = between(edges[walk[before]], x, edges[walk[after]]);
                if (!straight) {
                    // Then between the ends of the source edge it was cut on,
                    // whose blend is what it holds. Small pieces put several
                    // cuts on one edge, and a direction blended from two of
                    // them is not the direction blended from the ends.
                    while (before != i && isCut(edges[walk[before]].vertex)) {
                        before = (before + size - 1) % size;
                    }
                    while (after != i && isCut(edges[walk[after]].vertex)) {
                        after = (after + 1) % size;
                    }
                    straight = before != i && after != i && before != after &&
                               between(edges[walk[before]], x, edges[walk[after]]);
                }
                if (!straight) {
                    continue;
                }
                walk.erase(walk.begin() + static_cast<std::ptrdiff_t>(i));
                changed = true;
                --i;
            }
        }
        OutFace face;
        face.from = fragments.front();
        for (u32 e : walk) {
            face.corners.push_back(Corner{point[edges[e].vertex], edges[e].halfedge});
            report.cutPoints += isCut(edges[e].vertex) ? 1 : 0;
        }
        faces.push_back(std::move(face));
    }
    for (u32 f : added) {
        faces.push_back(asItIs(f));
    }

    // --- the mesh, from the records ---
    std::vector<u32> newVertex(vertexCount, kNone);
    std::vector<u32> oldVertex;
    FaceSet set;
    for (OutFace& face : faces) {
        std::vector<u32> loop;
        for (const Corner& c : face.corners) {
            if (newVertex[c.vertex] == kNone) {
                newVertex[c.vertex] = static_cast<u32>(oldVertex.size());
                oldVertex.push_back(c.vertex);
            }
            loop.push_back(newVertex[c.vertex]);
        }
        set.addFace(loop);
    }
    set.vertexCount = static_cast<u32>(oldVertex.size());
    RepairResult repaired = Repair(set);
    oldVertex.resize(repaired.faces.vertexCount);
    for (const VertexSplit& split : repaired.log.splits) {
        if (split.created < oldVertex.size() && split.original < oldVertex.size()) {
            oldVertex[split.created] = oldVertex[split.original];
        }
    }
    std::vector<u32> survivor;
    {
        std::size_t dropped = 0;
        for (u32 f = 0; f < faces.size(); ++f) {
            if (dropped < repaired.log.droppedFaces.size() &&
                repaired.log.droppedFaces[dropped].index == f) {
                ++dropped;
                continue;
            }
            survivor.push_back(f);
        }
    }
    Mesh out;
    out.name = m.name;
    out.lodLevel = m.lodLevel;
    out.sections = m.sections;
    out.setFaceSet(repaired.faces);
    if (!out.ensureConnectivity().ok()) {
        return report;
    }
    const auto isFracture = [](const std::string& name) { return name.rfind("fracture.", 0) == 0; };
    for (const AttrLayer& layer : m.attributes.layers()) {
        if (!isFracture(layer.name)) {
            out.attributes.create(layer.name, layer.domain, layer.type, layer.storage);
        }
    }
    const auto copyBytes = [](const AttrLayer& s, AttrLayer& o, u32 from, u32 to) {
        const u32 stride = AttrTypeSize(s.type);
        if ((from + 1) * static_cast<std::size_t>(stride) <= s.data.size() &&
            (to + 1) * static_cast<std::size_t>(stride) <= o.data.size()) {
            std::memcpy(o.data.data() + static_cast<std::size_t>(to) * stride,
                        s.data.data() + static_cast<std::size_t>(from) * stride, stride);
        }
    };
    const Topology& built = std::as_const(out).topology();
    for (const AttrLayer& layer : m.attributes.layers()) {
        if (isFracture(layer.name)) {
            continue;
        }
        AttrLayer* target = out.attributes.layer(layer.name, layer.domain);
        if (target == nullptr) {
            continue;
        }
        switch (layer.domain) {
        case Domain::Vertex:
            for (u32 v = 0; v < oldVertex.size(); ++v) {
                copyBytes(layer, *target, oldVertex[v], v);
            }
            break;
        case Domain::Face:
            for (u32 f = 0; f < survivor.size(); ++f) {
                copyBytes(layer, *target, faces[survivor[f]].from, f);
            }
            break;
        case Domain::Halfedge:
        case Domain::Edge:
            for (u32 f = 0; f < survivor.size(); ++f) {
                HalfedgeId h = built.halfedge(FaceId(f));
                for (const Corner& c : faces[survivor[f]].corners) {
                    if (layer.domain == Domain::Halfedge) {
                        copyBytes(layer, *target, c.from.value(), h.value());
                    } else {
                        copyBytes(layer, *target, Topology::edge(c.from).value(),
                                  Topology::edge(h).value());
                    }
                    h = built.next(h);
                }
            }
            break;
        case Domain::Mesh:
            target->data = layer.data;
            break;
        default:
            break;
        }
    }
    // The sections divided for a file's index width are one again.
    if (const auto origin = m.attributes.get<u32>(names::kFractureSection, Domain::Face); !origin.empty()) {
        const std::vector<u32> before = SectionsBefore(m);
        const std::span<u32> sectionOf = out.faceSections();
        for (u32 f = 0; f < survivor.size() && f < sectionOf.size(); ++f) {
            const u32 from = faces[survivor[f]].from;
            const u32 was = from < origin.size() && origin[from] != 0 ? origin[from] - 1 : sectionOf[f];
            sectionOf[f] = was < before.size() && before[was] != kInvalidIndex ? before[was] : 0;
        }
        for (std::size_t s = std::min(before.size(), out.sections.size()); s-- > 0;) {
            if (before[s] == kInvalidIndex) {
                out.sections.erase(out.sections.begin() + static_cast<std::ptrdiff_t>(s));
            }
        }
    }
    // Merge groups and the skin, as the source had them.
    {
        auto groups = MergeGroupsOf(out);
        u32 fresh = 0;
        for (u32 v = 0; v < oldVertex.size(); ++v) {
            const u32 w = oldVertex[v];
            if (w < weld.size() && weld[w] != 0) {
                fresh = std::max(fresh, weld[w]);
            }
        }
        std::map<u32, u32> freshOf;
        for (u32 v = 0; v < oldVertex.size(); ++v) {
            const u32 w = oldVertex[v];
            if (w < weld.size() && weld[w] != 0) {
                groups[v] = weld[w] - 1;
            } else {
                const auto [at, made] = freshOf.emplace(w, fresh);
                fresh += made ? 1 : 0;
                groups[v] = at->second;
            }
        }
    }
    // A point the source had, or a cut made, takes the skin the layers kept,
    // none included; a point added by hand keeps its own.
    const bool fromLayers = !lanes.empty() && !laneWeights.empty();
    {
        std::vector<std::vector<Influence>> lists(oldVertex.size());
        bool any = false;
        for (u32 v = 0; v < oldVertex.size(); ++v) {
            const u32 w = oldVertex[v];
            const bool kept = (w < weld.size() && weld[w] != 0) || isCut(w);
            if (kept && fromLayers && w < lanes.size()) {
                for (u32 k = 0; k < 4; ++k) {
                    const u32 index = lanes[w][k];
                    if (index != 0 && index - 1 < skinNodes.size() &&
                        skinNodes[index - 1] != kInvalidNode) {
                        lists[v].push_back(Influence{skinNodes[index - 1], laneWeights[w][k]});
                    }
                }
            } else if (!kept && !m.skin.empty() && w < m.skin.vertexCount()) {
                const auto own = std::as_const(m.skin).forVertex(w);
                lists[v].assign(own.begin(), own.end());
            }
            any = any || !lists[v].empty();
        }
        if (any) {
            out.skin.offsets.assign(1, 0);
            for (const auto& list : lists) {
                out.skin.appendVertex(list);
            }
            out.skin.sortByWeight();
        }
    }
    MaterialiseRows(out);
    out.invalidateConnectivity();
    out.recomputeBounds();
    mesh = std::move(out);
    return report;
}

std::vector<u32> SectionsBefore(const Mesh& made) {
    // Added: every face in it was moved there. One a face was put in by hand
    // since is the user's, and stays.
    const auto origin = made.attributes.get<u32>(names::kFractureSection, Domain::Face);
    const auto sectionOf = made.faceSections();
    std::vector<u8> moved(made.sections.size(), 0);
    std::vector<u8> own(made.sections.size(), 0);
    for (std::size_t f = 0; f < sectionOf.size(); ++f) {
        if (sectionOf[f] < moved.size()) {
            (f < origin.size() && origin[f] != 0 ? moved : own)[sectionOf[f]] = 1;
        }
    }
    std::vector<u32> before(made.sections.size());
    u32 next = 0;
    for (std::size_t s = 0; s < before.size(); ++s) {
        before[s] = moved[s] != 0 && own[s] == 0 ? kInvalidIndex : next++;
    }
    return before;
}

std::vector<u32> PieceOfNode(const FractureResult& result, u32 count) {
    std::vector<u32> piece(count, kInvalidIndex);
    for (u32 p = 0; p < result.pieces.size(); ++p) {
        if (result.pieces[p].node < count) {
            piece[result.pieces[p].node] = p;
        }
        for (const u32 bone : result.pieces[p].bones) {
            if (bone < count) {
                piece[bone] = p;
            }
        }
    }
    return piece;
}

void RestoreSource(Mesh& whole, const Mesh& made) {
    whole.name = made.name;
    for (std::size_t i = 0; i < whole.sections.size() && i < made.sections.size(); ++i) {
        whole.sections[i].profiles = made.sections[i].profiles;
    }
}

} // namespace fracture
} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
