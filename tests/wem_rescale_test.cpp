// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// `RescaleDocument` — §6.2's `sceneScale` as an operation rather than a note.
///
/// The interesting assertions are the two halves of "every length and nothing
/// else": that a rotation, a scale and a UV come out untouched, and that the
/// skinning matrix carries `k * v` to `k * (skin * v)` afterwards. A rescale
/// that got only the vertices would pass a bounds check and pose the model at
/// the wrong place; one that scaled the rotations too would pass a size check
/// and pose it at the wrong angle.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <whiteout/models/wem/retarget.h>

#include "wem_material_fixture.h"

#include <cstring>

using namespace whiteout;
using namespace whiteout::models::wem;
using namespace wemfix;

namespace {

using Catch::Matchers::WithinAbs;

/// Append a node of @p kind with a distinguishable transform, and return its
/// index.
u32 addNode(Model& model, const std::string& name, NodeKind kind, u32 parent = kInvalidNode) {
    Node node;
    node.name = name;
    node.kind = kind;
    node.parent = parent;
    node.resetPayloadForKind();
    node.pivot = Vector3f{1, 2, 3};
    node.local.translation = Vector3f{4, 5, 6};
    // A quarter turn about Z, so a rescale that touched the rotation would be
    // visible as a changed component rather than as a changed length.
    node.local.rotation = Quaternion{0.0f, 0.0f, 0.7071068f, 0.7071068f};
    node.local.scale = Vector3f{2, 2, 2};
    model.nodes.nodes.push_back(std::move(node));
    return static_cast<u32>(model.nodes.nodes.size() - 1);
}

/// One channel of @p channelKind on node @p node, keyed with @p values at one
/// time. Returns the channel id.
u32 addTrack(Model& model, Clip& clip, u32 node, Channel channelKind,
             const std::vector<f32>& values) {
    AnimChannel channel;
    channel.id = model.animChannels.nextFreeId();
    channel.target.kind = TrackTarget::Kind::Node;
    channel.target.node = node;
    channel.target.channel = channelKind;
    channel.valueType =
        values.size() == 3 ? geom::AttrType::F32x3
        : values.size() == 4
            ? (channelKind == Channel::Rotation ? geom::AttrType::Quat : geom::AttrType::F32x4)
            : geom::AttrType::F32;
    model.animChannels.add(channel);

    SubTrack track;
    track.channel = channel.id;
    track.times = {0.0f};
    track.values.resize(values.size() * sizeof(f32));
    std::memcpy(track.values.data(), values.data(), track.values.size());
    clip.containers[0].subTracks.push_back(std::move(track));
    return channel.id;
}

std::vector<f32> readTrack(const Clip& clip, u32 channel) {
    for (const SubTrack& track : clip.containers[0].subTracks) {
        if (track.channel != channel) {
            continue;
        }
        std::vector<f32> out(track.values.size() / sizeof(f32));
        std::memcpy(out.data(), track.values.data(), track.values.size());
        return out;
    }
    return {};
}

/// A document with a mesh, a two-node chain, a light, a camera, a collision
/// shape, and one clip keying a translation and a rotation.
Document makeRescaleDocument() {
    Document document = makeDocument(ProfileId::Sc2);
    document.bounds.minimum = Vector3f{-1, -1, -1};
    document.bounds.maximum = Vector3f{1, 1, 1};
    document.bounds.sphereRadius = 1.7320508f;
    document.unitScale = 1.0f;

    Model& model = document.models[0];
    model.bounds = document.bounds;
    model.meshes[0].recomputeBounds();

    model.nodes.poseSchema.push_back(PoseSchema{"bind", PoseSpace::Model, true, PoseStorage::Trs});
    const u32 root = addNode(model, "root", NodeKind::Bone);
    const u32 child = addNode(model, "child", NodeKind::Bone, root);
    for (u32 index : {root, child}) {
        Node& node = model.nodes.nodes[index];
        node.poses.push_back(
            Transform{Vector3f{7, 8, 9}, Quaternion{0, 0, 0, 1}, Vector3f{1, 1, 1}});
        Matrix44f bind = Matrix44f::identity();
        bind.data[3][0] = 10;
        bind.data[3][1] = 11;
        bind.data[3][2] = 12;
        node.poseMatrices.push_back(bind);
        auto& bone = std::get<BonePayload>(node.payload);
        bone.sphere.center = Vector3f{1, 1, 1};
        bone.sphere.radius = 2.0f;
    }

    const u32 light = addNode(model, "light", NodeKind::Light);
    auto& lightPayload = std::get<LightPayload>(model.nodes.nodes[light].payload);
    lightPayload.attenuationStart = 3.0f;
    lightPayload.attenuationEnd = 30.0f;
    lightPayload.intensity = 4.0f;
    lightPayload.color = Vector3f{1, 0.5f, 0.25f};

    const u32 camera = addNode(model, "camera", NodeKind::Camera);
    auto& cameraPayload = std::get<CameraPayload>(model.nodes.nodes[camera].payload);
    cameraPayload.fov = 0.8f;
    cameraPayload.nearClip = 0.1f;
    cameraPayload.farClip = 500.0f;

    const u32 shape = addNode(model, "shape", NodeKind::CollisionShape);
    auto& collision = std::get<CollisionPayload>(model.nodes.nodes[shape].payload);
    collision.shape.kind = CollisionShapeKind::Capsule;
    collision.shape.sphere.radius = 5.0f;
    collision.shape.height = 20.0f;

    Clip clip;
    clip.name = "walk";
    clip.model = 0;
    clip.duration = 1.0f;
    clip.containers.push_back(SubTrackContainer{});
    document.clips.push_back(std::move(clip));
    return document;
}

} // namespace

