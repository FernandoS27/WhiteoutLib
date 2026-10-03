// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/physics/cloth_cage.h>

#include <whiteout/models/wem/geometry/builder.h>
#include <whiteout/models/wem/geometry/modelling.h>
#include <whiteout/models/wem/geometry/ops.h>
#include <whiteout/models/wem/meshes/remove.h>
#include <whiteout/models/wem/physics/materials.h>
#include <whiteout/models/wem/physics/references.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <queue>
#include <set>
#include <unordered_map>
#include <variant>

namespace whiteout {
namespace models {
namespace wem {

namespace {

u32 Remapped(std::span<const u32> remap, u32 section) {
    return section < remap.size() ? remap[section] : section;
}

f32 Distance(const Vector3f& a, const Vector3f& b) {
    return (a - b).length();
}

/// Where @p node stands at rest, in model space.
Vector3f RestPivot(const NodeTree& tree, u32 node) {
    if (node >= tree.size()) {
        return {0, 0, 0};
    }
    if (tree.rig == RigConvention::PivotRelative) {
        return tree.nodes[node].pivot;
    }
    const Matrix44f frame = ToMatrix(tree.worldBind(node));
    return {frame.data[3][0], frame.data[3][1], frame.data[3][2]};
}

bool Contains(std::span<const u32> list, u32 value) {
    return std::find(list.begin(), list.end(), value) != list.end();
}

/// The closest point to @p p on triangle @p a @p b @p c, as barycentric
/// weights (Ericson, Real-Time Collision Detection §5.1.5).
std::array<f32, 3> ClosestOnTriangle(const Vector3f& p, const Vector3f& a, const Vector3f& b, const Vector3f& c) {
    const Vector3f ab = b - a, ac = c - a, ap = p - a;
    const f32 d1 = ab.dot(ap), d2 = ac.dot(ap);
    if (d1 <= 0.0f && d2 <= 0.0f) {
        return {1, 0, 0};
    }
    const Vector3f bp = p - b;
    const f32 d3 = ab.dot(bp), d4 = ac.dot(bp);
    if (d3 >= 0.0f && d4 <= d3) {
        return {0, 1, 0};
    }
    const f32 vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        const f32 v = d1 / (d1 - d3);
        return {1 - v, v, 0};
    }
    const Vector3f cp = p - c;
    const f32 d5 = ab.dot(cp), d6 = ac.dot(cp);
    if (d6 >= 0.0f && d5 <= d6) {
        return {0, 0, 1};
    }
    const f32 vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        const f32 w = d2 / (d2 - d6);
        return {1 - w, 0, w};
    }
    const f32 va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        const f32 w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return {0, 1 - w, w};
    }
    const f32 denom = 1.0f / (va + vb + vc);
    const f32 v = vb * denom, w = vc * denom;
    return {1 - v - w, v, w};
}

/// The smallest angle of a triangle, in radians; 0 when it is degenerate.
f32 MinAngle(const Vector3f& a, const Vector3f& b, const Vector3f& c) {
    const auto angle = [](const Vector3f& at, const Vector3f& p, const Vector3f& q) {
        const Vector3f u = p - at, v = q - at;
        const f32 lengths = u.length() * v.length();
        if (lengths <= 0.0f) {
            return 0.0f;
        }
        return std::acos(std::clamp(u.dot(v) / lengths, -1.0f, 1.0f));
    };
    return std::min({angle(a, b, c), angle(b, c, a), angle(c, a, b)});
}

Vector3f Normal(const Vector3f& a, const Vector3f& b, const Vector3f& c) {
    return cross(b - a, c - a);
}

/// A position welded to a grid of @p cell, so near points hash near.
struct WeldGrid {
    f32 cell = 1.0f;
    std::unordered_map<u64, std::vector<u32>> cells;

    static u64 Key(i64 x, i64 y, i64 z) {
        const auto pack = [](i64 v) { return static_cast<u64>(v + (1 << 20)) & 0x1FFFFFu; };
        return pack(x) | (pack(y) << 21) | (pack(z) << 42);
    }
    std::array<i64, 3> At(const Vector3f& p) const {
        return {static_cast<i64>(std::floor(p.x / cell)), static_cast<i64>(std::floor(p.y / cell)),
                static_cast<i64>(std::floor(p.z / cell))};
    }
    /// The first of @p points within @p cell of @p p, or `kInvalidIndex`.
    u32 find(const Vector3f& p, std::span<const Vector3f> points) const {
        const std::array<i64, 3> at = At(p);
        for (i64 dx = -1; dx <= 1; ++dx) {
            for (i64 dy = -1; dy <= 1; ++dy) {
                for (i64 dz = -1; dz <= 1; ++dz) {
                    const auto found = cells.find(Key(at[0] + dx, at[1] + dy, at[2] + dz));
                    if (found == cells.end()) {
                        continue;
                    }
                    for (const u32 index : found->second) {
                        if (Distance(points[index], p) <= cell) {
                            return index;
                        }
                    }
                }
            }
        }
        return kInvalidIndex;
    }
    void add(const Vector3f& p, u32 index) {
        const std::array<i64, 3> at = At(p);
        cells[Key(at[0], at[1], at[2])].push_back(index);
    }
};

/// The vertices each face of @p mesh uses, by face slot, and the triangles it
/// is drawn as.
struct FaceCorners {
    std::vector<std::size_t> first;
    const geom::FaceSet* faces = nullptr;

    explicit FaceCorners(const Mesh& mesh) : faces(&mesh.faceSet()) {
        first.resize(faces->faceValence.size());
        for (std::size_t f = 0, corner = 0; f < faces->faceValence.size(); corner += faces->faceValence[f], ++f) {
            first[f] = corner;
        }
    }
    std::span<const u32> of(u32 face) const {
        return std::span<const u32>(faces->cornerVertex.data() + first[face], faces->faceValence[face]);
    }
};

/// Face @p face's triangles, by vertex: its stored triangulation, else a fan.
std::vector<std::array<u32, 3>> FaceTriangles(const Mesh& mesh, const FaceCorners& corners, u32 face) {
    std::vector<std::array<u32, 3>> out;
    const std::span<const u32> row = mesh.triangulation.row(face);
    if (row.size() >= 3) {
        for (std::size_t k = 0; k + 2 < row.size(); k += 3) {
            out.push_back({row[k], row[k + 1], row[k + 2]});
        }
        return out;
    }
    const std::span<const u32> loop = corners.of(face);
    for (std::size_t k = 1; k + 1 < loop.size(); ++k) {
        out.push_back({loop[0], loop[k], loop[k + 1]});
    }
    return out;
}

} // namespace

// ============================================================================
// Sections
// ============================================================================

std::vector<u32> SectionFaces(const Mesh& mesh, u32 section) {
    std::vector<u32> faces;
    const std::span<const u32> sections = mesh.faceSections();
    for (u32 f = 0; f < sections.size(); ++f) {
        if (sections[f] == section) {
            faces.push_back(f);
        }
    }
    return faces;
}

void FollowSectionRemap(Document& document, u32 model, u32 mesh, std::span<const u32> remap, Diagnostics& out) {
    if (model >= document.models.size()) {
        return;
    }
    Model& owner = document.models[model];
    std::vector<u32> erased;
    for (AnimChannel& channel : owner.animChannels.channels) {
        if (channel.target.kind != TrackTarget::Kind::Section || channel.target.mesh != mesh) {
            continue;
        }
        const u32 to = Remapped(remap, channel.target.sub);
        if (to == kInvalidIndex) {
            erased.push_back(channel.id);
        } else {
            channel.target.sub = to;
        }
    }
    EraseChannels(document, model, erased);
    if (mesh == 0) {
        for (Node& node : owner.nodes.nodes) {
            if (auto* emitter = std::get_if<Sc2ParticleEmitterPayload>(&node.payload)) {
                std::erase_if(emitter->shapeSections, [&](u32 s) { return Remapped(remap, s) == kInvalidIndex; });
                for (u32& s : emitter->shapeSections) {
                    s = Remapped(remap, s);
                }
            }
        }
    }
    const std::vector<u32> gone = RemapPhysicsSections(owner.physics, mesh, remap);
    InvalidatePhysicsChannels(owner.animChannels, gone, out);
    DetachPoseStages(owner.poseStages, gone);
}

bool EraseSection(Document& document, u32 model, u32 mesh, u32 section, Diagnostics& out,
                  std::vector<u32>* faces) {
    if (model >= document.models.size() || mesh >= document.models[model].meshes.size()) {
        return false;
    }
    Mesh& target = document.models[model].meshes[mesh];
    const u32 count = static_cast<u32>(target.sections.size());
    if (section >= count) {
        return false;
    }
    geom::ElementSet gone;
    gone.faces = SectionFaces(target, section);
    if (!gone.faces.empty()) {
        geom::ModelPlan plan = geom::PlanDelete(target, gone);
        if (plan.refused()) {
            return false;
        }
        geom::FinishTool(target, plan);
        // The compaction keeps the order: a face moves down by the faces that
        // went before it, in the caller's order; one that went is invalid.
        if (faces != nullptr) {
            for (u32& f : *faces) {
                const auto below = std::lower_bound(gone.faces.begin(), gone.faces.end(), f);
                f = below != gone.faces.end() && *below == f
                        ? kInvalidIndex
                        : f - static_cast<u32>(below - gone.faces.begin());
            }
        }
    }
    std::vector<u32> remap(count);
    for (u32 s = 0; s < count; ++s) {
        remap[s] = s < section ? s : s == section ? kInvalidIndex : s - 1;
    }
    for (u32& s : target.faceSections()) {
        s = Remapped(remap, s);
    }
    std::erase_if(target.repairLog.droppedFaces,
                  [&](const geom::FaceRecord& record) { return record.section == section; });
    for (geom::FaceRecord& record : target.repairLog.droppedFaces) {
        record.section = Remapped(remap, record.section);
    }
    target.sections.erase(target.sections.begin() + section);
    target.recomputeBounds();
    FollowSectionRemap(document, model, mesh, remap, out);
    return true;
}

u32 CloneSectionChannels(Document& document, u32 model, u32 mesh, u32 from, u32 to) {
    AnimChannelTable& table = document.models[model].animChannels;
    std::vector<std::pair<u32, u32>> ids;
    const std::size_t count = table.channels.size();
    for (std::size_t c = 0; c < count; ++c) {
        const TrackTarget& target = table.channels[c].target;
        if (target.kind != TrackTarget::Kind::Section || target.mesh != mesh || target.sub != from) {
            continue;
        }
        AnimChannel clone = table.channels[c];
        clone.id = table.nextFreeId();
        clone.target.sub = to;
        ids.emplace_back(table.channels[c].id, clone.id);
        table.add(clone);
    }
    for (Clip& clip : document.clips) {
        if (clip.model != model) {
            continue;
        }
        for (SubTrackContainer& container : clip.containers) {
            const std::size_t tracks = container.subTracks.size();
            for (std::size_t t = 0; t < tracks; ++t) {
                for (const auto& [was, now] : ids) {
                    if (container.subTracks[t].channel == was) {
                        SubTrack copy = container.subTracks[t];
                        copy.channel = now;
                        container.subTracks.push_back(std::move(copy));
                    }
                }
            }
        }
    }
    return static_cast<u32>(ids.size());
}

// ============================================================================
// Choosing
// ============================================================================

std::vector<f32> BoneShare(const Mesh& mesh, std::span<const u32> bones) {
    std::vector<f32> share(mesh.vertexCount(), 0.0f);
    for (u32 v = 0; v < share.size() && v < mesh.skin.vertexCount(); ++v) {
        f32 on = 0.0f, total = 0.0f;
        for (const geom::Influence& influence : mesh.skin.forVertex(v)) {
            total += influence.weight;
            on += Contains(bones, influence.bone) ? influence.weight : 0.0f;
        }
        share[v] = total > 0.0f ? on / total : 0.0f;
    }
    return share;
}

std::vector<u32> FacesByBone(const Mesh& mesh, std::span<const u32> bones) {
    std::vector<u32> faces;
    if (bones.empty()) {
        return faces;
    }
    const std::vector<f32> share = BoneShare(mesh, bones);
    const FaceCorners corners(mesh);
    for (u32 f = 0; f < corners.first.size(); ++f) {
        f32 sum = 0.0f;
        const std::span<const u32> loop = corners.of(f);
        for (const u32 v : loop) {
            sum += v < share.size() ? share[v] : 0.0f;
        }
        if (!loop.empty() && sum >= 0.5f * static_cast<f32>(loop.size())) {
            faces.push_back(f);
        }
    }
    return faces;
}

