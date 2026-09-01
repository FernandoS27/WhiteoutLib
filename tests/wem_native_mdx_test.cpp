// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// WEM v3 P3c — the MDX native block round trip.
///
/// §16's P3 gate for `.mdx`: import, export, re-import, and the native blocks
/// must be identical. That is a stronger claim than "the fields parsed" — it says
/// the block is a *faithful* mirror, because a field the mirror silently dropped
/// would come back missing on the second import, and a field the copy halves got
/// backwards would come back wrong.
///
/// The mechanical copy halves are generated (§15.2), so what this really tests is
/// whether the generator's `@wem skip` / `@wem rename=` decisions were right. A
/// field wrongly skipped is exactly what this catches and the schema lock cannot.
///
/// The synthetic cases run everywhere; the corpus sweep skips when the corpus is
/// absent.

#include <cstdlib>
#include <cstring>
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

mdx_core::Context makeContext(u32 version, std::size_t textureCount) {
    mdx_core::Context context;
    context.modelVersion = version;
    for (u32 i = 0; i < static_cast<u32>(textureCount) + 8; ++i) {
        context.textureIndexMap.push_back(i);
    }
    return context;
}

/// Bit equality, not `==`. The question this file asks is "is the block the
/// same block", and a NaN — which shipped content does carry in a layer alpha —
/// compares unequal to itself under `==`, so `==` would report a round-trip
/// failure for a value that survived perfectly.
bool sameFloat(f32 a, f32 b) {
    return std::memcmp(&a, &b, sizeof(f32)) == 0;
}

bool sameInput(const native::MdxSubTexture& a, const native::MdxSubTexture& b) {
    return a.textureId == b.textureId && a.slot == b.slot;
}

bool sameLayer(const native::MdxLayer& a, const native::MdxLayer& b) {
    if (a.filterMode != b.filterMode || a.shadingFlags != b.shadingFlags ||
        a.textureId != b.textureId || a.textureAnimationId != b.textureAnimationId ||
        a.coordId != b.coordId || !sameFloat(a.alpha, b.alpha) ||
        !sameFloat(a.emissiveGain, b.emissiveGain) ||
        !sameFloat(a.fresnelOpacity, b.fresnelOpacity) ||
        !sameFloat(a.fresnelTeamColor, b.fresnelTeamColor) || a.shaderType != b.shaderType ||
        a.isHd != b.isHd || a.subTextures.size() != b.subTextures.size()) {
        return false;
    }
    if (!sameFloat(a.fresnelColor.x, b.fresnelColor.x) ||
        !sameFloat(a.fresnelColor.y, b.fresnelColor.y) ||
        !sameFloat(a.fresnelColor.z, b.fresnelColor.z)) {
        return false;
    }
    for (std::size_t i = 0; i < a.subTextures.size(); ++i) {
        if (!sameInput(a.subTextures[i], b.subTextures[i])) {
            return false;
        }
    }
    return true;
}

bool sameBlock(const native::MdxMaterial& a, const native::MdxMaterial& b) {
    if (a.sourceVersion != b.sourceVersion || a.priorityPlane != b.priorityPlane ||
        a.flags != b.flags || a.shader != b.shader || a.layers.size() != b.layers.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.layers.size(); ++i) {
        if (!sameLayer(a.layers[i], b.layers[i])) {
            return false;
        }
    }
    return true;
}

/// import -> export -> re-import, comparing the two blocks.
bool roundTrips(const mdx::Material& source, ProfileId profile, u32 version,
                std::size_t textureCount, Diagnostics& out) {
    const mdx_core::Context context = makeContext(version, textureCount);
    const Material first = mdx_core::ImportMaterial(source, profile, context, out);
    const mdx::Material exported = mdx_core::ExportMaterial(first, profile, context, out);
    const Material second = mdx_core::ImportMaterial(exported, profile, context, out);

    if (first.nativeKind() != NativeKind::Mdx || second.nativeKind() != NativeKind::Mdx) {
        return false;
    }
    return sameBlock(std::get<native::MdxMaterial>(first.Native()),
                     std::get<native::MdxMaterial>(second.Native()));
}

} // namespace

TEST_CASE("wem an mdx material survives import-export-import", "[wem][materials][mdx]") {
    mdx::Material source;
    source.priorityPlane = -3;
    source.flags = mdx::Material::Flag::TwoSided | mdx::Material::Flag::Unfogged;
    source.shader = "Shader_SD_FixedFunction";

    Layer base;
    base.filterMode = Layer::FilterMode::None;
    base.textureId = 2;
    base.coordId = 1;
    base.alpha = 0.5f;
    base.shadingFlags = Layer::ShadingFlag::WrapWidth | Layer::ShadingFlag::WrapHeight;
    source.layers.push_back(base);

    Layer overlay;
    overlay.filterMode = Layer::FilterMode::Modulate2x;
    overlay.textureId = 3;
    overlay.alpha = 1.0f;
    overlay.shadingFlags = Layer::ShadingFlag::WrapWidth | Layer::ShadingFlag::WrapHeight;
    source.layers.push_back(overlay);

    Diagnostics diagnostics;
    CHECK(roundTrips(source, ProfileId::Wc3Classic, 900, 8, diagnostics));
}

