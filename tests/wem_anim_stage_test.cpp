// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// G-C (WEM_ANIMATION_RUNTIME_PLAN.md §9-§11): a pose stage baked into keys
/// plays what the stage played. Each fixture is posed by the `Animator` and the
/// `StageRunner`, baked as an export bakes it, and the baked clip — no stage
/// left — posed again: the driven nodes must stand where they stood.

#include <cmath>
#include <cstring>
#include <set>
#include <span>
#include <sstream>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/wem/anim/animator.h>
#include <whiteout/models/wem/anim/stage_bake.h>
#include <whiteout/models/wem/anim/stages.h>
#include <whiteout/models/wem/document.h>
#include <whiteout/models/wem/nodes/remove.h>
#include <whiteout/models/wem/rigging/ik.h>
#include <whiteout/models/wem/validate.h>

#include "whiteout/common/binary_reader.h"
#include "whiteout/common/binary_writer.h"
#include "whiteout/common/streams.h"
#include "whiteout/models/wem/binary_read_visitor.h"
#include "whiteout/models/wem/binary_write_visitor.h"

using namespace whiteout;
using namespace whiteout::models;
using namespace whiteout::models::wem;

namespace {

template <class T>
void Append(std::vector<u8>& bytes, const T& value) {
    const u8* at = reinterpret_cast<const u8*>(&value);
    bytes.insert(bytes.end(), at, at + sizeof(T));
}

Quaternion AboutZ(f32 degrees) {
    const f32 half = degrees * 3.14159265f / 360.0f;
    return Quaternion{0, 0, std::sin(half), std::cos(half)};
}

/// A pivot rig: a root turning about z, a head on it, and a target helper
/// that travels in a loop around the head.
struct Rig {
    Document document;
    u32 root = 0, head = 1, target = 2, tip = 3;
    u32 clip = 0;

    u32 channel(u32 node, Channel kind) {
        Model& model = document.models[0];
        AnimChannel entry;
        entry.id = model.animChannels.nextFreeId();
        entry.target.kind = TrackTarget::Kind::Node;
        entry.target.node = node;
        entry.target.channel = kind;
        entry.valueType = kind == Channel::Rotation ? geom::AttrType::Quat : geom::AttrType::F32x3;
        return model.animChannels.add(entry);
    }

    Rig() {
        document.profiles = {ProfileId::Wc3Reforged, ProfileId::Sc2};
        document.defaultProfile = ProfileId::Wc3Reforged;
        Model& model = document.models.emplace_back();
        const auto node = [&](const char* name, u32 parent, Vector3f pivot, NodeKind kind) {
            Node made;
            made.name = name;
            made.parent = parent;
            made.pivot = pivot;
            made.kind = kind;
            return model.nodes.add(made);
        };
        root = node("root", kInvalidNode, Vector3f{0, 0, 0}, NodeKind::Bone);
        head = node("head", root, Vector3f{0, 0, 10}, NodeKind::Bone);
        target = node("target", kInvalidNode, Vector3f{0, 0, 0}, NodeKind::Helper);
        tip = node("tip", head, Vector3f{5, 0, 10}, NodeKind::Bone);

        Clip made;
        made.name = "Stand";
        made.model = 0;
        made.duration = 2.0f;
        made.looping = true;
        SubTrackContainer& body = made.containers.emplace_back();
        // The root sways, and the target circles, both looping.
        SubTrack sway;
        sway.channel = channel(root, Channel::Rotation);
        sway.interp = Interpolation::Linear;
        sway.times = {0.0f, 1.0f, 2.0f};
        for (const f32 deg : {0.0f, 25.0f, 0.0f}) {
            Append(sway.values, AboutZ(deg));
        }
        body.subTracks.push_back(sway);
        SubTrack circle;
        circle.channel = channel(target, Channel::Translation);
        circle.interp = Interpolation::Linear;
        for (int k = 0; k <= 8; ++k) {
            const f32 a = static_cast<f32>(k) / 8.0f * 6.2831853f;
            circle.times.push_back(static_cast<f32>(k) * 0.25f);
            Append(circle.values, Vector3f{30.0f * std::cos(a), 30.0f * std::sin(a), 12.0f});
        }
        body.subTracks.push_back(circle);
        document.clips.push_back(made);
    }

    /// A stage reading @p nodes: a constraint's sources, numbered from 1 at
    /// full weight (a Link's disabled), or an IK stage's goal and pole.
    PoseStage& stage(StageKind kind, std::vector<u32> driven, std::vector<u32> nodes) {
        PoseStage made;
        made.id = static_cast<u32>(document.models[0].poseStages.size()) + 1;
        made.name = ToString(kind);
        made.kind = kind;
        made.driven = std::move(driven);
        if (IsConstraint(kind)) {
            for (const u32 node : nodes) {
                made.sources.push_back(StageSource{static_cast<u32>(made.sources.size()) + 1, node,
                                                   kind == StageKind::Link ? 0.0f : 1.0f});
            }
        } else {
            made.targets = std::move(nodes);
        }
        CaptureStageOffset(document, 0, made);
        document.models[0].poseStages.push_back(made);
        return document.models[0].poseStages.back();
    }