std::vector<u32> ProposeClothBones(const Model& model, u32 mesh, std::span<const u32> faces) {
    if (mesh >= model.meshes.size()) {
        return {};
    }
    const Mesh& target = model.meshes[mesh];
    std::vector<u8> onCloth(target.vertexCount(), 0);
    const FaceCorners corners(target);
    for (const u32 f : faces) {
        if (f < corners.first.size()) {
            for (const u32 v : corners.of(f)) {
                onCloth[v] = 1;
            }
        }
    }
    std::vector<f32> total(model.nodes.size(), 0.0f), on(model.nodes.size(), 0.0f);
    for (u32 m = 0; m < model.meshes.size(); ++m) {
        const geom::SkinBinding& skin = model.meshes[m].skin;
        for (u32 v = 0; v < skin.vertexCount(); ++v) {
            for (const geom::Influence& influence : skin.forVertex(v)) {
                if (influence.bone >= total.size()) {
                    continue;
                }
                total[influence.bone] += influence.weight;
                if (m == mesh && v < onCloth.size() && onCloth[v] != 0) {
                    on[influence.bone] += influence.weight;
                }
            }
        }
    }
    std::vector<u8> cloth(total.size(), 0);
    for (u32 b = 0; b < total.size(); ++b) {
        cloth[b] = total[b] > 0.0f && on[b] >= 0.9f * total[b] ? 1 : 0;
    }
    // A bone that holds up a bone skinning anything else is what the cloth
    // hangs from, never cloth: the root under a sheet, the chest under a cape.
    for (bool changed = true; changed;) {
        changed = false;
        for (u32 b = 0; b < total.size(); ++b) {
            if (cloth[b] != 0 || total[b] <= 0.0f) {
                continue;
            }
            for (u32 up = model.nodes.nodes[b].parent; up < cloth.size(); up = model.nodes.nodes[up].parent) {
                if (cloth[up] != 0) {
                    cloth[up] = 0;
                    changed = true;
                }
            }
        }
    }
    std::vector<u32> bones;
    for (u32 b = 0; b < cloth.size(); ++b) {
        if (cloth[b] != 0) {
            bones.push_back(b);
        }
    }
    return bones;
}

std::vector<u32> ClothBoneRoots(const NodeTree& tree, std::span<const u32> bones) {
    std::vector<u32> roots;
    for (const u32 bone : bones) {
        if (bone < tree.size() && !Contains(bones, tree.nodes[bone].parent)) {
            roots.push_back(bone);
        }
    }
    return roots;
}

// ============================================================================
// The source and the pins
// ============================================================================

f32 ClothSource::size() const {
    return (upper - lower).length();
}

ClothSource GatherClothSource(const Mesh& mesh, u32 meshIndex, std::span<const u32> faces,
                              std::span<const u32> bones) {
    ClothSource source;
    source.mesh = meshIndex;
    const FaceCorners corners(mesh);
    const std::span<const Vector3f> positions =
        mesh.attributes.get<Vector3f>(geom::names::kPosition, geom::Domain::Vertex);
    for (const u32 f : faces) {
        if (f < corners.first.size()) {
            source.faces.push_back(f);
        }
    }
    std::sort(source.faces.begin(), source.faces.end());
    source.faces.erase(std::unique(source.faces.begin(), source.faces.end()), source.faces.end());
    std::vector<u8> chosen(corners.first.size(), 0);
    std::vector<u8> used(positions.size(), 0);
    // A rigid section's vertices carry no skin: they are its node's alone.
    std::vector<u32> rigid(positions.size(), kInvalidNode);
    const std::span<const u32> sections = mesh.faceSections();
    for (const u32 f : source.faces) {
        chosen[f] = 1;
        const u32 s = f < sections.size() ? sections[f] : kInvalidIndex;
        const bool isRigid = s < mesh.sections.size() && mesh.sections[s].rigidNode.has_value();
        for (const u32 v : corners.of(f)) {
            used[v] = 1;
            if (isRigid) {
                rigid[v] = *mesh.sections[s].rigidNode;
            }
        }
    }
    bool first = true;
    for (u32 v = 0; v < used.size(); ++v) {
        if (used[v] == 0) {
            continue;
        }
        const Vector3f& p = positions[v];
        source.lower = first ? p : Vector3f{std::min(source.lower.x, p.x), std::min(source.lower.y, p.y),
                                            std::min(source.lower.z, p.z)};
        source.upper = first ? p : Vector3f{std::max(source.upper.x, p.x), std::max(source.upper.y, p.y),
                                            std::max(source.upper.z, p.z)};
        first = false;
    }
    if (first) {
        return source;
    }

    // Welded within a ten-thousandth of the cloth's size: a UV seam's copies
    // are one point, so nothing tears where the faces are not apart (§5.1).
    WeldGrid grid;
    grid.cell = std::max(source.size() * 1e-4f, 1e-6f);
    const std::vector<f32> share = BoneShare(mesh, bones);
    source.pointOf.assign(positions.size(), kInvalidIndex);
    for (u32 v = 0; v < used.size(); ++v) {
        if (used[v] == 0) {
            continue;
        }
        u32 point = grid.find(positions[v], source.points);
        if (point == kInvalidIndex) {
            point = static_cast<u32>(source.points.size());
            source.points.push_back(positions[v]);
            grid.add(positions[v], point);
            std::vector<geom::Influence> skin;
            if (rigid[v] != kInvalidNode) {
                skin.push_back({rigid[v], 1.0f});
            } else if (v < mesh.skin.vertexCount()) {
                const std::span<const geom::Influence> influences = mesh.skin.forVertex(v);
                skin.assign(influences.begin(), influences.end());
            }
            source.share.push_back(rigid[v] != kInvalidNode ? (Contains(bones, rigid[v]) ? 1.0f : 0.0f)
                                                            : v < share.size() ? share[v] : 0.0f);
            source.skin.push_back(std::move(skin));
        }
        source.pointOf[v] = point;
    }
    // The seam: a point a face that is not cloth also uses, or stands on.
    source.seam.assign(source.points.size(), 0);
    for (u32 f = 0; f < corners.first.size(); ++f) {
        if (chosen[f] != 0) {
            continue;
        }
        for (const u32 v : corners.of(f)) {
            const u32 point = v < source.pointOf.size() && source.pointOf[v] != kInvalidIndex
                                  ? source.pointOf[v]
                                  : grid.find(positions[v], source.points);
            if (point != kInvalidIndex) {
                source.seam[point] = 1;
            }
        }
    }
    std::map<std::pair<u32, u32>, u32> edges;
    for (const u32 f : source.faces) {
        for (const std::array<u32, 3>& t : FaceTriangles(mesh, corners, f)) {
            const std::array<u32, 3> p{source.pointOf[t[0]], source.pointOf[t[1]], source.pointOf[t[2]]};
            if (p[0] == p[1] || p[1] == p[2] || p[0] == p[2]) {
                continue;
            }
            source.triangles.push_back(p);
            for (u32 k = 0; k < 3; ++k) {
                const u32 a = p[k], b = p[(k + 1) % 3];
                ++edges[{std::min(a, b), std::max(a, b)}];
            }
        }
    }
    source.border.assign(source.points.size(), 0);
    f32 length = 0.0f;
    for (const auto& [edge, count] : edges) {
        length += Distance(source.points[edge.first], source.points[edge.second]);
        if (count == 1) {
            source.border[edge.first] = source.border[edge.second] = 1;
        }
    }
    source.edge = edges.empty() ? source.size() : length / static_cast<f32>(edges.size());
    return source;
}

std::vector<u8> PinPoints(const NodeTree& tree, const ClothSource& source, std::span<const u32> bones,
                          f32 threshold) {
    const std::size_t count = source.points.size();
    std::vector<u8> pinned(count, 0);
    const f32 band = std::max(source.edge * 0.5f, source.size() * 1e-4f);
    // A row: every point within half an edge of the nearest to a place.
    const auto pinRowNear = [&](const auto& distanceOf) {
        f32 nearest = 1e30f;
        for (std::size_t p = 0; p < count; ++p) {
            nearest = std::min(nearest, distanceOf(p));
        }
        for (std::size_t p = 0; p < count; ++p) {
            pinned[p] = distanceOf(p) <= nearest + band ? 1 : 0;
        }
    };
    if (!bones.empty()) {
        for (std::size_t p = 0; p < count; ++p) {
            pinned[p] = source.share[p] < threshold ? 1 : 0;
        }
        if (std::find(pinned.begin(), pinned.end(), u8{1}) != pinned.end()) {
            return pinned;
        }
        const std::vector<u32> roots = ClothBoneRoots(tree, bones);
        if (!roots.empty()) {
            const Vector3f pivot = RestPivot(tree, roots.front());
            pinRowNear([&](std::size_t p) { return Distance(source.points[p], pivot); });
            return pinned;
        }
    }
    if (std::find(source.seam.begin(), source.seam.end(), u8{1}) != source.seam.end()) {
        return source.seam;
    }
    // The highest tenth: the rule *Make cloth* had before there were cloth bones.
    const f32 low = source.lower.z, high = source.upper.z;
    for (std::size_t p = 0; p < count; ++p) {
        pinned[p] = source.points[p].z >= high - (high - low) * 0.1f ? 1 : 0;
    }
    return pinned;
}

std::vector<geom::Influence> PinAnchors(const NodeTree& tree, std::span<const geom::Influence> skin,
                                        std::span<const u32> bones) {
    std::vector<geom::Influence> kept;
    f32 sum = 0.0f;
    u32 heaviest = kInvalidNode;
    f32 most = -1.0f;
    for (const geom::Influence& influence : skin) {
        if (Contains(bones, influence.bone)) {
            if (influence.weight > most) {
                most = influence.weight;
                heaviest = influence.bone;
            }
            continue;
        }
        kept.push_back(influence);
        sum += influence.weight;
    }
    if (sum > 0.0f) {
        for (geom::Influence& influence : kept) {
            influence.weight /= sum;
        }
        return kept;
    }
    // Wholly on cloth bones: held by the parent of its chain's root.
    if (heaviest == kInvalidNode) {
        const std::vector<u32> roots = ClothBoneRoots(tree, bones);
        heaviest = roots.empty() ? kInvalidNode : roots.front();
    }
    while (heaviest < tree.size() && Contains(bones, tree.nodes[heaviest].parent)) {
        heaviest = tree.nodes[heaviest].parent;
    }
    if (heaviest >= tree.size() || tree.nodes[heaviest].parent >= tree.size()) {
        return {};
    }
    return {geom::Influence{tree.nodes[heaviest].parent, 1.0f}};
}

// ============================================================================
// The cage
// ============================================================================

