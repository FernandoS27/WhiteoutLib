// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// WEM v3 P3c — one `.mdx` producing two profiles over one geometry.
///
/// The mechanism is per-*layer*: `mdx::Layer::is_hd` decides which profile a
/// layer belongs to, so a single material can feed a `Wc3Classic` set and a
/// `Wc3Reforged` set at once, and each set's native block holds only its own
/// layers. That is what makes `DeriveProfile(Wc3Reforged -> Wc3Classic)` a layer
/// filter rather than a re-derivation through `CommonMaterial` (§7.3), and it is
/// the reason the two WC3 profiles share one native block type at all.
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

TEST_CASE("wem each set's native block holds only its own layers", "[wem][materials][mdx]") {
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
    CHECK(reforgedBlock.layers.size() == 6);
    for (const native::MdxLayer& layer : classicBlock.layers) {
        CHECK_FALSE(layer.isHd);
    }
    for (const native::MdxLayer& layer : reforgedBlock.layers) {
        CHECK(layer.isHd);
    }

    // The block is the format version's, not the material's: nothing in
    // `mdx::Material` says which file it came out of.
    CHECK(classicBlock.sourceVersion == 1200);
}

TEST_CASE("wem each set exports back to its own half", "[wem][materials][mdx]") {
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

    REQUIRE(classicOut.layers.size() == 2);
    REQUIRE(reforgedOut.layers.size() == 6);
    CHECK(classicOut.layers[0].filterMode == Layer::FilterMode::None);
    CHECK(classicOut.layers[1].filterMode == Layer::FilterMode::Blend);
    for (const Layer& layer : reforgedOut.layers) {
        CHECK(layer.is_hd);
    }
    // Rejoining the two halves reproduces the source layer list.
    CHECK(classicOut.layers.size() + reforgedOut.layers.size() == source.layers.size());
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

TEST_CASE("wem SDOnHD layers belong to the classic set", "[wem][materials][mdx]") {
    // SD content drawn through the HD pipeline is still SD content: its layers
    // have no PBR slots to fill, so putting them in the Reforged set would
    // produce a slot map made of guesses.
    mdx::Material source;
    Layer layer = makeLayer(Layer::FilterMode::None, false, 0);
    layer.shader = Layer::ShaderType::SDOnHD;
    source.layers.push_back(layer);
    CHECK_FALSE(mdx_core::IsHdLayer(source, source.layers[0], 1200));
    CHECK(mdx_core::HasLayersFor(source, ProfileId::Wc3Classic, makeContext()));
}

TEST_CASE("wem a classic-only material produces no Reforged set", "[wem][materials][mdx]") {
    mdx::Material source;
    source.layers.push_back(makeLayer(Layer::FilterMode::None, false, 0));
    CHECK(mdx_core::HasLayersFor(source, ProfileId::Wc3Classic, makeContext()));
    CHECK_FALSE(mdx_core::HasLayersFor(source, ProfileId::Wc3Reforged, makeContext()));
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