    /// @p stage's source @p source's channel, keyed at @p times to @p values;
    /// stepped for a Link, whose values are its shares.
    void key(const PoseStage& stage, u32 source, std::vector<f32> times, std::vector<f32> values) {
        Model& model = document.models[0];
        AnimChannel entry;
        entry.id = model.animChannels.nextFreeId();
        entry.target.kind = TrackTarget::Kind::Node;
        entry.target.node = stage.driven.front();
        entry.target.channel =
            stage.kind == StageKind::Link ? Channel::StageSourceEnabled : Channel::StageSourceWeight;
        entry.target.sub = StageSub(stage.id, source);
        entry.valueType = geom::AttrType::F32;
        model.animChannels.add(entry);
        SubTrack track;
        track.channel = entry.id;
        track.interp = stage.kind == StageKind::Link ? Interpolation::Step : Interpolation::Linear;
        track.times = std::move(times);
        for (const f32 value : values) {
            Append(track.values, value);
        }
        document.clips[clip].containers[0].subTracks.push_back(track);
    }
};

/// Every node's frame at @p seconds, staged or not.
std::vector<Matrix44f> Frames(const Document& document, f32 seconds, bool staged) {
    const Animator animator(document, 0);
    Mix mix;
    mix.plays.push_back(Play{0, seconds, 1.0f, true});
    mix.worldSeconds = seconds;
    Pose pose;
    animator.evaluate(mix, pose);
    if (staged) {
        StageRunner runner(document, 0);
        runner.run(animator, mix, pose, seconds, document.clips[0].looping ? document.clips[0].duration : 0.0f,
                   nullptr);
    }
    return pose.frame;
}

f32 Worst(const Matrix44f& a, const Matrix44f& b) {
    f32 worst = 0;
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            worst = std::max(worst, std::fabs(a.data[i][j] - b.data[i][j]));
        }
    }
    return worst;
}

Vector3f OriginOf(const Matrix44f& m) {
    return Vector3f{m.data[3][0], m.data[3][1], m.data[3][2]};
}

/// The stage's pose against the bake's, for @p target, at a spread of times;
/// and the key counts the bake wrote. A loop whose stage changes over it (a
/// Link that switches) need not meet its own start.
void CheckBake(const Document& staged, u32 node, ProfileId target, f32 tolerance, bool seamless = true) {
    Document baked = staged;
    Diagnostics report;
    BakeStages(baked, target, nullptr, report);
    CHECK(baked.models[0].poseStages.empty());
    CHECK(report.countOf(DiagCode::AnimStageBaked) == staged.models[0].poseStages.size());
    for (const f32 at : {0.0f, 0.1f, 0.37f, 0.8f, 1.25f, 1.6f, 1.99f}) {
        INFO("at " << at);
        CHECK(Worst(Frames(staged, at, true)[node], Frames(baked, at, false)[node]) < tolerance);
    }
    // At most one key per sixtieth of a second on each channel it wrote.
    for (const SubTrack& track : baked.clips[0].containers[0].subTracks) {
        CHECK(track.times.size() <= static_cast<std::size_t>(std::ceil(staged.clips[0].duration * 60.0f)) + 1);
    }
    // A loop with looping inputs is seamless.
    if (seamless) {
        CHECK(Worst(Frames(baked, 0.0f, false)[node], Frames(baked, staged.clips[0].duration, false)[node]) <
              tolerance);
    }
}

} // namespace

TEST_CASE("wem a Look At turns its node toward the source and bakes to the same pose",
          "[wem][anim][stages]") {
    Rig rig;
    rig.stage(StageKind::LookAt, {rig.head}, {rig.target});
    // Aimed: the head's x axis runs from the head to the target.
    for (const f32 at : {0.3f, 1.1f}) {
        const std::vector<Matrix44f> frames = Frames(rig.document, at, true);
        const Vector3f head{frames[rig.head].data[3][0], frames[rig.head].data[3][1], frames[rig.head].data[3][2]};
        const Vector3f target{frames[rig.target].data[3][0], frames[rig.target].data[3][1],
                              frames[rig.target].data[3][2]};
        const Vector3f axis = Vector3f{frames[rig.head].data[0][0], frames[rig.head].data[0][1],
                                       frames[rig.head].data[0][2]}
                                  .normalized();
        CHECK(axis.dot((target - head).normalized()) > 0.9999f);
    }
    CheckBake(rig.document, rig.head, ProfileId::Wc3Reforged, 0.02f);
    CheckBake(rig.document, rig.head, ProfileId::Sc2, 0.02f);
}

