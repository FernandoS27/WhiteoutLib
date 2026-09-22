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
    Writer(Mesh& mesh, const NodeTree& nodes, const PointTable& points,
           std::span<const u32> heldBones = {})
        : mesh_(mesh), nodes_(nodes), points_(points), skeleton_(nodes),
          vertexLocked_(mesh.attributes.get<u8>(geom::names::kSkinLocked, geom::Domain::Vertex)),
          heldBones_(heldBones) {}

    bool boneLocked(u32 bone) const {
        if (bone < nodes_.size() && nodes_.nodes[bone].skin.locked) {
            return true;
        }
        // The caller's own set, held for this call alone. A bone set is short --
        // the checked rows of the bone list -- so a linear scan beats building
        // a lookup per operation.
        return std::find(heldBones_.begin(), heldBones_.end(), bone) != heldBones_.end();
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
    ///
    /// @return whether any member's influences moved: the result line counts
    /// what an operation changed, not what it looked at.
    bool write(u32 point, Weights weights) {
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
        if (mesh_.skin.offsets.empty()) {
            if (weights.empty()) {
                return false;
            }
            // A mesh no file skinned at all. An EMPTY binding is not a binding
            // of empty vertices -- `assignVertex` splices into an array that
            // has no row for the vertex yet and does nothing -- so the first
            // weight written to such a mesh is what makes it skinned. Without
            // this, skinning a fresh part from scratch silently writes nothing.
            mesh_.skin.reset(mesh_.vertexCount());
        }
        bool moved = false;
        for (const u32 vertex : points_.membersOf(point)) {
            // `assignVertex` sorts exactly as above, so the stored row compares
            // element by element.
            const std::span<const geom::Influence> stored = mesh_.skin.forVertex(vertex);
            const bool same =
                stored.size() == weights.size() &&
                std::equal(stored.begin(), stored.end(), weights.begin(),
                           [](const geom::Influence& a, const geom::Influence& b) {
                               return a.bone == b.bone && a.weight == b.weight;
                           });
            if (!same) {
                mesh_.skin.assignVertex(vertex, weights);
                moved = true;
            }
        }
        return moved;
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
        // The last case is a LOWERING: the bone held weight and gives some up.
        // A point with no unlocked weight at all -- never skinned, or holding
        // only locked bones -- has nothing to hand on, and a Remove or a
        // Subtract passing over it must not skin it to the bone's parent.
        const f32 was = WeightOf(weights, bone);
        if (others <= 0.0f && rest > 0.0f && was > target) {
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
    ///
    /// Every locked entry is kept, even past @p width: a lock is never broken
    /// to make room (§6.2). When the locks fill every lane the unlocked share
    /// has nowhere to go and is dropped rather than folded -- there is no lane
    /// to fold it into.
    void limit(Weights& weights, u32 point, u32 width) const {
        if (weights.size() <= width) {
            return;
        }
        Weights kept;
        Weights rest;
        for (const geom::Influence& influence : weights) {
            (boneLocked(influence.bone) ? kept : rest).push_back(influence);
        }
        std::sort(rest.begin(), rest.end(), [](const geom::Influence& a, const geom::Influence& b) {
            if (a.weight != b.weight) {
                return a.weight > b.weight;
            }
            return a.bone < b.bone;
        });
        const u32 room = kept.size() < width ? width - static_cast<u32>(kept.size()) : 0;
        if (room == 0) {
            rest.clear();
        } else if (rest.size() > room) {
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
            if (candidate.kind == NodeKind::Bone && !boneLocked(node) && node != bone) {
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
    std::span<const u32> heldBones_;
};

/// The shape every value-writing operation has: read the point, ask @p rule for
/// the request, apply §6.1, limit and write.
template <class Rule>
SkinResult WriteEach(Mesh& mesh, const NodeTree& nodes, const PointTable& points,
                     const SkinScope& scope, u32 bone, Rule&& rule) {
    SkinResult result;
    Writer writer(mesh, nodes, points, scope.heldBones);
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
        if (writer.write(point, std::move(weights))) {
            ++result.changed;
        }
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
    Writer writer(mesh, nodes, points, scope.heldBones);
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
        // A partial dab keeps every other influence, and the locks stay whatever
        // their number: either can take a point past the tools' four (§6.3).
        writer.limit(out, point, kToolInfluenceLimit);
        writer.normalise(out);
        if (writer.write(point, std::move(out))) {
            ++result.changed;
        }
    }
    return result;
}

SkinResult Assign(Mesh& mesh, const NodeTree& nodes, const PointTable& points,
                  const SkinScope& scope, const PointWeights& given) {
    SkinResult result;
    Writer writer(mesh, nodes, points, scope.heldBones);
    for (std::size_t i = 0; i < scope.points.size(); ++i) {
        const u32 point = scope.points[i];
        if (point >= points.pointCount) {
            continue;
        }
        if (writer.locked(point)) {
            ++result.locked;
            continue;
        }
        const f32 strength = std::clamp(scope.strengthOf(i), 0.0f, 1.0f);
        if (strength <= 0.0f) {
            continue;
        }
        const Weights held = writer.read(point);
        const f32 locked = writer.lockedShare(held);
        const f32 room = std::max(0.0f, 1.0f - locked);

        // What was asked for, minus the bones no operation may write. A locked
        // bone named by a transfer or a paste is dropped here rather than in
        // the write, so the rest of the set still shares the whole of `1 - L`.
        Weights wanted;
        f32 total = 0.0f;
        for (const geom::Influence& influence : given.of(i)) {
            if (!(influence.weight > 0.0f) || !std::isfinite(influence.weight)) {
                continue;
            }
            if (writer.boneLocked(influence.bone)) {
                continue;
            }
            // New weight lands on Bone nodes only (§6.4): a paste or a transfer
            // from a point bound to a helper gives the helper nothing more, and
            // a node the tree does not have -- a clipboard from before a removal
            // -- is never written at all.
            if (influence.bone >= nodes.size() ||
                nodes.nodes[influence.bone].kind != NodeKind::Bone) {
                continue;
            }
            SetWeight(wanted, influence.bone,
                      WeightOf(wanted, influence.bone) + influence.weight);
            total += influence.weight;
        }
        if (total <= 0.0f) {
            // Nothing writable was asked for, so the point keeps what it has --
            // §6.1's last case, counted rather than blanked.
            ++result.refused;
            continue;
        }

        Weights out;
        for (const geom::Influence& influence : held) {
            if (writer.boneLocked(influence.bone)) {
                out.push_back(influence);
            }
        }
        // Every bone either side names, once: the lerp has to run over the
        // union, or a bone only the old set holds would keep its full weight
        // through a partial dab.
        std::vector<u32> bones;
        for (const geom::Influence& influence : held) {
            if (!writer.boneLocked(influence.bone)) {
                bones.push_back(influence.bone);
            }
        }
        for (const geom::Influence& influence : wanted) {
            if (std::find(bones.begin(), bones.end(), influence.bone) == bones.end()) {
                bones.push_back(influence.bone);
            }
        }
        for (const u32 bone : bones) {
            const f32 to = WeightOf(wanted, bone) * room / total;
            const f32 from = WeightOf(held, bone);
            const f32 value = from + (to - from) * strength;
            if (value > 0.0f) {
                out.push_back({bone, value});
            }
        }
        writer.normalise(out);
        writer.limit(out, point, kToolInfluenceLimit);
        writer.normalise(out);
        if (writer.write(point, std::move(out))) {
            ++result.changed;
        }
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
    Writer writer(mesh, nodes, points, scope.heldBones);
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
        if (writer.write(point, std::move(weights))) {
            ++result.changed;
        }
    }
    return result;
}

SkinResult Prune(Mesh& mesh, const NodeTree& nodes, const PointTable& points,
                 const SkinScope& scope, f32 epsilon) {
    SkinResult result;
    Writer writer(mesh, nodes, points, scope.heldBones);
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
        if (writer.write(point, std::move(weights))) {
            ++result.changed;
        }
    }
    return result;
}

SkinResult Limit(Mesh& mesh, const NodeTree& nodes, const PointTable& points,
                 const SkinScope& scope, u32 width) {
    SkinResult result;
    Writer writer(mesh, nodes, points, scope.heldBones);
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
        if (writer.write(point, std::move(weights))) {
            ++result.changed;
        }
    }
    return result;
}

SkinResult Replace(Mesh& mesh, const NodeTree& nodes, const PointTable& points,
                   const SkinScope& scope, u32 from, u32 to) {
    SkinResult result;
    Writer writer(mesh, nodes, points, scope.heldBones);
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
        if (writer.write(point, std::move(weights))) {
            ++result.changed;
        }
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
    Writer writer(mesh, nodes, points, scope.heldBones);
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
            // A locked bone is neither blended nor brought in from the ring: its
            // weight on this point never moves (§6.2), and `normalise` then fits
            // the blend into what the locks leave.
            for (geom::Influence& influence : mixed) {
                if (writer.boneLocked(influence.bone)) {
                    continue;
                }
                const auto found = mean.find(influence.bone);
                const f32 target = found != mean.end() ? found->second : 0.0f;
                influence.weight = (1.0f - s) * influence.weight + s * target;
            }
            for (const auto& [bone, value] : mean) {
                if (writer.boneLocked(bone)) {
                    continue;
                }
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
                if (writer.write(point, std::move(out))) {
                    ++result.changed;
                }
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
    Writer writer(mesh, nodes, points, scope.heldBones);
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
                if (writer.boneLocked(influence.bone)) {
                    continue; // §6.2, as in Smooth
                }
                const auto found = mean.find(influence.bone);
                const f32 average =
                    found != mean.end() ? found->second / static_cast<f32>(ring.size()) : 0.0f;
                influence.weight =
                    std::max(0.0f, influence.weight + s * (influence.weight - average));
            }
        }
        writer.normalise(weights);
        if (writer.write(point, std::move(weights))) {
            ++result.changed;
        }
    }
    return result;
}

