// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "m2_shader_table.h"

namespace whiteout {
namespace models {
namespace wem {
namespace m2_core {

namespace {

using PS = M2PixelShader;
constexpr CombinerOp kOpaque = CombinerOp::Opaque;
constexpr CombinerOp kMod = CombinerOp::Mod;
constexpr CombinerOp kMod2x = CombinerOp::Mod2x;
constexpr CombinerOp kAdd = CombinerOp::Add;

/// The pixel column of `s_modelShaderEffect`. WEM wants ops, not shader ids, so
/// the vertex column — which only says which UV source feeds each unit — is not
/// transcribed here; `TextureInput::uvSet` carries that per unit instead.
constexpr PS kExplicitEffects[kNumShaderEffects] = {
    PS::Combiners_Opaque_Mod2xNA_Alpha,
    PS::Combiners_Opaque_AddAlpha,
    PS::Combiners_Opaque_AddAlpha_Alpha,
    PS::Combiners_Opaque_Mod2xNA_Alpha_Add,
    PS::Combiners_Mod_AddAlpha,
    PS::Combiners_Opaque_AddAlpha,
    PS::Combiners_Mod_AddAlpha,
    PS::Combiners_Mod_AddAlpha_Alpha,
    PS::Combiners_Opaque_Alpha_Alpha,
    PS::Combiners_Opaque_Mod2xNA_Alpha_3s,
    PS::Combiners_Opaque_AddAlpha_Wgt,
    PS::Combiners_Mod_Add_Alpha,
    PS::Combiners_Opaque_ModNA_Alpha,
    PS::Combiners_Mod_AddAlpha_Wgt,
    PS::Combiners_Mod_AddAlpha_Wgt,
    PS::Combiners_Opaque_AddAlpha_Wgt,
    PS::Combiners_Opaque_Mod_Add_Wgt,
    PS::Combiners_Opaque_Mod2xNA_Alpha_UnshAlpha,
    PS::Combiners_Mod_Dual_Crossfade,
    PS::Combiners_Mod_Depth,
    PS::Combiners_Opaque_Mod2xNA_Alpha_Alpha,
    PS::Combiners_Mod_Mod,
    PS::Combiners_Mod_Masked_Dual_Crossfade,
    PS::Combiners_Opaque_Alpha,
    PS::Combiners_Opaque_Mod2xNA_Alpha_UnshAlpha,
    PS::Combiners_Mod_Depth,
    PS::Guild,
    PS::Guild_NoBorder,
    PS::Guild_Opaque,
    PS::Illum,
    // 30-35, absent from 6.0.1. The corpus selects 33, 34 and 35; a 30-row
    // table sent all three down the bit-field path, which gets the edge fade
    // wrong every time.
    PS::Combiners_Unnamed_35,
    PS::Combiners_Unnamed_35,
    PS::Combiners_Opaque,
    PS::Combiners_Mod_Mod2x,
    PS::Combiners_Mod,
    PS::Combiners_Mod_Mod_Depth,
};

/// The client's two 8-entry tables, indexed by `shaderId & 7`.
constexpr PS kTwoTexture[8] = {
    PS::Combiners_Opaque_Opaque,   PS::Combiners_Opaque_Mod,      PS::Combiners_Opaque_Mod,
    PS::Combiners_Opaque_AddAlpha, PS::Combiners_Opaque_Mod2x,    PS::Combiners_Opaque_Mod,
    PS::Combiners_Opaque_Mod2xNA,  PS::Combiners_Opaque_AddAlpha,
};

constexpr PS kTwoTextureEnv[8] = {
    PS::Combiners_Mod_Opaque,  PS::Combiners_Mod_Mod,   PS::Combiners_Mod_Mod,
    PS::Combiners_Mod_Add,     PS::Combiners_Mod_Mod2x, PS::Combiners_Mod_Mod,
    PS::Combiners_Mod_Mod2xNA, PS::Combiners_Mod_AddNA,
};

constexpr const char* kNames[static_cast<std::size_t>(PS::Count)] = {
    "Combiners_Opaque",
    "Combiners_Mod",
    "Combiners_Opaque_Mod",
    "Combiners_Opaque_Mod2x",
    "Combiners_Opaque_Mod2xNA",
    "Combiners_Opaque_Opaque",
    "Combiners_Mod_Mod",
    "Combiners_Mod_Mod2x",
    "Combiners_Mod_Add",
    "Combiners_Mod_Mod2xNA",
    "Combiners_Mod_AddNA",
    "Combiners_Mod_Opaque",
    "Combiners_Opaque_Mod2xNA_Alpha",
    "Combiners_Opaque_AddAlpha",
    "Combiners_Opaque_AddAlpha_Alpha",
    "Combiners_Opaque_Mod2xNA_Alpha_Add",
    "Combiners_Mod_AddAlpha",
    "Combiners_Mod_AddAlpha_Alpha",
    "Combiners_Opaque_Alpha_Alpha",
    "Combiners_Opaque_Mod2xNA_Alpha_3s",
    "Combiners_Opaque_AddAlpha_Wgt",
    "Combiners_Mod_Add_Alpha",
    "Combiners_Opaque_ModNA_Alpha",
    "Combiners_Mod_AddAlpha_Wgt",
    "Combiners_Opaque_Mod_Add_Wgt",
    "Combiners_Opaque_Mod2xNA_Alpha_UnshAlpha",
    "Combiners_Mod_Dual_Crossfade",
    "Combiners_Opaque_Mod2xNA_Alpha_Alpha",
    "Combiners_Mod_Masked_Dual_Crossfade",
    "Combiners_Opaque_Alpha",
    "Guild",
    "Guild_NoBorder",
    "Guild_Opaque",
    "Combiners_Mod_Depth",
    "Illum",
    "Combiners_Unnamed_35",
    "Combiners_Mod_Mod_Depth",
};

/// One row per pixel shader. The name spells the chain — `Combiners_Opaque_Mod2x`
/// is stage 0 opaque, stage 1 mod2x — and everything after the second op is a
/// *modifier*, not another stage:
///
///   `NA`          the unit does not touch alpha, so its alpha op is `Opaque`
///   `_Alpha`      the unit's alpha gates the previous op
///   `_Wgt`        the contribution is weighted by a constant
///   `_3s`         a third sampler joins the same fold
///   `Dual_Crossfade`, `Masked_...`  an interpolation across units
///   `Depth`       a depth-driven edge fade
///
/// `CombinerOp` has six values and cannot say any of those, so a row that needs
/// one carries the text and the converter reports it once.
constexpr Chain kChains[static_cast<std::size_t>(PS::Count)] = {
    {1, {kOpaque}, {kOpaque}, nullptr},
    {1, {kMod}, {kMod}, nullptr},
    {2, {kOpaque, kMod}, {kOpaque, kMod}, nullptr},
    {2, {kOpaque, kMod2x}, {kOpaque, kMod2x}, nullptr},
    {2, {kOpaque, kMod2x}, {kOpaque, kOpaque}, nullptr},
    {2, {kOpaque, kOpaque}, {kOpaque, kOpaque}, nullptr},
    {2, {kMod, kMod}, {kMod, kMod}, nullptr},
    {2, {kMod, kMod2x}, {kMod, kMod2x}, nullptr},
    {2, {kMod, kAdd}, {kMod, kAdd}, nullptr},
    {2, {kMod, kMod2x}, {kMod, kOpaque}, nullptr},
    {2, {kMod, kAdd}, {kMod, kOpaque}, nullptr},
    {2, {kMod, kOpaque}, {kMod, kOpaque}, nullptr},
    {2, {kOpaque, kMod2x}, {kOpaque, kOpaque}, "unit 1's alpha gates the mod2x"},
    {2, {kOpaque, kAdd}, {kOpaque, kOpaque}, "the add is scaled by unit 1's alpha"},
    {2,
     {kOpaque, kAdd},
     {kOpaque, kOpaque},
     "the add is scaled by unit 1's alpha, and the "
     "result's alpha is unit 1's"},
    {3, {kOpaque, kMod2x, kAdd}, {kOpaque, kOpaque, kOpaque}, "unit 1's alpha gates the mod2x"},
    {2, {kMod, kAdd}, {kMod, kOpaque}, "the add is scaled by unit 1's alpha"},
    {2,
     {kMod, kAdd},
     {kMod, kOpaque},
     "the add is scaled by unit 1's alpha, and the result's "
     "alpha is unit 1's"},
    {2, {kOpaque, kOpaque}, {kOpaque, kOpaque}, "unit 1 contributes alpha only"},
    {3, {kOpaque, kMod2x, kMod}, {kOpaque, kOpaque, kOpaque}, "a third sampler joins the fold"},
    {2, {kOpaque, kAdd}, {kOpaque, kOpaque}, "the add is alpha-scaled and constant-weighted"},
    {2, {kMod, kAdd}, {kMod, kOpaque}, "the add is scaled by unit 1's alpha"},
    {2, {kOpaque, kMod}, {kOpaque, kOpaque}, "unit 1's alpha gates the modulate"},
    {2, {kMod, kAdd}, {kMod, kOpaque}, "the add is alpha-scaled and constant-weighted"},
    {3, {kOpaque, kMod, kAdd}, {kOpaque, kOpaque, kOpaque}, "the add is constant-weighted"},
    {3,
     {kOpaque, kMod2x, kMod},
     {kOpaque, kOpaque, kOpaque},
     "unit 1's alpha gates the mod2x, unshaded"},
    {3, {kMod, kMod, kMod}, {kMod, kMod, kMod}, "a crossfade across three units"},
    {3,
     {kOpaque, kMod2x, kOpaque},
     {kOpaque, kOpaque, kOpaque},
     "unit 1's alpha gates the "
     "mod2x"},
    {4,
     {kMod, kMod, kMod, kMod},
     {kMod, kMod, kMod, kMod},
     "a masked crossfade across four "
     "units"},
    {2, {kOpaque, kOpaque}, {kOpaque, kOpaque}, "unit 1 contributes alpha only"},
    {3, {kOpaque, kMod, kMod}, {kOpaque, kMod, kMod}, "guild tabard recolour"},
    {2, {kOpaque, kMod}, {kOpaque, kMod}, "guild tabard recolour"},
    {3, {kOpaque, kMod, kMod}, {kOpaque, kOpaque, kOpaque}, "guild tabard recolour, opaque"},
    {1, {kMod}, {kMod}, "a depth-driven edge fade"},
    {2, {kOpaque, kAdd}, {kOpaque, kOpaque}, "self-illumination"},
    {3,
     {kOpaque, kMod2x, kMod},
     {kOpaque, kOpaque, kOpaque},
     "a three-sampler member of the "
     "Mod2xNA_Alpha family"},
    {2, {kMod, kMod}, {kMod, kMod}, "a depth-driven edge fade"},
};

} // namespace

const char* ToString(M2PixelShader shader) {
    const auto index = static_cast<std::size_t>(shader);
    return index < static_cast<std::size_t>(PS::Count) ? kNames[index] : "?";
}

M2PixelShader PixelShaderFor(u32 textureCount, u16 shaderId, bool& outOfTable) {
    outOfTable = false;
    if (IsExplicitCombo(shaderId)) {
        const u32 row = shaderId & 0x7FFFu;
        if (row < kNumShaderEffects) {
            return kExplicitEffects[row];
        }
        // Fall THROUGH to the bit-field path rather than onto row 0. Row 0 is an
        // opaque two-texture environment combiner — the worst available guess —
        // and choosing it silently is the recorded black-box artifact.
        outOfTable = true;
    }

    if (textureCount <= 1) {
        return (shaderId & 0x70u) != 0 ? PS::Combiners_Mod : PS::Combiners_Opaque;
    }
    return ((shaderId & 0x70u) != 0 ? kTwoTextureEnv : kTwoTexture)[shaderId & 7u];
}

Chain ChainOf(M2PixelShader shader) {
    const auto index = static_cast<std::size_t>(shader);
    return index < static_cast<std::size_t>(PS::Count) ? kChains[index] : Chain{};
}

} // namespace m2_core
} // namespace wem
} // namespace models
} // namespace whiteout
