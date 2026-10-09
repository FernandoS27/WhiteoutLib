// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// FBX_OBJ_DESIGN O1/O2 — the OBJ/MTL codec and `ObjConverter`.
///
/// What only this crossing promises: a quad stays a quad both ways, the basis
/// is a permutation (bit-exact, no arithmetic), V flips at the boundary, the
/// writer is byte-stable and locale-free, and a hostile file is refused or
/// trimmed with warnings — never trusted.

#include <clocale>
#include <cmath>
#include <cstring>
#include <string>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/obj/parser.h>
#include <whiteout/models/obj/writer.h>
#include <whiteout/models/wem/geometry/builder.h>
#include <whiteout/models/wem/geometry/primitives.h>
#include <whiteout/models/wem/obj_converter.h>

using namespace whiteout;
using namespace whiteout::models;
using namespace whiteout::models::wem;

namespace {

const char* kCubeQuads = R"(# a test
mtllib My Model.mtl
o Box
v 0 0 0
v 1 0 0
v 1 1 0
v 0 1 0
vt 0 0
vt 1 0
vt 1 1
vt 0 1
vn 0 0 1
g front
usemtl red
s 1
f 1/1/1 2/2/1 3/3/1 4/4/1
g back
usemtl blue
s off
f -1/-1/-1 -2/-2/-1 \
  -3/-3/-1
l 1 2
)";

obj::Asset parse(const char* text) {
    obj::ParseOutcome outcome = obj::Parser::FromText(text);
    REQUIRE(outcome.ok());
    return std::move(*outcome.asset);
}

/// One quad in WEM's own space, a red Legacy material with a texture, and a UV
/// that is not symmetric under a V flip.
Document makeQuadDocument() {
    Document document;
    document.name = "quad";
    document.declare(ProfileId::Generic);
    Model model;
    model.name = "quad";
    model.materialSlots.push_back("paint");

    geom::MeshBuilder builder;
    const geom::VertexId a = builder.addVertex(Vector3f{0, 0, 0});
    const geom::VertexId b = builder.addVertex(Vector3f{1, 0, 0});
    const geom::VertexId c = builder.addVertex(Vector3f{1, 2, 0});
    const geom::VertexId d = builder.addVertex(Vector3f{0, 2, 0.5f});
    MeshSection section;
    section.name = "body";
    section.materialSlot = 0;
    builder.addSection(section);
    const geom::VertexId corners[4] = {a, b, c, d};
    const geom::FaceId face = builder.addFace(corners, 0);
    const Vector2f uvs[4] = {{0.0f, 0.25f}, {1.0f, 0.25f}, {1.0f, 0.875f}, {0.0f, 0.875f}};
    for (u32 k = 0; k < 4; ++k) {
        builder.setCornerAttr(face, k, geom::names::kNormal, Vector3f{0, 0, 1});
        builder.setCornerAttr(face, k, geom::names::uv(0), uvs[k]);
    }
    geom::MeshBuilder::BuildOutcome outcome = builder.build();
    outcome.mesh.name = "panel";
    model.meshes.push_back(std::move(outcome.mesh));

    TextureRef paint;
    paint.key = TexturePath{"textures/Paint Coat.blp"};
    paint.path = "textures/Paint Coat.blp";
    document.textures.push_back(paint);

    ProfileMaterialSet set;
    set.profile = ProfileId::Generic;
    set.looks = LookTable::Single();
    Material material;
    material.name = "paint";
    CommonMaterial& common = material.InitCommon();
    common.setKind(MaterialKind::LegacyDeferred);
    LegacyDeferredBody& body = *common.legacy();
    body.diffuseFactor = Vector4f{1.0f, 0.0f, 0.0f, 1.0f};
    body.specularFactor = Vector4f{0.5f, 0.5f, 0.5f, 1.0f};
    body.specularExponent = 32.0f;
    TextureInput diffuse;
    diffuse.texture = 0;
    body.set(LegacySlot::Diffuse, diffuse);
    set.materials.push_back(std::move(material));
    set.resizeBindings(model.materialSlots.size());
    set.slotBindings[0].byLook[0] = 0;
    model.profileSets.push_back(std::move(set));
    document.models.push_back(std::move(model));
    return document;
}

