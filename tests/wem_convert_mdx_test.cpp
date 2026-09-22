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

#include <whiteout/models/mdx/parser.h>
#include <whiteout/models/wem/converters.h>
#include <whiteout/models/wem/geometry/builder.h>
#include <whiteout/models/wem/parser.h>
#include <whiteout/models/wem/retarget.h>
#include <whiteout/models/wem/writer.h>

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

TEST_CASE("wem mdx a texture states the address mode its layers ask for",
          "[wem][convert][mdx][textures]") {
    Document document = makeSectionedDocument(1);
    document.textures.clear();
    TextureRef skin;
    skin.key = TexturePath{"skin.blp"};
    skin.path = "skin.blp";
    document.textures.push_back(skin);

    const auto withWrap = [&document](WrapMode u, WrapMode v) {
        CompositeBody body;
        CompositeLayer layer;
        layer.input.texture = 0;
        layer.input.wrapU = u;
        layer.input.wrapV = v;
        body.layers.push_back(layer);
        document.models[0].profileSets[0].materials[0].InitCommon().body = body;
    };

    const MdxConverter converter;

    // MDX states the address mode on the TEXTURE; everything else states it on
    // the layer, so `TextureRef::flags` is empty on an imported `.m3` -- and an
    // empty flag word reads as clamp, which smears one edge column across every
    // surface whose coordinates leave [0,1]. A Murky's body tiles to u ~ 2.
    document.defaultProfile = ProfileId::Heroes;
    withWrap(WrapMode::Repeat, WrapMode::Repeat);
    Result<mdx::Model> tiled = converter.toMdx(document, ProfileId::Wc3Classic, 800);
    REQUIRE(tiled.ok());
    REQUIRE(tiled->textures.size() == 1);
    CHECK(tiled->textures[0].flags ==
          (mdx::Texture::Flag::WrapWidth | mdx::Texture::Flag::WrapHeight));

    // And a layer that means clamp still gets it: the default is read, not
    // forced.
    withWrap(WrapMode::Clamp, WrapMode::Repeat);
    Result<mdx::Model> clamped = converter.toMdx(document, ProfileId::Wc3Classic, 800);
    REQUIRE(clamped.ok());
    REQUIRE(clamped->textures.size() == 1);
    CHECK(clamped->textures[0].flags == mdx::Texture::Flag::WrapHeight);

    // A document Warcraft III authored keeps its own word, the way it keeps its
    // own replaceable ids: there the field IS MDX's.
    document.defaultProfile = ProfileId::Wc3Classic;
    document.textures[0].flags = static_cast<u32>(mdx::Texture::Flag::WrapWidth);
    withWrap(WrapMode::Repeat, WrapMode::Repeat);
    Result<mdx::Model> kept = converter.toMdx(document, ProfileId::Wc3Classic, 800);
    REQUIRE(kept.ok());
    REQUIRE(kept->textures.size() == 1);
    CHECK(kept->textures[0].flags == mdx::Texture::Flag::WrapWidth);
}

