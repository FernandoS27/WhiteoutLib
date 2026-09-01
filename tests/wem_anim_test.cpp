// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// WEM v3 P7 — the animation containers, without a converter in sight.
///
/// What is worth asserting here is not that the structs hold values. It is the
/// three places the id indirection earns its keep, all of which are silent
/// failures if they are wrong:
///
/// - a channel's key stream is sized by the **channel's** type, so a converter
///   that keys the wrong width produces bytes nothing can read;
/// - removing a node invalidates its channels rather than dropping them, so the
///   sub-tracks that joined on them stay distinguishable from a merge that has
///   not landed;
/// - removing a layer shifts an **ordinal** and removing a feature kills an
///   **id**, and the target kind is what tells those apart.

#include <cstring>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/wem/anim/clip.h>
#include <whiteout/models/wem/document.h>
#include <whiteout/models/wem/materials/ops.h>
#include <whiteout/models/wem/nodes/remove.h>
#include <whiteout/models/wem/validate.h>

using namespace whiteout;
using namespace whiteout::models;
using namespace whiteout::models::wem;

namespace {

template <class T>
std::vector<u8> bytesOf(const T& value) {
    std::vector<u8> bytes(sizeof(T));
    std::memcpy(bytes.data(), &value, sizeof(T));
    return bytes;
}

std::string codes(const Diagnostics& report) {
    std::string joined;
    for (const Diagnostic& issue : report.all()) {
        if (issue.severity < Severity::Warning) {
            continue;
        }
        if (!joined.empty()) {
            joined += ',';
        }
        joined += ToString(issue.code);
    }
    return joined;
}

/// A model with three bones, one material slot bound in one profile, and a
/// three-stage material — enough for every referencer row to have something to
/// point at.
Model makeModel() {
    Model model;
    model.name = "rig";
    model.materialSlots.push_back("body");

    model.nodes.add(Node{});
    model.nodes.nodes[0].name = "root";
    model.nodes.nodes[0].kind = NodeKind::Bone;

    Node child;
    child.name = "spine";
    child.kind = NodeKind::Bone;
    child.parent = 0;
    model.nodes.add(child);

    Node event;
    event.name = "hit";
    event.kind = NodeKind::Event;
    event.parent = 1;
    model.nodes.add(event);

    ProfileMaterialSet set;
    set.profile = ProfileId::Wc3Classic;
    set.looks.add("A", 0);
    Material material;
    CommonMaterial& common = material.InitCommon();
    common.setKind(MaterialKind::Combiners);
    CombinersBody& body = *common.combiners();
    body.stages.resize(3);
    common.features.push_back(MaterialFeature{5, 0, UvAnimationFeature{}});
    set.materials.push_back(std::move(material));
    set.resizeBindings(model.materialSlots.size());
    set.slotBindings[0].byLook[0] = 0;
    model.profileSets.push_back(std::move(set));

    return model;
}

/// The three target kinds, ids 1/2/3, over `makeModel`'s shape.
void addChannels(Model& model) {
    AnimChannel node;
    node.id = 1;
    node.target.kind = TrackTarget::Kind::Node;
    node.target.node = 1;
    node.target.channel = Channel::Translation;
    node.valueType = geom::AttrType::F32x3;
    model.animChannels.add(node);

    AnimChannel layer;
    layer.id = 2;
    layer.target.kind = TrackTarget::Kind::MaterialLayer;
    layer.target.material.profile = ProfileId::Wc3Classic;
    layer.target.material.slot = 0;
    layer.target.sub = 2; // the last stage
    layer.target.channel = Channel::Alpha;
    layer.valueType = geom::AttrType::F32;
    model.animChannels.add(layer);

    AnimChannel feature;
    feature.id = 3;
    feature.target.kind = TrackTarget::Kind::MaterialFeature;
    feature.target.material.profile = ProfileId::Wc3Classic;
    feature.target.material.slot = 0;
    feature.target.sub = 5; // the feature's id
    feature.target.channel = Channel::UvTranslate;
    feature.valueType = geom::AttrType::F32x3;
    model.animChannels.add(feature);
}

Document makeDocument() {
    Document document;
    document.declare(ProfileId::Wc3Classic);
    document.defaultProfile = ProfileId::Wc3Classic;

    Model model = makeModel();
    addChannels(model);
    document.models.push_back(std::move(model));

    Clip clip;
    clip.name = "Stand";
    clip.model = 0;
    clip.duration = 1.0f;

    SubTrackContainer container;
    container.name = "base";
    SubTrack track;
    track.channel = 1;
    track.interp = Interpolation::Linear;
    track.times = {0.0f, 1.0f};
    track.values = bytesOf(Vector3f{0, 0, 0});
    const std::vector<u8> second = bytesOf(Vector3f{1, 0, 0});
    track.values.insert(track.values.end(), second.begin(), second.end());
    container.subTracks.push_back(std::move(track));
    clip.containers.push_back(std::move(container));

    clip.events.push_back(ClipEvent{0.5f, 2, "step", 0});
    document.clips.push_back(std::move(clip));
    return document;
}

} // namespace