SkinResult Unify(Mesh& mesh, const NodeTree& nodes, const PointTable& points,
                 const SkinScope& scope) {
    SkinResult result;
    Writer writer(mesh, nodes, points, scope.heldBones);
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
        // The unlocked bones take the members' mean, read as `read` reads (a
        // NaN or a negative share is no weight); a locked bone keeps the value
        // the point reads, its first member's, because no operation moves it.
        std::map<u32, f32> mean;
        for (const u32 vertex : members) {
            for (const geom::Influence& influence : mesh.skin.forVertex(vertex)) {
                if (influence.weight > 0.0f && std::isfinite(influence.weight) &&
                    !writer.boneLocked(influence.bone)) {
                    mean[influence.bone] += influence.weight;
                }
            }
        }
        Weights weights;
        for (const geom::Influence& influence : writer.read(point)) {
            if (writer.boneLocked(influence.bone)) {
                weights.push_back(influence);
            }
        }
        for (const auto& [bone, sum] : mean) {
            weights.push_back({bone, sum / static_cast<f32>(members.size())});
        }
        // Members that disagree can name eight bones between them.
        writer.limit(weights, point, kToolInfluenceLimit);
        writer.normalise(weights);
        if (writer.write(point, std::move(weights))) {
            ++result.changed;
        }
    }
    return result;
}

