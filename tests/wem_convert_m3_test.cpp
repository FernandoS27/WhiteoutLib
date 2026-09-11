// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// WEM v3 P5 — `M3Converter` on hand-built models.
///
/// Four claims a green parse cannot check: the profile follows the `MODL`
/// version, the SC2 -> Blizzard rebase is the exact one the design specifies
/// (and is bit-exact both ways), a region's faces are region-local, and a
/// vertex's bone index goes through its region's `boneLookup` window.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <whiteout/models/m3/engine_compat.h>
#include <whiteout/models/m3/parser.h>
#include <whiteout/models/m3/writer.h>
#include <whiteout/models/wem/converters.h>
#include <whiteout/models/wem/profile.h>

#include "wem_material_fixture.h"

using namespace whiteout;
using namespace whiteout::models::wem;

namespace {

/// The `.m3` vertex layout for `UV1` and nothing else: 32 bytes, position at 0,
/// weights at 12, indices at 16, normal UNORM at 20, uv i16 at 24.
constexpr std::size_t kStride = 32;

void pushVertex(std::vector<u8>& blob, const Vector3f& position, u8 boneSlot) {
    const std::size_t base = blob.size();
    blob.resize(base + kStride, 0);
    std::memcpy(blob.data() + base, &position, sizeof(Vector3f));
    blob[base + 12] = 255;      // weight 0 = 1.0
    blob[base + 16] = boneSlot; // index 0, region-local
    blob[base + 20] = 128;      // normal ~ (0, 0, 1) in UNORM
    blob[base + 21] = 128;
    blob[base + 22] = 255;
    blob[base + 23] = 255;
}

/// Two bones, a quad, one region whose bone-lookup window names bone 1 only.
m3::Model makeModel(u32 version) {
    m3::Model model;
    model.setVersion(static_cast<i32>(version));
    model.name = "unit";
    model.bounds.min = Vector3f{-1, -1, 0};
    model.bounds.max = Vector3f{1, 1, 0};

    m3::Bone root;
    root.name = "root";
    root.parentIndex = 0xFFFF;
    // What a shipped bone actually carries. The three `Inherit` bits are set on
    // NO bone in the corpus — 0 of 50,771 across 4,000 files — so a fixture
    // that sets them is the one shape the mapping below cannot get wrong.
    root.flags = m3::BoneFlag::Real | m3::BoneFlag::Animated | m3::BoneFlag::Skinned;
    root.scale.initValue = Vector3f{1, 1, 1};
    root.rotation.initValue = Quaternion{0, 0, 0, 1};
    m3::Bone child = root;
    child.name = "child";
    child.parentIndex = 0;
    // In SC2's basis: 3 units "forward" is -Y.
    child.position.initValue = Vector3f{0, -3, 0};
    model.bones.push_back(root);
    model.bones.push_back(child);

    std::vector<u8> blob;
    const Vector3f corners[4] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
    for (const Vector3f& corner : corners) {
        pushVertex(blob, corner, 0);
    }
    model.vertices.flags = m3::VertexFormatFlag::UV1;
    model.vertices.data = std::move(blob);
    model.vertices.initialize();

    // Slot 0 of this region's window is bone 1 — a converter reading the vertex
    // byte as a bone id would skin the quad to bone 0 instead.
    model.boneLookup = {1};

    m3::MeshDivision division;
    division.faces = {0, 1, 2, 0, 2, 3};
    m3::Region region;
    region.firstVertex = 0;
    region.vertexCount = 4;
    region.firstIndex = 0;
    region.indexCount = 6;
    region.firstBoneLookup = 0;
    region.boneLookupCount = 1;
    region.rootBone = 0;
    division.regions.push_back(region);

    m3::Batch batch;
    batch.regionIndex = 0;
    batch.materialIndex = 0;
    batch.boneCount = 0xFFFF;
    division.batches.push_back(batch);
    model.divisions.push_back(division);

    m3::StandardMaterial material;
    material.name = "body";
    m3::TextureLayer diffuse;
    diffuse.texturePath = "assets/textures/body.dds";
    material.diffuseLayer = diffuse;
    model.standardMaterials.push_back(material);

    m3::MaterialMap map;
    map.materialType = m3::MaterialType::Standard;
    map.materialIndex = 0;
    model.materialMaps.push_back(map);

    return model;
}

} // namespace

TEST_CASE("wem m3 bone flags stay native", "[wem][convert][m3][nodes]") {
    const M3Converter converter;
    Result<Document> imported = converter.fromM3(makeModel(25));
    REQUIRE(imported.ok());
    const NodeTree& tree = imported->models.front().nodes;
    REQUIRE(tree.nodes.size() >= 2);

    // `BONE.flags` says nothing `NodeFlags` can carry. StarCraft II reads two
    // bits out of it (both dead), never the three `Inherit` ones — and those
    // three are set on no shipped bone, so deriving `DontInherit*` from their
    // absence hands every bone of every `.m3` all three suppressions. A
    // pivot-relative target honours them, which detached every bone from its
    // parent the moment one was opened as Warcraft III.
    for (const Node& node : tree.nodes) {
        CHECK(node.flags == NodeFlags::None);
    }
    // The word itself survives, under a name only the M3 converter answers to.
    CHECK(tree.nodes[0].native.value("m3FlagBits") ==
          static_cast<i64>(static_cast<u32>(m3::BoneFlag::Real | m3::BoneFlag::Animated |
                                            m3::BoneFlag::Skinned)));
    CHECK(tree.nodes[0].native.find("flagBits") == nullptr);

    Result<m3::Model> exported = converter.toM3(*imported, ProfileId::Sc2);
    REQUIRE(exported.ok());
    REQUIRE(exported->bones.size() >= 2);
    CHECK(exported->bones[0].flags ==
          (m3::BoneFlag::Real | m3::BoneFlag::Animated | m3::BoneFlag::Skinned));
}

TEST_CASE("wem m3 picks its profile from the MODL version", "[wem][convert][m3]") {
    CHECK(M3Converter::ProfileForVersion(23) == ProfileId::Sc2);
    CHECK(M3Converter::ProfileForVersion(29) == ProfileId::Sc2);
    CHECK(M3Converter::ProfileForVersion(30) == ProfileId::Heroes);

    const M3Converter converter;
    Result<Document> sc2 = converter.fromM3(makeModel(25));
    REQUIRE(sc2.ok());
    CHECK(sc2->defaultProfile == ProfileId::Sc2);

    Result<Document> heroes = converter.fromM3(makeModel(30));
    REQUIRE(heroes.ok());
    CHECK(heroes->defaultProfile == ProfileId::Heroes);

    // A caller who knows better wins: content moves between the two games and a
    // version number is a signal, not a proof.
    Result<Document> forced = converter.fromM3(makeModel(30), ProfileId::Sc2);
    REQUIRE(forced.ok());
    CHECK(forced->defaultProfile == ProfileId::Sc2);
}

TEST_CASE("wem m3 rebases into the canonical space", "[wem][convert][m3][space]") {
    const M3Converter converter;
    Result<Document> result = converter.fromM3(makeModel(30));
    REQUIRE(result.ok());
    CHECK(result->space == CoordSpace::Blizzard);

    const NodeTree& nodes = result->models.front().nodes;
    REQUIRE(nodes.size() == 2);
    // SC2's (0, -3, 0) -- three units along its forward -- is (3, 0, 0) in
    // Blizzard's basis, where forward is +X.
    CHECK(nodes.nodes[1].local.translation.x == 3.0f);
    CHECK(nodes.nodes[1].local.translation.y == 0.0f);
}

TEST_CASE("wem m3 skinning goes through the region's bone lookup", "[wem][convert][m3][skin]") {
    const M3Converter converter;
    Result<Document> result = converter.fromM3(makeModel(30));
    REQUIRE(result.ok());
    const Mesh& mesh = result->models.front().meshes[0];

    CHECK(mesh.vertexCount() == 4);
    CHECK(mesh.faceCount() == 2);
    const auto influences = mesh.skin.forVertex(0);
    REQUIRE(influences.size() == 1);
    // Slot 0 of the window, not bone 0.
    CHECK(influences[0].bone == 1);
    CHECK(influences[0].weight == 1.0f);
}

TEST_CASE("wem m3 every division states its bounding volume",
          "[wem][convert][m3][version]") {
    // A chunk the game reaches for unconditionally, and did not find. The
    // Galaxy editor crashed importing a converted footman on it:
    // ACCESS_VIOLATION reading from 0x2C, which is inside the `AnimRef<Extent>`
    // of a `MSEC` record that was not there.
    const M3Converter converter;
    Result<Document> document = converter.fromM3(makeModel(29), ProfileId::Sc2);
    REQUIRE(document.ok());
    Result<m3::Model> written = converter.toM3(*document, ProfileId::Sc2, 29);
    REQUIRE(written.ok());

    SECTION("one record against node 0, holding the model's own bounds") {
        REQUIRE_FALSE(written->divisions.empty());
        REQUIRE(written->divisions[0].msec.size() == 1u);
        const m3::MeshSection& section = written->divisions[0].msec[0];
        CHECK(section.nodeIndex == 0u);
        CHECK(section.bounds.initValue.min.x == Catch::Approx(written->bounds.min.x));
        CHECK(section.bounds.initValue.max.z == Catch::Approx(written->bounds.max.z));
        // The sphere is around the BOX, not the origin -- |(max - min) / 2| on
        // all 377 shipped bounds measured, and a Warcraft III source states none.
        const Vector3f half{(written->bounds.max.x - written->bounds.min.x) * 0.5f,
                            (written->bounds.max.y - written->bounds.min.y) * 0.5f,
                            (written->bounds.max.z - written->bounds.min.z) * 0.5f};
        const f32 want = std::sqrt(half.x * half.x + half.y * half.y + half.z * half.z);
        CHECK(written->bounds.radius == Catch::Approx(want));
        CHECK(section.bounds.initValue.radius == Catch::Approx(want));
    }

}

TEST_CASE("wem m3 a built record carries no stack junk into the file",
          "[wem][convert][m3][version]") {
    // The parser fills every field, so only a record a conversion BUILDS can
    // reach the writer indeterminate -- and one did: halves of live heap
    // pointers landed in `flipbookColumns`, `textureSource` and the fresnel
    // fields of every exported layer. The editor truncates the column count to
    // a byte and, when that came out zero, queued a shader parameter with no
    // destination and wrote through the null next time round.
    m3::TextureLayer layer;
    CHECK(layer.flipbookRows == 0u);
    CHECK(layer.flipbookColumns == 0u);
    CHECK(layer.textureSource == 0xFFFFFFFFu);
    CHECK(layer.pocTexture == 0u);
    // Its own UVs (1,033 of 1,033 shipped diffuse layers) and the fresnel ramp
    // every shipped layer states whether or not it is on.
    CHECK(layer.uvSourceRelated == 0xFFFFFFFFu);
    CHECK(layer.fresnelMode == m3::FresnelMode::None);
    CHECK(layer.fresnelExponent == 4.0f);
    CHECK(layer.fresnelMin == 0.0f);
    CHECK(layer.fresnelMax == 1.0f);

    // And through the whole crossing, on the file the editor actually reads.
    const M3Converter converter;
    Result<Document> document = converter.fromM3(makeModel(29), ProfileId::Sc2);
    REQUIRE(document.ok());
    Result<m3::Model> written = converter.toM3(*document, ProfileId::Sc2, 29);
    REQUIRE(written.ok());
    const std::vector<u8> bytes = m3::Writer().write(*written);
    const m3::Model reparsed = m3::Parser().parse(bytes);
    REQUIRE_FALSE(reparsed.standardMaterials.empty());
    for (const m3::StandardMaterial& mat : reparsed.standardMaterials) {
        REQUIRE(mat.diffuseLayer.has_value());
        // A grid of zero is what an unused flipbook means; anything else here
        // is a field nobody wrote.
        CHECK(mat.diffuseLayer->flipbookColumns < 256u);
        CHECK(mat.diffuseLayer->flipbookRows < 256u);
        CHECK(mat.diffuseLayer->textureSource == 0xFFFFFFFFu);
    }
}

TEST_CASE("wem m3 an exported division draws once and states its declaration",
          "[wem][convert][m3][version]") {
    // The three fields a conversion left at zero that the client reads as
    // answers rather than as absences. The division's instance count is the
    // one that cost a render: the Galaxy editor built no index buffer for a
    // division claiming zero copies, logged "Index buffer not set" once per
    // batch per frame and failed the draw with D3DERR_INVALIDCALL -- the mesh
    // loaded, bound to nothing, and the viewport stayed black.
    const M3Converter converter;
    Result<Document> document = converter.fromM3(makeModel(29), ProfileId::Sc2);
    REQUIRE(document.ok());
    Result<m3::Model> written = converter.toM3(*document, ProfileId::Sc2, 29);
    REQUIRE(written.ok());

    REQUIRE_FALSE(written->divisions.empty());
    // 5,502 of 5,502 shipped divisions.
    CHECK(written->divisions.front().instances == 1u);

    // 5,510 of 5,510 shipped models set every bit of 0x01800061, whatever they
    // mean; a source that states no declaration of its own still gets them.
    CHECK((static_cast<u32>(written->vertices.flags) & 0x01800061u) == 0x01800061u);

    // A model with no collision mesh states no volume for one: all 2,448
    // shipped v29 models leave `collisionBounds` zero, and this converter
    // emits no collision mesh.
    CHECK(written->collisionVerts.empty());
    CHECK(written->collisionBounds.radius == 0.0f);
    CHECK(written->collisionBounds.min.x == 0.0f);
    CHECK(written->collisionBounds.max.z == 0.0f);

    // An enum member with no initialiser is the `TextureLayer` defect one
    // field over: nothing assigns the tight hit-test shape, so a
    // default-initialised one wrote junk shape types (711000064 in one export,
    // 0 in the next) into every file.
    CHECK(written->tightHitTestObject.shapeType == m3::HitTestShapeType::Sphere);
    CHECK(m3::HitTestShape{}.shapeType == m3::HitTestShapeType::Sphere);
}

