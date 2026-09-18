// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// WEM v3 P3c — one `.mdx` producing two profiles over one geometry.
///
/// The mechanism is per-*layer*, and the rule is which shaders each model may
/// use: a Reforged (HD) model all four — HD, Crystal, SD on HD, SD — and a
/// classic (SD) model SD alone. So a single material can feed a `Wc3Classic` set
/// (its SD layers) and a `Wc3Reforged` set (all of it) at once, and each set's
/// native block holds only what that profile may hold. That is what makes
/// `DeriveProfile(Wc3Reforged -> Wc3Classic)` a layer filter rather than a
/// re-derivation through `CommonMaterial` (§7.3), and it is the reason the two
/// WC3 profiles share one native block type at all.
///
/// The design's open question #1 asks how much shipped content actually carries
/// both kinds. The corpus case at the end publishes that number rather than
/// asserting one, because the answer decides whether the two-set import is the
/// common path or a rarity — and nobody has measured it.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/mdx/parser.h>

#include "test_helpers.h"
#include "wem_corpus_files.h"
#include "whiteout/models/wem/materials/mdx_core.h"

namespace fs = std::filesystem;
using namespace whiteout;
using namespace whiteout::models::wem;
using mdx::Layer;

namespace {

Layer makeLayer(Layer::FilterMode mode, bool hd, u32 textureId) {
    Layer layer;
    layer.filterMode = mode;
    layer.is_hd = hd;
    layer.textureId = textureId;
    if (hd) {
        layer.shader = Layer::ShaderType::HD;
    }
    return layer;
}

mdx_core::Context makeContext(u32 version = 1200, u32 textureCount = 12) {
    mdx_core::Context context;
    context.modelVersion = version;
    for (u32 i = 0; i < textureCount; ++i) {
        context.textureIndexMap.push_back(i);
    }
    return context;
}

/// Two SD layers and the six-layer positional HD convention, in one material.
///
/// Only representable from v1100 up: below that there is no per-layer shader, so
/// `Material::shader` is the only signal and the whole material is one kind or
/// the other. `makeContext` therefore defaults to v1200.
mdx::Material makeMixedMaterial() {
    mdx::Material material;
    material.shader = "Shader_HD_DefaultUnit";
    material.layers.push_back(makeLayer(Layer::FilterMode::None, false, 0));
    material.layers.push_back(makeLayer(Layer::FilterMode::Blend, false, 1));
    for (u32 slot = 0; slot < 6; ++slot) {
        material.layers.push_back(makeLayer(Layer::FilterMode::None, true, 2 + slot));
    }
    return material;
}

/// A `PBRDeferred` material with @p slots filled, each naming its own texture.
///
/// No native block, so `ExportMaterial` goes through the kind mapping — which is
/// the whole point: a material that came out of an `.mdx` reproduces its block
/// and never reaches `exportPbr`, so only a DERIVED one exercises this.
Material makePbr(std::initializer_list<std::pair<PbrSlot, u32>> slots) {
    Material material;
    material.name = "derived";
    PbrDeferredBody body;
    for (const auto& [slot, texture] : slots) {
        TextureInput input;
        input.texture = texture;
        body.set(slot, input);
    }
    material.InitCommon().body = std::move(body);
    return material;
}

/// A context that can intern the stock maps, the way the converter's does.
struct StockContext {
    std::vector<mdx::Texture> textures;
    mdx_core::Context context;

    explicit StockContext(u32 version) {
        context = makeContext(version);
        context.stockTextures = &textures;
        context.stockBase = static_cast<u32>(context.textureIndexMap.size());
    }

    /// What the `.mdx` texture @p id is, as a name — a stock file's path, or
    /// `#<replaceable>`, or `document` for one the document contributed.
    std::string nameOf(u32 id) const {
        if (id < context.stockBase) {
            return "document";
        }
        const std::size_t at = id - context.stockBase;
        if (at >= textures.size()) {
            return "<out of range>";
        }
        const mdx::Texture& texture = textures[at];
        return texture.replaceableId != 0 ? "#" + std::to_string(texture.replaceableId)
                                          : texture.fileName;
    }
};

/// The texture each of the six HD slots names, whichever way the version
/// spells them.
std::vector<u32> slotTextures(const mdx::Material& material) {
    std::vector<u32> ids;
    if (material.layers.size() == 1 && !material.layers[0].subTextures.empty()) {
        for (const Layer::SubTexture& sub : material.layers[0].subTextures) {
            ids.push_back(sub.textureId);
        }
        return ids;
    }
    for (const Layer& layer : material.layers) {
        ids.push_back(layer.textureId);
    }
    return ids;
}

} // namespace

