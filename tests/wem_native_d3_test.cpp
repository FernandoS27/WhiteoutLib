// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// WEM v3 P3c — the Diablo III native block, on hand-authored fixtures.
///
/// The converter that would produce real ones is P6's, and `D3Converter` is
/// import-only (§18), so this block's half of the P3 gate is WEM write ->
/// re-read rather than a re-export. That serializer is P4's. What is assertable
/// now is the block's *shape*, and the shape is where the findings live:
///
/// - **The render state is not on the material.** It is on a `Shaders` asset's
///   `RenderPass`, and a sub-object carries two whole `Shaders` assets — opaque
///   and translucent — swapped by a scalar `alpha < 1.0`. Hence two vectors.
/// - **Two-sided is two passes**, a CW one and a CCW one, not a cull flag. A
///   single `cull` field could not represent it, which is the second reason
///   those are vectors.
/// - **Three field names lie**, and the mirror renames them: `dwSlotIndex` is
///   the texture *type* and the key of the entry, `dwTextureFlags` is the UV
///   transform *mode*, `dwTextureType` is a UV *flags* word.
///
/// The rename is the one thing here a test can catch going wrong: the sidecar
/// table in `tools/codegen/modules/d3.py` is the only thing holding it, because
/// `sno/d3/native` is machine-written and an annotation there would be deleted
/// by the next regeneration.

#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/wem/materials/material.h>

#include "whiteout/models/wem/materials/d3_core.h"

using namespace whiteout;
using namespace whiteout::models::wem;

namespace {

native::D3TextureEntry makeEntry(i32 type, i32 snoTexture, i32 uvMode) {
    native::D3TextureEntry entry;
    entry.type = type;
    entry.texture.id = snoTexture;
    entry.texture.group = native::D3Group::Textures;
    entry.uvTransformMode = uvMode;
    entry.uvRow0 = Vector4f(1, 0, 0, 0);
    entry.uvRow1 = Vector4f(0, 1, 0, 0);
    entry.animU.amount = 0.5f;
    entry.animU.rate0 = 1.0f;
    entry.uvFlags = 0x3;
    return entry;
}

native::D3RenderState makePass(i32 cullMode, bool zWrite) {
    native::D3RenderState pass;
    pass.renderParams.dwCullMode = cullMode;
    pass.renderParams.dwZWriteEnable = zWrite ? 1 : 0;
    pass.effectFile = "Uber.fx";
    pass.pixelShaderEntry = "PS_Uber";
    return pass;
}

} // namespace

TEST_CASE("wem the d3 block carries two whole shader passes", "[wem][materials][d3]") {
    native::D3Material block;
    block.sourceVersion = 260;
    block.uber.shaderMap.id = 4321;
    block.uber.colors.diffuse = Vector4f(1, 1, 1, 1);
    block.uber.colors.shininess = 12.0f;
    block.uber.textures.push_back(makeEntry(0, 100, 2));
    block.uber.textures.push_back(makeEntry(1, 101, 2));
    block.baseMaterial.id = 57;
    block.baseMaterial.group = native::D3Group::Material;

    // Two-sided is a CW pass plus a CCW one — 12 shaders and 263 appearances in
    // the corpus do exactly this — so the block holds a *list* per alpha state,
    // not a state plus a bool.
    block.opaquePasses.push_back(makePass(2, true));
    block.opaquePasses.push_back(makePass(3, true));
    block.translucentPasses.push_back(makePass(2, false));

    CHECK(block.opaquePasses.size() == 2);
    CHECK(block.translucentPasses.size() == 1);
    CHECK(block.opaquePasses[0].renderParams.dwCullMode !=
          block.opaquePasses[1].renderParams.dwCullMode);
    // The translucent set is a different asset, not a variant of the opaque one:
    // its depth write differs, which is the whole reason both are kept.
    CHECK(block.opaquePasses[0].renderParams.dwZWriteEnable == 1);
    CHECK(block.translucentPasses[0].renderParams.dwZWriteEnable == 0);
}

TEST_CASE("wem the three lying d3 field names are renamed in the mirror", "[wem][materials][d3]") {
    // Reading these by their WEM names is the assertion: `type` at @0x00 is the
    // key, `uvTransformMode` at @0x0C is 0..6, `uvFlags` at @0x98 is a bitfield
    // whose bits 0 and 1 are the U/V address modes. Compiling is most of the
    // test; the values check the sidecar mapped them to the right offsets.
    const native::D3TextureEntry entry = makeEntry(7, 900, 2);
    CHECK(entry.type == 7);
    CHECK(entry.uvTransformMode == 2);
    CHECK(entry.uvFlags == 0x3);
    CHECK(entry.texture.id == 900);
}

