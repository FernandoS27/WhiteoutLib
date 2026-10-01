// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// FBX_OBJ_DESIGN S1, S5, S7 — the pieces every interchange exporter shares.
///
/// S1's byte-identity gate (every corpus `.glb` unchanged) proves the glTF
/// lowering kept its answers; these arms pin what the flat surface itself says,
/// that a derived Phong surface is no metal, and that a posed normal stays
/// perpendicular to its posed surface under any linear map.

#include <cmath>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/wem/geometry/builder.h>
#include <whiteout/models/wem/materials/surface_flatten.h>
#include <whiteout/models/wem/retarget.h>
#include <whiteout/models/wem/skinning/deform.h>
#include <whiteout/textures/pbr_bake.h>

#include "wem_material_fixture.h"

using namespace whiteout;
using namespace whiteout::models;
using namespace whiteout::models::wem;
using namespace wemfix;

namespace {

Document textured(u32 textures) {
    Document document;
    for (u32 i = 0; i < textures; ++i) {
        TextureRef ref;
        ref.key = TexturePath{"t" + std::to_string(i) + ".png"};
        ref.path = "t" + std::to_string(i) + ".png";
        document.textures.push_back(ref);
    }
    return document;
}

Vector3f cross(const Vector3f& a, const Vector3f& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

Vector3f normalized(const Vector3f& v) {
    const f32 length = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    return v * (1.0f / length);
}

} // namespace

TEST_CASE("a legacy surface keeps its Phong roles", "[wem][interchange]") {
    const Document document = textured(3);
    Material material;
    CommonMaterial& common = material.InitCommon();
    common.setKind(MaterialKind::LegacyDeferred);
    LegacyDeferredBody& body = *common.legacy();
    body.specularFactor = Vector4f{0.25f, 0.5f, 0.75f, 1.0f};
    body.specularExponent = 20.0f;
    body.set(LegacySlot::Diffuse, makeInput(0));
    body.set(LegacySlot::Specular, makeInput(1));
    body.set(LegacySlot::Lightmap, makeInput(2));

    const FlatSurface surface = FlattenSurface(document, material);
    CHECK(surface.hasSpecular);
    CHECK(surface.specularFactor.y == 0.5f);
    CHECK(surface.metallicFactor == 0.0f);
    CHECK(surface.roughnessFactor == Catch::Approx(textures::pbr::RoughnessFromExponent(20.0f)));
    REQUIRE(surface.find(SurfaceRole::BaseColor) != nullptr);
    REQUIRE(surface.find(SurfaceRole::Specular) != nullptr);
    CHECK(surface.find(SurfaceRole::Specular)->input.texture == 1);
    const SurfaceBinding* dropped = surface.find(SurfaceRole::Dropped);
    REQUIRE(dropped != nullptr);
    CHECK(std::string(dropped->what) == "the lightmap");
}

TEST_CASE("a combiner chain's base colour is the first stage with a file", "[wem][interchange]") {
    Document document = textured(2);
    document.textures[0].replaceableId = 1; // The team-colour plate.
    Material material;
    CommonMaterial& common = material.InitCommon();
    common.setKind(MaterialKind::Combiners);
    CombinersBody& body = *common.combiners();
    body.stages.push_back(CombinerStage{makeInput(0), CombinerOp::Opaque, CombinerOp::Opaque});
    body.stages.push_back(CombinerStage{makeInput(1), CombinerOp::Mod, CombinerOp::Mod});

    const FlatSurface surface = FlattenSurface(document, material);
    REQUIRE(surface.find(SurfaceRole::BaseColor) != nullptr);
    CHECK(surface.find(SurfaceRole::BaseColor)->input.texture == 1);
    CHECK_FALSE(surface.gameComposited);
    CHECK_FALSE(surface.hasSpecular);
}

TEST_CASE("a derived Phong surface is not a metal", "[wem][interchange][derive]") {
    Material material;
    material.name = "phong";
    CommonMaterial& common = material.InitCommon();
    common.setKind(MaterialKind::LegacyDeferred);
    common.legacy()->specularExponent = 20.0f;
    std::vector<Material> materials;
    materials.push_back(std::move(material));
    Document document = makeDocument(ProfileId::Generic);
    document.models[0].profileSets[0] = makeSet(ProfileId::Generic, std::move(materials));

    const DeriveResult result = DeriveProfile(document, ProfileId::Generic, ProfileId::Wc3Reforged);
    REQUIRE(result.ok);
    const ProfileMaterialSet* set = document.models[0].setFor(ProfileId::Wc3Reforged);
    REQUIRE(set != nullptr);
    REQUIRE(!set->materials.empty());
    const PbrDeferredBody* pbr = set->materials[0].Common().pbr();
    REQUIRE(pbr != nullptr);
    CHECK(pbr->metallicFactor == 0.0f);
    CHECK(pbr->roughnessFactor == Catch::Approx(textures::pbr::RoughnessFromExponent(20.0f)));
}

TEST_CASE("a posed normal stays perpendicular to its posed surface", "[wem][interchange][skin]") {
    // A sheared, non-uniformly scaled bone: the case a plain 3x3 gets wrong.
    geom::MeshBuilder builder;
    const Vector3f rest[3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 1}};
    geom::VertexId ids[3];
    for (u32 i = 0; i < 3; ++i) {
        ids[i] = builder.addVertex(rest[i]);
        builder.addInfluence(ids[i], 0, 1.0f);
    }
    MeshSection section;
    builder.addSection(section);
    const geom::FaceId face = builder.addTriangle(ids[0], ids[1], ids[2], 0);
    const Vector3f restNormal = normalized(cross(rest[1] - rest[0], rest[2] - rest[0]));
    for (u32 k = 0; k < 3; ++k) {
        builder.setCornerAttr(face, k, geom::names::kNormal, restNormal);
    }
    const Mesh mesh = builder.build().mesh;

    for (const f32 mirror : {1.0f, -1.0f}) {
        Matrix44f skin = Matrix44f::identity();
        skin.data[0][0] = 2.0f * mirror;
        skin.data[1][0] = 0.5f;
        skin.data[2][2] = 3.0f;
        skin.data[3][0] = 4.0f;
        const std::vector<Matrix44f> palette{skin};
        const std::vector<Vector3f> posed = skinning::DeformMesh(mesh, palette);
        const std::vector<Vector3f> normals = skinning::DeformNormals(mesh, palette);
        // The posed geometric normal, wound as the face is: a mirror flips it,
        // and the posed shading normal must follow.
        Vector3f expected = normalized(cross(posed[1] - posed[0], posed[2] - posed[0]));
        if (mirror < 0.0f) {
            expected = expected * -1.0f;
        }
        const auto corners =
            mesh.attributes.get<Vector3f>(geom::names::kNormal, geom::Domain::Halfedge);
        u32 checked = 0;
        for (std::size_t h = 0; h < normals.size(); ++h) {
            if (corners[h].x == 0.0f && corners[h].y == 0.0f && corners[h].z == 0.0f) {
                continue; // Boundary halfedges hold no normal.
            }
            INFO("mirror " << mirror << " halfedge " << h);
            CHECK(normals[h].x == Catch::Approx(expected.x).margin(1e-5));
            CHECK(normals[h].y == Catch::Approx(expected.y).margin(1e-5));
            CHECK(normals[h].z == Catch::Approx(expected.z).margin(1e-5));
            ++checked;
        }
        CHECK(checked == 3);
    }
}