u32 valenceOfFirstFace(const Mesh& mesh) {
    return mesh.faceSet().faceValence.empty() ? 0u : mesh.faceSet().faceValence[0];
}

} // namespace

TEST_CASE("obj parser reads pools, groups and negative indices", "[obj]") {
    const obj::Asset asset = parse(kCubeQuads);
    CHECK(asset.positions.size() == 4);
    CHECK(asset.uvs.size() == 4);
    CHECK(asset.normals.size() == 1);
    REQUIRE(asset.faces.size() == 2);
    CHECK(asset.faces[0].cornerCount == 4);
    CHECK(asset.faces[1].cornerCount == 3); // The `\` continuation joined it.
    CHECK(asset.corners[asset.faces[1].firstCorner].position == 3); // -1 is the last.
    CHECK(asset.faces[0].smoothing == 1);
    CHECK(asset.faces[1].smoothing == 0);
    CHECK(asset.smoothingStated);
    REQUIRE(asset.materialLibraries.size() == 1);
    CHECK(asset.materialLibraries[0] == "My Model.mtl"); // A name with a space.
    REQUIRE(asset.materials.size() == 2);
    CHECK(asset.groups[asset.faces[0].group].object == "Box");
    CHECK(asset.groups[asset.faces[1].group].group == "back");
    CHECK(asset.skippedElements == 1); // The line.
}

TEST_CASE("obj parser keeps the vertex-colour extension", "[obj]") {
    const obj::Asset asset = parse("v 0 0 0\nv 1 0 0 1 0.5 0\nv 0 1 0\nf 1 2 3\n");
    REQUIRE(asset.colors.size() == 3);
    CHECK(asset.colors[0].x == 1.0f); // Uncoloured vertices read white.
    CHECK(asset.colors[1].y == 0.5f);
}

TEST_CASE("obj parser refuses or trims hostile input", "[obj]") {
    const std::string binary("v 0 0 0\0garbage", 15);
    CHECK_FALSE(obj::Parser::FromText(binary).ok());

    const obj::ParseOutcome outcome =
        obj::Parser::FromText("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 4\nf 0 1 2\nf 1 2\nf 1 2 3\n"
                              "f -9999999999 1 2\nv nan 0 0\n");
    REQUIRE(outcome.ok());
    CHECK(outcome.asset->faces.size() == 1); // Only the well-formed face.
    CHECK(outcome.warnings.size() >= 4);
}

TEST_CASE("mtl parser reads factors, maps and their options", "[obj]") {
    const obj::MtlParseOutcome outcome = obj::Parser::MaterialsFromText(R"(newmtl stone
Kd 0.5
Ks 1 1 1
Ns 64
Tr 0.25
map_Kd -s 2 2 -o 0.5 0 -clamp on textures/stone wall.png
map_Bump -bm 0.5 stone_n.png
newmtl metal
Pr 0.3
Pm 1
map_Pr rough.png
)");
    REQUIRE(outcome.library.materials.size() == 2);
    const obj::Material& stone = outcome.library.materials[0];
    CHECK(stone.diffuse->y == 0.5f); // One value is a grey.
    CHECK(stone.dissolve.value() == Catch::Approx(0.75f));
    CHECK(stone.diffuseMap.file == "textures/stone wall.png");
    CHECK(stone.diffuseMap.scale.x == 2.0f);
    CHECK(stone.diffuseMap.offset.x == 0.5f);
    CHECK(stone.diffuseMap.clamp);
    CHECK(stone.bumpMap.bumpMultiplier == 0.5f);
    CHECK_FALSE(stone.isPbr());
    CHECK(outcome.library.materials[1].isPbr());
}

