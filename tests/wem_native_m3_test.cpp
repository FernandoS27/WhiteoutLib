// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// WEM v3 P3c — the M3 material core and its native block.
///
/// Three claims are worth asserting here rather than only sweeping for:
///
/// 1. **A material is reached through the map.** `MaterialMap{type, index}` is
///    what a region names, and each type has its own array. A converter that read
///    `standardMaterials[i]` directly would work on almost everything and then
///    quietly mis-read the first volume material it met.
/// 2. **The layers with no channel are reported, not dropped in silence.**
///    Height, lightmap and the normal-blend pair have no `SurfaceChannel`;
///    they survive in the native block and say so.
/// 3. **The nine non-standard kinds do not pretend.** A lens flare is not a layer
///    stack, and its common material must arrive with `LossyKindConversion` on it.

#include <iostream>
#include <map>
#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/m3/parser.h>

#include "test_helpers.h"
#include "wem_corpus_files.h"
#include "whiteout/models/wem/materials/m3_core.h"

namespace fs = std::filesystem;
using namespace whiteout;
using namespace whiteout::models::wem;

namespace {

m3::TextureLayer makeLayer(const char* path) {
    m3::TextureLayer layer;
    layer.texturePath = path;
    layer.color.initValue = m3::ColorBGRA{255, 255, 255, 255};
    layer.mapAlpha.initValue = 1.0f;
    return layer;
}

m3_core::Context makeContext(u32 version = 26) {
    m3_core::Context context;
    context.modelVersion = version;
    context.texturesByPath = {
        {"diffuse.dds", 0}, {"normal.dds", 1}, {"spec.dds", 2},
        {"emis.dds", 3},    {"gloss.dds", 4},  {"env.dds", 5},
    };
    return context;
}

m3::MaterialMap mapEntry(m3::MaterialType type, u32 index) {
    m3::MaterialMap entry;
    entry.materialType = type;
    entry.materialIndex = index;
    return entry;
}

} // namespace

TEST_CASE("wem an m3 standard material becomes a composite over channels", "[wem][materials][m3]") {
    m3::Model model;
    m3::StandardMaterial standard;
    standard.name = "hero_body";
    standard.blendMode = m3::BlendMode::AlphaBlend;
    standard.priority = 4;
    standard.alphaTestThreshold = 128;
    standard.specularExponent = 32.0f;
    standard.flags = m3::MaterialFlag::TwoSided | m3::MaterialFlag::Unfogged;
    standard.diffuseLayer = makeLayer("diffuse.dds");
    standard.normalLayer = makeLayer("normal.dds");
    standard.specularLayer = makeLayer("spec.dds");
    standard.emissiveLayer1 = makeLayer("emis.dds");
    model.standardMaterials.push_back(standard);

    Diagnostics diagnostics;
    const Material imported = m3_core::ImportMaterial(
        model, mapEntry(m3::MaterialType::Standard, 0), ProfileId::Sc2, makeContext(), diagnostics);

    CHECK(imported.name == "hero_body");
    CHECK(imported.Common().kind() == MaterialKind::Composite);
    CHECK(imported.Common().blend == BlendMode::AlphaBlend);
    CHECK(imported.Common().cull == CullMode::None);
    CHECK(imported.Common().priorityPlane == 4);

    // M3 stores the cut-off in 0..255; the common material normalises, so a
    // consumer never has to know which convention it came from.
    CHECK(imported.Common().alphaTestThreshold > 0.5f);
    CHECK(imported.Common().alphaTestThreshold < 0.51f);

    const CompositeBody* body = imported.Common().composite();
    REQUIRE(body != nullptr);
    REQUIRE(body->layers.size() == 4);
    CHECK(body->layers[0].target == SurfaceChannel::Color);
    CHECK(body->layers[0].op == CompositeOp::Set);
    CHECK(body->layers[1].target == SurfaceChannel::Specular);
    CHECK(body->layers[2].target == SurfaceChannel::Emissive);
    CHECK(body->layers[3].target == SurfaceChannel::Normal);
    CHECK(body->specularExponent == 32.0f);
}

