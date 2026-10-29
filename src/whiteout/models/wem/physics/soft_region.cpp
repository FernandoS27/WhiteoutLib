// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/physics/soft_region.h>

#include <whiteout/models/wem/geometry/attributes.h>
#include <whiteout/models/wem/geometry/render_view.h>
#include <whiteout/models/wem/nodes/node.h>
#include <whiteout/models/wem/skinning/points.h>

#include <whiteout/models/wem/skinning/deform.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <numeric>
#include <queue>

#include "soft_common.h"

namespace whiteout {
namespace models {
namespace wem {

namespace {

using softdetail::ForEachTriangle;
using softdetail::kNoWeight;
using softdetail::PositionsOf;

/// Per face of @p mesh, whether every corner is one of @p inRegion.
std::vector<u8> RegionFaces(const Mesh& mesh, const std::vector<u8>& inRegion) {
    const geom::FaceSet& faces = mesh.faceSet();
    std::vector<u8> out(faces.faceCount(), 0);
    std::size_t corner = 0;
    for (std::size_t f = 0; f < faces.faceCount(); ++f) {
        const u32 valence = faces.faceValence[f];
        bool all = valence >= 3;
        for (u32 k = 0; k < valence; ++k) {
            const u32 vertex = faces.cornerVertex[corner + k];
            all = all && vertex < inRegion.size() && inRegion[vertex] != 0;
        }
        out[f] = all ? 1 : 0;
        corner += valence;
    }
    return out;
}

u32 Find(std::vector<u32>& parent, u32 x) {
    while (parent[x] != x) {
        parent[x] = parent[parent[x]];
        x = parent[x];
    }
    return x;
}

} // namespace

SoftMeshPoints SoftPointsOfFaces(const Model& model, u32 mesh, std::span<const u32> faces) {
    SoftMeshPoints out;
    out.mesh = mesh;
    if (mesh >= model.meshes.size()) {
        return out;
    }
    const geom::FaceSet& set = model.meshes[mesh].faceSet();
    std::vector<u32> start(set.faceCount() + 1, 0);
    for (std::size_t f = 0; f < set.faceCount(); ++f) {
        start[f + 1] = start[f] + set.faceValence[f];
    }
    std::vector<u8> taken(set.vertexCount, 0);
    for (u32 face : faces) {
        if (face >= set.faceCount()) {
            continue;
        }
        for (u32 c = start[face]; c < start[face + 1]; ++c) {
            if (set.cornerVertex[c] < taken.size()) {
                taken[set.cornerVertex[c]] = 1;
            }
        }
    }
    for (u32 v = 0; v < taken.size(); ++v) {
        if (taken[v] != 0) {
            out.vertices.push_back(v);
        }
    }
    return out;
}

SoftMeshPoints SoftPointsOfMesh(const Model& model, u32 mesh) {
    if (mesh >= model.meshes.size()) {
        return SoftMeshPoints{mesh, {}};
    }
    std::vector<u32> faces(model.meshes[mesh].faceSet().faceCount());
    std::iota(faces.begin(), faces.end(), 0u);
    return SoftPointsOfFaces(model, mesh, faces);
}

bool SoftBoneIsStaged(const Model& model, u32 node) {
    for (const PoseStage& stage : model.poseStages) {
        const bool placed = stage.kind != StageKind::Link &&
                            std::find(stage.driven.begin(), stage.driven.end(), node) != stage.driven.end();
        const bool read = std::find(stage.targets.begin(), stage.targets.end(), node) != stage.targets.end() ||
                          std::any_of(stage.sources.begin(), stage.sources.end(),
                                      [&](const StageSource& source) { return source.node == node; }) ||
                          (stage.kind == StageKind::LookAt && stage.upNode == node);
        if (placed || read) {
            return true;
        }
    }
    return false;
}

u32 SoftCount::asTheyAre() const {
    u32 count = 0;
    for (const SoftBoneCount& bone : bones) {
        count += bone.asItIs() ? 1 : 0;
    }
    return count;
}

u32 SoftCount::copied() const {
    return static_cast<u32>(bones.size()) - asTheyAre();
}

u32 SoftCount::parentless() const {
    u32 count = 0;
    for (const SoftBoneCount& bone : bones) {
        count += bone.parentless ? 1 : 0;
    }
    return count;
}

f32 SoftCount::largestShare(u32 count) const {
    f32 total = 0.0f;
    for (u32 i = 0; i < count && i < bones.size(); ++i) {
        total += bones[i].share;
    }
    return total;
}

SoftCount CountSoftRegion(const Model& model, std::span<const SoftMeshPoints> region, u32 cap) {
    SoftCount out;
    const NodeTree& tree = model.nodes;
    SoftSkinSource skin(model, cap);

    // Per mesh, the region's points.
    std::vector<std::vector<u8>> inRegion(model.meshes.size());
    for (std::size_t m = 0; m < model.meshes.size(); ++m) {
        inRegion[m].assign(model.meshes[m].faceSet().vertexCount, 0);
    }
    for (const SoftMeshPoints& part : region) {
        if (part.mesh >= model.meshes.size()) {
            continue;
        }
        for (u32 vertex : part.vertices) {
            if (vertex < inRegion[part.mesh].size()) {
                inRegion[part.mesh][vertex] = 1;
            }
        }
    }

    std::vector<f64> inside(tree.size(), 0.0);
    std::vector<u8> outside(tree.size(), 0);
    f64 area = 0.0;
    std::array<f64, 4> byBones{};
    for (u32 m = 0; m < model.meshes.size(); ++m) {
        const Mesh& mesh = model.meshes[m];
        const std::span<const Vector3f> positions = PositionsOf(mesh);
        const std::vector<u8> regionFace = RegionFaces(mesh, inRegion[m]);
        std::vector<f64> pointArea(positions.size(), 0.0);
        std::vector<u8> drawn(positions.size(), 0); ///< 1: by a region face. 2: by another.

        // Shells: region faces joined through points, across seams.
        const skinning::PointTable points = skinning::BuildPointTable(mesh);
        std::vector<u32> shellOf(points.pointCount);
        std::iota(shellOf.begin(), shellOf.end(), 0u);
        struct Shell {
            u32 firstFace = kInvalidIndex;
            u32 faces = 0;
            f64 area = 0.0;
            f64 volume = 0.0;
            bool open = false;
        };
        std::map<std::pair<u32, u32>, u32> edgeFaces;

        ForEachTriangle(mesh, [&](u32 face, u32 a, u32 b, u32 c) {
            if (a >= positions.size() || b >= positions.size() || c >= positions.size()) {
                return;
            }
            if (regionFace[face] == 0) {
                drawn[a] |= 2;
                drawn[b] |= 2;
                drawn[c] |= 2;
                return;
            }
            drawn[a] |= 1;
            drawn[b] |= 1;
            drawn[c] |= 1;
            const Vector3f normal = cross(positions[b] - positions[a], positions[c] - positions[a]);
            const f64 triangle = 0.5 * static_cast<f64>(normal.length());
            pointArea[a] += triangle / 3.0;
            pointArea[b] += triangle / 3.0;
            pointArea[c] += triangle / 3.0;
            const u32 pa = points.pointOf[a], pb = points.pointOf[b], pc = points.pointOf[c];
            shellOf[Find(shellOf, pb)] = Find(shellOf, pa);
            shellOf[Find(shellOf, pc)] = Find(shellOf, pa);
            for (const auto& [u, v] : {std::pair{pa, pb}, std::pair{pb, pc}, std::pair{pc, pa}}) {
                ++edgeFaces[{std::min(u, v), std::max(u, v)}];
            }
        });

        std::map<u32, Shell> shells;
        ForEachTriangle(mesh, [&](u32 face, u32 a, u32 b, u32 c) {
            if (regionFace[face] == 0 || a >= positions.size() || b >= positions.size() || c >= positions.size()) {
                return;
            }
            Shell& shell = shells[Find(shellOf, points.pointOf[a])];
            if (shell.firstFace == kInvalidIndex) {
                shell.firstFace = face;
            }
            const Vector3f normal = cross(positions[b] - positions[a], positions[c] - positions[a]);
            shell.area += 0.5 * static_cast<f64>(normal.length());
            shell.volume += static_cast<f64>(positions[a].dot(cross(positions[b], positions[c]))) / 6.0;
        });
        for (std::size_t f = 0; f < regionFace.size(); ++f) {
            if (regionFace[f] != 0) {
                ++out.faces;
            }
        }
        // A polygon is one face of its shell however many triangles drew it.
        {
            const geom::FaceSet& faces = mesh.faceSet();
            std::size_t corner = 0;
            for (std::size_t f = 0; f < faces.faceCount(); ++f) {
                if (regionFace[f] != 0 && faces.cornerVertex[corner] < points.pointOf.size()) {
                    ++shells[Find(shellOf, points.pointOf[faces.cornerVertex[corner]])].faces;
                }
                corner += faces.faceValence[f];
            }
        }
        for (const auto& [edge, count] : edgeFaces) {
            if (count == 1) {
                shells[Find(shellOf, edge.first)].open = true;
            }
        }
        std::vector<Shell> ordered;
        for (const auto& [root, shell] : shells) {
            if (shell.faces != 0) {
                ordered.push_back(shell);
            }
        }
        std::sort(ordered.begin(), ordered.end(),
                  [](const Shell& a, const Shell& b) { return a.firstFace < b.firstFace; });
        for (const Shell& shell : ordered) {
            out.shells.push_back(
                {m, shell.faces, static_cast<f32>(shell.area), static_cast<f32>(shell.volume), !shell.open});
        }

        for (u32 v = 0; v < positions.size(); ++v) {
            if ((drawn[v] & 1) != 0) {
                ++out.points;
                const std::span<const geom::Influence> influences = skin.of(m, v);
                if (!influences.empty()) {
                    byBones[std::min<std::size_t>(influences.size(), 4) - 1] += pointArea[v];
                }
                for (const geom::Influence& influence : influences) {
                    if (influence.bone < inside.size()) {
                        inside[influence.bone] += static_cast<f64>(influence.weight) * pointArea[v];
                    }
                }
                area += pointArea[v];
            } else if (drawn[v] != 0 && inRegion[m][v] == 0) {
                for (const geom::Influence& influence : skin.of(m, v)) {
                    if (influence.bone < outside.size()) {
                        outside[influence.bone] = 1;
                    }
                }
            }
        }
    }

    out.area = static_cast<f32>(area);
    for (std::size_t k = 0; k < 4; ++k) {
        out.byBones[k] = area > 0.0 ? static_cast<f32>(byBones[k] / area) : 0.0f;
    }

    // What hangs below each node: a bone that carries skin, and one a point
    // outside the region rides.
    std::vector<u8> skinBelow(tree.size(), 0), outsideBelow(tree.size(), 0);
    for (u32 n = 0; n < tree.size(); ++n) {
        if (inside[n] <= 0.0 && outside[n] == 0) {
            continue;
        }
        u32 guard = 0;
        for (u32 at = tree.nodes[n].parent; at < tree.size() && guard <= tree.size(); at = tree.nodes[at].parent) {
            skinBelow[at] = 1;
            outsideBelow[at] |= outside[n];
            ++guard;
        }
    }
    for (u32 n = 0; n < tree.size(); ++n) {
        if (inside[n] <= 0.0) {
            continue;
        }
        SoftBoneCount bone;
        bone.node = n;
        bone.share = area > 0.0 ? static_cast<f32>(inside[n] / area) : 0.0f;
        bone.outside = outside[n] != 0;
        bone.outsideBelow = outsideBelow[n] != 0;
        bone.skinBelow = skinBelow[n] != 0;
        bone.parentless = tree.nodes[n].parent >= tree.size();
        bone.staged = SoftBoneIsStaged(model, n);
        out.bones.push_back(bone);
    }
    std::sort(out.bones.begin(), out.bones.end(), [](const SoftBoneCount& a, const SoftBoneCount& b) {
        return a.share != b.share ? a.share > b.share : a.node < b.node;
    });
    return out;
}

std::vector<std::vector<u8>> SoftSections(const Model& model) {
    std::vector<std::vector<u8>> out(model.meshes.size());
    for (std::size_t m = 0; m < model.meshes.size(); ++m) {
        const Mesh& mesh = model.meshes[m];
        out[m].assign(mesh.sections.size(), 0);
        const std::span<const u32> body = mesh.attributes.get<u32>(geom::names::kSoftBody, geom::Domain::Vertex);
        if (body.empty()) {
            continue;
        }
        const std::span<const u32> sections = mesh.faceSections();
        const auto soft = [&](u32 vertex) { return vertex < body.size() && body[vertex] != 0; };
        ForEachTriangle(mesh, [&](u32 face, u32 a, u32 b, u32 c) {
            const u32 section = face < sections.size() ? sections[face] : 0;
            if (section < out[m].size() && (soft(a) || soft(b) || soft(c))) {
                out[m][section] = 1;
            }
        });
    }
    return out;
}

f32 SoftRegionSize(const Model& model, std::span<const SoftMeshPoints> region) {
    Vector3f low{0, 0, 0}, high{0, 0, 0};
    bool any = false;
    for (const SoftMeshPoints& part : region) {
        if (part.mesh >= model.meshes.size()) {
            continue;
        }
        const std::span<const Vector3f> positions = PositionsOf(model.meshes[part.mesh]);
        for (const u32 vertex : part.vertices) {
            if (vertex >= positions.size()) {
                continue;
            }
            const Vector3f& p = positions[vertex];
            low = any ? Vector3f{std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z)} : p;
            high = any ? Vector3f{std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z)} : p;
            any = true;
        }
    }
    return std::max({high.x - low.x, high.y - low.y, high.z - low.z});
}

std::vector<std::vector<f32>> SoftBlendSoftness(const Model& model, std::span<const SoftMeshPoints> region,
                                                f32 blend) {
    std::vector<std::vector<f32>> out(region.size());
    const f32 reach = std::max(blend, 0.0f) * SoftRegionSize(model, region);
    for (std::size_t r = 0; r < region.size(); ++r) {
        const SoftMeshPoints& part = region[r];
        out[r].assign(part.vertices.size(), 1.0f);
        if (part.mesh >= model.meshes.size()) {
            continue;
        }
        const Mesh& mesh = model.meshes[part.mesh];
        const std::span<const Vector3f> positions = PositionsOf(mesh);
        const skinning::PointTable points = skinning::BuildPointTable(mesh);
        std::vector<u8> inRegion(points.pointCount, 0);
        for (const u32 vertex : part.vertices) {
            if (vertex < points.pointOf.size()) {
                inRegion[points.pointOf[vertex]] = 1;
            }
        }
        const auto at = [&](u32 point) {
            const std::span<const u32> members = points.membersOf(point);
            return !members.empty() && members[0] < positions.size() ? positions[members[0]] : Vector3f{0, 0, 0};
        };
        // The border: a region point beside one that is not. Dijkstra inward
        // from it, along the edges between region points.
        std::vector<f32> distance(points.pointCount, std::numeric_limits<f32>::infinity());
        using Entry = std::pair<f32, u32>;
        std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> queue;
        for (u32 p = 0; p < points.pointCount; ++p) {
            if (inRegion[p] == 0) {
                continue;
            }
            for (const u32 neighbour : points.ringOf(p)) {
                if (inRegion[neighbour] == 0) {
                    distance[p] = 0.0f;
                    queue.push({0.0f, p});
                    break;
                }
            }
        }
        while (!queue.empty()) {
            const auto [d, p] = queue.top();
            queue.pop();
            if (d > distance[p]) {
                continue;
            }
            for (const u32 neighbour : points.ringOf(p)) {
                if (inRegion[neighbour] == 0) {
                    continue;
                }
                const f32 reached = d + (at(neighbour) - at(p)).length();
                if (reached < distance[neighbour]) {
                    distance[neighbour] = reached;
                    queue.push({reached, neighbour});
                }
            }
        }
        for (std::size_t k = 0; k < part.vertices.size(); ++k) {
            const u32 vertex = part.vertices[k];
            if (vertex >= points.pointOf.size()) {
                continue;
            }
            const f32 d = distance[points.pointOf[vertex]];
            // An island the border never reaches is soft throughout.
            out[r][k] = !std::isfinite(d) ? 1.0f : (reach > 0.0f ? std::clamp(d / reach, 0.0f, 1.0f) : (d > 0.0f ? 1.0f : 0.0f));
        }
    }
    return out;
}

SoftFoldPlan PlanSoftFold(const Model& model, std::span<const SoftMeshPoints> region, f32 under, u32 cap) {
    SoftFoldPlan plan;
    SoftSkinSource skin(model, cap);
    const NodeTree& tree = model.nodes;
    std::vector<u32> parents;
    std::vector<Vector3f> pivots;
    for (u32 n = 0; n < tree.size(); ++n) {
        parents.push_back(tree.nodes[n].parent);
        const Matrix44f bind = Matrix44f::inverse(tree.inverseBindMatrix(n));
        pivots.push_back(Vector3f{bind.data[3][0], bind.data[3][1], bind.data[3][2]});
    }
    for (const SoftMeshPoints& part : region) {
        if (part.mesh >= model.meshes.size()) {
            continue;
        }
        const std::span<const Vector3f> positions = PositionsOf(model.meshes[part.mesh]);
        for (const u32 vertex : part.vertices) {
            const std::span<const geom::Influence> own = skin.of(part.mesh, vertex);
            if (own.empty()) {
                continue;
            }
            ++plan.before[std::min<std::size_t>(own.size(), 4) - 1];
            // Heaviest first, so the lightest is last. One merge at a time, the
            // skeleton's cheapest, for as long as a weight under the threshold
            // is left: a second fold then finds nothing to do.
            if (own.size() < 2 || own.back().weight >= under) {
                ++plan.after[std::min<std::size_t>(own.size(), 4) - 1];
                continue;
            }
            const Vector3f at = vertex < positions.size() ? positions[vertex] : Vector3f{0, 0, 0};
            SoftFoldPlan::Change change;
            change.mesh = part.mesh;
            change.vertex = vertex;
            change.skin.assign(own.begin(), own.end());
            while (change.skin.size() > 1 && change.skin.back().weight < under) {
                change.skin = geom::FoldInfluences(change.skin, static_cast<u32>(change.skin.size()) - 1, at, parents, pivots);
            }
            f32 total = 0.0f;
            for (const geom::Influence& influence : change.skin) {
                total += influence.weight;
            }
            for (geom::Influence& influence : change.skin) {
                influence.weight = total > 0.0f ? influence.weight / total : influence.weight;
            }
            ++plan.after[std::min<std::size_t>(std::max<std::size_t>(change.skin.size(), 1), 4) - 1];
            plan.changes.push_back(std::move(change));
        }
    }
    return plan;
}

f32 SoftFoldShift(const Model& model, const SoftFoldPlan& plan, std::span<const Matrix44f> skin) {
    f32 worst = 0.0f;
    for (const SoftFoldPlan::Change& change : plan.changes) {
        if (change.mesh >= model.meshes.size()) {
            continue;
        }
        const Mesh& mesh = model.meshes[change.mesh];
        const std::span<const Vector3f> positions = PositionsOf(mesh);
        if (change.vertex >= positions.size() || change.vertex >= mesh.skin.vertexCount()) {
            continue;
        }
        // The skin as the picture uses it: normalised.
        std::vector<geom::Influence> own;
        f32 total = 0.0f;
        for (const geom::Influence& influence : mesh.skin.forVertex(change.vertex)) {
            if (influence.weight > 0.0f) {
                own.push_back(influence);
                total += influence.weight;
            }
        }
        for (geom::Influence& influence : own) {
            influence.weight /= total;
        }
        const Vector3f was = skinning::DeformPosition(positions[change.vertex], own, skin);
        const Vector3f now = skinning::DeformPosition(positions[change.vertex], change.skin, skin);
        worst = std::max(worst, (now - was).length());
    }
    return worst;
}

void ApplySoftFold(Model& model, const SoftFoldPlan& plan) {
    for (const SoftFoldPlan::Change& change : plan.changes) {
        if (change.mesh < model.meshes.size()) {
            model.meshes[change.mesh].skin.assignVertex(change.vertex, change.skin);
        }
    }
}

} // namespace wem
} // namespace models
} // namespace whiteout