TEST_CASE("wem one mdx material feeds both WC3 profiles", "[wem][materials][mdx]") {
    const mdx::Material source = makeMixedMaterial();
    CHECK(mdx_core::HasLayersFor(source, ProfileId::Wc3Classic, makeContext()));
    CHECK(mdx_core::HasLayersFor(source, ProfileId::Wc3Reforged, makeContext()));

    Diagnostics diagnostics;
    const Material classic =
        mdx_core::ImportMaterial(source, ProfileId::Wc3Classic, makeContext(), diagnostics);
    const Material reforged =
        mdx_core::ImportMaterial(source, ProfileId::Wc3Reforged, makeContext(), diagnostics);

    // Each set gets the kind §7.2.6 names for its profile...
    CHECK(classic.Common().kind() == MaterialKind::Composite);
    CHECK(reforged.Common().kind() == MaterialKind::PBRDeferred);

    // ...and the classic stack is exactly the two SD layers, not eight.
    const CompositeBody* composite = classic.Common().composite();
    REQUIRE(composite != nullptr);
    CHECK(composite->layers.size() == 2);

    const PbrDeferredBody* pbr = reforged.Common().pbr();
    REQUIRE(pbr != nullptr);
    CHECK(pbr->slots.size() == 6);
    CHECK(pbr->find(PbrSlot::BaseColor) != nullptr);
    CHECK(pbr->find(PbrSlot::Normal) != nullptr);
    CHECK(pbr->find(PbrSlot::Orm) != nullptr);
    CHECK(pbr->find(PbrSlot::TeamColorMask) != nullptr);
}

TEST_CASE("wem the classic block holds the SD layers, the Reforged block all of them",
          "[wem][materials][mdx]") {
    const mdx::Material source = makeMixedMaterial();
    Diagnostics diagnostics;
    const Material classic =
        mdx_core::ImportMaterial(source, ProfileId::Wc3Classic, makeContext(), diagnostics);
    const Material reforged =
        mdx_core::ImportMaterial(source, ProfileId::Wc3Reforged, makeContext(), diagnostics);

    REQUIRE(classic.nativeKind() == NativeKind::Mdx);
    REQUIRE(reforged.nativeKind() == NativeKind::Mdx);

    const auto& classicBlock = std::get<native::MdxMaterial>(classic.Native());
    const auto& reforgedBlock = std::get<native::MdxMaterial>(reforged.Native());
    CHECK(classicBlock.layers.size() == 2);
    REQUIRE(reforgedBlock.layers.size() == 8);
    for (const native::MdxLayer& layer : classicBlock.layers) {
        CHECK_FALSE(layer.isHd);
    }
    // `isHd` is each layer's own shading, not the set it sits in: the two SD
    // layers a Reforged model keeps are still SD, and must read back so.
    for (std::size_t i = 0; i < reforgedBlock.layers.size(); ++i) {
        CHECK(reforgedBlock.layers[i].isHd == (i >= 2));
    }

    // The block is the format version's, not the material's: nothing in
    // `mdx::Material` says which file it came out of.
    CHECK(classicBlock.sourceVersion == 1200);
}

