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

#include <cmath>
#include <cstring>

using namespace whiteout;
using namespace whiteout::models::wem;
using namespace wemfix;

namespace {

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

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
    cameraPayload.target = Vector3f{10, -20, 30};
    cameraPayload.focusDistance = 250.0f;
    cameraPayload.focalLength = 65.0f;
    cameraPayload.fStop = 5.6f;

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
    CHECK_THAT(camera.target.y, WithinAbs(-20.0f * k, 1e-2f)); ///< A point, like a pivot.
    // The game blurs by (1/focus - 1/depth) * focalLength^2 / fStop: the focus is
    // a distance, and the f-stop carries the inverse so the blur holds.
    CHECK_THAT(camera.focusDistance, WithinAbs(250.0f * k, 1e-2f));
    CHECK(camera.focalLength == 65.0f);
    CHECK_THAT(camera.fStop, WithinAbs(5.6f / k, 1e-2f));

    const auto& collision = std::get<CollisionPayload>(model.nodes.nodes[4].payload);
    CHECK_THAT(collision.shape.sphere.radius, WithinAbs(5.0f * k, 1e-2f));
    CHECK_THAT(collision.shape.height, WithinAbs(20.0f * k, 1e-2f));

    // A model 100x as many units long has units a hundredth the size.
    CHECK_THAT(document.unitScale, WithinAbs(0.01f, 1e-6f));
}

TEST_CASE("wem a rescale keeps a light's falloff over the rescaled model", "[wem][rescale]") {
    // `exp(-damping d²) / (1 + linear d + quadratic d²)`: at k times the
    // distance, the same falloff needs each coefficient over k to the power of
    // its term. The shadow range is a pair of distances like the attenuation.
    Document document = makeRescaleDocument();
    Model& model = document.models[0];
    auto& light = std::get<LightPayload>(model.nodes.nodes[2].payload);
    light.shadowCastingStart = 5.0f;
    light.shadowCastingEnd = 50.0f;
    light.quadraticFalloff = 0.02f;
    light.linearFalloff = 0.3f;
    light.damping = 0.004f;

    AnimChannel falloff;
    falloff.id = 900;
    falloff.target.node = 2;
    falloff.target.channel = Channel::QuadraticFalloff;
    falloff.valueType = geom::AttrType::F32;
    model.animChannels.add(falloff);
    SubTrack keys;
    keys.channel = 900;
    keys.interp = Interpolation::Linear;
    keys.times = {0.0f};
    keys.values.resize(sizeof(f32));
    const f32 keyed = 0.02f;
    std::memcpy(keys.values.data(), &keyed, sizeof(f32));
    document.clips[0].containers[0].subTracks.push_back(keys);

    const f32 k = 10.0f;
    REQUIRE(RescaleDocument(document, k).ok);
    CHECK_THAT(light.shadowCastingStart, WithinAbs(5.0f * k, 1e-4f));
    CHECK_THAT(light.shadowCastingEnd, WithinAbs(50.0f * k, 1e-3f));
    CHECK_THAT(light.linearFalloff, WithinAbs(0.3f / k, 1e-7f));
    CHECK_THAT(light.quadraticFalloff, WithinAbs(0.02f / (k * k), 1e-8f));
    CHECK_THAT(light.damping, WithinAbs(0.004f / (k * k), 1e-9f));
    // Where a light of the model sits d from a point, it now sits k d away, and
    // the factor there has not moved.
    const auto factor = [](const LightPayload& l, f32 d) {
        return std::exp(-l.damping * d * d) / (1 + l.linearFalloff * d + l.quadraticFalloff * d * d);
    };
    LightPayload before;
    before.quadraticFalloff = 0.02f;
    before.linearFalloff = 0.3f;
    before.damping = 0.004f;
    CHECK_THAT(factor(light, 7.0f * k), WithinRel(factor(before, 7.0f), 1e-4f));

    const SubTrack& scaled = document.clips[0].containers[0].subTracks.back();
    f32 value = 0;
    std::memcpy(&value, scaled.values.data(), sizeof(f32));
    CHECK_THAT(value, WithinAbs(0.02f / (k * k), 1e-8f));
}

