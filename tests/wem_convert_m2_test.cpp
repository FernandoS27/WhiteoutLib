// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// WEM v3 P5 — `M2Converter` on hand-built models.
///
/// The three claims a green parse would not check: the skin profile's vertex
/// indirection is flattened (and the *indices* address the skin, not the model),
/// an attachment becomes a child of its bone rather than a loose model-space
/// point, and a multi-pass submesh keeps every batch's material even though only
/// one of them can be the section's.

#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/wem/converters.h>

using namespace whiteout;
using namespace whiteout::models::wem;

namespace {

/// Two bones, six global vertices of which a skin uses the last four, one
/// submesh, one batch.
m2::Model makeModel() {
    m2::Model model;
    model.modelName = "creature";
    model.bounding.minimum = Vector3f{-1, -1, 0};
    model.bounding.maximum = Vector3f{1, 1, 0};

    m2::Texture texture;
    texture.filename = "creature/body.blp";
    texture.type = 0;
    model.textures.push_back(texture);

    m2::Bone root;
    root.parentBoneId = -1;
    root.pivot = Vector3f{0, 0, 0};
    m2::Bone child;
    child.parentBoneId = 0;
    child.pivot = Vector3f{0, 0, 5};
    model.bones.push_back(root);
    model.bones.push_back(child);

    // Two vertices nothing references, so the flattening is visible: a skin
    // vertex index of 0 must reach global vertex 2, not global vertex 0.
    for (int i = 0; i < 2; ++i) {
        m2::Vertex filler;
        filler.position = Vector3f{99, 99, 99};
        model.vertices.push_back(filler);
    }
    const Vector3f corners[4] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
    for (const Vector3f& corner : corners) {
        m2::Vertex vertex;
        vertex.position = corner;
        vertex.normal = Vector3f{0, 0, 1};
        vertex.texCoords[0] = Vector2f{0.25f, 0.5f};
        vertex.boneIndices = {1, 0, 0, 0};
        vertex.boneWeights = {255, 0, 0, 0};
        model.vertices.push_back(vertex);
    }

    m2::Material material;
    material.blendingMode = 0;
    model.materials.push_back(material);
    model.textureCombos = {0};
    model.textureCoordCombos = {0};
    model.textureWeightCombos = {0};
    model.textureTransformCombos = {0};

    m2::SkinProfile skin;
    skin.vertices = {2, 3, 4, 5};
    skin.indices = {0, 1, 2, 0, 2, 3};
    m2::SkinSection submesh;
    submesh.skinSectionId = 7;
    submesh.vertexStart = 0;
    submesh.vertexCount = 4;
    submesh.indexStart = 0;
    submesh.indexCount = 6;
    skin.submeshes.push_back(submesh);

    m2::Batch batch;
    batch.skinSectionIndex = 0;
    batch.materialIndex = 0;
    batch.textureCount = 1;
    skin.batches.push_back(batch);
    model.skinProfiles.push_back(skin);

    m2::Attachment attachment;
    attachment.id = 12;
    attachment.boneId = 1;
    attachment.position = Vector3f{0, 0, 8};
    model.attachments.push_back(attachment);

    return model;
}

} // namespace

TEST_CASE("wem m2 import flattens the skin's vertex indirection", "[wem][convert][m2]") {
    const M2Converter converter;
    Result<Document> result = converter.fromM2(makeModel());
    REQUIRE(result.ok());
    const Model& model = result->models.front();

    REQUIRE(model.meshes.size() == 1);
    const Mesh& mesh = model.meshes[0];
    CHECK(mesh.vertexCount() == 4);
    CHECK(mesh.faceCount() == 2);

    // Skin vertex 0 is global vertex 2, which is the quad's first corner —
    // reading the indices as global would have picked up the (99,99,99) filler.
    const std::span<const Vector3f> positions =
        mesh.attributes.get<Vector3f>(geom::names::kPosition, geom::Domain::Vertex);
    REQUIRE(positions.size() == 4);
    CHECK(positions[0].x == 0.0f);
    CHECK(positions[0].y == 0.0f);
}