TEST_CASE("wem each set exports back what it holds", "[wem][materials][mdx]") {
    const mdx::Material source = makeMixedMaterial();
    Diagnostics diagnostics;
    const Material classic =
        mdx_core::ImportMaterial(source, ProfileId::Wc3Classic, makeContext(), diagnostics);
    const Material reforged =
        mdx_core::ImportMaterial(source, ProfileId::Wc3Reforged, makeContext(), diagnostics);

    const mdx::Material classicOut =
        mdx_core::ExportMaterial(classic, ProfileId::Wc3Classic, makeContext(), diagnostics);
    const mdx::Material reforgedOut =
        mdx_core::ExportMaterial(reforged, ProfileId::Wc3Reforged, makeContext(), diagnostics);

    // The classic set is the SD half; the Reforged set is the whole stack, its
    // SD layers still SD.
    REQUIRE(classicOut.layers.size() == 2);
    REQUIRE(reforgedOut.layers.size() == source.layers.size());
    CHECK(classicOut.layers[0].filterMode == Layer::FilterMode::None);
    CHECK(classicOut.layers[1].filterMode == Layer::FilterMode::Blend);
    for (std::size_t i = 0; i < reforgedOut.layers.size(); ++i) {
        CHECK(reforgedOut.layers[i].is_hd == (i >= 2));
        CHECK(reforgedOut.layers[i].textureId == source.layers[i].textureId);
    }
}

TEST_CASE("wem below v1100 the HD split is per material, not per layer", "[wem][materials][mdx]") {
    // `mdx::Layer::is_hd` is the field the format has for this and the parser
    // never writes it, so the discriminator is the shader — and below v1100
    // there is no per-layer shader at all. A v1000 material named
    // `Shader_HD_DefaultUnit` is therefore entirely HD, however many layers it
    // has, and a converter that split it per layer would invent a classic set
    // that the file does not contain.
    const mdx::Material source = makeMixedMaterial();
    const mdx_core::Context v1000 = makeContext(1000);
    CHECK(mdx_core::HasLayersFor(source, ProfileId::Wc3Reforged, v1000));
    for (const Layer& layer : source.layers) {
        CHECK(mdx_core::IsHdLayer(source, layer, 1000));
    }

    // ...and from v1100 the same material splits, because each layer carries
    // its own `ShaderType`.
    CHECK(mdx_core::IsHdLayer(source, source.layers[2], 1200));
    CHECK_FALSE(mdx_core::IsHdLayer(source, source.layers[1], 1200));
}

TEST_CASE("wem a Reforged model uses all four shaders, a classic model SD alone",
          "[wem][materials][mdx]") {
    mdx::Material source;
    for (const Layer::ShaderType shader :
         {Layer::ShaderType::SD, Layer::ShaderType::HD, Layer::ShaderType::SDOnHD,
          Layer::ShaderType::Crystal}) {
        Layer layer = makeLayer(Layer::FilterMode::None, false, 0);
        layer.shader = shader;
        source.layers.push_back(layer);
    }
    for (const Layer& layer : source.layers) {
        CHECK(mdx_core::LayerAllowedIn(source, layer, 1200, ProfileId::Wc3Reforged));
    }
    CHECK(mdx_core::LayerAllowedIn(source, source.layers[0], 1200, ProfileId::Wc3Classic));
    CHECK_FALSE(mdx_core::LayerAllowedIn(source, source.layers[1], 1200, ProfileId::Wc3Classic));
    CHECK_FALSE(mdx_core::LayerAllowedIn(source, source.layers[2], 1200, ProfileId::Wc3Classic));
    CHECK_FALSE(mdx_core::LayerAllowedIn(source, source.layers[3], 1200, ProfileId::Wc3Classic));

    // The classic set keeps the one SD layer, the Reforged set all four.
    Diagnostics diagnostics;
    const Material classic =
        mdx_core::ImportMaterial(source, ProfileId::Wc3Classic, makeContext(), diagnostics);
    const Material reforged =
        mdx_core::ImportMaterial(source, ProfileId::Wc3Reforged, makeContext(), diagnostics);
    CHECK(std::get<native::MdxMaterial>(classic.Native()).layers.size() == 1);
    CHECK(std::get<native::MdxMaterial>(reforged.Native()).layers.size() == 4);
}

