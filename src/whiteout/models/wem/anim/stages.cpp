// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "whiteout/models/wem/anim/stages.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>

#include "whiteout/models/wem/anim/crossing.h"
#include "whiteout/models/wem/anim/track_read.h"
#include "whiteout/models/wem/physics/switch_rule.h"
#include "whiteout/models/wem/rigging/ik.h"

namespace whiteout {
namespace models {
namespace wem {

namespace {

/// Physics steps at 60 a second whatever the frame rate, and keeps a snapshot
/// every half second of them.
constexpr f32 kStep = 1.0f / 60.0f;
constexpr i64 kSnapshotEvery = 30;
/// The bake keys a stage 60 times a second, and blends a physics loop's seam
/// over its last tenth of a second.
constexpr f32 kBakeRate = 60.0f;
constexpr f32 kSeamSeconds = 0.1f;

Vector3f Origin(const Matrix44f& m) {
    return Vector3f{m.data[3][0], m.data[3][1], m.data[3][2]};
}

Vector3f Row(const Matrix44f& m, int r) {
    return Vector3f{m.data[r][0], m.data[r][1], m.data[r][2]};
}

/// @p v, a direction in a frame's own space, in model space.
Vector3f Direction(const Vector3f& v, const Matrix44f& m) {
    return Row(m, 0) * v.x + Row(m, 1) * v.y + Row(m, 2) * v.z;
}

Vector3f Normalized(const Vector3f& v) {
    const f32 length = v.length();
    return length > 1e-8f ? v * (1.0f / length) : Vector3f{0, 0, 0};
}

/// A world turn @p q about the point @p about, as a row-vector matrix.
Matrix44f TurnAbout(const Quaternion& q, const Vector3f& about) {
    return Matrix44f::translation(-about) * Matrix44f::rotation(q).transpose() *
           Matrix44f::translation(about);
}

/// The shortest turn taking unit @p from onto unit @p to.
Quaternion Arc(const Vector3f& from, const Vector3f& to) {
    const f32 dot = std::clamp(from.dot(to), -1.0f, 1.0f);
    Vector3f axis = cross(from, to);
    if (axis.length() < 1e-6f) {
        if (dot > 0.0f) {
            return Quaternion{0, 0, 0, 1};
        }
        // Opposite: any axis square to them.
        axis = std::fabs(from.x) < 0.9f ? cross(from, Vector3f{1, 0, 0}) : cross(from, Vector3f{0, 1, 0});
    }
    return Quaternion::from_axis_angle(Normalized(axis), std::acos(dot));
}

Transform Blend(const Transform& a, const Transform& b, f32 w) {
    if (w >= 1.0f) {
        return b;
    }
    Transform out;
    out.translation = a.translation + (b.translation - a.translation) * w;
    out.scale = a.scale + (b.scale - a.scale) * w;
    Quaternion to = b.rotation;
    if (a.rotation.dot(to) < 0.0f) {
        to = Quaternion{-to.x, -to.y, -to.z, -to.w};
    }
    out.rotation = Quaternion{a.rotation.x + (to.x - a.rotation.x) * w, a.rotation.y + (to.y - a.rotation.y) * w,
                              a.rotation.z + (to.z - a.rotation.z) * w, a.rotation.w + (to.w - a.rotation.w) * w}
                       .normalized();
    return out;
}

/// @p node's local transform that stands it at model-space @p frame over its
/// parent's current frame, by the rig's composition: `T(-pivot)·S·R·T(pivot+t)`
/// on a pivot rig (as composed without don't-inherit flags), `S·R·T` on an
/// explicit-bind one.
Transform LocalOf(const NodeTree& tree, const Pose& pose, u32 node, const Matrix44f& frame) {
    const Node& self = tree.nodes[node];
    const bool rooted = self.parent < tree.size() && self.parent != node;
    const Matrix44f parent = rooted ? pose.frame[self.parent] : Matrix44f::identity();
    Transform out = FromMatrix(frame * Matrix44f::inverse(parent));
    if (tree.rig == RigConvention::PivotRelative) {
        const Vector3f parentPivot = rooted ? tree.nodes[self.parent].pivot : Vector3f{0, 0, 0};
        out.translation = out.translation - self.pivot + parentPivot;
    }
    return out;
}

/// Stands @p node at @p frame, weighted by @p weight against where it is, and
/// recomposes everything under it.
void Place(const Animator& animator, const NodeTree& tree, Pose& pose, u32 node, const Matrix44f& frame,
           f32 weight) {
    pose.local[node] = Blend(pose.local[node], LocalOf(tree, pose, node, frame), weight);
    animator.compose(pose);
}

bool SameFrame(const Matrix44f& a, const Matrix44f& b) {
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 3; ++c) {
            if (std::fabs(a.data[r][c] - b.data[r][c]) > 1e-5f * std::max(1.0f, std::fabs(b.data[r][c]))) {
                return false;
            }
        }
    }
    return true;
}

/// Stands a held node where it is held, its scale the pose's own, unless it
/// stands there already.
void Stand(const Animator& animator, const NodeTree& tree, Pose& pose, const StageHold& hold) {
    if (SameFrame(pose.frame[hold.node], hold.frame)) {
        return;
    }
    Transform local = LocalOf(tree, pose, hold.node, hold.frame);
    local.scale = pose.local[hold.node].scale;
    pose.local[hold.node] = local;
    animator.compose(pose);
}

/// How many nodes stand above @p node.
u32 DepthOf(const NodeTree& tree, u32 node) {
    u32 depth = 0;
    for (u32 up = tree.nodes[node].parent; up < tree.size() && depth <= tree.size(); up = tree.nodes[up].parent) {
        ++depth;
    }
    return depth;
}

/// @p held's nodes of @p pose, each once, parents first: standing one never
/// moves one stood before it.
std::vector<StageHold> InOrder(const NodeTree& tree, const Pose& pose, std::span<const StageHold> held) {
    std::vector<StageHold> out;
    for (const StageHold& hold : held) {
        if (hold.node < tree.size() && hold.node < pose.frame.size() &&
            std::none_of(out.begin(), out.end(), [&](const StageHold& other) { return other.node == hold.node; })) {
            out.push_back(hold);
        }
    }
    std::stable_sort(out.begin(), out.end(), [&](const StageHold& a, const StageHold& b) {
        return DepthOf(tree, a.node) < DepthOf(tree, b.node);
    });
    return out;
}

/// @p frame's rotation alone, its rows unit length.
Matrix44f Orientation(const Matrix44f& frame) {
    Matrix44f out = Matrix44f::identity();
    for (int r = 0; r < 3; ++r) {
        const Vector3f row = Normalized(Row(frame, r));
        out.data[r][0] = row.x;
        out.data[r][1] = row.y;
        out.data[r][2] = row.z;
    }
    return out;
}

/// @p frame's rows given @p rotation's directions at their own lengths, and
/// its origin kept.
Matrix44f WithOrientation(const Matrix44f& frame, const Matrix44f& rotation) {
    Matrix44f out = frame;
    for (int r = 0; r < 3; ++r) {
        const f32 length = Row(frame, r).length();
        for (int c = 0; c < 3; ++c) {
            out.data[r][c] = rotation.data[r][c] * length;
        }
    }
    return out;
}

bool ValidNode(const NodeTree& tree, u32 node) {
    return node < tree.size();
}

/// @p rotation as a matrix.
Matrix44f Turn(const Quaternion& rotation) {
    return ToMatrix(Transform{Vector3f{0, 0, 0}, rotation, Vector3f{1, 1, 1}});
}

/// @p node's parent's frame in @p pose, the identity for a root.
Matrix44f ParentFrame(const NodeTree& tree, const Pose& pose, u32 node) {
    const u32 parent = tree.nodes[node].parent;
    return parent < tree.size() && parent != node ? pose.frame[parent] : Matrix44f::identity();
}

/// The value @p pose holds for @p model's channel on @p node's @p channel with
/// sub @p sub, or null where the model has no such channel.
const std::vector<u8>* ChannelValue(const Model& model, const Pose& pose, u32 node, Channel channel, u32 sub) {
    const std::vector<AnimChannel>& channels = model.animChannels.channels;
    for (std::size_t i = 0; i < channels.size(); ++i) {
        const TrackTarget& target = channels[i].target;
        if (target.kind == TrackTarget::Kind::Node && target.node == node && target.channel == channel &&
            target.sub == sub) {
            return i < pose.channelValues.size() ? &pose.channelValues[i] : nullptr;
        }
    }
    return nullptr;
}

/// A constraint's sources blended by weight: where they stand, and how they
/// turn, each the short way round from the first.
struct SourceBlend {
    Vector3f position{0, 0, 0};
    Quaternion rotation{0, 0, 0, 1};
    bool valid = false;
};

SourceBlend BlendSources(const Model& model, const PoseStage& stage, const Pose& pose) {
    SourceBlend out;
    f32 total = 0.0f;
    Vector3f position{0, 0, 0};
    Quaternion sum{0, 0, 0, 0};
    Quaternion first{0, 0, 0, 1};
    bool seen = false;
    for (const StageSource& source : stage.sources) {
        if (!ValidNode(model.nodes, source.node) || source.node >= pose.frame.size()) {
            continue;
        }
        const f32 w = std::max(SourceWeight(model, stage, source, pose), 0.0f);
        if (w <= 0.0f) {
            continue;
        }
        const Matrix44f& frame = pose.frame[source.node];
        position = position + Origin(frame) * w;
        Quaternion r = FromMatrix(Orientation(frame)).rotation;
        if (!seen) {
            first = r;
            seen = true;
        }
        if (first.dot(r) < 0.0f) {
            r = Quaternion{-r.x, -r.y, -r.z, -r.w};
        }
        sum = Quaternion{sum.x + r.x * w, sum.y + r.y * w, sum.z + r.z * w, sum.w + r.w * w};
        total += w;
    }
    if (total <= 0.0f) {
        return out;
    }
    out.position = position * (1.0f / total);
    out.rotation = sum.normalized();
    out.valid = true;
    return out;
}

/// A share this small carries nothing.
constexpr f32 kShareEpsilon = 1e-6f;

/// The frames that can carry a Link's @p node in @p pose: its own parent's,
/// then each source's in order — the identity for the world, the parent's for
/// a node gone.
std::vector<Matrix44f> Carriers(const NodeTree& tree, const PoseStage& stage, u32 node, const Pose& pose) {
    std::vector<Matrix44f> out;
    out.push_back(ParentFrame(tree, pose, node));
    for (const StageSource& source : stage.sources) {
        if (source.node == kInvalidNode) {
            out.push_back(Matrix44f::identity());
        } else {
            out.push_back(ValidNode(tree, source.node) ? pose.frame[source.node] : out.front());
        }
    }
    return out;
}

/// Each carrier's share in @p pose, as `Carriers` lists them: the sources'
/// (`LinkShares`), and the parent's what they leave.
std::vector<f32> CarrierShares(const Model& model, const PoseStage& stage, const Pose& pose) {
    std::vector<f32> out(1, 0.0f);
    f32 left = 1.0f;
    for (const f32 share : LinkShares(model, stage, pose)) {
        out.push_back(share);
        left -= share;
    }
    out.front() = std::max(left, 0.0f);
    return out;
}

bool SameShares(const std::vector<f32>& a, const std::vector<f32>& b) {
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::fabs(a[i] - b[i]) > kShareEpsilon) {
            return false;
        }
    }
    return true;
}