TEST_CASE("wem a Reforged sub-texture material survives the round trip", "[wem][materials][mdx]") {
    mdx::Material source;
    source.shader = "Shader_HD_DefaultUnit";

    Layer hd;
    hd.is_hd = true;
    hd.shader = Layer::ShaderType::HD;
    hd.filterMode = Layer::FilterMode::None;
    hd.emissiveGain = 2.5f;
    hd.fresnelColor = Vector3f(0.25f, 0.5f, 0.75f);
    hd.fresnelOpacity = 0.4f;
    hd.fresnelTeamColor = 0.1f;
    for (u32 slot = 0; slot < 6; ++slot) {
        Layer::SubTexture sub;
        sub.textureId = slot;
        sub.slot = static_cast<Layer::SlotType>(slot);
        hd.subTextures.push_back(sub);
    }
    source.layers.push_back(hd);

    Diagnostics diagnostics;
    CHECK(roundTrips(source, ProfileId::Wc3Reforged, 1200, 8, diagnostics));
}

TEST_CASE("wem the native block keeps a shading flag the common header drops",
          "[wem][materials][mdx]") {
    // The point of keeping a native block at all: `ShadingFlag::Unshaded` on a
    // later layer has no home in `CommonMaterial`, whose lighting decision is
    // per material. The block still has it, so a re-export does not lose it.
    mdx::Material source;
    Layer first;
    first.filterMode = Layer::FilterMode::None;
    source.layers.push_back(first);
    Layer second;
    second.filterMode = Layer::FilterMode::Modulate;
    second.shadingFlags = Layer::ShadingFlag::Unshaded;
    source.layers.push_back(second);

    Diagnostics diagnostics;
    const mdx_core::Context context = makeContext(800, 4);
    const Material imported =
        mdx_core::ImportMaterial(source, ProfileId::Wc3Classic, context, diagnostics);

    const auto& block = std::get<native::MdxMaterial>(imported.Native());
    REQUIRE(block.layers.size() == 2);
    CHECK(hasFlag(block.layers[1].shadingFlags, native::MdxShadingFlag::Unshaded));
    CHECK_FALSE(hasFlag(block.layers[0].shadingFlags, native::MdxShadingFlag::Unshaded));

    const mdx::Material exported =
        mdx_core::ExportMaterial(imported, ProfileId::Wc3Classic, context, diagnostics);
    REQUIRE(exported.layers.size() == 2);
    CHECK(hasFlag(exported.layers[1].shadingFlags, Layer::ShadingFlag::Unshaded));
}

TEST_CASE("wem the generated copy halves declare no manual fields", "[wem][materials][mdx]") {
    // Nothing in `mdx::Material` needs converter attention today, so the manual
    // list is empty — and this asserts that rather than assuming it. When a
    // field gains `@wem manual`, this fails and names the count, which is the
    // whole reason the constant is emitted.
    CHECK(native::kMdxManualFieldCount == 0);
}

TEST_CASE("wem mdx corpus native blocks round trip", "[wem][corpus][materials][mdx]") {
    const std::vector<fs::path> files =
        test::gather("WEM_MDX_CORPUS_DIR", ".mdx", {"MDL", "Wc3Mdx"});
    if (files.empty()) {
        SKIP("MDX corpus not found");
    }
    const std::size_t limit = test::sweepLimit(files.size(), 200);

    std::size_t models = 0;
    std::size_t materials = 0;
    std::vector<std::string> failures;

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

        Diagnostics diagnostics;
        const mdx_core::Context context = makeContext(model.version, model.textures.size());
        for (std::size_t m = 0; m < model.materials.size(); ++m) {
            const mdx::Material& material = model.materials[m];
            for (ProfileId profile : {ProfileId::Wc3Classic, ProfileId::Wc3Reforged}) {
                if (!mdx_core::HasLayersFor(material, profile, context)) {
                    continue;
                }
                ++materials;
                if (roundTrips(material, profile, model.version, model.textures.size(),
                               diagnostics)) {
                    continue;
                }
                if (failures.size() < 20) {
                    failures.push_back(test::pathText(files[i].filename()) + " material " +
                                       std::to_string(m) + " (" + ToString(profile) + ")");
                }
            }
        }
    }

    if (materials == 0) {
        SKIP("no MDX materials parsed");
    }

    std::cout << "\n=== MDX native round trip ===\n"
              << "  models    : " << models << " of " << limit << " scanned\n"
              << "  materials : " << materials << "\n"
              << "  failures  : " << failures.size() << "\n";
    for (const std::string& failure : failures) {
        std::cout << "    " << failure << "\n";
    }
    CHECK(failures.empty());
}