TEST_CASE("wem SDOnHD layers belong to the Reforged set, never the classic one",
          "[wem][materials][mdx]") {
    // SD content drawn through the HD pipeline shades the classic way, but it is
    // a Reforged model's shader: a classic model draws SD alone.
    mdx::Material source;
    Layer layer = makeLayer(Layer::FilterMode::None, false, 0);
    layer.shader = Layer::ShaderType::SDOnHD;
    source.layers.push_back(layer);
    CHECK_FALSE(mdx_core::IsHdLayer(source, source.layers[0], 1200));
    CHECK(mdx_core::IsSdOnHdLayer(source, source.layers[0], 1200));
    CHECK(mdx_core::HasLayersFor(source, ProfileId::Wc3Reforged, makeContext()));
    CHECK_FALSE(mdx_core::HasLayersFor(source, ProfileId::Wc3Classic, makeContext()));

    // Below v1100 the material's name says it, for every layer.
    mdx::Material named;
    named.shader = "Shader_SD_FixedFunction";
    named.layers.push_back(makeLayer(Layer::FilterMode::None, false, 0));
    CHECK(mdx_core::IsSdOnHdLayer(named, named.layers[0], 1000));
    CHECK(mdx_core::HasLayersFor(named, ProfileId::Wc3Reforged, makeContext(1000)));
    CHECK_FALSE(mdx_core::HasLayersFor(named, ProfileId::Wc3Classic, makeContext(1000)));

    // A Reforged material of SD-on-HD layers has no PBR slots: it takes the
    // classic projection, not a slot map of guesses.
    Diagnostics diagnostics;
    const Material reforged =
        mdx_core::ImportMaterial(source, ProfileId::Wc3Reforged, makeContext(), diagnostics);
    CHECK(reforged.Common().kind() == MaterialKind::Composite);
}

TEST_CASE("wem an SD material serves Reforged only from a Reforged-era file", "[wem][materials][mdx]") {
    mdx::Material source;
    source.layers.push_back(makeLayer(Layer::FilterMode::None, false, 0));
    // v800 is an SD model: no Reforged set.
    CHECK(mdx_core::HasLayersFor(source, ProfileId::Wc3Classic, makeContext(800)));
    CHECK_FALSE(mdx_core::HasLayersFor(source, ProfileId::Wc3Reforged, makeContext(800)));
    // From v900 it is a Reforged model's material as well, which may use SD.
    CHECK(mdx_core::HasLayersFor(source, ProfileId::Wc3Classic, makeContext()));
    CHECK(mdx_core::HasLayersFor(source, ProfileId::Wc3Reforged, makeContext()));
}


// ============================================================================
// What a written Reforged material has to look like
// ============================================================================

TEST_CASE("wem a Reforged material never leaves a slot empty", "[wem][materials][mdx]") {
    // Measured, not chosen: across the 12,893 six-slot HD layers in
    // `war3.w3mod` not one slot is `-1`. The engine reads that field as a
    // texture id, and a material with no map of its own names Warcraft III's
    // stock neutral instead — `Textures/normal.blp` on 900 shipped files,
    // `Textures/Black32.blp` on 3,237, `ReplaceableTextures/EnvironmentMap.blp`
    // on 3,681, and replaceable 1 in the team slot on all of them.
    for (const u32 version : {1000u, 1200u}) {
        StockContext stock(version);
        Diagnostics diagnostics;
        const mdx::Material exported = mdx_core::ExportMaterial(
            makePbr({{PbrSlot::BaseColor, 0}, {PbrSlot::Normal, 1}, {PbrSlot::Orm, 2}}),
            ProfileId::Wc3Reforged, stock.context, diagnostics);

        const std::vector<u32> ids = slotTextures(exported);
        REQUIRE(ids.size() == mdx_core::kHdPositionalSlotCount);
        for (const u32 id : ids) {
            CHECK(id != mdx_core::kNoTexture);
        }
        CHECK(stock.nameOf(ids[0]) == "document");
        CHECK(stock.nameOf(ids[1]) == "document");
        CHECK(stock.nameOf(ids[2]) == "document");
        CHECK(stock.nameOf(ids[3]) == "Textures/Black32.blp");
        CHECK(stock.nameOf(ids[4]) == "#1");
        CHECK(stock.nameOf(ids[5]) == "ReplaceableTextures/EnvironmentMap.blp");
    }
}

