// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/skinning/ops.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <queue>

#include <whiteout/models/wem/geometry/render_view.h>

namespace whiteout {
namespace models {
namespace wem {
namespace skinning {

namespace {

/// One point's weights while an operation works on them: a bone to weight map,
/// small enough that a vector of pairs beats a hash.
using Weights = std::vector<geom::Influence>;

f32 WeightOf(const Weights& weights, u32 bone) {
    for (const geom::Influence& influence : weights) {
        if (influence.bone == bone) {
            return influence.weight;
        }
    }
    return 0.0f;
}

void SetWeight(Weights& weights, u32 bone, f32 value) {
    for (geom::Influence& influence : weights) {
        if (influence.bone == bone) {
            influence.weight = value;
            return;
        }
    }
    if (value != 0.0f) {
        weights.push_back({bone, value});
    }
}

/// The skeleton `geom::FoldInfluences` folds over: each node's parent and its
/// bind-space pivot. Built once per operation, not per point.
struct Skeleton {
    std::vector<u32> parents;
    std::vector<Vector3f> pivots;

    explicit Skeleton(const NodeTree& tree) {
        parents.reserve(tree.size());
        pivots.reserve(tree.size());
        for (u32 n = 0; n < tree.size(); ++n) {
            parents.push_back(tree.nodes[n].parent);
            const Matrix44f bind = Matrix44f::inverse(tree.inverseBindMatrix(n));
            pivots.push_back(Vector3f{bind.data[3][0], bind.data[3][1], bind.data[3][2]});
        }
    }
};

/// What every operation carries: the locks, the skeleton and the counters.
class Writer {
public:
    Writer(Mesh& mesh, const NodeTree& nodes, const PointTable& points)
        : mesh_(mesh), nodes_(nodes), points_(points), skeleton_(nodes),
          vertexLocked_(mesh.attributes.get<u8>(geom::names::kSkinLocked, geom::Domain::Vertex)) {}

    bool boneLocked(u32 bone) const {
        return bone < nodes_.size() && nodes_.nodes[bone].skin.locked;
    }

    /// The point's weights, taken from its first member: every member is written
    /// the same, so the first is the point's value.
    Weights read(u32 point) const {
        const std::span<const u32> members = points_.membersOf(point);
        Weights weights;
        if (members.empty()) {
            return weights;
        }
        for (const geom::Influence& influence : mesh_.skin.forVertex(members[0])) {
            if (influence.weight > 0.0f && std::isfinite(influence.weight)) {
                SetWeight(weights, influence.bone, WeightOf(weights, influence.bone) +
                                                       influence.weight);
            }
        }
        return weights;
    }

    bool locked(u32 point) const {
        if (vertexLocked_.empty()) {
            return false;
        }
        for (const u32 vertex : points_.membersOf(point)) {
            if (vertex < vertexLocked_.size() && vertexLocked_[vertex] == 0) {
                return false;
            }
        }
        return !points_.membersOf(point).empty();
    }

    /// The locked share of @p weights.
    f32 lockedShare(const Weights& weights) const {
        f32 total = 0.0f;
        for (const geom::Influence& influence : weights) {
            if (boneLocked(influence.bone)) {
                total += influence.weight;
            }
        }
        return total;
    }

    /// Writes @p weights to every member of @p point, dropping what is too small
    /// to hold and leaving each vertex sorted.
    void write(u32 point, Weights weights) {
        std::erase_if(weights, [](const geom::Influence& influence) {
            return !(influence.weight > kMinWeight) || !std::isfinite(influence.weight);
        });
        std::sort(weights.begin(), weights.end(),
                  [](const geom::Influence& a, const geom::Influence& b) {
                      if (a.weight != b.weight) {
                          return a.weight > b.weight;
                      }
                      return a.bone < b.bone;
                  });
        for (const u32 vertex : points_.membersOf(point)) {
            mesh_.skin.assignVertex(vertex, weights);
        }
    }

