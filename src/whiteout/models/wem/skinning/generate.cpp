// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/skinning/generate.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace whiteout {
namespace models {
namespace wem {
namespace skinning {

namespace {

/// @p node's joint in model space. The bind matrix rather than `Node::pivot`,
/// so a rig that binds with matrices answers the same question -- and it is the
/// formula `ops.cpp`'s fold already uses, so the two cannot drift.
Vector3f JointOf(const NodeTree& nodes, u32 node) {
    const Matrix44f bind = Matrix44f::inverse(nodes.inverseBindMatrix(node));
    return Vector3f{bind.data[3][0], bind.data[3][1], bind.data[3][2]};
}

/// A point's position: its first member's, which is the vertex every other
/// member is co-located with to within the weld tolerance (§3.2).
Vector3f PositionOf(std::span<const Vector3f> positions, const PointTable& points, u32 point) {
    const std::span<const u32> members = points.membersOf(point);
    if (members.empty() || members[0] >= positions.size()) {
        return Vector3f{0, 0, 0};
    }
    return positions[members[0]];
}

} // namespace

BoneSegments BuildBoneSegments(const NodeTree& nodes, std::span<const u32> bones) {
    BoneSegments built;

    std::vector<u8> wanted(nodes.size(), bones.empty() ? 1 : 0);
    for (const u32 bone : bones) {
        if (bone < nodes.size()) {
            wanted[bone] = 1;
        }
    }
    for (u32 node = 0; node < nodes.size(); ++node) {
        const Node& current = nodes.nodes[node];
        if (wanted[node] == 0 || current.kind != NodeKind::Bone || current.skin.locked) {
            continue;
        }
        built.bones.push_back(node);
    }
    if (built.bones.empty()) {
        return built;
    }

    // One segment per CHILD BONE, because a Warcraft III bone carries the mesh
    // between itself and its children (§8.2). A child helper is skipped: an
    // attachment point or an emitter names no geometry, and a segment reaching
    // out to one would pull weight off the body toward a muzzle flash.
    std::vector<Vector3f> joints(nodes.size(), Vector3f{0, 0, 0});
    for (u32 node = 0; node < nodes.size(); ++node) {
        joints[node] = JointOf(nodes, node);
    }
    for (const u32 bone : built.bones) {
        bool any = false;
        // Every child bone of the TREE, not only the ones in the set: the
        // geometry between a bone and its child is real whether or not the
        // child may be written to.
        for (u32 child = 0; child < nodes.size(); ++child) {
            if (nodes.nodes[child].parent != bone || nodes.nodes[child].kind != NodeKind::Bone) {
                continue;
            }
            built.segments.push_back({bone, joints[bone], joints[child], 0.0f});
            any = true;
        }
        if (!any) {
            // A leaf: a sphere at its joint, of radius half the distance to its
            // parent's. See the header -- with a bare point a leaf could never
            // beat the parent whose own segment ends on it.
            const u32 parent = nodes.nodes[bone].parent;
            f32 radius = 0.0f;
            if (parent != kInvalidNode && parent < nodes.size()) {
                const Vector3f up = joints[parent];
                const Vector3f gap{joints[bone].x - up.x, joints[bone].y - up.y,
                                   joints[bone].z - up.z};
                radius = 0.5f * std::sqrt(gap.x * gap.x + gap.y * gap.y + gap.z * gap.z);
            }
            built.segments.push_back({bone, joints[bone], joints[bone], radius});
        }
    }
    return built;
}

f32 DistanceToSegment(const Vector3f& position, const Vector3f& start, const Vector3f& end) {
    const Vector3f along{end.x - start.x, end.y - start.y, end.z - start.z};
    const f32 length = along.x * along.x + along.y * along.y + along.z * along.z;
    f32 t = 0.0f;
    if (length > 0.0f) {
        const Vector3f to{position.x - start.x, position.y - start.y, position.z - start.z};
        t = std::clamp((to.x * along.x + to.y * along.y + to.z * along.z) / length, 0.0f, 1.0f);
    }
    // Not `near`: <windows.h> defines that, and a header the library is
    // compiled beside may have pulled it in.
    const Vector3f closest{start.x + along.x * t, start.y + along.y * t, start.z + along.z * t};
    const Vector3f gap{position.x - closest.x, position.y - closest.y, position.z - closest.z};
    return std::sqrt(gap.x * gap.x + gap.y * gap.y + gap.z * gap.z);
}

u32 NearestBone(const BoneSegments& segments, const Vector3f& position) {
    u32 best = kInvalidNode;
    f32 bestDistance = std::numeric_limits<f32>::max();
    for (const BoneSegments::Segment& segment : segments.segments) {
        const f32 distance =
            DistanceToSegment(position, segment.start, segment.end) - segment.radius;
        // Ties to the bone nearer the root, which in a tree whose parents
        // precede their children is the lower index (§8.3). A strict `<` alone
        // would answer whichever segment the build happened to emit first.
        if (distance < bestDistance || (distance == bestDistance && segment.bone < best)) {
            bestDistance = distance;
            best = segment.bone;
        }
    }
    return best;
}

GenerateResult RigidPerIsland(Mesh& mesh, const NodeTree& nodes, const PointTable& points,
                              std::span<const u32> scope, const GenerateOptions& options) {
    GenerateResult result;
    const BoneSegments segments = BuildBoneSegments(nodes, options.bones);

    std::vector<u32> all;
    if (scope.empty()) {
        all.resize(points.pointCount);
        for (u32 point = 0; point < points.pointCount; ++point) {
            all[point] = point;
        }
        scope = all;
    }
    if (segments.empty()) {
        // No bone to write to, so every point in scope is unreached and nothing
        // is written -- which is the honest answer, not an empty skin.
        result.unreached = static_cast<u32>(scope.size());
        return result;
    }

    const std::span<const Vector3f> positions =
        mesh.attributes.get<Vector3f>(geom::names::kPosition, geom::Domain::Vertex);

    // The bones outside the set are held, so a point's weight on one survives
    // the assignment (§8.1). `Rigid` then writes `1 - L` to the chosen bone.
    std::vector<u32> held;
    if (!options.bones.empty()) {
        std::vector<u8> inSet(nodes.size(), 0);
        for (const u32 bone : segments.bones) {
            inSet[bone] = 1;
        }
        for (u32 node = 0; node < nodes.size(); ++node) {
            if (inSet[node] == 0 && nodes.nodes[node].kind == NodeKind::Bone) {
                held.push_back(node);
            }
        }
    }

    // Each point's answer first, then one `Rigid` per bone: an operation per
    // point would re-read the skeleton and the lock layer for every one of
    // them, and a mechanical model has thousands.
    std::vector<std::pair<u32, std::vector<u32>>> byBone;
    const auto add = [&](u32 bone, u32 point) {
        for (auto& [at, list] : byBone) {
            if (at == bone) {
                list.push_back(point);
                return;
            }
        }
        byBone.push_back({bone, {point}});
    };

    if (options.splitIslands) {
        for (const u32 point : scope) {
            if (point >= points.pointCount) {
                continue;
            }
            const u32 bone = NearestBone(segments, PositionOf(positions, points, point));
            if (bone == kInvalidNode) {
                ++result.unreached;
                continue;
            }
            add(bone, point);
        }
        result.islands = static_cast<u32>(scope.size());
    } else {
        // The vote: each island goes wholly to the bone most of its points are
        // nearest to. A tie goes to the bone nearer the root, which is what
        // `NearestBone` answers for a point and what the scan below keeps.
        std::vector<std::vector<u32>> islands(points.islandCount);
        for (const u32 point : scope) {
            if (point >= points.pointCount || point >= points.islandOf.size()) {
                continue;
            }
            const u32 island = points.islandOf[point];
            if (island < islands.size()) {
                islands[island].push_back(point);
            }
        }
        for (const std::vector<u32>& island : islands) {
            if (island.empty()) {
                continue;
            }
            ++result.islands;
            std::vector<std::pair<u32, u32>> votes; // bone, count
            for (const u32 point : island) {
                const u32 bone = NearestBone(segments, PositionOf(positions, points, point));
                if (bone == kInvalidNode) {
                    continue;
                }
                bool counted = false;
                for (auto& [voted, count] : votes) {
                    if (voted == bone) {
                        ++count;
                        counted = true;
                        break;
                    }
                }
                if (!counted) {
                    votes.push_back({bone, 1});
                }
            }
            u32 winner = kInvalidNode;
            u32 best = 0;
            for (const auto& [bone, count] : votes) {
                if (count > best || (count == best && bone < winner)) {
                    best = count;
                    winner = bone;
                }
            }
            if (winner == kInvalidNode) {
                result.unreached += static_cast<u32>(island.size());
                continue;
            }
            for (const u32 point : island) {
                add(winner, point);
            }
        }
    }

    for (const auto& [bone, list] : byBone) {
        const SkinScope span{list, {}, held};
        result.weights += Rigid(mesh, nodes, points, span, bone);
    }
    return result;
}

} // namespace skinning
} // namespace wem
} // namespace models
} // namespace whiteout