TEST_CASE("wem a Look At keeps its up axis toward the up node", "[wem][anim][stages]") {
    Rig rig;
    PoseStage& look = rig.stage(StageKind::LookAt, {rig.head}, {rig.target});
    look.up = Vector3f{0, 1, 0};
    look.upNode = rig.root;
    for (const f32 at : {0.3f, 1.1f}) {
        const std::vector<Matrix44f> frames = Frames(rig.document, at, true);
        const Vector3f head = OriginOf(frames[rig.head]);
        const Vector3f forward = (OriginOf(frames[rig.target]) - head).normalized();
        const Vector3f up =
            Vector3f{frames[rig.head].data[1][0], frames[rig.head].data[1][1], frames[rig.head].data[1][2]}.normalized();
        const Vector3f hint = OriginOf(frames[rig.root]) - head;
        const Vector3f square = (hint - forward * hint.dot(forward)).normalized();
        CHECK(up.dot(square) > 0.999f);
    }
}

TEST_CASE("wem a Position stands at its sources' weighted place", "[wem][anim][stages]") {
    Rig rig;
    PoseStage& position = rig.stage(StageKind::Position, {rig.tip}, {rig.target, rig.root});
    position.sources[0].weight = 0.25f;
    position.sources[1].weight = 0.75f;
    for (const f32 at : {0.2f, 0.9f}) {
        const std::vector<Matrix44f> frames = Frames(rig.document, at, true);
        const Vector3f want = OriginOf(frames[rig.target]) * 0.25f + OriginOf(frames[rig.root]) * 0.75f;
        CHECK((OriginOf(frames[rig.tip]) - want).length() < 1e-3f);
    }
    CheckBake(rig.document, rig.tip, ProfileId::Wc3Reforged, 0.02f);
}

TEST_CASE("wem a Position that keeps its offset starts where the node stood", "[wem][anim][stages]") {
    Rig rig;
    PoseStage& position = rig.stage(StageKind::Position, {rig.tip}, {rig.target});
    position.keepOffset = true;
    CaptureStageOffset(rig.document, 0, position);
    Mix rest;
    rest.globals = false;
    const Animator animator(rig.document, 0);
    Pose free;
    animator.evaluate(rest, free);
    Pose held = free;
    StageRunner(rig.document, 0).constrain(animator, rest, held);
    CHECK((OriginOf(held.frame[rig.tip]) - OriginOf(free.frame[rig.tip])).length() < 1e-3f);
}

TEST_CASE("wem a source's weight is keyed like any node channel", "[wem][anim][stages]") {
    Rig rig;
    const PoseStage& position = rig.stage(StageKind::Position, {rig.tip}, {rig.target, rig.root});
    // The target's share runs from all of it to none over the clip.
    rig.key(position, 1, {0.0f, 2.0f}, {1.0f, 0.0f});
    const std::vector<Matrix44f> early = Frames(rig.document, 0.0f, true);
    CHECK((OriginOf(early[rig.tip]) - (OriginOf(early[rig.target]) + OriginOf(early[rig.root])) * 0.5f).length() <
          1e-3f);
    const std::vector<Matrix44f> late = Frames(rig.document, 1.999f, true);
    CHECK((OriginOf(late[rig.tip]) - OriginOf(late[rig.root])).length() < 0.05f);
}

TEST_CASE("wem an Orientation turns its node as its source turns", "[wem][anim][stages]") {
    Rig rig;
    rig.stage(StageKind::Orientation, {rig.target}, {rig.head});
    const std::vector<Matrix44f> frames = Frames(rig.document, 0.7f, true);
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            CHECK(std::fabs(frames[rig.target].data[r][c] - frames[rig.head].data[r][c]) < 1e-4f);
        }
    }
    CheckBake(rig.document, rig.target, ProfileId::Wc3Reforged, 0.02f);
}

TEST_CASE("wem a Link keeps its node in place at a switch and then rides the source",
          "[wem][anim][stages]") {
    Rig rig;
    const PoseStage& link = rig.stage(StageKind::Link, {rig.tip}, {rig.target});
    // Its own parent until 1 s, the target after: the target's Enabled off,
    // then on.
    rig.key(link, 1, {0.0f, 1.0f}, {0.0f, 1.0f});
    const Matrix44f before = Frames(rig.document, 0.999f, false)[rig.tip];
    const Matrix44f unlinked = Frames(rig.document, 1.0f, false)[rig.tip];
    const std::vector<Matrix44f> at = Frames(rig.document, 1.0f, true);
    CHECK(Worst(at[rig.tip], unlinked) < 1e-3f);
    CHECK(Worst(Frames(rig.document, 0.999f, true)[rig.tip], before) < 1e-4f);
    // Riding the target: where it stands relative to it does not change.
    const std::vector<Matrix44f> later = Frames(rig.document, 1.6f, true);
    const Matrix44f was = at[rig.tip] * Matrix44f::inverse(at[rig.target]);
    const Matrix44f is = later[rig.tip] * Matrix44f::inverse(later[rig.target]);
    CHECK(Worst(was, is) < 1e-3f);
    CheckBake(rig.document, rig.tip, ProfileId::Wc3Reforged, 0.02f, false);
}

