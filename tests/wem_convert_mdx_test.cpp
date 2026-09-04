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
#include <whiteout/models/wem/geometry/builder.h>

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

/// One mesh, `sections` disjoint quads, one material slot each, every vertex
/// bound to the bone whose index is the section's.
///
/// The shape MDX cannot come from and every other format does: an `.m2` skin is
/// one mesh per batch, an `.m3` division one per region, a Diablo III
/// appearance one mesh of thirty sub-objects.
Document makeSectionedDocument(u32 sections) {
    geom::MeshBuilder builder;
    for (u32 s = 0; s < sections; ++s) {
        MeshSection section;
        section.name = "part_" + std::to_string(s);
        section.materialSlot = s;
        section.profiles = ProfileBit(ProfileId::Wc3Classic);
        builder.addSection(std::move(section));
    }
    for (u32 s = 0; s < sections; ++s) {
        const f32 x = static_cast<f32>(s) * 4.0f;
        const geom::VertexId a = builder.addVertex(Vector3f{x, 0, 0});
        const geom::VertexId b = builder.addVertex(Vector3f{x + 1, 0, 0});
        const geom::VertexId c = builder.addVertex(Vector3f{x + 1, 1, 0});
        const geom::VertexId d = builder.addVertex(Vector3f{x, 1, 0});
        for (const geom::VertexId v : {a, b, c, d}) {
            builder.addInfluence(v, s, 1.0f);
        }
        for (const geom::FaceId face :
             {builder.addTriangle(a, b, c, s), builder.addTriangle(a, c, d, s)}) {
            for (u32 corner = 0; corner < 3; ++corner) {
                builder.setCornerAttr(face, corner, geom::names::kNormal, Vector3f{0, 0, 1});
                builder.setCornerAttr(face, corner, geom::names::uv(0), Vector2f{0, 0});
            }
        }
    }

    Document document;
    document.declare(ProfileId::Wc3Classic);
    document.defaultProfile = ProfileId::Wc3Classic;
    document.name = "sectioned";
    document.textures.push_back(TextureRef{});

    Model model;
    model.name = "sectioned";
    model.meshes.push_back(builder.build().mesh);

    ProfileMaterialSet set;
    set.profile = ProfileId::Wc3Classic;
    set.looks.looks.push_back(Look{});
    for (u32 s = 0; s < sections; ++s) {
        model.addSlot("slot_" + std::to_string(s));
    }
    set.resizeBindings(model.materialSlots.size());
    for (u32 s = 0; s < sections; ++s) {
        Material material;
        material.name = "slot_" + std::to_string(s);
        set.slotBindings[s].byLook[0] = static_cast<u32>(set.materials.size());
        set.materials.push_back(std::move(material));
    }
    model.profileSets.push_back(std::move(set));

    for (u32 s = 0; s < sections; ++s) {
        Node bone;
        bone.name = "bone_" + std::to_string(s);
        bone.kind = NodeKind::Bone;
        bone.parent = kInvalidNode;
        model.nodes.nodes.push_back(std::move(bone));
    }

    document.models.push_back(std::move(model));
    return document;
}

} // namespace

TEST_CASE("wem mdx a mesh of several sections writes a geoset each",
          "[wem][convert][mdx][geometry]") {
    const Document document = makeSectionedDocument(3);
    const MdxConverter converter;
    Result<mdx::Model> exported = converter.toMdx(document, ProfileId::Wc3Classic, 800);
    REQUIRE(exported.ok());
    const mdx::Model& out = *exported;

    // A geoset carries ONE materialId, so a mesh of three sections is three
    // geosets. Writing one per mesh drew all of them with section 0's material.
    REQUIRE(out.geosets.size() == 3);
    for (u32 g = 0; g < 3; ++g) {
        const mdx::Geoset& geoset = out.geosets[g];
        CHECK(geoset.materialId == g);
        CHECK(geoset.lodName == "part_" + std::to_string(g));
        // Its OWN vertex slice: four corners, and every face index inside it.
        CHECK(geoset.vertexPositions.size() == 4);
        REQUIRE(geoset.faces.size() == 6);
        for (const u16 corner : geoset.faces) {
            CHECK(corner < geoset.vertexPositions.size());
        }
        // Disjoint, and in the section's own place along x.
        for (const Vector3f& position : geoset.vertexPositions) {
            CHECK(position.x >= static_cast<f32>(g) * 4.0f - 0.001f);
            CHECK(position.x <= static_cast<f32>(g) * 4.0f + 1.001f);
        }
        // A bound derived from the section, not inherited from the mesh.
        CHECK(geoset.extent.maximum.x <= static_cast<f32>(g) * 4.0f + 1.001f);
    }
}