TEST_CASE("wem m3 an exported file leaves the client nothing null to walk",
          "[wem][convert][m3][version]") {
    // Two parallel arrays and eighteen layer slots the client walks without
    // checking, and that shipped content therefore never leaves empty: 1,535 of
    // 1,535 models with attachment points carry one addon entry per point (all
    // 0xFFFF), 343 of 343 with cameras carry one per camera, and 29,214 unused
    // layer slots across the StarCraft II corpus each still carry a `LAYR` with
    // no texture path. A converted model filled none of them.
    const M3Converter converter;
    Result<Document> document = converter.fromM3(makeModel(29), ProfileId::Sc2);
    REQUIRE(document.ok());
    Result<m3::Model> written = converter.toM3(*document, ProfileId::Sc2, 29);
    REQUIRE(written.ok());

    SECTION("every scene object gets its addon entry") {
        REQUIRE(written->attachmentPointAddons.size() == written->attachmentPoints.size());
        REQUIRE(written->camerasAddons.size() == written->cameras.size());
        for (const u16 addon : written->attachmentPointAddons) {
            CHECK(addon == 0xFFFFu);
        }
        for (const u16 addon : written->camerasAddons) {
            CHECK(addon == 0xFFFFu);
        }
    }

    SECTION("every material layer slot reaches the file") {
        // The fill is the writer's, not the converter's: everything upstream
        // reads `has_value()` on a slot to mean the material carries that
        // layer, so the model itself keeps saying which layers it has and only
        // the bytes carry all eighteen.
        REQUIRE_FALSE(written->standardMaterials.empty());
        const std::vector<u8> bytes = m3::Writer().write(*written);
        REQUIRE_FALSE(bytes.empty());
        const m3::Model reparsed = m3::Parser().parse(bytes);
        REQUIRE_FALSE(reparsed.standardMaterials.empty());

        const m3::StandardMaterial& mat = reparsed.standardMaterials[0];
        const std::optional<m3::TextureLayer> m3::StandardMaterial::*const slots[] = {
            &m3::StandardMaterial::diffuseLayer,
            &m3::StandardMaterial::decalLayer,
            &m3::StandardMaterial::specularLayer,
            &m3::StandardMaterial::glossLayer,
            &m3::StandardMaterial::emissiveLayer1,
            &m3::StandardMaterial::emissiveLayer2,
            &m3::StandardMaterial::environmentLayer,
            &m3::StandardMaterial::environmentMaskLayer,
            &m3::StandardMaterial::alphaLayer1,
            &m3::StandardMaterial::alphaLayer2,
            &m3::StandardMaterial::normalLayer,
            &m3::StandardMaterial::heightLayer,
            &m3::StandardMaterial::lightMapLayer,
            &m3::StandardMaterial::ambientOcclusionLayer,
            &m3::StandardMaterial::normalBlend1MaskLayer,
            &m3::StandardMaterial::normalBlend2MaskLayer,
            &m3::StandardMaterial::normalBlend1Layer,
            &m3::StandardMaterial::normalBlend2Layer,
        };
        for (const auto slot : slots) {
            CHECK((mat.*slot).has_value());
        }
        // A slot the material does not use is still an empty layer, not one
        // pointing at a texture it never had.
        CHECK((reparsed.standardMaterials[0].normalBlend2Layer->texturePath.empty() ||
               reparsed.standardMaterials[0].normalBlend2Layer->texturePath == std::string(1, ' ')));
    }
}

TEST_CASE("wem m3 an exported chunk states the version its client reads",
          "[wem][convert][m3][version]") {
    // `MAT_` is the one chunk a conversion creates that the two engines
    // disagree about: StarCraft II reads v20, Heroes caps at v19, and the three
    // versions between them are the HDR environment multipliers this export
    // fills. Stated on the record rather than left to the writer because the
    // writer defaults to the version BOTH clients read -- which would drop them
    // from a StarCraft II file too -- and because the renderer reads the
    // version back off a model that was never written.
    const M3Converter converter;
    Result<Document> document = converter.fromM3(makeModel(29), ProfileId::Sc2);
    REQUIRE(document.ok());

    SECTION("StarCraft II takes MAT_ v20") {
        Result<m3::Model> written = converter.toM3(*document, ProfileId::Sc2, 29);
        REQUIRE(written.ok());
        REQUIRE_FALSE(written->standardMaterials.empty());
        CHECK(written->standardMaterials[0].getVersion() == 20);
        CHECK(m3::checkEngineSupport(*written).starcraft2);
    }

    SECTION("Heroes of the Storm caps at v19") {
        Result<Document> heroes = converter.fromM3(makeModel(30), ProfileId::Heroes);
        REQUIRE(heroes.ok());
        Result<m3::Model> written = converter.toM3(*heroes, ProfileId::Heroes, 30);
        REQUIRE(written.ok());
        REQUIRE_FALSE(written->standardMaterials.empty());
        CHECK(written->standardMaterials[0].getVersion() == 19);
        CHECK(m3::checkEngineSupport(*written).heroesOfTheStorm);
    }

    SECTION("Heroes is told what MAT_ v19 cannot hold") {
        // The three versions between v19 and v20 are the HDR environment
        // multipliers, and Heroes keeps the same quantities as properties of a
        // MADD blob instead. Dropping them is right; dropping them quietly is
        // not.
        m3::Model source = makeModel(30);
        source.standardMaterials[0].hdrEnvironmentConstant = 2.0f;
        Result<Document> heroes = converter.fromM3(source, ProfileId::Heroes);
        REQUIRE(heroes.ok());
        Result<m3::Model> written = converter.toM3(*heroes, ProfileId::Heroes, 30);
        REQUIRE(written.ok());
        CHECK(written->standardMaterials[0].getVersion() == 19);

        bool told = false;
        for (const Diagnostic& d : written.diagnostics.byCode(DiagCode::FeatureDropped)) {
            told = told || d.message.find("HDR environment multiplier") != std::string::npos;
        }
        CHECK(told);
    }

    SECTION("a region says it is the v5 record whose uvScale it filled") {
        Result<m3::Model> written = converter.toM3(*document, ProfileId::Sc2, 29);
        REQUIRE(written.ok());
        REQUIRE_FALSE(written->divisions.empty());
        REQUIRE_FALSE(written->divisions[0].regions.empty());
        // Without this the renderer's `getVersion() >= 5` gate reads the stock
        // 1/2048 pair instead of the one the encoder actually wrote.
        CHECK(written->divisions[0].regions[0].getVersion() == 5);
        CHECK(written->divisions[0].regions[0].uvScale == 16.0f);
    }
}

TEST_CASE("wem m3 a gate bone keeps the visibility it rests at",
          "[wem][convert][m3][visibility]") {
    m3::Model model = makeModel(30);
    // A batch gated on bone 1, whose visibility rests VISIBLE. Nothing keys it:
    // 972 of Heroes' 18,409 models carry no sequence at all, and Alexstrasza --
    // whose four batches all gate on one `Vis_` bone -- is one of them.
    model.bones[1].visibility.initValue = 1;
    model.divisions[0].batches[0].boneCount = 1;

    const M3Converter converter;
    Result<Document> document = converter.fromM3(model, ProfileId::Heroes);
    REQUIRE(document.ok());
    Result<m3::Model> written = converter.toM3(*document, ProfileId::Heroes, 30);
    REQUIRE(written.ok());

    REQUIRE(written->divisions.size() == 1);
    REQUIRE(written->divisions[0].batches.size() == 1);
    CHECK(written->divisions[0].batches[0].boneCount == 1);
    REQUIRE(written->bones.size() == 2);
    // Position, rotation and scale rest in the node; visibility has no node
    // field, so the export has to go back to the channel for it. Left at the
    // struct's zero the gate reads "invisible" and the model draws nothing.
    CHECK(written->bones[1].visibility.initValue == 1u);
}

TEST_CASE("wem m3 a region states the scale its UVs decode at",
          "[wem][convert][m3][uv]") {
    // The raw i16 pair every vertex of the quad carries.
    constexpr i16 kRawU = 4096;
    constexpr i16 kRawV = 2048;

    const auto imported = [&](bool v5, f32 scale, f32 offset) {
        m3::Model model = makeModel(30);
        for (std::size_t v = 0; v < 4; ++v) {
            std::memcpy(model.vertices.data.data() + v * kStride + 24, &kRawU, sizeof(i16));
            std::memcpy(model.vertices.data.data() + v * kStride + 26, &kRawV, sizeof(i16));
        }
        model.vertices.initialize();
        m3::Region& region = model.divisions[0].regions[0];
        if (v5) {
            region.setVersion(5);
            region.uvScale = scale;
            region.uvOffset = offset;
        }
        const M3Converter converter;
        Result<Document> document = converter.fromM3(model, ProfileId::Heroes);
        REQUIRE(document.ok());
        const Mesh& mesh = document->models.front().meshes.front();
        const auto uvs =
            mesh.attributes.get<const Vector2f>(geom::names::uv(0), geom::Domain::Halfedge);
        REQUIRE(!uvs.empty());
        return uvs[0];
    };

    // A region that says nothing is the flat divide every pre-v5 one implies.
    const Vector2f implied = imported(false, 0.0f, 0.0f);
    CHECK(implied.x == Catch::Approx(static_cast<f32>(kRawU) / 2048.0f));
    CHECK(implied.y == Catch::Approx(static_cast<f32>(kRawV) / 2048.0f));

    // The stock v5 pair is the same thing said out loud: 16 / 32767 is 1 / 2048
    // to within a part in 32767, which is why reading it wrong looks right on
    // most models.
    const Vector2f stock = imported(true, 16.0f, 0.0f);
    CHECK(stock.x == Catch::Approx(static_cast<f32>(kRawU) / 2048.0f).epsilon(0.001));

    // And one that says something else. Shipped Heroes regions carry scales
    // from 0.26 to 17 with an offset to match -- Alexstrasza's body is
    // 0.941 / 0.941 -- and at 1/2048 her [0,1] skin lands anywhere in
    // [-16, 16], which a clamped sampler then smears into one column.
    const Vector2f stated = imported(true, 0.941076f, 0.941076f);
    CHECK(stated.x ==
          Catch::Approx(static_cast<f32>(kRawU) * 0.941076f / 32767.0f + 0.941076f));
    CHECK(stated.y ==
          Catch::Approx(static_cast<f32>(kRawV) * 0.941076f / 32767.0f + 0.941076f));
}

TEST_CASE("wem m3 the alpha masks are the coverage channel",
          "[wem][convert][m3][coverage]") {
    // `cFinal.a = mask1.a * mask2.a * alphaFactor` -- the alpha-mask layers are
    // what StarCraft II blends and alpha-tests by (the diffuse alpha is the
    // team mask), and 104,869 of 176,955 shipped materials carry one. They
    // used to be reported as dropped.
    m3::Model model = makeModel(30);
    m3::StandardMaterial& material = model.standardMaterials[0];
    material.blendMode = m3::BlendMode::AlphaBlend;
    m3::TextureLayer mask;
    mask.texturePath = "assets/textures/fade.dds";
    mask.colorType = m3::ColorChannelSelect::Green;
    material.alphaLayer1 = mask;
    m3::TextureLayer mask2;
    mask2.texturePath = "assets/textures/cut.dds";
    mask2.colorType = m3::ColorChannelSelect::Alpha;
    material.alphaLayer2 = mask2;

    const M3Converter converter;
    Result<Document> imported = converter.fromM3(model, ProfileId::Heroes);
    REQUIRE(imported.ok());
    const Material& mat = imported->models[0].profileSets[0].materials[0];
    const CompositeBody* body = mat.Common().composite();
    REQUIRE(body != nullptr);
    const std::vector<u32> coverage = body->layersOf(SurfaceChannel::Coverage);
    REQUIRE(coverage.size() == 2);
    // The fold is a product: the first mask seeds the channel, the second
    // multiplies into it.
    CHECK(body->layers[coverage[0]].op == CompositeOp::Set);
    CHECK(body->layers[coverage[1]].op == CompositeOp::Modulate);

    // And they go back into the slots they came from.
    Result<m3::Model> written = converter.toM3(*imported, ProfileId::Heroes, 30);
    REQUIRE(written.ok());
    REQUIRE(written->standardMaterials.size() == 1);
    const m3::StandardMaterial& out = written->standardMaterials[0];
    REQUIRE(out.alphaLayer1.has_value());
    REQUIRE(out.alphaLayer2.has_value());
    CHECK(out.alphaLayer1->texturePath.substr(0, 23) == "assets/textures/fade.dd");
    CHECK(out.alphaLayer2->texturePath.substr(0, 22) == "assets/textures/cut.dd");
}