TEST_CASE("wem mdx the address mode is the texture's word, not the layer's",
          "[wem][convert][mdx][textures]") {
    // Warcraft III reads `TEXS`'s flag word and nothing off the layer. Every
    // Reforged file wraps its textures (0x3) and leaves the layer bits clear,
    // which a layer read turned into a clamp: a banshee whose skirt runs to
    // u = -1 sampled one edge column across half its body.
    mdx::Model model = makeModel();
    model.textures[0].flags = mdx::Texture::Flag::WrapWidth;
    const MdxConverter converter;
    Result<Document> imported = converter.fromMdx(model);
    REQUIRE(imported.ok());
    const CompositeBody* body =
        imported->models[0].profileSets[0].materials[0].Common().composite();
    REQUIRE(body != nullptr);
    REQUIRE_FALSE(body->layers.empty());
    CHECK(body->layers[0].input.wrapU == WrapMode::Repeat);
    CHECK(body->layers[0].input.wrapV == WrapMode::Clamp);
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

TEST_CASE("wem mdx a cylinder and a plane keep their two vertices",
          "[wem][convert][mdx][nodes]") {
    // `CLID` stores two vertices for a box, a plane and a cylinder, and a
    // reader takes them by the shape's type rather than by a count. Import read
    // both into the box's corners and export wrote them only for a box, so a
    // cylinder came back with its radius and no ends, and the chunk after it
    // misparsed.
    mdx::Model source = makeModel();
    mdx::CollisionShape cylinder;
    cylinder.node = makeNode("Collision Cylinder01", 2, mdx::Node::NO_PARENT);
    cylinder.type = mdx::CollisionShape::ShapeType::Cylinder;
    cylinder.vertices = {Vector3f{0, 0, -5}, Vector3f{0, 0, 15}};
    cylinder.radius = 3.0f;
    mdx::CollisionShape plane;
    plane.node = makeNode("Collision Plane01", 3, mdx::Node::NO_PARENT);
    plane.type = mdx::CollisionShape::ShapeType::Plane;
    plane.vertices = {Vector3f{-4, -2, 0}, Vector3f{4, 2, 0}};
    source.collisionShapes = {cylinder, plane};
    source.pivotPoints.push_back(Vector3f{1, 2, 3});
    source.pivotPoints.push_back(Vector3f{0, 0, 0});

    const MdxConverter converter;
    Result<Document> imported = converter.fromMdx(source);
    REQUIRE(imported.ok());
    Result<mdx::Model> exported = converter.toMdx(*imported, ProfileId::Wc3Classic, 800);
    REQUIRE(exported.ok());
    REQUIRE(exported->collisionShapes.size() == 2);
    for (std::size_t i = 0; i < 2; ++i) {
        const mdx::CollisionShape& shape = exported->collisionShapes[i];
        const mdx::CollisionShape& from = source.collisionShapes[i];
        CHECK(shape.type == from.type);
        REQUIRE(shape.vertices.size() == 2);
        CHECK(shape.vertices[0] == from.vertices[0]);
        CHECK(shape.vertices[1] == from.vertices[1]);
    }
    CHECK(exported->collisionShapes[0].radius == 3.0f);
}

TEST_CASE("wem mdx a camera keeps where it looks", "[wem][convert][mdx][nodes]") {
    // A camera is its position and its target; WEM kept the first alone, so every
    // exported camera looked at the model's origin.
    mdx::Model source = makeModel();
    mdx::Camera camera;
    camera.name = "Portrait";
    camera.position = Vector3f{120, -40, 90};
    camera.targetPosition = Vector3f{0, 5, 60};
    camera.fieldOfView = 0.7f;
    camera.nearClippingPlane = 8.0f;
    camera.farClippingPlane = 2000.0f;
    source.cameras.push_back(camera);

    const MdxConverter converter;
    Result<Document> imported = converter.fromMdx(source);
    REQUIRE(imported.ok());
    const NodeTree& nodes = imported->models.front().nodes;
    REQUIRE_FALSE(nodes.ofKind(NodeKind::Camera).empty());
    const Node& node = nodes.nodes[nodes.ofKind(NodeKind::Camera)[0]];
    CHECK(std::get<CameraPayload>(node.payload).target == camera.targetPosition);

    Result<mdx::Model> exported = converter.toMdx(*imported, ProfileId::Wc3Classic, 800);
    REQUIRE(exported.ok());
    REQUIRE(exported->cameras.size() == 1);
    const mdx::Camera& out = exported->cameras[0];
    CHECK(out.name == "Portrait");
    CHECK(out.position == camera.position);
    CHECK(out.targetPosition == camera.targetPosition);
    CHECK(out.fieldOfView == 0.7f);
    CHECK(out.farClippingPlane == 2000.0f);
}

TEST_CASE("wem mdx a light keeps its ambient term and its shadow", "[wem][convert][mdx][nodes]") {
    // The import kept the two intensities in thousandths and not the colour,
    // and the export wrote none of the three: a rebuilt light had no ambient.
    mdx::Model source = makeModel();
    source.pivotPoints.push_back(Vector3f{0, 0, 0});
    source.pivotPoints.push_back(Vector3f{0, 0, 0});
    mdx::Light lamp;
    lamp.node = makeNode("lamp", 2, 0);
    lamp.ambientColor = Vector3f{0.2f, 0.4f, 0.6f};
    lamp.ambientIntensity = 0.75f;
    lamp.shadowIntensity = 0.25f;
    source.lights.push_back(lamp);
    // 0.7f is 0.69999999: a truncation to thousandths kept 0.699.
    mdx::Light dim;
    dim.node = makeNode("dim", 3, 0);
    dim.ambientIntensity = 0.7f;
    source.lights.push_back(dim);

    const MdxConverter converter;
    Result<Document> imported = converter.fromMdx(source);
    REQUIRE(imported.ok());
    Result<mdx::Model> exported = converter.toMdx(*imported, ProfileId::Wc3Classic, 1200);
    REQUIRE(exported.ok());
    REQUIRE(exported->lights.size() == 2);
    for (const mdx::Light& in : source.lights) {
        const mdx::Light* out = nullptr;
        for (const mdx::Light& candidate : exported->lights) {
            if (candidate.node.name == in.node.name) {
                out = &candidate;
            }
        }
        REQUIRE(out != nullptr);
        CHECK(out->ambientColor == in.ambientColor);
        CHECK(out->ambientIntensity == in.ambientIntensity);
        CHECK(out->shadowIntensity == in.shadowIntensity);
    }
}

TEST_CASE("wem mdx a light keeps 3.0's shadow range and falloff", "[wem][convert][mdx][nodes]") {
    // WEM had no field for v1300's shadow casting or v1600's falloff, so every
    // Reforged light an edit rebuilt lost its shadow and took the game's
    // pre-1600 falloff (39-102 of the 306 lights in 1,382 shipped v1800 files).
    mdx::Model source = makeModel();
    source.version = 1800;
    mdx::Sequence stand;
    stand.name = "Stand";
    stand.intervalStart = 0;
    stand.intervalEnd = 1000;
    source.sequences.push_back(stand);
    source.pivotPoints.push_back(Vector3f{0, 0, 40});
    mdx::Light lamp;
    lamp.node = makeNode("lamp", 2, 0);
    lamp.shadowCasting = true;
    lamp.shadowCastingStart = 12.5f;
    lamp.shadowCastingEnd = 900.0f;
    lamp.quadraticFalloff = 0.002f;
    lamp.linearFalloff = 0.03f;
    lamp.damping = 0.0004f;
    const auto keyed = [](f32 a, f32 b) {
        mdx::Track<f32> track;
        track.isUsed = true;
        track.interpolationType = mdx::InterpolationType::Linear;
        track.timestamps = {0, 1000};
        track.keys_data = {a, b};
        track.keyCount = 2;
        return track;
    };
    lamp.shadowCastingStartTracks = keyed(12.5f, 20.0f);
    lamp.shadowCastingEndTracks = keyed(900.0f, 450.0f);
    lamp.quadraticFalloffTracks = keyed(0.002f, 0.004f);
    lamp.linearFalloffTracks = keyed(0.03f, 0.0f);
    lamp.dampingTracks = keyed(0.0004f, 0.0008f);
    source.lights.push_back(lamp);

    const MdxConverter converter;
    Result<Document> imported = converter.fromMdx(source);
    REQUIRE(imported.ok());
    const Model& model = imported->models[0];
    u32 node = kInvalidNode;
    for (u32 n = 0; n < model.nodes.size(); ++n) {
        if (model.nodes.nodes[n].name == "lamp") {
            node = n;
        }
    }
    REQUIRE(node != kInvalidNode);
    const auto& payload = std::get<LightPayload>(model.nodes.nodes[node].payload);
    CHECK(payload.shadowCasting);
    CHECK(payload.shadowCastingStart == 12.5f);
    CHECK(payload.shadowCastingEnd == 900.0f);
    CHECK(payload.quadraticFalloff == 0.002f);
    CHECK(payload.linearFalloff == 0.03f);
    CHECK(payload.damping == 0.0004f);
    for (const Channel channel : {Channel::ShadowCastingStart, Channel::ShadowCastingEnd,
                                  Channel::QuadraticFalloff, Channel::LinearFalloff,
                                  Channel::Damping}) {
        INFO(ToString(channel));
        u32 found = 0;
        for (const AnimChannel& declared : model.animChannels.channels) {
            found += declared.target.node == node && declared.target.channel == channel ? 1 : 0;
        }
        CHECK(found == 1u);
    }

    // Through the file: `NODE` v7 carries the six fields.
    Writer writer;
    const std::vector<u8> bytes = writer.write(*imported);
    Parser parser;
    std::optional<Document> reread = parser.parse(std::span<const u8>(bytes.data(), bytes.size()));
    REQUIRE(reread.has_value());

    Result<mdx::Model> exported = converter.toMdx(*reread, ProfileId::Wc3Classic, 1800);
    REQUIRE(exported.ok());
    REQUIRE(exported->lights.size() == 1u);
    const mdx::Light& out = exported->lights[0];
    CHECK(out.shadowCasting);
    CHECK(out.shadowCastingStart == lamp.shadowCastingStart);
    CHECK(out.shadowCastingEnd == lamp.shadowCastingEnd);
    CHECK(out.quadraticFalloff == lamp.quadraticFalloff);
    CHECK(out.linearFalloff == lamp.linearFalloff);
    CHECK(out.damping == lamp.damping);
    const auto same = [](const mdx::Track<f32>& a, const mdx::Track<f32>& b) {
        return a.isUsed == b.isUsed && a.timestamps == b.timestamps && a.keys_data == b.keys_data;
    };
    CHECK(same(out.shadowCastingStartTracks, lamp.shadowCastingStartTracks));
    CHECK(same(out.shadowCastingEndTracks, lamp.shadowCastingEndTracks));
    CHECK(same(out.quadraticFalloffTracks, lamp.quadraticFalloffTracks));
    CHECK(same(out.linearFalloffTracks, lamp.linearFalloffTracks));
    CHECK(same(out.dampingTracks, lamp.dampingTracks));
}

TEST_CASE("wem mdx a file is written at its profile's version, and Reforged's is 3.0's",
          "[wem][convert][mdx]") {
    CHECK(MdxFileVersion(ProfileId::Wc3Classic) == 800);
    CHECK(MdxFileVersion(ProfileId::Wc3Reforged) == 1800);
    CHECK(MdxFileVersion(ProfileId::Sc2) == 0);

    // A light's falloff is what a file older than v1600 has no room for.
    mdx::Model source = makeModel();
    source.pivotPoints.push_back(Vector3f{0, 0, 40});
    mdx::Light lamp;
    lamp.node = makeNode("lamp", 2, 0);
    lamp.damping = 0.0004f;
    source.lights.push_back(lamp);
    const MdxConverter converter;
    Result<Document> imported = converter.fromMdx(source);
    REQUIRE(imported.ok());
    Document& document = *imported.value;
    if (!document.carries(ProfileId::Wc3Reforged)) {
        REQUIRE(DeriveProfile(document, ProfileId::Wc3Classic, ProfileId::Wc3Reforged).ok);
    }

    // Asked for no version, which is what every caller that does not care asks.
    const auto written = [&](ProfileId profile) {
        Result<std::vector<u8>> bytes = converter.exportToBytes(document, profile);
        REQUIRE(bytes.ok());
        mdx::Parser parser(mdx::Parser::UpgradeMode::PreserveOriginal);
        return parser.parse(std::span<const u8>(bytes->data(), bytes->size()));
    };
    const mdx::Model reforged = written(ProfileId::Wc3Reforged);
    CHECK(reforged.version == 1800);
    REQUIRE(reforged.lights.size() == 1u);
    CHECK(reforged.lights[0].damping == 0.0004f);
    CHECK(written(ProfileId::Wc3Classic).version == 800);
    const Result<mdx::Model> model = converter.toMdx(document, ProfileId::Wc3Reforged);
    REQUIRE(model.ok());
    CHECK(model->version == 1800);
}

TEST_CASE("wem mdx a light from before 1600 keeps the falloff the game gives it",
          "[wem][convert][mdx][nodes]") {
    // A light with no falloff of its own plays the game's substitutes; a light
    // made from nothing starts at the same numbers, not at a falloff of zero.
    const LightPayload made;
    CHECK(made.quadraticFalloff == 0.0005f);
    CHECK(made.linearFalloff == 0.0f);
    CHECK(made.damping == 0.00001f);
    CHECK_FALSE(made.shadowCasting);

    mdx::Model source = makeModel();
    source.pivotPoints.push_back(Vector3f{0, 0, 0});
    mdx::Light lamp;
    lamp.node = makeNode("lamp", 2, 0);
    source.lights.push_back(lamp);
    const MdxConverter converter;
    Result<Document> imported = converter.fromMdx(source);
    REQUIRE(imported.ok());
    Result<mdx::Model> exported = converter.toMdx(*imported, ProfileId::Wc3Classic, 1800);
    REQUIRE(exported.ok());
    REQUIRE(exported->lights.size() == 1u);
    CHECK(exported->lights[0].quadraticFalloff == 0.0005f);
    CHECK(exported->lights[0].damping == 0.00001f);
}

TEST_CASE("wem mdx import produces one model with a classic set", "[wem][convert][mdx]") {
    // A v800 file: an SD model's format.
    mdx::Model source = makeModel();
    source.version = 800;
    const MdxConverter converter;
    Result<Document> result = converter.fromMdx(source);
    REQUIRE(result.ok());
    const Document& document = *result;

    CHECK(document.name == "test");
    CHECK(document.space == CoordSpace::Blizzard);
    REQUIRE(document.models.size() == 1);
    REQUIRE(document.textures.size() == 1);
    CHECK(document.textures[0].path == "textures/body.blp");

    // Only classic: an SD model's file, so no Reforged set is invented for it.
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

TEST_CASE("wem mdx an HD file's SD geoset draws in both profiles", "[wem][convert][mdx]") {
    mdx::Model source = makeModel();
    // A v1200 file: an SD material, then a second material with HD layers only,
    // and a second geoset that uses it.
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

    // An HD model uses SD materials too, so the SD geoset draws in both
    // profiles; a classic model uses SD alone, so the HD one draws only in
    // Reforged.
    CHECK(HasProfile(model.meshes[0].sections[0].profiles, ProfileId::Wc3Classic));
    CHECK(HasProfile(model.meshes[0].sections[0].profiles, ProfileId::Wc3Reforged));
    CHECK(HasProfile(model.meshes[1].sections[0].profiles, ProfileId::Wc3Reforged));
    CHECK_FALSE(HasProfile(model.meshes[1].sections[0].profiles, ProfileId::Wc3Classic));

    // The classic set binds the SD slot alone; the Reforged set binds both.
    CHECK(Resolve(model, 0, ProfileId::Wc3Classic) != nullptr);
    CHECK(Resolve(model, 1, ProfileId::Wc3Classic) == nullptr);
    CHECK(Resolve(model, 0, ProfileId::Wc3Reforged) != nullptr);
    CHECK(Resolve(model, 1, ProfileId::Wc3Reforged) != nullptr);

    // And the Reforged export keeps the SD material SD.
    Result<mdx::Model> exported = converter.toMdx(document, ProfileId::Wc3Reforged);
    REQUIRE(exported.ok());
    REQUIRE(exported->materials.size() == 2);
    REQUIRE_FALSE(exported->materials[0].layers.empty());
    CHECK_FALSE(exported->materials[0].layers[0].is_hd);
    CHECK(exported->materials[0].layers[0].shader == mdx::Layer::ShaderType::SD);
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

TEST_CASE("wem mdx a node that drops its parent's position keeps its pivot whole",
          "[wem][convert][mdx][nodes]") {
    // Warcraft III puts every node on its pivot at rest, whatever its flags.
    // `worldBind` composes nothing of the parent's position onto a
    // DontInheritTranslation node or a ModelSpace one, so a pivot difference
    // there would put it `parentPivot` short of where the game draws it, and
    // every node under it with it.
    mdx::Model source = makeModel();
    source.pivotPoints[0] = Vector3f{0, 0, 4};
    mdx::Bone held;
    held.node = makeNode("held", 2, 1);
    held.node.flags = mdx::Node::NodeFlag::DontInheritTranslation;
    mdx::Bone under;
    under.node = makeNode("under", 3, 2);
    mdx::Bone sparks;
    sparks.node = makeNode("sparks", 4, 1);
    sparks.node.flags = mdx::Node::NodeFlag::ModelSpace;
    source.bones.push_back(held);
    source.bones.push_back(under);
    source.bones.push_back(sparks);
    source.pivotPoints.push_back(Vector3f{2, 0, 25});
    source.pivotPoints.push_back(Vector3f{2, 1, 30});
    source.pivotPoints.push_back(Vector3f{-3, 0, 12});

    const MdxConverter converter;
    Result<Document> result = converter.fromMdx(source);
    REQUIRE(result.ok());
    const NodeTree& nodes = result->models.front().nodes;
    REQUIRE(nodes.size() == 5);
    for (u32 n = 0; n < nodes.size(); ++n) {
        INFO(nodes.nodes[n].name);
        const Vector3f world = nodes.worldBind(n).translation;
        CHECK(world.x == nodes.nodes[n].pivot.x);
        CHECK(world.y == nodes.nodes[n].pivot.y);
        CHECK(world.z == nodes.nodes[n].pivot.z);
    }

    // The export writes the pivots, not the locals, so the file is unchanged.
    Result<mdx::Model> exported = converter.toMdx(*result, ProfileId::Wc3Classic);
    REQUIRE(exported.ok());
    REQUIRE(exported->pivotPoints.size() == source.pivotPoints.size());
    for (std::size_t i = 0; i < source.pivotPoints.size(); ++i) {
        INFO("pivot " << i);
        CHECK(exported->pivotPoints[i].x == source.pivotPoints[i].x);
        CHECK(exported->pivotPoints[i].y == source.pivotPoints[i].y);
        CHECK(exported->pivotPoints[i].z == source.pivotPoints[i].z);
    }
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
    // A v800 file carries no Reforged set.
    mdx::Model source = makeModel();
    source.version = 800;
    const MdxConverter converter;
    Result<Document> imported = converter.fromMdx(source);
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

// ---------------------------------------------------------------------------
// The bone's link to its geoset, the flags word, the tint API and the geoset
// numbering (EDIT_MODE_MESH_PLAN.md L1)
// ---------------------------------------------------------------------------

namespace {

constexpr u32 kSentinel = mdx::Bone::MULTIPLE_GEOSETS;

/// The geoset a bone's gate hides with, resolved as the renderer and the game
/// resolve it (`mdx_model_adapter.cpp`, `IAnimCreateObjects`): through the
/// record, with the bone's own `geosetId` read only against the sentinel.
u32 gateGeosetOf(const mdx::Model& model, const mdx::Bone& bone) {
    if (bone.geosetId == kSentinel || bone.geosetAnimationId >= model.geosetAnimations.size()) {
        return kInvalidIndex;
    }
    const u32 geoset = model.geosetAnimations[bone.geosetAnimationId].geosetId;
    return geoset < model.geosets.size() ? geoset : kInvalidIndex;
}

const mdx::GeosetAnimation* recordFor(const mdx::Model& model, u32 geoset) {
    for (const mdx::GeosetAnimation& record : model.geosetAnimations) {
        if (record.geosetId == geoset) {
            return &record;
        }
    }
    return nullptr;
}

mdx::GeosetAnimation::Flag flagsOf(u32 word) {
    return static_cast<mdx::GeosetAnimation::Flag>(word);
}

/// Four quads, four bones, and a geoset-animation table whose order is NOT the
/// geosets': record 0 names geoset 2 and record 1 geoset 0, so a raw index read
/// as a geoset, or carried through a rebuilt table, lands on the wrong one.
///
/// - bone 0: record 0, so geoset 2.
/// - bone 1: `geosetId` 1 but record 1, so geoset 0 -- one of the 44 shipped
///   bones whose two fields disagree.
/// - bone 2: names geoset 1 and no record: no gate.
/// - bone 3: no link at all.
///
/// Geoset 2's record carries `DropShadow` and the unnamed bits 0x18; geoset 3's
/// carries a flags word of 0 and nothing else.
mdx::Model makeGatedModel() {
    mdx::Model model = makeModel();
    model.version = 800;
    const mdx::Geoset quad = model.geosets.front();
    model.geosets.clear();
    for (u32 g = 0; g < 4; ++g) {
        mdx::Geoset geoset = quad;
        geoset.lodName = "geoset_" + std::to_string(g);
        for (Vector3f& position : geoset.vertexPositions) {
            position.x += static_cast<f32>(g) * 4.0f;
        }
        model.geosets.push_back(geoset);
    }

    mdx::GeosetAnimation shadowed;
    shadowed.geosetId = 2;
    shadowed.color = Vector3f{0.5f, 0.25f, 1.0f};
    shadowed.flags = flagsOf(0x1B); // DropShadow | Color | 0x18
    mdx::GeosetAnimation half;
    half.geosetId = 0;
    half.alpha = 0.5f;
    half.flags = mdx::GeosetAnimation::Flag::Color;
    mdx::GeosetAnimation bare;
    bare.geosetId = 3;
    bare.flags = flagsOf(0);
    model.geosetAnimations = {shadowed, half, bare};

    model.bones.clear();
    const auto bone = [&](const char* name, u32 objectId, u32 geosetId, u32 record) {
        mdx::Bone b;
        b.node = makeNode(name, objectId, mdx::Node::NO_PARENT);
        b.geosetId = geosetId;
        b.geosetAnimationId = record;
        model.bones.push_back(b);
    };
    bone("byRecord", 0, 2, 0);
    bone("disagreeing", 1, 1, 1);
    bone("noRecord", 2, 1, kSentinel);
    bone("unlinked", 3, kSentinel, kSentinel);
    model.pivotPoints = {Vector3f{0, 0, 0}, Vector3f{0, 0, 1}, Vector3f{0, 0, 2},
                         Vector3f{0, 0, 3}};
    return model;
}

u32 boneNodeNamed(const Document& document, const std::string& name) {
    const NodeTree& nodes = document.models.front().nodes;
    for (u32 i = 0; i < nodes.size(); ++i) {
        if (nodes.nodes[i].name == name) {
            return i;
        }
    }
    return kInvalidIndex;
}

const mdx::Bone* boneNamed(const mdx::Model& model, const std::string& name) {
    for (const mdx::Bone& bone : model.bones) {
        if (bone.node.name == name) {
            return &bone;
        }
    }
    return nullptr;
}

std::vector<std::string> keysOf(const NativeBag& bag) {
    std::vector<std::string> keys;
    for (const NativeBag::Entry& entry : bag.entries) {
        keys.push_back(entry.name);
    }
    return keys;
}

} // namespace

TEST_CASE("wem mdx a bone gate is resolved through its record", "[wem][convert][mdx][gate]") {
    const mdx::Model source = makeGatedModel();
    const MdxConverter converter;
    Result<Document> imported = converter.fromMdx(source);
    REQUIRE(imported.ok());

    const auto gateMesh = [&](const char* name) {
        const u32 node = boneNodeNamed(*imported, name);
        REQUIRE(node != kInvalidIndex);
        return std::get<BonePayload>(imported->models[0].nodes.nodes[node].payload).gateMesh;
    };
    CHECK(gateMesh("byRecord") == 2u);
    CHECK(gateMesh("disagreeing") == 0u);
    CHECK(gateMesh("noRecord") == kInvalidIndex);
    CHECK(gateMesh("unlinked") == kInvalidIndex);

    // The raw pair is not carried: it would name records of a table the export
    // rebuilds.
    for (const Node& node : imported->models[0].nodes.nodes) {
        CHECK(node.native.find("geosetAnimationId") == nullptr);
    }

    Result<mdx::Model> exported = converter.toMdx(*imported, ProfileId::Wc3Classic, 800);
    REQUIRE(exported.ok());
    // The rebuilt table puts geoset 0's record first, so bone 0's old index 0
    // would now gate geoset 0: the fixture is only worth running if so.
    REQUIRE_FALSE(exported->geosetAnimations.empty());
    CHECK(exported->geosetAnimations[0].geosetId == 0u);

    for (const mdx::Bone& original : source.bones) {
        INFO(original.node.name);
        const mdx::Bone* written = boneNamed(*exported, original.node.name);
        REQUIRE(written != nullptr);
        CHECK(gateGeosetOf(*exported, *written) == gateGeosetOf(source, original));
    }
}

TEST_CASE("wem mdx a bone with a disagreeing geosetId keeps it", "[wem][convert][mdx][gate]") {
    const MdxConverter converter;
    Result<Document> imported = converter.fromMdx(makeGatedModel());
    REQUIRE(imported.ok());
    const Document& document = *imported;
    // Kept only where the export would not write it back by itself.
    const auto kept = [&](const char* name) {
        return document.models[0].nodes.nodes[boneNodeNamed(document, name)].native.find(
            "geosetId");
    };
    CHECK(kept("byRecord") == nullptr);
    REQUIRE(kept("disagreeing") != nullptr);
    CHECK(kept("disagreeing")->value == 1);
    REQUIRE(kept("noRecord") != nullptr);
    CHECK(kept("noRecord")->value == 1);
    CHECK(kept("unlinked") == nullptr);

    Result<mdx::Model> exported = converter.toMdx(document, ProfileId::Wc3Classic, 800);
    REQUIRE(exported.ok());
    CHECK(boneNamed(*exported, "byRecord")->geosetId == 2u);
    CHECK(boneNamed(*exported, "disagreeing")->geosetId == 1u);
    CHECK(boneNamed(*exported, "noRecord")->geosetId == 1u);
    CHECK(boneNamed(*exported, "noRecord")->geosetAnimationId == kSentinel);
    CHECK(boneNamed(*exported, "unlinked")->geosetId == kSentinel);
    CHECK(boneNamed(*exported, "unlinked")->geosetAnimationId == kSentinel);
}

TEST_CASE("wem mdx a gate on a geoset with no record gets one", "[wem][convert][mdx][gate]") {
    const MdxConverter converter;
    Result<Document> imported = converter.fromMdx(makeGatedModel());
    REQUIRE(imported.ok());
    Document document = *imported;
    // Geoset 1 carries nothing a record would hold.
    const u32 node = boneNodeNamed(document, "noRecord");
    std::get<BonePayload>(document.models[0].nodes.nodes[node].payload).gateMesh = 1;

    Result<mdx::Model> exported = converter.toMdx(document, ProfileId::Wc3Classic, 800);
    REQUIRE(exported.ok());
    const mdx::GeosetAnimation* record = recordFor(*exported, 1);
    REQUIRE(record != nullptr);
    CHECK(record->flags == mdx::GeosetAnimation::Flag::Color);
    CHECK(record->alpha == 1.0f);
    CHECK(gateGeosetOf(*exported, *boneNamed(*exported, "noRecord")) == 1u);
}

TEST_CASE("wem mdx DropShadow and the unnamed flag bits survive", "[wem][convert][mdx][gate]") {
    const MdxConverter converter;
    Result<Document> imported = converter.fromMdx(makeGatedModel());
    REQUIRE(imported.ok());
    const Model& model = imported->models[0];
    const MeshSection& shadowed = model.meshes[2].sections[0];
    CHECK(hasFlag(shadowed.flags, SectionFlags::ProjectedShadow));
    REQUIRE(shadowed.native.find("geosetAnimFlags") != nullptr);
    CHECK(shadowed.native.find("geosetAnimFlags")->value == 0x1A);
    // A plain `Color` word is what the export writes anyway, so it rides nothing.
    CHECK(model.meshes[0].sections[0].native.find("geosetAnimFlags") == nullptr);
    CHECK_FALSE(hasFlag(model.meshes[0].sections[0].flags, SectionFlags::ProjectedShadow));

    Result<mdx::Model> exported = converter.toMdx(*imported, ProfileId::Wc3Classic, 800);
    REQUIRE(exported.ok());
    REQUIRE(recordFor(*exported, 2) != nullptr);
    CHECK(static_cast<u32>(recordFor(*exported, 2)->flags) == 0x1Bu);
    // A record whose only content was its flags word comes back with it.
    REQUIRE(recordFor(*exported, 3) != nullptr);
    CHECK(static_cast<u32>(recordFor(*exported, 3)->flags) == 0u);
    CHECK(static_cast<u32>(recordFor(*exported, 0)->flags) == 0x2u);
    CHECK(recordFor(*exported, 0)->alpha == 0.5f);
    CHECK(recordFor(*exported, 2)->color == Vector3f(0.5f, 0.25f, 1.0f));

    // And a second trip is a fixed point.
    Result<Document> again = converter.fromMdx(*exported);
    REQUIRE(again.ok());
    Result<mdx::Model> twice = converter.toMdx(*again, ProfileId::Wc3Classic, 800);
    REQUIRE(twice.ok());
    REQUIRE(twice->geosetAnimations.size() == exported->geosetAnimations.size());
    for (std::size_t r = 0; r < twice->geosetAnimations.size(); ++r) {
        CHECK(twice->geosetAnimations[r].geosetId == exported->geosetAnimations[r].geosetId);
        CHECK(twice->geosetAnimations[r].flags == exported->geosetAnimations[r].flags);
    }
}

TEST_CASE("wem mdx the export map numbers geosets as toMdx writes them",
          "[wem][convert][mdx][geometry]") {
    Document document = makeSectionedDocument(3);
    Model& model = document.models[0];
    model.meshes.push_back(model.meshes[0]);
    model.meshes[1].sections[0].name = "second_0";
    Node& gate = model.nodes.nodes[0];
    gate.payload = BonePayload{};
    std::get<BonePayload>(gate.payload).gateMesh = 1;

    const MdxExportMap map = MdxExportMapOf(document, 0, ProfileId::Wc3Classic);
    REQUIRE(map.geosetsOfMesh.size() == 2);
    CHECK(map.geosetsOfMesh[0] == std::vector<u32>{0, 1, 2});
    CHECK(map.geosetsOfMesh[1] == std::vector<u32>{3, 4, 5});

    const MdxConverter converter;
    Result<mdx::Model> exported = converter.toMdx(document, ProfileId::Wc3Classic, 800);
    REQUIRE(exported.ok());
    REQUIRE(exported->geosets.size() == 6);
    CHECK(exported->geosets[1].lodName == "part_1");
    CHECK(exported->geosets[3].lodName == "second_0");
    // The gate lands on the first geoset the mesh became.
    const mdx::Bone* bone = boneNamed(*exported, "bone_0");
    REQUIRE(bone != nullptr);
    CHECK(bone->geosetId == 3u);
    CHECK(gateGeosetOf(*exported, *bone) == 3u);
}

TEST_CASE("wem mdx the geoset tint is written as the import writes it",
          "[wem][convert][mdx][tint]") {
    const MdxConverter converter;
    MeshSection section;
    section.native.set("selectionFlags", 4);
    section.native.set("geosetAnimFlags", 0x1A);

    SECTION("set and get") {
        const GeosetTint tint{Vector3f{0.5f, 0.25f, 1.0f}, 0.5f, false};
        converter.setGeosetTint(section, tint);
        CHECK(converter.geosetTint(section) == tint);
        // The import's order: the tint keys follow `selectionFlags`.
        CHECK(keysOf(section.native) ==
              std::vector<std::string>{"selectionFlags", "geosetColorR", "geosetColorG",
                                       "geosetColorB", "geosetAlpha", "geosetAnimFlags"});
    }
    SECTION("white and opaque erase") {
        converter.setGeosetTint(section, GeosetTint{Vector3f{0.5f, 0.5f, 0.5f}, 0.5f, false});
        converter.setGeosetTint(section, GeosetTint{});
        CHECK(keysOf(section.native) ==
              std::vector<std::string>{"selectionFlags", "geosetAnimFlags"});
        CHECK(converter.geosetTint(section) == GeosetTint{});
    }
    SECTION("hidden keeps the alpha for when it is shown") {
        converter.setGeosetTint(section, GeosetTint{Vector3f{1, 1, 1}, 0.5f, true});
        CHECK(hasFlag(section.flags, SectionFlags::Hidden));
        CHECK(converter.geosetTint(section).alpha == 0.5f);
        // Typing 0 is Hidden too, and leaves the stored alpha alone.
        converter.setGeosetTint(section, GeosetTint{Vector3f{1, 1, 1}, 0.5f, false});
        converter.setGeosetTint(section, GeosetTint{Vector3f{1, 1, 1}, 0.0f, false});
        CHECK(hasFlag(section.flags, SectionFlags::Hidden));
        CHECK(converter.geosetTint(section).alpha == 0.5f);
        converter.setGeosetTint(section, GeosetTint{Vector3f{1, 1, 1}, 0.5f, false});
        CHECK_FALSE(hasFlag(section.flags, SectionFlags::Hidden));
    }
    SECTION("an edit back to the imported value compares equal") {
        Result<Document> imported = converter.fromMdx(makeGatedModel());
        REQUIRE(imported.ok());
        MeshSection& shadowed = imported->models[0].meshes[2].sections[0];
        const NativeBag before = shadowed.native;
        const GeosetTint was = converter.geosetTint(shadowed);
        converter.setGeosetTint(shadowed, GeosetTint{});
        converter.setGeosetTint(shadowed, was);
        CHECK(keysOf(shadowed.native) == keysOf(before));
        REQUIRE(shadowed.native.entries.size() == before.entries.size());
        for (std::size_t i = 0; i < before.entries.size(); ++i) {
            CHECK(shadowed.native.entries[i].value == before.entries[i].value);
        }
    }
}

TEST_CASE("wem mdx a hidden geoset writes alpha 0 and keeps its own for later",
          "[wem][convert][mdx][tint]") {
    Document document = makeSectionedDocument(2);
    const MdxConverter converter;
    MeshSection& section = document.models[0].meshes[0].sections[1];
    converter.setGeosetTint(section, GeosetTint{Vector3f{1, 1, 1}, 0.5f, true});

    Result<mdx::Model> hidden = converter.toMdx(document, ProfileId::Wc3Classic, 800);
    REQUIRE(hidden.ok());
    REQUIRE(recordFor(*hidden, 1) != nullptr);
    CHECK(recordFor(*hidden, 1)->alpha == 0.0f);

    converter.setGeosetTint(section, GeosetTint{Vector3f{1, 1, 1}, 0.5f, false});
    Result<mdx::Model> shown = converter.toMdx(document, ProfileId::Wc3Classic, 800);
    REQUIRE(shown.ok());
    REQUIRE(recordFor(*shown, 1) != nullptr);
    CHECK(recordFor(*shown, 1)->alpha == 0.5f);
}

TEST_CASE("wem mdx the geoset flag words are written as the import writes them",
          "[wem][convert][mdx][tint]") {
    const MdxConverter converter;
    Result<Document> imported = converter.fromMdx(makeGatedModel());
    REQUIRE(imported.ok());
    Document document = *imported;
    MeshSection& plain = document.models[0].meshes[1].sections[0];
    CHECK(converter.geosetFlags(plain) == GeosetFlags{});

    // Unselectable, a drop shadow and an unnamed bit, on a geoset with no record.
    const GeosetFlags wanted{GeosetFlags::kUnselectable, 0x2 | 0x8 | GeosetFlags::kDropShadow};
    converter.setGeosetFlags(plain, wanted);
    CHECK(converter.geosetFlags(plain) == wanted);
    CHECK(hasFlag(plain.flags, SectionFlags::ProjectedShadow));

    Result<mdx::Model> exported = converter.toMdx(document, ProfileId::Wc3Classic, 800);
    REQUIRE(exported.ok());
    CHECK(exported->geosets[1].selectionFlags == GeosetFlags::kUnselectable);
    REQUIRE(recordFor(*exported, 1) != nullptr);
    CHECK(static_cast<u32>(recordFor(*exported, 1)->flags) == 0xBu);

    Result<Document> again = converter.fromMdx(*exported);
    REQUIRE(again.ok());
    CHECK(converter.geosetFlags(again->models[0].meshes[1].sections[0]) == wanted);

    // Back to plain: the bag says nothing again, and the record goes.
    converter.setGeosetFlags(plain, GeosetFlags{});
    CHECK(plain.native.find("geosetAnimFlags") == nullptr);
    CHECK_FALSE(hasFlag(plain.flags, SectionFlags::ProjectedShadow));
    Result<mdx::Model> plainAgain = converter.toMdx(document, ProfileId::Wc3Classic, 800);
    REQUIRE(plainAgain.ok());
    CHECK(recordFor(*plainAgain, 1) == nullptr);
}

namespace {

/// One quad whose second UV set seams where the first does not: each triangle
/// maps to its own half of the second map, so the shared diagonal takes two
/// `uv1` values. With @p constantUv1 every corner's `uv1` is (0, 0), as an M2
/// body mesh ships it.
Document makeTwoUvDocument(bool constantUv1) {
    geom::MeshBuilder builder;
    MeshSection section;
    section.name = "card";
    section.materialSlot = 0;
    section.profiles = ProfileBit(ProfileId::Wc3Classic) | ProfileBit(ProfileId::Wc3Reforged);
    builder.addSection(std::move(section));
    const geom::VertexId v[4] = {
        builder.addVertex(Vector3f{0, 0, 0}), builder.addVertex(Vector3f{1, 0, 0}),
        builder.addVertex(Vector3f{1, 1, 0}), builder.addVertex(Vector3f{0, 1, 0})};
    for (const geom::VertexId vertex : v) {
        builder.addInfluence(vertex, 0, 1.0f);
    }
    const Vector2f uv0[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    const u32 triangles[2][3] = {{0, 1, 2}, {0, 2, 3}};
    for (u32 t = 0; t < 2; ++t) {
        const geom::FaceId face =
            builder.addTriangle(v[triangles[t][0]], v[triangles[t][1]], v[triangles[t][2]], 0);
        for (u32 c = 0; c < 3; ++c) {
            const Vector2f first = uv0[triangles[t][c]];
            builder.setCornerAttr(face, c, geom::names::kNormal, Vector3f{0, 0, 1});
            builder.setCornerAttr(face, c, geom::names::uv(0), first);
            builder.setCornerAttr(face, c, geom::names::uv(1),
                                  constantUv1 ? Vector2f{0, 0}
                                              : Vector2f{first.x * 0.5f + 0.5f * t, first.y});
        }
    }

    Document document;
    document.declare(ProfileId::Wc3Classic);
    document.defaultProfile = ProfileId::Wc3Classic;
    document.name = "two_uv";
    document.textures.push_back(TextureRef{});

    Model model;
    model.name = "two_uv";
    model.meshes.push_back(builder.build().mesh);
    model.addSlot("card");
    ProfileMaterialSet set;
    set.profile = ProfileId::Wc3Classic;
    set.looks.looks.push_back(Look{});
    set.resizeBindings(1);
    Material material;
    material.name = "card";
    set.slotBindings[0].byLook[0] = 0;
    set.materials.push_back(std::move(material));
    model.profileSets.push_back(std::move(set));
    Node bone;
    bone.name = "root";
    bone.kind = NodeKind::Bone;
    bone.parent = kInvalidNode;
    model.nodes.nodes.push_back(std::move(bone));
    document.models.push_back(std::move(model));
    REQUIRE(DeriveProfile(document, ProfileId::Wc3Classic, ProfileId::Wc3Reforged).ok);
    return document;
}

} // namespace

TEST_CASE("wem mdx Reforged writes a second UV set that varies, and seams on it",
          "[wem][convert][mdx][geometry]") {
    // An M2 stage fed by T2 exports as `coordId 1`; a geoset with one UVAS set
    // left that layer sampling nothing the source meant.
    const MdxConverter converter;
    const Document document = makeTwoUvDocument(false);

    Result<mdx::Model> reforged = converter.toMdx(document, ProfileId::Wc3Reforged);
    REQUIRE(reforged.ok());
    REQUIRE(reforged->geosets.size() == 1);
    const mdx::Geoset& geoset = reforged->geosets[0];
    REQUIRE(geoset.textureCoordinateSets.size() == 2);
    // The diagonal's two ends split on uv1: four corners become six vertices.
    CHECK(geoset.vertexPositions.size() == 6);
    CHECK(geoset.textureCoordinateSets[1].size() == 6);
    bool rightHalf = false;
    for (const Vector2f& uv : geoset.textureCoordinateSets[1]) {
        rightHalf = rightHalf || uv.x > 0.5f;
    }
    CHECK(rightHalf);
    // What Edit Mode maps a file vertex through is what the export wrote.
    const auto vertices = MdxGeosetVertices(document, 0, ProfileId::Wc3Reforged);
    REQUIRE(vertices.size() == 1);
    CHECK(vertices[0].size() == geoset.vertexPositions.size());

    // Classic reads one set, so nothing splits on the second.
    Result<mdx::Model> classic = converter.toMdx(document, ProfileId::Wc3Classic, 800);
    REQUIRE(classic.ok());
    REQUIRE(classic->geosets.size() == 1);
    CHECK(classic->geosets[0].textureCoordinateSets.size() == 1);
    CHECK(classic->geosets[0].vertexPositions.size() == 4);
    CHECK(MdxGeosetVertices(document, 0, ProfileId::Wc3Classic)[0].size() == 4);

    // A second set that never varies carries nothing and is not written.
    Result<mdx::Model> constant = converter.toMdx(makeTwoUvDocument(true), ProfileId::Wc3Reforged);
    REQUIRE(constant.ok());
    CHECK(constant->geosets[0].textureCoordinateSets.size() == 1);
    CHECK(constant->geosets[0].vertexPositions.size() == 4);
}

TEST_CASE("wem mdx classic names no UV set it does not write", "[wem][convert][mdx][geometry]") {
    // Classic writes one set per geoset, so a layer left on set 1 would read
    // past every geoset in the file; it takes set 0.
    mdx::Model source = makeModel();
    source.version = 800;
    source.geosets[0].textureCoordinateSets.push_back(
        {Vector2f{0, 0}, Vector2f{0.5f, 0}, Vector2f{0.5f, 1}, Vector2f{0, 1}});
    source.materials[0].layers[0].coordId = 1;

    const MdxConverter converter;
    Result<Document> imported = converter.fromMdx(source);
    REQUIRE(imported.ok());
    Result<mdx::Model> exported = converter.toMdx(*imported, ProfileId::Wc3Classic, 800);
    REQUIRE(exported.ok());
    REQUIRE(exported->geosets.size() == 1);
    CHECK(exported->geosets[0].textureCoordinateSets.size() == 1);
    REQUIRE(exported->materials.size() == 1);
    REQUIRE(exported->materials[0].layers.size() == 1);
    CHECK(exported->materials[0].layers[0].coordId == 0);
    CHECK(exported.diagnostics.countOf(DiagCode::UvSetLimit) >= 1);
}
