// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "whiteout/models/wem/rigging/rebind.h"

#include "whiteout/models/wem/anim/pose.h"
#include "whiteout/models/wem/anim/track_read.h"
#include "whiteout/models/wem/rigging/limbs.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace whiteout {
namespace models {
namespace wem {

namespace {

/// Below this a rotation is the identity and a distance is nothing.
constexpr f32 kTiny = 1e-6f;

/// @p v turned by @p q.
Vector3f Turn(const Quaternion& q, const Vector3f& v) {
    const Vector3f u{q.x, q.y, q.z};
    const Vector3f t = ::whiteout::cross(u, v) * 2.0f;
    return v + t * q.w + ::whiteout::cross(u, t);
}

bool IsIdentity(const Quaternion& q) {
    return std::fabs(std::fabs(q.w) - 1.0f) < 1e-7f;
}

/// A constant turn this small is the rounding of two readings of one frame —
/// a node its parent carries rigidly, read once through a matrix — and a key of
/// it would declare a channel for nothing. `IsIdentity`'s 1e-7 is below one
/// ulp of `w`; this is on the axis, about 2e-4 of a degree.
bool Negligible(const Quaternion& q) {
    return Vector3f{q.x, q.y, q.z}.length() < 2e-6f;
}

/// One node's move, in the model's space: `v -> (v - from) * turn + to`.
struct Move {
    Quaternion turn{0, 0, 0, 1};
    Vector3f from{0, 0, 0};
    Vector3f to{0, 0, 0};
    bool moves = false;