TEST_CASE("wem m2 attachments become bone children", "[wem][convert][m2][nodes]") {
    const M2Converter converter;
    Result<Document> result = converter.fromM2(makeModel());
    REQUIRE(result.ok());
    const NodeTree& nodes = result->models.front().nodes;

    // Two bones, then the attachment.
    REQUIRE(nodes.size() == 3);
    const Node& attachment = nodes.nodes[2];
    CHECK(attachment.kind == NodeKind::Attachment);
    CHECK(attachment.parent == 1);
    // The equip slot is the point of an M2 attachment, so it has to survive.
    CHECK(attachment.native.value("attachmentId") == 12);
    // Model-space z=8 under a bone pivoted at z=5 is a local z=3, and the world
    // bind pose puts it back where the file had it.
    CHECK(attachment.local.translation.z == 3.0f);
    CHECK(nodes.worldBind(2).translation.z == 8.0f);
}

TEST_CASE("wem m2 keeps a multi-pass batch's material", "[wem][convert][m2][materials]") {
    m2::Model source = makeModel();
    m2::Batch second = source.skinProfiles[0].batches[0];
    second.materialLayer = 1;
    source.skinProfiles[0].batches.push_back(second);

    const M2Converter converter;
    Result<Document> result = converter.fromM2(source);
    REQUIRE(result.ok());
    const Model& model = result->models.front();

    // Both batches are slots and both are bound; the section draws the base one.
    REQUIRE(model.materialSlots.size() == 2);
    CHECK(Resolve(model, 0, ProfileId::Wow) != nullptr);
    CHECK(Resolve(model, 1, ProfileId::Wow) != nullptr);
    REQUIRE(model.meshes[0].sections.size() == 1);
    CHECK(model.meshes[0].sections[0].materialSlot == 0);
    // And the second pass says out loud that nothing draws it.
    CHECK(result.diagnostics.countOf(DiagCode::MaterialSlotUnused) == 1);
}

TEST_CASE("wem m2 has no byte import and says so", "[wem][convert][m2]") {
    const M2Converter converter;
    CHECK_FALSE(converter.supportsImport());
    const u8 bytes[4] = {'M', 'D', '2', '0'};
    Result<Document> result = converter.importFromBytes(std::span<const u8>(bytes, 4));
    CHECK_FALSE(result.ok());
    CHECK(result.diagnostics.countOf(DiagCode::OperationUnsupported) == 1);
}

TEST_CASE("wem m2 round trip keeps geometry and the bone join", "[wem][convert][m2]") {
    const M2Converter converter;
    Result<Document> imported = converter.fromM2(makeModel());
    REQUIRE(imported.ok());

    Result<m2::Model> exported = converter.toM2(*imported, ProfileId::Wow);
    REQUIRE(exported.ok());
    const m2::Model& out = *exported;

    CHECK(out.modelName == "creature");
    REQUIRE(out.bones.size() == 2);
    CHECK(out.bones[1].parentBoneId == 0);
    CHECK(out.bones[1].pivot.z == 5.0f);
    REQUIRE(out.attachments.size() == 1);
    CHECK(out.attachments[0].id == 12);
    CHECK(out.attachments[0].boneId == 1);
    CHECK(out.attachments[0].position.z == 8.0f);

    REQUIRE(out.skinProfiles.size() == 1);
    CHECK(out.skinProfiles[0].vertices.size() == 4);
    CHECK(out.skinProfiles[0].indices.size() == 6);
    REQUIRE(out.skinProfiles[0].submeshes.size() == 1);
    // The submesh id rides `native` rather than being re-derived, so it comes
    // back as 7 and not as an ordinal.
    CHECK(out.skinProfiles[0].submeshes[0].skinSectionId == 7);
    // The export writes only the four vertices the skin used; the two fillers
    // nothing referenced are gone, which is a de-duplication and not a loss.
    CHECK(out.vertices.size() == 4);
}