SkinResult Soften(Mesh& mesh, const NodeTree& nodes, const PointTable& points,
                  const SkinScope& scope, u32 boneA, u32 boneB) {
    SkinResult result;
    if (boneA == boneB || scope.points.empty()) {
        return result;
    }
    Writer writer(mesh, nodes, points, scope.heldBones);
    if (writer.boneLocked(boneA) || writer.boneLocked(boneB)) {
        // The blend IS the two bones' weights: with either locked there is
        // nothing Soften may write (§6.2).
        result.locked = static_cast<u32>(scope.points.size());
        return result;
    }

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
        // §9.4's Set(A, 1 − t) then Set(B, t), inside the share the locks leave:
        // the locked bones stay exactly, and every other unlocked influence of
        // the band goes, as Rigid's does -- the band is rewritten as the blend.
        const Weights before = writer.read(point);
        const f32 room = std::max(0.0f, 1.0f - writer.lockedShare(before));
        Weights weights;
        for (const geom::Influence& influence : before) {
            if (writer.boneLocked(influence.bone)) {
                weights.push_back(influence);
            }
        }
        weights.push_back({boneA, (1.0f - t) * room});
        weights.push_back({boneB, t * room});
        writer.limit(weights, point, kToolInfluenceLimit);
        writer.normalise(weights);
        if (writer.write(point, std::move(weights))) {
            ++result.changed;
        }
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