    Vector3f point(const Vector3f& v) const {
        return Turn(turn, v - from) + to;
    }
};

/// @p v through @p m, as a point.
Vector3f Point(const Matrix44f& m, const Vector3f& v) {
    return Vector3f{v.x * m.data[0][0] + v.y * m.data[1][0] + v.z * m.data[2][0] + m.data[3][0],
                    v.x * m.data[0][1] + v.y * m.data[1][1] + v.z * m.data[2][1] + m.data[3][1],
                    v.x * m.data[0][2] + v.y * m.data[1][2] + v.z * m.data[2][2] + m.data[3][2]};
}

/// `B(b)` as a matrix: `T(-from) * Rot(turn) * T(to)`, the rigid move the whole
/// rewrite is built on. The fit needs it as a matrix because it inverts it.
Matrix44f MoveMatrix(const Move& one) {
    Matrix44f out = ToMatrix(Transform{Vector3f{0, 0, 0}, one.turn, Vector3f{1, 1, 1}});
    const Vector3f moved = Turn(one.turn, Vector3f{-one.from.x, -one.from.y, -one.from.z});
    out.data[3][0] = moved.x + one.to.x;
    out.data[3][1] = moved.y + one.to.y;
    out.data[3][2] = moved.z + one.to.z;
    return out;
}

/// Solves the symmetric `N * x = rhs` by Cramer, or answers false when `N` is
/// too near singular to trust — a vertex whose bones never move in any clip.
bool Solve3(const f32 n[9], const Vector3f& rhs, Vector3f& out) {
    const f32 a = n[0], b = n[1], cc = n[2];
    const f32 d = n[3], e = n[4], f = n[5];
    const f32 g = n[6], h = n[7], i = n[8];
    const f32 det = a * (e * i - f * h) - b * (d * i - f * g) + cc * (d * h - e * g);
    const f32 scale = std::fabs(a) + std::fabs(e) + std::fabs(i);
    if (std::fabs(det) <= 1e-9f * scale * scale * scale) {
        return false;
    }
    const f32 inv = 1.0f / det;
    out.x = inv * (rhs.x * (e * i - f * h) - b * (rhs.y * i - f * rhs.z) +
                   cc * (rhs.y * h - e * rhs.z));
    out.y = inv * (a * (rhs.y * i - f * rhs.z) - rhs.x * (d * i - f * g) +
                   cc * (d * rhs.z - rhs.y * g));
    out.z = inv * (a * (e * rhs.z - rhs.y * h) - b * (d * rhs.z - rhs.y * g) +
                   rhs.x * (d * h - e * g));
    return true;
}

/// Where a node rests now: its pivot in a pivot rig, its composed bind
/// otherwise. The same reading `SolveTPose` takes.
Vector3f RestAt(const NodeTree& tree, u32 node) {
    if (node >= tree.size()) {
        return {0, 0, 0};
    }
    return tree.rig == RigConvention::PivotRelative ? tree.nodes[node].pivot
                                                    : tree.worldBind(node).translation;
}

/// The channel a sub-track drives, for a node.
struct NodeChannel {
    u32 node = kInvalidNode;
    Channel channel = Channel::Translation;
};

std::unordered_map<u32, NodeChannel> NodeChannels(const Model& model) {
    std::unordered_map<u32, NodeChannel> out;
    for (const AnimChannel& channel : model.animChannels.channels) {
        if (channel.target.kind != TrackTarget::Kind::Node) {
            continue;
        }
        if (channel.target.channel != Channel::Rotation &&
            channel.target.channel != Channel::Translation) {
            continue;
        }
        out.emplace(channel.id, NodeChannel{channel.target.node, channel.target.channel});
    }
    return out;
}

/// The channel a node is keyed on for @p which, declaring one if it has none —
/// a node the re-bind moved needs somewhere to put its constant key.
u32 NodeChannelOf(Model& model, u32 node, Channel which, geom::AttrType type) {
    for (const AnimChannel& channel : model.animChannels.channels) {
        if (channel.target.kind == TrackTarget::Kind::Node && channel.target.node == node &&
            channel.target.channel == which && channel.valueType == type) {
            return channel.id;
        }
    }
    AnimChannel made;
    made.id = model.animChannels.nextFreeId();
    made.target.kind = TrackTarget::Kind::Node;
    made.target.node = node;
    made.target.channel = which;
    made.valueType = type;
    return model.animChannels.add(made);
}

u32 RotationChannelOf(Model& model, u32 node) {
    return NodeChannelOf(model, node, Channel::Rotation, geom::AttrType::Quat);
}

u32 TranslationChannelOf(Model& model, u32 node) {
    return NodeChannelOf(model, node, Channel::Translation, geom::AttrType::F32x3);
}

void WriteQuat(std::vector<u8>& into, std::size_t at, const Quaternion& q) {
    const f32 values[4] = {q.x, q.y, q.z, q.w};
    std::memcpy(into.data() + at, values, sizeof(values));
}

Quaternion ReadQuat(const std::vector<u8>& from, std::size_t at) {
    f32 values[4];
    std::memcpy(values, from.data() + at, sizeof(values));
    return Quaternion{values[0], values[1], values[2], values[3]};
}

void WriteVec3(std::vector<u8>& into, std::size_t at, const Vector3f& v) {
    const f32 values[3] = {v.x, v.y, v.z};
    std::memcpy(into.data() + at, values, sizeof(values));
}

Vector3f ReadVec3(const std::vector<u8>& from, std::size_t at) {
    f32 values[3];
    std::memcpy(values, from.data() + at, sizeof(values));
    return Vector3f{values[0], values[1], values[2]};
}

/// What each node does, in the model's space: one @ref Move per node.
std::vector<Move> MovesOf(const NodeTree& tree, std::span<const Transform> rest) {
    std::vector<Move> move(tree.size());
    for (u32 n = 0; n < tree.size(); ++n) {
        move[n].turn =
            IsIdentity(rest[n].rotation) ? Quaternion::identity() : rest[n].rotation.normalized();
        move[n].from = RestAt(tree, n);
        move[n].to = rest[n].translation;
        move[n].moves =
            !IsIdentity(move[n].turn) || (move[n].to - move[n].from).length() > kTiny;
    }
    return move;
}

/// An offset under this share of the model is the rounding of the caller's
/// own composition, not a move. `TPoseRest` carries a pivot through matrices
/// and this file through quaternions: over the five HD T-poses `rebind_test`
/// re-binds, the widest gap is 1.21e-07 of the model (the grunt), so 100x that.
constexpr f32 kOffsetTiny = 1.2e-05f;

/// Where each node's new pivot sits off the place its parent's move carries
/// the old one, `o_b = p'_b - m_parent(p_b)`: what a key's translation gives
/// back so a node that moved on its own still plays where it played. Zero,
/// snapped, for a rotation-only rest; a free move, a clip frame or the grid
/// lift makes one.
std::vector<Vector3f> OffsetsOf(const NodeTree& tree, const std::vector<Move>& move, f32 size) {
    std::vector<Vector3f> offset(tree.size(), Vector3f{0, 0, 0});
    for (u32 n = 0; n < tree.size(); ++n) {
        const u32 parent = ParentOf(tree, n);
        const bool carried = parent != kInvalidNode && move[parent].moves;
        if (!move[n].moves && !carried) {
            continue;
        }
        const Vector3f off = move[n].to - (carried ? move[parent].point(move[n].from)
                                                   : move[n].from);
        if (off.length() > kOffsetTiny * size) {
            offset[n] = off;
        }
    }
    return offset;
}

bool Shifted(const Vector3f& offset) {
    return offset.x != 0.0f || offset.y != 0.0f || offset.z != 0.0f;
}

/// Which bones hold each vertex: the binding where there is one, and a
/// section's `rigidNode` where every vertex of it binds at weight 1.
std::vector<std::vector<geom::Influence>> HoldsOf(const Mesh& mesh, std::size_t count) {
    std::vector<std::vector<geom::Influence>> holds(count);
    if (!mesh.skin.empty()) {
        for (u32 v = 0; v < count && v < mesh.skin.vertexCount(); ++v) {
            for (const geom::Influence& one : mesh.skin.forVertex(v)) {
                holds[v].push_back(one);
            }
        }
        return holds;
    }
    const geom::FaceSet& faces = mesh.faceSet();
    const std::span<const u32> sectionOf = mesh.faceSections();
    std::size_t corner = 0;
    for (std::size_t f = 0; f < faces.faceValence.size(); ++f) {
        const u32 section = f < sectionOf.size() ? sectionOf[f] : 0u;
        const std::optional<u32> rigid =
            section < mesh.sections.size() ? mesh.sections[section].rigidNode : std::nullopt;
        for (u32 k = 0; k < faces.faceValence[f]; ++k, ++corner) {
            if (!rigid || corner >= faces.cornerVertex.size()) {
                continue;
            }
            const u32 v = faces.cornerVertex[corner];
            if (v < holds.size() && holds[v].empty()) {
                holds[v].push_back(geom::Influence{*rigid, 1.0f});
            }
        }
    }
    return holds;
}

/// Where @p holds and @p move together put one vertex. The one place a re-bind
/// is not exact: a vertex blended between two bones is skinned twice.
bool Landing(const std::vector<geom::Influence>& holds, const std::vector<Move>& move,
             const Vector3f& from, Vector3f& out) {
    f32 weight = 0;
    Vector3f moved{0, 0, 0};
    for (const geom::Influence& one : holds) {
        if (one.bone >= move.size() || one.weight <= 0) {
            continue;
        }
        weight += one.weight;
        moved = moved + move[one.bone].point(from) * one.weight;
    }
    if (weight <= kTiny) {
        return false;
    }
    out = moved * (1.0f / weight);
    return true;
}

/// Which node channels each of @p model's clips READS, in any container: what
/// the constant passes ask once per node and clip, looked up rather than
/// walked. A sub-track is read where its clip's rule reads a key of it — under
/// `Wc3` only the keys inside the window, so the bracket an importer slices
/// round a sequence that keys nothing plays the rest there — and only the first
/// of its channel in a container is read at all (`SubTrackContainer::find`).
/// A snapshot, so it is taken after the writes it has to see.
struct ClipKeys {
    const Document& document;
    std::vector<std::vector<u32>> rotation;          ///< Per node, its channel ids.
    std::vector<std::vector<u32>> translation;       ///< Per node, its channel ids.
    std::vector<std::unordered_set<u32>> keyed;      ///< Per clip; empty for another model's.
    std::unordered_map<u32, Interpolation> interpOf; ///< Per channel, as a clip keys it.