TEST_CASE("wem a Link rides its first enabled source, and none is its own parent", "[wem][anim][stages]") {
    Rig rig;
    PoseStage& link = rig.stage(StageKind::Link, {rig.tip}, {rig.target, rig.root});
    // Both enabled where nothing keys them: the first wins.
    link.sources[0].weight = 1.0f;
    link.sources[1].weight = 1.0f;
    const auto relative = [&](f32 at, u32 to) {
        const std::vector<Matrix44f> frames = Frames(rig.document, at, true);
        return frames[rig.tip] * Matrix44f::inverse(frames[to]);
    };
    CHECK(Worst(relative(0.3f, rig.target), relative(1.4f, rig.target)) < 1e-3f);
    // None enabled: the animation as it is.
    link.sources[0].weight = 0.0f;
    link.sources[1].weight = 0.0f;
    CHECK(Worst(Frames(rig.document, 0.6f, true)[rig.tip], Frames(rig.document, 0.6f, false)[rig.tip]) < 1e-5f);
}

TEST_CASE("wem a Link blends its sources by share, and its own parent takes what they leave",
          "[wem][anim][stages]") {
    // Half the target and half the root, which carries the tip as its own
    // parent does; and half the target alone.
    const auto linked = [](bool both) {
        Rig rig;
        const PoseStage& link = rig.stage(StageKind::Link, {rig.tip}, {rig.target, rig.root});
        rig.key(link, 1, {0.0f}, {0.5f});
        if (both) {
            rig.key(link, 2, {0.0f}, {0.5f});
        }
        return rig;
    };
    const Rig halves = linked(true);
    const Rig half = linked(false);
    const Rig& rig = halves;
    CHECK(Worst(Frames(halves.document, 1.4f, true)[rig.tip], Frames(half.document, 1.4f, true)[rig.tip]) < 1e-3f);
    // Where the keys switch it on it keeps its place, and then it moves.
    CHECK(Worst(Frames(halves.document, 0.0f, true)[rig.tip], Frames(halves.document, 0.0f, false)[rig.tip]) < 1e-3f);
    const std::vector<Matrix44f> free = Frames(halves.document, 1.4f, false);
    const std::vector<Matrix44f> staged = Frames(halves.document, 1.4f, true);
    CHECK((OriginOf(staged[rig.tip]) - OriginOf(free[rig.tip])).length() > 1.0f);
    // Its carrier stands halfway between the parent and where the target alone
    // would carry it.
    Rig whole;
    whole.key(whole.stage(StageKind::Link, {whole.tip}, {whole.target}), 1, {0.0f}, {1.0f});
    const Matrix44f local = free[rig.tip] * Matrix44f::inverse(free[rig.head]);
    const Matrix44f riding = Matrix44f::inverse(local) * Frames(whole.document, 1.4f, true)[rig.tip];
    const Matrix44f carrier = Matrix44f::inverse(local) * staged[rig.tip];
    CHECK((OriginOf(carrier) - (OriginOf(riding) + OriginOf(free[rig.head])) * 0.5f).length() < 1e-2f);
    CheckBake(halves.document, rig.tip, ProfileId::Wc3Reforged, 0.02f, false);
}

TEST_CASE("wem a Link's shares add up to 1 at most, the first sources first", "[wem][anim][stages]") {
    // The target, then the world.
    const auto tip = [](f32 target, f32 world) {
        Rig rig;
        PoseStage& link = rig.stage(StageKind::Link, {rig.tip}, {rig.target});
        link.sources.push_back(StageSource{2, kInvalidNode, 0.0f});
        rig.key(link, 1, {0.0f}, {target});
        rig.key(link, 2, {0.0f}, {world});
        return Frames(rig.document, 1.4f, true)[rig.tip];
    };
    CHECK(Worst(tip(1.0f, 1.0f), tip(1.0f, 0.0f)) < 1e-4f);
    CHECK(Worst(tip(0.7f, 0.5f), tip(0.7f, 0.3f)) < 1e-4f);
    CHECK(Worst(tip(0.7f, 0.3f), tip(0.7f, 0.0f)) > 1e-2f);
}

TEST_CASE("wem a Link keyed Linear hands its node over without a jump", "[wem][anim][stages]") {
    Rig rig;
    PoseStage& link = rig.stage(StageKind::Link, {rig.tip}, {rig.target});
    link.sources.push_back(StageSource{2, kInvalidNode, 0.0f});
    // From the target to the world over the first second.
    rig.key(link, 1, {0.0f, 1.0f}, {1.0f, 0.0f});
    rig.key(link, 2, {0.0f, 1.0f}, {0.0f, 1.0f});
    for (SubTrack& track : rig.document.clips[0].containers[0].subTracks) {
        if (track.interp == Interpolation::Step) {
            track.interp = Interpolation::Linear;
        }
    }
    const auto at = [&](f32 seconds) { return OriginOf(Frames(rig.document, seconds, true)[rig.tip]); };
    CHECK((at(0.999f) - at(1.0f)).length() < 1.0f);
    CHECK((at(1.0f) - at(1.001f)).length() < 1.0f);
    // Then the world holds it.
    CHECK((at(1.0f) - at(1.5f)).length() < 1e-2f);
}

