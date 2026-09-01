// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// WEM v3 P5 — `MdxConverter` on hand-built models.
///
/// The corpus sweep (`wem_convert_corpus_test`) answers "does it survive shipped
/// content"; this answers "does it mean the right thing", which a sweep cannot.
/// Every case here is a claim the design makes about MDX that a green parse
/// would not check: the two-profile split, the pivot-to-local conversion, the
/// section mask, and the two skinning conventions.

#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/wem/converters.h>

using namespace whiteout;
using namespace whiteout::models::wem;

namespace {

mdx::Node makeNode(const std::string& name, u32 objectId, u32 parentId) {
    mdx::Node node;
    node.name = name;
    node.objectId = objectId;
    node.parentId = parentId;
    return node;
}

mdx::Layer makeLayer(mdx::Layer::FilterMode mode, bool hd, u32 textureId) {
    mdx::Layer layer;
    layer.filterMode = mode;
    layer.is_hd = hd;
    layer.textureId = textureId;
    if (hd) {
        layer.shader = mdx::Layer::ShaderType::HD;
    }
    return layer;
}

/// A quad in two triangles, one material, two bones.
mdx::Model makeModel() {
    mdx::Model model;
    model.version = 1200;
    model.modelName = "test";
    model.modelExtent.minimum = Vector3f{-1, -1, 0};
    model.modelExtent.maximum = Vector3f{1, 1, 0};

    mdx::Texture texture;
    texture.fileName = "textures/body.blp";
    model.textures.push_back(texture);

    mdx::Material material;
    material.layers.push_back(makeLayer(mdx::Layer::FilterMode::None, false, 0));
    model.materials.push_back(material);

    mdx::Bone root;
    root.node = makeNode("root", 0, mdx::Node::NO_PARENT);
    mdx::Bone child;
    child.node = makeNode("child", 1, 0);
    model.bones.push_back(root);
    model.bones.push_back(child);
    model.pivotPoints = {Vector3f{0, 0, 0}, Vector3f{0, 0, 10}};

    mdx::Geoset geoset;
    geoset.lodName = "body";
    geoset.vertexPositions = {Vector3f{0, 0, 0}, Vector3f{1, 0, 0}, Vector3f{1, 1, 0},
                              Vector3f{0, 1, 0}};
    geoset.vertexNormals = {Vector3f{0, 0, 1}, Vector3f{0, 0, 1}, Vector3f{0, 0, 1},
                            Vector3f{0, 0, 1}};
    geoset.textureCoordinateSets.push_back(
        {Vector2f{0, 0}, Vector2f{1, 0}, Vector2f{1, 1}, Vector2f{0, 1}});
    geoset.faces = {0, 1, 2, 0, 2, 3};
    geoset.materialId = 0;
    model.geosets.push_back(geoset);
    return model;
}

} // namespace

TEST_CASE("wem mdx import produces one model with a classic set", "[wem][convert][mdx]") {
    const MdxConverter converter;
    Result<Document> result = converter.fromMdx(makeModel());
    REQUIRE(result.ok());
    const Document& document = *result;

    CHECK(document.name == "test");
    CHECK(document.space == CoordSpace::Blizzard);
    REQUIRE(document.models.size() == 1);
    REQUIRE(document.textures.size() == 1);
    CHECK(document.textures[0].path == "textures/body.blp");

    // Only classic: the one material has no HD layer, so no Reforged set is
    // invented for it.
    CHECK(document.carries(ProfileId::Wc3Classic));
    CHECK_FALSE(document.carries(ProfileId::Wc3Reforged));

    const Model& model = document.models.front();
    REQUIRE(model.meshes.size() == 1);
    CHECK(model.meshes[0].faceCount() == 2);
    CHECK(model.meshes[0].vertexCount() == 4);
    REQUIRE(model.materialSlots.size() == 1);
    REQUIRE(model.profileSets.size() == 1);
    CHECK(Resolve(model, 0, ProfileId::Wc3Classic) != nullptr);
}

TEST_CASE("wem mdx one material feeding both profiles masks its section", "[wem][convert][mdx]") {
    mdx::Model source = makeModel();
    // A second material with HD layers only, and a second geoset that uses it.
    mdx::Material reforged;
    reforged.shader = "Shader_HD_DefaultUnit";
    for (u32 slot = 0; slot < 6; ++slot) {
        reforged.layers.push_back(makeLayer(mdx::Layer::FilterMode::None, true, 0));
    }
    source.materials.push_back(reforged);
    mdx::Geoset second = source.geosets[0];
    second.lodName = "hd_body";
    second.materialId = 1;
    source.geosets.push_back(second);

    const MdxConverter converter;
    Result<Document> result = converter.fromMdx(source);
    REQUIRE(result.ok());
    const Document& document = *result;

    CHECK(document.carries(ProfileId::Wc3Classic));
    CHECK(document.carries(ProfileId::Wc3Reforged));

    const Model& model = document.models.front();
    REQUIRE(model.meshes.size() == 2);
    REQUIRE(model.meshes[0].sections.size() == 1);
    REQUIRE(model.meshes[1].sections.size() == 1);

    // The SD geoset draws only in classic, the HD one only in Reforged. That
    // mask is what keeps a Reforged consumer from drawing the SD copy on top.
    CHECK(HasProfile(model.meshes[0].sections[0].profiles, ProfileId::Wc3Classic));
    CHECK_FALSE(HasProfile(model.meshes[0].sections[0].profiles, ProfileId::Wc3Reforged));
    CHECK(HasProfile(model.meshes[1].sections[0].profiles, ProfileId::Wc3Reforged));
    CHECK_FALSE(HasProfile(model.meshes[1].sections[0].profiles, ProfileId::Wc3Classic));

    // And each set binds only its own slot.
    CHECK(Resolve(model, 0, ProfileId::Wc3Classic) != nullptr);
    CHECK(Resolve(model, 1, ProfileId::Wc3Classic) == nullptr);
    CHECK(Resolve(model, 0, ProfileId::Wc3Reforged) == nullptr);
    CHECK(Resolve(model, 1, ProfileId::Wc3Reforged) != nullptr);
}