    ClipKeys(const Document& in, u32 model) : document(in) {
        const Model& owner = in.models[model];
        rotation.resize(owner.nodes.size());
        translation.resize(owner.nodes.size());
        std::unordered_map<u32, geom::AttrType> typeOf;
        for (const AnimChannel& channel : owner.animChannels.channels) {
            if (channel.target.kind != TrackTarget::Kind::Node ||
                channel.target.node >= owner.nodes.size()) {
                continue;
            }
            if (channel.target.channel == Channel::Rotation) {
                rotation[channel.target.node].push_back(channel.id);
            } else if (channel.target.channel == Channel::Translation) {
                translation[channel.target.node].push_back(channel.id);
            } else {
                continue;
            }
            typeOf.emplace(channel.id, channel.valueType);
        }
        keyed.resize(in.clips.size());
        for (u32 c = 0; c < in.clips.size(); ++c) {
            const Clip& clip = in.clips[c];
            if (clip.model != model) {
                continue;
            }
            const SampleWindow window = ClipWindow(clip, 0, -1);
            for (const SubTrackContainer& container : clip.containers) {
                std::unordered_set<u32> first;
                for (const SubTrack& track : container.subTracks) {
                    const auto type = typeOf.find(track.channel);
                    if (type == typeOf.end() || !first.insert(track.channel).second ||
                        track.times.empty()) {
                        continue;
                    }
                    interpOf.emplace(track.channel, track.interp);
                    if (ReadTrack(clip, track, type->second, window)) {
                        keyed[c].insert(track.channel);
                    }
                }
            }
        }
    }

    /// Whether clip @p clip reads @p node's @p which channel.
    bool keys(u32 clip, u32 node, Channel which) const {
        for (const u32 id : (which == Channel::Rotation ? rotation : translation)[node]) {
            if (keyed[clip].count(id) != 0) {
                return true;
            }
        }
        return false;
    }

    /// Whether a global loop reads it: a node keyed on a loop of its own plays
    /// that track through every clip, and a second would claim it.
    bool onLoop(u32 node, Channel which) const {
        for (u32 c = 0; c < keyed.size(); ++c) {
            if (IsGlobalLoop(document.clips[c]) && keys(c, node, which)) {
                return true;
            }
        }
        return false;
    }