TEST_CASE("wem a layer with no surface channel is reported, not silently lost",
          "[wem][materials][m3]") {
    m3::Model model;
    m3::StandardMaterial standard;
    standard.diffuseLayer = makeLayer("diffuse.dds");
    standard.glossLayer = makeLayer("gloss.dds");
    standard.heightLayer = makeLayer("gloss.dds");
    standard.alphaLayer1 = makeLayer("gloss.dds");
    model.standardMaterials.push_back(standard);

    Diagnostics diagnostics;
    const Material imported = m3_core::ImportMaterial(
        model, mapEntry(m3::MaterialType::Standard, 0), ProfileId::Sc2, makeContext(), diagnostics);

    // Height alone. The alpha mask is the surface's coverage and the gloss its
    // specularity scale; each lands on its own channel.
    CHECK(diagnostics.countOf(DiagCode::LayerDropped) == 1);
    const CompositeBody* body = imported.Common().composite();
    REQUIRE(body != nullptr);
    CHECK(body->layersOf(SurfaceChannel::Coverage).size() == 1);
    CHECK(body->layersOf(SurfaceChannel::Gloss).size() == 1);
    // Reported, and still there: the native block is what makes the report a
    // note rather than a loss.
    const auto& block = std::get<native::M3Material>(imported.Native());
    const auto* standardBody = std::get_if<native::M3Standard>(&block.body);
    REQUIRE(standardBody != nullptr);
    CHECK(standardBody->glossLayer.has_value());
    CHECK(standardBody->heightLayer.has_value());
    CHECK(standardBody->alphaLayer1.has_value());
}

TEST_CASE("wem the second emissive layer takes the second emissive blend mode",
          "[wem][materials][m3]") {
    m3::Model model;
    m3::StandardMaterial standard;
    standard.diffuseLayer = makeLayer("diffuse.dds");
    standard.emissiveLayer1 = makeLayer("emis.dds");
    standard.emissiveLayer2 = makeLayer("emis.dds");
    standard.emissiveBlendMode1 = m3::LayerBlendOp::Mod;
    standard.emissiveBlendMode2 = m3::LayerBlendOp::Add;
    model.standardMaterials.push_back(standard);

    Diagnostics diagnostics;
    const Material imported = m3_core::ImportMaterial(
        model, mapEntry(m3::MaterialType::Standard, 0), ProfileId::Sc2, makeContext(), diagnostics);

    const CompositeBody* body = imported.Common().composite();
    REQUIRE(body != nullptr);
    const std::vector<u32> emissive = body->layersOf(SurfaceChannel::Emissive);
    REQUIRE(emissive.size() == 2);
    CHECK(body->layers[emissive[0]].op == CompositeOp::Modulate);
    // `AddAlpha`, not `Add`: M3's `Add` is `base + layer.rgb * layer.a`
    // (`CombineLayerColor`), and `AddNoAlpha` is the one that ignores alpha.
    CHECK(body->layers[emissive[1]].op == CompositeOp::AddAlpha);
}

TEST_CASE("wem m3 tells its two adds apart", "[wem][materials][m3]") {
    // The distinction the shader draws and the import used to fold away. It only
    // shows once the material leaves StarCraft II: Warcraft III's `Additive` does
    // not consult alpha and its `AddAlpha` does, so collapsing both onto `Add`
    // drew a white decal texel at full strength.
    const auto opOfEmissive = [](m3::LayerBlendOp blend) {
        m3::Model model;
        m3::StandardMaterial standard;
        standard.diffuseLayer = makeLayer("diffuse.dds");
        standard.emissiveLayer1 = makeLayer("emis.dds");
        standard.emissiveBlendMode1 = blend;
        model.standardMaterials.push_back(standard);

        Diagnostics diagnostics;
        const Material imported =
            m3_core::ImportMaterial(model, mapEntry(m3::MaterialType::Standard, 0), ProfileId::Sc2,
                                    makeContext(), diagnostics);
        const CompositeBody* body = imported.Common().composite();
        REQUIRE(body != nullptr);
        const std::vector<u32> emissive = body->layersOf(SurfaceChannel::Emissive);
        REQUIRE(emissive.size() == 1);
        return body->layers[emissive[0]].op;
    };
    CHECK(opOfEmissive(m3::LayerBlendOp::Add) == CompositeOp::AddAlpha);
    CHECK(opOfEmissive(m3::LayerBlendOp::AddNoAlpha) == CompositeOp::Add);
}

