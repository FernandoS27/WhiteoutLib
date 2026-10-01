// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// `WmoConverter::fromWmo` on hand-built WMOs: which batches cross, how a material's shader becomes a
/// combiner chain and which side of a vertex-alpha lerp it takes, the light MOCV gives a vertex-lit
/// batch, and the doodads and lights as nodes. Then `AppendDocument`, which brings a doodad's model in.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <vector>

#include <whiteout/models/wem/materials/ops.h>
#include <whiteout/models/wem/wmo_converter.h>
#include <whiteout/models/wow/wmo/wmo.h>

using namespace whiteout;
using namespace whiteout::models;
using namespace whiteout::models::wem;
namespace wmo = whiteout::models::wow::wmo;

namespace {

/// A quad of two triangles at @p first, its corners' UVs (0,0)..(1,1) in both sets.
void addQuad(wmo::Group& g, f32 x) {
    const u32 base = static_cast<u32>(g.positions.size());
    for (u32 i = 0; i < 4; ++i) {
        g.positions.push_back({x + static_cast<f32>(i & 1), static_cast<f32>(i >> 1), 0.0f});
        g.normals.push_back({0.0f, 0.0f, 1.0f});
    }
    for (u32 i : {0u, 1u, 2u, 1u, 3u, 2u})
        g.indices.push_back(base + i);
}

wmo::Batch batchOf(u32 material, u32 startIndex, u16 firstVertex, u16 lastVertex) {
    wmo::Batch b;
    b.materialSmall = static_cast<u8>(material);
    b.startIndex = startIndex;
    b.indexCount = 6;
    b.minIndex = firstVertex;
    b.maxIndex = lastVertex;
    return b;
}

/// Material 0: shader 0, opaque, one texture. Material 1: shader 13 (two layers lerped by MOCV
/// set 1's alpha), alpha-keyed, two-sided, clamped in S. Material 2: shader 10, a window.
wmo::Model makeModel() {
    wmo::Model model;
    wmo::Root& root = model.root;
    root.materials.resize(3);
    root.materials[0].shader = 0;
    root.materials[0].texture0 = 1001;
    root.materials[1].shader = 13;
    root.materials[1].blendMode = 1;
    root.materials[1].flags = 0x4 | 0x40;
    root.materials[1].texture0 = 2001;
    root.materials[1].texture1 = 2002;
    root.materials[2].shader = 10;
    root.materials[2].texture0 = 3001;
    root.groups.resize(3);
    root.groups[1].flags = 0x8;

    // Group 0, interior, with both colour sets: three batches, the window's not drawn.
    wmo::Group inside;
    inside.header.flags = 0x4; // MOCV set 0
    addQuad(inside, 0.0f);
    addQuad(inside, 2.0f);
    addQuad(inside, 4.0f);
    inside.uvSets.assign(2, std::vector<Vector2f>(12, Vector2f{0.5f, 0.5f}));
    // Set 0: (64, 64, 64), alpha 0. Set 1: alpha 255 on the first two quads, 0 on the third.
    std::vector<wmo::Color> set0(12, wmo::Color{64, 64, 64, 0});
    std::vector<wmo::Color> set1(12, wmo::Color{0, 0, 0, 255});
    for (u32 v = 8; v < 12; ++v)
        set1[v].a = 0;
    inside.vertexColors = {set0, set1};
    inside.batches = {batchOf(0, 0, 0, 3), batchOf(1, 6, 4, 7), batchOf(2, 12, 8, 11)};
    // The third quad is material 1 again: its set-1 alpha is 0, the second layer's side.
    inside.batches[2].materialSmall = 1;
    inside.batches.push_back(batchOf(2, 12, 8, 11)); // the window, over the same quad

    // Group 1, exterior, no colours.
    wmo::Group outside;
    outside.header.flags = 0x8;
    addQuad(outside, 10.0f);
    outside.batches = {batchOf(0, 0, 0, 3)};

    // Group 2: an antiportal, never drawn.
    wmo::Group antiportal;
    antiportal.header.flags = 0x4000000;
    addQuad(antiportal, 20.0f);
    antiportal.batches = {batchOf(0, 0, 0, 3)};

    model.groups = {inside, outside, antiportal};
    return model;
}

const Material& materialOf(const Document& document, u32 slot) {
    const ProfileMaterialSet& set = document.models[0].profileSets[0];
    return set.materials[set.slotBindings[slot].byLook[0]];
}

} // namespace

