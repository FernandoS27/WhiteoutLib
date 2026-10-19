// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// Fewer bones for a skinned model (BONE_REDUCTION_RESEARCH.md §7.3).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include <whiteout/models/wem/anim/track_read.h>
#include <whiteout/models/wem/geometry/attributes.h>
#include <whiteout/models/wem/nodes/node.h>
#include <whiteout/models/wem/skinning/reduce.h>

using namespace whiteout;
using namespace whiteout::models::wem;

namespace {

Quaternion About(f32 degrees, const Vector3f& axis) {
    const f32 half = degrees * 3.14159265f / 360.0f;
    return Quaternion(axis.x * std::sin(half), axis.y * std::sin(half), axis.z * std::sin(half), std::cos(half));
}

/// A pivot rig 100 units tall: a hip, a spine that bends, a `still` bone at
/// the spine's top that only rides it, and a `wave` arm off the hip that
/// swings on its own. Each carries a few vertices of its own.
struct Body {
    Document document;
    u32 hip = 0, spine = 0, still = 0, wave = 0;

    explicit Body(const std::string& waveName = "wave") {
        document.models.emplace_back();
        Model& model = document.models.back();
        model.nodes.rig = RigConvention::PivotRelative;
        const auto bone = [&](const char* name, u32 parent, const Vector3f& pivot) {
            Node node;
            node.name = name;
            node.kind = NodeKind::Bone;
            node.parent = parent;
            node.pivot = pivot;
            return model.nodes.add(node);
        };
        hip = bone("hip", kInvalidNode, Vector3f{0, 0, 0});
        spine = bone("spine", hip, Vector3f{0, 0, 40});
        still = bone("still", spine, Vector3f{0, 0, 80});
        wave = bone(waveName.c_str(), hip, Vector3f{20, 0, 20});

        const std::vector<Vector3f> points = {{-5, 0, 0},  {5, 0, 0},   {0, 5, 10},  // hip
                                              {-5, 0, 50}, {5, 0, 50},  {0, 5, 60},  // spine
                                              {-5, 0, 90}, {5, 0, 90},  {0, 5, 100}, // still
                                              {30, 0, 20}, {40, 0, 20}, {35, 5, 25}}; // wave
        geom::FaceSet faces;
        faces.vertexCount = static_cast<u32>(points.size());
        for (u32 t = 0; t < 4; ++t) {
            faces.addTriangle(t * 3, t * 3 + 1, t * 3 + 2);
        }
        Mesh mesh;
        mesh.setFaceSet(faces);
        const std::span<Vector3f> written = mesh.attributes.getOrCreate<Vector3f>(
            geom::names::kPosition, geom::Domain::Vertex, geom::AttrType::F32x3);
        std::copy(points.begin(), points.end(), written.begin());
        mesh.attributes.getOrCreate<u32>(geom::names::kSection, geom::Domain::Face, geom::AttrType::U32);
        mesh.sections.emplace_back();
        const u32 owners[4] = {hip, spine, still, wave};
        mesh.skin.offsets.push_back(0);
        for (u32 v = 0; v < points.size(); ++v) {
            mesh.skin.influences.push_back(geom::Influence{owners[v / 3], 1.0f});
            mesh.skin.offsets.push_back(static_cast<u32>(mesh.skin.influences.size()));
        }
        model.meshes.push_back(std::move(mesh));

        // One clip: the spine bends 40 degrees, the arm swings 90.
        Clip clip;
        clip.name = "bend";
        clip.model = 0;
        clip.duration = 1.0f;
        clip.readRule = ReadRule::Wc3;
        clip.containers.emplace_back();
        for (const auto& [node, degrees] : {std::pair<u32, f32>{spine, 40.0f}, std::pair<u32, f32>{wave, 90.0f}}) {
            AnimChannel channel;
            channel.id = model.animChannels.nextFreeId();
            channel.target.kind = TrackTarget::Kind::Node;
            channel.target.node = node;
            channel.target.channel = Channel::Rotation;
            channel.valueType = geom::AttrType::Quat;
            SubTrack track;
            track.channel = model.animChannels.add(channel);
            track.interp = Interpolation::Linear;
            for (int i = 0; i <= 4; ++i) {
                const f32 t = static_cast<f32>(i) / 4.0f;
                track.times.push_back(t);
                const Quaternion q = About(degrees * t, Vector3f{0, 1, 0});
                const u8* bytes = reinterpret_cast<const u8*>(&q);
                track.values.insert(track.values.end(), bytes, bytes + sizeof(Quaternion));
            }
            clip.containers[0].subTracks.push_back(std::move(track));
        }
        document.clips.push_back(std::move(clip));
    }
};

} // namespace