TEST_CASE("wem a non-standard m3 kind reports that its projection is a likeness",
          "[wem][materials][m3]") {
    m3::Model model;
    m3::VolumeMaterial volume;
    volume.name = "smoke";
    volume.colorMap = makeLayer("diffuse.dds");
    model.volumeMaterials.push_back(volume);

    Diagnostics diagnostics;
    const Material imported = m3_core::ImportMaterial(model, mapEntry(m3::MaterialType::Volume, 0),
                                                      ProfileId::Sc2, makeContext(), diagnostics);

    CHECK(imported.name == "smoke");
    CHECK(diagnostics.countOf(DiagCode::LossyKindConversion) == 1);
    const auto& block = std::get<native::M3Material>(imported.Native());
    CHECK(block.kind == native::M3MaterialKind::Volume);
    CHECK(std::holds_alternative<native::M3Volume>(block.body));
}

TEST_CASE("wem a defunct HAI_ material diagnoses rather than parsing", "[wem][materials][m3]") {
    // The parser header says `hairMaterials` is always null and the corpus
    // agrees, so the WEM variant has no alternative for it. A file that does
    // carry one must say so rather than being quietly reinterpreted.
    m3::Model model;
    model.hairMaterials.push_back(m3::HairMaterial{});
    model.materialMaps.push_back(mapEntry(m3::MaterialType::Hair, 0));

    Diagnostics diagnostics;
    const std::vector<Material> materials =
        m3_core::ImportMaterials(model, ProfileId::Sc2, makeContext(), diagnostics);
    REQUIRE(materials.size() == 1);
    CHECK(diagnostics.countOf(DiagCode::UnsupportedMaterialKind) == 2);
}

TEST_CASE("wem a material map naming a missing index is an error", "[wem][materials][m3]") {
    m3::Model model;
    Diagnostics diagnostics;
    m3_core::ImportMaterial(model, mapEntry(m3::MaterialType::Standard, 3), ProfileId::Sc2,
                            makeContext(), diagnostics);
    CHECK(diagnostics.countOf(DiagCode::IndexOutOfRange) == 1);
}

TEST_CASE("wem an m3 material survives import-export-import", "[wem][materials][m3]") {
    m3::Model model;
    m3::StandardMaterial standard;
    standard.name = "round_trip";
    standard.blendMode = m3::BlendMode::Mod2x;
    standard.priority = -2;
    standard.specularExponent = 12.5f;
    standard.hdrEmissiveMultiplier = 3.0f;
    standard.diffuseLayer = makeLayer("diffuse.dds");
    standard.normalLayer = makeLayer("normal.dds");
    standard.glossLayer = makeLayer("gloss.dds");
    model.standardMaterials.push_back(standard);

    Diagnostics diagnostics;
    const m3_core::Context context = makeContext();
    const Material first = m3_core::ImportMaterial(model, mapEntry(m3::MaterialType::Standard, 0),
                                                   ProfileId::Sc2, context, diagnostics);

    m3::Model rebuilt;
    const m3::MaterialMap entry =
        m3_core::ExportMaterial(first, ProfileId::Sc2, context, rebuilt, diagnostics);
    REQUIRE(entry.materialType == m3::MaterialType::Standard);
    REQUIRE(rebuilt.standardMaterials.size() == 1);

    const Material second =
        m3_core::ImportMaterial(rebuilt, entry, ProfileId::Sc2, context, diagnostics);

    const auto& a = std::get<native::M3Standard>(std::get<native::M3Material>(first.Native()).body);
    const auto& b =
        std::get<native::M3Standard>(std::get<native::M3Material>(second.Native()).body);
    CHECK(a.name == b.name);
    CHECK(a.blendMode == b.blendMode);
    CHECK(a.priority == b.priority);
    CHECK(a.specularExponent == b.specularExponent);
    CHECK(a.hdrEmissiveMultiplier == b.hdrEmissiveMultiplier);
    CHECK(a.diffuseLayer.has_value() == b.diffuseLayer.has_value());
    CHECK(a.normalLayer.has_value() == b.normalLayer.has_value());
    // The gloss layer has no channel and still comes back: that is the block
    // doing its job, and the reason the export path prefers it over the common
    // material (§7.1).
    CHECK(a.glossLayer.has_value() == b.glossLayer.has_value());
    CHECK(a.glossLayer->texturePath == b.glossLayer->texturePath);
}