// ============================================================================
// The table
// ============================================================================

TEST_CASE("wem channel ids are the join key, not positions", "[wem][anim]") {
    Model model = makeModel();
    addChannels(model);
    AnimChannelTable& table = model.animChannels;

    REQUIRE(table.channels.size() == 3u);
    CHECK(table.indexOf(2) == 1u);
    CHECK(table.indexOf(99) == kInvalidIndex);
    REQUIRE(table.find(3) != nullptr);
    CHECK(table.find(3)->target.channel == Channel::UvTranslate);
    CHECK(table.find(99) == nullptr);

    // An id is never reused, so the next free one is past the largest even after
    // the middle of the table has gone.
    CHECK(table.nextFreeId() == 4u);
    table.channels.erase(table.channels.begin() + 1);
    CHECK(table.nextFreeId() == 4u);
}

TEST_CASE("wem a hermite key is three values wide", "[wem][anim]") {
    // The one place `SubTrack`'s layout is not obvious: the tangents share the
    // key stream rather than living in vectors of their own, which is MDX's own
    // layout and what makes that import a copy.
    CHECK(ValuesPerKey(Interpolation::Step) == 1u);
    CHECK(ValuesPerKey(Interpolation::Linear) == 1u);
    CHECK(ValuesPerKey(Interpolation::Slerp) == 1u);
    CHECK(ValuesPerKey(Interpolation::Hermite) == 3u);
    CHECK(ValuesPerKey(Interpolation::Bezier) == 3u);

    SubTrack track;
    track.interp = Interpolation::Hermite;
    track.times = {0.0f, 1.0f};
    track.values.resize(2 * 3 * sizeof(f32));
    CHECK(track.wellSized(geom::AttrType::F32));
    CHECK_FALSE(track.wellSized(geom::AttrType::F32x3));

    track.interp = Interpolation::Linear;
    CHECK_FALSE(track.wellSized(geom::AttrType::F32));
}

// ============================================================================
// Validation
// ============================================================================

TEST_CASE("wem a clean animated document validates", "[wem][anim][validate]") {
    const Document document = makeDocument();
    CHECK(codes(Validate(document, ValidateLevel::Profile)) == "");
}

TEST_CASE("wem two channels may not share an id", "[wem][anim][validate]") {
    Document document = makeDocument();
    document.models[0].animChannels.channels[2].id = 1;
    CHECK(codes(Validate(document, ValidateLevel::Structural)) == "MaterialBodyInvalid");
}

TEST_CASE("wem a sub-track is sized by its channel's type", "[wem][anim][validate]") {
    Document document = makeDocument();
    // Three floats per key, against a channel that says one — the defect a green
    // conversion hides, because nothing else in the document disagrees.
    document.models[0].animChannels.channels[0].valueType = geom::AttrType::F32;
    CHECK(codes(Validate(document, ValidateLevel::Structural)) == "AttributeCountMismatch");
}

TEST_CASE("wem a sub-track must join on a declared channel", "[wem][anim][validate]") {
    Document document = makeDocument();
    document.clips[0].containers[0].subTracks[0].channel = 77;
    CHECK(codes(Validate(document, ValidateLevel::Structural)) == "AnimChannelInvalidated");
}

TEST_CASE("wem a clip names the model whose channels it drives", "[wem][anim][validate]") {
    Document document = makeDocument();
    document.clips[0].model = 3;
    CHECK(codes(Validate(document, ValidateLevel::Structural)) == "ClipTargetMissing");
}

TEST_CASE("wem an anim set's rows and fallback are checked", "[wem][anim][validate]") {
    Document document = makeDocument();
    AnimSet set;
    set.name = "core";
    set.byTag.push_back(AnimTag{7, 4}); // no such clip
    set.baseAnimSet = 9;                // no such set
    document.animSets.push_back(std::move(set));
    document.models[0].animSet = 5; // no such set either

    CHECK(codes(Validate(document, ValidateLevel::Structural)) ==
          "ClipTargetMissing,ClipTargetMissing,ClipTargetMissing");
}

TEST_CASE("wem a channel's node target is a referencer", "[wem][anim][validate]") {
    Document document = makeDocument();
    document.models[0].animChannels.channels[0].target.node = 9;
    CHECK(codes(Validate(document, ValidateLevel::Profile)) == "DanglingNodeReference");
}

TEST_CASE("wem a channel's material target is a referencer", "[wem][anim][validate]") {
    Document document = makeDocument();
    // Past the three stages the material holds.
    document.models[0].animChannels.channels[1].target.sub = 7;
    CHECK(codes(Validate(document, ValidateLevel::Structural)) == "IndexOutOfRange");
}

// ============================================================================
// §10.6 — node removal
// ============================================================================