TEST_CASE("obj writer is byte-stable, locale-free and reads back exactly", "[obj]") {
    obj::Asset asset = parse(kCubeQuads);
    asset.positions[1] = Vector3f{0.1f, 1e-7f, -123456.789f};
    const std::string first = obj::Writer::ToText(asset, "header");
    CHECK(first == obj::Writer::ToText(asset, "header"));

    // A decimal-comma locale must not leak into the file.
    const std::string saved = std::setlocale(LC_NUMERIC, nullptr);
    const bool german = std::setlocale(LC_NUMERIC, "de_DE.UTF-8") != nullptr ||
                        std::setlocale(LC_NUMERIC, "German_Germany.1252") != nullptr ||
                        std::setlocale(LC_NUMERIC, "de-DE") != nullptr;
    const std::string localized = obj::Writer::ToText(asset, "header");
    const obj::ParseOutcome reread = obj::Parser::FromText(localized);
    std::setlocale(LC_NUMERIC, saved.c_str());
    if (german) {
        CHECK(localized == first);
    }
    REQUIRE(reread.ok());
    for (std::size_t i = 0; i < asset.positions.size(); ++i) {
        CHECK(std::memcmp(&reread.asset->positions[i], &asset.positions[i], sizeof(Vector3f)) ==
              0);
    }
    CHECK(reread.asset->faces.size() == asset.faces.size());
}

TEST_CASE("obj import keeps polygons and permutes the basis exactly", "[obj][wem]") {
    const obj::Asset asset = parse("v 1 2 3\nv 4 5 6\nv 7 8 10\nv 1 9 2\n"
                                   "vt 0 0\nvt 1 0\nvt 1 0.75\nvt 0 1\n"
                                   "f 1/1 2/2 3/3 4/4\n");
    const ObjConverter converter;
    Result<Document> imported = converter.fromObj(asset, nullptr);
    REQUIRE(imported.ok());
    CHECK(imported->profiles == std::vector<ProfileId>{ProfileId::Generic});
    REQUIRE(imported->models.size() == 1);
    REQUIRE(imported->models[0].meshes.size() == 1);
    const Mesh& mesh = imported->models[0].meshes[0];
    CHECK(mesh.faceCount() == 1);
    CHECK(valenceOfFirstFace(mesh) == 4); // Still a quad.

    // Y-up facing +Z: file (x, y, z) is WEM (z, x, y).
    const auto positions = mesh.attributes.get<Vector3f>(geom::names::kPosition,
                                                         geom::Domain::Vertex);
    REQUIRE(positions.size() == 4);
    CHECK(positions[0].x == 3.0f);
    CHECK(positions[0].y == 1.0f);
    CHECK(positions[0].z == 2.0f);

    // V flips: 0.75 in the file is 0.25 in WEM.
    const auto uvs = mesh.attributes.get<Vector2f>(geom::names::uv(0), geom::Domain::Halfedge);
    bool sawFlip = false;
    for (const Vector2f& uv : uvs) {
        sawFlip = sawFlip || (uv.x == 1.0f && uv.y == 0.25f);
    }
    CHECK(sawFlip);
    // No `vn`, no `s`: normals come from the default shading angle.
    CHECK(mesh.attributes.has(geom::names::kNormal, geom::Domain::Halfedge));
}

TEST_CASE("obj smoothing groups decide shared and faceted normals", "[obj][wem]") {
    // Two triangles folded 90 degrees along a shared edge: smoothed in one
    // group they share a normal at the hinge; `s off` keeps them faceted.
    const char* folded = "v 0 0 0\nv 1 0 0\nv 0 1 0\nv 0 0 1\n";
    const ObjConverter converter;
    for (const char* smoothing : {"s 1", "s off"}) {
        const std::string text = std::string(folded) + smoothing + "\nf 1 2 3\nf 2 1 4\n";
        Result<Document> imported = converter.fromObj(parse(text.c_str()), nullptr);
        REQUIRE(imported.ok());
        const Mesh& mesh = imported->models[0].meshes[0];
        const auto normals =
            mesh.attributes.get<Vector3f>(geom::names::kNormal, geom::Domain::Halfedge);
        const auto same = [](const Vector3f& a, const Vector3f& b) {
            return std::fabs(a.x - b.x) < 1e-5f && std::fabs(a.y - b.y) < 1e-5f &&
                   std::fabs(a.z - b.z) < 1e-5f;
        };
        u32 distinct = 0;
        for (std::size_t i = 0; i < normals.size(); ++i) {
            // Boundary halfedges belong to no face and hold no normal.
            if (same(normals[i], Vector3f{0, 0, 0})) {
                continue;
            }
            bool seen = false;
            for (std::size_t j = 0; j < i; ++j) {
                seen = seen || same(normals[i], normals[j]);
            }
            distinct += seen ? 0u : 1u;
        }
        INFO(smoothing);
        if (std::string(smoothing) == "s 1") {
            CHECK(distinct > 2); // Corners at the hinge average; the far ones do not.
        } else {
            CHECK(distinct == 2); // One per face.
        }
    }
}