/// Each carrier under the offset a Link's switches left it.
std::vector<Matrix44f> Carried(const std::vector<Matrix44f>& offsets, const std::vector<Matrix44f>& carriers) {
    std::vector<Matrix44f> out;
    for (std::size_t i = 0; i < carriers.size(); ++i) {
        out.push_back(offsets[i] * carriers[i]);
    }
    return out;
}

/// Where a Link's switches are in a mix: the first of its plays that keys any
/// of the Link's sources' Enabled up to where it plays, where that is, and
/// those keys' times in order. Not keyed where nothing keys one. On a host's
/// timeline, the keys it has of them up to the mix's world clock.
struct LinkSwitches {
    bool keyed = false;
    f32 now = 0.0f;
    std::vector<f32> times;
};

LinkSwitches SwitchesOf(const Document& document, u32 modelIndex, const PoseStage& stage, const Mix& mix,
                        const StageTimeline* timeline) {
    LinkSwitches out;
    const Model& model = document.models[modelIndex];
    std::vector<u32> channels;
    for (const AnimChannel& entry : model.animChannels.channels) {
        if (entry.target.kind == TrackTarget::Kind::Node && entry.target.channel == Channel::StageSourceEnabled &&
            entry.target.node == stage.driven.front() && StageOfSub(entry.target.sub) == stage.id) {
            channels.push_back(entry.id);
        }
    }
    if (channels.empty()) {
        return out;
    }
    if (timeline != nullptr) {
        out.now = mix.worldSeconds;
        if (timeline->keys) {
            out.times = timeline->keys(channels, out.now);
        }
        out.keyed = !out.times.empty();
        return out;
    }
    for (const Play& play : mix.plays) {
        if (play.clip >= document.clips.size() || document.clips[play.clip].model != modelIndex) {
            continue;
        }
        const Clip& clip = document.clips[play.clip];
        // The end itself is the end, as the tracks read it; only past it wraps.
        f32 now = play.seconds;
        if (play.loop && clip.duration > 0.0f && now > clip.duration) {
            now = std::fmod(now, clip.duration);
        }
        // By the millisecond, as the tracks are read: a key switches at the
        // sample that first reads its value, and none sees one without the other.
        const u32 nowMs = Milliseconds(now);
        std::set<f32> times;
        for (const u32 id : channels) {
            if (const SubTrack* track = FindSubTrack(clip, id)) {
                for (const f32 t : track->times) {
                    if (Milliseconds(t) <= nowMs) {
                        times.insert(t);
                    }
                }
            }
        }
        if (!times.empty()) {
            out.keyed = true;
            out.now = now;
            out.times.assign(times.begin(), times.end());
            break;
        }
    }
    return out;
}

/// A mix's moment, to tell it from the others one pass reads: each play's
/// time and the world's, to the microsecond.
std::vector<i64> MomentOf(const Mix& mix) {
    std::vector<i64> out;
    out.reserve(mix.plays.size() + 1);
    for (const Play& play : mix.plays) {
        out.push_back(std::llround(static_cast<f64>(play.seconds) * 1e6));
    }
    out.push_back(std::llround(static_cast<f64>(mix.worldSeconds) * 1e6));
    return out;
}

/// @p frames blended by @p shares: where they stand, how they turn (each the
/// short way round from the heaviest) and their scale. A lone one is itself.
Matrix44f BlendFrames(const std::vector<Matrix44f>& frames, const std::vector<f32>& shares) {
    std::size_t heaviest = 0;
    std::size_t count = 0;
    f32 total = 0.0f;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        if (shares[i] > kShareEpsilon) {
            total += shares[i];
            ++count;
        }
        if (shares[i] > shares[heaviest]) {
            heaviest = i;
        }
    }
    if (count <= 1) {
        return frames[heaviest];
    }
    const Quaternion first = FromMatrix(frames[heaviest]).rotation;
    Transform out;
    out.translation = Vector3f{0, 0, 0};
    out.scale = Vector3f{0, 0, 0};
    Quaternion sum{0, 0, 0, 0};
    for (std::size_t i = 0; i < frames.size(); ++i) {
        if (shares[i] <= kShareEpsilon) {
            continue;
        }
        const f32 w = shares[i] / total;
        const Transform part = FromMatrix(frames[i]);
        out.translation = out.translation + part.translation * w;
        out.scale = out.scale + part.scale * w;
        Quaternion r = part.rotation;
        if (first.dot(r) < 0.0f) {
            r = Quaternion{-r.x, -r.y, -r.z, -r.w};
        }
        sum = Quaternion{sum.x + r.x * w, sum.y + r.y * w, sum.z + r.z * w, sum.w + r.w * w};
    }
    out.rotation = sum.normalized();
    return ToMatrix(out);
}