namespace {

/// Per triangle, the triangle across each edge (edge k runs from corner k to
/// k + 1), `kInvalidIndex` on a border.
std::vector<std::array<u32, 3>> TriangleNeighbours(const std::vector<std::array<u32, 3>>& triangles) {
    std::map<std::pair<u32, u32>, std::vector<std::pair<u32, u32>>> users;
    for (u32 t = 0; t < triangles.size(); ++t) {
        for (u32 k = 0; k < 3; ++k) {
            const u32 a = triangles[t][k], b = triangles[t][(k + 1) % 3];
            users[{std::min(a, b), std::max(a, b)}].emplace_back(t, k);
        }
    }
    std::vector<std::array<u32, 3>> out(triangles.size(), {kInvalidIndex, kInvalidIndex, kInvalidIndex});
    for (const auto& [edge, sides] : users) {
        if (sides.size() == 2) {
            out[sides[0].first][sides[0].second] = sides[1].first;
            out[sides[1].first][sides[1].second] = sides[0].first;
        }
    }
    return out;
}

/// Each point's distance from @p seeds over the faces' edges (Dijkstra).
std::vector<f32> EdgeDistance(const ClothSource& source, std::span<const u8> seeds) {
    const std::size_t count = source.points.size();
    std::vector<std::vector<std::pair<u32, f32>>> around(count);
    for (const std::array<u32, 3>& t : source.triangles) {
        for (u32 k = 0; k < 3; ++k) {
            const u32 a = t[k], b = t[(k + 1) % 3];
            const f32 length = Distance(source.points[a], source.points[b]);
            around[a].emplace_back(b, length);
            around[b].emplace_back(a, length);
        }
    }
    std::vector<f32> distance(count, 1e30f);
    using Entry = std::pair<f32, u32>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<>> open;
    for (u32 p = 0; p < count; ++p) {
        if (p < seeds.size() && seeds[p] != 0) {
            distance[p] = 0.0f;
            open.push({0.0f, p});
        }
    }
    while (!open.empty()) {
        const auto [d, p] = open.top();
        open.pop();
        if (d > distance[p]) {
            continue;
        }
        for (const auto& [q, length] : around[p]) {
            if (d + length < distance[q]) {
                distance[q] = d + length;
                open.push({distance[q], q});
            }
        }
    }
    return distance;
}

/// Per point, its island: the points the faces join. @p count receives how many.
std::vector<u32> Islands(const ClothSource& source, u32& count) {
    std::vector<u32> parent(source.points.size());
    for (u32 p = 0; p < parent.size(); ++p) {
        parent[p] = p;
    }
    const auto find = [&](u32 p) {
        while (parent[p] != p) {
            parent[p] = parent[parent[p]];
            p = parent[p];
        }
        return p;
    };
    for (const std::array<u32, 3>& t : source.triangles) {
        parent[find(t[1])] = find(t[0]);
        parent[find(t[2])] = find(t[0]);
    }
    std::vector<u32> island(parent.size(), kInvalidIndex);
    std::map<u32, u32> number;
    for (u32 p = 0; p < parent.size(); ++p) {
        const auto [at, fresh] = number.emplace(find(p), static_cast<u32>(number.size()));
        island[p] = at->second;
    }
    count = static_cast<u32>(number.size());
    return island;
}

/// The point of @p source's faces closest to @p p, by triangle and weights.
struct OnSurface {
    u32 tri = kInvalidIndex;
    std::array<f32, 3> bary{1, 0, 0};
    Vector3f point{0, 0, 0};
    f32 distance = 1e30f;
};

/// @p within, when given, limits the search to its triangles.
OnSurface ClosestOnSource(const ClothSource& source, const Vector3f& p, const std::vector<u8>* within = nullptr) {
    OnSurface best;
    for (u32 t = 0; t < source.triangles.size(); ++t) {
        if (within != nullptr && (*within)[t] == 0) {
            continue;
        }
        const std::array<u32, 3>& tri = source.triangles[t];
        const Vector3f& a = source.points[tri[0]];
        const Vector3f& b = source.points[tri[1]];
        const Vector3f& c = source.points[tri[2]];
        const std::array<f32, 3> w = ClosestOnTriangle(p, a, b, c);
        const Vector3f at = a * w[0] + b * w[1] + c * w[2];
        const f32 d = Distance(p, at);
        if (d < best.distance) {
            best = {t, w, at, d};
        }
    }
    return best;
}

/// The skin of a point on the faces: its corners' skins blended by its weights
/// (Blizzard's *Skin Wrap*, "Weight All Points"), the four heaviest kept.
std::vector<geom::Influence> WrapSkin(const ClothSource& source, const OnSurface& at) {
    std::map<u32, f32> sum;
    if (at.tri < source.triangles.size()) {
        for (u32 k = 0; k < 3; ++k) {
            for (const geom::Influence& influence : source.skin[source.triangles[at.tri][k]]) {
                sum[influence.bone] += influence.weight * at.bary[k];
            }
        }
    }
    std::vector<geom::Influence> out;
    for (const auto& [bone, weight] : sum) {
        if (weight > 1e-5f) {
            out.push_back({bone, weight});
        }
    }
    std::sort(out.begin(), out.end(), [](const geom::Influence& a, const geom::Influence& b) {
        return a.weight != b.weight ? a.weight > b.weight : a.bone < b.bone;
    });
    if (out.size() > 4) {
        out.resize(4);
    }
    f32 total = 0.0f;
    for (const geom::Influence& influence : out) {
        total += influence.weight;
    }
    for (geom::Influence& influence : out) {
        influence.weight /= total > 0.0f ? total : 1.0f;
    }
    return out;
}

/// A particle at @p at on the faces: its anchors wrapped from them, and
/// re-anchored off the cloth bones when it is pinned.
void AddParticle(ClothCage& cage, const NodeTree& tree, const ClothSource& source, const Vector3f& at, bool pinned,
                 std::span<const u32> bones) {
    const OnSurface on = ClosestOnSource(source, at);
    const std::vector<geom::Influence> skin = WrapSkin(source, on);
    cage.points.push_back(at);
    cage.movable.push_back(pinned ? 0 : 1);
    cage.anchors.push_back(pinned ? PinAnchors(tree, skin, bones) : skin);
    // A particle on a drawn point is that point, which a painted pin and a
    // re-anchor read.
    u32 same = kInvalidIndex;
    if (on.tri < source.triangles.size()) {
        for (u32 k = 0; k < 3; ++k) {
            if (on.bary[k] > 0.9999f) {
                same = source.triangles[on.tri][k];
            }
        }
    }
    cage.sourcePoint.push_back(same);
}

/// The principal direction of @p points (power iteration on their covariance).
Vector3f PrincipalAxis(std::span<const Vector3f> points, Vector3f& centre) {
    centre = {0, 0, 0};
    for (const Vector3f& p : points) {
        centre = centre + p;
    }
    centre = centre / static_cast<f32>(std::max<std::size_t>(points.size(), 1));
    f32 c[3][3] = {};
    for (const Vector3f& p : points) {
        const f32 v[3] = {p.x - centre.x, p.y - centre.y, p.z - centre.z};
        for (u32 i = 0; i < 3; ++i) {
            for (u32 j = 0; j < 3; ++j) {
                c[i][j] += v[i] * v[j];
            }
        }
    }
    Vector3f axis{1.0f, 0.7f, 0.3f};
    for (u32 it = 0; it < 32; ++it) {
        const Vector3f next{c[0][0] * axis.x + c[0][1] * axis.y + c[0][2] * axis.z,
                            c[1][0] * axis.x + c[1][1] * axis.y + c[1][2] * axis.z,
                            c[2][0] * axis.x + c[2][1] * axis.y + c[2][2] * axis.z};
        const f32 length = next.length();
        if (length <= 1e-12f) {
            break;
        }
        axis = next / length;
    }
    return axis;
}

/// The hang line's points (§4.2), chain by chain: every pinned point a free one
/// neighbours, walked nearest first from one end of their spread. A chain whose
/// ends meet closes, as a skirt's waist does.
struct HangChain {
    std::vector<Vector3f> points;
    bool closed = false;
};

std::vector<HangChain> HangChains(const ClothSource& source, std::span<const u8> pinned, f32 h) {
    std::vector<u8> hang(source.points.size(), 0);
    for (const std::array<u32, 3>& t : source.triangles) {
        for (u32 k = 0; k < 3; ++k) {
            const u32 a = t[k], b = t[(k + 1) % 3];
            if (pinned[a] != 0 && pinned[b] == 0) {
                hang[a] = 1;
            }
            if (pinned[b] != 0 && pinned[a] == 0) {
                hang[b] = 1;
            }
        }
    }
    std::vector<Vector3f> left;
    for (u32 p = 0; p < hang.size(); ++p) {
        if (hang[p] != 0) {
            left.push_back(source.points[p]);
        }
    }
    std::vector<HangChain> chains;
    if (left.empty()) {
        return chains;
    }
    Vector3f centre;
    const Vector3f axis = PrincipalAxis(left, centre);
    // Hang points sit an edge or so apart; a gap of three cells starts a chain.
    const f32 reach = std::max(h * 3.0f, source.edge * 3.0f);
    while (!left.empty()) {
        std::size_t first = 0;
        for (std::size_t i = 1; i < left.size(); ++i) {
            if ((left[i] - centre).dot(axis) < (left[first] - centre).dot(axis)) {
                first = i;
            }
        }
        HangChain chain;
        chain.points.push_back(left[first]);
        left.erase(left.begin() + static_cast<std::ptrdiff_t>(first));
        while (!left.empty()) {
            std::size_t nearest = 0;
            for (std::size_t i = 1; i < left.size(); ++i) {
                if (Distance(left[i], chain.points.back()) < Distance(left[nearest], chain.points.back())) {
                    nearest = i;
                }
            }
            if (Distance(left[nearest], chain.points.back()) > reach) {
                break;
            }
            chain.points.push_back(left[nearest]);
            left.erase(left.begin() + static_cast<std::ptrdiff_t>(nearest));
        }
        f32 length = 0.0f;
        for (std::size_t i = 1; i < chain.points.size(); ++i) {
            length += Distance(chain.points[i - 1], chain.points[i]);
        }
        chain.closed = chain.points.size() >= 6 &&
                       Distance(chain.points.front(), chain.points.back()) <=
                           2.0f * length / static_cast<f32>(chain.points.size() - 1);
        chains.push_back(std::move(chain));
    }
    return chains;
}

/// @p chain's polyline sampled every @p h of its length, both ends kept on an
/// open one.
std::vector<Vector3f> Resample(const HangChain& chain, f32 h) {
    std::vector<Vector3f> line = chain.points;
    if (chain.closed) {
        line.push_back(line.front());
    }
    f32 length = 0.0f;
    for (std::size_t i = 1; i < line.size(); ++i) {
        length += Distance(line[i - 1], line[i]);
    }
    std::vector<Vector3f> out;
    if (line.size() < 2 || length <= 0.0f) {
        return line.size() == 1 ? std::vector<Vector3f>{line.front()} : out;
    }
    const u32 cells = std::max<u32>(1, static_cast<u32>(std::lround(length / h)));
    const u32 samples = chain.closed ? cells : cells + 1;
    std::size_t segment = 1;
    f32 walked = 0.0f;
    for (u32 s = 0; s < samples; ++s) {
        const f32 at = length * static_cast<f32>(s) / static_cast<f32>(cells);
        while (segment + 1 < line.size() && walked + Distance(line[segment - 1], line[segment]) < at) {
            walked += Distance(line[segment - 1], line[segment]);
            ++segment;
        }
        const f32 span = Distance(line[segment - 1], line[segment]);
        const f32 u = span > 0.0f ? std::clamp((at - walked) / span, 0.0f, 1.0f) : 0.0f;
        out.push_back(line[segment - 1] * (1.0f - u) + line[segment] * u);
    }
    return out;
}

/// The column a streamline makes down @p distance's gradient from @p seed
/// (§5.3 steps 3-6): a point each time it has gone @p h further from the pins,
/// and the hem's own point where it leaves the faces.
std::vector<Vector3f> Column(const ClothSource& source, const std::vector<std::array<u32, 3>>& neighbours,
                             const std::vector<f32>& distance, const Vector3f& seed, f32 h) {
    std::vector<Vector3f> column{seed};
    // Start in a face that runs down from the pins, not one they lie flat on.
    std::vector<u8> sloped(source.triangles.size(), 0);
    for (u32 t = 0; t < source.triangles.size(); ++t) {
        const std::array<u32, 3>& tri = source.triangles[t];
        sloped[t] = std::max({distance[tri[0]], distance[tri[1]], distance[tri[2]]}) > 1e-6f ? 1 : 0;
    }
    OnSurface at = ClosestOnSource(source, seed, &sloped);
    if (at.tri == kInvalidIndex) {
        return column;
    }
    u32 tri = at.tri;
    std::array<f32, 3> bary = at.bary;
    Vector3f point = at.point;
    f32 since = 0.0f;  // how far down since the last point, in distance
    u32 entered = kInvalidIndex;
    for (std::size_t step = 0; step < source.triangles.size() * 4 + 16; ++step) {
        const std::array<u32, 3>& c = source.triangles[tri];
        const Vector3f& p0 = source.points[c[0]];
        const Vector3f& p1 = source.points[c[1]];
        const Vector3f& p2 = source.points[c[2]];
        const Vector3f n = Normal(p0, p1, p2);
        const f32 area2 = n.length();
        if (area2 <= 1e-12f) {
            break;
        }
        const Vector3f unit = n / area2;
        // The barycentrics' gradients, and the field's.
        const std::array<Vector3f, 3> phi{cross(unit, p2 - p1) / area2, cross(unit, p0 - p2) / area2,
                                          cross(unit, p1 - p0) / area2};
        const Vector3f grad = phi[0] * distance[c[0]] + phi[1] * distance[c[1]] + phi[2] * distance[c[2]];
        const f32 slope = grad.length();
        if (slope <= 1e-6f) {
            break;
        }
        const Vector3f dir = grad / slope;
        // Where the line leaves the face, and where it reaches the next row.
        f32 exit = 1e30f;
        u32 through = kInvalidIndex;
        for (u32 k = 0; k < 3; ++k) {
            const f32 rate = phi[k].dot(dir);
            if (rate < -1e-9f) {
                const f32 s = -bary[k] / rate;
                if (s < exit) {
                    exit = std::max(s, 0.0f);
                    through = k;
                }
            }
        }
        const f32 toRow = (h - since) / slope;
        if (toRow <= exit) {
            for (u32 k = 0; k < 3; ++k) {
                bary[k] += phi[k].dot(dir) * toRow;
            }
            point = point + dir * toRow;
            column.push_back(point);
            since = 0.0f;
            continue;
        }
        point = point + dir * exit;
        since += exit * slope;
        // Out through the edge opposite corner `through`: edge (through + 1).
        const u32 next = through == kInvalidIndex ? kInvalidIndex : neighbours[tri][(through + 1) % 3];
        if (next == kInvalidIndex || next == entered) {
            // The hem, or a ridge where two falls meet: its own point when it
            // is a fair part of a row away.
            if (since > 0.3f * h) {
                column.push_back(point);
            }
            break;
        }
        entered = tri;
        tri = next;
        const std::array<u32, 3>& d = source.triangles[tri];
        bary = ClosestOnTriangle(point, source.points[d[0]], source.points[d[1]], source.points[d[2]]);
    }
    return column;
}

/// The Grid (§5.3): columns down from the hang line, every @p h, rows where the
/// cloth is a further @p h from its pins, each cell split along its shorter
/// diagonal. False when it cannot be laid: no hang line, too few columns, or a
/// fold, a cell that faces against the faces it stands on.
bool BuildGrid(const NodeTree& tree, const ClothSource& source, std::span<const u8> pinned, const ClothRecipe& recipe,
               ClothCage& cage) {
    f32 area = 0.0f;
    for (const std::array<u32, 3>& t : source.triangles) {
        area += Normal(source.points[t[0]], source.points[t[1]], source.points[t[2]]).length() * 0.5f;
    }
    const f32 h = std::sqrt(area / static_cast<f32>(std::max<u32>(recipe.particles, 4)));
    if (h <= 0.0f) {
        return false;
    }
    const std::vector<f32> distance = EdgeDistance(source, pinned);
    const std::vector<std::array<u32, 3>> neighbours = TriangleNeighbours(source.triangles);
    ClothCage grid;
    grid.kind = ClothCageKind::Grid;
    u32 columns = 0;
    for (const HangChain& chain : HangChains(source, pinned, h)) {
        const std::vector<Vector3f> seeds = Resample(chain, h);
        std::vector<std::vector<u32>> ids;
        for (const Vector3f& seed : seeds) {
            std::vector<u32>& column = ids.emplace_back();
            const std::vector<Vector3f> points = Column(source, neighbours, distance, seed, h);
            for (std::size_t r = 0; r < points.size(); ++r) {
                column.push_back(static_cast<u32>(grid.points.size()));
                AddParticle(grid, tree, source, points[r], r == 0, recipe.bones);
            }
        }
        const std::size_t count = ids.size();
        for (std::size_t c = 0; c + (chain.closed ? 0 : 1) < count; ++c) {
            const std::vector<u32>& a = ids[c];
            const std::vector<u32>& b = ids[(c + 1) % count];
            for (std::size_t r = 0; r + 1 < std::max(a.size(), b.size()); ++r) {
                const u32 A = r < a.size() ? a[r] : kInvalidIndex, B = r < b.size() ? b[r] : kInvalidIndex;
                const u32 C = r + 1 < a.size() ? a[r + 1] : kInvalidIndex;
                const u32 D = r + 1 < b.size() ? b[r + 1] : kInvalidIndex;
                const u32 have = (A != kInvalidIndex) + (B != kInvalidIndex) + (C != kInvalidIndex) +
                                 (D != kInvalidIndex);
                if (have == 4) {
                    if (Distance(grid.points[A], grid.points[D]) <= Distance(grid.points[B], grid.points[C])) {
                        grid.triangles.push_back({A, C, D});
                        grid.triangles.push_back({A, D, B});
                    } else {
                        grid.triangles.push_back({A, C, B});
                        grid.triangles.push_back({B, C, D});
                    }
                } else if (have == 3) {
                    std::array<u32, 3> tri{};
                    u32 n = 0;
                    for (const u32 v : {A, C, D, B}) {
                        if (v != kInvalidIndex) {
                            tri[n++] = v;
                        }
                    }
                    grid.triangles.push_back(tri);
                }
            }
        }
        columns += static_cast<u32>(count);
    }
    if (columns < 2 || grid.triangles.empty()) {
        return false;
    }
    // Facing as the faces do; a cell that will not is a fold.
    i32 agree = 0;
    std::vector<f32> facing(grid.triangles.size(), 0.0f);
    for (std::size_t t = 0; t < grid.triangles.size(); ++t) {
        const std::array<u32, 3>& tri = grid.triangles[t];
        const Vector3f n = Normal(grid.points[tri[0]], grid.points[tri[1]], grid.points[tri[2]]);
        const OnSurface on = ClosestOnSource(source, (grid.points[tri[0]] + grid.points[tri[1]] + grid.points[tri[2]]) / 3.0f);
        if (on.tri == kInvalidIndex || n.length() <= 0.0f) {
            continue;
        }
        const std::array<u32, 3>& s = source.triangles[on.tri];
        const Vector3f m = Normal(source.points[s[0]], source.points[s[1]], source.points[s[2]]);
        facing[t] = n.dot(m) / std::max(n.length() * m.length(), 1e-12f);
        agree += facing[t] >= 0.0f ? 1 : -1;
    }
    for (std::size_t t = 0; t < grid.triangles.size(); ++t) {
        if (agree < 0) {
            std::swap(grid.triangles[t][1], grid.triangles[t][2]);
            facing[t] = -facing[t];
        }
        if (facing[t] < -0.2f) {
            return false;
        }
    }
    cage = std::move(grid);
    return true;
}

/// The chain the Strip runs down (§5.4), root to tip: the cloth bones' longest
/// chain, carried on to the faces' farthest point along its last bone; with no
/// cloth bones, the faces' principal axis from their pinned end.
std::vector<Vector3f> StripAxis(const NodeTree& tree, const ClothSource& source, std::span<const u8> pinned,
                                std::span<const u32> bones) {
    std::vector<Vector3f> axis;
    const std::vector<u32> roots = ClothBoneRoots(tree, bones);
    if (!roots.empty()) {
        // Down the bones, the deepest child first.
        std::vector<u32> chain{roots.front()};
        for (bool grew = true; grew;) {
            grew = false;
            u32 best = kInvalidIndex, depth = 0;
            for (const u32 bone : bones) {
                if (bone >= tree.size() || tree.nodes[bone].parent != chain.back()) {
                    continue;
                }
                u32 below = 0;
                for (u32 up = bone; up < tree.size() && Contains(bones, up); up = tree.nodes[up].parent) {
                    ++below;
                }
                if (best == kInvalidIndex || below >= depth) {
                    best = bone;
                    depth = below;
                }
            }
            if (best != kInvalidIndex) {
                chain.push_back(best);
                grew = true;
            }
        }
        for (const u32 bone : chain) {
            axis.push_back(RestPivot(tree, bone));
        }
        // On to the tip: the faces' farthest point along the last bone.
        const Vector3f last = axis.back();
        const Vector3f along = axis.size() >= 2 ? (axis.back() - axis[axis.size() - 2]).normalized() : Vector3f{0, 0, 0};
        Vector3f tip = last;
        f32 far = 0.0f;
        for (const Vector3f& p : source.points) {
            const f32 reach = along.length() > 0.0f ? (p - last).dot(along) : Distance(p, last);
            if (reach > far) {
                far = reach;
                tip = along.length() > 0.0f ? last + along * reach : p;
            }
        }
        if (far > 0.0f) {
            axis.push_back(tip);
        }
        return axis;
    }
    Vector3f centre;
    const Vector3f principal = PrincipalAxis(source.points, centre);
    f32 low = 1e30f, high = -1e30f;
    Vector3f pinCentre{0, 0, 0};
    u32 pins = 0;
    for (u32 p = 0; p < source.points.size(); ++p) {
        const f32 s = (source.points[p] - centre).dot(principal);
        low = std::min(low, s);
        high = std::max(high, s);
        if (pinned[p] != 0) {
            pinCentre = pinCentre + source.points[p];
            ++pins;
        }
    }
    Vector3f a = centre + principal * low, b = centre + principal * high;
    if (pins != 0 && Distance(pinCentre / static_cast<f32>(pins), b) < Distance(pinCentre / static_cast<f32>(pins), a)) {
        std::swap(a, b);
    }
    return {a, b};
}

/// Whether @p source wraps round its cloth bones' chain (§5.2): its faces'
/// normals turn more than three quarters of the way round it.
bool WrapsChain(const NodeTree& tree, const ClothSource& source, std::span<const u8> pinned,
                std::span<const u32> bones) {
    if (bones.empty()) {
        return false;
    }
    const std::vector<Vector3f> axis = StripAxis(tree, source, pinned, bones);
    if (axis.size() < 2 || Distance(axis.front(), axis.back()) <= 0.0f) {
        return false;
    }
    const Vector3f along = (axis.back() - axis.front()).normalized();
    const Vector3f helper = std::abs(along.z) < 0.9f ? Vector3f{0, 0, 1} : Vector3f{1, 0, 0};
    const Vector3f u = cross(helper, along).normalized();
    const Vector3f v = cross(along, u);
    std::vector<f32> angles;
    for (const std::array<u32, 3>& t : source.triangles) {
        const Vector3f n = Normal(source.points[t[0]], source.points[t[1]], source.points[t[2]]);
        const Vector3f across = n - along * n.dot(along);
        if (across.length() <= 1e-6f * std::max(n.length(), 1e-6f)) {
            continue;
        }
        angles.push_back(std::atan2(across.dot(v), across.dot(u)));
    }
    if (angles.size() < 3) {
        return false;
    }
    std::sort(angles.begin(), angles.end());
    f32 gap = angles.front() + 2.0f * 3.14159265f - angles.back();
    for (std::size_t i = 1; i < angles.size(); ++i) {
        gap = std::max(gap, angles[i] - angles[i - 1]);
    }
    return 2.0f * 3.14159265f - gap > 1.5f * 3.14159265f;
}

/// The Strip (§5.4): two particles at each station down the chain, either side
/// of the faces' cross-section there along its widest way, half its width
/// apart; the first station pinned.
bool BuildStrip(const NodeTree& tree, const ClothSource& source, std::span<const u8> pinned,
                const ClothRecipe& recipe, ClothCage& cage) {
    const std::vector<Vector3f> axis = StripAxis(tree, source, pinned, recipe.bones);
    f32 length = 0.0f;
    for (std::size_t i = 1; i < axis.size(); ++i) {
        length += Distance(axis[i - 1], axis[i]);
    }
    if (axis.size() < 2 || length <= 0.0f) {
        return false;
    }
    const u32 stations = std::max<u32>(2, recipe.particles / 2);
    const f32 spacing = length / static_cast<f32>(stations - 1);
    const f32 near = std::max(spacing * 3.0f, source.size() * 0.2f);
    ClothCage strip;
    strip.kind = ClothCageKind::Strip;
    Vector3f widest{0, 0, 0};
    f32 width = spacing;
    std::size_t segment = 1;
    f32 walked = 0.0f;
    for (u32 s = 0; s < stations; ++s) {
        const f32 at = length * static_cast<f32>(s) / static_cast<f32>(stations - 1);
        while (segment + 1 < axis.size() && walked + Distance(axis[segment - 1], axis[segment]) < at) {
            walked += Distance(axis[segment - 1], axis[segment]);
            ++segment;
        }
        const f32 span = Distance(axis[segment - 1], axis[segment]);
        const f32 k = span > 0.0f ? std::clamp((at - walked) / span, 0.0f, 1.0f) : 0.0f;
        const Vector3f station = axis[segment - 1] * (1.0f - k) + axis[segment] * k;
        const Vector3f tangent = (axis[segment] - axis[segment - 1]).normalized();
        // The cross-section: where the faces near the station cross its plane.
        std::vector<Vector3f> cut;
        for (const std::array<u32, 3>& t : source.triangles) {
            for (u32 e = 0; e < 3; ++e) {
                const Vector3f& a = source.points[t[e]];
                const Vector3f& b = source.points[t[(e + 1) % 3]];
                const f32 da = (a - station).dot(tangent), db = (b - station).dot(tangent);
                // A point on the plane counts once, from the edge leaving it.
                if (!((da <= 0.0f && db > 0.0f) || (da > 0.0f && db <= 0.0f))) {
                    continue;
                }
                const Vector3f x = a + (b - a) * (da / (da - db));
                if (Distance(x, station) <= near) {
                    cut.push_back(x);
                }
            }
        }
        Vector3f centre = station;
        if (cut.size() >= 2) {
            const Vector3f axisOfCut = PrincipalAxis(cut, centre);
            Vector3f flat = axisOfCut - tangent * axisOfCut.dot(tangent);
            if (flat.length() > 1e-6f) {
                flat = flat.normalized();
                if (widest.length() > 0.0f && flat.dot(widest) < 0.0f) {
                    flat = flat * -1.0f;
                }
                widest = flat;
                f32 low = 1e30f, high = -1e30f;
                for (const Vector3f& x : cut) {
                    low = std::min(low, (x - centre).dot(widest));
                    high = std::max(high, (x - centre).dot(widest));
                }
                width = std::max(high - low, spacing * 0.25f);
            }
        }
        if (widest.length() <= 0.0f) {
            const Vector3f helper = std::abs(tangent.z) < 0.9f ? Vector3f{0, 0, 1} : Vector3f{1, 0, 0};
            widest = cross(helper, tangent).normalized();
        }
        AddParticle(strip, tree, source, centre - widest * (width * 0.25f), s == 0, recipe.bones);
        AddParticle(strip, tree, source, centre + widest * (width * 0.25f), s == 0, recipe.bones);
    }
    for (u32 s = 0; s + 1 < stations; ++s) {
        const u32 l0 = s * 2, r0 = l0 + 1, l1 = l0 + 2, r1 = l0 + 3;
        strip.triangles.push_back({l0, r0, l1});
        strip.triangles.push_back({r0, r1, l1});
    }
    cage = std::move(strip);
    return true;
}

/// Sheets back to back (§5.5): another island of the faces facing the other way
/// within a tenth of the cloth's size. Per island, the island it lies on, or
/// itself.
std::vector<u32> LayerPartners(const ClothSource& source, const std::vector<u32>& island, u32 islands) {
    std::vector<u32> partner(islands);
    for (u32 i = 0; i < islands; ++i) {
        partner[i] = i;
    }
    if (islands < 2) {
        return partner;
    }
    std::vector<Vector3f> normal(islands, Vector3f{0, 0, 0});
    std::vector<f32> area(islands, 0.0f);
    for (const std::array<u32, 3>& t : source.triangles) {
        const Vector3f n = Normal(source.points[t[0]], source.points[t[1]], source.points[t[2]]);
        normal[island[t[0]]] = normal[island[t[0]]] + n;
        area[island[t[0]]] += n.length() * 0.5f;
    }
    const f32 thickness = source.size() * 0.1f;
    for (u32 b = 0; b < islands; ++b) {
        for (u32 a = 0; a < islands; ++a) {
            if (a == b || area[a] < area[b] || partner[a] != a || normal[a].dot(normal[b]) >= 0.0f) {
                continue;
            }
            std::vector<u8> within(source.triangles.size(), 0);
            for (u32 t = 0; t < source.triangles.size(); ++t) {
                within[t] = island[source.triangles[t][0]] == a ? 1 : 0;
            }
            u32 near = 0, total = 0;
            for (u32 p = 0; p < source.points.size(); ++p) {
                if (island[p] != b) {
                    continue;
                }
                ++total;
                near += ClosestOnSource(source, source.points[p], &within).distance <= thickness ? 1u : 0u;
            }
            if (total != 0 && near * 10 >= total * 9) {
                partner[b] = a;
                break;
            }
        }
    }
    return partner;
}

} // namespace