TEST_CASE("wem removing a node invalidates its channels and keeps them", "[wem][anim][nodes]") {
    Document document = makeDocument();
    Model& model = document.models[0];

    NodeReferencers referencers;
    referencers.meshes = std::span<Mesh>(model.meshes.data(), model.meshes.size());
    referencers.channels = &model.animChannels;
    referencers.clips = std::span<Clip>(document.clips.data(), document.clips.size());

    // Take the spine and its subtree — which is the event node the clip fires at.
    const RemoveResult removed = RemoveNode(model.nodes, 1, RemovePolicy::RemoveSubtree,
                                            SkinPolicy::Refuse, true, referencers);
    REQUIRE(removed.removed);

    Diagnostics report;
    CompactNodes(model.nodes, referencers, report);

    CHECK(model.nodes.size() == 1u);
    // The declaration survives: an id is never reused, and dropping it would
    // make its sub-tracks indistinguishable from a merge that has not landed.
    REQUIRE(model.animChannels.channels.size() == 3u);
    CHECK(model.animChannels.find(1) != nullptr);
    CHECK(model.animChannels.find(1)->target.node == kInvalidNode);
    CHECK(document.clips[0].containers[0].subTracks.size() == 1u);
    CHECK(document.clips[0].events[0].node == kInvalidNode);
    CHECK(report.countOf(DiagCode::DanglingNodeReference) == 1u);
}

TEST_CASE("wem compacting remaps a surviving channel's node", "[wem][anim][nodes]") {
    Document document = makeDocument();
    Model& model = document.models[0];

    // Remove the root, reparenting: the spine becomes node 0 and the channel has
    // to follow it.
    NodeReferencers referencers;
    referencers.meshes = std::span<Mesh>(model.meshes.data(), model.meshes.size());
    referencers.channels = &model.animChannels;
    referencers.clips = std::span<Clip>(document.clips.data(), document.clips.size());

    REQUIRE(RemoveNode(model.nodes, 0, RemovePolicy::ReparentChildren, SkinPolicy::Refuse, true,
                       referencers)
                .removed);
    Diagnostics report;
    CompactNodes(model.nodes, referencers, report);

    CHECK(model.animChannels.find(1)->target.node == 0u);
    CHECK(document.clips[0].events[0].node == 1u);
    CHECK(report.countOf(DiagCode::DanglingNodeReference) == 0u);
}

// ============================================================================
// §7.5 — the material axis
// ============================================================================

TEST_CASE("wem removing a layer shifts a channel's ordinal", "[wem][anim][materials]") {
    Document document = makeDocument();
    Model& model = document.models[0];

    const RemovalResult result = RemoveLayer(model, ProfileId::Wc3Classic, 0, 1);
    REQUIRE(result.removed);
    CHECK(result.rewritten == 1u);
    CHECK(result.invalidated == 0u);
    CHECK(model.animChannels.find(2)->target.sub == 1u);

    // And the feature channel is untouched: an id is not an ordinal.
    CHECK(model.animChannels.find(3)->target.sub == 5u);
    CHECK(model.animChannels.find(3)->target.material.slot == 0u);
}

TEST_CASE("wem removing the targeted layer invalidates the channel", "[wem][anim][materials]") {
    Document document = makeDocument();
    Model& model = document.models[0];

    const RemovalResult result = RemoveLayer(model, ProfileId::Wc3Classic, 0, 2);
    REQUIRE(result.removed);
    CHECK(result.invalidated == 1u);
    CHECK(model.animChannels.find(2)->target.material.slot == kInvalidIndex);
    CHECK(result.diagnostics.countOf(DiagCode::AnimChannelInvalidated) == 1u);
}

TEST_CASE("wem removing a feature invalidates the channel that joined on its id",
          "[wem][anim][materials]") {
    Document document = makeDocument();
    Model& model = document.models[0];

    const RemovalResult result = RemoveFeature(model, ProfileId::Wc3Classic, 0, 5);
    REQUIRE(result.removed);
    CHECK(result.invalidated == 1u);
    CHECK(model.animChannels.find(3)->target.material.slot == kInvalidIndex);
    // The layer channel is untouched: nothing renumbers when a feature goes.
    CHECK(model.animChannels.find(2)->target.sub == 2u);
}

TEST_CASE("wem removing a look shifts the channels above it", "[wem][anim][materials]") {
    Document document = makeDocument();
    Model& model = document.models[0];
    ProfileMaterialSet& set = *model.setFor(ProfileId::Wc3Classic);
    set.looks.add("B", 1);
    set.resizeBindings(model.materialSlots.size());
    set.slotBindings[0].byLook[1] = 0;
    model.animChannels.find(2)->target.material.look = 1;

    const RemovalResult result = RemoveLook(model, ProfileId::Wc3Classic, 0);
    REQUIRE(result.removed);
    CHECK(model.animChannels.find(2)->target.material.look == 0u);
    // The two channels that were on look 0 lost the look they named.
    CHECK(model.animChannels.find(3)->target.material.slot == kInvalidIndex);
}