TEST_CASE("wem m3 interns texture paths as it imports", "[wem][convert][m3][textures]") {
    const M3Converter converter;
    Result<Document> result = converter.fromM3(makeModel(30));
    REQUIRE(result.ok());

    // The document's texture table is built by the material import, because the
    // layers a material reads are the only enumeration of them there is.
    REQUIRE(result->textures.size() == 1);
    CHECK(result->textures[0].path == "assets/textures/body.dds");
    REQUIRE(result->models.front().materialSlots.size() == 1);
    CHECK(Resolve(result->models.front(), 0, ProfileId::Heroes) != nullptr);
}

TEST_CASE("wem m3 round trip returns to the source basis", "[wem][convert][m3]") {
    const M3Converter converter;
    Result<Document> imported = converter.fromM3(makeModel(30));
    REQUIRE(imported.ok());

    Result<m3::Model> exported = converter.toM3(*imported, ProfileId::Heroes, 30);
    REQUIRE(exported.ok());
    const m3::Model& out = *exported;

    REQUIRE(out.bones.size() == 2);
    // Bit-exact: the rebase is an axis permutation with sign flips, so a round
    // trip is not "close enough", it is equal.
    CHECK(out.bones[1].position.initValue.y == -3.0f);
    CHECK(out.bones[1].position.initValue.x == 0.0f);

    REQUIRE(out.divisions.size() == 1);
    CHECK(out.divisions[0].faces.size() == 6);
    REQUIRE(out.divisions[0].regions.size() == 1);
    CHECK(out.divisions[0].regions[0].vertexCount == 4);
    CHECK(out.vertices.vertexCount() == 4);

    const std::vector<Vector3f> positions = out.vertices.getPositions();
    REQUIRE(positions.size() == 4);
    CHECK(positions[0].x == 0.0f);
    CHECK(positions[0].y == 0.0f);
}

TEST_CASE("wem m3 carries IREF through the document as a matrix", "[wem][convert][m3]") {
    // IREF is not derivable and not a TRS. It is not the inverse of the composed
    // rest chain — on `Marine.m3` the two are 2.2 units apart — and across 250
    // corpus models 23 of them hold shear no translation/rotation/scale can
    // reproduce, up to 30.7 units out. So the pose is stored as the matrix it
    // is, and this is the shape of the model that proves it: a bind matrix whose
    // upper 3x3 is deliberately non-orthogonal.
    m3::Model source = makeModel(29);
    m3::InitialReference root;
    root.matrix = Matrix44f::identity();
    m3::InitialReference sheared;
    sheared.matrix = Matrix44f::identity();
    sheared.matrix.data[0][1] = 0.35f; // a shear, not a rotation
    sheared.matrix.data[1][0] = -0.10f;
    sheared.matrix.data[0][0] = 2.0f; // and a non-uniform scale under it
    sheared.matrix.data[1][1] = 0.5f;
    sheared.matrix.data[3][2] = 7.0f;
    source.initialReference.push_back(root);
    source.initialReference.push_back(sheared);

    const M3Converter converter;
    Result<Document> document = converter.fromM3(source);
    REQUIRE(document.ok());
    const NodeTree& tree = document->models.front().nodes;
    REQUIRE(tree.poseSchema.size() == 1);
    CHECK(tree.poseSchema[0].storage == PoseStorage::Matrix);
    REQUIRE(tree.nodes.size() >= 2);
    CHECK(tree.nodes[1].poseMatrices.size() == 1);

    Result<m3::Model> back = converter.toM3(*document, ProfileId::Sc2, 29);
    REQUIRE(back.ok());
    REQUIRE(back->initialReference.size() == source.initialReference.size());
    for (std::size_t b = 0; b < source.initialReference.size(); ++b) {
        for (int c = 0; c < 4; ++c) {
            for (int r = 0; r < 4; ++r) {
                INFO("bone " << b << " element [" << c << "][" << r << "]");
                CHECK(std::fabs(back->initialReference[b].matrix.data[c][r] -
                                source.initialReference[b].matrix.data[c][r]) < 1e-5f);
            }
        }
    }
}

TEST_CASE("wem m3 derives IREF when the document carries none", "[wem][convert][m3]") {
    // A document from another format has no `iref` pose, and a bind pose is the
    // one thing an M3 cannot be written without. The inverse of the composed
    // rest chain is the only answer available, and taking it beats writing
    // identity.
    //
    // This used to assert the chain itself rather than its inverse, which is the
    // `.mdx -> .m3` defect in miniature: `poseMatrixOf` answers with a schema
    // entry that does not say `inverse`, so the exporter wrote the bone's
    // position where the matrix undoing it belongs and every skinned vertex left
    // twice as far from the bone as it started. `inverseBindMatrix` is the
    // question the exporter actually has.
    Document document;
    document.declare(ProfileId::Sc2);
    document.defaultProfile = ProfileId::Sc2;
    Model model;
    Node root;
    root.name = "root";
    root.kind = NodeKind::Bone;
    root.resetPayloadForKind();
    Node child = root;
    child.name = "child";
    child.parent = 0;
    child.local.translation = Vector3f{0, 4, 0};
    model.nodes.add(std::move(root));
    model.nodes.add(std::move(child));
    ProfileMaterialSet set;
    set.profile = ProfileId::Sc2;
    set.looks.looks.push_back(Look{});
    model.profileSets.push_back(std::move(set));
    document.models.push_back(std::move(model));

    const M3Converter converter;
    Result<m3::Model> back = converter.toM3(document, ProfileId::Sc2, 29);
    REQUIRE(back.ok());
    REQUIRE(back->initialReference.size() == 2);
    // The child sits 4 units from the root along WEM's +Y, which is `.m3`'s +X,
    // so the matrix that undoes it translates by -4 there.
    const Matrix44f& childIref = back->initialReference[1].matrix;
    const Vector3f undone = childIref.extract_translation();
    CHECK(std::fabs(undone.x + 4.0f) < 1e-4f);
    CHECK(std::fabs(undone.y) < 1e-4f);

    // And the root, which sits at the origin, binds as the identity.
    const Vector3f rootUndone = back->initialReference[0].matrix.extract_translation();
    CHECK(std::fabs(rootUndone.x) < 1e-4f);
    CHECK(std::fabs(rootUndone.y) < 1e-4f);
    CHECK(std::fabs(rootUndone.z) < 1e-4f);
}

// ---------------------------------------------------------------------------
// Two regions in one division, which is where a shared vertex buffer breaks.
// ---------------------------------------------------------------------------

namespace {

/// Three bones and two quads, each quad its own region with its own one-entry
/// bone-lookup window: the first skins to bone 1, the second to bone 2, and
/// both write slot 0 into the vertex. Nothing but the region a vertex belongs
/// to distinguishes them, so a converter that pools the two regions' vertices
/// -- or their windows -- gets the second quad's bone wrong.
m3::Model makeTwoRegionModel() {
    m3::Model model = makeModel(30);

    m3::Bone second = model.bones[1];
    second.name = "child2";
    second.position.initValue = Vector3f{0, -6, 0};
    model.bones.push_back(second);

    std::vector<u8> blob = model.vertices.data;
    // Quad B sits at x 2..3, so a vertex's bone is decidable from its position.
    const Vector3f corners[4] = {{2, 0, 0}, {3, 0, 0}, {3, 1, 0}, {2, 1, 0}};
    for (const Vector3f& corner : corners) {
        pushVertex(blob, corner, 0);
    }
    model.vertices.data = std::move(blob);
    model.vertices.initialize();

    model.boneLookup = {1, 2};

    m3::MeshDivision& division = model.divisions[0];
    // Region-local corners, which is what the format stores.
    division.faces.insert(division.faces.end(), {0, 1, 2, 0, 2, 3});
    m3::Region region = division.regions[0];
    region.index = 1;
    region.firstVertex = 4;
    region.vertexCount = 4;
    region.firstIndex = 6;
    region.indexCount = 6;
    region.firstBoneLookup = 1;
    region.boneLookupCount = 1;
    division.regions.push_back(region);

    m3::Batch batch = division.batches[0];
    batch.regionIndex = 1;
    division.batches.push_back(batch);
    return model;
}

/// Every vertex of `model`, as (position, the bone its region's window names).
std::vector<std::pair<Vector3f, u32>> skinOf(const m3::Model& model) {
    std::vector<std::pair<Vector3f, u32>> out;
    const std::vector<Vector3f> positions = model.vertices.getPositions();
    const std::vector<std::array<u8, 4>> indices = model.vertices.getBoneIndices();
    const std::vector<std::array<u8, 4>> weights = model.vertices.getBoneWeights();
    for (const m3::MeshDivision& division : model.divisions) {
        for (const m3::Region& region : division.regions) {
            for (u32 v = 0; v < region.vertexCount; ++v) {
                const std::size_t g = region.firstVertex + v;
                if (g >= positions.size()) {
                    break;
                }
                for (std::size_t k = 0; k < 4; ++k) {
                    if (weights[g][k] == 0) {
                        continue;
                    }
                    const std::size_t slot = region.firstBoneLookup + indices[g][k];
                    REQUIRE(indices[g][k] < region.boneLookupCount);
                    REQUIRE(slot < model.boneLookup.size());
                    out.emplace_back(positions[g], model.boneLookup[slot]);
                }
            }
        }
    }
    return out;
}

} // namespace

TEST_CASE("wem m3 two regions keep their own vertices and windows",
          "[wem][convert][m3][skin]") {
    const M3Converter converter;
    Result<Document> doc = converter.fromM3(makeTwoRegionModel());
    REQUIRE(doc.ok());
    Result<m3::Model> back = converter.toM3(*doc.value, ProfileId::Heroes, 30);
    REQUIRE(back.ok());

    REQUIRE(back->divisions.size() == 1);
    const m3::MeshDivision& division = back->divisions[0];
    REQUIRE(division.regions.size() == 2);

    // A vertex belongs to one region: its four bone indices are slots in that
    // region's window and mean nothing in another's. Overlapping slices would
    // make the same byte have to mean two things.
    const m3::Region& a = division.regions[0];
    const m3::Region& b = division.regions[1];
    CHECK((a.firstVertex + a.vertexCount <= b.firstVertex ||
           b.firstVertex + b.vertexCount <= a.firstVertex));
    CHECK(a.boneLookupCount > 0);
    CHECK(b.boneLookupCount > 0);

    // Both quads' bones survive, and the round trip agrees with the source
    // vertex for vertex -- position decides which quad, the window the bone.
    const std::vector<std::pair<Vector3f, u32>> before = skinOf(makeTwoRegionModel());
    const std::vector<std::pair<Vector3f, u32>> after = skinOf(*back.value);
    REQUIRE(before.size() == 8);
    REQUIRE(after.size() == before.size());
    for (const auto& [position, bone] : after) {
        CHECK(bone == (position.x < 1.5f ? 1u : 2u));
    }
}

// ---------------------------------------------------------------------------
// A region's bone palette is a hardware limit, not a preference.
// ---------------------------------------------------------------------------

namespace {

/// One region skinned across `bones` bones, one triangle per bone, so the
/// window grows by exactly one per triangle and any cap has to cut it.
m3::Model makeWideSkinModel(u32 bones) {
    m3::Model model = makeModel(29);
    model.bones.resize(1);
    m3::Bone limb = model.bones[0];
    limb.parentIndex = 0;
    for (u32 i = 0; i < bones; ++i) {
        limb.name = "limb" + std::to_string(i);
        model.bones.push_back(limb);
    }

    std::vector<u8> blob;
    std::vector<u16> faces;
    model.boneLookup.clear();
    for (u32 i = 0; i < bones; ++i) {
        const f32 x = static_cast<f32>(i);
        pushVertex(blob, Vector3f{x, 0, 0}, static_cast<u8>(i));
        pushVertex(blob, Vector3f{x, 1, 0}, static_cast<u8>(i));
        pushVertex(blob, Vector3f{x, 0, 1}, static_cast<u8>(i));
        faces.push_back(static_cast<u16>(3 * i));
        faces.push_back(static_cast<u16>(3 * i + 1));
        faces.push_back(static_cast<u16>(3 * i + 2));
        model.boneLookup.push_back(static_cast<u16>(i + 1));
    }
    model.vertices.flags = m3::VertexFormatFlag::UV1;
    model.vertices.data = std::move(blob);
    model.vertices.initialize();

    m3::MeshDivision division;
    division.faces = std::move(faces);
    m3::Region region;
    region.firstVertex = 0;
    region.vertexCount = 3 * bones;
    region.firstIndex = 0;
    region.indexCount = 3 * bones;
    region.firstBoneLookup = 0;
    region.boneLookupCount = static_cast<u16>(bones);
    region.rootBone = 0;
    division.regions.push_back(region);

    m3::Batch batch;
    batch.regionIndex = 0;
    batch.materialIndex = 0;
    batch.boneCount = 0xFFFF;
    division.batches.push_back(batch);

    model.divisions.clear();
    model.divisions.push_back(division);
    return model;
}

} // namespace