ClothCageKind PickCageKind(const NodeTree& tree, const ClothSource& source, std::span<const u8> pinned,
                           std::span<const u32> bones, u32 particles) {
    if (source.points.size() <= particles) {
        return ClothCageKind::AsModelled;
    }
    return WrapsChain(tree, source, pinned, bones) ? ClothCageKind::Strip : ClothCageKind::Grid;
}

ClothCage BuildCage(const NodeTree& tree, const ClothSource& whole, std::span<const u8> wholePinned,
                    const ClothRecipe& recipe) {
    // Layers merged (§5.5): the cage is built on each pair's first sheet and
    // moved halfway to the second; both bind to it.
    u32 islands = 0;
    const std::vector<u32> island = Islands(whole, islands);
    const std::vector<u32> partner = LayerPartners(whole, island, islands);
    u32 merged = 0;
    for (u32 i = 0; i < islands; ++i) {
        merged += partner[i] != i ? 1u : 0u;
    }
    ClothSource source = whole;
    std::vector<u8> pinned(wholePinned.begin(), wholePinned.end());
    std::vector<u8> otherSheet;
    if (merged != 0) {
        std::vector<u32> remap(whole.points.size(), kInvalidIndex);
        source.points.clear();
        source.skin.clear();
        source.share.clear();
        source.seam.clear();
        source.border.clear();
        pinned.clear();
        for (u32 p = 0; p < whole.points.size(); ++p) {
            if (partner[island[p]] != island[p]) {
                continue;
            }
            remap[p] = static_cast<u32>(source.points.size());
            source.points.push_back(whole.points[p]);
            source.skin.push_back(whole.skin[p]);
            source.share.push_back(whole.share[p]);
            source.seam.push_back(whole.seam[p]);
            source.border.push_back(whole.border[p]);
            pinned.push_back(p < wholePinned.size() ? wholePinned[p] : 0);
        }
        source.triangles.clear();
        otherSheet.assign(whole.triangles.size(), 0);
        for (u32 t = 0; t < whole.triangles.size(); ++t) {
            const std::array<u32, 3>& tri = whole.triangles[t];
            if (remap[tri[0]] != kInvalidIndex) {
                source.triangles.push_back({remap[tri[0]], remap[tri[1]], remap[tri[2]]});
            } else {
                otherSheet[t] = 1;
            }
        }
    }

    ClothCage cage;
    const ClothCageKind wanted = recipe.cage == ClothCageKind::Auto
                                     ? PickCageKind(tree, source, pinned, recipe.bones, recipe.particles)
                                     : recipe.cage;
    bool built = false;
    if (wanted == ClothCageKind::Grid) {
        built = BuildGrid(tree, source, pinned, recipe, cage);
    } else if (wanted == ClothCageKind::Strip) {
        built = BuildStrip(tree, source, pinned, recipe, cage);
    }
    if (!built) {
        // As modelled, and Reduced: the faces themselves, collapsed when
        // asked for or when a Grid or a Strip could not be laid.
        cage.kind = wanted == ClothCageKind::AsModelled ? ClothCageKind::AsModelled : ClothCageKind::Reduced;
        cage.points = source.points;
        cage.triangles = source.triangles;
        cage.sourcePoint.resize(source.points.size());
        cage.movable.resize(source.points.size());
        cage.anchors.resize(source.points.size());
        for (u32 p = 0; p < source.points.size(); ++p) {
            cage.sourcePoint[p] = p;
            const bool pin = p < pinned.size() && pinned[p] != 0;
            cage.movable[p] = pin ? 0 : 1;
            cage.anchors[p] = pin ? PinAnchors(tree, source.skin[p], recipe.bones) : source.skin[p];
        }
        if (cage.kind == ClothCageKind::Reduced) {
            ReduceCage(cage, std::max<u32>(recipe.particles, 3));
        }
    }
    if (merged != 0) {
        // Halfway to the other sheet: a lining and its outside move as one.
        cage.layers = 1 + merged;
        for (Vector3f& point : cage.points) {
            const OnSurface on = ClosestOnSource(whole, point, &otherSheet);
            if (on.tri != kInvalidIndex) {
                cage.layerGap = std::max(cage.layerGap, on.distance * 0.5f);
                point = (point + on.point) * 0.5f;
            }
        }
        // The particles stand on no drawn point now.
        std::fill(cage.sourcePoint.begin(), cage.sourcePoint.end(), kInvalidIndex);
    }
    return cage;
}