    /// The interpolation @p channel is keyed in elsewhere, @p otherwise where
    /// no clip keys it.
    Interpolation interp(u32 channel, Interpolation otherwise) const {
        const auto found = interpOf.find(channel);
        return found == interpOf.end() ? otherwise : found->second;
    }
};

/// Whether @p clip may take a constant for a node: a loop plays over every
/// clip and keys only what it animates (its container abstains on the rest),
/// so a constant there would override the node in every clip.
bool TakesConstant(const Clip& clip, u32 model) {
    return clip.model == model && !clip.containers.empty() && !IsGlobalLoop(clip);
}

/// Makes @p clip play @p channel at one value, a turn @p q or a step @p t: a
/// key in the sub-track the clip has for it where that one is read nowhere in
/// the window — the first of a channel is the one read, so a second would be
/// ignored — else a sub-track of its own. In the interpolation the channel is
/// keyed in elsewhere: `toMdx` writes a channel's clips as ONE track, and a
/// part with fewer values per key would flatten every curve it has. Flat, so
/// every control point of a turn is the turn, a Bezier handle is the step and
/// a Hermite tangent (a derivative) is zero.
void PutConstant(Clip& clip, u32 channel, Interpolation interp, bool rotation, const Quaternion& q,
                 const Vector3f& t) {
    SubTrack* into = nullptr;
    for (SubTrackContainer& container : clip.containers) {
        for (SubTrack& track : container.subTracks) {
            if (track.channel == channel) {
                into = &track;
                break;
            }
        }
        if (into != nullptr) {
            break;
        }
    }
    if (into == nullptr) {
        clip.containers.front().subTracks.emplace_back();
        into = &clip.containers.front().subTracks.back();
    }
    *into = SubTrack{};
    into->channel = channel;
    into->interp = interp;
    into->times.push_back(0.0f);
    const u32 stride = ValuesPerKey(interp);
    const std::size_t each = (rotation ? 4 : 3) * sizeof(f32);
    into->values.resize(stride * each);
    for (u32 k = 0; k < stride; ++k) {
        if (rotation) {
            WriteQuat(into->values, k * each, q);
        } else {
            WriteVec3(into->values, k * each,
                      k == 0 || interp == Interpolation::Bezier ? t : Vector3f{0, 0, 0});
        }
    }
}

/// Take the change (§5.3): every key of each posed node with its delta laid
/// over it, as `PoseNodes` lays one over a sampled frame — a node with a delta
/// and no track a clip reads gains a constant — before the re-bind proper runs
/// as it does for play as before. Both are constant maps per key, so the
/// curve between keys is carried exactly. The keys it lays a delta over are
/// counted once, by the key pass that re-binds them after; the constants it
/// puts are counted as added, and @p constants names them per clip so that pass
/// does not count them again.
void CarryPose(Document& document, u32 model, std::span<const PoseDelta> carry,
               RebindResult& result, std::vector<std::unordered_set<u32>>& constants) {
    Model& owner = document.models[model];
    const u32 count = owner.nodes.size();
    const std::unordered_map<u32, NodeChannel> channels = NodeChannels(owner);
    const ClipKeys index(document, model);
    const auto turns = [&](u32 n) { return !IsIdentity(carry[n].rotation.normalized()); };
    const auto steps = [&](u32 n) { return carry[n].translation.length() > 0.0f; };
    for (Clip& clip : document.clips) {
        if (clip.model != model) {
            continue;
        }
        for (SubTrackContainer& container : clip.containers) {
            for (SubTrack& track : container.subTracks) {
                const auto found = channels.find(track.channel);
                if (found == channels.end() || found->second.node >= count) {
                    continue;
                }
                const u32 node = found->second.node;
                const u32 stride = ValuesPerKey(track.interp);
                if (found->second.channel == Channel::Rotation && turns(node)) {
                    // Left-multiplying every control point by one unit
                    // quaternion keeps every raw dot, so `Wc3Slerp` and
                    // `Wc3Squad` read the same curve, turned.
                    const Quaternion delta = carry[node].rotation.normalized();
                    const std::size_t each = 4 * sizeof(f32);
                    const std::size_t keys =
                        std::min(track.times.size(), track.values.size() / (stride * each));
                    for (std::size_t k = 0; k < keys * stride; ++k) {
                        WriteQuat(track.values, k * each, delta * ReadQuat(track.values, k * each));
                    }
                } else if (found->second.channel == Channel::Translation && steps(node)) {
                    // The constant on values and Bezier handles, never on a
                    // Hermite tangent (a derivative), as §5.1's offset.
                    const bool handles = track.interp == Interpolation::Bezier;
                    const std::size_t each = 3 * sizeof(f32);
                    const std::size_t keys =
                        std::min(track.times.size(), track.values.size() / (stride * each));
                    for (std::size_t k = 0; k < keys * stride; ++k) {
                        if (k % stride == 0 || handles) {
                            WriteVec3(track.values, k * each,
                                      ReadVec3(track.values, k * each) + carry[node].translation);
                        }
                    }
                }
            }
        }
    }
    constants.assign(document.clips.size(), {});
    for (u32 n = 0; n < count; ++n) {
        for (const Channel which : {Channel::Rotation, Channel::Translation}) {
            const bool rotation = which == Channel::Rotation;
            if (!(rotation ? turns(n) : steps(n)) || index.onLoop(n, which)) {
                continue;
            }
            for (u32 c = 0; c < document.clips.size(); ++c) {
                Clip& clip = document.clips[c];
                if (!TakesConstant(clip, model) || index.keys(c, n, which)) {
                    continue;
                }
                const u32 channel =
                    rotation ? RotationChannelOf(owner, n) : TranslationChannelOf(owner, n);
                const Interpolation interp =
                    index.interp(channel, rotation ? Interpolation::Slerp : Interpolation::Linear);
                PutConstant(clip, channel, interp, rotation, carry[n].rotation.normalized(),
                            carry[n].translation);
                constants[c].insert(channel);
                ++result.keysAdded;
            }
        }
    }
}

/// Every node, parents before children — the hierarchy's order, not the
/// array's: an `.mdx` numbers by `objectId`, which is per chunk.
std::vector<u32> PreOrder(const NodeTree& tree) {
    std::vector<u32> order;
    order.reserve(tree.size());
    for (const u32 root : tree.roots()) {
        for (const u32 node : tree.subtree(root)) {
            order.push_back(node);
        }
    }
    if (order.size() < tree.size()) {
        std::vector<u8> seen(tree.size(), 0);
        for (const u32 node : order) {
            seen[node] = 1;
        }
        for (u32 n = 0; n < tree.size(); ++n) {
            if (!seen[n]) {
                order.push_back(n);
            }
        }
    }
    return order;
}

} // namespace

const char* ToString(RebindCase which) {
    switch (which) {
    case RebindCase::NonUniformScale:
        return "NonUniformScale";
    case RebindCase::DontInherit:
        return "DontInherit";
    case RebindCase::Billboard:
        return "Billboard";
    case RebindCase::CollisionShape:
        return "CollisionShape";
    case RebindCase::GlobalSequence:
        return "GlobalSequence";
    case RebindCase::Count:
        break;
    }
    return "NonUniformScale";
}

RebindResult Rebind(Document& document, u32 model, std::span<const Transform> rest,
                    const RebindOptions& options) {
    RebindResult result;
    if (model >= document.models.size()) {
        result.diagnostics.error(DiagCode::Unspecified, "no such model");
        return result;
    }
    Model& owner = document.models[model];
    NodeTree& tree = owner.nodes;
    if (rest.size() != tree.size()) {
        result.diagnostics.error(DiagCode::Unspecified,
                                 "a re-bind takes one rest transform per node: " +
                                     std::to_string(rest.size()) + " for " +
                                     std::to_string(tree.size()) + " nodes");
        return result;
    }
    // D§5.6: the identity above is a pivot rig's. An explicit bind stores a
    // frame per node that this would have to derive, and a half-derived one is
    // a model that disagrees with itself.
    if (tree.rig != RigConvention::PivotRelative) {
        result.diagnostics.error(DiagCode::Unspecified,
                                 "a re-bind moves pivots, and this rig binds by explicit frames");
        return result;
    }
    if (!options.carry.empty() && options.carry.size() != tree.size()) {
        result.diagnostics.error(DiagCode::Unspecified,
                                 "taking the change carries one delta per node: " +
                                     std::to_string(options.carry.size()) + " for " +
                                     std::to_string(tree.size()) + " nodes");
        return result;
    }
    if (tree.empty()) {
        result.ok = true;
        return result;
    }

    // --- 1. what each node does ---------------------------------------------
    const std::vector<Move> move = MovesOf(tree, rest);
    for (u32 n = 0; n < tree.size(); ++n) {
        if (move[n].moves) {
            ++result.nodesMoved;
        }
    }
    if (result.nodesMoved == 0) {
        result.ok = true;
        return result;
    }

    const auto parentMove = [&](u32 node) -> const Move& {
        static const Move still;
        const u32 parent = ParentOf(tree, node);
        return parent == kInvalidNode ? still : move[parent];
    };

    // From the positions, not from `Mesh::bounds`: a mesh whose bounds nobody
    // has recomputed would make the worst vertex a share of nothing.
    f32 size = 1.0f;
    {
        Vector3f low{1e30f, 1e30f, 1e30f}, high{-1e30f, -1e30f, -1e30f};
        bool any = false;
        for (Mesh& mesh : owner.meshes) {
            for (const Vector3f& p :
                 mesh.attributes.get<Vector3f>(geom::names::kPosition, geom::Domain::Vertex)) {
                low = Vector3f{std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z)};
                high = Vector3f{std::max(high.x, p.x), std::max(high.y, p.y),
                                std::max(high.z, p.z)};
                any = true;
            }
        }
        // A model with no mesh is measured by its rig, or a snap meant as a
        // share of it would be absolute.
        for (u32 n = 0; n < tree.size() && !any; ++n) {
            for (const Vector3f& p : {RestAt(tree, n), rest[n].translation}) {
                low = Vector3f{std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z)};
                high = Vector3f{std::max(high.x, p.x), std::max(high.y, p.y),
                                std::max(high.z, p.z)};
            }
        }
        const Vector3f span = high - low;
        size = std::max(1.0f, std::max(span.x, std::max(span.y, span.z)));
    }
    const std::vector<Vector3f> offset = OffsetsOf(tree, move, size);