TEST_CASE("wem a rescale carries a clip's move speed", "[wem][rescale]") {
    Document document = makeRescaleDocument();
    SetClipMoveSpeed(document.clips[0], 2.5f);

    REQUIRE(RescaleDocument(document, 100.0f).ok);
    // A length per second, and the second is not a length: the model and the
    // speed it walks at have to move together or its feet slide.
    CHECK(ClipMoveSpeed(document.clips[0]) == 250.0f);

    // A clip whose source stated no travel does not acquire one here.
    Document silent = makeRescaleDocument();
    REQUIRE(RescaleDocument(silent, 100.0f).ok);
    CHECK(silent.clips[0].native.empty());
}

TEST_CASE("wem a move speed keeps StarCraft II's scale, which is Warcraft III's", "[wem][rescale]") {
    // An .m3 Walk at 180 walks at 180 in an .mdx, the model restated 100x.
    CHECK(MoveSpeedUnitsBetween(ProfileId::Sc2, ProfileId::Wc3Reforged) == 0.01f);
    CHECK(MoveSpeedUnitsBetween(ProfileId::Wc3Classic, ProfileId::Heroes) == 100.0f);
    CHECK(MoveSpeedUnitsBetween(ProfileId::Wow, ProfileId::Wc3Reforged) == 1.0f);

    Document document = makeRescaleDocument();
    SetClipMoveSpeed(document.clips[0], 180.0f);
    const f32 factor = RescaleFactorBetween(ProfileId::Sc2, ProfileId::Wc3Reforged);
    REQUIRE(RescaleDocument(document, factor, factor * MoveSpeedUnitsBetween(ProfileId::Sc2, ProfileId::Wc3Reforged)).ok);
    CHECK_THAT(ClipMoveSpeed(document.clips[0]), WithinAbs(180.0f, 1e-3f));
    CHECK_THAT(document.models[0].nodes.nodes[1].local.translation.x, WithinAbs(400.0f, 1e-2f));

    // Two games at one scale whose speeds are not: the speed alone moves.
    Document wow = makeRescaleDocument();
    SetClipMoveSpeed(wow.clips[0], 180.0f);
    REQUIRE(RescaleDocument(wow, 1.0f, MoveSpeedUnitsBetween(ProfileId::Sc2, ProfileId::Wow)).ok);
    CHECK_THAT(ClipMoveSpeed(wow.clips[0]), WithinAbs(1.8f, 1e-3f));
    CHECK(wow.models[0].nodes.nodes[1].local.translation.x == 4.0f);
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
    // A camera's aim is a point in the model; its roll an angle.
    const u32 target = addTrack(model, clip, 1, Channel::Target, {0.0f, 4.0f, 0.0f});
    const u32 roll = addTrack(model, clip, 1, Channel::Roll, {-1.5f});

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
    CHECK_THAT(readTrack(document.clips[0], target)[1], WithinAbs(400.0f, 1e-2f));
    CHECK(readTrack(document.clips[0], roll)[0] == -1.5f);
    CHECK(readTrack(document.clips[0], rotation)[3] == 0.866f);
    CHECK(readTrack(document.clips[0], scaleChannel)[0] == 2.0f);
    CHECK(readTrack(document.clips[0], alpha)[0] == 0.75f);
    CHECK_THAT(document.clips[0].bounds.maximum.z, WithinAbs(200.0f, 1e-2f));

    f32 scaledRest = 0;
    std::memcpy(&scaledRest, document.models[0].animChannels.find(restId)->initValue.data(),
                sizeof(f32));
    CHECK_THAT(scaledRest, WithinAbs(500.0f, 1e-2f));
}