TEST_CASE("wem an m3 layer's UV transform is a pivoted TRS", "[wem][materials][m3][uv]") {
    // StarCraft II composes `uv' = tiling * R(uvAngle.z) * (uv - 0.5 - offset)
    // + 0.5` (`sub_102ABBDE0`), and `TextureInput::uvTransform` is the flat
    // affine that means. Writing the three fields into the matrix's diagonal
    // and translation column -- which is what this replaced -- said something
    // else entirely: the offset came out positive, applied after the scale,
    // and the angle was never written at all.
    m3::TextureLayer layer = makeLayer("scroll.dds");

    SECTION("a plain scroll is the offset negated") {
        layer.uvOffset.initValue = Vector2f(0.25f, -0.5f);
        const Matrix3x2f matrix = m3_core::UvTransformOf(layer);
        // uv' = uv - offset: the linear block is the identity and the column
        // is what the pivot leaves behind.
        CHECK(matrix.m[0][0] == Catch::Approx(1.0f));
        CHECK(matrix.m[1][1] == Catch::Approx(1.0f));
        CHECK(matrix.m[0][2] == Catch::Approx(-0.25f));
        CHECK(matrix.m[1][2] == Catch::Approx(0.5f));
        const Vector2f moved = matrix.apply(Vector2f{0.5f, 0.5f});
        CHECK(moved.x == Catch::Approx(0.25f));
        CHECK(moved.y == Catch::Approx(1.0f));
    }

    SECTION("a turn is about the texture centre") {
        layer.uvAngle.initValue = Vector3f(0.0f, 0.0f, 1.5707963f);
        const Matrix3x2f matrix = m3_core::UvTransformOf(layer);
        // The centre is the fixed point of a pure turn.
        const Vector2f centre = matrix.apply(Vector2f{0.5f, 0.5f});
        CHECK(centre.x == Catch::Approx(0.5f).margin(1e-5f));
        CHECK(centre.y == Catch::Approx(0.5f).margin(1e-5f));
        // …and the top-left corner lands where a quarter turn puts it.
        const Vector2f corner = matrix.apply(Vector2f{0.0f, 0.0f});
        CHECK(corner.x == Catch::Approx(1.0f).margin(1e-5f));
        CHECK(corner.y == Catch::Approx(0.0f).margin(1e-5f));
    }

    SECTION("a tiling of zero is silence, not a scale of zero") {
        layer.uvTiling.initValue = Vector2f(0.0f, 0.0f);
        const Matrix3x2f matrix = m3_core::UvTransformOf(layer);
        CHECK(matrix.isIdentity());
    }

    SECTION("all three return through the inverse") {
        layer.uvOffset.initValue = Vector2f(0.125f, -0.75f);
        layer.uvAngle.initValue = Vector3f(0.0f, 0.0f, -1.5707963f);
        layer.uvTiling.initValue = Vector2f(2.0f, 2.0f);
        const Matrix3x2f matrix = m3_core::UvTransformOf(layer);

        m3::TextureLayer back = makeLayer("scroll.dds");
        m3_core::SetUvTransform(matrix, back);
        CHECK(back.uvOffset.initValue.x == Catch::Approx(0.125f));
        CHECK(back.uvOffset.initValue.y == Catch::Approx(-0.75f));
        CHECK(back.uvAngle.initValue.z == Catch::Approx(-1.5707963f));
        CHECK(back.uvTiling.initValue.x == Catch::Approx(2.0f));
        CHECK(back.uvTiling.initValue.y == Catch::Approx(2.0f));
    }
}

TEST_CASE("wem the m3 copy halves declare no manual fields", "[wem][materials][m3]") {
    CHECK(native::kM3ManualFieldCount == 0);
}

// ============================================================================
// Corpus
// ============================================================================