TEST_CASE("bone reduction drops a bone that only rides its parent, for nothing", "[wem][skinning][reduce]") {
    const Body body;
    skinning::BoneReduceOptions options;
    options.rmsShare = 1e-5f;
    const skinning::BoneReducePlan plan = skinning::PlanBoneReduction(body.document, 0, options);
    REQUIRE(plan.dropped == std::vector<u32>{body.still});
    CHECK(plan.bonesBefore == 4);
    CHECK(plan.bonesAfter == 3);
    CHECK(plan.rms < 1e-3f);
    // Its vertices go wholly to the spine.
    REQUIRE(plan.weights.size() == 1);
    REQUIRE(plan.weights[0].size() == 3);
    for (const auto& [vertex, influences] : plan.weights[0]) {
        CHECK(vertex >= 6);
        CHECK(vertex <= 8);
        REQUIRE(influences.size() == 1);
        CHECK(influences.front().bone == body.spine);
        CHECK(influences.front().weight > 0.999f);
    }
}

TEST_CASE("bone reduction stops at the count asked, cheapest first", "[wem][skinning][reduce]") {
    const Body body;
    skinning::BoneReduceOptions options;
    options.keep = 2;
    const skinning::BoneReducePlan plan = skinning::PlanBoneReduction(body.document, 0, options);
    CHECK(plan.bonesAfter == 2);
    REQUIRE(plan.dropped.size() == 2);
    // The rider first, then the cheaper of the spine (a 40-degree bend near
    // the hip) and the arm (a 90-degree swing out at 20 units).
    CHECK(plan.dropped.front() == body.still);
    CHECK(plan.rms > 0.0f);
    // A parent goes only after its children: the spine after `still`.
    const auto at = [&](u32 node) {
        return std::find(plan.dropped.begin(), plan.dropped.end(), node) - plan.dropped.begin();
    };
    if (std::find(plan.dropped.begin(), plan.dropped.end(), body.spine) != plan.dropped.end()) {
        CHECK(at(body.still) < at(body.spine));
    }
}

TEST_CASE("bone reduction keeps a bone the game finds by name", "[wem][skinning][reduce]") {
    const Body body("bone_hand");
    skinning::BoneReduceOptions options;
    options.keep = 1;
    const skinning::BoneReducePlan plan = skinning::PlanBoneReduction(body.document, 0, options);
    CHECK(std::find(plan.dropped.begin(), plan.dropped.end(), body.wave) == plan.dropped.end());
    CHECK(std::find(plan.dropped.begin(), plan.dropped.end(), body.hip) == plan.dropped.end());
    CHECK(skinning::ReducibleBones(body.document, 0)[body.wave] == 0);
}