TEST_CASE("wem a rescale scales a TCB track's tangents and not its parameters", "[wem][rescale]") {
    // A Hermite tangent is a length like the value it flanks; tension,
    // continuity and bias have no units, so the curve they chose is the same
    // curve at the new scale (EDIT_MODE_ANIMATIONS_DESIGN.md §3.3).
    Document document = makeRescaleDocument();
    Model& model = document.models[0];
    Clip& clip = document.clips[0];
    const u32 translation = addTrack(model, clip, 1, Channel::Translation, {1.0f, 2.0f, 3.0f});
    SubTrack& track = clip.containers[0].subTracks.back();
    const std::vector<f32> key = {1.0f, 2.0f, 3.0f, 0.5f, 0.0f, 0.0f, 0.25f, 0.0f, 0.0f};
    track.interp = Interpolation::Hermite;
    track.values.resize(key.size() * sizeof(f32));
    std::memcpy(track.values.data(), key.data(), track.values.size());
    const std::vector<f32> tcb = {0.5f, -0.25f, 1.0f};
    track.tcb = tcb;

    REQUIRE(RescaleDocument(document, 100.0f).ok);

    const std::vector<f32> scaled = readTrack(document.clips[0], translation);
    REQUIRE(scaled.size() == 9u);
    CHECK_THAT(scaled[2], WithinAbs(300.0f, 1e-2f));
    CHECK_THAT(scaled[3], WithinAbs(50.0f, 1e-3f));
    CHECK_THAT(scaled[6], WithinAbs(25.0f, 1e-3f));
    CHECK(document.clips[0].containers[0].subTracks.back().tcb == tcb);
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

// ============================================================================
// StarCraft II's records and native streams
// ============================================================================

namespace {

/// A native stream (`TrackTarget::Kind::Native`) under @p animId, keyed with
/// @p values at one time, the way an `.m3` import declares one: its slot, never
/// its field, and the `channel` it defaulted to.
void addNative(Model& model, Clip& clip, u32 animId, u32 slot, geom::AttrType type,
               const std::vector<f32>& values) {
    AnimChannel channel;
    channel.id = animId;
    channel.target.kind = TrackTarget::Kind::Native;
    channel.target.sub = slot;
    channel.valueType = type;
    model.animChannels.add(channel);

    SubTrack track;
    track.channel = animId;
    track.times = {0.0f};
    track.values.resize(values.size() * sizeof(f32));
    std::memcpy(track.values.data(), values.data(), track.values.size());
    clip.containers[0].subTracks.push_back(std::move(track));
}

template <class T>
native::M3AnimRef<T> ref(u32 animId, T init, T null) {
    native::M3AnimRef<T> out;
    out.animId = animId;
    out.initValue = init;
    out.nullValue = null;
    return out;
}

} // namespace

TEST_CASE("wem a rescale restates StarCraft II's records and only their lengths' streams",
          "[wem][rescale]") {
    // A native stream's slot is its value type, so a camera's field of view and
    // its far clip are both an `F32` in slot 5: only the record says which is a
    // length (EDIT_MODE_SC2_SCENE_TOOLS_DESIGN.md F2).
    Document document = makeRescaleDocument();
    Model& model = document.models[0];
    Clip& clip = document.clips[0];

    const u32 camera = addNode(model, "sc2 camera", NodeKind::Camera);
    native::M3Camera cam;
    cam.fieldOfView = ref<f32>(0x100, 0.9f, 0.9f);
    cam.farClip = ref<f32>(0x101, 400.0f, 400.0f);
    cam.nearClip = ref<f32>(0, 0.5f, 0.5f);
    cam.dofAmount = ref<f32>(0x102, 0.25f, 0.0f);
    std::get<CameraPayload>(model.nodes.nodes[camera].payload).sc2 = cam;
    addNative(model, clip, 0x100, 5, geom::AttrType::F32, {1.2f});
    addNative(model, clip, 0x101, 5, geom::AttrType::F32, {600.0f});
    addNative(model, clip, 0x102, 5, geom::AttrType::F32, {0.5f});

    const u32 projector = addNode(model, "sc2 projector", NodeKind::Projector);
    native::M3Projector& proj = std::get<ProjectorPayload>(model.nodes.nodes[projector].payload).record;
    proj.yaw = ref<f32>(0x110, 0.3f, 0.3f);
    proj.offset = ref<Vector3f>(0x111, Vector3f{1, 2, 3}, Vector3f{0, 0, 0});
    proj.far = ref<f32>(0, 50.0f, 50.0f);
    addNative(model, clip, 0x110, 5, geom::AttrType::F32, {0.6f});
    addNative(model, clip, 0x111, 2, geom::AttrType::F32x3, {4, 5, 6});

    Node& bone = model.nodes.nodes[0];
    native::M3ShadowBox box;
    box.length = ref<f32>(0x120, 7.0f, 7.0f);
    box.width = ref<f32>(0, 8.0f, 8.0f);
    bone.sc2ShadowBox = box;
    addNative(model, clip, 0x120, 5, geom::AttrType::F32, {9.0f});

    const u32 light = addNode(model, "sc2 light", NodeKind::Light);
    native::M3Light lite;
    lite.decay = ref<f32>(0x130, 12.0f, 12.0f);
    lite.attenuationEnd = 12.0f;
    lite.specularColor = ref<Vector3f>(0x131, Vector3f{1, 1, 1}, Vector3f{1, 1, 1});
    std::get<LightPayload>(model.nodes.nodes[light].payload).sc2 = lite;
    addNative(model, clip, 0x131, 2, geom::AttrType::F32x3, {0.5f, 0.5f, 0.5f});

    native::M3MeshSection bounds;
    bounds.nodeIndex = 0;
    native::M3Extent extent{Vector3f{-1, -2, -3}, Vector3f{1, 2, 3}, 4.0f};
    bounds.bounds = ref<native::M3Extent>(0x140, extent, extent);
    model.meshes[0].sc2Bounds.push_back(bounds);
    addNative(model, clip, 0x140, 12, geom::AttrType::Extent, {-1, -1, -1, 1, 1, 1, 2});

    native::M3HitTest hit;
    hit.sizeX = 2.0f;
    hit.transform = Matrix44f::identity();
    hit.transform.data[3][0] = 3.0f;
    hit.vertexPositions = {Vector3f{1, 0, 0}};
    std::get<CollisionPayload>(model.nodes.nodes[4].payload).shape.sc2 = hit;

    PoseStage stage;
    stage.kind = StageKind::Sc2LegIk;
    stage.legIk = Sc2LegIkParams{};
    model.poseStages.push_back(stage);

    ClipEvent event;
    event.transform.data[3][2] = 5.0f;
    clip.events.push_back(event);

    REQUIRE(NativeChannelLength(model, 0x101).power == 1);
    REQUIRE(NativeChannelLength(model, 0x100).power == 0);
    REQUIRE(NativeChannelLength(model, 0x140).node == 0);

    constexpr f32 k = 2.0f;
    REQUIRE(RescaleDocument(document, k).ok);

    // Angles, strengths and colours keyed natively are left alone...
    CHECK(readTrack(clip, 0x100) == std::vector<f32>{1.2f});
    CHECK(readTrack(clip, 0x102) == std::vector<f32>{0.5f});
    CHECK(readTrack(clip, 0x110) == std::vector<f32>{0.6f});
    CHECK(readTrack(clip, 0x131) == std::vector<f32>{0.5f, 0.5f, 0.5f});
    // ...and the lengths are restated, a section's bounds as a whole extent.
    CHECK(readTrack(clip, 0x101) == std::vector<f32>{1200.0f});
    // A projector's offset the Art Tools exporter never scales.
    CHECK(readTrack(clip, 0x111) == std::vector<f32>{4, 5, 6});
    CHECK(readTrack(clip, 0x120) == std::vector<f32>{18.0f});
    CHECK(readTrack(clip, 0x140) == std::vector<f32>{-2, -2, -2, 2, 2, 2, 4});

    // The records' own rests and null values, which `toM3` writes back.
    const auto& camOut = *std::get<CameraPayload>(model.nodes.nodes[camera].payload).sc2;
    CHECK(camOut.fieldOfView.initValue == 0.9f);
    CHECK(camOut.farClip.initValue == 800.0f);
    CHECK(camOut.farClip.nullValue == 800.0f);
    CHECK(camOut.nearClip.initValue == 1.0f);
    CHECK(camOut.dofAmount.initValue == 0.25f);
    const auto& projOut = std::get<ProjectorPayload>(model.nodes.nodes[projector].payload).record;
    CHECK(projOut.yaw.initValue == 0.3f);
    CHECK(projOut.far.initValue == 100.0f);
    CHECK(projOut.offset.initValue.z == 3.0f);
    CHECK(model.nodes.nodes[0].sc2ShadowBox->width.nullValue == 16.0f);
    const auto& liteOut = *std::get<LightPayload>(model.nodes.nodes[light].payload).sc2;
    CHECK(liteOut.decay.initValue == 24.0f);
    CHECK(liteOut.attenuationEnd == 24.0f);
    CHECK(liteOut.specularColor.initValue.x == 1.0f);
    const native::M3Extent& extentOut = model.meshes[0].sc2Bounds[0].bounds.nullValue;
    CHECK(extentOut.max.z == 6.0f);
    CHECK(extentOut.radius == 8.0f);
    const auto& hitOut = *std::get<CollisionPayload>(model.nodes.nodes[4].payload).shape.sc2;
    CHECK(hitOut.sizeX == 4.0f);
    CHECK(hitOut.transform.data[3][0] == 6.0f);
    CHECK(hitOut.transform.data[0][0] == 1.0f);
    CHECK(hitOut.vertexPositions[0].x == 2.0f);
    CHECK(model.poseStages[0].legIk->probeUp == 3.0f);
    CHECK(clip.events[0].transform.data[3][2] == 10.0f);
}
