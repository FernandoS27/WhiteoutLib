// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/geometry/fracture/fracture.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
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

/// The inside faces' UVs, colours and tangents (§6.3).
void DressInside(Mesh& mesh, std::span<const u32> facePiece, std::span<const u64> facePlane,
                 f64 density, f32 uvScale, u32 seed, f64 size) {
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
        const f64 extent = density > 0.0 ? 1.0 / (density * std::max(uvScale, 1e-6f))
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
            const f64 h0 = HashUnit(FractureHash(seed, static_cast<u32>(plane >> 32), plane & 0xFFFFFFFFu));
            const f64 h1 = HashUnit(FractureHash(seed, static_cast<u32>(plane >> 32),
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

} // namespace

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
    std::vector<Mesh> sources(count);
    std::vector<Mesh> work(count);
    for (u32 i = 0; i < count; ++i) {
        sources[i] = model.meshes[spec.targets[i].mesh];
        sources[i].recomputeBounds();
        work[i] = sources[i];
        result.trianglesBefore += TriangleCount(sources[i]);
        if (spec.openParts == OpenParts::Thicken && spec.targets[i].pieces > 1) {
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

    // --- seeds, cells, cut ---
    std::vector<Vector3d> seeds;
    if (!spec.seeds.empty()) {
        for (const Vector3f& s : spec.seeds) {
            seeds.push_back(D(s));
        }
    } else {
        std::vector<SeedTarget> seedTargets(count);
        for (u32 i = 0; i < count; ++i) {
            seedTargets[i].winding = &winding[i];
            seedTargets[i].surface = &surfaces[i];
            seedTargets[i].pieces = spec.targets[i].pieces;
        }
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
    const VoronoiDiagram diagram = VoronoiCells(seeds, low - margin, high + margin, voronoi);
    std::vector<CutTarget> targets(count);
    for (u32 i = 0; i < count; ++i) {
        targets[i].mesh = &work[i];
        targets[i].winding = &winding[i];
        targets[i].whole = spec.targets[i].pieces < 2;
    }
    CutOptions cutOptions;
    cutOptions.smallest = spec.smallest;
    cutOptions.threads = spec.threads;
    CutResult cut = CutPieces(targets, diagram, cutOptions);
    if (cut.pieces.empty()) {
        result.refusal = Refusal::NoPieces;
        return result;
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
    const bool shared = std::all_of(parent.begin(), parent.end(), [&](u32 n) { return n == parent[0]; });

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
    result.pieces.resize(pieceCount);
    for (u32 p = 0; p < pieceCount; ++p) {
        const CutPiece& piece = cut.pieces[p];
        FracturePiece& out = result.pieces[p];
        out.parent = shared ? result.helper : parent[p];
        out.centroid = F(piece.centroid);
        out.volume = static_cast<f32>(piece.volume);
        out.area = static_cast<f32>(piece.area);
        out.solid = piece.solid;
        out.cells = piece.cells;
        out.targets = piece.targets;
        out.node = AddNodeAt(tree, UniqueName(tree, PieceName(name, p, pieceCount)), NodeKind::Bone,
                             out.parent, Translation(out.centroid));
    }

    // --- each target becomes its pieces ---
    std::map<u32, u32> skinIndex;
    result.made.assign(2 * count, kInvalidIndex);
    result.sources.assign(count, kInvalidIndex);
    std::vector<std::pair<u32, Mesh>> insideMeshes;
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
            bound.appendVertex(std::span<const Influence>(&whole, 1));
        }
        out.skin = std::move(bound);

        out.ensureConnectivity();
        DressInside(out, cut.facePiece[i], cut.facePlane[i], UvDensity(out), spec.uvScale, spec.seed,
                    Diagonal(sources[i].bounds));
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
    for (auto& [i, inside] : insideMeshes) {
        const u32 index = static_cast<u32>(model.meshes.size());
        model.meshes.push_back(std::move(inside));
        result.made[2 * i + 1] = index;
        CopySectionTracks(document, modelIndex, spec.targets[i].mesh, index);
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
            for (u32 k = 0; k < components; ++k) {
                f64 va;
                f64 vb;
                f64 vc;
                f64 slack;
                if (layer->type == AttrType::U8x4) {
                    va = a[k];
                    vb = b[k];
                    vc = c[k];
                    slack = 1.5;
                } else {
                    f32 fa;
                    f32 fb;
                    f32 fc;
                    std::memcpy(&fa, a + 4 * k, 4);
                    std::memcpy(&fb, b + 4 * k, 4);
                    std::memcpy(&fc, c + 4 * k, 4);
                    va = fa;
                    vb = fb;
                    vc = fc;
                    slack = 1e-3 * (1.0 + std::fabs(va) + std::fabs(vc));
                }
                if (std::fabs(vb - (va + t * (vc - va))) > slack) {
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
        bool changed = true;
        while (changed && walk.size() > 3) {
            changed = false;
            for (std::size_t i = 0; i < walk.size() && walk.size() > 3; ++i) {
                const Edge& x = edges[walk[i]];
                if (!isCut(x.vertex)) {
                    continue;
                }
                const Edge& p = edges[walk[(i + walk.size() - 1) % walk.size()]];
                const Edge& n = edges[walk[(i + 1) % walk.size()]];
                const Vector3d pp = D(positions[p.vertex]);
                const Vector3d px = D(positions[x.vertex]);
                const Vector3d pn = D(positions[n.vertex]);
                const Vector3d along = pn - pp;
                const f64 length2 = Dot(along, along);
                if (!(length2 > 0.0)) {
                    continue;
                }
                const f64 t = Dot(px - pp, along) / length2;
                const f64 off = Length(cross(px - pp, along)) / std::sqrt(length2);
                if (t <= 0.0 || t >= 1.0 || off > tolerance) {
                    continue;
                }
                if (!linearAt(p.halfedge, x.halfedge, n.halfedge, t)) {
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