TEST_CASE("a WMO converts to a mesh per drawn group and a section per drawn batch", "[wem][wmo]") {
    Result<Document> converted = WmoConverter{}.fromWmo(makeModel());
    REQUIRE(converted.ok());
    const Document& document = *converted;
    CHECK(document.defaultProfile == ProfileId::Wow);
    REQUIRE(document.models.size() == 1);
    const Model& model = document.models[0];

    // The antiportal draws nothing; the window batch is not an ordinary one.
    REQUIRE(model.meshes.size() == 2);
    REQUIRE(model.meshes[0].sections.size() == 3);
    CHECK(model.meshes[1].sections.size() == 1);
    CHECK(model.materialSlots.size() == 4);
    CHECK(model.materialSlots[1] == "group_0_batch_1");

    const MeshSection& section = model.meshes[0].sections[1];
    CHECK(section.materialSlot == 1);
    REQUIRE(section.rigidNode.has_value());
    CHECK(*section.rigidNode == 0);
    CHECK(section.native.value("wmoGroup", -1) == 0);
    CHECK(section.native.value("wmoBatch", -1) == 1);
    CHECK(section.native.value("wmoMaterial", -1) == 1);
    CHECK(section.native.value("wmoShader", -1) == 13);
    CHECK(model.nodes.nodes[0].kind == NodeKind::Bone);

    // Three textures, by FileDataID, the window's never named.
    REQUIRE(document.textures.size() == 3);
    const auto* first = std::get_if<TextureFileDataId>(&document.textures[0].key);
    REQUIRE(first != nullptr);
    CHECK(first->value == 1001);
}

TEST_CASE("a WMO material's common state and its chain", "[wem][wmo]") {
    Result<Document> converted = WmoConverter{}.fromWmo(makeModel());
    REQUIRE(converted.ok());
    const Document& document = *converted;

    const CommonMaterial& plain = materialOf(document, 0).Common();
    CHECK(plain.blend == BlendMode::Opaque);
    const CombinersBody* body = std::get_if<CombinersBody>(&plain.body);
    REQUIRE(body != nullptr);
    REQUIRE(body->stages.size() == 1);
    CHECK(body->stages[0].rgb == CombinerOp::Opaque);
    CHECK(body->stages[0].alpha == CombinerOp::Mod);

    // Shader 13 where MOCV set 1's alpha is 1: the first layer is the seed.
    const CommonMaterial& keyed = materialOf(document, 1).Common();
    CHECK(keyed.blend == BlendMode::AlphaKey);
    CHECK(keyed.alphaTestThreshold == Catch::Approx(128.0 / 255.0));
    CHECK(keyed.cull == CullMode::None);
    CHECK(keyed.depth.write);
    const CombinersBody* two = std::get_if<CombinersBody>(&keyed.body);
    REQUIRE(two != nullptr);
    REQUIRE(two->stages.size() == 2);
    CHECK(two->stages[0].input.texture == 1);
    CHECK(two->stages[0].input.uvSet == 0);
    CHECK(two->stages[0].input.wrapU == WrapMode::Clamp);
    CHECK(two->stages[0].input.wrapV == WrapMode::Repeat);
    CHECK(two->stages[1].rgb == CombinerOp::Pass);

    // The same material where the alpha is 0: the second layer, on its own UVs.
    const CombinersBody* other = std::get_if<CombinersBody>(&materialOf(document, 2).Common().body);
    REQUIRE(other != nullptr);
    CHECK(other->stages[0].input.texture == 2);
    CHECK(other->stages[0].input.uvSet == 1);
    CHECK(converted.diagnostics.countOf(DiagCode::LossyKindConversion) == 1);
}