TEST_CASE("wem a Link to the world holds its node still", "[wem][anim][stages]") {
    Rig rig;
    PoseStage& link = rig.stage(StageKind::Link, {rig.tip}, {});
    link.sources.push_back(StageSource{1, kInvalidNode, 0.0f});
    rig.key(link, 1, {0.0f, 0.5f}, {0.0f, 1.0f});
    const Vector3f held = OriginOf(Frames(rig.document, 0.5f, true)[rig.tip]);
    CHECK((OriginOf(Frames(rig.document, 1.4f, true)[rig.tip]) - held).length() < 1e-3f);
    // Before the switch it swings with its parent.
    CHECK((OriginOf(Frames(rig.document, 0.1f, true)[rig.tip]) - OriginOf(Frames(rig.document, 0.1f, false)[rig.tip]))
              .length() < 1e-4f);
}

TEST_CASE("wem many Links cost a Link each, and each rides as it would alone", "[wem][anim][stages]") {
    // Forty loose bones, each linked to the head: off, switched on at the
    // clip's start, and switched on a second in. Reading a Link's carry runs
    // the stages before it, so worked out afresh a Link doubles the work: the
    // fortieth would be a million million poses.
    const auto linked = [](u32 count, int keyed) {
        Rig rig;
        for (u32 n = 0; n < count; ++n) {
            Node made;
            made.name = "loose " + std::to_string(n);
            made.pivot = Vector3f{static_cast<f32>(n), 3.0f, 12.0f};
            made.kind = NodeKind::Bone;
            const u32 loose = rig.document.models[0].nodes.add(made);
            const PoseStage link = rig.stage(StageKind::Link, {loose}, {rig.head});
            if (keyed == 1) {
                rig.key(link, 1, {0.0f}, {1.0f});
            } else if (keyed == 2) {
                rig.key(link, 1, {0.0f, 1.0f}, {0.0f, 1.0f});
            }
        }
        return rig;
    };
    for (const int keyed : {0, 1, 2}) {
        INFO("keyed " << keyed);
        const Rig many = linked(40, keyed);
        const Rig one = linked(1, keyed);
        const u32 first = one.tip + 1;
        for (const f32 at : {0.0f, 0.4f, 1.0f, 1.6f}) {
            INFO("at " << at);
            const std::vector<Matrix44f> frames = Frames(many.document, at, true);
            // The first of the forty is the one alone.
            CHECK(Worst(frames[first], Frames(one.document, at, true)[first]) < 1e-4f);
            // Off, each stands where the animation has it; on, each keeps its
            // place under the head from its switch on.
            const std::vector<Matrix44f> free = Frames(many.document, at, false);
            const f32 since = keyed == 1 ? 0.0f : 1.0f;
            const std::vector<Matrix44f> then = Frames(many.document, since, false);
            for (u32 n = 0; n < 40; ++n) {
                const u32 loose = first + n;
                if (keyed == 0 || at < since) {
                    CHECK(Worst(frames[loose], free[loose]) < 1e-4f);
                } else {
                    CHECK(Worst(frames[loose] * Matrix44f::inverse(free[many.head]),
                                then[loose] * Matrix44f::inverse(then[many.head])) < 1e-3f);
                }
            }
        }
    }
}

TEST_CASE("wem a Link reads its source as the Links before it leave it", "[wem][anim][stages]") {
    // The tip rides the target, and a loose bone rides the tip: it goes
    // where the first Link takes the tip, not where the animation has it.
    Rig rig;
    Node made;
    made.name = "loose";
    made.pivot = Vector3f{2, 3, 12};
    made.kind = NodeKind::Bone;
    const u32 loose = rig.document.models[0].nodes.add(made);
    const PoseStage first = rig.stage(StageKind::Link, {rig.tip}, {rig.target});
    const PoseStage second = rig.stage(StageKind::Link, {loose}, {rig.tip});
    rig.key(first, 1, {0.0f}, {1.0f});
    rig.key(second, 1, {0.0f, 0.5f}, {0.0f, 1.0f});
    const std::vector<Matrix44f> at = Frames(rig.document, 0.5f, true);
    const std::vector<Matrix44f> later = Frames(rig.document, 1.4f, true);
    CHECK(Worst(at[loose], Frames(rig.document, 0.5f, false)[loose]) < 1e-3f);
    CHECK(Worst(later[loose] * Matrix44f::inverse(later[rig.tip]), at[loose] * Matrix44f::inverse(at[rig.tip])) < 1e-3f);
    // The tip is off where the animation has it by then, and the bone with it.
    CHECK((OriginOf(later[rig.tip]) - OriginOf(Frames(rig.document, 1.4f, false)[rig.tip])).length() > 1.0f);
    CHECK((OriginOf(later[loose]) - OriginOf(Frames(rig.document, 1.4f, false)[loose])).length() > 1.0f);
}

