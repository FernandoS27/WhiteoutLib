// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "whiteout/models/wem/anim/stages.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>

#include "whiteout/models/wem/anim/crossing.h"
#include "whiteout/models/wem/anim/track_read.h"
#include "whiteout/models/wem/physics/crossing.h"
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
    default:
        return 9.8f;
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

void StageRunner::constrainOne(const Animator& animator, const Mix* mix, Pose& pose, const PoseStage& stage) const {
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
        // The node's own animation under whatever carries it: its place under
        // its parent, then its carriers, each under the offset the last switch
        // left it, blended by their shares.
        const Matrix44f local = pose.frame[node] * Matrix44f::inverse(ParentFrame(tree, pose, node));
        const LinkCarry carry = linkCarry(animator, mix, stage);
        const Matrix44f carrier = BlendFrames(Carried(carry.offsets, Carriers(tree, stage, node, pose)),
                                              CarrierShares(*model_, stage, pose));
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

void StageRunner::constrainBefore(const Animator& animator, const Mix* mix, Pose& pose,
                                  const PoseStage& stage) const {
    for (const PoseStage& earlier : model_->poseStages) {
        if (&earlier == &stage) {
            return;
        }
        if (earlier.enabled && !IsPhysicsStage(earlier.kind)) {
            constrainOne(animator, mix, pose, earlier);
        }
    }
}

StageRunner::LinkCarry StageRunner::linkCarry(const Animator& animator, const Mix* mix,
                                              const PoseStage& stage) const {
    const NodeTree& tree = model_->nodes;
    const u32 node = stage.driven.front();
    // The pose at @p at: the layers and every stage before this one.
    const auto poseAt = [&](const Mix& at) {
        Pose pose;
        animator.evaluate(at, pose);
        constrainBefore(animator, &at, pose, stage);
        return pose;
    };
    // At rest every carrier is offset so that it stands the node where its
    // parent puts it.
    Mix rest;
    rest.globals = false;
    const Pose resting = poseAt(rest);
    const std::vector<Matrix44f> still = Carriers(tree, stage, node, resting);
    LinkCarry carry;
    for (const Matrix44f& carrier : still) {
        carry.offsets.push_back(still.front() * Matrix44f::inverse(carrier));
    }
    if (mix == nullptr) {
        return carry;
    }
    // The switches are where the sources' Enabled keys are, in the newest play
    // that keys any, up to where it plays. Where one changes the shares, every
    // carrier is rebased onto the blend the old shares made, so the node stays
    // put: `O_i' = K_old·C_i⁻¹` at that moment.
    std::vector<u32> channels;
    for (const AnimChannel& entry : model_->animChannels.channels) {
        if (entry.target.kind == TrackTarget::Kind::Node && entry.target.channel == Channel::StageSourceEnabled &&
            entry.target.node == node && StageOfSub(entry.target.sub) == stage.id) {
            channels.push_back(entry.id);
        }
    }
    std::vector<f32> held = CarrierShares(*model_, stage, resting);
    for (const Play& play : mix->plays) {
        if (play.clip >= document_->clips.size() || document_->clips[play.clip].model != modelIndex_) {
            continue;
        }
        const Clip& clip = document_->clips[play.clip];
        // The end itself is the end, as the tracks read it; only past it wraps.
        f32 now = play.seconds;
        if (play.loop && clip.duration > 0.0f && now > clip.duration) {
            now = std::fmod(now, clip.duration);
        }
        std::set<f32> times;
        for (const u32 id : channels) {
            if (const SubTrack* track = FindSubTrack(clip, id)) {
                for (const f32 t : track->times) {
                    if (t <= now + 1e-6f) {
                        times.insert(t);
                    }
                }
            }
        }
        if (times.empty()) {
            continue;
        }
        const auto shifted = [&](f32 t) {
            Mix moved = *mix;
            const f32 shift = t - now;
            for (Play& other : moved.plays) {
                other.seconds = std::max(other.seconds + shift, 0.0f);
            }
            moved.worldSeconds = std::max(moved.worldSeconds + shift, 0.0f);
            return moved;
        };
        constexpr f32 kMillisecond = 0.001f;
        for (const f32 t : times) {
            // The shares just before, as they played a millisecond earlier: a
            // stepped key's the last key's, an interpolated one's nearly its
            // own. At the clip's start, rest's.
            if (t >= kMillisecond) {
                Pose before;
                animator.sample(shifted(t - kMillisecond), before, true);
                held = CarrierShares(*model_, stage, before);
            }
            const Pose at = poseAt(shifted(t));
            const std::vector<f32> shares = CarrierShares(*model_, stage, at);
            if (SameShares(shares, held)) {
                continue;
            }
            const std::vector<Matrix44f> carriers = Carriers(tree, stage, node, at);
            const Matrix44f standing = BlendFrames(Carried(carry.offsets, carriers), held);
            for (std::size_t i = 0; i < carriers.size(); ++i) {
                carry.offsets[i] = standing * Matrix44f::inverse(carriers[i]);
            }
            held = shares;
        }
        break;
    }
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
    for (const PoseStage& stage : model_->poseStages) {
        if (stage.enabled && !IsPhysicsStage(stage.kind)) {
            constrainOne(animator, &moved, carried, stage);
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
    if (model_ == nullptr) {
        return;
    }
    for (const PoseStage& stage : model_->poseStages) {
        if (stage.enabled && !IsPhysicsStage(stage.kind)) {
            constrainOne(animator, &mix, pose, stage);
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
/// from every clip.
void RemoveStages(Document& document, u32 model) {
    Model& owner = document.models[model];
    std::set<u32> weights;
    for (const AnimChannel& channel : owner.animChannels.channels) {
        if (IsStageChannel(channel.target)) {
            weights.insert(channel.id);
        }
    }
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
    owner.poseStages.clear();
}

} // namespace

u32 BakeStages(Document& document, ProfileId target, const StageHooks* hooks, Diagnostics& diagnostics) {
    u32 rewritten = 0;
    // The rigs and cages first, so a stage over a rig overrides its start.
    rewritten += FitPhysicsToProfile(document, target, diagnostics);
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
        // A rig or cloth the target runs itself goes out as its records and
        // their switches, not as keys.
        for (PoseStage& stage : document.models[m].poseStages) {
            if (stage.enabled && RunsNatively(document.models[m], stage, target)) {
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
        const bool hosted = hooks != nullptr && static_cast<bool>(hooks->step);
        for (PoseStage& stage : document.models[m].poseStages) {
            if (stage.enabled && (stage.kind == StageKind::Ragdoll || stage.kind == StageKind::Cloth) && !hosted) {
                diagnostics.error(DiagCode::AnimStageNotBaked,
                                  "stage '" + stage.name + "' needs the host's physics, which this export does not have",
                                  ElementRef(ElementKind::Node, stage.driven.empty() ? kInvalidNode : stage.driven.front()));
                stage.enabled = false;
            }
        }
        const Model& model = document.models[m];
        std::set<std::pair<u32, Channel>> written;
        for (const PoseStage& stage : model.poseStages) {
            if (!stage.enabled) {
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
            if (stage.enabled) {
                diagnostics.info(DiagCode::AnimStageBaked, "stage '" + stage.name + "' baked into keys",
                                 ElementRef(ElementKind::Node, stage.driven.empty() ? kInvalidNode : stage.driven.front()));
            }
        }
        RemoveStages(document, m);
    }
    return rewritten;
}

} // namespace wem
} // namespace models
} // namespace whiteout