    // --- 2. the structural cases, counted and never worked around ----------
    //
    // A stretch is one whether the rest holds it or a key does: a turn does
    // not commute with a scale that differs by axis, so either one is moved
    // only approximately. One walk of the sub-tracks, the channels looked up.
    std::vector<u8> keyedStretch(tree.size(), 0);
    std::unordered_map<u32, u32> scaleOf;
    for (const AnimChannel& channel : owner.animChannels.channels) {
        if (channel.target.kind == TrackTarget::Kind::Node &&
            channel.target.channel == Channel::Scale &&
            channel.valueType == geom::AttrType::F32x3 && channel.target.node < tree.size()) {
            scaleOf.emplace(channel.id, channel.target.node);
        }
    }
    for (const Clip& clip : document.clips) {
        if (clip.model != model || scaleOf.empty()) {
            continue;
        }
        for (const SubTrackContainer& container : clip.containers) {
            for (const SubTrack& track : container.subTracks) {
                const auto found = scaleOf.find(track.channel);
                if (found == scaleOf.end() || keyedStretch[found->second] != 0) {
                    continue;
                }
                const std::size_t each = 3 * sizeof(f32) * ValuesPerKey(track.interp);
                for (std::size_t at = 0; at + each <= track.values.size(); at += each) {
                    const Vector3f s = ReadVec3(track.values, at);
                    if (std::fabs(s.x - s.y) > 1e-4f || std::fabs(s.y - s.z) > 1e-4f) {
                        keyedStretch[found->second] = 1;
                    }
                }
            }
        }
    }
    for (u32 n = 0; n < tree.size(); ++n) {
        if (!move[n].moves && !parentMove(n).moves) {
            continue;
        }
        const Node& node = tree.nodes[n];
        const Vector3f& s = node.local.scale;
        if (std::fabs(s.x - s.y) > 1e-4f || std::fabs(s.y - s.z) > 1e-4f || keyedStretch[n] != 0) {
            result.cases.push_back({RebindCase::NonUniformScale, n});
        }
        if (hasFlag(node.flags, NodeFlags::DontInheritTranslation) ||
            hasFlag(node.flags, NodeFlags::DontInheritRotation) ||
            hasFlag(node.flags, NodeFlags::DontInheritScale)) {
            result.cases.push_back({RebindCase::DontInherit, n});
        }
        if (hasFlag(node.flags, NodeFlags::Billboarded) ||
            hasFlag(node.flags, NodeFlags::BillboardLockX) ||
            hasFlag(node.flags, NodeFlags::BillboardLockY) ||
            hasFlag(node.flags, NodeFlags::BillboardLockZ)) {
            result.cases.push_back({RebindCase::Billboard, n});
        }
        // Its parent's move, or a turn of its own; a step of its own alone
        // moves a box exactly.
        if (node.kind == NodeKind::CollisionShape &&
            (parentMove(n).moves || !Negligible(parentMove(n).turn.conjugate() * move[n].turn))) {
            result.cases.push_back({RebindCase::CollisionShape, n});
        }
    }

    // Take the change, first: favouring the animations fits against the
    // frames the chosen kind produces, and the key pass below re-binds keys
    // the carry has already laid the pose over.
    std::vector<std::unordered_set<u32>> carried(document.clips.size());
    if (!options.carry.empty()) {
        CarryPose(document, model, options.carry, result, carried);
    }

    // --- 3. the mesh --------------------------------------------------------
    //
    // A vertex goes where the bones it is bound to take it, by their weights —
    // the one place a re-bind is not exact, because a vertex blended between
    // two bones is skinned twice: once into this rest and once out of the old
    // one (§5.3).
    //
    // §5.5, favour the animations: the rest each vertex would need so that the
    // frames it is actually SEEN in move least, instead of the one a single
    // skinning puts it at. It runs here, before section 4 rewrites a key,
    // because it fits against the animation as it stands.
    //
    // The re-bound frames are not evaluated a second time: `A'(b,t)` is
    // `B(b)^-1 * A(b,t)`, which is the identity the whole rewrite is built on,
    // so one sampling answers both sides. A gate that read them back out of the
    // document instead would be measuring §5.1 twice.
    std::vector<std::vector<Matrix44f>> sampledOld;
    std::vector<std::vector<Matrix44f>> sampledNew;
    if (options.favourAnimations) {
        std::vector<Matrix44f> inverse(tree.size(), Matrix44f::identity());
        for (u32 n = 0; n < tree.size(); ++n) {
            inverse[n] = Matrix44f::inverse(MoveMatrix(move[n]));
        }
        const u32 each = std::max(1u, options.sampleFrames);
        std::vector<Matrix44f> at;
        for (u32 c = 0; c < document.clips.size(); ++c) {
            if (document.clips[c].model != model) {
                continue;
            }
            const ClipPose pose(document, model, c);
            if (pose.empty() || pose.keyTimes().empty()) {
                continue;
            }
            const f32 span = document.clips[c].duration > 0 ? document.clips[c].duration
                                                            : pose.keyTimes().back();
            for (u32 f = 0; f < each && sampledOld.size() < options.maxSampleFrames; ++f) {
                pose.skinningAt(each > 1 ? span * static_cast<f32>(f) / static_cast<f32>(each - 1)
                                         : 0.0f,
                                at);
                if (at.size() != tree.size()) {
                    continue;
                }
                std::vector<Matrix44f> rebound(at.size());
                for (u32 n = 0; n < tree.size(); ++n) {
                    rebound[n] = inverse[n] * at[n];
                }
                sampledOld.push_back(at);
                sampledNew.push_back(std::move(rebound));
            }
        }
        if (sampledOld.empty()) {
            result.diagnostics.info(DiagCode::Unspecified,
                                    "favouring the animations had no keyed clip to fit "
                                    "against; the rest mesh was skinned once");
        }
    }

