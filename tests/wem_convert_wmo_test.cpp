// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// `WmoConverter::fromWmo` on hand-built WMOs: which batches cross, how a material's shader becomes a
/// combiner chain and which side of a vertex-alpha lerp it takes, the light MOCV gives a vertex-lit
/// batch, and the doodads and lights as nodes. Then `AppendDocument`, which brings a doodad's model in,
/// and what an `.mdx` export does with both: the lights in Warcraft III's terms, the doodads written in.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstring>
#include <optional>
#include <vector>

#include <whiteout/models/cross/m2_wc3_lights.h>
#include <whiteout/models/wem/anim/track_read.h>
#include <whiteout/models/wem/inline_models.h>
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

/// Two quads under one shader-23 batch: layers 4001..4004 over heights 5001..5004,
/// four UV sets with set s at vertex v (s + 0.5, v), and MOC2 putting the first
/// quad wholly on layer 0 and the second's on it by @p secondRed (layer 3 the rest).
wmo::Model makeLayered(u8 secondRed) {
    wmo::Model model;
    wmo::Material& material = model.root.materials.emplace_back();
    material.shader = 23;
    material.texture1 = 4001;
    material.texture2 = 4002;
    material.textureExtra = {4003, 4004, 5001, 5002, 5003, 5004};
    model.root.groups.resize(1);
    model.root.groups[0].flags = 0x8;

    wmo::Group group;
    group.header.flags = 0x8;
    addQuad(group, 0.0f);
    addQuad(group, 2.0f);
    group.uvSets.resize(4);
    for (u32 s = 0; s < 4; ++s) {
        for (u32 v = 0; v < 8; ++v)
            group.uvSets[s].push_back({static_cast<f32>(s) + 0.5f, static_cast<f32>(v)});
    }
    std::vector<wmo::Color> weights(8);
    for (u32 v = 0; v < 8; ++v)
        weights[v].r = v < 4 ? u8{255} : secondRed;
    group.vertexColors2 = weights;
    group.batches = {batchOf(0, 0, 0, 7)};
    group.batches[0].indexCount = 12;
    model.groups = {group};
    return model;
}