TEST_CASE("wem a Link replays its switches along a host's timeline as along a clip", "[wem][anim][stages]") {
    // The clip handed over as a timeline of the host's own, the same poses and
    // the same keys: the stages leave each node where the mix leaves it. The
    // tip rides the target for a while and a loose bone rides the tip, so the
    // second Link reads the first at a moment of the timeline too.
    Rig rig;
    Node made;
    made.name = "loose";
    made.pivot = Vector3f{2, 3, 12};
    made.kind = NodeKind::Bone;
    const u32 loose = rig.document.models[0].nodes.add(made);
    const PoseStage first = rig.stage(StageKind::Link, {rig.tip}, {rig.target});
    const PoseStage second = rig.stage(StageKind::Link, {loose}, {rig.tip});
    rig.key(first, 1, {0.0f, 0.3f, 1.2f}, {0.0f, 1.0f, 0.0f});
    rig.key(second, 1, {0.0f, 0.5f}, {0.0f, 1.0f});
    const Animator animator(rig.document, 0);
    StageTimeline timeline;
    timeline.sample = [&](f32 seconds, Pose& out) {
        Mix mix;
        mix.plays.push_back(Play{0, seconds, 1.0f, true});
        mix.worldSeconds = seconds;
        animator.sample(mix, out, true);
    };
    timeline.keys = [&](std::span<const u32> channels, f32 seconds) {
        std::set<f32> times;
        for (const u32 id : channels) {
            if (const SubTrack* track = FindSubTrack(rig.document.clips[0], id)) {
                for (const f32 t : track->times) {
                    if (Milliseconds(t) <= Milliseconds(seconds)) {
                        times.insert(t);
                    }
                }
            }
        }
        return std::vector<f32>(times.begin(), times.end());
    };
    const StageRunner runner(rig.document, 0);
    const auto staged = [&](f32 seconds) {
        Pose pose;
        timeline.sample(seconds, pose);
        animator.compose(pose);
        runner.constrain(animator, timeline, seconds, pose);
        return pose.frame;
    };
    for (const f32 at : {0.0f, 0.2f, 0.3f, 0.7f, 1.2f, 1.9f}) {
        INFO("at " << at);
        const std::vector<Matrix44f> frames = staged(at);
        const std::vector<Matrix44f> played = Frames(rig.document, at, true);
        CHECK(Worst(frames[rig.tip], played[rig.tip]) < 1e-4f);
        CHECK(Worst(frames[loose], played[loose]) < 1e-4f);
    }
    // Kept from one pose to the next, whatever order they come in, each
    // switch is replayed once and leaves what it left.
    StageCarries kept;
    timeline.carries = &kept;
    for (const f32 at : {1.9f, 0.7f, 0.2f, 1.2f, 0.7f, 1.9f}) {
        INFO("kept, at " << at);
        const std::vector<Matrix44f> frames = staged(at);
        const std::vector<Matrix44f> played = Frames(rig.document, at, true);
        CHECK(Worst(frames[rig.tip], played[rig.tip]) < 1e-4f);
        CHECK(Worst(frames[loose], played[loose]) < 1e-4f);
    }
    CHECK(kept.offsets.size() == 5u);
    timeline.carries = nullptr;
    // The switches are the timeline's: with none on it the tip rides the
    // target from its rest, not from where it stood 0.3 s in.
    timeline.keys = [](std::span<const u32>, f32) { return std::vector<f32>{}; };
    CHECK((OriginOf(staged(0.7f)[rig.tip]) - OriginOf(Frames(rig.document, 0.7f, true)[rig.tip])).length() > 1.0f);
}

TEST_CASE("wem a constraint reads a held node where it is held, and leaves a held node alone",
          "[wem][anim][stages]") {
    Rig rig;
    // The tip rides the target by a Link that is on.
    PoseStage& link = rig.stage(StageKind::Link, {rig.tip}, {rig.target});
    link.sources[0].weight = 1.0f;
    const Animator animator(rig.document, 0);
    const StageRunner runner(rig.document, 0);
    Mix mix;
    mix.plays.push_back(Play{0, 0.6f, 1.0f, true});
    Pose keys;
    animator.evaluate(mix, keys);
    Pose plain = keys;
    runner.constrain(animator, mix, plain);
    // Something else stands the target 20 units along x: the tip goes with it.
    Matrix44f moved = plain.frame[rig.target];
    moved.data[3][0] += 20.0f;
    const StageHold held[] = {StageHold{rig.target, moved}};
    Pose read = keys;
    runner.constrain(animator, mix, read, StageHolds{held, nullptr});
    CHECK(Worst(read.frame[rig.target], moved) < 1e-3f);
    CHECK((OriginOf(read.frame[rig.tip]) - OriginOf(plain.frame[rig.tip]) - Vector3f{20, 0, 0}).length() < 1e-3f);
    // A held tip is its holder's: the Link does not carry it.
    Matrix44f stood = keys.frame[rig.tip];
    stood.data[3][2] += 7.0f;
    const StageHold own[] = {StageHold{rig.tip, stood}};
    Pose kept = keys;
    runner.constrain(animator, mix, kept, StageHolds{own, nullptr});
    CHECK(Worst(kept.frame[rig.tip], stood) < 1e-3f);
}