TEST_CASE("wem the environment slot is Warcraft III's own, never the source's",
          "[wem][materials][mdx]") {
    // All 12,893 shipped HD layers name the same file there, because the engine
    // substitutes the current map's reflection for it. A source's own
    // reflection map is a cube the map does not agree with, so it is dropped
    // rather than bound.
    StockContext stock(1200);
    Diagnostics diagnostics;
    const mdx::Material exported = mdx_core::ExportMaterial(
        makePbr({{PbrSlot::BaseColor, 0}, {PbrSlot::Orm, 1}, {PbrSlot::Environment, 2}}),
        ProfileId::Wc3Reforged, stock.context, diagnostics);

    const std::vector<u32> ids = slotTextures(exported);
    REQUIRE(ids.size() == mdx_core::kHdPositionalSlotCount);
    CHECK(stock.nameOf(ids[5]) == "ReplaceableTextures/EnvironmentMap.blp");
}

TEST_CASE("wem a colour-only material goes out as SD on HD", "[wem][materials][mdx]") {
    // A `.m3` effect plane and a `.m2` creature both arrive with a colour map
    // and nothing else, and the HD shader has nothing to say about one: it
    // would light the surface through a flat normal and a mid roughness, which
    // is a plastic sheen over art whose lighting is already painted in.
    // Warcraft III has a shader for exactly this inside a Reforged model.
    for (const u32 version : {1000u, 1200u}) {
        StockContext stock(version);
        Diagnostics diagnostics;
        const mdx::Material exported =
            mdx_core::ExportMaterial(makePbr({{PbrSlot::BaseColor, 3}}), ProfileId::Wc3Reforged,
                                     stock.context, diagnostics);

        CHECK(exported.shader == "Shader_SD_FixedFunction");
        REQUIRE(exported.layers.size() == 1);
        CHECK(exported.layers[0].shader == Layer::ShaderType::SDOnHD);
        CHECK_FALSE(exported.layers[0].is_hd);
        CHECK(slotTextures(exported) == std::vector<u32>{3});
        // Nothing was interned: an SD pass has no slots to fill.
        CHECK(stock.textures.empty());
    }
}

TEST_CASE("wem an emissive that is the colour map is still colour-only", "[wem][materials][mdx]") {
    // How a self-lit surface comes out of a derive: one map, bound twice. There
    // is nothing in the second binding an HD material could say that the first
    // does not.
    StockContext stock(1200);
    Diagnostics diagnostics;
    const mdx::Material same = mdx_core::ExportMaterial(
        makePbr({{PbrSlot::BaseColor, 4}, {PbrSlot::Emissive, 4}}), ProfileId::Wc3Reforged,
        stock.context, diagnostics);
    CHECK(same.shader == "Shader_SD_FixedFunction");
    REQUIRE(same.layers.size() == 1);

    // An emissive map of its own is a different surface, and the HD shader is
    // the only one that can draw it.
    StockContext other(1200);
    const mdx::Material distinct = mdx_core::ExportMaterial(
        makePbr({{PbrSlot::BaseColor, 4}, {PbrSlot::Emissive, 5}}), ProfileId::Wc3Reforged,
        other.context, diagnostics);
    CHECK(distinct.shader == "Shader_HD_DefaultUnit");
    REQUIRE(distinct.layers.size() == 1);
    CHECK(slotTextures(distinct).size() == mdx_core::kHdPositionalSlotCount);
    CHECK(other.nameOf(slotTextures(distinct)[3]) == "document");
}

TEST_CASE("wem without a sink an absent slot still writes no texture", "[wem][materials][mdx]") {
    // The hand-built context a test makes has nowhere to intern to, and there
    // is no answer but `-1` then. Stated so the fallback is a decision rather
    // than something a caller discovers.
    Diagnostics diagnostics;
    const mdx::Material exported = mdx_core::ExportMaterial(
        makePbr({{PbrSlot::BaseColor, 0}, {PbrSlot::Orm, 1}}), ProfileId::Wc3Reforged,
        makeContext(1200), diagnostics);
    const std::vector<u32> ids = slotTextures(exported);
    REQUIRE(ids.size() == mdx_core::kHdPositionalSlotCount);
    CHECK(ids[3] == mdx_core::kNoTexture);
}

// ============================================================================
// The corpus number the design left open
// ============================================================================