TEST_CASE("bone reduction keeps StarCraft II's bones: by name, a billboard, a shadow box, an event's",
          "[wem][skinning][reduce]") {
    CHECK(skinning::ReducibleBones(Body("Bone_Turret 01").document, 0)[3] == 0);
    CHECK(skinning::ReducibleBones(Body("Bone_Leg Left Front").document, 0)[3] == 0);
    CHECK(skinning::ReducibleBones(Body("Bone_Arm").document, 0)[3] == 1);
    {
        Body body;
        body.document.models[0].nodes.nodes[body.still].flags = NodeFlags::BillboardLockZ;
        CHECK(skinning::ReducibleBones(body.document, 0)[body.still] == 0);
    }
    {
        Body body;
        body.document.models[0].nodes.nodes[body.still].sc2Billboards.emplace_back();
        CHECK(skinning::ReducibleBones(body.document, 0)[body.still] == 0);
    }
    {
        Body body;
        body.document.models[0].nodes.nodes[body.still].sc2ShadowBox.emplace();
        CHECK(skinning::ReducibleBones(body.document, 0)[body.still] == 0);
    }
    {
        Body body;
        ClipEvent event;
        event.node = body.still;
        event.name = "Snd_Step";
        body.document.clips[0].events.push_back(event);
        CHECK(skinning::ReducibleBones(body.document, 0)[body.still] == 0);
        CHECK(skinning::ReducibleBones(body.document, 0)[body.wave] == 1);
    }
}

TEST_CASE("bone reduction holds a bone something else names, and one asked to stay", "[wem][skinning][reduce]") {
    Body body;
    // A section riding the spine rigidly names it.
    body.document.models[0].meshes[0].sections[0].rigidNode = body.still;
    skinning::BoneReduceOptions options;
    options.keep = 1;
    options.locked = {body.wave};
    const skinning::BoneReducePlan plan = skinning::PlanBoneReduction(body.document, 0, options);
    CHECK(std::find(plan.dropped.begin(), plan.dropped.end(), body.still) == plan.dropped.end());
    CHECK(std::find(plan.dropped.begin(), plan.dropped.end(), body.wave) == plan.dropped.end());
    // The spine holds `still`, so nothing can go.
    CHECK(plan.dropped.empty());
}

TEST_CASE("bone reduction with no bound leaves every bone", "[wem][skinning][reduce]") {
    const Body body;
    const skinning::BoneReducePlan plan = skinning::PlanBoneReduction(body.document, 0, {});
    CHECK(plan.dropped.empty());
    CHECK(plan.bonesAfter == plan.bonesBefore);
}

TEST_CASE("bone reduction applied takes the bones out with their channels and keys", "[wem][skinning][reduce]") {
    Body body;
    skinning::BoneReduceOptions options;
    options.keep = 2;
    const skinning::BoneReducePlan plan = skinning::PlanBoneReduction(body.document, 0, options);
    REQUIRE(plan.dropped.size() == 2);
    REQUIRE(skinning::ApplyBoneReduction(body.document, 0, plan));
    const Model& model = body.document.models[0];
    CHECK(model.nodes.size() == 2);
    // Every weight names a node that is left, and still sums to one.
    for (u32 v = 0; v < model.meshes[0].skin.vertexCount(); ++v) {
        f32 total = 0.0f;
        for (const geom::Influence& influence : model.meshes[0].skin.forVertex(v)) {
            CHECK(influence.bone < model.nodes.size());
            total += influence.weight;
        }
        CHECK(std::fabs(total - 1.0f) < 1e-4f);
    }
    // No channel aims past the tree, and no key names a channel that went.
    for (const AnimChannel& channel : model.animChannels.channels) {
        if (channel.target.kind == TrackTarget::Kind::Node) {
            CHECK(channel.target.node < model.nodes.size());
        }
    }
    for (const SubTrack& track : body.document.clips[0].containers[0].subTracks) {
        CHECK(std::any_of(model.animChannels.channels.begin(), model.animChannels.channels.end(),
                          [&](const AnimChannel& channel) { return channel.id == track.channel; }));
    }
    CHECK_FALSE(skinning::ApplyBoneReduction(body.document, 0, skinning::BoneReducePlan{}));
}

TEST_CASE("bone reduction tiers loosen from Ultra to Low", "[wem][skinning][reduce]") {
    const skinning::BoneReduceOptions low = skinning::BoneReduceTier(0), ultra = skinning::BoneReduceTier(3);
    CHECK(low.rmsShare > ultra.rmsShare);
    CHECK(low.worstShare > ultra.worstShare);
    CHECK(skinning::BoneReduceTier(9).rmsShare == ultra.rmsShare);
    CHECK(skinning::BoneReduceTier(1, 12).keep == 12);
}