/// The face corners' UV set 0 that a converter wrote, boundary halfedges left out.
std::vector<Vector2f> cornerUvs(const Mesh& mesh) {
    std::vector<Vector2f> out;
    for (const Vector2f& uv : mesh.attributes.get<Vector2f>(geom::names::uv(0), geom::Domain::Halfedge)) {
        if (uv.x > 0.0f)
            out.push_back(uv);
    }
    return out;
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

TEST_CASE("a batch splits by the layer or side each triangle shows", "[wem][wmo]") {
    // Shader 23: the first quad is layer 0, the second layer 3, each a section
    // whose stage reads set 0, where its corners carry the layer's own set.
    Result<Document> layered = WmoConverter{}.fromWmo(makeLayered(0));
    REQUIRE(layered.ok());
    const Model& model = layered->models[0];
    REQUIRE(model.meshes.size() == 1);
    const Mesh& mesh = model.meshes[0];
    REQUIRE(mesh.sections.size() == 2);
    CHECK(mesh.sections[0].name == "batch_0_layer0");
    CHECK(mesh.sections[1].name == "batch_0_layer3");
    CHECK(model.materialSlots[1] == "group_0_batch_0_layer3");
    const auto seed = [&](u32 slot) {
        const CombinerStage& stage = std::get<CombinersBody>(materialOf(*layered, slot).Common().body).stages[0];
        CHECK(stage.input.uvSet == 0);
        return std::get<TextureFileDataId>(layered->textures[stage.input.texture].key).value;
    };
    CHECK(seed(0) == 4001);
    CHECK(seed(1) == 4004);
    for (const Vector2f& uv : cornerUvs(mesh))
        CHECK(uv.x == (uv.y < 4.0f ? 0.5f : 3.5f));
    CHECK(cornerUvs(mesh).size() == 12);
    CHECK_FALSE(mesh.attributes.has(geom::names::uv(1), geom::Domain::Halfedge));
    // The height maps are no stage's textures.
    CHECK(layered->textures.size() == 2);

    // Red 100 leaves layer 3 the heavier weight, so it takes the second quad;
    // with layer 3's height at 25/255 against layer 0's 1, layer 0 takes both.
    Result<Document> byWeight = WmoConverter{}.fromWmo(makeLayered(100));
    REQUIRE(byWeight.ok());
    CHECK(byWeight->models[0].meshes[0].sections.size() == 2);
    WmoImportOptions options;
    options.meanAlpha = [](const TextureRef& ref) -> std::optional<f32> {
        return std::get<TextureFileDataId>(ref.key).value == 5004 ? 25.0f / 255.0f : 1.0f;
    };
    Result<Document> byHeight = WmoConverter{}.fromWmo(makeLayered(100), options);
    REQUIRE(byHeight.ok());
    const Mesh& one = byHeight->models[0].meshes[0];
    REQUIRE(one.sections.size() == 1);
    CHECK(one.sections[0].name == "batch_0");
    CHECK(byHeight->models[0].materialSlots[0] == "group_0_batch_0");

    // Shader 13 over the second quad, MOCV set 1's alpha 255, 128, 128, 0: its
    // first triangle averages above a half, its second below.
    wmo::Model lerped = makeModel();
    std::vector<wmo::Color>& set1 = lerped.groups[0]->vertexColors[1];
    set1[4].a = 255;
    set1[5].a = 128;
    set1[6].a = 128;
    set1[7].a = 0;
    Result<Document> sides = WmoConverter{}.fromWmo(lerped);
    REQUIRE(sides.ok());
    const Model& sided = sides->models[0];
    REQUIRE(sided.meshes[0].sections.size() == 4);
    CHECK(sided.meshes[0].sections[1].name == "batch_1_side0");
    CHECK(sided.meshes[0].sections[2].name == "batch_1_side1");
    const auto stages = [&](u32 slot) {
        return std::get<CombinersBody>(materialOf(*sides, slot).Common().body).stages;
    };
    CHECK(stages(1)[0].input.texture == 1);
    CHECK(stages(2)[0].input.texture == 2);
    CHECK(stages(2)[0].input.uvSet == 1);
}

TEST_CASE("a triangle is cut where its heavier layer changes", "[wem][wmo]") {
    // One quad, layer 0 along y = 0 and layer 3 along y = 1: the two weigh the
    // same at y = 0.5, where both triangles are cut, the diagonal once for both.
    wmo::Model source = makeLayered(0);
    wmo::Group& group = *source.groups[0];
    group.indices.resize(6);
    group.batches[0].indexCount = 6;
    for (u32 v = 0; v < 8; ++v)
        (*group.vertexColors2)[v].r = v < 2 ? u8{255} : u8{0};
    Result<Document> cut = WmoConverter{}.fromWmo(source);
    REQUIRE(cut.ok());
    const Mesh& mesh = cut->models[0].meshes[0];
    REQUIRE(mesh.sections.size() == 2);
    CHECK(mesh.vertexCount() == 7);
    CHECK(mesh.faceCount() == 6);
    CHECK(mesh.facesOfSection(0).size() == 3);
    CHECK(mesh.facesOfSection(1).size() == 3);
    CHECK(mesh.sections[0].bounds.maximum.y == Catch::Approx(0.5));
    CHECK(mesh.sections[1].bounds.minimum.y == Catch::Approx(0.5));
    u32 onCut = 0;
    for (const Vector3f& p : mesh.attributes.get<Vector3f>(geom::names::kPosition, geom::Domain::Vertex))
        onCut += p.y == 0.5f ? 1u : 0u;
    CHECK(onCut == 3);
    // A cut corner's UVs are its edge's, interpolated: set 3's (3.5, v) on layer 3.
    for (const Vector2f& uv : cornerUvs(mesh))
        CHECK((uv.x == 0.5f || uv.x == 3.5f));
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

namespace {

f32 Wc3Falls(const cross::Wc3Falloff& f, f32 d) {
    return std::exp(-f.damping * d * d) / (1.0f + f.linear * d + f.quadratic * d * d);
}

} // namespace

TEST_CASE("a WMO light falls off where 12.1's does, its colour and intensity squared", "[wem][wmo]") {
    // Half where 12.1's square is a half, one 8-bit step at the end.
    const auto reach = cross::Wc3FalloffFor(0.0f, 10.0f);
    REQUIRE(reach);
    const f32 half = 10.0f * (1.0f - std::sqrt(0.5f));
    CHECK(Wc3Falls(*reach, half) == Catch::Approx(0.5f).margin(1e-4));
    CHECK(Wc3Falls(*reach, 10.0f) == Catch::Approx(1.0f / 255.0f).margin(1e-5));
    CHECK(reach->quadratic > 0.0f);
    // Flat for most of its reach: the Gaussian through the half alone.
    const auto plateau = cross::Wc3FalloffFor(8.0f, 10.0f);
    REQUIRE(plateau);
    CHECK(plateau->quadratic == 0.0f);
    CHECK(Wc3Falls(*plateau, 8.0f + 2.0f * (1.0f - std::sqrt(0.5f))) == Catch::Approx(0.5f).margin(1e-4));
    CHECK_FALSE(cross::Wc3FalloffFor(0.0f, 0.0f));

    wmo::Model source = makeModel();
    wmo::NewLight point;
    point.innerColor = {0, 0, 128, 255};
    point.attenuationEnd = 10.0f;
    point.intensity = 2.0f;
    wmo::NewLight spot = point;
    spot.lightIndex = 1;
    spot.type = 1;
    spot.innerAngle = 1.0f;
    spot.outerAngle = 2.0f;
    source.root.newLights = {point, spot};
    source.groups[0]->newLightRefs = {0, 1};
    Result<Document> converted = WmoConverter{}.fromWmo(source);
    REQUIRE(converted.ok());
    Document document = std::move(*converted.value);

    // And a `.m2` light, whose state is all keys.
    Model& model = document.models[0];
    Node bulb;
    bulb.name = "m2 light";
    bulb.kind = NodeKind::Light;
    bulb.resetPayloadForKind();
    bulb.parent = 0;
    bulb.native.set("m2LightType", 1);
    const u32 bulbNode = model.nodes.add(bulb);
    const auto key = [](std::vector<f32> values) {
        std::vector<u8> bytes(values.size() * sizeof(f32));
        std::memcpy(bytes.data(), values.data(), bytes.size());
        return bytes;
    };
    const auto channelOf = [&](u32 id, Channel what, geom::AttrType type) {
        AnimChannel channel;
        channel.id = id;
        channel.target.node = bulbNode;
        channel.target.channel = what;
        channel.valueType = type;
        model.animChannels.add(channel);
    };
    channelOf(100, Channel::Color, geom::AttrType::F32x3);
    channelOf(101, Channel::AttenuationEnd, geom::AttrType::F32);
    Clip stand;
    stand.model = 0;
    stand.containers.emplace_back();
    SubTrack color;
    color.channel = 100;
    color.times = {0.0f};
    color.values = key({0.5f, 1.0f, 0.0f});
    SubTrack end;
    end.channel = 101;
    end.times = {0.0f, 1.0f};
    end.values = key({4.0f, 6.0f});
    stand.containers[0].subTracks = {color, end};
    document.clips.push_back(stand);

    const cross::M2LightReport report = cross::CrossM2Lights(document);
    CHECK(report.restated == 3);
    CHECK(report.spots == 1);
    const NodeTree& tree = document.models[0].nodes;
    const auto& lit = std::get<LightPayload>(tree.nodes[1].payload);
    CHECK(lit.color.x == Catch::Approx((128.0f / 255.0f) * (128.0f / 255.0f)));
    CHECK(lit.intensity == Catch::Approx(4.0f));
    CHECK(Wc3Falls(cross::Wc3Falloff{lit.quadraticFalloff, lit.linearFalloff, lit.damping}, half) ==
          Catch::Approx(0.5f).margin(1e-4));
    // The spot, an omni light with the share of the sphere its cone lights.
    const auto& cone = std::get<LightPayload>(tree.nodes[2].payload);
    CHECK(cone.intensity == Catch::Approx(4.0f * 0.5f * (1.0f - std::cos(0.75f))));
    // The `.m2` light: its colour keys squared, its reach its first key.
    f32 squared[3];
    std::memcpy(squared, document.clips.back().containers[0].subTracks[0].values.data(), sizeof(squared));
    CHECK(squared[0] == Catch::Approx(0.25f));
    CHECK(squared[1] == Catch::Approx(1.0f));
    const auto& bulbLight = std::get<LightPayload>(tree.nodes[bulbNode].payload);
    const f32 bulbHalf = 4.0f * (1.0f - std::sqrt(0.5f));
    CHECK(Wc3Falls(cross::Wc3Falloff{bulbLight.quadraticFalloff, bulbLight.linearFalloff, bulbLight.damping},
                   bulbHalf) == Catch::Approx(0.5f).margin(1e-4));
}

TEST_CASE("an attached model goes into the model where it stands, keyed on a global loop", "[wem][wmo]") {
    // The host: a WMO with one doodad, scaled 2, a quarter turn about +Z, at (3, 4, 5).
    wmo::Model source = makeModel();
    wmo::DoodadSet global;
    global.count = 1;
    source.root.doodadSets = {global};
    source.root.doodadFileIds = {500};
    source.root.doodadDefs.resize(1);
    source.root.doodadDefs[0].position = {3.0f, 4.0f, 5.0f};
    source.root.doodadDefs[0].scale = 2.0f;
    source.root.doodadDefs[0].rotation = {0.0f, 0.0f, 0.70710678f, 0.70710678f};
    source.groups[0]->doodadRefs = {0};
    Result<Document> host = WmoConverter{}.fromWmo(source);
    REQUIRE(host.ok());
    Document document = std::move(*host.value);
    const Matrix44f placed = Matrix44f::scaling({2.0f, 2.0f, 2.0f}) *
                             Matrix44f::rotation(source.root.doodadDefs[0].rotation).transpose() *
                             Matrix44f::translation(source.root.doodadDefs[0].position);

    // The doodad: another WMO's meshes on a bone pivoting at (1, 0, 0), which a
    // clip turns and moves, and an emitter on it no key turns.
    Result<Document> other = WmoConverter{}.fromWmo(makeModel());
    REQUIRE(other.ok());
    Document child = std::move(*other.value);
    Model& doodad = child.models[0];
    doodad.name = "brazier";
    doodad.nodes.nodes[0].pivot = {1.0f, 0.0f, 0.0f};
    doodad.nodes.nodes[0].local.translation = {1.0f, 0.0f, 0.0f};
    Node emitter;
    emitter.name = "fire";
    emitter.kind = NodeKind::Wc3ParticleEmitter2;
    emitter.resetPayloadForKind();
    std::get<Wc3ParticleEmitter2Payload>(emitter.payload).speed = 10.0f;
    std::get<Wc3ParticleEmitter2Payload>(emitter.payload).width = 4.0f;
    std::get<Wc3ParticleEmitter2Payload>(emitter.payload).start.scaling = 3.0f;
    emitter.parent = 0;
    emitter.pivot = {1.0f, 0.0f, 1.0f};
    emitter.local.translation = {0.0f, 0.0f, 1.0f};
    const u32 fire = doodad.nodes.add(emitter);
    AnimChannel turn;
    turn.id = 0;
    turn.target.node = 0;
    turn.target.channel = Channel::Rotation;
    turn.valueType = geom::AttrType::Quat;
    doodad.animChannels.add(turn);
    AnimChannel move = turn;
    move.id = 1;
    move.target.channel = Channel::Translation;
    move.valueType = geom::AttrType::F32x3;
    doodad.animChannels.add(move);
    const Quaternion aboutX{0.38268343f, 0.0f, 0.0f, 0.92387953f}; // 45 degrees
    const Vector3f by{0.5f, 0.0f, 0.25f};
    Clip stand;
    stand.name = "Stand";
    stand.model = 0;
    stand.duration = 2.0f;
    stand.native.set("animationId", 0);
    stand.containers.emplace_back();
    SubTrack turning;
    turning.channel = 0;
    turning.interp = Interpolation::Slerp;
    turning.times = {0.0f};
    turning.values.resize(sizeof(Quaternion));
    std::memcpy(turning.values.data(), &aboutX, sizeof(Quaternion));
    SubTrack moving;
    moving.channel = 1;
    moving.times = {0.0f};
    moving.values.resize(sizeof(Vector3f));
    std::memcpy(moving.values.data(), &by, sizeof(Vector3f));
    stand.containers[0].subTracks = {turning, moving};
    child.clips.push_back(stand);
    const u32 childNodes = doodad.nodes.size();
    const u32 childMeshes = static_cast<u32>(doodad.meshes.size());
    const Vector3f corner =
        doodad.meshes[0].attributes.get<Vector3f>(geom::names::kPosition, geom::Domain::Vertex)[1];

    Diagnostics diagnostics;
    const u32 at = AppendDocument(document, std::move(child), diagnostics);
    REQUIRE(at == 1);
    const u32 attachment = 1;
    REQUIRE(document.models[0].nodes.nodes[attachment].kind == NodeKind::Attachment);
    std::get<AttachmentPayload>(document.models[0].nodes.nodes[attachment].payload).model = at;
    const u32 hostNodes = document.models[0].nodes.size();
    const u32 hostMeshes = static_cast<u32>(document.models[0].meshes.size());
    const u32 hostClips = static_cast<u32>(document.clips.size());

    const InlineReport report = InlineAttachedModels(document, 0);
    CHECK(report.placements == 1);
    CHECK(report.models == 1);
    CHECK(report.loops == 1);
    const Model& model = document.models[0];
    REQUIRE(model.nodes.size() == hostNodes + childNodes);
    REQUIRE(model.meshes.size() == hostMeshes + childMeshes);
    CHECK(std::get<AttachmentPayload>(model.nodes.nodes[attachment].payload).model == kInvalidIndex);

    // Nodes behind the attachment's name, the doodad's root under its parent.
    const Node& bone = model.nodes.nodes[hostNodes];
    CHECK(bone.name == "doodad_0 wmo_root");
    CHECK(bone.parent == model.nodes.nodes[attachment].parent);
    const auto near = [](const Vector3f& a, const Vector3f& b) {
        CHECK(a.x == Catch::Approx(b.x).margin(1e-4));
        CHECK(a.y == Catch::Approx(b.y).margin(1e-4));
        CHECK(a.z == Catch::Approx(b.z).margin(1e-4));
    };
    near(bone.pivot, whiteout::transform_point({1.0f, 0.0f, 0.0f}, placed));
    near(model.meshes[hostMeshes].attributes.get<Vector3f>(geom::names::kPosition, geom::Domain::Vertex)[1],
         whiteout::transform_point(corner, placed));

    // Its materials once, under slots of its own.
    const MeshSection& section = model.meshes[hostMeshes].sections[0];
    CHECK(model.materialSlots[section.materialSlot].rfind("brazier|", 0) == 0);
    const ProfileMaterialSet* set = model.setFor(ProfileId::Wow);
    REQUIRE(set != nullptr);
    CHECK(set->slotBindings[section.materialSlot].bound(0));

    // The clip it plays, a global loop of the host, whose keys move a point as
    // placing the point the doodad moves does.
    REQUIRE(document.clips.size() == hostClips + 1);
    const Clip& loop = document.clips.back();
    CHECK(loop.model == 0);
    CHECK(IsGlobalLoop(loop));
    CHECK(loop.duration == 2.0f);
    Quaternion turned{0.0f, 0.0f, 0.0f, 1.0f};
    Vector3f moved{0.0f, 0.0f, 0.0f};
    std::optional<Quaternion> frameKey;
    for (const SubTrack& track : loop.containers[0].subTracks) {
        const AnimChannel* channel = model.animChannels.find(track.channel);
        REQUIRE(channel != nullptr);
        if (channel->target.node == hostNodes && channel->target.channel == Channel::Rotation)
            std::memcpy(&turned, track.values.data(), sizeof(Quaternion));
        if (channel->target.node == hostNodes && channel->target.channel == Channel::Translation)
            std::memcpy(&moved, track.values.data(), sizeof(Vector3f));
        if (channel->target.node == hostNodes + fire && channel->target.channel == Channel::Rotation) {
            Quaternion q{0.0f, 0.0f, 0.0f, 1.0f};
            std::memcpy(&q, track.values.data(), sizeof(Quaternion));
            frameKey = q;
        }
    }
    const auto pose = [](const Vector3f& x, const Vector3f& pivot, const Quaternion& q, const Vector3f& t) {
        return whiteout::transform_point(x - pivot, Matrix44f::rotation(q).transpose()) + pivot + t;
    };
    const Vector3f probe{2.0f, 1.0f, -1.0f};
    near(pose(whiteout::transform_point(probe, placed), bone.pivot, turned, moved),
         whiteout::transform_point(pose(probe, {1.0f, 0.0f, 0.0f}, aboutX, by), placed));

    // Its particles spread and fly by the placement's scale, and keep their size.
    const auto& sprayed = std::get<Wc3ParticleEmitter2Payload>(model.nodes.nodes[hostNodes + fire].payload);
    CHECK(sprayed.speed == Catch::Approx(20.0f));
    CHECK(sprayed.width == Catch::Approx(8.0f));
    CHECK(sprayed.start.scaling == Catch::Approx(3.0f));

    // The emitter's frame turns with the placement, by a key of its own.
    REQUIRE(frameKey);
    near(whiteout::transform_point({1.0f, 0.0f, 0.0f}, Matrix44f::rotation(*frameKey).transpose()),
         {0.0f, 1.0f, 0.0f});
}