// ============================================================================
// The factor
// ============================================================================

TEST_CASE("wem the rescale factor is the two profiles' scene scales", "[wem][rescale]") {
    // §6.2's numbers, read as a ratio. World of Warcraft's 100 is a unit
    // conversion and Diablo III's 17 is a framing constant, and a host putting
    // either on Warcraft III's grid applies the same division.
    CHECK(RescaleFactorBetween(ProfileId::Wow, ProfileId::Wc3Reforged) == 100.0f);
    CHECK(RescaleFactorBetween(ProfileId::Sc2, ProfileId::Wc3Classic) == 100.0f);
    CHECK(RescaleFactorBetween(ProfileId::Diablo3, ProfileId::Wc3Reforged) == 17.0f);
    // The two Warcraft III profiles are one game; a classic model opened as
    // Reforged must not move.
    CHECK(RescaleFactorBetween(ProfileId::Wc3Classic, ProfileId::Wc3Reforged) == 1.0f);
    CHECK(RescaleFactorBetween(ProfileId::Wc3Reforged, ProfileId::Wow) == 0.01f);
    CHECK(RescaleFactorBetween(ProfileId::Count, ProfileId::Wow) == 1.0f);
}

TEST_CASE("wem a rescale factor must be finite and positive", "[wem][rescale]") {
    Document document = makeRescaleDocument();
    const Vector3f before = document.models[0].nodes.nodes[0].local.translation;

    for (f32 bad : {0.0f, -2.0f}) {
        Document copy = document;
        const RescaleResult result = RescaleDocument(copy, bad);
        CHECK_FALSE(result.ok);
        CHECK(result.diagnostics.hasErrors());
        CHECK(copy.models[0].nodes.nodes[0].local.translation.x == before.x);
    }
}

TEST_CASE("wem a rescale by one is a success that changes nothing", "[wem][rescale]") {
    // A pair of profiles at the same scale asks for exactly this, and a caller
    // should not have to special-case it.
    Document document = makeRescaleDocument();
    const Document before = document;
    const RescaleResult result = RescaleDocument(document, 1.0f);
    REQUIRE(result.ok);
    CHECK(result.diagnostics.empty());
    CHECK(document.models[0].nodes.nodes[1].local.translation.y ==
          before.models[0].nodes.nodes[1].local.translation.y);
    CHECK(document.unitScale == before.unitScale);
}

// ============================================================================
// Every length
// ============================================================================