namespace {

/// A symmetric 4x4 quadric, upper triangle (Garland-Heckbert).
struct Quadric {
    std::array<f64, 10> q{};

    static Quadric Plane(const Vector3f& n, f32 d, f64 weight) {
        Quadric out;
        const f64 a = n.x, b = n.y, c = n.z, e = d;
        out.q = {a * a, a * b, a * c, a * e, b * b, b * c, b * e, c * c, c * e, e * e};
        for (f64& v : out.q) {
            v *= weight;
        }
        return out;
    }
    Quadric& operator+=(const Quadric& other) {
        for (std::size_t i = 0; i < q.size(); ++i) {
            q[i] += other.q[i];
        }
        return *this;
    }
    f64 at(const Vector3f& p) const {
        const f64 x = p.x, y = p.y, z = p.z;
        return q[0] * x * x + 2 * q[1] * x * y + 2 * q[2] * x * z + 2 * q[3] * x + q[4] * y * y + 2 * q[5] * y * z +
               2 * q[6] * y + q[7] * z * z + 2 * q[8] * z + q[9];
    }
};

} // namespace

void ReduceCage(ClothCage& cage, u32 target) {
    const u32 count = static_cast<u32>(cage.points.size());
    if (count <= target) {
        return;
    }
    std::vector<std::array<u32, 3>>& tris = cage.triangles;
    std::vector<u8> triAlive(tris.size(), 1);
    std::vector<std::vector<u32>> around(count);
    for (u32 t = 0; t < tris.size(); ++t) {
        for (const u32 v : tris[t]) {
            around[v].push_back(t);
        }
    }
    // The border, from the edges one triangle uses.
    std::map<std::pair<u32, u32>, std::vector<u32>> edgeTris;
    for (u32 t = 0; t < tris.size(); ++t) {
        for (u32 k = 0; k < 3; ++k) {
            const u32 a = tris[t][k], b = tris[t][(k + 1) % 3];
            edgeTris[{std::min(a, b), std::max(a, b)}].push_back(t);
        }
    }
    std::vector<u8> border(count, 0);
    std::vector<Quadric> quadric(count);
    for (u32 t = 0; t < tris.size(); ++t) {
        const Vector3f& a = cage.points[tris[t][0]];
        const Vector3f n = Normal(a, cage.points[tris[t][1]], cage.points[tris[t][2]]);
        const f32 area = n.length();
        if (area <= 0.0f) {
            continue;
        }
        const Vector3f unit = n / area;
        const Quadric plane = Quadric::Plane(unit, -unit.dot(a), area * 0.5f);
        for (const u32 v : tris[t]) {
            quadric[v] += plane;
        }
    }
    for (const auto& [edge, users] : edgeTris) {
        if (users.size() != 1) {
            continue;
        }
        border[edge.first] = border[edge.second] = 1;
        // A plane through the border edge, across its face, so the outline
        // costs to move (Garland-Heckbert's boundary constraint).
        const std::array<u32, 3>& t = tris[users.front()];
        const Vector3f& a = cage.points[edge.first];
        const Vector3f& b = cage.points[edge.second];
        const Vector3f faceNormal = Normal(cage.points[t[0]], cage.points[t[1]], cage.points[t[2]]);
        Vector3f across = cross(b - a, faceNormal);
        const f32 length = across.length();
        if (length <= 0.0f) {
            continue;
        }
        across = across / length;
        const Quadric plane = Quadric::Plane(across, -across.dot(a), 10.0 * (b - a).length_squared());
        quadric[edge.first] += plane;
        quadric[edge.second] += plane;
    }

    std::vector<u8> alive(count, 1);
    std::vector<u32> stamp(count, 0);
    struct Candidate {
        f64 cost;
        u32 from, to, stampFrom, stampTo;
        bool operator>(const Candidate& other) const {
            return cost > other.cost;
        }
    };
    std::priority_queue<Candidate, std::vector<Candidate>, std::greater<>> heap;
    const auto neighbours = [&](u32 v) {
        std::vector<u32> out;
        for (const u32 t : around[v]) {
            if (triAlive[t] == 0) {
                continue;
            }
            for (const u32 w : tris[t]) {
                if (w != v && !Contains(out, w)) {
                    out.push_back(w);
                }
            }
        }
        return out;
    };
    const auto isBorderEdge = [&](u32 a, u32 b) {
        u32 shared = 0;
        for (const u32 t : around[a]) {
            if (triAlive[t] != 0 && (tris[t][0] == b || tris[t][1] == b || tris[t][2] == b)) {
                ++shared;
            }
        }
        return shared == 1;
    };
    const auto push = [&](u32 from, u32 to) {
        // A pinned particle stays; a border one moves only along the border.
        if (cage.movable[from] == 0 || (border[from] != 0 && (border[to] == 0 || !isBorderEdge(from, to)))) {
            return;
        }
        Quadric sum = quadric[from];
        sum += quadric[to];
        heap.push({sum.at(cage.points[to]), from, to, stamp[from], stamp[to]});
    };
    for (u32 v = 0; v < count; ++v) {
        for (const u32 w : neighbours(v)) {
            push(v, w);
        }
    }

    u32 left = count;
    constexpr f32 kThinnest = 15.0f * 3.14159265f / 180.0f;
    while (left > target && !heap.empty()) {
        const Candidate c = heap.top();
        heap.pop();
        if (alive[c.from] == 0 || alive[c.to] == 0 || stamp[c.from] != c.stampFrom || stamp[c.to] != c.stampTo) {
            continue;
        }
        // The link condition: the two share only the corners of their edge.
        const std::vector<u32> ringFrom = neighbours(c.from);
        const std::vector<u32> ringTo = neighbours(c.to);
        if (!Contains(ringFrom, c.to)) {
            continue;
        }
        u32 common = 0, opposite = 0;
        for (const u32 t : around[c.from]) {
            if (triAlive[t] != 0 && (tris[t][0] == c.to || tris[t][1] == c.to || tris[t][2] == c.to)) {
                ++opposite;
            }
        }
        for (const u32 w : ringFrom) {
            common += Contains(ringTo, w) ? 1u : 0u;
        }
        if (common != opposite) {
            continue;
        }
        // No flip and no sliver, against what each moved triangle was.
        bool legal = true;
        for (const u32 t : around[c.from]) {
            if (triAlive[t] == 0 || tris[t][0] == c.to || tris[t][1] == c.to || tris[t][2] == c.to) {
                continue;
            }
            std::array<Vector3f, 3> before{cage.points[tris[t][0]], cage.points[tris[t][1]], cage.points[tris[t][2]]};
            std::array<Vector3f, 3> after = before;
            for (u32 k = 0; k < 3; ++k) {
                if (tris[t][k] == c.from) {
                    after[k] = cage.points[c.to];
                }
            }
            const Vector3f n0 = Normal(before[0], before[1], before[2]);
            const Vector3f n1 = Normal(after[0], after[1], after[2]);
            const f32 lengths = n0.length() * n1.length();
            if (lengths <= 0.0f || n0.dot(n1) < 0.2f * lengths) {
                legal = false;
                break;
            }
            const f32 thin = MinAngle(after[0], after[1], after[2]);
            if (thin < kThinnest && thin < MinAngle(before[0], before[1], before[2])) {
                legal = false;
                break;
            }
        }
        if (!legal) {
            continue;
        }
        // Collapse: the removed particle's triangles name the survivor.
        for (const u32 t : around[c.from]) {
            if (triAlive[t] == 0) {
                continue;
            }
            if (tris[t][0] == c.to || tris[t][1] == c.to || tris[t][2] == c.to) {
                triAlive[t] = 0;
                continue;
            }
            for (u32& v : tris[t]) {
                v = v == c.from ? c.to : v;
            }
            around[c.to].push_back(t);
        }
        alive[c.from] = 0;
        quadric[c.to] += quadric[c.from];
        --left;
        ++stamp[c.to];
        for (const u32 w : neighbours(c.to)) {
            ++stamp[w];
        }
        for (const u32 w : neighbours(c.to)) {
            push(c.to, w);
            push(w, c.to);
            for (const u32 x : neighbours(w)) {
                push(w, x);
            }
        }
    }

    // Compacted: the survivors, in their order.
    std::vector<u32> remap(count, kInvalidIndex);
    ClothCage out;
    out.kind = cage.kind;
    out.layers = cage.layers;
    for (u32 v = 0; v < count; ++v) {
        if (alive[v] == 0) {
            continue;
        }
        remap[v] = static_cast<u32>(out.points.size());
        out.points.push_back(cage.points[v]);
        out.movable.push_back(cage.movable[v]);
        out.anchors.push_back(cage.anchors[v]);
        out.sourcePoint.push_back(cage.sourcePoint[v]);
    }
    for (u32 t = 0; t < tris.size(); ++t) {
        if (triAlive[t] != 0) {
            out.triangles.push_back({remap[tris[t][0]], remap[tris[t][1]], remap[tris[t][2]]});
        }
    }
    cage = std::move(out);
}