    for (Mesh& mesh : owner.meshes) {
        const std::span<Vector3f> positions =
            mesh.attributes.get<Vector3f>(geom::names::kPosition, geom::Domain::Vertex);
        if (positions.empty()) {
            continue;
        }
        const std::vector<std::vector<geom::Influence>> holds =
            HoldsOf(mesh, positions.size());

        // The fit, one 3x3 normal-equation solve a vertex. In row vectors a
        // frame reads `x(t) = v * M(t) + c(t)`, so the least-squares rest is
        // `v' * sum(M M^T) = sum((x - c) M^T)` — three sums over the samples
        // and a Cramer solve, which is why §5.5 calls it cheap.
        std::vector<Vector3f> fitted;
        std::vector<u8> solved;
        if (!sampledOld.empty()) {
            const std::size_t count = positions.size();
            std::vector<f32> normal(count * 9, 0.0f);
            std::vector<Vector3f> rhs(count, Vector3f{0, 0, 0});
            fitted.assign(count, Vector3f{0, 0, 0});
            solved.assign(count, 0);
            for (std::size_t s = 0; s < sampledOld.size(); ++s) {
                const std::vector<Matrix44f>& was = sampledOld[s];
                const std::vector<Matrix44f>& now = sampledNew[s];
                for (std::size_t v = 0; v < count; ++v) {
                    f32 weight = 0;
                    f32 m[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
                    Vector3f shift{0, 0, 0};
                    Vector3f target{0, 0, 0};
                    for (const geom::Influence& one : holds[v]) {
                        if (one.bone >= tree.size() || one.weight <= 0) {
                            continue;
                        }
                        weight += one.weight;
                        const Matrix44f& before = was[one.bone];
                        const Matrix44f& after = now[one.bone];
                        for (u32 r = 0; r < 3; ++r) {
                            for (u32 k = 0; k < 3; ++k) {
                                m[r * 3 + k] += one.weight * after.data[r][k];
                            }
                        }
                        shift = shift + Vector3f{after.data[3][0], after.data[3][1],
                                                 after.data[3][2]} *
                                            one.weight;
                        target = target + Point(before, positions[v]) * one.weight;
                    }
                    if (weight <= kTiny) {
                        continue;
                    }
                    const f32 share = 1.0f / weight;
                    for (u32 k = 0; k < 9; ++k) {
                        m[k] *= share;
                    }
                    const Vector3f want = target * share - shift * share;
                    f32* into = normal.data() + v * 9;
                    for (u32 r = 0; r < 3; ++r) {
                        for (u32 k = 0; k < 3; ++k) {
                            into[r * 3 + k] += m[r * 3 + 0] * m[k * 3 + 0] +
                                               m[r * 3 + 1] * m[k * 3 + 1] +
                                               m[r * 3 + 2] * m[k * 3 + 2];
                        }
                    }
                    rhs[v] = rhs[v] + Vector3f{want.x * m[0] + want.y * m[1] + want.z * m[2],
                                               want.x * m[3] + want.y * m[4] + want.z * m[5],
                                               want.x * m[6] + want.y * m[7] + want.z * m[8]};
                }
            }
            for (std::size_t v = 0; v < count; ++v) {
                solved[v] = Solve3(normal.data() + v * 9, rhs[v], fitted[v]) ? 1 : 0;
            }
        }

        const std::span<Vector3f> normals =
            mesh.attributes.get<Vector3f>(geom::names::kNormal, geom::Domain::Halfedge);
        const std::span<Vector4f> tangents =
            mesh.attributes.get<Vector4f>(geom::names::kTangent, geom::Domain::Halfedge);

        // A normal and a tangent live on a corner, so they take the turn of
        // whichever bones their own vertex is bound to.
        std::vector<Quaternion> vertexTurn(positions.size(), Quaternion::identity());
        for (std::size_t v = 0; v < positions.size(); ++v) {
            f32 weight = 0;
            Vector3f moved{0, 0, 0};
            const Quaternion* heaviest = nullptr;
            f32 best = 0;
            for (const geom::Influence& one : holds[v]) {
                if (one.bone >= tree.size() || one.weight <= 0) {
                    continue;
                }
                weight += one.weight;
                moved = moved + move[one.bone].point(positions[v]) * one.weight;
                if (one.weight > best) {
                    best = one.weight;
                    heaviest = &move[one.bone].turn;
                }
            }
            if (weight <= kTiny) {
                continue;
            }
            const Vector3f to = moved * (1.0f / weight);
            // What the re-bind COSTS, not how far the vertex went: a vertex
            // held by one bone lands exactly, and one blended between two is
            // skinned twice — once out of the old rest and once into the new —
            // so the spread between what its bones each wanted is the error
            // (§5.3). Zero for every rigid vertex, which is why the corpus
            // median is 0.000%.
            for (const geom::Influence& one : holds[v]) {
                if (one.bone >= tree.size() || one.weight <= 0) {
                    continue;
                }
                result.worstVertexFraction =
                    std::max(result.worstVertexFraction,
                             (move[one.bone].point(positions[v]) - to).length() / size);
            }
            // The fit where it solved, the skin where it did not: a vertex
            // whose bones never move in any clip has no animated difference to
            // minimise, and its normal matrix says so by being singular.
            Vector3f landed = to;
            if (v < solved.size() && solved[v] != 0) {
                landed = fitted[v];
                result.restDrift = std::max(result.restDrift, (landed - to).length() / size);
            }
            if ((landed - positions[v]).length() > kTiny) {
                ++result.verticesMoved;
            }
            positions[v] = landed;
            if (heaviest != nullptr) {
                vertexTurn[v] = *heaviest;
            }
        }

        if (!normals.empty() || !tangents.empty()) {
            // A normal and a tangent sit on a HALFEDGE, and a halfedge's index
            // is the topology's, not the face set's corner order — the builder
            // walks `next` from each face's first halfedge to place them. So
            // the connectivity is what says which vertex a corner belongs to,
            // and a mesh that did not carry it gets it back as it was.
            const bool had = mesh.hasConnectivity();
            mesh.ensureConnectivity();
            const geom::Topology& topology = mesh.topology();
            for (u32 h = 0; h < topology.halfedgeCount(); ++h) {
                const geom::HalfedgeId edge{h};
                if (topology.isBoundary(edge)) {
                    continue;
                }
                const std::size_t v = topology.from(edge).index();
                if (v >= vertexTurn.size() || IsIdentity(vertexTurn[v])) {
                    continue;
                }
                if (h < normals.size()) {
                    const Vector3f turned = Turn(vertexTurn[v], normals[h]);
                    const f32 length = turned.length();
                    normals[h] = length > kTiny ? turned * (1.0f / length) : turned;
                }
                if (h < tangents.size()) {
                    const Vector4f& was = tangents[h];
                    const Vector3f turned = Turn(vertexTurn[v], Vector3f{was.x, was.y, was.z});
                    const f32 length = turned.length();
                    const Vector3f unit = length > kTiny ? turned * (1.0f / length) : turned;
                    // `w` is a handedness sign, not a direction: it is carried,
                    // never rotated.
                    tangents[h] = Vector4f{unit.x, unit.y, unit.z, was.w};
                }
            }
            if (!had) {
                mesh.invalidateConnectivity();
            }
        }
        mesh.recomputeBounds();
    }

    // --- 4. the keys --------------------------------------------------------
    //
    // A constant map per node, applied at each key's own unchanged time, so no
    // claim changes hands and no window moves (§5.4).
    const std::unordered_map<u32, NodeChannel> channels = NodeChannels(owner);
    for (u32 c = 0; c < document.clips.size(); ++c) {
        Clip& clip = document.clips[c];
        if (clip.model != model) {
            continue;
        }
        for (SubTrackContainer& container : clip.containers) {
            for (SubTrack& track : container.subTracks) {
                const auto found = channels.find(track.channel);
                if (found == channels.end() || found->second.node >= tree.size()) {
                    continue;
                }
                const u32 node = found->second.node;
                const Move& self = move[node];
                const Move& above = parentMove(node);
                if (!self.moves && !above.moves) {
                    continue;
                }
                // The carry's constants are rewritten with the rest, and were
                // counted as added.
                const u32 counted = carried[c].count(track.channel) != 0 ? 0u : 1u;
                const u32 stride = ValuesPerKey(track.interp);
                if (found->second.channel == Channel::Rotation) {
                    const std::size_t each = 4 * sizeof(f32);
                    const std::size_t count =
                        std::min(track.times.size(), track.values.size() / (stride * each));
                    for (std::size_t k = 0; k < count * stride; ++k) {
                        // Row vectors: `R(a) * R(b)` is `R(b * a)`, which is
                        // what forces this order. The other way round is the
                        // single most likely bug in the whole phase, and it
                        // costs hundreds of units, not a rounding.
                        const Quaternion had = ReadQuat(track.values, k * each);
                        WriteQuat(track.values, k * each,
                                  above.turn * had * self.turn.conjugate());
                    }
                    // A tangent is a control point of the same curve, so it
                    // takes the same map; `tcb` derives its tangents linearly
                    // from the values and so needs no rewrite at all.
                    result.keysRewritten += static_cast<u32>(count) * counted;
                } else if (above.moves || Shifted(offset[node])) {
                    const std::size_t each = 3 * sizeof(f32);
                    const std::size_t count =
                        std::min(track.times.size(), track.values.size() / (stride * each));
                    // A Bezier handle is a position and takes the constant with
                    // its key; a Hermite tangent is a derivative, which it does
                    // not change.
                    const bool handles = track.interp == Interpolation::Bezier;
                    for (std::size_t k = 0; k < count * stride; ++k) {
                        // The pivot terms cancel identically, so this is the
                        // whole of it: `t * L(parent) - o_b`.
                        Vector3f value = ReadVec3(track.values, k * each);
                        if (above.moves) {
                            value = Turn(above.turn, value);
                        }
                        if (Shifted(offset[node]) && (k % stride == 0 || handles)) {
                            value = value - offset[node];
                        }
                        WriteVec3(track.values, k * each, value);
                    }
                    result.keysRewritten += static_cast<u32>(count) * counted;
                }
            }
        }
    }

    // A moved node with no rotation key in a clip still has to say where it
    // now rests, or the clip would play it at the identity it no longer holds.
    // A track the clip reads was already re-bound above, so only a clip that
    // reads it nowhere gains one (`PutConstant`), never a loop
    // (`TakesConstant`), and none at all where a loop reads it: that track
    // plays through every clip.
    const ClipKeys index(document, model);
    for (u32 n = 0; n < tree.size(); ++n) {
        const Move& self = move[n];
        const Move& above = parentMove(n);
        if (!self.moves && !above.moves) {
            continue;
        }
        const Quaternion constant = (above.turn * self.turn.conjugate()).normalized();
        if (Negligible(constant) || index.onLoop(n, Channel::Rotation)) {
            continue;
        }
        for (u32 c = 0; c < document.clips.size(); ++c) {
            Clip& clip = document.clips[c];
            if (!TakesConstant(clip, model) || index.keys(c, n, Channel::Rotation)) {
                continue;
            }
            const u32 channel = RotationChannelOf(owner, n);
            PutConstant(clip, channel, index.interp(channel, Interpolation::Slerp), true, constant,
                        Vector3f{0, 0, 0});
            ++result.keysAdded;
        }
    }

    // And a node that moved off its parent's carry with no translation key has
    // to give the offset back the same way, by the same rules.
    for (u32 n = 0; n < tree.size(); ++n) {
        if (!Shifted(offset[n]) || index.onLoop(n, Channel::Translation)) {
            continue;
        }
        for (u32 c = 0; c < document.clips.size(); ++c) {
            Clip& clip = document.clips[c];
            if (!TakesConstant(clip, model) || index.keys(c, n, Channel::Translation)) {
                continue;
            }
            const u32 channel = TranslationChannelOf(owner, n);
            PutConstant(clip, channel, index.interp(channel, Interpolation::Linear), false,
                        Quaternion::identity(), Vector3f{0, 0, 0} - offset[n]);
            ++result.keysAdded;
        }
    }

    // Every track on a clock of its own that the key pass rewrote is a row of
    // the report, one a node: it plays outside any clip's window, so a reader
    // has to know it moved.
    std::vector<u8> looped(tree.size(), 0);
    for (const Clip& clip : document.clips) {
        if (clip.model != model || !IsGlobalLoop(clip)) {
            continue;
        }
        for (const SubTrackContainer& container : clip.containers) {
            for (const SubTrack& track : container.subTracks) {
                const auto found = channels.find(track.channel);
                if (found == channels.end() || found->second.node >= tree.size()) {
                    continue;
                }
                const u32 n = found->second.node;
                const bool rewritten = found->second.channel == Channel::Rotation
                                           ? move[n].moves || parentMove(n).moves
                                           : parentMove(n).moves || Shifted(offset[n]);
                if (rewritten && looped[n] == 0) {
                    looped[n] = 1;
                    result.cases.push_back({RebindCase::GlobalSequence, n});
                }
            }
        }
    }

    // --- 5. the nodes themselves --------------------------------------------
    for (const u32 n : PreOrder(tree)) {
        Node& node = tree.nodes[n];
        const Move& self = move[n];
        const Move& above = parentMove(n);
        if (!self.moves && !above.moves) {
            continue;
        }
        node.pivot = self.to;
        const u32 parent = ParentOf(tree, n);
        const bool absolute = parent == kInvalidNode ||
                              hasFlag(node.flags, NodeFlags::DontInheritTranslation) ||
                              hasFlag(node.flags, NodeFlags::ModelSpace);
        node.local.translation = absolute ? self.to : self.to - move[parent].to;

        // A pose that is not the bind is an orientation in the model's space —
        // `BPOS` is the one that exists — so it turns with its node and lands
        // on its new pivot.
        for (std::size_t p = 0; p < node.poses.size(); ++p) {
            if (p < tree.poseSchema.size() && p == tree.authoritativePose) {
                node.poses[p] = node.local;
                continue;
            }
            Transform& pose = node.poses[p];
            pose.rotation = (self.turn * pose.rotation).normalized();
            pose.translation = self.to;
        }
        for (Matrix44f& matrix : node.poseMatrices) {
            const Matrix44f turn = ToMatrix(Transform{Vector3f{0, 0, 0}, self.turn, {1, 1, 1}});
            matrix = matrix * turn;
            matrix.data[3][0] = self.to.x;
            matrix.data[3][1] = self.to.y;
            matrix.data[3][2] = self.to.z;
        }

        // A collision shape's vertices are model-space geometry, not
        // pivot-relative: `CLID` lands them verbatim. They go with the node's
        // own move when it has one of its own; otherwise that move IS its
        // parent's carry, and reading `above` keeps every such output as it was.
        if (auto* collision = std::get_if<CollisionPayload>(&node.payload)) {
            const bool own =
                Shifted(offset[n]) || !Negligible(above.turn.conjugate() * self.turn);
            const Move& carry = own ? self : above;
            if (carry.moves) {
                collision->shape.box.minimum = carry.point(collision->shape.box.minimum);
                collision->shape.box.maximum = carry.point(collision->shape.box.maximum);
                // A turned box comes back as a new axis-aligned one, which is a
                // report row rather than a silent shrink.
                const Vector3f low{std::min(collision->shape.box.minimum.x, collision->shape.box.maximum.x),
                                   std::min(collision->shape.box.minimum.y, collision->shape.box.maximum.y),
                                   std::min(collision->shape.box.minimum.z, collision->shape.box.maximum.z)};
                const Vector3f high{std::max(collision->shape.box.minimum.x, collision->shape.box.maximum.x),
                                    std::max(collision->shape.box.minimum.y, collision->shape.box.maximum.y),
                                    std::max(collision->shape.box.minimum.z, collision->shape.box.maximum.z)};
                collision->shape.box.minimum = low;
                collision->shape.box.maximum = high;
                collision->shape.sphere.center = carry.point(collision->shape.sphere.center);
                ++result.shapesMoved;
            }
        } else if (auto* camera = std::get_if<CameraPayload>(&node.payload)) {
            camera->target = self.point(camera->target);
        }
    }

    if (!sampledOld.empty()) {
        result.diagnostics.info(DiagCode::Unspecified,
                                "favoured the animations over " +
                                    std::to_string(sampledOld.size()) +
                                    " sampled frames; the rest mesh drifted " +
                                    std::to_string(result.restDrift * 100.0f) +
                                    "% of the model from the skinned one");
    }

    result.ok = true;
    result.diagnostics.info(
        DiagCode::GeometryRescaled,
        "re-bound " + std::to_string(result.nodesMoved) + " nodes: " +
            std::to_string(result.verticesMoved) + " vertices, " +
            std::to_string(result.keysRewritten) + " keys rewritten, " +
            std::to_string(result.keysAdded) + " added, " + std::to_string(result.shapesMoved) +
            " collision shapes, worst vertex " +
            std::to_string(result.worstVertexFraction * 100.0f) + "% of the model");
    return result;
}

Extent RestExtent(const Model& model, std::span<const Transform> rest) {
    Extent out;
    ResetExtent(out);
    if (rest.size() != model.nodes.size() || model.nodes.empty() ||
        model.nodes.rig != RigConvention::PivotRelative) {
        return out;
    }
    const std::vector<Move> move = MovesOf(model.nodes, rest);
    bool any = false;
    for (const Mesh& mesh : model.meshes) {
        const std::span<const Vector3f> positions =
            mesh.attributes.get<Vector3f>(geom::names::kPosition, geom::Domain::Vertex);
        if (positions.empty()) {
            continue;
        }
        const std::vector<std::vector<geom::Influence>> holds = HoldsOf(mesh, positions.size());
        for (std::size_t v = 0; v < positions.size(); ++v) {
            Vector3f landed{0, 0, 0};
            if (!Landing(holds[v], move, positions[v], landed)) {
                continue;
            }
            GrowExtent(out, landed);
            any = true;
        }
    }
    if (!any) {
        ResetExtent(out);
        return out;
    }
    FinishExtent(out);
    return out;
}

} // namespace wem
} // namespace models
} // namespace whiteout