/// A host's physics stage stepped on @p pose, and the pose composed again. A
/// force field rides its bone's animation, never the bodies the physics moved
/// under it: a later stage reads it, and the view draws it, where the
/// animation has it.
void StepHost(const StageHooks& hooks, const Document& document, u32 model, const Animator& animator,
              const PoseStage& stage, Pose& pose, f32 dt) {
    const NodeTree& tree = document.models[model].nodes;
    std::vector<std::pair<u32, Matrix44f>> fields;
    for (u32 n = 0; n < tree.size() && n < pose.frame.size(); ++n) {
        if (tree.nodes[n].kind == NodeKind::ForceField) {
            fields.emplace_back(n, pose.frame[n]);
        }
    }
    hooks.step(document, model, stage, pose, dt);
    animator.compose(pose);
    for (const auto& [node, frame] : fields) {
        pose.frame[node] = frame;
    }
}

} // namespace

const char* ToString(StageKind kind) {
    switch (kind) {
    case StageKind::Position:
        return "position";
    case StageKind::Orientation:
        return "orientation";
    case StageKind::LookAt:
        return "lookAt";
    case StageKind::Link:
        return "link";
    case StageKind::LimbIk:
        return "limbIk";
    case StageKind::ChainIk:
        return "chainIk";
    case StageKind::Spring:
        return "spring";
    case StageKind::Ragdoll:
        return "ragdoll";
    case StageKind::Cloth:
        return "cloth";
    case StageKind::Sc2LegIk:
        return "sc2LegIk";
    case StageKind::Sc2Turret:
        return "sc2Turret";
    case StageKind::Count:
        break;
    }
    return "invalid";
}

f32 DefaultGravity(Game game) {
    switch (game) {
    case Game::Warcraft:
        return 540.0f;
    case Game::Diablo:
        return 34.0f;
    case Game::StarCraft:
        return PhysicsHostFor(game).gravity.length();
    default:
        return 9.8f;
    }
}

f32 UnitsPerMetre(Game game) {
    switch (game) {
    case Game::Warcraft:
        return 540.0f / 9.8f;
    case Game::Diablo:
        return 34.0f / 9.8f;
    default:
        return 1.0f;
    }
}

Transform StageLocalFor(const NodeTree& tree, const Pose& pose, u32 node, const Matrix44f& frame) {
    return node < tree.size() ? LocalOf(tree, pose, node, frame) : Transform{};
}

const AnimChannel* StageWeightChannel(const Model& model, const PoseStage& stage) {
    if (stage.driven.empty()) {
        return nullptr;
    }
    for (const AnimChannel& channel : model.animChannels.channels) {
        if (IsStageWeight(channel.target) && channel.target.node == stage.driven.front() &&
            channel.target.sub == stage.id) {
            return &channel;
        }
    }
    return nullptr;
}

f32 StageWeight(const Model& model, const PoseStage& stage, const Pose& pose) {
    const AnimChannel* channel = StageWeightChannel(model, stage);
    if (channel == nullptr) {
        return 1.0f;
    }
    const u32 index = model.animChannels.indexOf(channel->id);
    if (index >= pose.channelValues.size() || pose.channelValues[index].size() != sizeof(f32)) {
        return 1.0f;
    }
    f32 weight = 1.0f;
    std::memcpy(&weight, pose.channelValues[index].data(), sizeof(f32));
    return std::clamp(weight, 0.0f, 1.0f);
}

f32 SourceWeight(const Model& model, const PoseStage& stage, const StageSource& source, const Pose& pose) {
    if (stage.driven.empty()) {
        return source.weight;
    }
    const Channel channel = stage.kind == StageKind::Link ? Channel::StageSourceEnabled : Channel::StageSourceWeight;
    const std::vector<u8>* value = ChannelValue(model, pose, stage.driven.front(), channel, StageSub(stage.id, source.id));
    if (value == nullptr || value->size() != sizeof(f32)) {
        return source.weight;
    }
    f32 weight = source.weight;
    std::memcpy(&weight, value->data(), sizeof(f32));
    return weight;
}

std::vector<f32> LinkShares(const Model& model, const PoseStage& stage, const Pose& pose) {
    std::vector<f32> out;
    out.reserve(stage.sources.size());
    f32 left = 1.0f;
    for (const StageSource& source : stage.sources) {
        const f32 value = SourceWeight(model, stage, source, pose);
        const f32 share = value > 0.0f ? std::min(value, left) : 0.0f;
        out.push_back(share);
        left -= share;
    }
    return out;
}

void CaptureStageOffset(const Document& document, u32 model, PoseStage& stage) {
    stage.offset = Transform{};
    if (model >= document.models.size() || stage.driven.empty() ||
        (stage.kind != StageKind::Position && stage.kind != StageKind::Orientation)) {
        return;
    }
    const Model& owner = document.models[model];
    if (!ValidNode(owner.nodes, stage.driven.front())) {
        return;
    }
    // At rest: no play, and no global loop either.
    const Animator animator(document, model);
    Mix rest;
    rest.globals = false;
    Pose pose;
    animator.evaluate(rest, pose);
    const SourceBlend blend = BlendSources(owner, stage, pose);
    if (!blend.valid) {
        return;
    }
    const Matrix44f& driven = pose.frame[stage.driven.front()];
    if (stage.kind == StageKind::Position) {
        stage.offset.translation = Origin(driven) - blend.position;
    } else {
        stage.offset.rotation = FromMatrix(Orientation(driven) * Matrix44f::inverse(Turn(blend.rotation))).rotation;
    }
}

StageRunner::StageRunner(const Document& document, u32 model) {
    if (model >= document.models.size()) {
        return;
    }
    document_ = &document;
    model_ = &document.models[model];
    modelIndex_ = model;
}

bool StageRunner::empty() const {
    if (model_ == nullptr) {
        return true;
    }
    return std::none_of(model_->poseStages.begin(), model_->poseStages.end(),
                        [](const PoseStage& stage) { return stage.enabled; });
}

/// A Link's carry is read off the pose the stages before it leave, at rest and
/// at each of its switches. Worked out afresh, that runs every stage before it
/// again, each of which asks the same of the ones before it: twice the work
/// for every Link more. So a pass keeps what it reads: each moment's pose is
/// stepped through the stages once, and each Link's carry there worked out once.
struct StageRunner::LinkPass {
    /// A mix the pass reads, and its pose with the stages before `done` on it.
    struct Moment {
        Mix mix;
        Pose pose;
        std::size_t done = 0;
        std::vector<StageHold> held; ///< What was held then, parents first.
    };
    const StageHolds* held = nullptr;
    /// A host's timeline, where the moments are its and not a mix's plays,
    /// and what its switches have left: the host's, or this pass's own.
    const StageTimeline* timeline = nullptr;
    StageCarries* kept = nullptr;
    StageCarries own;
    Moment rest;
    bool rested = false;
    std::map<std::vector<i64>, Moment> moments;   ///< By `MomentOf`.
    std::map<std::size_t, LinkReading> stills;    ///< A Link at rest, by its place in the stages.
    std::map<std::pair<std::size_t, std::vector<i64>>, LinkCarry> carries;
};