ClothBind BindToCage(const ClothCage& cage, const Vector3f& point, f32 reach) {
    ClothBind bind;
    f32 best = 1e30f;
    std::array<u32, 3> corners{};
    std::array<f32, 3> weights{};
    for (const std::array<u32, 3>& t : cage.triangles) {
        const Vector3f& a = cage.points[t[0]];
        const Vector3f& b = cage.points[t[1]];
        const Vector3f& c = cage.points[t[2]];
        const std::array<f32, 3> w = ClosestOnTriangle(point, a, b, c);
        const f32 d = Distance(point, a * w[0] + b * w[1] + c * w[2]);
        if (d < best) {
            best = d;
            corners = t;
            weights = w;
        }
    }
    bind.distance = best;
    if (best > reach || cage.triangles.empty()) {
        return bind;
    }
    // Weights under a ten-thousandth are dropped: a point on a particle takes one lane.
    u32 lane = 0;
    f32 sum = 0.0f;
    for (u32 k = 0; k < 3; ++k) {
        if (weights[k] >= 1e-4f) {
            sum += weights[k];
        }
    }
    for (u32 k = 0; k < 3; ++k) {
        if (weights[k] >= 1e-4f) {
            bind.lanes[lane] = corners[k];
            bind.weights[lane] = weights[k] / sum;
            ++lane;
        }
    }
    return bind;
}

// ============================================================================
// The verbs
// ============================================================================

ClothPreview PreviewCloth(const Model& model, u32 mesh, std::span<const u32> faces, const ClothRecipe& recipe,
                          std::span<const u32> cageFaces) {
    ClothPreview preview;
    if (mesh >= model.meshes.size() || faces.empty()) {
        return preview;
    }
    preview.bones = recipe.bones.empty() ? ProposeClothBones(model, mesh, faces) : recipe.bones;
    const ClothSource source = GatherClothSource(model.meshes[mesh], mesh, faces, preview.bones);
    preview.faces = static_cast<u32>(source.faces.size());
    preview.points = static_cast<u32>(source.points.size());
    const std::vector<u8> pinned = PinPoints(model.nodes, source, preview.bones, recipe.threshold);
    for (u32 p = 0; p < pinned.size(); ++p) {
        if (pinned[p] != 0) {
            ++preview.pinned;
            preview.pins.push_back(source.points[p]);
        }
    }
    // The hang line: each pinned edge of a triangle whose third corner moves;
    // a pin with no such edge hangs by its edges to the points that move.
    std::set<std::pair<u32, u32>> hang;
    std::vector<u8> onLine(pinned.size(), 0);
    for (const std::array<u32, 3>& t : source.triangles) {
        for (u32 k = 0; k < 3; ++k) {
            const u32 a = t[k], b = t[(k + 1) % 3], c = t[(k + 2) % 3];
            if (pinned[a] != 0 && pinned[b] != 0 && pinned[c] == 0) {
                hang.insert({std::min(a, b), std::max(a, b)});
                onLine[a] = onLine[b] = 1;
            }
        }
    }
    for (const std::array<u32, 3>& t : source.triangles) {
        for (u32 k = 0; k < 3; ++k) {
            const u32 a = t[k];
            for (u32 j = 1; j < 3; ++j) {
                const u32 b = t[(k + j) % 3];
                if (pinned[a] != 0 && onLine[a] == 0 && pinned[b] == 0) {
                    hang.insert({std::min(a, b), std::max(a, b)});
                }
            }
        }
    }
    for (const auto& [a, b] : hang) {
        preview.hangLine.emplace_back(source.points[a], source.points[b]);
    }
    if (recipe.cage == ClothCageKind::FromFaces && !cageFaces.empty()) {
        preview.kind = ClothCageKind::FromFaces;
        preview.particles = static_cast<u32>(GatherClothSource(model.meshes[mesh], mesh, cageFaces, {}).points.size());
        return preview;
    }
    ClothRecipe resolved = recipe;
    resolved.bones = preview.bones;
    const ClothCage cage = BuildCage(model.nodes, source, pinned, resolved);
    preview.kind = cage.kind;
    preview.particles = static_cast<u32>(cage.points.size());
    return preview;
}

const char* CanMakeClothFromFaces(const Document& document, u32 model, u32 mesh, std::span<const u32> faces) {
    if (model >= document.models.size() || mesh >= document.models[model].meshes.size()) {
        return "physics.cloth.why.no_mesh";
    }
    if (faces.empty()) {
        return "physics.cloth.why.no_faces";
    }
    const Mesh& target = document.models[model].meshes[mesh];
    const std::span<const u32> sections = target.faceSections();
    for (const u32 f : faces) {
        if (f >= sections.size() || sections[f] >= target.sections.size()) {
            return "physics.cloth.why.no_faces";
        }
        const SectionFlags flags = target.sections[sections[f]].flags;
        if (hasFlag(flags, SectionFlags::ClothSimulated)) {
            return "physics.cloth.why.cage";
        }
        if (hasFlag(flags, SectionFlags::ClothInfluenced)) {
            return "physics.cloth.why.already";
        }
    }
    return nullptr;
}

ClothRecipe DefaultClothRecipe(const Model& model, u32 mesh, std::span<const u32> faces) {
    ClothRecipe recipe;
    for (u32 n = 1;; ++n) {
        char name[16];
        std::snprintf(name, sizeof(name), "Cloth%02u", n);
        const bool taken = std::any_of(model.physics.cloths.begin(), model.physics.cloths.end(),
                                       [&](const Cloth& cloth) { return cloth.recipe && cloth.recipe->name == name; });
        if (!taken) {
            recipe.name = name;
            break;
        }
    }
    recipe.bones = ProposeClothBones(model, mesh, faces);
    recipe.bakeInto = recipe.bones.empty() ? ClothBakeInto::FullDetail : ClothBakeInto::Bones;
    return recipe;
}

std::vector<u32> ClothFaces(const Model& model, const Cloth& cloth) {
    std::vector<u32> faces;
    if (cloth.cage.mesh >= model.meshes.size()) {
        return faces;
    }
    const Mesh& mesh = model.meshes[cloth.cage.mesh];
    for (const ClothBinding& binding : cloth.bindings) {
        if (binding.section.mesh == cloth.cage.mesh) {
            const std::vector<u32> own = SectionFaces(mesh, binding.section.section);
            faces.insert(faces.end(), own.begin(), own.end());
        }
    }
    std::sort(faces.begin(), faces.end());
    return faces;
}

