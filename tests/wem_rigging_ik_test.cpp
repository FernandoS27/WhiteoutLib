// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// The IK solvers (`rigging/ik.h`), pinned on fixtures before a pose stage
/// wraps them (WEM_ANIMATION_RUNTIME_PLAN.md §10), and G-C's IK half: a limb
/// stage reaching a keyed goal, baked, plays what the stage played.

#include <cmath>
#include <cstring>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/wem/anim/animator.h>
#include <whiteout/models/wem/anim/stages.h>
#include <whiteout/models/wem/document.h>
#include <whiteout/models/wem/rigging/ik.h>

using namespace whiteout;
using namespace whiteout::models;
using namespace whiteout::models::wem;

namespace {

f32 Distance(const Vector3f& a, const Vector3f& b) {
    return (a - b).length();
}

template <class T>
void Append(std::vector<u8>& bytes, const T& value) {
    const u8* at = reinterpret_cast<const u8*>(&value);
    bytes.insert(bytes.end(), at, at + sizeof(T));
}

Vector3f OriginOf(const Matrix44f& m) {
    return Vector3f{m.data[3][0], m.data[3][1], m.data[3][2]};
}

} // namespace

TEST_CASE("wem SolveLimb lands the end on a goal in reach and keeps the bones' lengths",
          "[wem][rigging][ik]") {
    LimbPress press;
    press.upper = Vector3f{0, 0, 20};
    press.lower = Vector3f{0, 2, 10};
    press.end = Vector3f{0, 0, 0};
    LimbSetup setup;
    setup.hinge = cross(press.lower - press.upper, press.end - press.lower).normalized();
    IkGoal goal;
    goal.position = Vector3f{3, 4, 6};
    const LimbSolve solve = SolveLimb(press, setup, goal);
    REQUIRE(solve.ok);
    CHECK(solve.reached);
    CHECK(Distance(solve.end, goal.position) < 1e-3f);
    CHECK(std::fabs(Distance(solve.lower, press.upper) - Distance(press.lower, press.upper)) < 1e-3f);
    CHECK(std::fabs(Distance(solve.end, solve.lower) - Distance(press.end, press.lower)) < 1e-3f);
    // The turns do what the points say: the upper turned about its pivot takes
    // the lower where the solve put it.
    CHECK(Distance(press.upper + Rotate(solve.upperDelta, press.lower - press.upper), solve.lower) < 1e-3f);

    // Out of reach: pointing there, short of it.
    goal.position = Vector3f{0, 0, -30};
    const LimbSolve far = SolveLimb(press, setup, goal);
    REQUIRE(far.ok);
    CHECK_FALSE(far.reached);
    CHECK(Distance(far.end, goal.position) > 1.0f);
}

TEST_CASE("wem SolveChain reaches a goal and keeps every segment's length", "[wem][rigging][ik]") {
    const std::vector<Vector3f> press = {Vector3f{0, 0, 0}, Vector3f{0, 0, 5}, Vector3f{0, 1, 10},
                                         Vector3f{0, 3, 14}};
    IkGoal goal;
    goal.position = Vector3f{6, 2, 9};
    const ChainSolve solve = SolveChain(press, Quaternion{0, 0, 0, 1}, goal);
    REQUIRE(solve.ok);
    REQUIRE(solve.points.size() == press.size());
    CHECK(solve.reached);
    CHECK(Distance(solve.points.back(), goal.position) < 1e-2f);
    for (std::size_t k = 0; k + 1 < press.size(); ++k) {
        CHECK(std::fabs(Distance(solve.points[k + 1], solve.points[k]) - Distance(press[k + 1], press[k])) < 1e-3f);
    }
    CHECK(Distance(solve.points.front(), press.front()) < 1e-5f);
}

TEST_CASE("wem a limb IK stage reaches its goal and bakes to the same pose", "[wem][anim][stages][ik]") {
    // A pivot rig: hip, knee, ankle down a bent leg, and a goal helper that
    // travels a loop in front of it.
    Document document;
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
    const u32 hip = node("hip", kInvalidNode, Vector3f{0, 0, 20}, NodeKind::Bone);
    const u32 knee = node("knee", hip, Vector3f{0, 2, 10}, NodeKind::Bone);
    const u32 ankle = node("ankle", knee, Vector3f{0, 0, 0}, NodeKind::Bone);
    const u32 goal = node("goal", kInvalidNode, Vector3f{0, 0, 0}, NodeKind::Helper);
    AnimChannel channel;
    channel.id = 1;
    channel.target.kind = TrackTarget::Kind::Node;
    channel.target.node = goal;
    channel.target.channel = Channel::Translation;
    channel.valueType = geom::AttrType::F32x3;
    model.animChannels.add(channel);
    Clip clip;
    clip.name = "Walk";
    clip.model = 0;
    clip.duration = 1.0f;
    clip.looping = true;
    SubTrack path;
    path.channel = 1;
    for (int k = 0; k <= 4; ++k) {
        const f32 a = static_cast<f32>(k) / 4.0f * 6.2831853f;
        path.times.push_back(static_cast<f32>(k) * 0.25f);
        Append(path.values, Vector3f{3.0f * std::cos(a), 2.0f + 3.0f * std::sin(a), 4.0f});
    }
    clip.containers.emplace_back().subTracks.push_back(path);
    document.clips.push_back(clip);
    PoseStage stage;
    stage.id = 1;
    stage.name = "leg";
    stage.kind = StageKind::LimbIk;
    stage.driven = {hip, knee, ankle};
    stage.targets = {goal};
    model.poseStages.push_back(stage);

    const auto frames = [&](const Document& doc, f32 at, bool staged) {
        const Animator animator(doc, 0);
        Mix mix;
        mix.plays.push_back(Play{0, at, 1.0f, true});
        Pose pose;
        animator.evaluate(mix, pose);
        if (staged) {
            StageRunner(doc, 0).constrain(animator, mix, pose);
        }
        return pose.frame;
    };
    for (const f32 at : {0.0f, 0.3f, 0.61f}) {
        const std::vector<Matrix44f> posed = frames(document, at, true);
        CHECK(Distance(OriginOf(posed[ankle]), OriginOf(posed[goal])) < 1e-2f);
    }
    for (const ProfileId target : {ProfileId::Wc3Reforged, ProfileId::Sc2}) {
        Document baked = document;
        Diagnostics report;
        BakeStages(baked, target, nullptr, report);
        REQUIRE(baked.models[0].poseStages.empty());
        for (const f32 at : {0.0f, 0.13f, 0.3f, 0.61f, 0.88f}) {
            INFO("at " << at);
            const std::vector<Matrix44f> want = frames(document, at, true);
            const std::vector<Matrix44f> have = frames(baked, at, false);
            for (const u32 joint : {hip, knee, ankle}) {
                CHECK(Distance(OriginOf(want[joint]), OriginOf(have[joint])) < 0.05f);
            }
        }
        // Seamless: the goal loops, so the leg does.
        const std::vector<Matrix44f> first = frames(baked, 0.0f, false);
        const std::vector<Matrix44f> last = frames(baked, 1.0f, false);
        CHECK(Distance(OriginOf(first[ankle]), OriginOf(last[ankle])) < 0.05f);
    }
}