TEST_CASE("wem m3 a region never names more bones than the palette holds",
          "[wem][convert][m3][skin]") {
    // A draw whose bone palette overruns the vertex shader's matrix registers
    // fails with D3DERR_INVALIDCALL, and the Galaxy editor answers that by
    // resetting the device -- every frame, onto a black viewport, with the model
    // loaded and no complaint made about it. No shipped v5 region names more
    // than 63 bones: 436 of the corpus's 67,320 regions sit exactly there and
    // none goes past, so a range needing more is split, not clamped.
    constexpr u32 kBones = 100;
    constexpr u16 kCap = 63;
    CHECK(Profile(ProfileId::Sc2).maxBonesPerPalette == kCap);
    CHECK(Profile(ProfileId::Heroes).maxBonesPerPalette == kCap);

    const M3Converter converter;
    Result<Document> document = converter.fromM3(makeWideSkinModel(kBones), ProfileId::Sc2);
    REQUIRE(document.ok());
    Result<m3::Model> written = converter.toM3(*document, ProfileId::Sc2, 29);
    REQUIRE(written.ok());
    REQUIRE(written->divisions.size() == 1);

    const m3::MeshDivision& division = written->divisions[0];
    REQUIRE(division.regions.size() > 1);
    u32 corners = 0;
    for (const m3::Region& region : division.regions) {
        CHECK(region.boneLookupCount <= kCap);
        corners += region.indexCount;
    }
    // A split redistributes triangles; it drops none of them.
    CHECK(corners == 3 * kBones);

    // And every region it produced still draws, under the material the one
    // region started with: a region no batch names is geometry never submitted.
    REQUIRE(division.batches.size() == division.regions.size());
    std::vector<bool> drawn(division.regions.size(), false);
    for (const m3::Batch& batch : division.batches) {
        REQUIRE(batch.regionIndex < division.regions.size());
        CHECK(batch.materialIndex == 0);
        drawn[batch.regionIndex] = true;
    }
    CHECK(std::find(drawn.begin(), drawn.end(), false) == drawn.end());
}

// ---------------------------------------------------------------------------
// The vertex declaration is the model's, not the converter's.
// ---------------------------------------------------------------------------

namespace {

/// 40 bytes: the base 24, a BGRA colour, two UV pairs, and the tangent. The
/// bits outside the layout set are ones this reader cannot name -- they are
/// here to be carried, not understood. `0x01800061` is the part every one of
/// the 5,510 corpus models states, so the whole word is a real declaration
/// (124 shipped models carry exactly this one) rather than an invented mix.
constexpr u32 kRichFlags = 0x01800061u | 0x0200u | 0x20000u | 0x40000u;
constexpr std::size_t kRichStride = 40;

void pushRichVertex(std::vector<u8>& blob, const Vector3f& position, const Vector2f& uv0,
                    const Vector2f& uv1, const std::array<u8, 4>& rgba,
                    const std::array<u8, 3>& tangent) {
    const std::size_t base = blob.size();
    blob.resize(base + kRichStride, 0);
    std::memcpy(blob.data() + base, &position, sizeof(Vector3f));
    blob[base + 12] = 255; // weight 0 = 1.0
    blob[base + 16] = 0;   // slot 0 of the region's window
    blob[base + 20] = 128; // normal ~ (0, 0, 1)
    blob[base + 21] = 128;
    blob[base + 22] = 255;
    blob[base + 23] = 0; // bitangent handedness -1
    blob[base + 24] = rgba[2];
    blob[base + 25] = rgba[1];
    blob[base + 26] = rgba[0];
    blob[base + 27] = rgba[3];
    const i16 coords[4] = {static_cast<i16>(uv0.x * 2048.0f), static_cast<i16>(uv0.y * 2048.0f),
                           static_cast<i16>(uv1.x * 2048.0f), static_cast<i16>(uv1.y * 2048.0f)};
    std::memcpy(blob.data() + base + 28, coords, sizeof(coords));
    blob[base + 36] = tangent[0];
    blob[base + 37] = tangent[1];
    blob[base + 38] = tangent[2];
    blob[base + 39] = 255;
}

m3::Model makeRichModel() {
    m3::Model model = makeModel(30);
    std::vector<u8> blob;
    const Vector3f corners[4] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
    for (std::size_t i = 0; i < 4; ++i) {
        // Exact multiples of 1/2048, so nothing here is a rounding question.
        const f32 t = static_cast<f32>(i) / 2048.0f;
        pushRichVertex(blob, corners[i], Vector2f{t, 0.5f}, Vector2f{0.25f, t},
                       {static_cast<u8>(10 * i), 20, 30, 255},
                       {static_cast<u8>(200 + i), 60, 70});
    }
    model.vertices.flags = static_cast<m3::VertexFormatFlag>(kRichFlags);
    model.vertices.data = std::move(blob);
    model.vertices.initialize();
    return model;
}

} // namespace

TEST_CASE("wem m3 carries the vertex declaration it was given", "[wem][convert][m3]") {
    const m3::Model source = makeRichModel();
    REQUIRE(source.vertices.vertexSize() == kRichStride);
    REQUIRE(source.vertices.UVsNum() == 2);
    REQUIRE(source.vertices.hasVertexColors());

    const M3Converter converter;
    Result<Document> doc = converter.fromM3(source);
    REQUIRE(doc.ok());
    Result<m3::Model> back = converter.toM3(*doc.value, ProfileId::Heroes, 30);
    REQUIRE(back.ok());

    // The word itself, opaque bits included: only the UV and colour bits are
    // this converter's to decide.
    CHECK(static_cast<u32>(back->vertices.flags) == kRichFlags);
    CHECK(back->vertices.vertexSize() == kRichStride);
    REQUIRE(back->vertices.vertexCount() == 4);

    // Position identifies a vertex; the render view is free to reorder them.
    const std::vector<Vector3f> positions = back->vertices.getPositions();
    const std::vector<Vector2f> uv0 = back->vertices.getUVs(0);
    const std::vector<Vector2f> uv1 = back->vertices.getUVs(1);
    const std::vector<m3::ColorBGRA> colors = back->vertices.getColors();
    const std::vector<Vector4f> tangents = back->vertices.getTangents();
    REQUIRE(uv1.size() == 4);
    REQUIRE(colors.size() == 4);

    const std::vector<Vector2f> srcUv0 = source.vertices.getUVs(0);
    const std::vector<Vector2f> srcUv1 = source.vertices.getUVs(1);
    const std::vector<m3::ColorBGRA> srcColors = source.vertices.getColors();
    const std::vector<Vector4f> srcTangents = source.vertices.getTangents();
    const std::vector<Vector3f> srcPositions = source.vertices.getPositions();

    std::size_t matched = 0;
    for (std::size_t v = 0; v < positions.size(); ++v) {
        for (std::size_t s = 0; s < srcPositions.size(); ++s) {
            if (positions[v].x != srcPositions[s].x || positions[v].y != srcPositions[s].y) {
                continue;
            }
            ++matched;
            CHECK(uv0[v].x == srcUv0[s].x);
            CHECK(uv0[v].y == srcUv0[s].y);
            CHECK(uv1[v].x == srcUv1[s].x);
            CHECK(uv1[v].y == srcUv1[s].y);
            CHECK(colors[v].r == srcColors[s].r);
            CHECK(colors[v].g == srcColors[s].g);
            CHECK(colors[v].b == srcColors[s].b);
            CHECK(colors[v].a == srcColors[s].a);
            for (int c = 0; c < 3; ++c) {
                CHECK(tangents[v].data[c] == srcTangents[s].data[c]);
            }
            // The bitangent's handedness rides the normal's fourth byte.
            CHECK(tangents[v].w == srcTangents[s].w);
            break;
        }
    }
    CHECK(matched == 4);
}

// ── the Warcraft III crossings, against Blizzard's own answer key ────────────
//
// `mods/war3.sc2mod` (3,004 shipped conversions) settles each of these:
// see WC3_TO_SC2_DESIGN.md §1.

namespace {

/// A one-slot Sc2 document around @p material, with texture @p replaceable
/// marked as that Warcraft III replaceable id.
Document wc3Document(Material material, u32 replaceableTexture = 0, u32 replaceableId = 0) {
    Document document = wemfix::makeDocument(ProfileId::Sc2);
    Model& model = document.models[0];
    model.materialSlots = {"body"};
    model.meshes.clear();
    model.meshes.push_back(wemfix::makeMesh({"body"}));
    for (MeshSection& section : model.meshes[0].sections) {
        section.profiles = ProfileBit(ProfileId::Sc2);
    }
    std::vector<Material> materials;
    materials.push_back(std::move(material));
    model.profileSets.clear();
    model.profileSets.push_back(wemfix::makeSet(ProfileId::Sc2, std::move(materials)));
    if (replaceableId != 0 && replaceableTexture < document.textures.size()) {
        document.textures[replaceableTexture].replaceableId = replaceableId;
        document.textures[replaceableTexture].path.clear();
        document.textures[replaceableTexture].key = TexturePath{""};
    }
    return document;
}

const m3::StandardMaterial& firstMaterial(const Result<m3::Model>& written) {
    REQUIRE(written.ok());
    REQUIRE_FALSE(written->standardMaterials.empty());
    return written->standardMaterials[0];
}

/// The same document, authored in Warcraft III: the pass fold then picks a
/// pass's home by its shading and reads both additive filters as the
/// alpha-weighted add (WC3_SD_MATERIAL_TO_SC2_DESIGN.md §5).
Document warcraftDocument(Material material, u32 replaceableTexture = 0,
                          u32 replaceableId = 0) {
    Document document = wc3Document(std::move(material), replaceableTexture, replaceableId);
    document.declare(ProfileId::Wc3Classic);
    document.defaultProfile = ProfileId::Wc3Classic;
    return document;
}

CompositeLayer colorLayer(u32 texture, CompositeOp op, f32 weight = 1.0f) {
    CompositeLayer layer;
    layer.input = wemfix::makeInput(texture);
    layer.input.weight = weight;
    layer.target = SurfaceChannel::Color;
    layer.op = op;
    return layer;
}

Material stack(const char* name, std::vector<CompositeLayer> layers) {
    Material material;
    material.name = name;
    CompositeBody body;
    body.layers = std::move(layers);
    material.InitCommon().body = std::move(body);
    return material;
}

/// Pass @p ordinal keeps its own shading (a `LayerShading` feature).
void shade(Material& material, u32 ordinal, bool unlit, bool twoSided = false) {
    LayerShadingFeature shading;
    shading.unlit = unlit;
    shading.twoSided = twoSided;
    MaterialFeature feature;
    feature.id = NextFeatureId(material.Common().features);
    feature.layer = ordinal;
    feature.payload = shading;
    material.MutableCommon().features.push_back(feature);
}

/// An alpha track on pass @p ordinal of slot 0, keyed @p keys over one second.
u32 alphaTrack(Document& document, u32 ordinal, std::vector<f32> keys) {
    Model& model = document.models.front();
    AnimChannel channel;
    channel.id = (std::max)(1u, model.animChannels.nextFreeId());
    channel.target.kind = TrackTarget::Kind::MaterialLayer;
    channel.target.material.profile = ProfileId::Sc2;
    channel.target.material.slot = 0;
    channel.target.material.look = 0;
    channel.target.sub = ordinal;
    channel.target.channel = Channel::Alpha;
    channel.valueType = geom::AttrType::F32;
    model.animChannels.add(channel);

    if (document.clips.empty()) {
        Clip clip;
        clip.name = "Stand";
        clip.model = 0;
        clip.duration = 1.0f;
        clip.containers.push_back(SubTrackContainer{});
        document.clips.push_back(std::move(clip));
    }
    SubTrack track;
    track.channel = channel.id;
    track.interp = Interpolation::Linear;
    for (std::size_t k = 0; k < keys.size(); ++k) {
        track.times.push_back(static_cast<f32>(k) / static_cast<f32>((std::max)(keys.size(), std::size_t{2}) - 1));
    }
    track.values.resize(keys.size() * sizeof(f32));
    std::memcpy(track.values.data(), keys.data(), track.values.size());
    document.clips[0].containers[0].subTracks.push_back(std::move(track));
    return channel.id;
}

} // namespace

TEST_CASE("wem m3 a warcraft team stack folds to the RGBA select",
          "[wem][convert][m3][team]") {
    // Replaceable 1 UNDER a keyed diffuse: Blizzard's conversions write ONE
    // diffuse with the RGBA select (texture alpha = team mask, uninverted)
    // and no alpha test -- the test would cut the team regions out.
    Material material;
    material.name = "team";
    CompositeBody body;
    CompositeLayer team;
    team.input = wemfix::makeInput(1);
    team.target = SurfaceChannel::Color;
    team.op = CompositeOp::Set;
    body.layers.push_back(team);
    CompositeLayer diffuse;
    diffuse.input = wemfix::makeInput(0);
    diffuse.target = SurfaceChannel::Color;
    diffuse.op = CompositeOp::AlphaKey;
    body.layers.push_back(diffuse);
    material.InitCommon().body = std::move(body);
    material.MutableCommon().blend = BlendMode::AlphaKey;
    material.MutableCommon().alphaTestThreshold = 0.75f;

    Document document = wc3Document(std::move(material), 1, 1);
    const M3Converter converter;
    Result<m3::Model> written = converter.toM3(document, ProfileId::Sc2, 29);
    const m3::StandardMaterial& out = firstMaterial(written);
    REQUIRE(out.diffuseLayer.has_value());
    CHECK(out.diffuseLayer->texturePath == "tex0.dds");
    CHECK(out.diffuseLayer->colorType == m3::ColorChannelSelect::RGBA);
    CHECK(out.alphaTestThreshold == 0);
    CHECK_FALSE(out.alphaLayer1.has_value());
}