TEST_CASE("mtl crosses to Legacy and PBR bodies", "[obj][wem]") {
    const obj::Asset asset = parse("mtllib m.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nv 1 1 0\nv 2 0 0\n"
                                   "usemtl phong\nf 1 2 3\nusemtl pbr\nf 2 4 3\n"
                                   "usemtl missing\nf 2 5 4\n");
    const obj::MtlParseOutcome library = obj::Parser::MaterialsFromText(
        "newmtl phong\nKd 1 0 0\nKs 0.5 0.5 0.5\nNs 64\nmap_Kd paint.png\n"
        "newmtl pbr\nKd 0 1 0\nPr 0.25\nmap_Pm metal.png\n");
    ObjReadOptions options;
    options.textureDirectory = "materials";
    const ObjConverter converter;
    Result<Document> imported = converter.fromObj(asset, &library.library, options);
    REQUIRE(imported.ok());
    const Model& model = imported->models[0];
    REQUIRE(model.materialSlots.size() == 3);
    const Material* phong = Resolve(model, 0, ProfileId::Generic, 0);
    REQUIRE(phong != nullptr);
    REQUIRE(phong->Common().legacy() != nullptr);
    CHECK(phong->Common().legacy()->specularExponent == 64.0f);
    REQUIRE(phong->Common().legacy()->find(LegacySlot::Diffuse) != nullptr);
    const Material* pbr = Resolve(model, 1, ProfileId::Generic, 0);
    REQUIRE(pbr != nullptr);
    REQUIRE(pbr->Common().pbr() != nullptr);
    CHECK(pbr->Common().pbr()->roughnessFactor == 0.25f);
    CHECK(pbr->Common().pbr()->metallicFactor == 1.0f); // A map with no factor is the map.
    // Texture paths are relative to the .obj, through the library's folder.
    bool sawPrefixed = false;
    for (const TextureRef& ref : imported->textures) {
        sawPrefixed = sawPrefixed || ref.path == "materials/paint.png";
    }
    CHECK(sawPrefixed);
    CHECK(imported.diagnostics.byCode(DiagCode::SlotNotBound).size() == 1);
}

TEST_CASE("obj export numbers smoothing groups from the hard edges", "[obj][wem]") {
    // A mesh with no `smoothGroup` layer but with hard edges: a group is what
    // they enclose (EDIT_MODE_NORMALS_DESIGN.md §5.5). A cylinder's side is
    // one, and each cap, a polygon alone, is `s off`.
    Document source = makeQuadDocument();
    Mesh can = geom::MakeCylinder(geom::PrimitiveParams{});
    can.sections[0] = source.models[0].meshes[0].sections[0];
    source.models[0].meshes[0] = can;
    Result<ObjExport> exported = ObjConverter{}.toObj(source, ProfileId::Generic);
    REQUIRE(exported.ok());
    REQUIRE(exported->asset.faces.size() == 14);
    u32 off = 0;
    u32 side = 0;
    for (const obj::Face& face : exported->asset.faces) {
        off += face.smoothing == 0 ? 1 : 0;
        if (face.smoothing != 0) {
            CHECK((side == 0 || face.smoothing == side));
            side = face.smoothing;
        }
    }
    CHECK(off == 2);
    CHECK(side != 0);
}