TEST_CASE("a vertex-lit batch takes MOCV's light as colour 0", "[wem][wmo]") {
    Result<Document> converted = WmoConverter{}.fromWmo(makeModel());
    REQUIRE(converted.ok());
    const Model& model = converted->models[0];

    // Uploaded: (64 + 64·0/64 − 0) >> 1 = 32, so the light is 2·32 = 64, alpha 255.
    const auto light = model.meshes[0].attributes.get<std::array<u8, 4>>(geom::names::color(0),
                                                                          geom::Domain::Halfedge);
    // Every face corner, three quads' 18; the boundary halfedges have no corner and stay zero.
    u32 lit = 0;
    for (const auto& corner : light) {
        lit += corner == std::array<u8, 4>{64, 64, 64, 255} ? 1u : 0u;
        CHECK((corner == std::array<u8, 4>{64, 64, 64, 255} || corner == std::array<u8, 4>{}));
    }
    CHECK(lit == 18);
    // The file's sets ride along under names no exporter reads.
    const geom::AttrLayer* raw = model.meshes[0].attributes.layer("wmo.color0", geom::Domain::Vertex);
    REQUIRE(raw != nullptr);
    CHECK(raw->type == geom::AttrType::U8x4);
    CHECK(model.meshes[0].attributes.has("wmo.color1", geom::Domain::Vertex));

    // The exterior group is sun-lit: no colour 0 at all.
    CHECK_FALSE(model.meshes[1].attributes.has(geom::names::color(0), geom::Domain::Halfedge));
}

TEST_CASE("MOUV scrolls a stage through its texture matrix", "[wem][wmo]") {
    wmo::Model source = makeModel();
    source.root.uvAnimations.resize(3);
    source.root.uvAnimations[0].speed0 = {0.5f, -0.25f};
    Result<Document> converted = WmoConverter{}.fromWmo(source);
    REQUIRE(converted.ok());

    const CommonMaterial& common = materialOf(*converted, 0).Common();
    REQUIRE(common.features.size() == 1);
    CHECK(common.features[0].layer == 0);
    const UvAnimationFeature* uv = common.features[0].uvAnimation();
    REQUIRE(uv != nullptr);
    CHECK(uv->scrollRate.x == 0.5f);
    CHECK(uv->scrollRate.y == -0.25f);
}