TEST_CASE("wem m3 a team glow layer is the team emissive op",
          "[wem][convert][m3][team]") {
    // Replaceable 2, additive: the oracle's `TeamGlow` material -- an
    // emissive whose op is TeamColorEmissiveAdd with a RED select; the
    // texture is the mask, the team colour supplies the RGB.
    Material material;
    material.name = "glow";
    CompositeBody body;
    CompositeLayer glow;
    glow.input = wemfix::makeInput(2);
    glow.target = SurfaceChannel::Color;
    glow.op = CompositeOp::Set;
    body.layers.push_back(glow);
    material.InitCommon().body = std::move(body);
    material.MutableCommon().blend = BlendMode::Additive;

    Document document = wc3Document(std::move(material), 2, 2);
    const M3Converter converter;
    Result<m3::Model> written = converter.toM3(document, ProfileId::Sc2, 29);
    const m3::StandardMaterial& out = firstMaterial(written);
    CHECK_FALSE(out.diffuseLayer.has_value());
    REQUIRE(out.emissiveLayer1.has_value());
    CHECK(out.emissiveBlendMode1 == m3::LayerBlendOp::TeamColorEmissiveAdd);
    CHECK(out.emissiveLayer1->colorType == m3::ColorChannelSelect::Red);
}

TEST_CASE("wem m3 a keyed material states its own coverage",
          "[wem][convert][m3][coverage]") {
    // StarCraft II tests the COMPOSED alpha; without a mask nothing is ever
    // cut. Opaque + threshold 192 + an Alpha select on the diffuse's own
    // texture is, texel for texel, the oracle's spelling of a Warcraft III
    // Transparent filter (192 = 0.75 * 256 -- truncating through 255 gives
    // the off-by-one 191).
    Material material = wemfix::makeComposite("cut");
    material.MutableCommon().blend = BlendMode::AlphaKey;
    material.MutableCommon().alphaTestThreshold = 0.75f;

    Document document = wc3Document(std::move(material));
    const M3Converter converter;
    Result<m3::Model> written = converter.toM3(document, ProfileId::Sc2, 29);
    const m3::StandardMaterial& out = firstMaterial(written);
    CHECK(out.alphaTestThreshold == 192);
    REQUIRE(out.alphaLayer1.has_value());
    CHECK(out.alphaLayer1->texturePath == "tex0.dds");
    CHECK(out.alphaLayer1->colorType == m3::ColorChannelSelect::Alpha);
    REQUIRE(out.diffuseLayer.has_value());
    // The multiply rests at one -- the struct's zero is a black layer.
    CHECK(out.diffuseLayer->rgbMultiply.initValue == Catch::Approx(1.0f));
}

TEST_CASE("wem m3 an additive second colour layer is a glow",
          "[wem][convert][m3][layers]") {
    // A second colour PASS has no slot of its own; Blizzard's conversions put
    // an additive one in the emissive slot (638 materials) and never stack
    // batches (0 of 3,004 models).
    Material material;
    material.name = "shine";
    CompositeBody body;
    CompositeLayer base;
    base.input = wemfix::makeInput(0);
    base.target = SurfaceChannel::Color;
    base.op = CompositeOp::Set;
    body.layers.push_back(base);
    CompositeLayer sheen;
    sheen.input = wemfix::makeInput(3);
    sheen.target = SurfaceChannel::Color;
    sheen.op = CompositeOp::Add;
    body.layers.push_back(sheen);
    material.InitCommon().body = std::move(body);

    Document document = wc3Document(std::move(material));
    const M3Converter converter;
    Result<m3::Model> written = converter.toM3(document, ProfileId::Sc2, 29);
    const m3::StandardMaterial& out = firstMaterial(written);
    REQUIRE(out.emissiveLayer1.has_value());
    CHECK(out.emissiveLayer1->texturePath == "tex3.dds");
    CHECK(out.emissiveBlendMode1 == m3::LayerBlendOp::AddNoAlpha);
}

// ============================================================================
// The pass fold (WC3_SD_MATERIAL_TO_SC2_DESIGN.md §5)
// ============================================================================

TEST_CASE("wem m3 a blend pass picks its home by shading", "[wem][convert][m3][fold]") {
    // Unlit -> emissive Lerp, after lighting; lit -> decal Lerp, before it.
    // Both exact; the first draft only had the decal.
    for (const bool unlit : {true, false}) {
        Material material = stack("trim", {colorLayer(0, CompositeOp::Set),
                                           colorLayer(3, CompositeOp::AlphaBlend)});
        shade(material, 1, unlit);
        Document document = warcraftDocument(std::move(material));
        const M3Converter converter;
        Result<m3::Model> written = converter.toM3(document, ProfileId::Sc2, 29);
        const m3::StandardMaterial& out = firstMaterial(written);
        if (unlit) {
            REQUIRE(out.emissiveLayer1.has_value());
            CHECK(out.emissiveLayer1->texturePath == "tex3.dds");
            CHECK(out.emissiveBlendMode1 == m3::LayerBlendOp::Lerp);
            CHECK(out.emissiveLayer1->colorType == m3::ColorChannelSelect::RGBA);
            CHECK_FALSE(out.decalLayer.has_value());
        } else {
            REQUIRE(out.decalLayer.has_value());
            CHECK(out.decalLayer->texturePath == "tex3.dds");
            CHECK(out.layerBlendMode == m3::LayerBlendOp::Lerp);
            CHECK_FALSE(out.emissiveLayer1.has_value());
        }
        CHECK(written->compositeMaterials.empty());
        CHECK(written.diagnostics.countOf(DiagCode::UnlitFold) == 0);
        CHECK(written.diagnostics.countOf(DiagCode::LitFold) == 0);
    }
}

TEST_CASE("wem m3 a built layer states the rests every shipped layer does",
          "[wem][convert][m3][fold]") {
    // Fields the renderer in this build never reads and the game does: over
    // 3,679 sampled shipped textured layers the multiply and map alpha rest at
    // one, the tiling at (1, 1), the W tiling is one, and the specular
    // multiplier of the material is at least one on 1,270 of 1,275 -- 436 of
    // them, like this one, with no specular layer to scale.
    Material material = stack("skin", {colorLayer(0, CompositeOp::Set)});
    Document document = warcraftDocument(std::move(material));
    const M3Converter converter;
    Result<m3::Model> written = converter.toM3(document, ProfileId::Sc2, 29);
    const m3::StandardMaterial& out = firstMaterial(written);
    REQUIRE(out.diffuseLayer.has_value());
    const m3::TextureLayer& diffuse = *out.diffuseLayer;
    CHECK(diffuse.uvSourceRelated == 0xFFFFFFFFu);
    CHECK(diffuse.fresnelMode == m3::FresnelMode::None);
    CHECK(diffuse.fresnelExponent == 4.0f);
    CHECK(diffuse.fresnelMax == 1.0f);
    CHECK(diffuse.rgbMultiply.initValue == 1.0f);
    CHECK(diffuse.rgbMultiply.nullValue == 1.0f);
    CHECK(diffuse.mapAlpha.nullValue == 1.0f);
    CHECK(diffuse.uvTiling.nullValue.x == 1.0f);
    CHECK(diffuse.uvTiling.nullValue.y == 1.0f);
    CHECK(diffuse.wTiling.initValue == 1.0f);
    CHECK(diffuse.triplanarScale.initValue.z == 1.0f);
    CHECK_FALSE(out.specularLayer.has_value());
    CHECK(out.hdrSpecularMultiplier == Catch::Approx(1.0f));
}

TEST_CASE("wem m3 a solid-colour carrier rests its multiply and map alpha at one",
          "[wem][convert][m3][fold]") {
    // The game restores a written layer constant to its null after the draw
    // and skips a still layer whose init equals its null. A carrier resting at
    // zero left the alpha-mask slot at zero, and the footman's alpha-tested
    // body, whose mask rests at one, drew fully clipped.
    Material material = stack("banshee", {colorLayer(0, CompositeOp::Set, 0.66f)});
    material.MutableCommon().blend = BlendMode::AlphaBlend;
    Document document = warcraftDocument(std::move(material));
    const M3Converter converter;
    Result<m3::Model> written = converter.toM3(document, ProfileId::Sc2, 29);
    const m3::StandardMaterial& out = firstMaterial(written);
    REQUIRE(out.alphaLayer2.has_value());
    REQUIRE(hasFlag(out.alphaLayer2->flags, m3::TextureLayerFlag::Color));
    CHECK(out.alphaLayer2->rgbMultiply.nullValue == 1.0f);
    CHECK(out.alphaLayer2->mapAlpha.nullValue == 1.0f);

    // The fade carriers the animation and section paths build start from the
    // struct, so its rests are theirs.
    const m3::TextureLayer blank;
    CHECK(blank.rgbMultiply.nullValue == 1.0f);
    CHECK(blank.mapAlpha.nullValue == 1.0f);
    CHECK(blank.rgbAdd.nullValue == 0.0f);
}

TEST_CASE("wem m3 a lit additive pass takes the decal", "[wem][convert][m3][fold]") {
    // Lighting is linear in albedo, so `lit(base + t) = lit(base) + lit(t)`:
    // the decal's Add is a shaded additive pass exactly. Over an UNLIT base
    // the decal is unlit too, so it goes to an emissive and says so.
    Material lit = stack("lava", {colorLayer(0, CompositeOp::Set),
                                  colorLayer(3, CompositeOp::AddAlpha)});
    Document document = warcraftDocument(std::move(lit));
    const M3Converter converter;
    Result<m3::Model> written = converter.toM3(document, ProfileId::Sc2, 29);
    const m3::StandardMaterial& out = firstMaterial(written);
    REQUIRE(out.decalLayer.has_value());
    CHECK(out.layerBlendMode == m3::LayerBlendOp::Add);
    CHECK(out.decalLayer->colorType == m3::ColorChannelSelect::RGBA);
    CHECK_FALSE(out.emissiveLayer1.has_value());
    CHECK(out.hdrEmissiveMultiplier == Catch::Approx(1.0f));

    Material unlitBase = stack("lava", {colorLayer(0, CompositeOp::Set),
                                        colorLayer(3, CompositeOp::AddAlpha)});
    unlitBase.MutableCommon().flags |= MaterialFlags::Unlit;
    shade(unlitBase, 1, false);
    Document second = warcraftDocument(std::move(unlitBase));
    Result<m3::Model> folded = converter.toM3(second, ProfileId::Sc2, 29);
    const m3::StandardMaterial& approx = firstMaterial(folded);
    REQUIRE(approx.emissiveLayer1.has_value());
    CHECK(approx.emissiveBlendMode1 == m3::LayerBlendOp::Add);
    CHECK(folded.diagnostics.countOf(DiagCode::ShadedAdditiveFolded) == 1);
}

TEST_CASE("wem m3 both warcraft additive filters are the alpha-weighted add",
          "[wem][convert][m3][fold]") {
    // The engine draws Additive and AddAlpha alike (SrcAlpha, One), so both
    // are `Add` -- and the multiplier that scales the sum is written as 1,
    // where its zero is a black glow in the real game.
    Material material = stack("glow", {colorLayer(0, CompositeOp::Set),
                                       colorLayer(3, CompositeOp::Add),
                                       colorLayer(4, CompositeOp::AddAlpha)});
    shade(material, 1, true);
    shade(material, 2, true);
    Document document = warcraftDocument(std::move(material));
    const M3Converter converter;
    Result<m3::Model> written = converter.toM3(document, ProfileId::Sc2, 29);
    const m3::StandardMaterial& out = firstMaterial(written);
    REQUIRE(out.emissiveLayer1.has_value());
    REQUIRE(out.emissiveLayer2.has_value());
    CHECK(out.emissiveBlendMode1 == m3::LayerBlendOp::Add);
    CHECK(out.emissiveBlendMode2 == m3::LayerBlendOp::Add);
    CHECK(out.emissiveLayer1->texturePath == "tex3.dds");
    CHECK(out.emissiveLayer2->texturePath == "tex4.dds");
    CHECK(out.hdrEmissiveMultiplier == Catch::Approx(1.0f));
    CHECK(written->compositeMaterials.empty());
}

TEST_CASE("wem m3 an additive-only stack is emissive-only", "[wem][convert][m3][fold]") {
    // No diffuse: every pass an emissive weighted by ITS OWN alpha, so the
    // sum is Warcraft III's sum of passes -- the diffuse spelling would have
    // weighted the second pass by the first's alpha.
    Material material = stack("fx", {colorLayer(0, CompositeOp::Set),
                                     colorLayer(3, CompositeOp::AddAlpha)});
    material.MutableCommon().blend = BlendMode::AdditiveAlpha;
    material.MutableCommon().flags |= MaterialFlags::Unlit;
    Document document = warcraftDocument(std::move(material));
    const M3Converter converter;
    Result<m3::Model> written = converter.toM3(document, ProfileId::Sc2, 29);
    const m3::StandardMaterial& out = firstMaterial(written);
    CHECK_FALSE(out.diffuseLayer.has_value());
    CHECK_FALSE(out.alphaLayer1.has_value());
    CHECK(out.blendMode == m3::BlendMode::AlphaAdd);
    REQUIRE(out.emissiveLayer1.has_value());
    REQUIRE(out.emissiveLayer2.has_value());
    CHECK(out.emissiveLayer1->texturePath == "tex0.dds");
    CHECK(out.emissiveLayer2->texturePath == "tex3.dds");
    CHECK(out.emissiveBlendMode1 == m3::LayerBlendOp::Add);
    CHECK(out.emissiveBlendMode2 == m3::LayerBlendOp::Add);
    CHECK(out.emissiveLayer1->colorType == m3::ColorChannelSelect::RGBA);

    // The ghost heroes: a team plate drawn additive under an additive
    // texture. The plate is a team add on a solid-colour carrier.
    Material ghost = stack("ghost", {colorLayer(1, CompositeOp::Set),
                                     colorLayer(3, CompositeOp::AddAlpha, 0.5f)});
    ghost.MutableCommon().blend = BlendMode::Additive;
    ghost.MutableCommon().flags |= MaterialFlags::Unlit;
    Document haunted = warcraftDocument(std::move(ghost), 1, 1);
    Result<m3::Model> spectral = converter.toM3(haunted, ProfileId::Sc2, 29);
    const m3::StandardMaterial& spirit = firstMaterial(spectral);
    CHECK_FALSE(spirit.diffuseLayer.has_value());
    REQUIRE(spirit.emissiveLayer1.has_value());
    CHECK(hasFlag(spirit.emissiveLayer1->flags, m3::TextureLayerFlag::Color));
    CHECK(spirit.emissiveBlendMode1 == m3::LayerBlendOp::TeamColorDiffuseAdd);
    REQUIRE(spirit.emissiveLayer2.has_value());
    CHECK(spirit.emissiveBlendMode2 == m3::LayerBlendOp::Add);
    // The pass's static alpha rides its layer, where retail reads `mapAlpha`
    // and this build reads the tint alpha.
    CHECK(spirit.emissiveLayer2->mapAlpha.initValue == Catch::Approx(0.5f));
    CHECK(spirit.emissiveLayer2->color.initValue.a == 128);
    CHECK(spirit.blendMode == m3::BlendMode::AlphaAdd);
}