void StageRunner::constrainOne(const Animator& animator, const Mix& mix, Pose& pose, const PoseStage& stage,
                               LinkPass& pass) const {
    const NodeTree& tree = model_->nodes;
    const f32 weight = StageWeight(*model_, stage, pose);
    if (weight <= 0.0f || stage.driven.empty() || !ValidNode(tree, stage.driven.front())) {
        return;
    }
    const u32 node = stage.driven.front();
    const bool targeted = !stage.targets.empty() && ValidNode(tree, stage.targets.front());
    switch (stage.kind) {
    case StageKind::Position: {
        const SourceBlend blend = BlendSources(*model_, stage, pose);
        if (!blend.valid) {
            return;
        }
        Matrix44f moved = pose.frame[node];
        const Vector3f to = blend.position + (stage.keepOffset ? stage.offset.translation : Vector3f{0, 0, 0});
        moved.data[3][0] = to.x;
        moved.data[3][1] = to.y;
        moved.data[3][2] = to.z;
        Place(animator, tree, pose, node, moved, weight);
        return;
    }
    case StageKind::Orientation: {
        const SourceBlend blend = BlendSources(*model_, stage, pose);
        if (!blend.valid) {
            return;
        }
        const Matrix44f turned =
            stage.keepOffset ? Turn(stage.offset.rotation) * Turn(blend.rotation) : Turn(blend.rotation);
        Place(animator, tree, pose, node, WithOrientation(pose.frame[node], turned), weight);
        return;
    }
    case StageKind::LookAt: {
        const SourceBlend blend = BlendSources(*model_, stage, pose);
        if (!blend.valid) {
            return;
        }
        const Matrix44f& frame = pose.frame[node];
        const Vector3f at = Origin(frame);
        const Vector3f forward = Normalized(blend.position - at);
        const Vector3f pointing = Normalized(Direction(stage.axis, frame));
        if (forward.length() < 0.5f || pointing.length() < 0.5f) {
            return;
        }
        Matrix44f aimed = frame * TurnAbout(Arc(pointing, forward), at);
        // Then about the aim itself, so the up axis leans toward the up node,
        // or world up.
        const Vector3f hint = ValidNode(tree, stage.upNode) ? Normalized(Origin(pose.frame[stage.upNode]) - at)
                                                            : Vector3f{0, 0, 1};
        const Vector3f upNow = Normalized(Direction(stage.up, aimed));
        const Vector3f a = Normalized(upNow - forward * upNow.dot(forward));
        const Vector3f b = Normalized(hint - forward * hint.dot(forward));
        if (a.length() > 0.5f && b.length() > 0.5f) {
            const f32 angle = std::atan2(cross(a, b).dot(forward), a.dot(b));
            aimed = aimed * TurnAbout(Quaternion::from_axis_angle(forward, angle), at);
        }
        Place(animator, tree, pose, node, aimed, weight);
        return;
    }
    case StageKind::Link: {
        // Its own parent's whole, and no switch behind it: it stands where
        // the animation has it.
        const std::vector<f32> shares = CarrierShares(*model_, stage, pose);
        if (shares.front() >= 1.0f - kShareEpsilon &&
            !SwitchesOf(*document_, modelIndex_, stage, mix, pass.timeline).keyed) {
            return;
        }
        // The node's own animation under whatever carries it: its place under
        // its parent, then its carriers, each under the offset the last switch
        // left it, blended by their shares.
        const Matrix44f local = pose.frame[node] * Matrix44f::inverse(ParentFrame(tree, pose, node));
        const LinkCarry carry = linkCarry(animator, mix, stage, pass);
        const Matrix44f carrier = BlendFrames(Carried(carry.offsets, Carriers(tree, stage, node, pose)), shares);
        Place(animator, tree, pose, node, local * carrier, weight);
        return;
    }
    case StageKind::LimbIk: {
        if (stage.driven.size() < 3 || !targeted || !ValidNode(tree, stage.driven[1]) ||
            !ValidNode(tree, stage.driven[2])) {
            return;
        }
        const u32 upper = stage.driven[0], lower = stage.driven[1], end = stage.driven[2];
        LimbPress press;
        press.upper = Origin(pose.frame[upper]);
        press.lower = Origin(pose.frame[lower]);
        press.end = Origin(pose.frame[end]);
        press.endWorld = FromMatrix(Orientation(pose.frame[end])).rotation;
        LimbSetup setup;
        setup.hinge = Normalized(cross(press.lower - press.upper, press.end - press.lower));
        if (setup.hinge.length() < 0.5f && stage.targets.size() > 1 && ValidNode(tree, stage.targets[1])) {
            // Straight: the pole names the plane it bends in.
            const Vector3f pole = Origin(pose.frame[stage.targets[1]]);
            setup.hinge = Normalized(cross(pole - press.upper, press.end - press.upper));
        }
        if (setup.hinge.length() < 0.5f) {
            return;
        }
        IkGoal goal;
        goal.position = Origin(pose.frame[stage.targets.front()]);
        goal.orientation = press.endWorld;
        goal.holdOrientation = true;
        const LimbSolve solve = SolveLimb(press, setup, goal);
        if (!solve.ok) {
            return;
        }
        // Each turn about where the turns before it left the joint.
        Place(animator, tree, pose, upper, pose.frame[upper] * TurnAbout(solve.upperDelta, Origin(pose.frame[upper])),
              weight);
        Place(animator, tree, pose, lower, pose.frame[lower] * TurnAbout(solve.lowerDelta, Origin(pose.frame[lower])),
              weight);
        Place(animator, tree, pose, end, pose.frame[end] * TurnAbout(solve.endDelta, Origin(pose.frame[end])), weight);
        return;
    }
    case StageKind::ChainIk: {
        if (stage.driven.size() < 2 || !targeted) {
            return;
        }
        std::vector<Vector3f> points;
        for (const u32 joint : stage.driven) {
            if (!ValidNode(tree, joint)) {
                return;
            }
            points.push_back(Origin(pose.frame[joint]));
        }
        IkGoal goal;
        goal.position = Origin(pose.frame[stage.targets.front()]);
        goal.orientation = FromMatrix(Orientation(pose.frame[stage.driven.back()])).rotation;
        goal.holdOrientation = true;
        const ChainSolve solve = SolveChain(points, goal.orientation, goal);
        if (!solve.ok) {
            return;
        }
        for (std::size_t j = 0; j < solve.deltas.size() && j < stage.driven.size(); ++j) {
            const u32 joint = stage.driven[j];
            Place(animator, tree, pose, joint, pose.frame[joint] * TurnAbout(solve.deltas[j], Origin(pose.frame[joint])),
                  weight);
        }
        const u32 tip = stage.driven.back();
        Place(animator, tree, pose, tip, pose.frame[tip] * TurnAbout(solve.endDelta, Origin(pose.frame[tip])), weight);
        return;
    }
    default:
        return;
    }
}

StageRunner::LinkReading StageRunner::linkReading(const Animator& animator, const Mix* mix, const PoseStage& stage,
                                                  LinkPass& pass, f32 ago) const {
    const NodeTree& tree = model_->nodes;
    const std::vector<PoseStage>& stages = model_->poseStages;
    const std::size_t index = static_cast<std::size_t>(&stage - stages.data());
    const auto read = [&](const Pose& pose) {
        return LinkReading{Carriers(tree, stage, stage.driven.front(), pose), CarrierShares(*model_, stage, pose)};
    };
    // What the pass holds stands where it stood at the moment read.
    const auto begin = [&](LinkPass::Moment& moment, const Mix& at, bool resting) {
        moment = LinkPass::Moment{};
        moment.mix = at;
        if (!resting && pass.timeline != nullptr) {
            pass.timeline->sample(at.worldSeconds, moment.pose);
            animator.compose(moment.pose);
        } else {
            animator.evaluate(at, moment.pose);
        }
        if (resting || pass.held == nullptr) {
            return;
        }
        constexpr f32 kNow = 1e-6f;
        std::span<const StageHold> then = pass.held->now;
        if (ago > kNow) {
            then = pass.held->earlier ? pass.held->earlier(ago) : std::span<const StageHold>{};
        }
        moment.held = InOrder(tree, moment.pose, then);
        for (const StageHold& hold : moment.held) {
            Stand(animator, tree, moment.pose, hold);
        }
    };
    // The stages before this one onto @p moment's pose. A stage is on it by
    // the time `done` passes it, so a Link reading itself at the moment it is
    // stepped through finds the pose as the stages before it left it.
    const auto step = [&](LinkPass::Moment& moment, bool resting) {
        for (; moment.done < index; ++moment.done) {
            const PoseStage& earlier = stages[moment.done];
            // At rest a Link stands its node where its parent puts it: nothing to run.
            if (earlier.enabled && !IsPhysicsStage(earlier.kind) && !(resting && earlier.kind == StageKind::Link)) {
                constrainHeld(animator, moment.mix, moment.pose, earlier, pass, moment.held);
            }
        }
    };
    if (mix == nullptr) {
        if (const auto found = pass.stills.find(index); found != pass.stills.end()) {
            return found->second;
        }
        // No play, and no global loop either.
        if (!pass.rested || pass.rest.done > index) {
            Mix rest;
            rest.globals = false;
            begin(pass.rest, rest, true);
            pass.rested = true;
        }
        step(pass.rest, true);
        return pass.stills.emplace(index, read(pass.rest.pose)).first->second;
    }
    const auto [found, fresh] = pass.moments.try_emplace(MomentOf(*mix));
    LinkPass::Moment& moment = found->second;
    if (fresh) {
        begin(moment, *mix, false);
    }
    if (moment.done > index) {
        // Stepped past it for a later stage: once more from the start, on the
        // carries the pass holds of the Links between.
        LinkPass::Moment again;
        begin(again, *mix, false);
        step(again, false);
        return read(again.pose);
    }
    step(moment, false);
    return read(moment.pose);
}