    /**
     * @brief §6.1: give @p bone the value @p request on @p weights.
     *
     * @return false when the point is refused -- the rule's last case, where the
     * only unlocked influence is being lowered and there is nowhere to put what
     * it gives up.
     */
    bool retarget(Weights& weights, u32 bone, f32 request) const {
        const f32 locked = lockedShare(weights);
        f32 target = std::clamp(request, 0.0f, std::max(0.0f, 1.0f - locked));
        f32 rest = 1.0f - locked - target;

        f32 others = 0.0f;
        for (const geom::Influence& influence : weights) {
            if (influence.bone != bone && !boneLocked(influence.bone) && influence.weight > 0.0f) {
                others += influence.weight;
            }
        }
        bool refused = false;
        u32 heir = kInvalidNode;
        if (others <= 0.0f && rest > 0.0f) {
            // The weight taken from the only unlocked bone goes to its nearest
            // Bone ancestor, the same rule a node removal follows
            // (`SkinPolicy::ReassignToParent`), and for the same reason: it
            // keeps the mesh drawn and moving with the nearest bone that makes
            // sense.
            heir = ancestorFor(bone);
            if (heir == kInvalidNode) {
                target = 1.0f - locked;
                rest = 0.0f;
                refused = true;
            }
        }

        const f32 scale = others > 0.0f ? rest / others : 0.0f;
        for (geom::Influence& influence : weights) {
            if (influence.bone != bone && !boneLocked(influence.bone)) {
                influence.weight *= scale;
            }
        }
        SetWeight(weights, bone, target);
        if (heir != kInvalidNode) {
            SetWeight(weights, heir, WeightOf(weights, heir) + rest);
        }
        return !refused;
    }

    /// The unlocked share scaled to `1 - L`, the locks kept (§6.3's Normalize).
    void normalise(Weights& weights) const {
        const f32 locked = lockedShare(weights);
        f32 unlocked = 0.0f;
        for (const geom::Influence& influence : weights) {
            if (!boneLocked(influence.bone)) {
                unlocked += influence.weight;
            }
        }
        if (unlocked <= 0.0f) {
            return;
        }
        const f32 scale = std::max(0.0f, 1.0f - locked) / unlocked;
        for (geom::Influence& influence : weights) {
            if (!boneLocked(influence.bone)) {
                influence.weight *= scale;
            }
        }
    }

    /// Keeps the @p width heaviest, locked first, folding the rest into the
    /// nearest joints.
    void limit(Weights& weights, u32 point, u32 width) const {
        if (weights.size() <= width) {
            return;
        }
        Weights kept;
        for (const geom::Influence& influence : weights) {
            if (boneLocked(influence.bone) && kept.size() < width) {
                kept.push_back(influence);
            }
        }
        Weights rest;
        for (const geom::Influence& influence : weights) {
            if (!boneLocked(influence.bone)) {
                rest.push_back(influence);
            }
        }
        std::sort(rest.begin(), rest.end(), [](const geom::Influence& a, const geom::Influence& b) {
            if (a.weight != b.weight) {
                return a.weight > b.weight;
            }
            return a.bone < b.bone;
        });
        const u32 room = width - static_cast<u32>(kept.size());
        if (rest.size() > room) {
            const std::span<const u32> members = points_.membersOf(point);
            const std::span<const Vector3f> positions =
                mesh_.attributes.get<Vector3f>(geom::names::kPosition, geom::Domain::Vertex);
            const Vector3f position = !members.empty() && members[0] < positions.size()
                                          ? positions[members[0]]
                                          : Vector3f{0, 0, 0};
            rest = geom::FoldInfluences(rest, room, position, skeleton_.parents, skeleton_.pivots);
        }
        for (const geom::Influence& influence : rest) {
            kept.push_back(influence);
        }
        weights = std::move(kept);
    }

    const NodeTree& nodes() const {
        return nodes_;
    }
    const PointTable& points() const {
        return points_;
    }
    Mesh& mesh() const {
        return mesh_;
    }

private:
    /// The nearest ancestor of @p bone that is a `Bone`, unlocked, and not
    /// itself.
    u32 ancestorFor(u32 bone) const {
        u32 node = bone < nodes_.size() ? nodes_.nodes[bone].parent : kInvalidNode;
        u32 guard = 0;
        while (node != kInvalidNode && node < nodes_.size() && guard++ < nodes_.size()) {
            const Node& candidate = nodes_.nodes[node];
            if (candidate.kind == NodeKind::Bone && !candidate.skin.locked && node != bone) {
                return node;
            }
            node = candidate.parent;
        }
        return kInvalidNode;
    }