TEST_CASE("wem m3 a later team plate is a weighted team add", "[wem][convert][m3][fold]") {
    // `lerp(base, team, w)`: the diffuse at `1 - w`, the team colour added at
    // `w` through a solid-colour decal. The RGBA lerp keys on the RAW sampled
    // alpha, so no tint can do it.
    Material material = stack("soldier", {colorLayer(0, CompositeOp::Set),
                                          colorLayer(1, CompositeOp::AlphaBlend, 0.4f),
                                          colorLayer(3, CompositeOp::Modulate)});
    shade(material, 2, true);
    Document document = warcraftDocument(std::move(material), 1, 1);
    const M3Converter converter;
    Result<m3::Model> written = converter.toM3(document, ProfileId::Sc2, 29);
    const m3::StandardMaterial& out = firstMaterial(written);
    REQUIRE(out.diffuseLayer.has_value());
    CHECK(out.diffuseLayer->texturePath == "tex0.dds");
    CHECK(out.diffuseLayer->rgbMultiply.initValue == Catch::Approx(0.6f));
    REQUIRE(out.decalLayer.has_value());
    CHECK(hasFlag(out.decalLayer->flags, m3::TextureLayerFlag::Color));
    CHECK(out.decalLayer->color.initValue.a == 102);
    CHECK(out.layerBlendMode == m3::LayerBlendOp::TeamColorDiffuseAdd);
    // The modulate that follows takes an emissive slot (post-lighting, exact
    // for an unlit pass): one material for three passes.
    REQUIRE(out.emissiveLayer1.has_value());
    CHECK(out.emissiveBlendMode1 == m3::LayerBlendOp::Mod);
    CHECK(written->compositeMaterials.empty());

    // At full weight the plate covers the base: solid team colour.
    Material covered = stack("flag", {colorLayer(0, CompositeOp::Set),
                                      colorLayer(1, CompositeOp::AlphaBlend, 1.0f)});
    Document banner = warcraftDocument(std::move(covered), 1, 1);
    Result<m3::Model> flag = converter.toM3(banner, ProfileId::Sc2, 29);
    const m3::StandardMaterial& solid = firstMaterial(flag);
    REQUIRE(solid.diffuseLayer.has_value());
    CHECK(solid.diffuseLayer->colorType == m3::ColorChannelSelect::RGBA);
    CHECK_FALSE(solid.decalLayer.has_value());
}

TEST_CASE("wem m3 the same texture keyed over its fading self is a coverage switch",
          "[wem][convert][m3][fold]") {
    // The city buildings: an opaque pass that fades out under a keyed pass
    // of the same texels. Alive every texel passes the test (add 1, clamped);
    // dead only the keyed ones do (add 0). The base's alpha track drives the
    // add, not a carrier.
    Material material = stack("ruin", {colorLayer(0, CompositeOp::Set),
                                       colorLayer(0, CompositeOp::AlphaKey)});
    shade(material, 1, false, true);
    Document document = warcraftDocument(std::move(material));
    alphaTrack(document, 0, {1.0f, 0.0f});
    const M3Converter converter;
    Result<m3::Model> written = converter.toM3(document, ProfileId::Sc2, 29);
    const m3::StandardMaterial& out = firstMaterial(written);
    CHECK(out.blendMode == m3::BlendMode::Opaque);
    CHECK(out.alphaTestThreshold == 192);
    REQUIRE(out.alphaLayer1.has_value());
    CHECK(out.alphaLayer1->texturePath == "tex0.dds");
    CHECK(out.alphaLayer1->colorType == m3::ColorChannelSelect::Alpha);
    CHECK(hasFlag(out.alphaLayer1->flags, m3::TextureLayerFlag::ColorClamp));
    CHECK(out.alphaLayer1->rgbAdd.initValue == Catch::Approx(1.0f));
    CHECK(out.alphaLayer1->rgbAdd.animId != 0);
    CHECK_FALSE(out.alphaLayer2.has_value());
    CHECK(hasFlag(out.flags, m3::MaterialFlag::TwoSided));
    CHECK(written.diagnostics.countOf(DiagCode::PassFlagsFolded) == 1);
    CHECK(written.diagnostics.countOf(DiagCode::BaseFadeShared) == 0);
    CHECK(written->compositeMaterials.empty());
}

TEST_CASE("wem m3 a blend after an additive emissive takes the first slot",
          "[wem][convert][m3][fold]") {
    // Retail's accumulator oddity: a Mod-family emissive2 after an Add-family
    // emissive1 lands on the accumulator. The unlit blend takes slot 1 and the
    // add moves to slot 2 -- the sum is applied after both either way -- and
    // the fold says the add is no longer blended down. Its own section under
    // exactPasses.
    Material material = stack("order", {colorLayer(0, CompositeOp::Set),
                                        colorLayer(3, CompositeOp::AddAlpha),
                                        colorLayer(4, CompositeOp::AlphaBlend)});
    shade(material, 1, true);
    shade(material, 2, true);
    Document document = warcraftDocument(std::move(material));
    const M3Converter converter;
    Result<m3::Model> written = converter.toM3(document, ProfileId::Sc2, 29);
    const m3::StandardMaterial& out = firstMaterial(written);
    REQUIRE(out.emissiveLayer1.has_value());
    CHECK(out.emissiveLayer1->texturePath == "tex4.dds");
    CHECK(out.emissiveBlendMode1 == m3::LayerBlendOp::Lerp);
    REQUIRE(out.emissiveLayer2.has_value());
    CHECK(out.emissiveLayer2->texturePath == "tex3.dds");
    CHECK(out.emissiveBlendMode2 == m3::LayerBlendOp::Add);
    CHECK_FALSE(out.decalLayer.has_value());
    CHECK(written.diagnostics.countOf(DiagCode::PassOrderFolded) == 1);
    CHECK(written->compositeMaterials.empty());

    M3ExportSettings exact;
    exact.exactPasses = true;
    Result<m3::Model> split = converter.toM3(document, ProfileId::Sc2, 29, exact);
    REQUIRE(split.ok());
    REQUIRE(split->compositeMaterials.size() == 1);
    CHECK(split->compositeMaterials[0].sections.size() == 2);
    CHECK(split.diagnostics.countOf(DiagCode::CompositeEmitted) == 1);
}

TEST_CASE("wem m3 a stack no material can carry folds maximal runs into a composite",
          "[wem][convert][m3][fold]") {
    // The Jackal Tank turret: an additive base under a keyed pass (one blend
    // state cannot add and then replace), a second keyed pass, an additive,
    // and an unlit blend that takes the emissive slot ahead of the add. Two
    // sections, not five; the sections' map entries follow the slots.
    Material material = stack("turret", {colorLayer(0, CompositeOp::Set),
                                         colorLayer(1, CompositeOp::AlphaKey),
                                         colorLayer(2, CompositeOp::AlphaKey),
                                         colorLayer(3, CompositeOp::AddAlpha),
                                         colorLayer(4, CompositeOp::AlphaBlend)});
    material.MutableCommon().blend = BlendMode::AdditiveAlpha;
    shade(material, 3, true);
    shade(material, 4, true);
    Document document = warcraftDocument(std::move(material));
    const M3Converter converter;
    Result<m3::Model> written = converter.toM3(document, ProfileId::Sc2, 29);
    REQUIRE(written.ok());
    REQUIRE(written->materialMaps.size() == 1 + 2);
    CHECK(written->materialMaps[0].materialType == m3::MaterialType::Composite);
    REQUIRE(written->compositeMaterials.size() == 1);
    const m3::CompositeMaterial& composite = written->compositeMaterials[0];
    REQUIRE(composite.sections.size() == 2);
    REQUIRE(written->standardMaterials.size() == 2);
    for (std::size_t k = 0; k < 2; ++k) {
        const u32 map = composite.sections[k].materialIndex;
        REQUIRE(map == 1 + k);
        CHECK(written->materialMaps[map].materialType == m3::MaterialType::Standard);
        CHECK(composite.sections[k].mapMultiplier.initValue == Catch::Approx(1.0f));
    }
    // Section 0: the additive base alone (AlphaAdd, its own alpha as coverage).
    const m3::StandardMaterial& first = written->standardMaterials[0];
    CHECK(first.blendMode == m3::BlendMode::AlphaAdd);
    REQUIRE(first.diffuseLayer.has_value());
    CHECK(first.diffuseLayer->texturePath == "tex0.dds");
    // Section 1: keyed base + lit keyed decal + the unlit blend ahead of the
    // unlit additive in the emissive slots.
    const m3::StandardMaterial& second = written->standardMaterials[1];
    CHECK(second.blendMode == m3::BlendMode::Opaque);
    CHECK(second.alphaTestThreshold == 192);
    REQUIRE(second.diffuseLayer.has_value());
    CHECK(second.diffuseLayer->texturePath == "tex1.dds");
    REQUIRE(second.decalLayer.has_value());
    CHECK(second.decalLayer->texturePath == "tex2.dds");
    REQUIRE(second.emissiveLayer1.has_value());
    CHECK(second.emissiveLayer1->texturePath == "tex4.dds");
    CHECK(second.emissiveBlendMode1 == m3::LayerBlendOp::Lerp);
    REQUIRE(second.emissiveLayer2.has_value());
    CHECK(second.emissiveLayer2->texturePath == "tex3.dds");
    CHECK(second.emissiveBlendMode2 == m3::LayerBlendOp::Add);
    CHECK(written.diagnostics.countOf(DiagCode::CompositeEmitted) == 1);

    // Exact passes: every fold onto a keyed base is confined to its coverage,
    // so each pass is its own section -- five draws, as Warcraft III drew it.
    M3ExportSettings exact;
    exact.exactPasses = true;
    Result<m3::Model> split = converter.toM3(document, ProfileId::Sc2, 29, exact);
    REQUIRE(split.ok());
    REQUIRE(split->compositeMaterials.size() == 1);
    CHECK(split->compositeMaterials[0].sections.size() == 5);
}

TEST_CASE("wem m3 a composite of single-pass sections reads back as the stack",
          "[wem][convert][m3][fold]") {
    // The import inverse: an additive base under a keyed pass goes out as two
    // sections and comes back as two Color passes under the first's header.
    Material material = stack("jackal", {colorLayer(0, CompositeOp::Set),
                                         colorLayer(1, CompositeOp::AlphaKey)});
    material.MutableCommon().blend = BlendMode::AdditiveAlpha;
    Document document = warcraftDocument(std::move(material));
    const M3Converter converter;
    Result<m3::Model> written = converter.toM3(document, ProfileId::Sc2, 29);
    REQUIRE(written.ok());
    REQUIRE(written->compositeMaterials.size() == 1);
    REQUIRE(written->compositeMaterials[0].sections.size() == 2);

    Result<Document> back = converter.fromM3(*written, ProfileId::Sc2);
    REQUIRE(back.ok());
    const Model& model = back->models.front();
    const Material* imported = Resolve(model, 0, ProfileId::Sc2, 0);
    REQUIRE(imported != nullptr);
    const CompositeBody* body = imported->Common().composite();
    REQUIRE(body != nullptr);
    REQUIRE(body->layers.size() == 2);
    CHECK(body->layers[0].op == CompositeOp::Set);
    CHECK(body->layers[1].op == CompositeOp::AlphaKey);
    CHECK(imported->Common().blend == BlendMode::AdditiveAlpha);
}