StageRunner::LinkCarry StageRunner::linkCarry(const Animator& animator, const Mix& mix, const PoseStage& stage,
                                              LinkPass& pass) const {
    const std::size_t index = static_cast<std::size_t>(&stage - model_->poseStages.data());
    std::pair<std::size_t, std::vector<i64>> key{index, MomentOf(mix)};
    if (const auto found = pass.carries.find(key); found != pass.carries.end()) {
        return found->second;
    }
    // At rest every carrier is offset so that it stands the node where its
    // parent puts it.
    const LinkReading still = linkReading(animator, nullptr, stage, pass);
    LinkCarry carry;
    for (const Matrix44f& carrier : still.carriers) {
        carry.offsets.push_back(still.carriers.front() * Matrix44f::inverse(carrier));
    }
    // The switches are where the sources' Enabled keys are, in the newest play
    // that keys any, up to where it plays. Where one changes the shares, every
    // carrier is rebased onto the blend the old shares made, so the node stays
    // put: `O_i' = K_old·C_i⁻¹` at that moment.
    const LinkSwitches switches = SwitchesOf(*document_, modelIndex_, stage, mix, pass.timeline);
    const auto shifted = [&](f32 t) {
        Mix moved = mix;
        const f32 shift = t - switches.now;
        for (Play& other : moved.plays) {
            other.seconds = std::max(other.seconds + shift, 0.0f);
        }
        moved.worldSeconds = std::max(moved.worldSeconds + shift, 0.0f);
        return moved;
    };
    constexpr f32 kMillisecond = 0.001f;
    std::vector<f32> held = still.shares;
    // On a timeline a switch is replayed once: from the last one kept, on.
    const auto keptAt = [&](f32 t) {
        return std::pair<u32, i64>{static_cast<u32>(index), std::llround(static_cast<f64>(t) * 1e6)};
    };
    std::size_t first = 0;
    for (std::size_t k = switches.times.size(); pass.kept != nullptr && k-- > 0;) {
        const auto found = pass.kept->offsets.find(keptAt(switches.times[k]));
        if (found != pass.kept->offsets.end() && found->second.size() == carry.offsets.size()) {
            carry.offsets = found->second;
            first = k + 1;
            break;
        }
    }
    for (std::size_t k = first; k < switches.times.size(); ++k) {
        const f32 t = switches.times[k];
        // The shares just before, as they played a millisecond earlier: a
        // stepped key's the last key's, an interpolated one's nearly its
        // own. At the clip's start, rest's.
        if (t >= kMillisecond) {
            Pose before;
            const Mix earlier = shifted(t - kMillisecond);
            if (pass.timeline != nullptr) {
                pass.timeline->sample(earlier.worldSeconds, before);
            } else {
                animator.sample(earlier, before, true);
            }
            held = CarrierShares(*model_, stage, before);
        }
        const Mix then = shifted(t);
        const LinkReading at = linkReading(animator, &then, stage, pass, switches.now - t);
        if (!SameShares(at.shares, held)) {
            const Matrix44f standing = BlendFrames(Carried(carry.offsets, at.carriers), held);
            for (std::size_t i = 0; i < at.carriers.size(); ++i) {
                carry.offsets[i] = standing * Matrix44f::inverse(at.carriers[i]);
            }
            held = at.shares;
        }
        if (pass.kept != nullptr) {
            pass.kept->offsets[keptAt(t)] = carry.offsets;
        }
    }
    pass.carries.emplace(std::move(key), carry);
    return carry;
}

void StageRunner::simulate(const Animator& animator, Pose& pose, const PoseStage& stage, f32 dt,
                           const StageHooks* hooks) {
    const NodeTree& tree = model_->nodes;
    if (stage.kind != StageKind::Spring) {
        if (hooks != nullptr && hooks->step) {
            StepHost(*hooks, *document_, modelIndex_, animator, stage, pose, dt);
        }
        return;
    }
    // A damped jiggle: each driven node's origin is a mass pulled toward where
    // the pose carries it.
    Body& body = state_.bodies[stage.id];
    const bool fresh = !body.primed || body.position.size() != stage.driven.size();
    body.position.resize(stage.driven.size());
    body.velocity.resize(stage.driven.size(), Vector3f{0, 0, 0});
    body.primed = true;
    const f32 pull = std::clamp(stage.stiffness, 0.0f, 1.0f) * 3600.0f;
    const f32 keep = std::clamp(stage.damping, 0.0f, 1.0f) * 60.0f;
    for (std::size_t k = 0; k < stage.driven.size(); ++k) {
        const u32 node = stage.driven[k];
        if (!ValidNode(tree, node)) {
            continue;
        }
        const Vector3f carried = Origin(pose.frame[node]);
        Vector3f& x = body.position[k];
        Vector3f& v = body.velocity[k];
        if (fresh) {
            x = carried;
            v = Vector3f{0, 0, 0};
        }
        const Vector3f accel = (carried - x) * pull - v * keep + Vector3f{0, 0, -stage.gravity};
        v = v + accel * dt;
        x = x + v * dt;
    }
}

void StageRunner::step(const Animator& animator, const Mix& mix, f32 at, f32 loop, const StageHooks* hooks) {
    // The carried pose at the step's own time: the layers and every
    // constraint, with the physics stages to come acting on it.
    Mix moved = mix;
    for (Play& play : moved.plays) {
        f32 when = play.seconds + (at - moved.worldSeconds);
        if (loop > 0.0f) {
            when = std::fmod(std::fmod(when, loop) + loop, loop);
        }
        play.seconds = std::max(when, 0.0f);
    }
    moved.worldSeconds = std::max(at, 0.0f);
    Pose carried;
    animator.evaluate(moved, carried);
    LinkPass pass;
    for (const PoseStage& stage : model_->poseStages) {
        if (stage.enabled && !IsPhysicsStage(stage.kind)) {
            constrainOne(animator, moved, carried, stage, pass);
        }
    }
    for (const PoseStage& stage : model_->poseStages) {
        if (stage.enabled && IsPhysicsStage(stage.kind)) {
            simulate(animator, carried, stage, kStep, hooks);
        }
    }
    started_ = true;
}