TEST_CASE("doodads become attachments at their placement, lights become nodes", "[wem][wmo]") {
    wmo::Model source = makeModel();
    wmo::Root& root = source.root;
    wmo::DoodadSet global;
    global.count = 1;
    wmo::DoodadSet extra;
    extra.startIndex = 1;
    extra.count = 1;
    root.doodadSets = {global, extra};
    root.doodadFileIds = {500, 600};
    root.doodadDefs.resize(2);
    root.doodadDefs[0].position = {3.0f, 4.0f, 5.0f};
    root.doodadDefs[0].scale = 2.0f;
    // A quarter turn about +Z.
    root.doodadDefs[0].rotation = {0.0f, 0.0f, 0.70710678f, 0.70710678f};
    root.doodadDefs[1].nameAndFlags = 1;
    source.groups[0]->doodadRefs = {0, 1};

    wmo::NewLight light;
    light.lightIndex = 7;
    light.innerColor = {0, 0, 255, 255};
    light.position = {1.0f, 2.0f, 3.0f};
    light.attenuationEnd = 9.0f;
    root.newLights = {light};
    source.groups[0]->newLightRefs = {0};
    source.groups[1]->newLightRefs = {0}; // the same record twice is one light

    Result<Document> converted = WmoConverter{}.fromWmo(source);
    REQUIRE(converted.ok());
    const NodeTree& tree = converted->models[0].nodes;
    REQUIRE(tree.size() == 3); // root, the light, set 0's doodad
    const Node& lit = tree.nodes[1];
    CHECK(lit.kind == NodeKind::Light);
    CHECK(lit.local.translation.z == 3.0f);
    const auto& payload = std::get<LightPayload>(lit.payload);
    CHECK(payload.color.x == Catch::Approx(1.0f));
    CHECK(payload.attenuationEnd == 9.0f);

    const Node& doodad = tree.nodes[2];
    CHECK(doodad.kind == NodeKind::Attachment);
    CHECK(doodad.native.value("wmoDoodad", -1) == 0);
    const auto& attachment = std::get<AttachmentPayload>(doodad.payload);
    CHECK(attachment.asset.id == 500);
    CHECK(attachment.model == kInvalidIndex);
    // Where the renderer puts the model's +X: scaled, turned onto +Y, moved.
    const Vector3f tip = TransformPoint(tree.worldBind(2), {1.0f, 0.0f, 0.0f});
    const Vector3f expected = whiteout::transform_point(
        {1.0f, 0.0f, 0.0f}, Matrix44f::scaling({2.0f, 2.0f, 2.0f}) *
                                Matrix44f::rotation(root.doodadDefs[0].rotation).transpose() *
                                Matrix44f::translation(root.doodadDefs[0].position));
    CHECK(tip.x == Catch::Approx(expected.x).margin(1e-5));
    CHECK(tip.y == Catch::Approx(expected.y).margin(1e-5));
    CHECK(tip.z == Catch::Approx(expected.z).margin(1e-5));

    // The second set on brings its doodad too.
    WmoImportOptions options;
    options.doodadSets = {1};
    Result<Document> both = WmoConverter{}.fromWmo(source, options);
    REQUIRE(both.ok());
    CHECK(both->models[0].nodes.size() == 4);
}

TEST_CASE("AppendDocument re-bases what the appended models name", "[wem][wmo]") {
    Result<Document> converted = WmoConverter{}.fromWmo(makeModel());
    REQUIRE(converted.ok());
    Document into = std::move(*converted.value);
    const u32 texturesBefore = static_cast<u32>(into.textures.size());

    Document child;
    child.declare(ProfileId::Wow);
    child.textures.resize(2);
    Model model;
    model.addSlot("only");
    ProfileMaterialSet set;
    set.profile = ProfileId::Wow;
    set.looks.looks.push_back(Look{});
    Material material;
    CombinersBody body;
    CombinerStage stage;
    stage.input.texture = 1;
    body.stages.push_back(stage);
    material.InitCommon().body = body;
    native::M2Material block;
    block.units.push_back({});
    block.units[0].texture = 1;
    material.SetNativeInSync(block);
    set.materials.push_back(std::move(material));
    set.resizeBindings(1);
    set.slotBindings[0].byLook[0] = 0;
    model.profileSets.push_back(std::move(set));
    child.models.push_back(std::move(model));
    Clip clip;
    clip.model = 0;
    child.clips.push_back(clip);

    Diagnostics diagnostics;
    const u32 at = AppendDocument(into, std::move(child), diagnostics);
    REQUIRE(at == 1);
    REQUIRE(into.models.size() == 2);
    CHECK(into.textures.size() == texturesBefore + 2);
    const Material& moved = into.models[1].profileSets[0].materials[0];
    CHECK(std::get<CombinersBody>(moved.Common().body).stages[0].input.texture == texturesBefore + 1);
    CHECK(std::get<native::M2Material>(moved.Native()).units[0].texture == texturesBefore + 1);
    CHECK(moved.sync() == NativeSync::InSync);
    REQUIRE(into.clips.size() == 1);
    CHECK(into.clips[0].model == 1);
}