TEST_CASE("wem how much WC3 content carries both layer kinds", "[wem][corpus][materials][mdx]") {
    const std::vector<fs::path> files =
        test::gather("WEM_MDX_CORPUS_DIR", ".mdx", {"MDL", "Wc3Mdx"});
    if (files.empty()) {
        SKIP("MDX corpus not found");
    }
    const std::size_t limit = test::sweepLimit(files.size(), 200);

    std::size_t models = 0;
    std::size_t materials = 0;
    std::size_t classicOnly = 0;
    std::size_t reforgedOnly = 0;
    std::size_t both = 0;
    // §7.2.6's other half, measured on the same pass rather than on a second
    // walk of a 34 GB tree: which kind each imported material actually became.
    std::size_t imported = 0;
    std::size_t asComposite = 0;
    std::size_t asCombiners = 0;
    std::size_t asPbr = 0;
    std::size_t other = 0;

    for (std::size_t i = 0; i < limit; ++i) {
        if (test::isKnownBad(files[i])) {
            continue;
        }
        test::trace(files[i]);
        const std::vector<u8> bytes = test::readCorpusFile(files[i]);
        if (bytes.empty()) {
            continue;
        }
        mdx::Parser parser;
        const mdx::Model model = parser.parse(bytes);
        if (model.materials.empty()) {
            continue;
        }
        ++models;
        mdx_core::Context context;
        context.modelVersion = model.version;
        for (u32 t = 0; t < static_cast<u32>(model.textures.size()); ++t) {
            context.textureIndexMap.push_back(t);
        }
        Diagnostics diagnostics;

        for (const mdx::Material& material : model.materials) {
            ++materials;
            const bool classic = mdx_core::HasLayersFor(material, ProfileId::Wc3Classic, context);
            const bool reforged = mdx_core::HasLayersFor(material, ProfileId::Wc3Reforged, context);
            if (classic && reforged) {
                ++both;
            } else if (reforged) {
                ++reforgedOnly;
            } else if (classic) {
                ++classicOnly;
            }

            for (ProfileId profile : {ProfileId::Wc3Classic, ProfileId::Wc3Reforged}) {
                if (!mdx_core::HasLayersFor(material, profile, context)) {
                    continue;
                }
                ++imported;
                const Material result =
                    mdx_core::ImportMaterial(material, profile, context, diagnostics);
                switch (result.Common().kind()) {
                case MaterialKind::Composite:
                    ++asComposite;
                    break;
                case MaterialKind::Combiners:
                    ++asCombiners;
                    break;
                case MaterialKind::PBRDeferred:
                    ++asPbr;
                    break;
                default:
                    ++other;
                    break;
                }
                // `Combiners` only where the collapse is blend-equivalent —
                // asserted per material, because a stack that reached the chain
                // without qualifying is exactly the silent wrong answer.
                if (result.Common().kind() == MaterialKind::Combiners) {
                    std::vector<const Layer*> layers;
                    for (const Layer& layer : material.layers) {
                        if (mdx_core::IsHdLayer(material, layer, model.version) ==
                            (profile == ProfileId::Wc3Reforged)) {
                            layers.push_back(&layer);
                        }
                    }
                    if (!mdx_core::StackCollapses(layers)) {
                        ++other;
                    }
                }
            }
        }
    }

    if (materials == 0) {
        SKIP("no MDX materials parsed");
    }

    std::cout << "\n=== WC3 layer-kind mix (design section 17, open question 1) ===\n"
              << "  files with materials : " << models << " of " << limit << " scanned\n"
              << "  materials            : " << materials << "\n"
              << "  classic layers only  : " << classicOnly << "\n"
              << "  HD layers only       : " << reforgedOnly << "\n"
              << "  BOTH kinds           : " << both << "  ("
              << (100.0 * static_cast<double>(both) / static_cast<double>(materials)) << "%)\n"
              << "  imported materials   : " << imported << "\n"
              << "    Composite          : " << asComposite << "\n"
              << "    Combiners          : " << asCombiners << "\n"
              << "    PBRDeferred        : " << asPbr << "\n";

    // Every material lands in exactly one of the three buckets; a material with
    // no layers at all would be in none of them, and that is worth catching.
    CHECK(classicOnly + reforgedOnly + both == materials);
    // And every imported material landed in the kind §7.2.6 names for it, with
    // `Combiners` only where the collapse was blend-equivalent.
    CHECK(other == 0);
    CHECK(asComposite + asCombiners + asPbr == imported);
}