void StageRunner::run(const Animator& animator, const Mix& mix, Pose& pose, f32 seconds, f32 loop,
                      const StageHooks* hooks) {
    if (empty()) {
        return;
    }
    constrain(animator, mix, pose);
    const bool physics = std::any_of(model_->poseStages.begin(), model_->poseStages.end(),
                                     [](const PoseStage& stage) { return stage.enabled && IsPhysicsStage(stage.kind); });
    if (!physics) {
        return;
    }
    // Fixed steps up to @p seconds; a looping timeline starts a cycle early,
    // so the first cycle shown is a warmed one.
    const i64 first = loop > 0.0f ? -static_cast<i64>(std::lround(loop / kStep)) : 0;
    const i64 wanted = static_cast<i64>(std::floor(seconds / kStep + 1e-4f));
    // A host's bodies are not in the snapshots: going back with one starts
    // over, warmed, which lands where a straight run does all the same.
    const bool hosted = std::any_of(model_->poseStages.begin(), model_->poseStages.end(), [](const PoseStage& s) {
        return s.enabled && IsPhysicsStage(s.kind) && s.kind != StageKind::Spring;
    });
    if (started_ && wanted < steps_ && hosted) {
        reset(hooks);
    }
    if (!started_ || wanted < steps_) {
        // Back in time, or a first run: the nearest snapshot at or before it.
        auto found = snapshots_.upper_bound(wanted);
        if (found != snapshots_.begin() && started_) {
            --found;
            state_ = found->second;
            steps_ = found->first;
        } else {
            state_ = State{};
            started_ = false;
            steps_ = first;
        }
    }
    Mix at = mix;
    at.worldSeconds = seconds;
    while (steps_ < wanted) {
        ++steps_;
        step(animator, at, static_cast<f32>(steps_) * kStep, loop, hooks);
        if (steps_ % kSnapshotEvery == 0) {
            snapshots_[steps_] = state_;
        }
    }
    // The state onto the pose: each spring's masses become its nodes' origins,
    // and a host's bodies are laid on without a step.
    const NodeTree& tree = model_->nodes;
    for (const PoseStage& stage : model_->poseStages) {
        if (stage.enabled && IsPhysicsStage(stage.kind) && stage.kind != StageKind::Spring && hooks != nullptr &&
            hooks->step) {
            StepHost(*hooks, *document_, modelIndex_, animator, stage, pose, 0.0f);
        }
    }
    for (const PoseStage& stage : model_->poseStages) {
        if (!stage.enabled || stage.kind != StageKind::Spring) {
            continue;
        }
        const auto found = state_.bodies.find(stage.id);
        if (found == state_.bodies.end()) {
            continue;
        }
        const f32 weight = StageWeight(*model_, stage, pose);
        for (std::size_t k = 0; k < stage.driven.size() && k < found->second.position.size(); ++k) {
            const u32 node = stage.driven[k];
            if (!ValidNode(tree, node)) {
                continue;
            }
            Matrix44f moved = pose.frame[node];
            moved.data[3][0] = found->second.position[k].x;
            moved.data[3][1] = found->second.position[k].y;
            moved.data[3][2] = found->second.position[k].z;
            Place(animator, tree, pose, node, moved, weight);
        }
    }
}

void StageRunner::constrain(const Animator& animator, const Mix& mix, Pose& pose) const {
    constrain(animator, mix, pose, StageHolds{});
}

void StageRunner::constrain(const Animator& animator, const Mix& mix, Pose& pose, const StageHolds& held) const {
    constrainOn(animator, mix, pose, held, nullptr);
}

void StageRunner::constrain(const Animator& animator, const StageTimeline& timeline, f32 seconds, Pose& pose) const {
    if (!timeline.sample) {
        return;
    }
    // No play: the moment is the timeline's, by the world clock alone.
    Mix moment;
    moment.globals = false;
    moment.worldSeconds = seconds;
    constrainOn(animator, moment, pose, StageHolds{}, &timeline);
}

void StageRunner::constrainOn(const Animator& animator, const Mix& mix, Pose& pose, const StageHolds& held,
                              const StageTimeline* timeline) const {
    if (model_ == nullptr) {
        return;
    }
    LinkPass pass;
    pass.held = &held;
    pass.timeline = timeline;
    if (timeline != nullptr) {
        pass.kept = timeline->carries != nullptr ? timeline->carries : &pass.own;
    }
    const std::vector<StageHold> now = InOrder(model_->nodes, pose, held.now);
    for (const StageHold& hold : now) {
        Stand(animator, model_->nodes, pose, hold);
    }
    for (const PoseStage& stage : model_->poseStages) {
        if (stage.enabled && !IsPhysicsStage(stage.kind)) {
            constrainHeld(animator, mix, pose, stage, pass, now);
        }
    }
}

void StageRunner::constrainHeld(const Animator& animator, const Mix& mix, Pose& pose, const PoseStage& stage,
                                LinkPass& pass, std::span<const StageHold> held) const {
    if (held.empty()) {
        constrainOne(animator, mix, pose, stage, pass);
        return;
    }
    const NodeTree& tree = model_->nodes;
    const auto holds = [&](u32 node) {
        return std::any_of(held.begin(), held.end(), [&](const StageHold& hold) { return hold.node == node; });
    };
    // Every node it drives is held: what holds them has the stage's say.
    bool any = false;
    bool all = true;
    for (const u32 node : stage.driven) {
        if (ValidNode(tree, node)) {
            any = true;
            all = all && holds(node);
        }
    }
    if (any && all) {
        return;
    }
    constrainOne(animator, mix, pose, stage, pass);
    // What it moved or carried of the held nodes, back where they are held.
    for (const StageHold& hold : held) {
        u32 climbed = 0;
        for (u32 up = hold.node; up < tree.size() && climbed <= tree.size(); up = tree.nodes[up].parent, ++climbed) {
            if (std::find(stage.driven.begin(), stage.driven.end(), up) != stage.driven.end()) {
                Stand(animator, tree, pose, hold);
                break;
            }
        }
    }
}

void StageRunner::reset(const StageHooks* hooks) {
    state_ = State{};
    snapshots_.clear();
    started_ = false;
    steps_ = 0;
    if (hooks != nullptr && hooks->reset && model_ != nullptr) {
        for (const PoseStage& stage : model_->poseStages) {
            hooks->reset(modelIndex_, stage);
        }
    }
}

SimulatedStages::SimulatedStages(const Document& document, u32 model, SimulatedSteps& steps)
    : stages_(document, model), steps_(steps) {
    if (model >= document.models.size()) {
        return;
    }
    model_ = &document.models[model];
    const NodeTree& tree = model_->nodes;
    for (const PoseStage& stage : model_->poseStages) {
        if (!stage.enabled || IsPhysicsStage(stage.kind) || IsGameStage(stage.kind)) {
            continue;
        }
        for (const u32 node : stage.driven) {
            if (ValidNode(tree, node) && std::find(driven_.begin(), driven_.end(), node) == driven_.end()) {
                driven_.push_back(node);
            }
        }
    }
    std::stable_sort(driven_.begin(), driven_.end(),
                     [&](u32 a, u32 b) { return DepthOf(tree, a) < DepthOf(tree, b); });
    for (u32 n = 0; n < tree.size(); ++n) {
        if (tree.nodes[n].kind == NodeKind::ForceField) {
            fields_.push_back(n);
        }
    }
    // A Link reads back to its clip's start at most.
    for (const Clip& clip : document.clips) {
        if (clip.model == model) {
            keep_ = std::max(keep_, clip.duration);
        }
    }
    keep_ += 1.0f;
}

StageHolds SimulatedStages::holds(f32 seconds, std::span<const StageHold> now) const {
    StageHolds out;
    out.now = now;
    out.earlier = [this, seconds, now](f32 ago) -> std::span<const StageHold> {
        // Nearer this step than the last one kept: this step's.
        const f32 at = seconds - ago;
        if (steps_.times.empty() || at >= (steps_.times.back() + seconds) * 0.5f) {
            return now;
        }
        auto found = std::lower_bound(steps_.times.begin(), steps_.times.end(), at);
        if (found == steps_.times.end() || (found != steps_.times.begin() && at - *(found - 1) < *found - at)) {
            --found;
        }
        return steps_.held[static_cast<std::size_t>(found - steps_.times.begin())];
    };
    return out;
}

void SimulatedStages::before(const Animator& animator, const Mix& mix, f32 seconds, Pose& pose,
                             std::span<const u8> simulated) {
    steps_.led = false;
    if (empty()) {
        return;
    }
    steps_.tracks = pose;
    stages_.constrain(animator, mix, pose);
    // What the last step wrote and the coming one still moves.
    std::vector<StageHold> lag;
    if (!steps_.held.empty()) {
        for (const StageHold& hold : steps_.held.back()) {
            if (hold.node < simulated.size() && simulated[hold.node] != 0) {
                lag.push_back(hold);
            }
        }
    }
    if (lag.empty()) {
        return;
    }
    steps_.read = steps_.tracks;
    stages_.constrain(animator, mix, steps_.read, holds(seconds, lag));
    // The pose stays the animation's but for the constrained nodes, which
    // stand where the simulation puts what they read; one it moves itself is
    // its own.
    const NodeTree& tree = model_->nodes;
    std::vector<Matrix44f> fields;
    for (const u32 node : fields_) {
        fields.push_back(pose.frame[node]);
    }
    for (const u32 node : driven_) {
        const bool own = std::any_of(lag.begin(), lag.end(), [&](const StageHold& hold) { return hold.node == node; });
        if (!own) {
            Stand(animator, tree, pose, StageHold{node, steps_.read.frame[node]});
        }
    }
    for (std::size_t f = 0; f < fields_.size(); ++f) {
        pose.frame[fields_[f]] = fields[f];
    }
    steps_.led = true;
}