namespace {

/// A particle of the cage before its mesh was rebuilt: what painted pins are
/// carried from.
struct OldParticle {
    Vector3f point{0, 0, 0};
    u8 movable = 1;
};

/// Cloth @p id's geometry undone: its bound sections merged back into theirs
/// of origin, its cage erased, its lanes cleared. Its record is taken out
/// first, so no channel naming it is let go, and returned with where it stood.
/// @p faces, face slots of its mesh, ride the renumbering.
std::optional<std::pair<Cloth, std::size_t>> TakeClothGeometry(Document& document, u32 model, u32 id, Diagnostics& out,
                                                          std::vector<u32>* faces,
                                                          std::vector<OldParticle>* particles,
                                                          std::vector<u32>* ownCage = nullptr) {
    Model& owner = document.models[model];
    const auto found = std::find_if(owner.physics.cloths.begin(), owner.physics.cloths.end(),
                                    [&](const Cloth& cloth) { return cloth.id == id; });
    if (found == owner.physics.cloths.end() || found->cage.mesh >= owner.meshes.size()) {
        return std::nullopt;
    }
    const std::size_t index = static_cast<std::size_t>(found - owner.physics.cloths.begin());
    Cloth record = *found;
    owner.physics.cloths.erase(found);
    const u32 mesh = record.cage.mesh;
    Mesh& target = owner.meshes[mesh];
    if (particles != nullptr) {
        const std::span<const Vector3f> positions =
            target.attributes.get<Vector3f>(geom::names::kPosition, geom::Domain::Vertex);
        const std::span<const u8> movable =
            target.attributes.get<u8>(geom::names::kClothMovable, geom::Domain::Vertex);
        const FaceCorners corners(target);
        std::set<u32> seen;
        for (const u32 f : SectionFaces(target, record.cage.section)) {
            for (const u32 v : corners.of(f)) {
                if (seen.insert(v).second && v < positions.size()) {
                    particles->push_back({positions[v], v < movable.size() ? movable[v] : u8{1}});
                }
            }
        }
    }
    // The faces it drew, which give their lanes back below.
    std::vector<u32> drawn = ClothFaces(owner, record);

    // Each bound section back into its section of origin, and a cage of the
    // user's own into its own: what it was before the cloth was made.
    std::vector<std::pair<u32, u32>> pairs;
    for (std::size_t b = 0; b < record.bindings.size(); ++b) {
        const u32 from = record.recipe && b < record.recipe->from.size() ? record.recipe->from[b]
                                                                          : record.bindings[b].section.section;
        pairs.emplace_back(record.bindings[b].section.section, from);
    }
    u32 cage = record.cage.section;
    const bool own = record.recipe && record.recipe->cageFrom < target.sections.size() &&
                     record.recipe->cageFrom != cage;
    if (own) {
        if (ownCage != nullptr) {
            *ownCage = SectionFaces(target, cage);
        }
        // Its pins and lanes go with it; its faces stay, the user's.
        if (const std::span<u8> movable = target.attributes.get<u8>(geom::names::kClothMovable, geom::Domain::Vertex);
            !movable.empty()) {
            const FaceCorners corners(target);
            for (const u32 f : SectionFaces(target, cage)) {
                for (const u32 v : corners.of(f)) {
                    movable[v] = 0;
                }
            }
        }
        pairs.insert(pairs.begin(), {cage, record.recipe->cageFrom});
        cage = kInvalidIndex;
    }
    const auto boundElsewhere = [&](u32 section) {
        return std::any_of(owner.physics.cloths.begin(), owner.physics.cloths.end(), [&](const Cloth& other) {
            return std::any_of(other.bindings.begin(), other.bindings.end(), [&](const ClothBinding& binding) {
                return binding.section.mesh == mesh && binding.section.section == section;
            });
        });
    };
    while (!pairs.empty()) {
        const auto [bound, from] = pairs.back();
        pairs.pop_back();
        if (bound >= target.sections.size()) {
            continue;
        }
        if (bound == from || from >= target.sections.size()) {
            if (!boundElsewhere(bound)) {
                SectionFlags& flags = target.sections[bound].flags;
                flags = static_cast<SectionFlags>(static_cast<u32>(flags) &
                                                  ~static_cast<u32>(SectionFlags::ClothInfluenced));
            }
            continue;
        }
        const u32 sections[] = {bound};
        const std::vector<u32> remap = geom::MergeSections(target, sections, from);
        if (remap.empty()) {
            out.warn(DiagCode::ClothTopologyInvalid, "a cloth's faces could not go back to their section; kept apart",
                     ElementRef(ElementKind::Mesh, mesh));
            continue;
        }
        FollowSectionRemap(document, model, mesh, remap, out);
        for (auto& [b, f] : pairs) {
            b = Remapped(remap, b);
            f = Remapped(remap, f);
        }
        cage = cage == kInvalidIndex ? cage : Remapped(remap, cage);
    }

    if (cage < target.sections.size()) {
        std::vector<u32> keep = drawn;
        const std::size_t split = keep.size();
        if (faces != nullptr) {
            keep.insert(keep.end(), faces->begin(), faces->end());
        }
        if (!EraseSection(document, model, mesh, cage, out, &keep)) {
            out.warn(DiagCode::ClothTopologyInvalid, "a cloth's cage is its whole mesh, and stays",
                     ElementRef(ElementKind::Mesh, mesh));
        }
        drawn.assign(keep.begin(), keep.begin() + static_cast<std::ptrdiff_t>(split));
        std::erase(drawn, kInvalidIndex);
        if (faces != nullptr) {
            faces->assign(keep.begin() + static_cast<std::ptrdiff_t>(split), keep.end());
            std::erase(*faces, kInvalidIndex);
        }
    }
    // Its lanes cleared; with no cloth left on the mesh, its layers go.
    const bool another = std::any_of(owner.physics.cloths.begin(), owner.physics.cloths.end(),
                                     [&](const Cloth& other) { return other.cage.mesh == mesh; });
    if (!another) {
        for (const char* layer :
             {geom::names::kClothMovable, geom::names::kClothBindVertex, geom::names::kClothBindWeight}) {
            target.attributes.remove(layer, geom::Domain::Vertex);
        }
    } else {
        const std::span<std::array<u32, 4>> lanes =
            target.attributes.get<std::array<u32, 4>>(geom::names::kClothBindVertex, geom::Domain::Vertex);
        const FaceCorners corners(target);
        for (const u32 f : drawn) {
            if (f >= corners.first.size()) {
                continue;
            }
            for (const u32 v : corners.of(f)) {
                if (v < lanes.size()) {
                    lanes[v] = {geom::kInvalidId, geom::kInvalidId, geom::kInvalidId, geom::kInvalidId};
                }
            }
        }
    }
    return std::make_pair(std::move(record), index);
}

/// *From other faces* (§5.2): @p cageFaces, modelled by the user, become the
/// cage as they are: moved into a `Hidden | ClothSimulated` section, their
/// vertices the particles, skinned to anchors wrapped from the drawn faces and
/// pinned by the rule; the drawn faces in @p source bind to them.
bool MakeOwnCage(Document& document, u32 model, u32 mesh, const ClothSource& source, std::span<const u32> cageFaces,
                 ClothRecipe kept, Cloth record, std::size_t index, Diagnostics& out) {
    Model& owner = document.models[model];
    Mesh& target = owner.meshes[mesh];
    const std::span<u32> sections = target.faceSections();
    const u32 from = sections[cageFaces.front()];
    MeshSection section = target.sections[from];
    section.name = kept.name + " cage";
    section.native = {};
    section.flags = SectionFlags::Hidden | SectionFlags::ClothSimulated;
    const u32 cageSection = static_cast<u32>(target.sections.size());
    target.sections.push_back(section);
    for (const u32 f : cageFaces) {
        sections[f] = cageSection;
    }
    kept.cageFrom = from;

    // The cage's own points, welded only to read them; each a vertex of it.
    ClothSource own = GatherClothSource(target, mesh, cageFaces, {});
    for (u32 p = 0; p < own.points.size(); ++p) {
        const OnSurface on = ClosestOnSource(source, own.points[p]);
        own.skin[p] = WrapSkin(source, on);
        f32 share = 0.0f;
        for (const geom::Influence& influence : own.skin[p]) {
            share += Contains(kept.bones, influence.bone) ? influence.weight : 0.0f;
        }
        own.share[p] = share;
    }
    const std::vector<u8> pinned = PinPoints(owner.nodes, own, kept.bones, kept.threshold);
    ClothCage cage;
    cage.kind = ClothCageKind::FromFaces;
    cage.points = own.points;
    cage.triangles = own.triangles;
    for (u32 p = 0; p < own.points.size(); ++p) {
        cage.movable.push_back(pinned[p] != 0 ? 0 : 1);
        cage.anchors.push_back(pinned[p] != 0 ? PinAnchors(owner.nodes, own.skin[p], kept.bones) : own.skin[p]);
    }
    if (target.skin.empty()) {
        target.skin.reset(target.vertexCount());
    }
    const std::span<u8> movable =
        target.attributes.getOrCreate<u8>(geom::names::kClothMovable, geom::Domain::Vertex, geom::AttrType::Bool);
    std::vector<u32> vertexOf(cage.points.size(), kInvalidIndex);
    for (u32 v = 0; v < own.pointOf.size(); ++v) {
        const u32 p = own.pointOf[v];
        if (p == kInvalidIndex) {
            continue;
        }
        vertexOf[p] = vertexOf[p] == kInvalidIndex ? v : vertexOf[p];
        target.skin.assignVertex(v, cage.anchors[p]);
        movable[v] = cage.movable[p];
    }

    const bool fresh = !target.attributes.has(geom::names::kClothBindVertex, geom::Domain::Vertex);
    const std::span<std::array<u32, 4>> lanes = target.attributes.getOrCreate<std::array<u32, 4>>(
        geom::names::kClothBindVertex, geom::Domain::Vertex, geom::AttrType::U32x4);
    const std::span<std::array<f32, 4>> weights = target.attributes.getOrCreate<std::array<f32, 4>>(
        geom::names::kClothBindWeight, geom::Domain::Vertex, geom::AttrType::F32x4);
    constexpr std::array<u32, 4> kUnbound{geom::kInvalidId, geom::kInvalidId, geom::kInvalidId, geom::kInvalidId};
    if (fresh) {
        std::fill(lanes.begin(), lanes.end(), kUnbound);
    }
    const std::span<const Vector3f> positions =
        target.attributes.get<Vector3f>(geom::names::kPosition, geom::Domain::Vertex);
    const f32 reach = kept.reach * std::max(source.size(), own.size());
    const FaceCorners corners(target);
    std::set<u32> boundVertices;
    for (const ClothBinding& binding : record.bindings) {
        for (const u32 f : SectionFaces(target, binding.section.section)) {
            for (const u32 v : corners.of(f)) {
                boundVertices.insert(v);
            }
        }
    }
    u32 unbound = 0;
    for (const u32 v : boundVertices) {
        const ClothBind bind = BindToCage(cage, positions[v], reach);
        lanes[v] = kUnbound;
        weights[v] = {0, 0, 0, 0};
        if (bind.lanes[0] == kInvalidIndex) {
            ++unbound;
            continue;
        }
        for (u32 k = 0; k < 4; ++k) {
            if (bind.lanes[k] != kInvalidIndex) {
                lanes[v][k] = vertexOf[bind.lanes[k]];
                weights[v][k] = bind.weights[k];
            }
        }
    }
    if (unbound != 0) {
        out.warn(DiagCode::ClothTopologyInvalid,
                 std::to_string(unbound) + " drawn points are out of the cage's reach and keep their skin",
                 ElementRef(ElementKind::PhysicsRecord, record.id));
    }
    target.recomputeBounds();
    record.cage = SectionRef{mesh, cageSection};
    record.recipe = kept;
    owner.physics.cloths.insert(owner.physics.cloths.begin() +
                                    static_cast<std::ptrdiff_t>(std::min(index, owner.physics.cloths.size())),
                                std::move(record));
    return true;
}

/// The split, the cage and the binding of @p faces, into @p record (whose id,
/// parameters and colliders are kept), inserted at @p index.
bool MakeInto(Document& document, u32 model, u32 mesh, std::span<const u32> faces, const ClothRecipe& recipe,
              Cloth record, std::size_t index, std::span<const OldParticle> painted, Diagnostics& out,
              std::span<const u32> cageFaces = {}) {
    Model& owner = document.models[model];
    Mesh& target = owner.meshes[mesh];
    if (!target.hasConnectivity() && !target.ensureConnectivity().ok()) {
        return false;
    }
    // 1. The split: each section's chosen faces into one of their own.
    std::map<u32, std::vector<u32>> bySection;
    {
        const std::span<const u32> sections = target.faceSections();
        for (const u32 f : faces) {
            if (f < sections.size()) {
                bySection[sections[f]].push_back(f);
            }
        }
    }
    if (bySection.empty()) {
        return false;
    }
    record.bindings.clear();
    ClothRecipe kept = recipe;
    kept.from.clear();
    kept.cageFrom = kInvalidIndex;
    for (const auto& [section, chosen] : bySection) {
        u32 bound = section;
        if (chosen.size() != SectionFaces(target, section).size()) {
            bound = static_cast<u32>(target.sections.size());
            MeshSection split = target.sections[section];
            split.name = (split.name.empty() ? std::string("Cloth") : split.name) + " (cloth)";
            target.sections.push_back(split);
            const std::span<u32> sections = target.faceSections();
            for (const u32 f : chosen) {
                sections[f] = bound;
            }
            CloneSectionChannels(document, model, mesh, section, bound);
        }
        target.sections[bound].flags |= SectionFlags::ClothInfluenced;
        record.bindings.push_back(ClothBinding{SectionRef{mesh, bound}});
        kept.from.push_back(section);
    }

    // 2. The pins, 3. the cage, 4. its anchors.
    const ClothSource source = GatherClothSource(target, mesh, faces, kept.bones);
    if (source.triangles.empty()) {
        return false;
    }
    if (kept.cage == ClothCageKind::FromFaces && !cageFaces.empty()) {
        return MakeOwnCage(document, model, mesh, source, cageFaces, kept, std::move(record), index, out);
    }
    const std::vector<u8> pinned = PinPoints(owner.nodes, source, kept.bones, kept.threshold);
    ClothCage cage = BuildCage(owner.nodes, source, pinned, kept);
    // Painted pins win: each particle takes the state of the old one nearest it.
    if (kept.pinsByHand && !painted.empty()) {
        for (u32 p = 0; p < cage.points.size(); ++p) {
            const OldParticle* nearest = &painted.front();
            for (const OldParticle& old : painted) {
                if (Distance(old.point, cage.points[p]) < Distance(nearest->point, cage.points[p])) {
                    nearest = &old;
                }
            }
            const bool pin = nearest->movable == 0;
            if (pin != (cage.movable[p] == 0)) {
                cage.movable[p] = pin ? 0 : 1;
                const u32 from = cage.sourcePoint[p];
                const std::vector<geom::Influence> skin =
                    from < source.skin.size() ? source.skin[from]
                                              : WrapSkin(source, ClosestOnSource(source, cage.points[p]));
                cage.anchors[p] = pin ? PinAnchors(owner.nodes, skin, kept.bones) : skin;
            }
        }
    }

    // The cage as a section of the mesh, appended through a merge so every
    // layer the mesh carries lands on it too.
    geom::MeshBuilder builder;
    MeshSection section = target.sections[record.bindings.front().section.section];
    section.name = kept.name + " cage";
    section.native = {};
    section.rigidNode.reset();
    section.flags = SectionFlags::Hidden | SectionFlags::ClothSimulated;
    builder.addSection(section);
    for (u32 p = 0; p < cage.points.size(); ++p) {
        const geom::VertexId v = builder.addVertex(cage.points[p]);
        for (const geom::Influence& influence : cage.anchors[p]) {
            builder.addInfluence(v, influence.bone, influence.weight);
        }
    }
    for (const std::array<u32, 3>& t : cage.triangles) {
        builder.addTriangle(geom::VertexId(t[0]), geom::VertexId(t[1]), geom::VertexId(t[2]), 0);
    }
    Mesh made = builder.build().mesh;
    std::vector<std::pair<std::string, geom::Domain>> foreign;
    for (const geom::AttrLayer& layer : made.attributes.layers()) {
        if (!target.attributes.has(layer.name, layer.domain)) {
            foreign.emplace_back(layer.name, layer.domain);
        }
    }
    for (const auto& [name, domain] : foreign) {
        made.attributes.remove(name, domain);
    }
    const u32 cageSection = static_cast<u32>(target.sections.size());
    {
        const Mesh inputs[] = {target, std::move(made)};
        Mesh merged = geom::MergeMeshes(inputs);
        merged.name = target.name;
        merged.lodLevel = target.lodLevel;
        merged.repairLog = target.repairLog;
        target = std::move(merged);
    }
    if (!target.hasConnectivity() && !target.ensureConnectivity().ok()) {
        return false;
    }
    target.recomputeBounds();

    // Each particle's vertex, by where it stands: a merge may renumber the cage.
    const std::span<const Vector3f> positions =
        target.attributes.get<Vector3f>(geom::names::kPosition, geom::Domain::Vertex);
    const FaceCorners corners(target);
    std::vector<u32> cageVertices;
    for (const u32 f : SectionFaces(target, cageSection)) {
        for (const u32 v : corners.of(f)) {
            if (!Contains(cageVertices, v)) {
                cageVertices.push_back(v);
            }
        }
    }
    std::vector<u32> vertexOf(cage.points.size(), kInvalidIndex);
    for (u32 p = 0; p < cage.points.size(); ++p) {
        f32 best = 1e30f;
        for (const u32 v : cageVertices) {
            const f32 d = Distance(positions[v], cage.points[p]);
            if (d < best) {
                best = d;
                vertexOf[p] = v;
            }
        }
    }
    // Shaded like the rest, where the mesh is shaded at all.
    if (target.attributes.has(geom::names::kNormal, geom::Domain::Halfedge)) {
        std::vector<geom::FaceId> cageFaces;
        for (const u32 f : SectionFaces(target, cageSection)) {
            cageFaces.push_back(geom::FaceId(f));
        }
        geom::RecomputeNormals(target, cageFaces, 1.047197551f);
    }

    // 5. The layers: pins on the particles, lanes on the faces that draw.
    const bool fresh = !target.attributes.has(geom::names::kClothBindVertex, geom::Domain::Vertex);
    const std::span<u8> movable =
        target.attributes.getOrCreate<u8>(geom::names::kClothMovable, geom::Domain::Vertex, geom::AttrType::Bool);
    const std::span<std::array<u32, 4>> lanes = target.attributes.getOrCreate<std::array<u32, 4>>(
        geom::names::kClothBindVertex, geom::Domain::Vertex, geom::AttrType::U32x4);
    const std::span<std::array<f32, 4>> weights = target.attributes.getOrCreate<std::array<f32, 4>>(
        geom::names::kClothBindWeight, geom::Domain::Vertex, geom::AttrType::F32x4);
    constexpr std::array<u32, 4> kUnbound{geom::kInvalidId, geom::kInvalidId, geom::kInvalidId, geom::kInvalidId};
    if (fresh) {
        std::fill(lanes.begin(), lanes.end(), kUnbound);
    }
    for (const u32 v : cageVertices) {
        lanes[v] = kUnbound;
        weights[v] = {0, 0, 0, 0};
    }
    for (u32 p = 0; p < cage.points.size(); ++p) {
        if (vertexOf[p] < movable.size()) {
            movable[vertexOf[p]] = cage.movable[p];
        }
    }
    // Reach, and far enough for a merged second sheet.
    const f32 reach = std::max(kept.reach * source.size(), cage.layerGap * 1.5f);
    std::set<u32> boundVertices;
    for (const ClothBinding& binding : record.bindings) {
        for (const u32 f : SectionFaces(target, binding.section.section)) {
            for (const u32 v : corners.of(f)) {
                boundVertices.insert(v);
            }
        }
    }
    u32 unbound = 0;
    for (const u32 v : boundVertices) {
        const ClothBind bind = BindToCage(cage, positions[v], reach);
        lanes[v] = kUnbound;
        weights[v] = {0, 0, 0, 0};
        if (bind.lanes[0] == kInvalidIndex) {
            ++unbound;
            continue;
        }
        for (u32 k = 0; k < 4; ++k) {
            if (bind.lanes[k] != kInvalidIndex && vertexOf[bind.lanes[k]] != kInvalidIndex) {
                lanes[v][k] = vertexOf[bind.lanes[k]];
                weights[v][k] = bind.weights[k];
            }
        }
    }
    if (unbound != 0) {
        out.warn(DiagCode::ClothTopologyInvalid,
                 std::to_string(unbound) + " drawn points are out of the cage's reach and keep their skin",
                 ElementRef(ElementKind::PhysicsRecord, record.id));
    }

    // 6. The record.
    record.cage = SectionRef{mesh, cageSection};
    kept.cage = recipe.cage;
    record.recipe = kept;
    owner.physics.cloths.insert(owner.physics.cloths.begin() + static_cast<std::ptrdiff_t>(
                                                                    std::min(index, owner.physics.cloths.size())),
                                std::move(record));
    return true;
}

} // namespace

