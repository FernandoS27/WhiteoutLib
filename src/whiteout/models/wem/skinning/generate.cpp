// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/skinning/generate.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <memory>
#include <utility>

#include <whiteout/utils/job_group.h>

#include "generate_common.h"

namespace whiteout {
namespace models {
namespace wem {
namespace skinning {

namespace detail {

Vector3f JointOf(const NodeTree& nodes, u32 node) {
    const Matrix44f bind = Matrix44f::inverse(nodes.inverseBindMatrix(node));
    return Vector3f{bind.data[3][0], bind.data[3][1], bind.data[3][2]};
}

Vector3f PositionOf(std::span<const Vector3f> positions, const PointTable& points, u32 point) {
    const std::span<const u32> members = points.membersOf(point);
    if (members.empty() || members[0] >= positions.size()) {
        return Vector3f{0, 0, 0};
    }
    return positions[members[0]];
}

std::vector<u32> ScopeOrEvery(const PointTable& points, std::span<const u32> scope) {
    if (!scope.empty()) {
        return std::vector<u32>(scope.begin(), scope.end());
    }
    std::vector<u32> all(points.pointCount);
    for (u32 point = 0; point < points.pointCount; ++point) {
        all[point] = point;
    }
    return all;
}

std::vector<u32> HeldBones(const NodeTree& nodes, const BoneSegments& segments,
                           std::span<const u32> asked) {
    std::vector<u32> held;
    if (asked.empty()) {
        return held;
    }
    std::vector<u8> inSet(nodes.size(), 0);
    for (const u32 bone : segments.bones) {
        inSet[bone] = 1;
    }
    for (u32 node = 0; node < nodes.size(); ++node) {
        if (inSet[node] == 0 && nodes.nodes[node].kind == NodeKind::Bone) {
            held.push_back(node);
        }
    }
    return held;
}

std::vector<u32> TrianglesOf(const Mesh& mesh) {
    const geom::FaceSet& faces = mesh.faceSet();
    std::vector<u32> out;
    std::size_t corner = 0;
    for (const u32 valence : faces.faceValence) {
        for (u32 i = 2; i < valence; ++i) {
            out.push_back(faces.cornerVertex[corner]);
            out.push_back(faces.cornerVertex[corner + i - 1]);
            out.push_back(faces.cornerVertex[corner + i]);
        }
        corner += valence;
    }
    return out;
}

u32 VoteBone(const BoneSegments& segments, std::span<const Vector3f> positions,
             const PointTable& points, std::span<const u32> members) {
    std::vector<std::pair<u32, u32>> votes; // bone, count
    for (const u32 point : members) {
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
    return winner;
}

bool RunJobs(u32 count, const GenerateControl& control, u32& done, u32 total,
             const std::function<void(u32)>& job) {
    std::atomic<u32> next{0};
    std::atomic<u32> finished{0};
    std::atomic<bool> stopped{false};
    // One loop for every thread: take the next index until there is none, or
    // until someone has seen the stop.
    const auto take = [&]() -> bool {
        if (stopped.load(std::memory_order_acquire)) {
            return false;
        }
        if (control.stopped()) {
            stopped.store(true, std::memory_order_release);
            return false;
        }
        const u32 index = next.fetch_add(1, std::memory_order_relaxed);
        if (index >= count) {
            return false;
        }
        job(index);
        finished.fetch_add(1, std::memory_order_acq_rel);
        return true;
    };

    // Owned by the helpers too: the last `done()` counts down before it
    // signals, and `wait()` can return in between -- a group on this stack
    // would be gone under that signal. A helper touches nothing else after
    // `done()`.
    const auto group = std::make_shared<utils::JobGroup>();
    if (control.pool != nullptr && count > 1) {
        const u32 helpers =
            std::min<u32>(static_cast<u32>(control.pool->threadCount()), count - 1);
        group->add(helpers);
        for (u32 i = 0; i < helpers; ++i) {
            interfaces::WorkerTask task;
            task.fn = [group, &take]() {
                while (take()) {
                }
                group->done();
            };
            control.pool->submit(task);
        }
    }
    // The calling thread works too, and it is the one that reports: a progress
    // bar is not a thread-safe thing to hand a pool.
    while (take()) {
        if (control.progress) {
            control.progress(done + finished.load(std::memory_order_acquire), total);
        }
    }
    group->wait();
    done += finished.load(std::memory_order_acquire);
    if (control.progress) {
        control.progress(done, total);
    }
    return !stopped.load(std::memory_order_acquire);
}

} // namespace detail

namespace {

using detail::JointOf;
using detail::PositionOf;

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
    // between itself and its children (§8.2). A branch that ends without a bone
    // is skipped: an attachment point or an emitter names no geometry, and a
    // segment reaching out to one would pull weight off the body toward a
    // muzzle flash.
    std::vector<Vector3f> joints(nodes.size(), Vector3f{0, 0, 0});
    std::vector<std::vector<u32>> children(nodes.size());
    for (u32 node = 0; node < nodes.size(); ++node) {
        joints[node] = JointOf(nodes, node);
        const u32 parent = nodes.nodes[node].parent;
        if (parent != kInvalidNode && parent < nodes.size() && parent != node) {
            children[parent].push_back(node);
        }
    }
    // The nearest Bone above @p bone, through any helpers between them.
    const auto boneAbove = [&nodes](u32 bone) {
        u32 node = nodes.nodes[bone].parent;
        u32 guard = 0;
        while (node != kInvalidNode && node < nodes.size() && guard++ < nodes.size()) {
            if (nodes.nodes[node].kind == NodeKind::Bone) {
                return node;
            }
            node = nodes.nodes[node].parent;
        }
        return kInvalidNode;
    };
    std::vector<u32> walk;
    for (const u32 bone : built.bones) {
        bool any = false;
        // Every bone BELOW, looking through the helpers between: a joint that
        // skins nothing is exported as a helper, and the limb still runs across
        // it (13 of the corpus's 60 text MDLs, herogroveghost's abdomen to its
        // chest among them). Every bone of the TREE, not only the ones in the
        // set: the geometry between a bone and its child is real whether or not
        // the child may be written to.
        walk.assign(children[bone].rbegin(), children[bone].rend());
        u32 guard = 0;
        while (!walk.empty() && guard++ < nodes.size()) {
            const u32 child = walk.back();
            walk.pop_back();
            if (nodes.nodes[child].kind == NodeKind::Bone) {
                built.segments.push_back({bone, joints[bone], joints[child], 0.0f});
                any = true;
                continue;
            }
            walk.insert(walk.end(), children[child].rbegin(), children[child].rend());
        }
        if (!any) {
            // A leaf: a sphere at its joint, of radius half the distance to its
            // parent bone's, through any helper between them. See the header --
            // with a bare point a leaf could never beat the parent whose own
            // segment ends on it. A bone with no bone above keeps its own
            // parent, whatever it is.
            u32 parent = boneAbove(bone);
            if (parent == kInvalidNode) {
                parent = nodes.nodes[bone].parent;
            }
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
            if (point >= points.pointCount) {
                continue;
            }
            const u32 island = point < points.islandOf.size() ? points.islandOf[point]
                                                               : kInvalidIndex;
            if (island < islands.size()) {
                islands[island].push_back(point);
                continue;
            }
            // A point no face holds belongs to no island: it is one on its own,
            // and §8.1 never leaves a point empty.
            ++result.islands;
            const u32 bone = NearestBone(segments, PositionOf(positions, points, point));
            if (bone == kInvalidNode) {
                ++result.unreached;
                continue;
            }
            add(bone, point);
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

namespace {

/// The reach of one segment: the distance to its nearest point and how far
/// along it that point lies. `DistanceToSegment` answers only the first, and an
/// envelope needs both.
SegmentReach ReachOfSegment(const Vector3f& position, const Vector3f& start,
                            const Vector3f& end) {
    const Vector3f along{end.x - start.x, end.y - start.y, end.z - start.z};
    const f32 length = along.x * along.x + along.y * along.y + along.z * along.z;
    f32 t = 0.0f;
    if (length > 0.0f) {
        const Vector3f to{position.x - start.x, position.y - start.y, position.z - start.z};
        t = std::clamp((to.x * along.x + to.y * along.y + to.z * along.z) / length, 0.0f, 1.0f);
    }
    const Vector3f closest{start.x + along.x * t, start.y + along.y * t, start.z + along.z * t};
    const Vector3f gap{position.x - closest.x, position.y - closest.y, position.z - closest.z};
    return {std::sqrt(gap.x * gap.x + gap.y * gap.y + gap.z * gap.z), t};
}

f32 Median(std::vector<f32>& values) {
    if (values.empty()) {
        return 0.0f;
    }
    const std::size_t middle = values.size() / 2;
    std::nth_element(values.begin(), values.begin() + middle, values.end());
    return values[middle];
}

/// §8.1's prune: a share under this, of the point's own total, is not written.
constexpr f32 kGeneratedPrune = 0.01f;

/// @p given pruned at `kGeneratedPrune` of its own total, into @p kept. A point
/// whose every share is under the bar keeps its heaviest: pruning it to nothing
/// would leave it for `Assign` to refuse.
void PruneShares(std::span<const geom::Influence> given, std::vector<geom::Influence>& kept) {
    kept.clear();
    f32 total = 0.0f;
    const geom::Influence* heaviest = nullptr;
    for (const geom::Influence& influence : given) {
        if (!(influence.weight > 0.0f) || !std::isfinite(influence.weight)) {
            continue;
        }
        total += influence.weight;
        if (heaviest == nullptr || influence.weight > heaviest->weight) {
            heaviest = &influence;
        }
    }
    for (const geom::Influence& influence : given) {
        if (influence.weight > 0.0f && influence.weight >= total * kGeneratedPrune) {
            kept.push_back(influence);
        }
    }
    if (kept.empty() && heaviest != nullptr) {
        kept.push_back(*heaviest);
    }
}

} // namespace

SegmentReach ReachOfBone(const BoneSegments& segments, u32 bone, const Vector3f& position) {
    SegmentReach best{std::numeric_limits<f32>::max(), 0.0f};
    for (const BoneSegments::Segment& segment : segments.segments) {
        if (segment.bone != bone) {
            continue;
        }
        const SegmentReach reach = ReachOfSegment(position, segment.start, segment.end);
        if (reach.distance < best.distance) {
            best = reach;
        }
    }
    return best;
}

f32 EnvelopeWeight(const Envelope& envelope, const SegmentReach& reach) {
    const f32 t = std::clamp(reach.along, 0.0f, 1.0f);
    const f32 inner = envelope.innerStart + (envelope.innerEnd - envelope.innerStart) * t;
    const f32 outer = envelope.outerStart + (envelope.outerEnd - envelope.outerStart) * t;
    if (!(outer > 0.0f)) {
        return 0.0f;
    }
    if (reach.distance <= inner) {
        return 1.0f;
    }
    if (!(reach.distance < outer)) {
        return 0.0f;
    }
    if (!(outer > inner)) {
        // Inner at or past outer: there is no band to fall off over, so the
        // envelope is the hard one its radii describe.
        return 1.0f;
    }
    const f32 x = (reach.distance - inner) / (outer - inner);
    switch (envelope.falloff) {
    case EnvelopeFalloff::Hard:
        return 1.0f;
    case EnvelopeFalloff::Linear:
        return 1.0f - x;
    case EnvelopeFalloff::Gaussian: {
        // Sigma is a third of the gap, so the bell has fallen to about 1 % of
        // its height by the outer radius and the cut there is not a step.
        const f32 sigma = 1.0f / 3.0f;
        return std::exp(-(x * x) / (2.0f * sigma * sigma));
    }
    case EnvelopeFalloff::Smooth:
    default:
        break;
    }
    const f32 s = 1.0f - x;
    return s * s * (3.0f - 2.0f * s);
}

std::vector<Envelope> MeasureEnvelopes(const Mesh& mesh, const PointTable& points,
                                       const BoneSegments& segments, EnvelopePreset preset) {
    const MeshPoints one[1]{{&mesh, &points}};
    return MeasureEnvelopes(one, segments, preset);
}

std::vector<Envelope> MeasureEnvelopes(std::span<const MeshPoints> meshes,
                                       const BoneSegments& segments, EnvelopePreset preset) {
    std::vector<Envelope> out(segments.bones.size());
    const EnvelopeFalloff falloff = preset == EnvelopePreset::Mechanical
                                        ? EnvelopeFalloff::Hard
                                        : EnvelopeFalloff::Smooth;
    for (Envelope& envelope : out) {
        envelope.falloff = falloff;
    }
    if (segments.bones.empty()) {
        return out;
    }

    std::vector<u32> slotOf(segments.bones.back() + 1, kInvalidIndex);
    for (std::size_t i = 0; i < segments.bones.size(); ++i) {
        slotOf[segments.bones[i]] = static_cast<u32>(i);
    }

    // The Voronoi cells in ONE walk per mesh: `NearestBone` per point, then
    // each point's distance filed under the half of its own bone's segment it
    // is nearer to.
    std::vector<std::vector<f32>> nearStart(segments.bones.size());
    std::vector<std::vector<f32>> nearEnd(segments.bones.size());
    for (const MeshPoints& entry : meshes) {
        if (entry.mesh == nullptr || entry.points == nullptr) {
            continue;
        }
        const std::span<const Vector3f> positions =
            entry.mesh->attributes.get<Vector3f>(geom::names::kPosition, geom::Domain::Vertex);
        for (u32 point = 0; point < entry.points->pointCount; ++point) {
            const Vector3f at = PositionOf(positions, *entry.points, point);
            const u32 bone = NearestBone(segments, at);
            if (bone >= slotOf.size() || slotOf[bone] == kInvalidIndex) {
                continue;
            }
            const u32 slot = slotOf[bone];
            const SegmentReach reach = ReachOfBone(segments, bone, at);
            if (!std::isfinite(reach.distance)) {
                continue;
            }
            (reach.along < 0.5f ? nearStart[slot] : nearEnd[slot]).push_back(reach.distance);
        }
    }

    f32 innerScale = 0.6f;
    f32 outerScale = 1.6f;
    switch (preset) {
    case EnvelopePreset::Mechanical:
        innerScale = 1.0f;
        outerScale = 1.0f;
        break;
    case EnvelopePreset::Hybrid:
        innerScale = 1.0f;
        outerScale = 1.3f;
        break;
    case EnvelopePreset::Organic:
    default:
        break;
    }
    const auto measure = [preset](std::vector<f32>& values) {
        if (values.empty()) {
            return 0.0f;
        }
        if (preset == EnvelopePreset::Mechanical) {
            // The cell EDGE: the envelope is meant to hold the whole of it.
            return *std::max_element(values.begin(), values.end());
        }
        return Median(values);
    };
    for (std::size_t i = 0; i < out.size(); ++i) {
        f32 start = measure(nearStart[i]);
        f32 end = measure(nearEnd[i]);
        if (!(start > 0.0f)) {
            start = end;
        }
        if (!(end > 0.0f)) {
            end = start;
        }
        out[i].innerStart = start * innerScale;
        out[i].innerEnd = end * innerScale;
        out[i].outerStart = start * outerScale;
        out[i].outerEnd = end * outerScale;
    }
    return out;
}

Envelope MeasureEnvelope(std::span<const MeshPoints> meshes, const BoneSegments& segments,
                         u32 bone, EnvelopePreset preset) {
    const std::vector<Envelope> all = MeasureEnvelopes(meshes, segments, preset);
    for (std::size_t i = 0; i < segments.bones.size(); ++i) {
        if (segments.bones[i] == bone) {
            return all[i];
        }
    }
    Envelope out;
    out.falloff = preset == EnvelopePreset::Mechanical ? EnvelopeFalloff::Hard
                                                       : EnvelopeFalloff::Smooth;
    return out;
}

GenerateResult EnvelopeWeights(Mesh& mesh, const NodeTree& nodes, const PointTable& points,
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
        result.unreached = static_cast<u32>(scope.size());
        return result;
    }

    const std::span<const Vector3f> positions =
        mesh.attributes.get<Vector3f>(geom::names::kPosition, geom::Domain::Vertex);

    // The saved envelope where there is one, the default where there is not.
    // The caller's defaults are preferred, because a model of several meshes
    // has to measure over all of them (§8.2).
    std::vector<Envelope> envelopes =
        options.defaults.size() == segments.bones.size()
            ? options.defaults
            : MeasureEnvelopes(mesh, points, segments, EnvelopePreset::Organic);
    for (std::size_t i = 0; i < segments.bones.size(); ++i) {
        const u32 bone = segments.bones[i];
        if (bone < nodes.size() && nodes.nodes[bone].skin.envelope.has_value()) {
            envelopes[i] = *nodes.nodes[bone].skin.envelope;
        }
    }

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

    // The radius `NearestBone` measures a bone's volume with: 0 for a real
    // segment and the sphere's for a leaf (§8.2). The Hard rule below has to
    // break its tie with exactly this, or the mechanical preset would hand a
    // hand's points to the forearm whose segment ENDS on the hand's joint.
    std::vector<f32> radiusOf(segments.bones.size(), 0.0f);
    for (const BoneSegments::Segment& segment : segments.segments) {
        for (std::size_t i = 0; i < segments.bones.size(); ++i) {
            if (segments.bones[i] == segment.bone) {
                radiusOf[i] = std::max(radiusOf[i], segment.radius);
                break;
            }
        }
    }

    std::vector<u32> wrote;
    PointWeights given;
    std::vector<std::pair<u32, std::vector<u32>>> byBone; // the unreached, per nearest bone
    const auto addRigid = [&byBone](u32 bone, u32 point) {
        for (auto& [at, list] : byBone) {
            if (at == bone) {
                list.push_back(point);
                return;
            }
        }
        byBone.push_back({bone, {point}});
    };

    std::vector<geom::Influence> reached;
    std::vector<geom::Influence> pruned;
    for (const u32 point : scope) {
        if (point >= points.pointCount) {
            continue;
        }
        const Vector3f at = PositionOf(positions, points, point);
        reached.clear();
        // The Hard rule: a point inside more than one Hard envelope goes whole
        // to the nearer bone. One running best rather than a second pass, since
        // only a handful of bones are ever near one point.
        u32 hardBone = kInvalidNode;
        f32 hardDistance = std::numeric_limits<f32>::max();
        for (std::size_t i = 0; i < segments.bones.size(); ++i) {
            const u32 bone = segments.bones[i];
            const SegmentReach reach = ReachOfBone(segments, bone, at);
            const f32 weight = EnvelopeWeight(envelopes[i], reach);
            if (!(weight > 0.0f)) {
                continue;
            }
            if (envelopes[i].falloff == EnvelopeFalloff::Hard) {
                const f32 signedDistance = reach.distance - radiusOf[i];
                if (signedDistance < hardDistance) {
                    hardDistance = signedDistance;
                    hardBone = bone;
                }
                continue;
            }
            reached.push_back({bone, weight});
        }
        if (hardBone != kInvalidNode) {
            reached.push_back({hardBone, 1.0f});
        }
        // §8.1's prune holds for every generator: a Smooth falloff's last
        // crumbs are not four real influences for `Assign` to fold.
        PruneShares(reached, pruned);
        reached.swap(pruned);
        if (reached.empty()) {
            ++result.unreached;
            const u32 bone = NearestBone(segments, at);
            if (bone != kInvalidNode) {
                addRigid(bone, point);
            }
            continue;
        }
        wrote.push_back(point);
        given.add(reached);
    }

    if (!wrote.empty()) {
        SkinScope span;
        span.points = wrote;
        span.heldBones = held;
        result.weights += Assign(mesh, nodes, points, span, given);
    }
    for (const auto& [bone, list] : byBone) {
        const SkinScope span{list, {}, held};
        result.weights += Rigid(mesh, nodes, points, span, bone);
    }
    return result;
}

// ---- The heavy generators' write (§8.1) ---------------------------------------

GenerateResult WriteGenerated(Mesh& mesh, const NodeTree& nodes, const PointTable& points,
                              const GeneratedWeights& weights, const GenerateOptions& options) {
    GenerateResult result;
    result.islands = weights.islands;
    result.unreached = weights.unreached;
    if (weights.points.empty()) {
        return result;
    }
    const BoneSegments segments = BuildBoneSegments(nodes, options.bones);
    const std::vector<u32> held = detail::HeldBones(nodes, segments, options.bones);

    // The prune first, relative to the point's own total, so that `Assign`'s
    // limit folds four real influences rather than a tail of crumbs.
    PointWeights pruned;
    std::vector<geom::Influence> kept;
    for (std::size_t i = 0; i < weights.points.size(); ++i) {
        PruneShares(weights.given.of(i), kept);
        pruned.add(kept);
    }

    SkinScope scope;
    scope.points = weights.points;
    scope.heldBones = held;
    result.weights = Assign(mesh, nodes, points, scope, pruned);
    return result;
}

namespace detail {

GenerateResult WriteOneMesh(Mesh& mesh, const NodeTree& nodes, const PointTable& points,
                            const GeneratedRun& run, const GenerateOptions& options) {
    if (run.cancelled || run.meshes.empty()) {
        GenerateResult stopped;
        stopped.cancelled = run.cancelled;
        return stopped;
    }
    return WriteGenerated(mesh, nodes, points, run.meshes.front(), options);
}

} // namespace detail

} // namespace skinning
} // namespace wem
} // namespace models
} // namespace whiteout