void SimulatedStages::after(const Animator& animator, const Mix& mix, f32 seconds, Pose& pose,
                            std::span<const u8> written) {
    if (empty()) {
        return;
    }
    std::vector<StageHold> held;
    for (u32 node = 0; node < written.size() && node < pose.frame.size(); ++node) {
        if (written[node] != 0) {
            held.push_back(StageHold{node, pose.frame[node]});
        }
    }
    // Nothing simulated and nothing led: the pose is the animation's already.
    if (!held.empty() || steps_.led) {
        steps_.read = steps_.tracks;
        for (const StageHold& hold : held) {
            steps_.read.local[hold.node].translation = pose.local[hold.node].translation;
            steps_.read.local[hold.node].rotation = pose.local[hold.node].rotation;
        }
        animator.compose(steps_.read);
        stages_.constrain(animator, mix, steps_.read, holds(seconds, held));
        for (const u32 node : fields_) {
            steps_.read.frame[node] = pose.frame[node];
        }
        std::swap(pose, steps_.read);
    }
    // Kept for as long as a Link can read back.
    if (steps_.times.size() > 64 && steps_.times[steps_.times.size() / 2] < seconds - keep_) {
        const auto half = static_cast<std::ptrdiff_t>(steps_.times.size() / 2);
        steps_.times.erase(steps_.times.begin(), steps_.times.begin() + half);
        steps_.held.erase(steps_.held.begin(), steps_.held.begin() + half);
    }
    steps_.times.push_back(seconds);
    steps_.held.push_back(std::move(held));
    steps_.led = false;
}

namespace {

/// The node channels a stage of @p kind writes: rotations for orientation,
/// look-at and IK; translation for position and spring; all three for a link
/// and a host's physics.
std::vector<Channel> WrittenBy(StageKind kind) {
    switch (kind) {
    case StageKind::Orientation:
    case StageKind::LookAt:
    case StageKind::LimbIk:
    case StageKind::ChainIk:
        return {Channel::Rotation};
    case StageKind::Position:
    case StageKind::Spring:
        return {Channel::Translation};
    default:
        return {Channel::Translation, Channel::Rotation, Channel::Scale};
    }
}

/// @p model's channel for @p node's @p channel, declared when it has none.
u32 NodeChannel(Model& model, u32 node, Channel channel) {
    for (const AnimChannel& entry : model.animChannels.channels) {
        if (entry.target.kind == TrackTarget::Kind::Node && entry.target.node == node &&
            entry.target.channel == channel && entry.target.sub == 0) {
            return entry.id;
        }
    }
    AnimChannel made;
    made.id = model.animChannels.nextFreeId();
    made.target.kind = TrackTarget::Kind::Node;
    made.target.node = node;
    made.target.channel = channel;
    made.valueType = channel == Channel::Rotation ? geom::AttrType::Quat : geom::AttrType::F32x3;
    return model.animChannels.add(made);
}

template <class T>
void Append(std::vector<u8>& bytes, const T& value) {
    const u8* at = reinterpret_cast<const u8*>(&value);
    bytes.insert(bytes.end(), at, at + sizeof(T));
}

/// Whether @p target runs @p stage itself: the rig or cloth it names goes out
/// as records, and the bake leaves it to the game (WEM_PHYSICS_DESIGN.md §7).
bool RunsNatively(const Model& model, const PoseStage& stage, ProfileId target) {
    if (IsGameStage(stage.kind)) {
        return (Profile(target).nativeStages & (1u << static_cast<u32>(stage.kind))) != 0;
    }
    const PhysicsCaps& caps = Profile(target).physics;
    if (stage.kind == StageKind::Ragdoll) {
        return stage.rig != 0 && caps.shapeKinds != 0 && model.physics.rig(stage.rig) != nullptr;
    }
    return stage.kind == StageKind::Cloth && stage.cloth != 0 && caps.cloth &&
           model.physics.cloth(stage.cloth) != nullptr;
}

/// A natively run stage's weight as the switches the game reads: each rig
/// body's `PhysicsDynamic`, or the cloth's `ClothActive`, on wherever the
/// weight is over a half at the bake's rate. They replace the records' own.
void WeightToSwitches(Document& document, u32 m, const PoseStage& stage) {
    Model& model = document.models[m];
    const bool ragdoll = stage.kind == StageKind::Ragdoll;
    const Channel channel = ragdoll ? Channel::PhysicsDynamic : Channel::ClothActive;
    // No weight key means a weight of 1: on.
    std::vector<u32> switches;
    for (const u32 record : ragdoll ? model.physics.rig(stage.rig)->bodies : std::vector<u32>{stage.cloth}) {
        if (PhysicsBody* body = ragdoll ? model.physics.body(record) : nullptr) {
            body->simulates = true;
        } else if (Cloth* cloth = ragdoll ? nullptr : model.physics.cloth(record)) {
            cloth->active = true;
        } else {
            continue;
        }
        switches.push_back(PhysicsSwitchChannel(model, record, channel));
    }
    const AnimChannel* weight = StageWeightChannel(model, stage);
    const u32 weightId = weight != nullptr ? weight->id : 0;
    const f32 one = 1.0f;
    for (Clip& clip : document.clips) {
        if (clip.model != m || clip.containers.empty()) {
            continue;
        }
        for (SubTrackContainer& container : clip.containers) {
            std::erase_if(container.subTracks, [&](const SubTrack& track) {
                return std::find(switches.begin(), switches.end(), track.channel) != switches.end();
            });
        }
        const SubTrack* keyed = weight != nullptr ? FindSubTrack(clip, weightId) : nullptr;
        if (keyed == nullptr || keyed->times.empty()) {
            continue;
        }
        const u32 count = std::max<u32>(1, static_cast<u32>(std::ceil(clip.duration * kBakeRate)));
        std::vector<i32> timesMs;
        for (u32 k = 0; k <= count; ++k) {
            timesMs.push_back(static_cast<i32>(Milliseconds(std::min(static_cast<f32>(k) / kBakeRate, clip.duration))));
        }
        const std::vector<u8> sampled = SampleSubTrackBatch(
            clip, *keyed, geom::AttrType::F32, timesMs,
            std::span<const u8>(reinterpret_cast<const u8*>(&one), sizeof(one)));
        SubTrack track;
        track.interp = Interpolation::Step;
        for (u32 k = 0; k < timesMs.size() && (k + 1) * sizeof(f32) <= sampled.size(); ++k) {
            f32 w = 1.0f;
            std::memcpy(&w, sampled.data() + k * sizeof(f32), sizeof(f32));
            const f32 on = w > 0.5f ? 1.0f : 0.0f;
            f32 last = -1.0f;
            if (!track.values.empty()) {
                std::memcpy(&last, track.values.data() + track.values.size() - sizeof(f32), sizeof(f32));
            }
            if (on != last) {
                track.times.push_back(static_cast<f32>(timesMs[k]) / 1000.0f);
                Append(track.values, on);
            }
        }
        for (const u32 id : switches) {
            track.channel = id;
            clip.containers.front().subTracks.push_back(track);
        }
    }
}

/// Every stage and its own channels gone from @p model, with their sub-tracks
/// from every clip -- but a game stage @p target runs itself, which stays for
/// the export to write as its records (EDIT_MODE_SC2_PROFILE_PLAN.md P1). Its
/// weight goes: nothing in the game keys it.
void RemoveStages(Document& document, u32 model, ProfileId target) {
    Model& owner = document.models[model];
    std::set<u32> weights;
    for (const AnimChannel& channel : owner.animChannels.channels) {
        if (IsStageChannel(channel.target)) {
            weights.insert(channel.id);
        }
    }
    const auto kept = [&](const PoseStage& stage) {
        return IsGameStage(stage.kind) && RunsNatively(owner, stage, target);
    };
    std::erase_if(owner.animChannels.channels,
                  [&](const AnimChannel& channel) { return weights.count(channel.id) != 0; });
    for (Clip& clip : document.clips) {
        if (clip.model != model) {
            continue;
        }
        for (SubTrackContainer& container : clip.containers) {
            std::erase_if(container.subTracks,
                          [&](const SubTrack& track) { return weights.count(track.channel) != 0; });
        }
    }
    std::erase_if(owner.poseStages, [&](const PoseStage& stage) { return !kept(stage); });
}

} // namespace

