// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// WEM v3 P5 — `M3Converter` on hand-built models.
///
/// Four claims a green parse cannot check: the profile follows the `MODL`
/// version, the SC2 -> Blizzard rebase is the exact one the design specifies
/// (and is bit-exact both ways), a region's faces are region-local, and a
/// vertex's bone index goes through its region's `boneLookup` window.

#include <cstring>

#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/wem/converters.h>

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
    root.flags = m3::BoneFlag::InheritTranslation | m3::BoneFlag::InheritScale |
                 m3::BoneFlag::InheritRotation;
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