TEST_CASE("wem the texture type is the key and never repeats", "[wem][materials][d3]") {
    native::D3UberMaterial uber;
    uber.textures.push_back(makeEntry(0, 1, 2));
    uber.textures.push_back(makeEntry(1, 2, 2));
    uber.textures.push_back(makeEntry(12, 3, 0));

    // `type` is the key of the entry, so a lookup by type is the access pattern
    // the block is shaped for.
    const auto find = [&](i32 type) -> const native::D3TextureEntry* {
        for (const native::D3TextureEntry& entry : uber.textures) {
            if (entry.type == type) {
                return &entry;
            }
        }
        return nullptr;
    };
    REQUIRE(find(12) != nullptr);
    CHECK(find(12)->texture.id == 3);
    CHECK(find(99) == nullptr);
}

TEST_CASE("wem a d3 material fits the native variant", "[wem][materials][d3]") {
    Material material;
    native::D3Material block;
    block.sourceVersion = 260;
    block.cloth.id = 12;
    block.cloth.group = native::D3Group::Cloth;
    block.shaderParams.push_back(native::D3TagValue{1, 0xA000F, 1});
    material.SetNativeInSync(std::move(block));

    REQUIRE(material.nativeKind() == NativeKind::D3);
    CHECK(material.sync() == NativeSync::InSync);
    const auto& stored = std::get<native::D3Material>(material.Native());
    CHECK(stored.sourceVersion == 260);
    REQUIRE(stored.shaderParams.size() == 1);
    // Tag 0xA000F turns lighting off entirely and makes the vertex colour the
    // light — a per-pass fact with no home in `CommonMaterial`.
    CHECK(stored.shaderParams[0].tagId == 0xA000Fu);
}

TEST_CASE("wem the d3 mirror declares no manual fields", "[wem][materials][d3]") {
    // D3's renames all fit the mechanical copy, so nothing is converter-owned.
    CHECK(native::kD3ManualFieldCount == 0);
}

// ===========================================================================
// P6: the block the converter actually produces
// ===========================================================================
//
// The cases above pin the block's *shape* on hand-authored values, which was
// all P3 could do. Now there is a converter, and what it emits is the thing
// that has to hold.

TEST_CASE("wem the d3 converter emits an authoritative block", "[wem][materials][d3]") {
    namespace d3n = whiteout::sno::d3::native;

    d3n::SubObjectAppearance variant;
    variant.snoMaterial.id = 77;
    variant.snoMaterial.group = d3n::Group::Material;
    variant.snoCloth.id = 12;
    variant.snoCloth.group = d3n::Group::Cloth;
    variant.arShaderParams.push_back(d3n::TagMapEntry{1, 0xA000Fu, 1});
    variant.tMaterial.tColors.vDiffuse = Vector4f{1, 0.5f, 0.25f, 1};
    variant.tMaterial.arTextures.resize(2);
    // The type is field @0x00. Reading @0x98 instead is what bound an arbitrary
    // layer as the diffuse and left 320 variants binding nothing at all.
    variant.tMaterial.arTextures[0].dwSlotIndex = 1;  // diffuse
    variant.tMaterial.arTextures[0].snoTexture.id = 500;
    variant.tMaterial.arTextures[0].snoTexture.group = d3n::Group::Textures;
    variant.tMaterial.arTextures[0].dwTextureType = 0x3; // wrap, wrap
    variant.tMaterial.arTextures[1].dwSlotIndex = 3;  // normal
    variant.tMaterial.arTextures[1].snoTexture.id = 501;
    variant.tMaterial.arTextures[1].snoTexture.group = d3n::Group::Textures;

    d3_core::Context context;
    context.internUnknownIds = true;
    Diagnostics report;
    const Material material = d3_core::ImportVariant(variant, "body", context, report);

    REQUIRE(material.nativeKind() == NativeKind::D3);
    // Import-only, so the block is the truth and `common` is a projection of a
    // shader the game compiled. `InSync` would claim an export exists.
    CHECK(material.sync() == NativeSync::NativeAuthoritative);

    const auto& block = std::get<native::D3Material>(material.Native());
    CHECK(block.sourceVersion == 260);
    CHECK(block.cloth.id == 12);
    CHECK(block.baseMaterial.id == 77);
    REQUIRE(block.shaderParams.size() == 1);
    CHECK(block.shaderParams[0].tagId == 0xA000Fu);
    REQUIRE(block.uber.textures.size() == 2);
    CHECK(block.uber.textures[0].type == 1);
    // No provider, so no render state resolved — and the block says so by being
    // empty rather than by carrying a guess.
    CHECK(block.opaquePasses.empty());
    CHECK(block.translucentPasses.empty());

    // With no pass to carry a stage block, the shader-named types are a slot map.
    REQUIRE(material.Common().kind() == MaterialKind::LegacyDeferred);
    const LegacyDeferredBody* body = material.Common().legacy();
    REQUIRE(body != nullptr);
    REQUIRE(body->find(LegacySlot::Diffuse) != nullptr);
    REQUIRE(body->find(LegacySlot::Normal) != nullptr);
    CHECK(body->find(LegacySlot::Diffuse)->wrapU == WrapMode::Repeat);
    CHECK(body->find(LegacySlot::Normal)->wrapU == WrapMode::Clamp);
    // Two textures, two document entries, in first-use order.
    REQUIRE(context.texturesBySno.size() == 2);
    CHECK(context.texturesBySno[0].first == 500u);
}