TEST_CASE("wem a rescale moves every length in the document", "[wem][rescale]") {
    Document document = makeRescaleDocument();
    const Document before = document;
    constexpr f32 k = 100.0f;

    const RescaleResult result = RescaleDocument(document, k);
    REQUIRE(result.ok);
    CHECK(result.verticesScaled == before.models[0].meshes[0].vertexCount());
    CHECK(result.nodesScaled == before.models[0].nodes.nodes.size());

    const Model& model = document.models[0];
    const Model& was = before.models[0];

    // Geometry and every bound derived from it.
    const auto positions =
        model.meshes[0].attributes.get<Vector3f>(geom::names::kPosition, geom::Domain::Vertex);
    const auto wasPositions =
        was.meshes[0].attributes.get<Vector3f>(geom::names::kPosition, geom::Domain::Vertex);
    REQUIRE(positions.size() == wasPositions.size());
    for (std::size_t i = 0; i < positions.size(); ++i) {
        CHECK_THAT(positions[i].x, WithinAbs(wasPositions[i].x * k, 1e-3f));
    }
    CHECK_THAT(document.bounds.sphereRadius, WithinAbs(before.bounds.sphereRadius * k, 1e-3f));
    CHECK_THAT(model.meshes[0].bounds.maximum.x,
               WithinAbs(was.meshes[0].bounds.maximum.x * k, 1e-3f));
    CHECK_THAT(model.meshes[0].sections[1].bounds.maximum.x,
               WithinAbs(was.meshes[0].sections[1].bounds.maximum.x * k, 1e-3f));

    // Nodes: pivot, local translation, both bind pose forms.
    const Node& node = model.nodes.nodes[1];
    CHECK_THAT(node.pivot.z, WithinAbs(3.0f * k, 1e-2f));
    CHECK_THAT(node.local.translation.x, WithinAbs(4.0f * k, 1e-2f));
    CHECK_THAT(node.poses[0].translation.y, WithinAbs(8.0f * k, 1e-2f));
    CHECK_THAT(node.poseMatrices[0].data[3][2], WithinAbs(12.0f * k, 1e-2f));
    CHECK_THAT(std::get<BonePayload>(node.payload).sphere.radius, WithinAbs(2.0f * k, 1e-2f));

    // Payload lengths, and only those.
    const auto& light = std::get<LightPayload>(model.nodes.nodes[2].payload);
    CHECK_THAT(light.attenuationStart, WithinAbs(3.0f * k, 1e-2f));
    CHECK_THAT(light.attenuationEnd, WithinAbs(30.0f * k, 1e-2f));
    CHECK(light.intensity == 4.0f);
    CHECK(light.color.y == 0.5f);

    const auto& camera = std::get<CameraPayload>(model.nodes.nodes[3].payload);
    CHECK(camera.fov == 0.8f); ///< An angle, not a distance.
    CHECK_THAT(camera.nearClip, WithinAbs(0.1f * k, 1e-2f));
    CHECK_THAT(camera.farClip, WithinAbs(500.0f * k, 1e-1f));

    const auto& collision = std::get<CollisionPayload>(model.nodes.nodes[4].payload);
    CHECK_THAT(collision.shape.sphere.radius, WithinAbs(5.0f * k, 1e-2f));
    CHECK_THAT(collision.shape.height, WithinAbs(20.0f * k, 1e-2f));

    // A model 100x as many units long has units a hundredth the size.
    CHECK_THAT(document.unitScale, WithinAbs(0.01f, 1e-6f));
}

TEST_CASE("wem a rescale leaves rotation and scale alone", "[wem][rescale]") {
    // The half a size check cannot see. A rescale that multiplied the whole
    // `Transform` would leave the model the right size and every joint at the
    // wrong angle.
    Document document = makeRescaleDocument();
    const Transform before = document.models[0].nodes.nodes[1].local;

    REQUIRE(RescaleDocument(document, 17.0f).ok);

    const Transform& after = document.models[0].nodes.nodes[1].local;
    CHECK(after.rotation.z == before.rotation.z);
    CHECK(after.rotation.w == before.rotation.w);
    CHECK(after.scale.x == before.scale.x);
    // The bind pose's 3x3 is a rotation-scale and is dimensionless too, so only
    // its translation row moved.
    CHECK(document.models[0].nodes.nodes[1].poseMatrices[0].data[0][0] == 1.0f);
}