TEST_CASE("wem m3 corpus materials land in the kind 7.2.6 names", "[wem][corpus][materials][m3]") {
    const std::vector<fs::path> files =
        test::gather("WEM_M3_CORPUS_DIR", ".m3", {"Sc2M3", "Sc2BetaM3", "HotSM3", "StarM3"});
    if (files.empty()) {
        SKIP("M3 corpus not found");
    }
    const std::size_t limit = test::sweepLimit(files.size(), 200);

    std::size_t models = 0;
    std::size_t materials = 0;
    std::size_t composites = 0;
    std::size_t dataDriven = 0;
    std::size_t restored = 0;
    // Whether a MADD chunk is even present is a separate question from whether
    // the material map points at one, and the two disagree: MADD is a v30+
    // chunk, so a v29 file has none to point at.
    std::size_t modelsWithMadd = 0;
    std::size_t maddRecords = 0;
    // A MADD record no material map entry points at. Whether a record exists and
    // whether a material reaches it are separate questions, and the corpus
    // answers them differently.
    std::size_t maddOrphans = 0;
    // Why a MADD did not restore, in the restore's own words. A bare
    // restored-vs-authored count says the path is lossy without saying where.
    std::map<std::string, std::size_t> maddBlockers;
    std::size_t byKind[16] = {};

    for (std::size_t i = 0; i < limit; ++i) {
        test::trace(files[i]);
        const std::vector<u8> bytes = test::readCorpusFile(files[i]);
        if (bytes.empty()) {
            continue;
        }
        m3::Parser parser;
        const m3::Model model = parser.parse(bytes);
        if (model.materialMaps.empty()) {
            continue;
        }
        ++models;
        if (!model.dataDrivenMaterials.empty()) {
            ++modelsWithMadd;
            maddRecords += model.dataDrivenMaterials.size();

            std::vector<bool> reached(model.dataDrivenMaterials.size(), false);
            for (const auto& entry : model.materialMaps) {
                if (entry.materialType == m3::MaterialType::DataDriven &&
                    entry.materialIndex < reached.size()) {
                    reached[entry.materialIndex] = true;
                }
            }
            for (const bool hit : reached) {
                if (!hit) {
                    ++maddOrphans;
                }
            }
        }

        Diagnostics diagnostics;
        m3_core::Context context;
        context.modelVersion = static_cast<u32>(model.getVersion());
        const std::vector<Material> imported =
            m3_core::ImportMaterials(model, ProfileId::Sc2, context, diagnostics);

        constexpr std::string_view kNoForm = "MADD has no StandardMaterial form (";
        for (const Diagnostic& note : diagnostics.all()) {
            const std::size_t open = note.message.find(kNoForm);
            if (open == std::string::npos) {
                continue;
            }
            const std::size_t start = open + kNoForm.size();
            const std::size_t close = note.message.find(')', start);
            if (close != std::string::npos) {
                ++maddBlockers[note.message.substr(start, close - start)];
            }
        }

        for (const Material& material : imported) {
            ++materials;
            if (material.Common().kind() == MaterialKind::Composite) {
                ++composites;
            }
            const auto& block = std::get<native::M3Material>(material.Native());
            const u32 kind = static_cast<u32>(block.kind);
            if (kind < 16) {
                ++byKind[kind];
            }
            if (block.authoredDataDriven) {
                ++dataDriven;
                if (block.restoredStandard.has_value()) {
                    ++restored;
                }
            }
        }
    }

    if (materials == 0) {
        SKIP("no M3 materials parsed");
    }

    std::cout << "\n=== M3 material kinds ===\n"
              << "  models       : " << models << " of " << limit << " scanned\n"
              << "  materials    : " << materials << "\n"
              << "  Composite    : " << composites << "\n"
              << "  MADD authored: " << dataDriven << " (" << restored << " restored)\n"
              << "  MADD records : " << maddRecords << " in " << modelsWithMadd << " model(s), "
              << maddOrphans << " unreferenced\n";
    for (const auto& [blocker, count] : maddBlockers) {
        std::cout << "    no standard form: " << blocker << " : " << count << "\n";
    }
    for (u32 kind = 1; kind < 13; ++kind) {
        if (byKind[kind] != 0) {
            std::cout << "    " << ToString(static_cast<native::M3MaterialKind>(kind)) << " : "
                      << byKind[kind] << "\n";
        }
    }

    // §7.2.6 says every SC2 / Heroes material becomes `Composite`. Nothing in
    // the M3 path produces any other kind, so this is the whole assertion the
    // sweep can make — and it is the one that would catch a body accidentally
    // left as a slot map.
    CHECK(composites == materials);
}

