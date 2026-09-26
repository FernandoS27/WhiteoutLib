// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// G-C's physics half (WEM_ANIMATION_RUNTIME_PLAN.md §11): a spring stage
/// steps at 60 a second whatever the preview's frame rate, a scrub lands where
/// a straight run does, and a baked loop has no seam. A ragdoll or cloth stage
/// with no host is reported and not baked.

#include <cmath>
#include <cstring>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/wem/anim/animator.h>
#include <whiteout/models/wem/anim/stages.h>
#include <whiteout/models/wem/document.h>

using namespace whiteout;
using namespace whiteout::models;
using namespace whiteout::models::wem;

namespace {

template <class T>
void Append(std::vector<u8>& bytes, const T& value) {
    const u8* at = reinterpret_cast<const u8*>(&value);
    bytes.insert(bytes.end(), at, at + sizeof(T));
}

Vector3f OriginOf(const Matrix44f& m) {
    return Vector3f{m.data[3][0], m.data[3][1], m.data[3][2]};
}

/// A root swinging side to side and a tip on a spring under it.
struct Jiggle {
    Document document;
    u32 root = 0, tip = 1;

    Jiggle() {
        document.profiles = {ProfileId::Wc3Reforged};
        document.defaultProfile = ProfileId::Wc3Reforged;
        Model& model = document.models.emplace_back();
        Node a;
        a.name = "root";
        a.kind = NodeKind::Bone;
        root = model.nodes.add(a);
        Node b;
        b.name = "tip";
        b.parent = root;
        b.pivot = Vector3f{0, 0, 10};
        b.kind = NodeKind::Bone;
        tip = model.nodes.add(b);
        AnimChannel channel;
        channel.id = 1;
        channel.target.kind = TrackTarget::Kind::Node;
        channel.target.node = root;
        channel.target.channel = Channel::Translation;
        channel.valueType = geom::AttrType::F32x3;
        model.animChannels.add(channel);
        Clip clip;
        clip.name = "Stand";
        clip.model = 0;
        clip.duration = 2.0f;
        clip.looping = true;
        SubTrack swing;
        swing.channel = 1;
        swing.times = {0.0f, 0.5f, 1.0f, 1.5f, 2.0f};
        for (const f32 x : {0.0f, 8.0f, 0.0f, -8.0f, 0.0f}) {
            Append(swing.values, Vector3f{x, 0, 0});
        }
        clip.containers.emplace_back().subTracks.push_back(swing);
        document.clips.push_back(clip);
        PoseStage spring;
        spring.id = 1;
        spring.name = "jiggle";
        spring.kind = StageKind::Spring;
        spring.driven = {tip};
        spring.stiffness = 0.05f;
        spring.damping = 0.1f;
        model.poseStages.push_back(spring);
    }

    /// The tip's origin after running @p runner at each of @p times in turn.
    Vector3f run(StageRunner& runner, const std::vector<f32>& times) const {
        const Animator animator(document, 0);
        Vector3f at{0, 0, 0};
        for (const f32 t : times) {
            Mix mix;
            mix.plays.push_back(Play{0, t, 1.0f, true});
            mix.worldSeconds = t;
            Pose pose;
            animator.evaluate(mix, pose);
            runner.run(animator, mix, pose, t, document.clips[0].duration, nullptr);
            at = OriginOf(pose.frame[tip]);
        }
        return at;
    }
};

std::vector<f32> Frames(f32 rate, f32 until) {
    std::vector<f32> out;
    for (f32 t = 0.0f; t <= until + 1e-6f; t += 1.0f / rate) {
        out.push_back(std::min(t, until));
    }
    if (out.back() < until) {
        out.push_back(until);
    }
    return out;
}

} // namespace

TEST_CASE("wem a spring steps the same at any preview frame rate", "[wem][anim][stages][physics]") {
    const Jiggle jiggle;
    StageRunner slow(jiggle.document, 0);
    StageRunner fast(jiggle.document, 0);
    const Vector3f a = jiggle.run(slow, Frames(30.0f, 1.3f));
    const Vector3f b = jiggle.run(fast, Frames(144.0f, 1.3f));
    CHECK((a - b).length() < 1e-4f);
    // And it lags: the tip is not where the swing alone puts it.
    const Animator animator(jiggle.document, 0);
    Mix mix;
    mix.plays.push_back(Play{0, 1.3f, 1.0f, true});
    Pose pose;
    animator.evaluate(mix, pose);
    CHECK((OriginOf(pose.frame[jiggle.tip]) - a).length() > 0.1f);
}

TEST_CASE("wem scrubbing a spring back lands where a straight run does", "[wem][anim][stages][physics]") {
    const Jiggle jiggle;
    StageRunner straight(jiggle.document, 0);
    const Vector3f want = jiggle.run(straight, Frames(60.0f, 1.2f));
    StageRunner scrubbed(jiggle.document, 0);
    jiggle.run(scrubbed, Frames(60.0f, 1.9f));
    const Vector3f have = jiggle.run(scrubbed, {1.2f});
    CHECK((want - have).length() < 1e-4f);
}

TEST_CASE("wem a baked spring loop has no seam", "[wem][anim][stages][physics]") {
    const Jiggle jiggle;
    Document baked = jiggle.document;
    Diagnostics report;
    BakeStages(baked, ProfileId::Wc3Reforged, nullptr, report);
    REQUIRE(baked.models[0].poseStages.empty());
    CHECK(report.countOf(DiagCode::AnimStageBaked) == 1u);
    const auto tipAt = [&](f32 t) {
        const Animator animator(baked, 0);
        Mix mix;
        mix.plays.push_back(Play{0, t, 1.0f, true});
        Pose pose;
        animator.evaluate(mix, pose);
        return OriginOf(pose.frame[jiggle.tip]);
    };
    CHECK((tipAt(0.0f) - tipAt(2.0f)).length() < 1e-3f);
}

TEST_CASE("wem a ragdoll with no host is reported and not baked", "[wem][anim][stages][physics]") {
    Jiggle jiggle;
    jiggle.document.models[0].poseStages[0].kind = StageKind::Ragdoll;
    Diagnostics report;
    BakeStages(jiggle.document, ProfileId::Wc3Reforged, nullptr, report);
    CHECK(report.countOf(DiagCode::AnimStageNotBaked) == 1u);
    CHECK(report.hasErrors());
    CHECK(report.countOf(DiagCode::AnimStageBaked) == 0u);
}

TEST_CASE("wem a hosted stage bakes every clip from rest", "[wem][anim][stages][physics]") {
    // Two clips: the host is told to forget its bodies before each, so the
    // second does not start where the first left them.
    Jiggle jiggle;
    jiggle.document.models[0].poseStages[0].kind = StageKind::Ragdoll;
    Clip walk = jiggle.document.clips[0];
    walk.name = "Walk";
    jiggle.document.clips.push_back(walk);
    std::vector<u32> resets;
    StageHooks hooks;
    hooks.step = [](const Document&, u32, const PoseStage&, Pose&, f32) { return true; };
    hooks.reset = [&](u32 model, const PoseStage& stage) {
        CHECK(model == 0u);
        resets.push_back(stage.id);
    };
    Diagnostics report;
    BakeStages(jiggle.document, ProfileId::Wc3Reforged, &hooks, report);
    CHECK(resets.size() >= 2u);
    CHECK(report.countOf(DiagCode::AnimStageNotBaked) == 0u);
}