// ============================================================================
// Animation
// ============================================================================

TEST_CASE("wem a rescale moves translation keys and no others", "[wem][rescale]") {
    Document document = makeRescaleDocument();
    Model& model = document.models[0];
    Clip& clip = document.clips[0];

    const u32 translation = addTrack(model, clip, 1, Channel::Translation, {1.0f, 2.0f, 3.0f});
    const u32 rotation = addTrack(model, clip, 1, Channel::Rotation, {0.0f, 0.0f, 0.5f, 0.866f});
    const u32 scaleChannel = addTrack(model, clip, 1, Channel::Scale, {2.0f, 2.0f, 2.0f});
    const u32 alpha = addTrack(model, clip, 1, Channel::Alpha, {0.75f});
    const u32 attenuation = addTrack(model, clip, 2, Channel::AttenuationEnd, {30.0f});

    // An un-keyed channel's rest value is a value of the channel like any other.
    AnimChannel rest;
    rest.id = model.animChannels.nextFreeId();
    rest.target.kind = TrackTarget::Kind::Node;
    rest.target.node = 1;
    rest.target.channel = Channel::Translation;
    rest.valueType = geom::AttrType::F32x3;
    rest.initValue.resize(3 * sizeof(f32));
    const f32 restValue[3] = {5.0f, 0.0f, 0.0f};
    std::memcpy(rest.initValue.data(), restValue, sizeof(restValue));
    const u32 restId = model.animChannels.add(rest);

    clip.bounds.maximum = Vector3f{2, 2, 2};

    REQUIRE(RescaleDocument(document, 100.0f).ok);

    CHECK_THAT(readTrack(document.clips[0], translation)[2], WithinAbs(300.0f, 1e-2f));
    CHECK_THAT(readTrack(document.clips[0], attenuation)[0], WithinAbs(3000.0f, 1e-1f));
    CHECK(readTrack(document.clips[0], rotation)[3] == 0.866f);
    CHECK(readTrack(document.clips[0], scaleChannel)[0] == 2.0f);
    CHECK(readTrack(document.clips[0], alpha)[0] == 0.75f);
    CHECK_THAT(document.clips[0].bounds.maximum.z, WithinAbs(200.0f, 1e-2f));

    f32 scaledRest = 0;
    std::memcpy(&scaledRest, document.models[0].animChannels.find(restId)->initValue.data(),
                sizeof(f32));
    CHECK_THAT(scaledRest, WithinAbs(500.0f, 1e-2f));
}

// ============================================================================
// The invariant the whole thing exists for
// ============================================================================

TEST_CASE("wem a rescale is a conjugation, so skinning still lands", "[wem][rescale]") {
    // `skin(b) = inverseBind(b) * world(b)`, and a rescale is exact when it
    // carries `k * v` to `k * (skin * v)` for every bone. Composed here down a
    // two-node chain carrying a rotation and a non-unit scale, which is where a
    // naive "multiply the translations" would go wrong if `K` did not commute
    // with the linear part.
    Document document = makeRescaleDocument();
    const Model& was = document.models[0];
    const Transform worldBefore = Compose(was.nodes.nodes[0].local, was.nodes.nodes[1].local);
    const Vector3f point{0.3f, -0.7f, 1.1f};
    const Vector3f posedBefore = TransformPoint(worldBefore, point);

    constexpr f32 k = 100.0f;
    REQUIRE(RescaleDocument(document, k).ok);

    const Model& model = document.models[0];
    const Transform worldAfter = Compose(model.nodes.nodes[0].local, model.nodes.nodes[1].local);
    const Vector3f posedAfter =
        TransformPoint(worldAfter, Vector3f{point.x * k, point.y * k, point.z * k});

    CHECK_THAT(posedAfter.x, WithinAbs(posedBefore.x * k, 1e-1f));
    CHECK_THAT(posedAfter.y, WithinAbs(posedBefore.y * k, 1e-1f));
    CHECK_THAT(posedAfter.z, WithinAbs(posedBefore.z * k, 1e-1f));
}