TEST_CASE("wem m3 a static pass alpha rides a carrier the fade can share",
          "[wem][convert][m3][fold]") {
    // A blended base at 0.66: its own alpha is the coverage layer, and the
    // weight is a solid-colour carrier's multiply (retail multiplies rgba),
    // with the carrier's map alpha free for a geoset fade to ride.
    Material material = stack("banshee", {colorLayer(0, CompositeOp::Set, 0.66f)});
    material.MutableCommon().blend = BlendMode::AlphaBlend;
    Document document = warcraftDocument(std::move(material));
    const M3Converter converter;
    Result<m3::Model> written = converter.toM3(document, ProfileId::Sc2, 29);
    const m3::StandardMaterial& out = firstMaterial(written);
    CHECK(out.blendMode == m3::BlendMode::AlphaBlend);
    REQUIRE(out.alphaLayer1.has_value());
    CHECK(out.alphaLayer1->texturePath == "tex0.dds");
    CHECK(out.alphaLayer1->mapAlpha.initValue == Catch::Approx(1.0f));
    REQUIRE(out.alphaLayer2.has_value());
    CHECK(hasFlag(out.alphaLayer2->flags, m3::TextureLayerFlag::Color));
    CHECK(out.alphaLayer2->rgbMultiply.initValue == Catch::Approx(0.66f));

    // An opaque glow at half strength: Opaque never reads the material alpha,
    // so the weight rides the glow layer itself.
    Material glow = stack("glow", {colorLayer(2, CompositeOp::Set, 0.5f)});
    Document shine = warcraftDocument(std::move(glow), 2, 2);
    Result<m3::Model> lit = converter.toM3(shine, ProfileId::Sc2, 29);
    const m3::StandardMaterial& halo = firstMaterial(lit);
    CHECK(halo.blendMode == m3::BlendMode::Opaque);
    REQUIRE(halo.emissiveLayer1.has_value());
    CHECK(halo.emissiveLayer1->mapAlpha.initValue == Catch::Approx(0.5f));
    CHECK_FALSE(halo.alphaLayer1.has_value());
}

TEST_CASE("wem m3 a team plate under an opaque texture is dropped", "[wem][convert][m3][fold]") {
    // Two official materials: no texel ever reveals the plate. With the
    // texture's alpha class known the fold drops it and keeps the texture as
    // a plain opaque diffuse.
    Material material = stack("shipyard", {colorLayer(1, CompositeOp::Set),
                                           colorLayer(0, CompositeOp::AlphaBlend)});
    Document document = warcraftDocument(std::move(material), 1, 1);
    M3ExportSettings settings;
    settings.textureAlphaClasses.assign(document.textures.size(), 0);
    settings.textureAlphaClasses[0] = 1; // opaque
    const M3Converter converter;
    Result<m3::Model> written = converter.toM3(document, ProfileId::Sc2, 29, settings);
    const m3::StandardMaterial& out = firstMaterial(written);
    REQUIRE(out.diffuseLayer.has_value());
    CHECK(out.diffuseLayer->texturePath == "tex0.dds");
    CHECK(out.diffuseLayer->colorType == m3::ColorChannelSelect::RGB);
    CHECK(out.blendMode == m3::BlendMode::Opaque);
    CHECK(written.diagnostics.countOf(DiagCode::TeamPlateCovered) == 1);
}

TEST_CASE("wem m3 a sphere-mapped pass is the environment layer", "[wem][convert][m3][fold]") {
    // Applied after lighting and after the emissives, so an UNLIT sphere pass
    // is exact; its static weight sits on a solid mask on all four channels.
    CompositeLayer sheen = colorLayer(3, CompositeOp::AddAlpha, 0.5f);
    sheen.input.mapping = UVMappingMode::EnvSphere;
    Material material = stack("dump", {colorLayer(0, CompositeOp::Set), sheen});
    shade(material, 1, true);
    Document document = warcraftDocument(std::move(material));
    const M3Converter converter;
    Result<m3::Model> written = converter.toM3(document, ProfileId::Sc2, 29);
    const m3::StandardMaterial& out = firstMaterial(written);
    REQUIRE(out.environmentLayer.has_value());
    CHECK(out.environmentLayer->texturePath == "tex3.dds");
    CHECK(out.environmentLayer->uvMapping == m3::UVMappingMode::ReflectSphericalEnvio);
    CHECK(out.layerBlendMode == m3::LayerBlendOp::Add);
    CHECK(out.hdrEnvironmentConstant == Catch::Approx(1.0f));
    REQUIRE(out.environmentMaskLayer.has_value());
    CHECK(hasFlag(out.environmentMaskLayer->flags, m3::TextureLayerFlag::Color));
    CHECK(out.environmentMaskLayer->color.initValue.a == 128);
    CHECK(out.environmentMaskLayer->color.initValue.r == 128);
    CHECK(written.diagnostics.countOf(DiagCode::LitEnvFolded) == 0);
}

TEST_CASE("wem m3 a coverage layer restores the test a team stack dropped",
          "[wem][convert][m3][coverage][team]") {
    // The RGBA select spends the diffuse alpha on the team mask, so the fold
    // clears the test rather than cut the team regions out. An explicit
    // coverage layer -- the Reforged base colour, whose alpha is the cutout --
    // is what the engine tests by, and the header's own test stands again.
    Material material;
    material.name = "ghost";
    CompositeBody body;
    body.layers.push_back(colorLayer(1, CompositeOp::Set));
    body.layers.push_back(colorLayer(0, CompositeOp::AlphaBlend));
    CompositeLayer coverage;
    coverage.input = wemfix::makeInput(2);
    coverage.target = SurfaceChannel::Coverage;
    coverage.op = CompositeOp::Set;
    body.layers.push_back(coverage);
    material.InitCommon().body = std::move(body);
    const M3Converter converter;

    SECTION("keyed") {
        material.MutableCommon().blend = BlendMode::AlphaKey;
        material.MutableCommon().alphaTestThreshold = 0.75f;
        Document document = wc3Document(std::move(material), 1, 1);
        Result<m3::Model> written = converter.toM3(document, ProfileId::Sc2, 29);
        const m3::StandardMaterial& out = firstMaterial(written);
        REQUIRE(out.diffuseLayer.has_value());
        CHECK(out.diffuseLayer->colorType == m3::ColorChannelSelect::RGBA);
        CHECK(out.blendMode == m3::BlendMode::Opaque);
        CHECK(out.alphaTestThreshold == 192);
        REQUIRE(out.alphaLayer1.has_value());
        CHECK(out.alphaLayer1->texturePath == "tex2.dds");
        CHECK(out.alphaLayer1->colorType == m3::ColorChannelSelect::Alpha);
    }
    SECTION("blended") {
        material.MutableCommon().blend = BlendMode::AlphaBlend;
        Document document = wc3Document(std::move(material), 1, 1);
        Result<m3::Model> written = converter.toM3(document, ProfileId::Sc2, 29);
        const m3::StandardMaterial& out = firstMaterial(written);
        CHECK(out.blendMode == m3::BlendMode::AlphaBlend);
        CHECK(out.alphaTestThreshold == 0);
        REQUIRE(out.alphaLayer1.has_value());
        CHECK(out.alphaLayer1->texturePath == "tex2.dds");
    }
}

TEST_CASE("wem m3 a whole-material fresnel is a rim over the lit colour",
          "[wem][convert][m3][fresnel]") {
    // Reforged's overlay is `lerp(lit, tint, opacity * (1 - n.v)^2)`. Two solid
    // carriers spell it exactly: a Mod emissive1 dims the lit colour by `1 - w`
    // (its ramp runs from 1 down to 1 - opacity) and an alpha-free add in
    // emissive2 brings `tint * w`.
    Material material = wemfix::makeComposite("rim");
    FresnelFeature rim;
    rim.color = Vector3f{0.5f, 0.79f, 1.0f};
    rim.exponent = 2.0f;
    rim.outMin = 0.0f;
    rim.outMax = 0.85f;
    MaterialFeature feature;
    feature.id = 1;
    feature.layer = kWholeMaterial;
    feature.payload = rim;
    material.MutableCommon().features.push_back(feature);
    const M3Converter converter;

    SECTION("both emissive slots free") {
        Document document = wc3Document(std::move(material));
        Result<m3::Model> written = converter.toM3(document, ProfileId::Sc2, 29);
        const m3::StandardMaterial& out = firstMaterial(written);
        REQUIRE(out.emissiveLayer1.has_value());
        CHECK(hasFlag(out.emissiveLayer1->flags, m3::TextureLayerFlag::Color));
        CHECK(out.emissiveBlendMode1 == m3::LayerBlendOp::Mod);
        CHECK(out.emissiveLayer1->fresnelMode == m3::FresnelMode::Standard);
        CHECK(out.emissiveLayer1->fresnelExponent == Catch::Approx(2.0f));
        CHECK(out.emissiveLayer1->fresnelMin == Catch::Approx(1.0f));
        CHECK(out.emissiveLayer1->fresnelMax == Catch::Approx(0.15f));
        CHECK(out.emissiveLayer1->color.initValue.r == 255);
        REQUIRE(out.emissiveLayer2.has_value());
        CHECK(out.emissiveBlendMode2 == m3::LayerBlendOp::AddNoAlpha);
        CHECK(out.emissiveLayer2->fresnelMode == m3::FresnelMode::Standard);
        CHECK(out.emissiveLayer2->fresnelMin == Catch::Approx(0.0f));
        CHECK(out.emissiveLayer2->fresnelMax == Catch::Approx(0.85f));
        CHECK(out.emissiveLayer2->color.initValue.r == 128);
        CHECK(out.emissiveLayer2->color.initValue.g == 201);
        CHECK(out.emissiveLayer2->color.initValue.b == 255);
        CHECK_FALSE(out.decalLayer.has_value());
        CHECK(written.diagnostics.countOf(DiagCode::FresnelFolded) == 0);
    }
    SECTION("beside an emissive map the dim takes the decal") {
        CompositeBody* body = material.MutableCommon().composite();
        REQUIRE(body != nullptr);
        CompositeLayer glow;
        glow.input = wemfix::makeInput(3);
        glow.target = SurfaceChannel::Emissive;
        glow.op = CompositeOp::Add;
        body->layers.push_back(glow);
        body->emissiveFactor = Vector4f{2.0f, 0.0f, 0.0f, 1.0f};
        Document document = wc3Document(std::move(material));
        Result<m3::Model> written = converter.toM3(document, ProfileId::Sc2, 29);
        const m3::StandardMaterial& out = firstMaterial(written);
        REQUIRE(out.emissiveLayer1.has_value());
        CHECK(out.emissiveLayer1->texturePath == "tex3.dds");
        REQUIRE(out.decalLayer.has_value());
        CHECK(out.layerBlendMode == m3::LayerBlendOp::Mod);
        CHECK(out.decalLayer->fresnelMin == Catch::Approx(1.0f));
        CHECK(out.decalLayer->fresnelMax == Catch::Approx(0.15f));
        REQUIRE(out.emissiveLayer2.has_value());
        CHECK(out.emissiveBlendMode2 == m3::LayerBlendOp::AddNoAlpha);
        // The add slots' sum is scaled by the HDR multiplier of 2, so the tint
        // is written at half.
        CHECK(out.emissiveLayer2->color.initValue.r == 64);
        CHECK(out.emissiveLayer2->color.initValue.b == 128);
        CHECK(written.diagnostics.countOf(DiagCode::FresnelFolded) == 0);
    }
}

TEST_CASE("wem m3 a layer's own fresnel survives an edit", "[wem][convert][m3][fresnel]") {
    // Inverted runs `1 - term` through the ramp -- the Standard ramp with its
    // ends swapped -- so WEM keeps one spelling and the export writes Standard.
    m3::Model model = makeModel(29);
    m3::TextureLayer& diffuse = *model.standardMaterials[0].diffuseLayer;
    diffuse.fresnelMode = m3::FresnelMode::Inverted;
    diffuse.fresnelExponent = 3.0f;
    diffuse.fresnelMin = 0.2f;
    diffuse.fresnelMax = 0.9f;
    const M3Converter converter;
    Result<Document> imported = converter.fromM3(model, ProfileId::Sc2);
    REQUIRE(imported.ok());
    Material& mat = imported->models[0].profileSets[0].materials[0];
    const MaterialFeature* feature = mat.Common().feature(FeatureKind::Fresnel, 0);
    REQUIRE(feature != nullptr);
    REQUIRE(feature->fresnel() != nullptr);
    CHECK(feature->fresnel()->outMin == Catch::Approx(0.9f));
    CHECK(feature->fresnel()->outMax == Catch::Approx(0.2f));

    mat.MutableCommon(); // an edit: the fold answers, not the native block
    Result<m3::Model> written = converter.toM3(*imported, ProfileId::Sc2, 29);
    const m3::StandardMaterial& out = firstMaterial(written);
    REQUIRE(out.diffuseLayer.has_value());
    CHECK(out.diffuseLayer->fresnelMode == m3::FresnelMode::Standard);
    CHECK(out.diffuseLayer->fresnelExponent == Catch::Approx(3.0f));
    CHECK(out.diffuseLayer->fresnelMin == Catch::Approx(0.9f));
    CHECK(out.diffuseLayer->fresnelMax == Catch::Approx(0.2f));
}

TEST_CASE("wem m3 a built material states that its geometry is visible",
          "[wem][convert][m3][material]") {
    // `GeometryVisible` (0x80000000) is what puts a batch in the colour pass:
    // 35,492 of the corpus's 35,533 v20 materials set it, and the 41 that do
    // not are trigger volumes (`War3_FootSwitch.m3`) meant to be unseen.
    // Without it the Galaxy editor loads the model, casts its shadow, draws no
    // mesh, and complains about none of it -- every draw succeeds.
    const Document document = wemfix::makeDocument(ProfileId::Sc2);
    const M3Converter converter;
    const Result<m3::Model> written = converter.toM3(document, ProfileId::Sc2, 29);
    REQUIRE(written.ok());
    REQUIRE_FALSE(written->standardMaterials.empty());
    for (const m3::StandardMaterial& material : written->standardMaterials) {
        CHECK(hasFlag(material.flags, m3::MaterialFlag::GeometryVisible));
    }
    // It rides on the default, so no export path can be the one that forgets
    // -- and a material read back off a file keeps whatever that file said.
    CHECK(hasFlag(m3::StandardMaterial{}.flags, m3::MaterialFlag::GeometryVisible));
}