u32 MakeClothFromFaces(Document& document, u32 model, u32 mesh, std::span<const u32> faces,
                       const ClothRecipe& recipe, Diagnostics& out, std::span<const u32> cageFaces) {
    if (CanMakeClothFromFaces(document, model, mesh, faces) != nullptr ||
        (!cageFaces.empty() && CanUseCageFaces(document, model, mesh, faces, cageFaces) != nullptr)) {
        return 0;
    }
    Model& owner = document.models[model];
    Cloth record;
    record.id = owner.physics.allocateId();
    for (const ClothCollider& collider : owner.physics.colliders) {
        record.colliders.push_back(collider.id);
    }
    ApplyClothPreset(record, Sc2ClothPresets()[kClothMedium]);
    const u32 id = record.id;
    if (!MakeInto(document, model, mesh, faces, recipe, std::move(record), owner.physics.cloths.size(), {}, out,
                  cageFaces)) {
        return 0;
    }
    return id;
}

const char* CanUseCageFaces(const Document& document, u32 model, u32 mesh, std::span<const u32> faces,
                            std::span<const u32> cageFaces) {
    if (model >= document.models.size() || mesh >= document.models[model].meshes.size() || cageFaces.empty()) {
        return "physics.cloth.why.no_cage_faces";
    }
    const Mesh& target = document.models[model].meshes[mesh];
    const std::span<const u32> sections = target.faceSections();
    const FaceCorners corners(target);
    std::set<u32> drawn;
    for (const u32 f : faces) {
        if (f < corners.first.size()) {
            for (const u32 v : corners.of(f)) {
                drawn.insert(v);
            }
        }
    }
    for (const u32 f : cageFaces) {
        if (f >= sections.size() || sections[f] >= target.sections.size() || f >= corners.first.size()) {
            return "physics.cloth.why.no_cage_faces";
        }
        if (std::find(faces.begin(), faces.end(), f) != faces.end()) {
            return "physics.cloth.why.cage_is_cloth";
        }
        const SectionFlags flags = target.sections[sections[f]].flags;
        if (hasFlag(flags, SectionFlags::ClothSimulated) || hasFlag(flags, SectionFlags::ClothInfluenced)) {
            return "physics.cloth.why.already";
        }
        for (const u32 v : corners.of(f)) {
            if (drawn.count(v) != 0) {
                return "physics.cloth.why.cage_touches";
            }
        }
    }
    return nullptr;
}

bool RemakeCloth(Document& document, u32 model, u32 cloth, std::span<const u32> faces, const ClothRecipe& recipe,
                 Diagnostics& out) {
    if (model >= document.models.size() || faces.empty()) {
        return false;
    }
    const Cloth* current = document.models[model].physics.cloth(cloth);
    if (current == nullptr || current->cage.mesh >= document.models[model].meshes.size()) {
        return false;
    }
    const u32 mesh = current->cage.mesh;
    // Faces of another cloth, or of a cage, cannot join this one.
    {
        const Mesh& target = document.models[model].meshes[mesh];
        const std::span<const u32> sections = target.faceSections();
        std::set<u32> own;
        for (const ClothBinding& binding : current->bindings) {
            own.insert(binding.section.section);
        }
        for (const u32 f : faces) {
            if (f >= sections.size() || sections[f] >= target.sections.size()) {
                return false;
            }
            const SectionFlags flags = target.sections[sections[f]].flags;
            if (own.count(sections[f]) == 0 && (hasFlag(flags, SectionFlags::ClothSimulated) ||
                                                 hasFlag(flags, SectionFlags::ClothInfluenced))) {
                return false;
            }
        }
    }
    std::vector<u32> kept(faces.begin(), faces.end());
    std::vector<OldParticle> painted;
    std::vector<u32> ownCage;
    std::optional<std::pair<Cloth, std::size_t>> taken =
        TakeClothGeometry(document, model, cloth, out, &kept, &painted, &ownCage);
    if (!taken) {
        return false;
    }
    return MakeInto(document, model, mesh, kept, recipe, std::move(taken->first), taken->second,
                    recipe.pinsByHand ? std::span<const OldParticle>(painted) : std::span<const OldParticle>(), out,
                    ownCage);
}

bool DeleteCloth(Document& document, u32 model, u32 cloth, Diagnostics& out) {
    if (model >= document.models.size()) {
        return false;
    }
    if (!TakeClothGeometry(document, model, cloth, out, nullptr, nullptr)) {
        return false;
    }
    Model& owner = document.models[model];
    const u32 gone[] = {cloth};
    InvalidatePhysicsChannels(owner.animChannels, gone, out);
    DetachPoseStages(owner.poseStages, gone);
    return true;
}

} // namespace wem
} // namespace models
} // namespace whiteout