u32 BakeStageKeys(Document& document, ProfileId target, const StageHooks* hooks, Diagnostics& diagnostics) {
    u32 rewritten = 0;
    // StarCraft II plays every layer itself (§4): its clips go out as
    // authored. Anything else has one layer, flattened first so the stages
    // below write over the whole pose.
    if (GameOf(target) != Game::StarCraft) {
        rewritten += FlattenContainers(document, diagnostics);
    }
    for (u32 m = 0; m < document.models.size(); ++m) {
        if (document.models[m].poseStages.empty()) {
            continue;
        }
        // A game's own stage goes out as its records for the game that runs
        // it, and is dropped for any other: nothing else runs it, and it is
        // the game's behaviour, not an animation to bake.
        for (PoseStage& stage : document.models[m].poseStages) {
            if (!IsGameStage(stage.kind)) {
                continue;
            }
            const ElementRef where(ElementKind::Node, stage.driven.empty() ? kInvalidNode : stage.driven.front());
            if (!RunsNatively(document.models[m], stage, target)) {
                diagnostics.info(DiagCode::AnimStageNotBaked,
                                 "stage '" + stage.name + "' is StarCraft II's own; " + Profile(target).displayName +
                                     " does not run it, so it is left out",
                                 where);
                stage.enabled = false;
                continue;
            }
            const AnimChannel* weight = StageWeightChannel(document.models[m], stage);
            bool keyed = false;
            for (const Clip& clip : document.clips) {
                keyed = keyed || (weight != nullptr && clip.model == m && FindSubTrack(clip, weight->id) != nullptr);
            }
            if (keyed) {
                diagnostics.warn(DiagCode::AnimStageNotBaked,
                                 "stage '" + stage.name + "' has a keyed weight, which the game has no switch for",
                                 where);
            }
        }
        // A rig or cloth the target runs itself goes out as its records and
        // their switches, not as keys.
        for (PoseStage& stage : document.models[m].poseStages) {
            if (stage.enabled && !IsGameStage(stage.kind) && RunsNatively(document.models[m], stage, target)) {
                const ElementRef where(ElementKind::Node, stage.driven.empty() ? kInvalidNode : stage.driven.front());
                // A game with no switches runs what it builds from creation.
                if (Profile(target).physics.switches) {
                    WeightToSwitches(document, m, stage);
                    diagnostics.info(DiagCode::AnimStageBaked, "stage '" + stage.name + "' left to the game's physics",
                                     where);
                } else {
                    diagnostics.info(DiagCode::AnimStageBaked,
                                     "stage '" + stage.name +
                                         "' left to the game's physics, which runs from creation; its weight is not kept",
                                     where);
                }
                stage.enabled = false;
            }
        }
        // What can be baked here: every enabled stage but a host's physics
        // with no host to run it.
        const auto hosted = [&](StageKind kind) {
            return hooks != nullptr && static_cast<bool>(hooks->step) && (!hooks->steps || hooks->steps(kind));
        };
        for (PoseStage& stage : document.models[m].poseStages) {
            if (stage.enabled && (stage.kind == StageKind::Ragdoll || stage.kind == StageKind::Cloth) &&
                !hosted(stage.kind)) {
                diagnostics.error(DiagCode::AnimStageNotBaked,
                                  "stage '" + stage.name + "' needs the host's physics, which this export does not have",
                                  ElementRef(ElementKind::Node, stage.driven.empty() ? kInvalidNode : stage.driven.front()));
                stage.enabled = false;
            }
        }
        const Model& model = document.models[m];
        std::set<std::pair<u32, Channel>> written;
        for (const PoseStage& stage : model.poseStages) {
            if (!stage.enabled || IsGameStage(stage.kind)) {
                continue;
            }
            for (const u32 node : stage.driven) {
                for (const Channel channel : WrittenBy(stage.kind)) {
                    written.insert({node, channel});
                }
            }
        }
        if (!written.empty()) {
            // Sampled on the document as it stands, then written.
            const Document source = document;
            const Animator animator(source, m);
            for (u32 c = 0; c < source.clips.size(); ++c) {
                const Clip& clip = source.clips[c];
                if (clip.model != m || IsGlobalLoop(clip) || clip.containers.empty()) {
                    continue;
                }
                // Each clip starts from rest, whatever the host kept from the last.
                StageRunner runner(source, m);
                runner.reset(hooks);
                const f32 loop = clip.looping ? clip.duration : 0.0f;
                const u32 count = std::max<u32>(1, static_cast<u32>(std::ceil(clip.duration * kBakeRate)));
                std::vector<f32> times;
                std::vector<Pose> poses;
                for (u32 k = 0; k <= count; ++k) {
                    const f32 at = std::min(static_cast<f32>(k) / kBakeRate, clip.duration);
                    Mix mix;
                    mix.plays.push_back(Play{c, at, 1.0f, clip.looping});
                    mix.worldSeconds = at;
                    Pose pose;
                    animator.evaluate(mix, pose);
                    runner.run(animator, mix, pose, at, loop, hooks);
                    times.push_back(at);
                    poses.push_back(std::move(pose));
                }
                // A physics loop's seam, blended over its last tenth of a
                // second back into the first frame.
                const bool physical = std::any_of(model.poseStages.begin(), model.poseStages.end(),
                                                  [](const PoseStage& s) { return s.enabled && IsPhysicsStage(s.kind); });
                if (physical && loop > 0.0f) {
                    for (std::size_t k = 0; k < times.size(); ++k) {
                        const f32 into = times[k] - (clip.duration - kSeamSeconds);
                        if (into <= 0.0f) {
                            continue;
                        }
                        const f32 w = std::min(into / kSeamSeconds, 1.0f);
                        for (u32 n = 0; n < poses[k].local.size(); ++n) {
                            poses[k].local[n] = Blend(poses[k].local[n], poses.front().local[n], w);
                        }
                    }
                }
                Model& owner = document.models[m];
                Clip& out = document.clips[c];
                for (const auto& [node, channel] : written) {
                    if (node >= owner.nodes.size()) {
                        continue;
                    }
                    const u32 id = NodeChannel(owner, node, channel);
                    SubTrack track;
                    track.channel = id;
                    track.interp = channel == Channel::Rotation ? Interpolation::Slerp : Interpolation::Linear;
                    for (std::size_t k = 0; k < times.size(); ++k) {
                        track.times.push_back(times[k]);
                        const Transform& local = poses[k].local[node];
                        if (channel == Channel::Rotation) {
                            Append(track.values, local.rotation);
                        } else if (channel == Channel::Translation) {
                            Append(track.values, local.translation);
                        } else {
                            Append(track.values, local.scale);
                        }
                    }
                    // The stage's output is what plays, over every layer.
                    for (SubTrackContainer& container : out.containers) {
                        std::erase_if(container.subTracks, [&](const SubTrack& t) { return t.channel == id; });
                    }
                    out.containers.front().subTracks.push_back(std::move(track));
                }
                ++rewritten;
            }
        }
        for (const PoseStage& stage : document.models[m].poseStages) {
            if (stage.enabled && !IsGameStage(stage.kind)) {
                diagnostics.info(DiagCode::AnimStageBaked, "stage '" + stage.name + "' baked into keys",
                                 ElementRef(ElementKind::Node, stage.driven.empty() ? kInvalidNode : stage.driven.front()));
            }
        }
        RemoveStages(document, m, target);
    }
    return rewritten;
}

} // namespace wem
} // namespace models
} // namespace whiteout