TEST_CASE("wem mdx writes no inheritance a node did not claim", "[wem][convert][mdx][nodes]") {
    Document document = makeSectionedDocument(2);
    // What every `.m3` and most `.m2` bones carry: nothing. Warcraft III honours
    // all three `DontInherit*` bits, so one invented here detaches the bone from
    // its parent and takes the subtree with it.
    document.models[0].nodes.nodes[1].parent = 0;

    const MdxConverter converter;
    Result<mdx::Model> exported = converter.toMdx(document, ProfileId::Wc3Classic, 800);
    REQUIRE(exported.ok());
    REQUIRE(exported->bones.size() == 2);
    for (const mdx::Bone& bone : exported->bones) {
        CHECK(bone.node.flags == mdx::Node::NodeFlag::None);
    }

    // And a bag written by another format does not become MDX flags. Three
    // converters had picked the bare name `flagBits` for three unrelated bit
    // vocabularies, so `.m3`'s `BoneFlag::Real` reached Warcraft III as
    // `CollisionShape` and `Skinned` as `Attachment`.
    document.models[0].nodes.nodes[0].native.set("m3FlagBits", 0x2A00);
    document.models[0].nodes.nodes[1].native.set("m2FlagBits", 0x2A00);
    exported = converter.toMdx(document, ProfileId::Wc3Classic, 800);
    REQUIRE(exported.ok());
    for (const mdx::Bone& bone : exported->bones) {
        CHECK(bone.node.flags == mdx::Node::NodeFlag::None);
    }

    // Its own name still round-trips, which is the whole point of the bag.
    document.models[0].nodes.nodes[0].native.set("mdxFlagBits", 0x2000);
    exported = converter.toMdx(document, ProfileId::Wc3Classic, 800);
    REQUIRE(exported.ok());
    CHECK(exported->bones[0].node.flags == mdx::Node::NodeFlag::CollisionShape);
}

TEST_CASE("wem mdx keeps only a replaceable id MDX numbers", "[wem][convert][mdx][textures]") {
    Document document = makeSectionedDocument(1);
    document.textures.clear();
    TextureRef skin;
    skin.key = TexturePath{"skin.blp"};
    skin.path = "skin.blp";
    // World of Warcraft's texture TYPE 11 — a monster's first skin — in the
    // field Warcraft III reads as replaceable 11, a tileset. The adapter takes
    // the replaceable branch before it looks at the name, so the model drew
    // white with a perfectly resolvable key sitting unused beside the slot.
    skin.replaceableId = 11;
    document.textures.push_back(skin);

    const MdxConverter converter;

    document.defaultProfile = ProfileId::Wow;
    Result<mdx::Model> fromWow = converter.toMdx(document, ProfileId::Wc3Classic, 800);
    REQUIRE(fromWow.ok());
    REQUIRE(fromWow->textures.size() == 1);
    CHECK(fromWow->textures[0].replaceableId == 0);
    CHECK(fromWow->textures[0].fileName == "skin.blp");

    // A document Warcraft III authored keeps them: there the number IS MDX's,
    // and `defaultProfile` is the authoring profile a derive does not move.
    document.defaultProfile = ProfileId::Wc3Classic;
    Result<mdx::Model> fromMdx = converter.toMdx(document, ProfileId::Wc3Classic, 800);
    REQUIRE(fromMdx.ok());
    REQUIRE(fromMdx->textures.size() == 1);
    CHECK(fromMdx->textures[0].replaceableId == 11);
}

TEST_CASE("wem mdx a hidden section becomes a static alpha of zero",
          "[wem][convert][mdx][geometry]") {
    Document document = makeSectionedDocument(3);
    document.models[0].meshes[0].sections[1].flags |= SectionFlags::Hidden;

    const MdxConverter converter;
    Result<mdx::Model> exported = converter.toMdx(document, ProfileId::Wc3Classic, 800);
    REQUIRE(exported.ok());

    // The only per-geoset visibility MDX has, and what Warcraft III itself uses
    // to keep an alternate body part out of the frame.
    REQUIRE(exported->geosetAnimations.size() == 1);
    const mdx::GeosetAnimation& animation = exported->geosetAnimations.front();
    CHECK(animation.geosetId == 1);
    CHECK(animation.alpha == 0.0f);
    CHECK_FALSE(animation.alphaTracks.isUsed);

    // And back: a static alpha is not a track, so nothing else in the import
    // would have seen it.
    Result<Document> reimported = converter.fromMdx(*exported);
    REQUIRE(reimported.ok());
    REQUIRE(reimported->models.front().meshes.size() == 3);
    CHECK_FALSE(
        hasFlag(reimported->models.front().meshes[0].sections[0].flags, SectionFlags::Hidden));
    CHECK(hasFlag(reimported->models.front().meshes[1].sections[0].flags, SectionFlags::Hidden));
    CHECK_FALSE(
        hasFlag(reimported->models.front().meshes[2].sections[0].flags, SectionFlags::Hidden));
}