TEST_CASE("wem the stages around a step read what the step wrote", "[wem][anim][stages]") {
    Rig rig;
    PoseStage& link = rig.stage(StageKind::Link, {rig.tip}, {rig.target});
    link.sources[0].weight = 1.0f;
    const Animator animator(rig.document, 0);
    const StageRunner runner(rig.document, 0);
    const NodeTree& tree = rig.document.models[0].nodes;
    SimulatedSteps steps;
    SimulatedStages around(rig.document, 0, steps);
    REQUIRE_FALSE(around.empty());
    // A simulation that takes the target and drops it further every step.
    std::vector<u8> simulated(tree.size(), 0);
    simulated[rig.target] = 1;
    Matrix44f left = Matrix44f::identity();
    for (u32 k = 0; k < 4; ++k) {
        INFO("step " << k);
        const f32 seconds = static_cast<f32>(k) / 60.0f;
        Mix mix;
        mix.plays.push_back(Play{0, seconds, 1.0f, true});
        mix.worldSeconds = seconds;
        Pose pose;
        animator.evaluate(mix, pose);
        Pose plain = pose;
        runner.constrain(animator, mix, plain);
        // The tip as the Link carries it by a target standing at a frame.
        const auto carried = [&](const Matrix44f& target) {
            return plain.frame[rig.tip] * Matrix44f::inverse(plain.frame[rig.target]) * target;
        };
        around.before(animator, mix, seconds, pose, simulated);
        // The step runs on the animation's target, and on the tip standing by
        // where the last step left it.
        CHECK(Worst(pose.frame[rig.target], plain.frame[rig.target]) < 1e-3f);
        CHECK(Worst(pose.frame[rig.tip], k == 0 ? plain.frame[rig.tip] : carried(left)) < 1e-3f);
        Matrix44f dropped = plain.frame[rig.target];
        dropped.data[3][2] -= 5.0f * static_cast<f32>(k + 1);
        const Transform local = StageLocalFor(tree, pose, rig.target, dropped);
        pose.local[rig.target].translation = local.translation;
        pose.local[rig.target].rotation = local.rotation;
        animator.compose(pose);
        around.after(animator, mix, seconds, pose, simulated);
        // What it leaves: the target where it wrote it, the tip by it there.
        CHECK(Worst(pose.frame[rig.target], dropped) < 1e-3f);
        CHECK(Worst(pose.frame[rig.tip], carried(dropped)) < 1e-3f);
        left = dropped;
    }
}

TEST_CASE("wem a Link switches at the millisecond its key is read", "[wem][anim][stages]") {
    Rig rig;
    PoseStage& link = rig.stage(StageKind::Link, {rig.tip}, {rig.target});
    link.sources[0].weight = 1.0f;
    // Off the target at 0.517 s. A sample at 31/60 s reads that key, the tracks
    // being read by the millisecond, so the Link has switched there too.
    rig.key(link, 1, {0.0f, 0.517f}, {1.0f, 0.0f});
    const Vector3f before = OriginOf(Frames(rig.document, 0.516f, true)[rig.tip]);
    CHECK((before - OriginOf(Frames(rig.document, 0.516f, false)[rig.tip])).length() > 3.0f);
    CHECK((OriginOf(Frames(rig.document, 31.0f / 60.0f, true)[rig.tip]) - before).length() < 0.5f);
}

TEST_CASE("wem a stage's weight is a node channel no exporter writes", "[wem][anim][stages]") {
    Rig rig;
    rig.stage(StageKind::LookAt, {rig.head}, {rig.target});
    const u32 weight = rig.channel(rig.head, Channel::StageWeight);
    rig.document.models[0].animChannels.find(weight)->target.sub = rig.document.models[0].poseStages[0].id;
    rig.document.models[0].animChannels.find(weight)->valueType = geom::AttrType::F32;
    SubTrack half;
    half.channel = weight;
    half.times = {0.0f};
    Append(half.values, 0.0f);
    rig.document.clips[0].containers[0].subTracks.push_back(half);
    // Weight 0: the stage does nothing.
    CHECK(Worst(Frames(rig.document, 0.5f, true)[rig.head], Frames(rig.document, 0.5f, false)[rig.head]) < 1e-5f);
    // Baked away with its stage.
    Document baked = rig.document;
    Diagnostics report;
    BakeStages(baked, ProfileId::Wc3Reforged, nullptr, report);
    CHECK(baked.models[0].animChannels.find(weight) == nullptr);
}