TEST_CASE("wem m3 a built layer is not a render target", "[wem][convert][m3][material]") {
    // `textureSource` is -1 on every one of the corpus's 23,988 layers, and the
    // editor reads it as `b_iIsRTTTexture = textureSource != -1`
    // (`sub_141F96390`). A render-target layer has its UVs remapped into the
    // target's sub-rect -- `uv * p_vRTTTextureOffsetScale.zw + .xy` -- and
    // nothing fills that constant for a layer that reads a file, so the whole
    // model samples uv (0,0): flat team colour where the diffuse selects RGBA,
    // flat black where it selects RGB. The same field is one of the four terms
    // that mark a layer ACTIVE, so a zero also switched on the empty slots.
    CHECK(m3::TextureLayer{}.textureSource == 0xFFFFFFFFu);

    const Document document = wemfix::makeDocument(ProfileId::Sc2);
    const M3Converter converter;
    const Result<m3::Model> written = converter.toM3(document, ProfileId::Sc2, 29);
    REQUIRE(written.ok());
    REQUIRE_FALSE(written->standardMaterials.empty());
    // Through the writer, because the eighteen slots a material does not use are
    // its `emptyLayer` and reach the file all the same.
    const m3::Model reparsed = m3::Parser().parse(m3::Writer().write(*written));
    REQUIRE_FALSE(reparsed.standardMaterials.empty());
    std::size_t layers = 0;
    for (const m3::StandardMaterial& material : reparsed.standardMaterials) {
        for (const auto slot : wemfix::kLayerSlots) {
            REQUIRE((material.*slot).has_value());
            CHECK((material.*slot)->textureSource == 0xFFFFFFFFu);
            ++layers;
        }
    }
    CHECK(layers > 0);
}

TEST_CASE("wem m3 a textured layer states the pair its clamp needs",
          "[wem][convert][m3][material]") {
    // All 7,726 shipped layers that name a texture set `ColorAdd |
    // ColorMultiply`, and the editor makes the pair the precondition for the
    // clamp beside it: `b_iClamp = (flags & 0xC0) && (flags & ColorClamp)`.
    // Without it the coverage switch's `a * 1 + rgbAdd` never saturates. A
    // constant-colour carrier is the other shape and ships as plain `Color`
    // (442 of 446), so the pair belongs to layers with a path, not to all.
    const Document document = wemfix::makeDocument(ProfileId::Sc2);
    const M3Converter converter;
    const Result<m3::Model> written = converter.toM3(document, ProfileId::Sc2, 29);
    REQUIRE(written.ok());
    std::size_t textured = 0;
    for (const m3::StandardMaterial& material : written->standardMaterials) {
        for (const auto slot : wemfix::kLayerSlots) {
            const std::optional<m3::TextureLayer>& layer = material.*slot;
            if (!layer.has_value() || layer->texturePath.empty()) {
                continue;
            }
            ++textured;
            CHECK(hasFlag(layer->flags, m3::TextureLayerFlag::ColorAdd));
            CHECK(hasFlag(layer->flags, m3::TextureLayerFlag::ColorMultiply));
        }
    }
    CHECK(textured > 0);
}

TEST_CASE("wem m3 a blend op nobody authored adds rather than multiplies",
          "[wem][convert][m3][material]") {
    // Over 7,519 shipped materials the decal blend is Add on 97.4%, emissive 1
    // on 96.8% (Add or its team-colour form) and emissive 2 on 98.5% -- Mod is
    // what an unassigned field reads as, and it is the destructive one: a Mod
    // emissive MULTIPLIES the lit colour, and an unused emissive slot is black.
    const m3::StandardMaterial fresh;
    CHECK(fresh.layerBlendMode == m3::LayerBlendOp::Add);
    CHECK(fresh.emissiveBlendMode1 == m3::LayerBlendOp::Add);
    CHECK(fresh.emissiveBlendMode2 == m3::LayerBlendOp::Add);

    const Document document = wemfix::makeDocument(ProfileId::Sc2);
    const M3Converter converter;
    const Result<m3::Model> written = converter.toM3(document, ProfileId::Sc2, 29);
    REQUIRE(written.ok());
    REQUIRE_FALSE(written->standardMaterials.empty());
    for (const m3::StandardMaterial& material : written->standardMaterials) {
        CHECK(material.emissiveBlendMode1 != m3::LayerBlendOp::Mod);
        CHECK(material.emissiveBlendMode2 != m3::LayerBlendOp::Mod);
    }
}

// ---------------------------------------------------------------------------
// The skin fields a shipped region states.
// ---------------------------------------------------------------------------

namespace {

/// The fixture quad blended between its two bones, one vertex per shape: an
/// even split, a lone influence parked in slot 1, an uneven split heavier in
/// slot 1, and a lone influence in slot 0. The import orders each vertex
/// heaviest first (`SkinBinding`), so that is the order the file comes back in.
m3::Model makeBlendedModel() {
    m3::Model model = makeModel(29);
    model.boneLookup = {0, 1};
    model.divisions[0].regions[0].boneLookupCount = 2;
    const u8 weights[4][4] = {{128, 128, 0, 0}, {0, 255, 0, 0}, {100, 155, 0, 0}, {255, 0, 0, 0}};
    for (std::size_t v = 0; v < 4; ++v) {
        const std::size_t base = v * kStride;
        for (std::size_t k = 0; k < 4; ++k) {
            model.vertices.data[base + 12 + k] = weights[v][k];
            model.vertices.data[base + 16 + k] = static_cast<u8>(k < 2 ? k : 0);
        }
    }
    model.vertices.initialize();
    return model;
}

} // namespace

TEST_CASE("wem m3 a vertex stores weights summing to exactly 255",
          "[wem][convert][m3][skin]") {
    // 1,002,734 shipped vertices sum to exactly 255. Rounding each weight alone
    // wrote an even two-bone split as 128 + 128.
    const M3Converter converter;
    Result<Document> doc = converter.fromM3(makeBlendedModel());
    REQUIRE(doc.ok());
    Result<m3::Model> back = converter.toM3(*doc.value, ProfileId::Sc2, 29);
    REQUIRE(back.ok());
    REQUIRE(back->divisions.size() == 1);
    REQUIRE(back->divisions[0].regions.size() == 1);
    const m3::Region& region = back->divisions[0].regions[0];
    REQUIRE(region.vertexCount == 4u);

    const std::vector<Vector3f> positions = back->vertices.getPositions();
    const std::vector<std::array<u8, 4>> indices = back->vertices.getBoneIndices();
    const std::vector<std::array<u8, 4>> weights = back->vertices.getBoneWeights();
    for (u32 v = 0; v < region.vertexCount; ++v) {
        const std::size_t g = region.firstVertex + v;
        const std::array<u8, 4>& w = weights[g];
        CHECK(w[0] + w[1] + w[2] + w[3] == 255);
        CHECK((w[0] >= w[1] && w[1] >= w[2] && w[2] >= w[3]));
        const u32 heaviest = back->boneLookup[region.firstBoneLookup + indices[g][0]];
        const Vector3f& p = positions[g];
        if (p.x < 0.5f && p.y < 0.5f) { // the even split
            CHECK(w[0] == 128);
            CHECK(w[1] == 127);
        } else if (p.x > 0.5f && p.y < 0.5f) { // the lone influence from slot 1
            CHECK(w[0] == 255);
            CHECK(heaviest == 1u);
        } else if (p.x > 0.5f) { // the split heavier in slot 1
            CHECK(w[0] == 155);
            CHECK(w[1] == 100);
            CHECK(heaviest == 1u);
        } else {
            CHECK(w[0] == 255);
            CHECK(heaviest == 0u);
        }
    }
    CHECK(region.boneWeightPairs == 4);
    CHECK(region.boneIndexPairs == 4);
    CHECK(region.unknown2 == region.boneLookupCount);
}

TEST_CASE("wem m3 a region no vertex blends states one weight pair",
          "[wem][convert][m3][skin]") {
    // All 730 shipped regions whose vertices each follow one bone say 1, and
    // 874 of 874 repeat their lookup count at +24.
    const M3Converter converter;
    Result<Document> doc = converter.fromM3(makeTwoRegionModel());
    REQUIRE(doc.ok());
    Result<m3::Model> back = converter.toM3(*doc.value, ProfileId::Heroes, 30);
    REQUIRE(back.ok());
    REQUIRE(back->divisions.size() == 1);
    REQUIRE(back->divisions[0].regions.size() == 2);
    for (const m3::Region& region : back->divisions[0].regions) {
        CHECK(region.boneWeightPairs == 1);
        // 829 of 840 shipped regions state the index pairs equal to the weight
        // pairs; a 1 / 4 split is on eleven.
        CHECK(region.boneIndexPairs == 1);
        CHECK(region.unknown2 == region.boneLookupCount);
    }
}

TEST_CASE("wem m3 a region names its first lookup bone as its root",
          "[wem][convert][m3][skin]") {
    // 840 of 840 shipped regions, rigid and blended alike. A Warcraft III
    // source has no region record to carry one, and every region reached the
    // engine naming bone 0.
    const M3Converter converter;

    SECTION("a source without region records") {
        Result<Document> doc = converter.fromM3(makeTwoRegionModel());
        REQUIRE(doc.ok());
        for (Mesh& mesh : doc->models.front().meshes) {
            for (MeshSection& section : mesh.sections) {
                section.native = SectionNative{};
            }
        }
        Result<m3::Model> back = converter.toM3(*doc.value, ProfileId::Heroes, 30);
        REQUIRE(back.ok());
        REQUIRE(back->divisions[0].regions.size() == 2);
        for (const m3::Region& region : back->divisions[0].regions) {
            REQUIRE(region.boneLookupCount >= 1);
            CHECK(region.rootBone == back->boneLookup[region.firstBoneLookup]);
        }
        CHECK(back->divisions[0].regions[0].rootBone != back->divisions[0].regions[1].rootBone);
    }

    SECTION("a native record keeps the root it carried") {
        m3::Model model = makeTwoRegionModel();
        model.divisions[0].regions[0].rootBone = 2;
        model.divisions[0].regions[1].rootBone = 1;
        Result<Document> doc = converter.fromM3(model);
        REQUIRE(doc.ok());
        Result<m3::Model> back = converter.toM3(*doc.value, ProfileId::Heroes, 30);
        REQUIRE(back.ok());
        REQUIRE(back->divisions[0].regions.size() == 2);
        CHECK(back->divisions[0].regions[0].rootBone == 2);
        CHECK(back->divisions[0].regions[1].rootBone == 1);
    }
}

TEST_CASE("wem m3 an unfogged material states the editor's unfogged bit",
          "[wem][convert][m3][material]") {
    // The Galaxy editor's flag table names 0x2000 Unfogged. 0x4 is a normal blend
    // from v19, whose factors the draw reads without checking the count, so the
    // Skink High Priest's and Spirit of Vengeance's unfogged glows, written as
    // 0x4, crashed the editor on an empty factor array.
    CHECK(static_cast<u32>(m3::MaterialFlag::Unfogged) == 0x2000u);
    CHECK(static_cast<u32>(m3::MaterialFlag::NormalBlend) == 0x4u);

    Document document = wemfix::makeDocument(ProfileId::Sc2);
    for (Material& material : document.models[0].profileSets[0].materials) {
        material.InitCommon().flags |= MaterialFlags::Unfogged;
    }
    const Result<m3::Model> written = M3Converter().toM3(document, ProfileId::Sc2, 29);
    REQUIRE(written.ok());
    REQUIRE_FALSE(written->standardMaterials.empty());
    for (const m3::StandardMaterial& material : written->standardMaterials) {
        CHECK(hasFlag(material.flags, m3::MaterialFlag::Unfogged));
        CHECK_FALSE(hasFlag(material.flags, m3::MaterialFlag::NormalBlend));
    }
}

TEST_CASE("wem m3 a normal blend is written only with the factors it reads",
          "[wem][convert][m3][material]") {
    // A record restored from a pre-v19 source keeps that version's 0x4, and the
    // export stamps it v20, where the bit is a normal blend.
    const auto exportedFlags = [](std::size_t factors) {
        m3::Model source = makeModel(29);
        m3::StandardMaterial& material = source.standardMaterials.front();
        material.flags |= m3::MaterialFlag::NormalBlend | m3::MaterialFlag::NormalBlend2;
        material.normalBlendFactors.resize(factors);
        const M3Converter converter;
        Result<Document> document = converter.fromM3(source, ProfileId::Sc2);
        REQUIRE(document.ok());
        REQUIRE(document->models[0].profileSets[0].materials.front().NativeIsAuthoritative());
        Result<m3::Model> written = converter.toM3(*document, ProfileId::Sc2, 29);
        REQUIRE(written.ok());
        REQUIRE(written->standardMaterials.size() == 1);
        return written->standardMaterials.front().flags;
    };
    CHECK_FALSE(hasFlag(exportedFlags(0), m3::MaterialFlag::NormalBlend));
    CHECK_FALSE(hasFlag(exportedFlags(0), m3::MaterialFlag::NormalBlend2));
    CHECK(hasFlag(exportedFlags(4), m3::MaterialFlag::NormalBlend));
    CHECK_FALSE(hasFlag(exportedFlags(4), m3::MaterialFlag::NormalBlend2));
    CHECK(hasFlag(exportedFlags(8), m3::MaterialFlag::NormalBlend2));
}