    Mesh& mesh_;
    const NodeTree& nodes_;
    const PointTable& points_;
    Skeleton skeleton_;
    std::span<const u8> vertexLocked_;
};

/// The shape every value-writing operation has: read the point, ask @p rule for
/// the request, apply §6.1, limit and write.
template <class Rule>
SkinResult WriteEach(Mesh& mesh, const NodeTree& nodes, const PointTable& points,
                     const SkinScope& scope, u32 bone, Rule&& rule) {
    SkinResult result;
    Writer writer(mesh, nodes, points);
    if (bone != kInvalidNode && writer.boneLocked(bone)) {
        // A locked bone's weight never moves, whatever is asked of it.
        result.locked = static_cast<u32>(scope.points.size());
        return result;
    }
    for (std::size_t i = 0; i < scope.points.size(); ++i) {
        const u32 point = scope.points[i];
        if (point >= points.pointCount) {
            continue;
        }
        if (writer.locked(point)) {
            ++result.locked;
            continue;
        }
        Weights weights = writer.read(point);
        const f32 was = WeightOf(weights, bone);
        const f32 wanted = rule(weights, was);
        const f32 strength = std::clamp(scope.strengthOf(i), 0.0f, 1.0f);
        const f32 request = was + (wanted - was) * strength;
        if (!writer.retarget(weights, bone, request)) {
            ++result.refused;
        }
        writer.limit(weights, point, kToolInfluenceLimit);
        writer.normalise(weights);
        writer.write(point, std::move(weights));
        ++result.changed;
    }
    return result;
}

f32 Smoothstep(f32 t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

} // namespace

SkinResult Rigid(Mesh& mesh, const NodeTree& nodes, const PointTable& points,
                 const SkinScope& scope, u32 bone) {
    SkinResult result;
    Writer writer(mesh, nodes, points);
    if (writer.boneLocked(bone)) {
        result.locked = static_cast<u32>(scope.points.size());
        return result;
    }
    for (std::size_t i = 0; i < scope.points.size(); ++i) {
        const u32 point = scope.points[i];
        if (point >= points.pointCount) {
            continue;
        }
        if (writer.locked(point)) {
            ++result.locked;
            continue;
        }
        Weights weights = writer.read(point);
        // A partial strength is a blend toward rigid, which is what a brush dab
        // of Rigid means; at full strength every unlocked influence goes.
        const f32 strength = std::clamp(scope.strengthOf(i), 0.0f, 1.0f);
        const f32 locked = writer.lockedShare(weights);
        Weights out;
        for (const geom::Influence& influence : weights) {
            if (writer.boneLocked(influence.bone)) {
                out.push_back(influence);
            } else if (strength < 1.0f && influence.bone != bone) {
                out.push_back({influence.bone, influence.weight * (1.0f - strength)});
            }
        }
        const f32 share = std::max(0.0f, 1.0f - locked);
        SetWeight(out, bone, WeightOf(weights, bone) * (1.0f - strength) + share * strength);
        writer.normalise(out);
        writer.write(point, std::move(out));
        ++result.changed;
    }
    return result;
}

SkinResult Set(Mesh& mesh, const NodeTree& nodes, const PointTable& points,
               const SkinScope& scope, u32 bone, f32 value) {
    return WriteEach(mesh, nodes, points, scope, bone,
                     [value](const Weights&, f32) { return value; });
}

SkinResult Add(Mesh& mesh, const NodeTree& nodes, const PointTable& points,
               const SkinScope& scope, u32 bone, f32 delta) {
    return WriteEach(mesh, nodes, points, scope, bone,
                     [delta](const Weights&, f32 was) { return was + delta; });
}

SkinResult Scale(Mesh& mesh, const NodeTree& nodes, const PointTable& points,
                 const SkinScope& scope, u32 bone, f32 factor) {
    return WriteEach(mesh, nodes, points, scope, bone,
                     [factor](const Weights&, f32 was) { return was * factor; });
}

SkinResult Remove(Mesh& mesh, const NodeTree& nodes, const PointTable& points,
                  const SkinScope& scope, u32 bone) {
    return Set(mesh, nodes, points, scope, bone, 0.0f);
}

SkinResult Normalize(Mesh& mesh, const NodeTree& nodes, const PointTable& points,
                     const SkinScope& scope) {
    SkinResult result;
    Writer writer(mesh, nodes, points);
    for (const u32 point : scope.points) {
        if (point >= points.pointCount) {
            continue;
        }
        if (writer.locked(point)) {
            ++result.locked;
            continue;
        }
        Weights weights = writer.read(point);
        writer.normalise(weights);
        writer.write(point, std::move(weights));
        ++result.changed;
    }
    return result;
}

SkinResult Prune(Mesh& mesh, const NodeTree& nodes, const PointTable& points,
                 const SkinScope& scope, f32 epsilon) {
    SkinResult result;
    Writer writer(mesh, nodes, points);
    for (const u32 point : scope.points) {
        if (point >= points.pointCount) {
            continue;
        }
        if (writer.locked(point)) {
            ++result.locked;
            continue;
        }
        Weights weights = writer.read(point);
        std::erase_if(weights, [&](const geom::Influence& influence) {
            return !writer.boneLocked(influence.bone) && influence.weight < epsilon;
        });
        writer.normalise(weights);
        writer.write(point, std::move(weights));
        ++result.changed;
    }
    return result;
}

SkinResult Limit(Mesh& mesh, const NodeTree& nodes, const PointTable& points,
                 const SkinScope& scope, u32 width) {
    SkinResult result;
    Writer writer(mesh, nodes, points);
    for (const u32 point : scope.points) {
        if (point >= points.pointCount) {
            continue;
        }
        if (writer.locked(point)) {
            ++result.locked;
            continue;
        }
        Weights weights = writer.read(point);
        writer.limit(weights, point, std::max<u32>(width, 1));
        writer.normalise(weights);
        writer.write(point, std::move(weights));
        ++result.changed;
    }
    return result;
}

SkinResult Replace(Mesh& mesh, const NodeTree& nodes, const PointTable& points,
                   const SkinScope& scope, u32 from, u32 to) {
    SkinResult result;
    Writer writer(mesh, nodes, points);
    if (writer.boneLocked(from) || writer.boneLocked(to) || from == to) {
        result.locked = static_cast<u32>(scope.points.size());
        return result;
    }
    for (const u32 point : scope.points) {
        if (point >= points.pointCount) {
            continue;
        }
        if (writer.locked(point)) {
            ++result.locked;
            continue;
        }
        Weights weights = writer.read(point);
        const f32 moved = WeightOf(weights, from);
        if (moved <= 0.0f) {
            continue;
        }
        // One entry per bone, never two: `to` may already be there.
        SetWeight(weights, to, WeightOf(weights, to) + moved);
        std::erase_if(weights,
                      [from](const geom::Influence& influence) { return influence.bone == from; });
        writer.normalise(weights);
        writer.write(point, std::move(weights));
        ++result.changed;
    }
    return result;
}

namespace {

/// Every bone any point in @p scope or its ring binds, and each point's weights:
/// what Smooth and Sharpen read before either writes anything.
struct Neighbourhood {
    std::map<u32, Weights> weights; ///< Point -> its weights, scope and ring.
};

Neighbourhood ReadNeighbourhood(const Writer& writer, const PointTable& points,
                                const SkinScope& scope) {
    Neighbourhood out;
    for (const u32 point : scope.points) {
        if (point >= points.pointCount) {
            continue;
        }
        out.weights.try_emplace(point, writer.read(point));
        for (const u32 neighbour : points.ringOf(point)) {
            out.weights.try_emplace(neighbour, writer.read(neighbour));
        }
    }
    return out;
}

} // namespace

SkinResult Smooth(Mesh& mesh, const NodeTree& nodes, const PointTable& points,
                  const SkinScope& scope, f32 strength, u32 passes) {
    SkinResult result;
    Writer writer(mesh, nodes, points);
    Neighbourhood around = ReadNeighbourhood(writer, points, scope);
    for (u32 pass = 0; pass < passes; ++pass) {
        std::map<u32, Weights> next;
        for (std::size_t i = 0; i < scope.points.size(); ++i) {
            const u32 point = scope.points[i];
            if (point >= points.pointCount || writer.locked(point)) {
                continue;
            }
            const std::span<const u32> ring = points.ringOf(point);
            if (ring.empty()) {
                continue;
            }
            Weights mixed = around.weights[point];
            const f32 s = std::clamp(strength * scope.strengthOf(i), 0.0f, 1.0f);
            // Per bone, over the welded ring: the mean includes neighbours
            // outside the scope, which is how a selection's edge blends into
            // what surrounds it rather than into itself.
            std::map<u32, f32> mean;
            for (const u32 neighbour : ring) {
                for (const geom::Influence& influence : around.weights[neighbour]) {
                    mean[influence.bone] += influence.weight;
                }
            }
            for (auto& [bone, sum] : mean) {
                sum /= static_cast<f32>(ring.size());
            }
            for (geom::Influence& influence : mixed) {
                const auto found = mean.find(influence.bone);
                const f32 target = found != mean.end() ? found->second : 0.0f;
                influence.weight = (1.0f - s) * influence.weight + s * target;
            }
            for (const auto& [bone, value] : mean) {
                if (WeightOf(mixed, bone) == 0.0f && value > 0.0f) {
                    SetWeight(mixed, bone, s * value);
                }
            }
            next[point] = std::move(mixed);
        }
        for (auto& [point, weights] : next) {
            around.weights[point] = weights;
        }
        if (pass + 1 == passes) {
            for (auto& [point, weights] : next) {
                Weights out = std::move(weights);
                writer.limit(out, point, kToolInfluenceLimit);
                writer.normalise(out);
                writer.write(point, std::move(out));
                ++result.changed;
            }
        }
    }
    for (const u32 point : scope.points) {
        if (point < points.pointCount && writer.locked(point)) {
            ++result.locked;
        }
    }
    return result;
}

SkinResult Sharpen(Mesh& mesh, const NodeTree& nodes, const PointTable& points,
                   const SkinScope& scope, f32 strength) {
    SkinResult result;
    Writer writer(mesh, nodes, points);
    const Neighbourhood around = ReadNeighbourhood(writer, points, scope);
    for (std::size_t i = 0; i < scope.points.size(); ++i) {
        const u32 point = scope.points[i];
        if (point >= points.pointCount) {
            continue;
        }
        if (writer.locked(point)) {
            ++result.locked;
            continue;
        }
        const std::span<const u32> ring = points.ringOf(point);
        Weights weights = around.weights.at(point);
        if (!ring.empty()) {
            const f32 s = std::max(0.0f, strength * scope.strengthOf(i));
            std::map<u32, f32> mean;
            for (const u32 neighbour : ring) {
                for (const geom::Influence& influence : around.weights.at(neighbour)) {
                    mean[influence.bone] += influence.weight;
                }
            }
            for (geom::Influence& influence : weights) {
                const auto found = mean.find(influence.bone);
                const f32 average =
                    found != mean.end() ? found->second / static_cast<f32>(ring.size()) : 0.0f;
                influence.weight =
                    std::max(0.0f, influence.weight + s * (influence.weight - average));
            }
        }
        writer.normalise(weights);
        writer.write(point, std::move(weights));
        ++result.changed;
    }
    return result;
}

SkinResult Unify(Mesh& mesh, const NodeTree& nodes, const PointTable& points,
                 const SkinScope& scope) {
    SkinResult result;
    Writer writer(mesh, nodes, points);
    for (const u32 point : scope.points) {
        if (point >= points.pointCount) {
            continue;
        }
        if (writer.locked(point)) {
            ++result.locked;
            continue;
        }
        const std::span<const u32> members = points.membersOf(point);
        if (members.size() < 2) {
            continue;
        }
        std::map<u32, f32> mean;
        for (const u32 vertex : members) {
            for (const geom::Influence& influence : mesh.skin.forVertex(vertex)) {
                mean[influence.bone] += influence.weight;
            }
        }
        Weights weights;
        for (const auto& [bone, sum] : mean) {
            weights.push_back({bone, sum / static_cast<f32>(members.size())});
        }
        writer.normalise(weights);
        writer.write(point, std::move(weights));
        ++result.changed;
    }
    return result;
}

SkinResult Soften(Mesh& mesh, const NodeTree& nodes, const PointTable& points,
                  const SkinScope& scope, u32 boneA, u32 boneB) {
    SkinResult result;
    if (boneA == boneB || scope.points.empty()) {
        return result;
    }
    Writer writer(mesh, nodes, points);

    // The band plus one ring: the points the distances are measured over, and
    // where the two ends are looked for.
    std::vector<u32> field = GrowPoints(points, scope.points, 1);
    std::vector<u8> inField(points.pointCount, 0);
    for (const u32 point : field) {
        inField[point] = 1;
    }

    std::vector<u8> inBand(points.pointCount, 0);
    for (const u32 point : scope.points) {
        if (point < points.pointCount) {
            inBand[point] = 1;
        }
    }

    // A walk over the welded ring, one hop per edge, from every point OUTSIDE
    // the band that goes wholly to the bone. Outside, because the band is what
    // is being rewritten: a band point that is already wholly A would otherwise
    // anchor itself and the blend would be a step.
    const auto distancesFrom = [&](u32 bone) {
        std::vector<u32> distance(points.pointCount, kInvalidIndex);
        std::queue<u32> front;
        for (const u32 point : field) {
            if (inBand[point] != 0) {
                continue;
            }
            const Weights weights = writer.read(point);
            if (WeightOf(weights, bone) >= 0.999f) {
                distance[point] = 0;
                front.push(point);
            }
        }
        while (!front.empty()) {
            const u32 point = front.front();
            front.pop();
            for (const u32 neighbour : points.ringOf(point)) {
                if (inField[neighbour] != 0 && distance[neighbour] == kInvalidIndex) {
                    distance[neighbour] = distance[point] + 1;
                    front.push(neighbour);
                }
            }
        }
        return distance;
    };
    const std::vector<u32> toA = distancesFrom(boneA);
    const std::vector<u32> toB = distancesFrom(boneB);

    for (std::size_t i = 0; i < scope.points.size(); ++i) {
        const u32 point = scope.points[i];
        if (point >= points.pointCount) {
            continue;
        }
        if (writer.locked(point)) {
            ++result.locked;
            continue;
        }
        const u32 dA = toA[point];
        const u32 dB = toB[point];
        if (dA == kInvalidIndex || dB == kInvalidIndex) {
            // One end is out of reach, so there is no blend to write.
            ++result.refused;
            continue;
        }
        const f32 t = dA + dB == 0
                          ? 0.5f
                          : Smoothstep(static_cast<f32>(dA) / static_cast<f32>(dA + dB));
        Weights weights;
        weights.push_back({boneA, 1.0f - t});
        weights.push_back({boneB, t});
        writer.normalise(weights);
        writer.write(point, std::move(weights));
        ++result.changed;
    }
    return result;
}

bool PointLocked(const Mesh& mesh, const PointTable& points, u32 point) {
    const std::span<const u8> locked =
        mesh.attributes.get<u8>(geom::names::kSkinLocked, geom::Domain::Vertex);
    if (locked.empty()) {
        return false;
    }
    const std::span<const u32> members = points.membersOf(point);
    if (members.empty()) {
        return false;
    }
    for (const u32 vertex : members) {
        if (vertex < locked.size() && locked[vertex] == 0) {
            return false;
        }
    }
    return true;
}

u32 SetPointsLocked(Mesh& mesh, const PointTable& points, std::span<const u32> scope, bool locked) {
    if (scope.empty()) {
        return 0;
    }
    std::span<u8> layer = mesh.attributes.get<u8>(geom::names::kSkinLocked, geom::Domain::Vertex);
    if (layer.empty()) {
        if (!locked) {
            return 0; // Clearing where nothing is locked writes no layer.
        }
        layer = mesh.attributes.getOrCreate<u8>(geom::names::kSkinLocked, geom::Domain::Vertex,
                                                geom::AttrType::Bool);
    }
    u32 changed = 0;
    for (const u32 point : scope) {
        if (point >= points.pointCount) {
            continue;
        }
        bool moved = false;
        for (const u32 vertex : points.membersOf(point)) {
            if (vertex < layer.size() && (layer[vertex] != 0) != locked) {
                layer[vertex] = locked ? 1 : 0;
                moved = true;
            }
        }
        changed += moved ? 1 : 0;
    }
    return changed;
}

} // namespace skinning
} // namespace wem
} // namespace models
} // namespace whiteout