TEST_CASE("wem a stage that loses its driven node goes with it, and one that loses a source keeps the rest",
          "[wem][anim][stages]") {
    Rig rig;
    rig.stage(StageKind::LookAt, {rig.target}, {rig.head});
    rig.stage(StageKind::Position, {rig.tip}, {rig.target, rig.root});
    Model& model = rig.document.models[0];
    NodeReferencers referencers;
    referencers.channels = &model.animChannels;
    referencers.clips = std::span<Clip>(rig.document.clips);
    referencers.stages = &model.poseStages;
    Diagnostics report;
    RemoveNode(model.nodes, rig.target, RemovePolicy::ReparentChildren, SkinPolicy::Refuse, true, referencers);
    CompactNodes(model.nodes, referencers, report);
    REQUIRE(model.poseStages.size() == 1);
    CHECK(model.poseStages[0].kind == StageKind::Position);
    REQUIRE(model.poseStages[0].sources.size() == 1);
    CHECK(model.poseStages[0].sources[0].node == rig.root);
    CHECK(model.poseStages[0].sources[0].id == 2u);
}

TEST_CASE("wem a physics stage leaves a force field where the animation has it", "[wem][anim][stages]") {
    // A field on the head, which the host's ragdoll turns a quarter turn: the
    // field rides the head's animation, never the ragdoll.
    Rig rig;
    Model& model = rig.document.models[0];
    Node made;
    made.name = "field";
    made.kind = NodeKind::ForceField;
    made.resetPayloadForKind();
    made.parent = rig.head;
    made.pivot = Vector3f{0, 5, 10};
    const u32 field = model.nodes.add(made);
    rig.stage(StageKind::Ragdoll, {rig.head}, {});
    StageHooks hooks;
    hooks.step = [&](const Document&, u32, const PoseStage&, Pose& pose, f32) {
        pose.local[rig.head].rotation = AboutZ(90.0f);
        return true;
    };
    const Animator animator(rig.document, 0);
    Mix mix;
    mix.plays.push_back(Play{0, 0.5f, 1.0f, true});
    mix.worldSeconds = 0.5f;
    Pose pose;
    animator.evaluate(mix, pose);
    const std::vector<Matrix44f> animated = pose.frame;
    StageRunner runner(rig.document, 0);
    runner.run(animator, mix, pose, 0.5f, 0.0f, &hooks);
    REQUIRE(Worst(pose.frame[rig.tip], animated[rig.tip]) > 1.0f);
    CHECK(Worst(pose.frame[field], animated[field]) < 1e-4f);
}

TEST_CASE("wem Validate runs constraints before physics", "[wem][anim][stages]") {
    Rig rig;
    rig.stage(StageKind::Spring, {rig.tip}, {});
    rig.stage(StageKind::LookAt, {rig.head}, {rig.target});
    CHECK(Validate(rig.document, ValidateLevel::Profile).hasErrors());
    std::swap(rig.document.models[0].poseStages[0], rig.document.models[0].poseStages[1]);
    CHECK_FALSE(Validate(rig.document, ValidateLevel::Profile).hasErrors());
}

TEST_CASE("wem pose stages survive a written file, and a MODL v6 reads with none", "[wem][anim][stages]") {
    Rig rig;
    PoseStage& look = rig.stage(StageKind::LookAt, {rig.head}, {rig.target, rig.root});
    look.axis = Vector3f{0, 1, 0};
    look.upNode = rig.tip;
    look.sources[1].weight = 0.4f;
    std::vector<u8> bytes;
    {
        common::vector_streambuf streambuf(bytes);
        std::ostream out(&streambuf);
        common::BinaryWriter writer(out);
        BinaryWriteVisitor visitor(writer);
        visitor.write(rig.document, kCurrentVersion, {});
    }
    common::span_streambuf streambuf(std::span<const u8>(bytes.data(), bytes.size()));
    std::istream in(&streambuf);
    common::BinaryReader reader(in);
    BinaryReadVisitor visitor(reader);
    Document read;
    visitor.read(read, kCurrentVersion);
    REQUIRE(read.models.size() == 1);
    REQUIRE(read.models[0].poseStages.size() == 1);
    const PoseStage& stage = read.models[0].poseStages[0];
    CHECK(stage.kind == StageKind::LookAt);
    CHECK(stage.driven == std::vector<u32>{rig.head});
    REQUIRE(stage.sources.size() == 2);
    CHECK(stage.sources[1].node == rig.root);
    CHECK(stage.sources[1].id == 2u);
    CHECK(stage.sources[1].weight == 0.4f);
    CHECK(stage.upNode == rig.tip);
    CHECK(stage.axis.y == 1.0f);
}