TEST_CASE("obj export round-trips polygons, UVs and materials", "[obj][wem]") {
    const Document source = makeQuadDocument();
    const ObjConverter converter;
    ObjWriteOptions options;
    options.materialLibrary = "quad.mtl";
    Result<ObjExport> exported = converter.toObj(source, ProfileId::Generic, options);
    REQUIRE(exported.ok());
    REQUIRE(exported->asset.faces.size() == 1);
    CHECK(exported->asset.faces[0].cornerCount == 4);
    REQUIRE(exported->materials.materials.size() == 1);
    const obj::Material& paint = exported->materials.materials[0];
    CHECK(paint.diffuse->x == 1.0f);
    CHECK(paint.exponent.value() == 32.0f);
    CHECK(paint.diffuseMap.file == "Paint_Coat.png"); // Portable name, PNG.
    REQUIRE(exported->images.size() == 1);
    CHECK(exported->images[0].texture == 0);

    const std::string text = obj::Writer::ToText(exported->asset);
    CHECK(text == obj::Writer::ToText(exported->asset)); // Byte-stable.
    const obj::MtlParseOutcome library =
        obj::Parser::MaterialsFromText(obj::Writer::MaterialsToText(exported->materials));
    Result<Document> back = converter.fromObj(parse(text.c_str()), &library.library);
    REQUIRE(back.ok());
    const Mesh& mesh = back->models[0].meshes[0];
    CHECK(mesh.faceCount() == 1);
    CHECK(valenceOfFirstFace(mesh) == 4);

    // Positions come back bit-exact through the permutation, both ways.
    const auto original = source.models[0].meshes[0].attributes.get<Vector3f>(
        geom::names::kPosition, geom::Domain::Vertex);
    const auto returned =
        mesh.attributes.get<Vector3f>(geom::names::kPosition, geom::Domain::Vertex);
    REQUIRE(original.size() == returned.size());
    for (const Vector3f& p : original) {
        bool found = false;
        for (const Vector3f& q : returned) {
            found = found || std::memcmp(&p, &q, sizeof(Vector3f)) == 0;
        }
        CHECK(found);
    }
    // And the UV that is not symmetric under the flip.
    const auto uvs = mesh.attributes.get<Vector2f>(geom::names::uv(0), geom::Domain::Halfedge);
    bool sawOriginal = false;
    for (const Vector2f& uv : uvs) {
        sawOriginal = sawOriginal || (uv.x == 1.0f && uv.y == 0.875f);
    }
    CHECK(sawOriginal);
    const Material* material = Resolve(back->models[0], 0, ProfileId::Generic, 0);
    REQUIRE(material != nullptr);
    REQUIRE(material->Common().legacy() != nullptr);
    CHECK(material->Common().legacy()->specularExponent == 32.0f);
}

TEST_CASE("obj export can cut the faces the model draws", "[obj][wem]") {
    const Document source = makeQuadDocument();
    ObjWriteOptions options;
    options.triangulate = true;
    Result<ObjExport> exported = ObjConverter{}.toObj(source, ProfileId::Generic, options);
    REQUIRE(exported.ok());
    CHECK(exported->asset.faces.size() == 2);
    for (const obj::Face& face : exported->asset.faces) {
        CHECK(face.cornerCount == 3);
    }
}

TEST_CASE("obj export refuses a profile the document does not carry", "[obj][wem]") {
    const Document source = makeQuadDocument();
    Result<ObjExport> exported = ObjConverter{}.toObj(source, ProfileId::Wow);
    CHECK_FALSE(exported.ok());
    CHECK(exported.diagnostics.byCode(DiagCode::ProfileNotCarried).size() == 1);
}

TEST_CASE("the axis presets are exact signed permutations", "[obj][wem]") {
    const Vector3f v{1.5f, -2.25f, 3.125f};
    for (const AxisPreset preset : {AxisPreset::YUp, AxisPreset::ZUpMax, AxisPreset::Native}) {
        const AxisBasis basis = AxisBasis::FromPreset(preset);
        CHECK(basis.determinant() == 1.0f);
        const Vector3f back = basis.fromFile(basis.toFile(v));
        CHECK(std::memcmp(&back, &v, sizeof(Vector3f)) == 0);
    }
    // ZUpMax faces the model down −Y, its left on +X (`CoordSpace::Max`).
    const AxisBasis max = AxisBasis::FromPreset(AxisPreset::ZUpMax);
    const Vector3f forward = max.toFile(Vector3f{1, 0, 0});
    CHECK(forward.y == -1.0f);
    const Vector3f left = max.toFile(Vector3f{0, 1, 0});
    CHECK(left.x == 1.0f);
    // The declaration Max writes describes the same basis.
    const std::optional<AxisBasis> declared = AxisBasis::FromDeclaration(2, 1, 1, -1, 0, 1);
    REQUIRE(declared.has_value());
    CHECK(declared->index == max.index);
    CHECK(declared->sign == max.sign);
}