TEST_CASE("wem a gloss layer and simulate-roughness fold back through the composite",
          "[wem][materials][m3]") {
    m3::Model model;
    m3::StandardMaterial standard;
    standard.name = "pbr";
    standard.flags = m3::MaterialFlag::SimulateRoughness;
    standard.layerBlendMode = m3::LayerBlendOp::Add;
    standard.specularExponent = 512.0f;
    standard.diffuseLayer = makeLayer("diffuse.dds");
    standard.specularLayer = makeLayer("spec.dds");
    standard.glossLayer = makeLayer("gloss.dds");
    standard.environmentLayer = makeLayer("env.dds");
    standard.environmentLayer->uvMapping = m3::UVMappingMode::ReflectCubicEnvio;
    standard.environmentMaskLayer = makeLayer("spec.dds");
    model.standardMaterials.push_back(standard);

    Diagnostics diagnostics;
    const m3_core::Context context = makeContext();
    Material imported = m3_core::ImportMaterial(model, mapEntry(m3::MaterialType::Standard, 0),
                                                ProfileId::Sc2, context, diagnostics);
    const CompositeBody* body = imported.Common().composite();
    REQUIRE(body != nullptr);
    CHECK(body->simulateRoughness);
    CHECK(body->layersOf(SurfaceChannel::Gloss).size() == 1);
    CHECK(body->layersOf(SurfaceChannel::Environment).size() == 2);

    // Edited, so the fold and not the native block answers the export.
    imported.MutableCommon();
    m3::Model rebuilt;
    const m3::MaterialMap entry =
        m3_core::ExportMaterial(imported, ProfileId::Sc2, context, rebuilt, diagnostics);
    REQUIRE(entry.materialType == m3::MaterialType::Standard);
    REQUIRE(rebuilt.standardMaterials.size() == 1);
    const m3::StandardMaterial& out = rebuilt.standardMaterials[0];
    CHECK((static_cast<u32>(out.flags) & static_cast<u32>(m3::MaterialFlag::SimulateRoughness)) !=
          0);
    REQUIRE(out.glossLayer.has_value());
    CHECK(out.glossLayer->colorType == m3::ColorChannelSelect::Alpha);
    REQUIRE(out.environmentLayer.has_value());
    CHECK(out.environmentLayer->uvMapping == m3::UVMappingMode::ReflectCubicEnvio);
    CHECK(out.environmentMaskLayer.has_value());
    CHECK(out.layerBlendMode == m3::LayerBlendOp::Add);
    CHECK(out.hdrEnvironmentConstant > 0.0f);
}

TEST_CASE("wem a rim beside an additive reflection leaves the shared op alone",
          "[wem][materials][m3]") {
    Material material;
    CommonMaterial& common = material.MutableCommon();
    const auto layer = [](u32 texture, SurfaceChannel target, CompositeOp op) {
        CompositeLayer l;
        l.input.texture = texture;
        l.target = target;
        l.op = op;
        return l;
    };
    CompositeBody body;
    body.layers.push_back(layer(0, SurfaceChannel::Color, CompositeOp::Set));
    body.layers.push_back(layer(3, SurfaceChannel::Emissive, CompositeOp::Add));
    CompositeLayer env = layer(5, SurfaceChannel::Environment, CompositeOp::Add);
    env.input.mapping = UVMappingMode::EnvCube;
    body.layers.push_back(env);
    body.layers.push_back(layer(2, SurfaceChannel::Environment, CompositeOp::Modulate));
    body.environmentFactor = 1.0f;
    common.body = body;
    MaterialFeature rim;
    rim.id = NextFeatureId(common.features);
    rim.layer = kWholeMaterial;
    FresnelFeature fresnel;
    fresnel.color = Vector3f{1.0f, 1.0f, 1.0f};
    fresnel.outMin = 0.0f;
    fresnel.outMax = 0.5f;
    fresnel.exponent = 2.0f;
    rim.payload = fresnel;
    common.features.push_back(rim);

    Diagnostics diagnostics;
    m3::Model rebuilt;
    const m3::MaterialMap entry =
        m3_core::ExportMaterial(material, ProfileId::Sc2, makeContext(), rebuilt, diagnostics);
    REQUIRE(entry.materialType == m3::MaterialType::Standard);
    REQUIRE(rebuilt.standardMaterials.size() == 1);
    const m3::StandardMaterial& out = rebuilt.standardMaterials[0];
    // With the emissive slot taken by the map, the dim would have gone to the
    // decal and turned the reflection's Add into a Mod over its own mask.
    CHECK(out.environmentLayer.has_value());
    CHECK(out.layerBlendMode == m3::LayerBlendOp::Add);
    CHECK_FALSE(out.decalLayer.has_value());
}