TEST_CASE("wem mdx writes the skinning encoding the target version reads",
          "[wem][convert][mdx][skin]") {
    const Document document = makeSectionedDocument(2);
    const MdxConverter converter;

    SECTION("800 writes groups and nothing else") {
        Result<mdx::Model> exported = converter.toMdx(document, ProfileId::Wc3Classic, 800);
        REQUIRE(exported.ok());
        const mdx::Geoset& geoset = exported->geosets.at(1);
        // `SKIN` is written only above 800, so at 800 the group encoding is the
        // only skinning the file has -- and it must be there, or the geoset
        // ships a GNDX of zero entries and poses at nothing.
        CHECK(geoset.skinData.empty());
        CHECK(geoset.vertexGroups.size() == geoset.vertexPositions.size());
        // Every vertex of this section binds the one bone, so one group.
        REQUIRE(geoset.matrixGroups.size() == 1);
        CHECK(geoset.matrixGroups[0] == 1);
        REQUIRE(geoset.matrixIndices.size() == 1);
        CHECK(geoset.matrixIndices[0] == 1);
        for (const u8 group : geoset.vertexGroups) {
            CHECK(group == 0);
        }
    }

    SECTION("1000 writes SKIN over a palette") {
        // The version decides, not the profile: `SKIN` is a chunk the writer
        // gates on 800, and Reforged is the same container at a later one.
        Result<mdx::Model> exported = converter.toMdx(document, ProfileId::Wc3Classic, 1000);
        REQUIRE(exported.ok());
        const mdx::Geoset& geoset = exported->geosets.at(1);
        // `SKIN` indexes `MATS` directly, so `MATS` is the palette and `MTGC`
        // has no choice but one group per bone -- the shape a shipped Reforged
        // file writes.
        CHECK(geoset.skinData.size() == geoset.vertexPositions.size() * 8);
        REQUIRE(geoset.matrixIndices.size() == 1);
        CHECK(geoset.matrixIndices[0] == 1);
        REQUIRE(geoset.matrixGroups.size() == 1);
        CHECK(geoset.matrixGroups[0] == 1);
        CHECK(geoset.vertexGroups.size() == geoset.vertexPositions.size());
        for (std::size_t v = 0; v < geoset.vertexPositions.size(); ++v) {
            CHECK(geoset.skinData[v * 8 + 0] == 0);
            CHECK(geoset.skinData[v * 8 + 4] == 255);
        }
    }
}

TEST_CASE("wem mdx numbers object ids by chunk, not by node order",
          "[wem][convert][mdx][nodes][skin]") {
    Document document = makeSectionedDocument(2);
    Model& model = document.models.front();

    // A helper BETWEEN the two bones -- the shape `RetargetSkeleton` leaves
    // behind when it splits a sheared node, since the stretch parent lands
    // immediately before the bone it stretches.
    Node helper;
    helper.name = "bone_0_stretch";
    helper.kind = NodeKind::Helper;
    helper.resetPayloadForKind();
    helper.parent = 0;
    model.nodes.nodes.insert(model.nodes.nodes.begin() + 1, std::move(helper));
    model.nodes.nodes[2].parent = 1;
    model.nodes.invalidateHierarchy();
    for (geom::Influence& influence : model.meshes.front().skin.influences) {
        if (influence.bone >= 1) {
            ++influence.bone;
        }
    }

    const MdxConverter converter;
    Result<mdx::Model> exported = converter.toMdx(document, ProfileId::Wc3Classic, 800);
    REQUIRE(exported.ok());
    REQUIRE(exported->bones.size() == 2);
    REQUIRE(exported->helpers.size() == 1);

    // MDX numbers a node by the chunk it lands in, so both bones come before
    // the helper whatever order the document holds them in. `MATS` names an
    // object id and every reader takes one for an index into the bone array --
    // ours asks `BoneIndexToNodeIndex` first -- so the two agree only while the
    // bones are 0..n-1. Numbering in node order put the helper at 1 and skinned
    // every vertex of the second bone to the first.
    CHECK(exported->bones[0].node.objectId == 0);
    CHECK(exported->bones[1].node.objectId == 1);
    CHECK(exported->helpers[0].node.objectId == 2);
    CHECK(exported->bones[1].node.parentId == 2); // Still parented through it.

    // `PIVT` is indexed by object id, so it is filled by id and not appended.
    REQUIRE(exported->pivotPoints.size() == 3);

    const mdx::Geoset& second = exported->geosets.at(1);
    REQUIRE(second.matrixIndices.size() == 1);
    CHECK(second.matrixIndices[0] == 1);
}

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