TEST_CASE("wem mdx pivots become local translations", "[wem][convert][mdx][nodes]") {
    const MdxConverter converter;
    Result<Document> result = converter.fromMdx(makeModel());
    REQUIRE(result.ok());
    const NodeTree& nodes = result->models.front().nodes;

    REQUIRE(nodes.size() == 2);
    CHECK(nodes.nodes[0].name == "root");
    CHECK(nodes.nodes[1].parent == 0);

    // The pivot is carried verbatim...
    CHECK(nodes.nodes[1].pivot.z == 10.0f);
    // ...and the local translation is the difference, so `worldBind` lands the
    // child back on its absolute pivot.
    CHECK(nodes.nodes[1].local.translation.z == 10.0f);
    CHECK(nodes.worldBind(1).translation.z == 10.0f);
}

TEST_CASE("wem mdx reads both skinning conventions", "[wem][convert][mdx][skin]") {
    SECTION("Reforged skinData addresses matrixIndices") {
        mdx::Model source = makeModel();
        mdx::Geoset& geoset = source.geosets[0];
        geoset.matrixIndices = {1}; // palette slot 0 -> object id 1 -> node 1
        geoset.skinData.assign(4 * 8, 0);
        for (std::size_t v = 0; v < 4; ++v) {
            geoset.skinData[v * 8 + 0] = 0;
            geoset.skinData[v * 8 + 4] = 255;
        }

        const MdxConverter converter;
        Result<Document> result = converter.fromMdx(source);
        REQUIRE(result.ok());
        const Mesh& mesh = result->models.front().meshes[0];
        REQUIRE(mesh.skin.vertexCount() == 4);
        const auto influences = mesh.skin.forVertex(0);
        REQUIRE(influences.size() == 1);
        CHECK(influences[0].bone == 1);
        CHECK(influences[0].weight == 1.0f);
    }

    SECTION("classic matrix groups weight equally") {
        mdx::Model source = makeModel();
        mdx::Geoset& geoset = source.geosets[0];
        geoset.matrixIndices = {0, 1};
        geoset.matrixGroups = {2};
        geoset.vertexGroups = {0, 0, 0, 0};

        const MdxConverter converter;
        Result<Document> result = converter.fromMdx(source);
        REQUIRE(result.ok());
        const Mesh& mesh = result->models.front().meshes[0];
        const auto influences = mesh.skin.forVertex(0);
        REQUIRE(influences.size() == 2);
        CHECK(influences[0].weight == 0.5f);
        CHECK(influences[1].weight == 0.5f);
    }
}

TEST_CASE("wem mdx export refuses a profile the document does not carry", "[wem][convert][mdx]") {
    const MdxConverter converter;
    Result<Document> imported = converter.fromMdx(makeModel());
    REQUIRE(imported.ok());

    Result<mdx::Model> exported = converter.toMdx(*imported, ProfileId::Wc3Reforged);
    CHECK_FALSE(exported.ok());
    CHECK(exported.diagnostics.countOf(DiagCode::ProfileNotCarried) == 1);

    // And a profile no MDX converter serves is refused before the document is
    // even consulted.
    Result<std::vector<u8>> wrong = converter.exportToBytes(*imported, ProfileId::Wow);
    CHECK_FALSE(wrong.ok());
    CHECK(wrong.diagnostics.countOf(DiagCode::ProfileNotCarried) == 1);
}

TEST_CASE("wem mdx round trip keeps geometry, nodes and slots", "[wem][convert][mdx]") {
    const MdxConverter converter;
    Result<Document> imported = converter.fromMdx(makeModel());
    REQUIRE(imported.ok());

    Result<mdx::Model> exported = converter.toMdx(*imported, ProfileId::Wc3Classic, 1200);
    REQUIRE(exported.ok());
    const mdx::Model& out = *exported;

    CHECK(out.version == 1200);
    CHECK(out.modelName == "test");
    REQUIRE(out.geosets.size() == 1);
    CHECK(out.geosets[0].vertexPositions.size() == 4);
    CHECK(out.geosets[0].faces.size() == 6);
    REQUIRE(out.textures.size() == 1);
    CHECK(out.textures[0].fileName == "textures/body.blp");
    REQUIRE(out.bones.size() == 2);
    CHECK(out.bones[1].node.parentId == 0);
    REQUIRE(out.pivotPoints.size() == 2);
    CHECK(out.pivotPoints[1].z == 10.0f);
    // One MDX material per slot, so the geoset's materialId is the slot index
    // and needs no fix-up table.
    REQUIRE(out.materials.size() == 1);
    CHECK(out.geosets[0].materialId == 0);
}
