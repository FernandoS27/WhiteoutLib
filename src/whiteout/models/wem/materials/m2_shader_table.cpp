// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "m2_shader_table.h"

namespace whiteout {
namespace models {
namespace wem {
namespace m2_core {

namespace {

using PS = M2PixelShader;
constexpr CombinerOp kSeed = CombinerOp::Opaque;
constexpr CombinerOp kMod = CombinerOp::Mod;
constexpr CombinerOp kMod2x = CombinerOp::Mod2x;
constexpr CombinerOp kAdd = CombinerOp::Add;
constexpr CombinerOp kAddA = CombinerOp::AddAlpha;
constexpr CombinerOp kFade = CombinerOp::Fade;
constexpr CombinerOp kPass = CombinerOp::Pass;

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

/// One row per pixel shader, read off the shader **bodies** rather than off the
/// names — `renderer/shaders/m2_combiners.slang`, which transcribes the fifteen
/// wowdev.wiki publishes and derives the rest from the naming grammar.
///
/// Reading the names instead is what the first version of this table did, and a
/// body settles two things the names get wrong:
///
/// - **A unit never replaces.** `Combiners_Opaque_Opaque` is
///   `c.rgb * t0.rgb * t1.rgb`, not "t1 wins": in World of Warcraft the word
///   `Opaque` says the unit contributes **no alpha**, and its colour multiplies
///   in either way. So `rgb[0]` is the seed on every row and `rgb[i>0]` is only
///   ever a fold. A mid-chain `Opaque` reached MDX as `FilterMode::None`, which
///   is a fresh opaque draw, and the units before it stopped existing.
/// - **The `_Alpha` family's mask is unit 0's alpha, not unit 1's.**
///   `lerp(t1.rgb * 2, 1, t0.a)`: wherever the base map is opaque — which is
///   most of a shipped base map — the second unit contributes **nothing**. MDX
///   has no pass that reads another pass's alpha, so those stages are `Pass`:
///   the identity is the value they carry over most of the surface, and drawing
///   the environment map unmasked over all of it turned Kil'jaeden's red skin
///   tan and washed out the Lich's gem.
///
/// The modifiers stay in `unexpressed`, which the converter reports once:
///
///   `NA`          the unit does not touch alpha, so its alpha op is `Pass`
///   `_Alpha`      unit 0's alpha masks the unit before it
///   `_Wgt`        the contribution is scaled by a batch weight
///   `_3s`         a third sampler replaces the `1` the mask lerps toward
///   `Dual_Crossfade`, `Masked_...`  an interpolation by batch weights
///   `Depth`       a depth-driven edge fade, which needs the EDGF chunk
constexpr Chain kChains[static_cast<std::size_t>(PS::Count)] = {
    // 0  Combiners_Opaque                 c*t0                      | c.a
    {1, {kSeed}, {kPass}, nullptr},
    // 1  Combiners_Mod                    c*t0                      | c.a*t0.a
    {1, {kSeed}, {kMod}, nullptr},
    // 2  Combiners_Opaque_Mod             c*t0*t1                   | c.a*t1.a
    {2, {kSeed, kMod}, {kPass, kMod}, nullptr},
    // 3  Combiners_Opaque_Mod2x           c*t0*t1*2                 | c.a*t1.a*2
    {2, {kSeed, kMod2x}, {kPass, kMod2x}, nullptr},
    // 4  Combiners_Opaque_Mod2xNA         c*t0*t1*2                 | c.a
    {2, {kSeed, kMod2x}, {kPass, kPass}, nullptr},
    // 5  Combiners_Opaque_Opaque          c*t0*t1                   | c.a
    {2, {kSeed, kMod}, {kPass, kPass}, nullptr},
    // 6  Combiners_Mod_Mod                c*t0*t1                   | c.a*t0.a*t1.a
    {2, {kSeed, kMod}, {kMod, kMod}, nullptr},
    // 7  Combiners_Mod_Mod2x              c*t0*t1*2                 | c.a*t0.a*t1.a*2
    {2, {kSeed, kMod2x}, {kMod, kMod2x}, nullptr},
    // 8  Combiners_Mod_Add                c*t0 + t1                 | c.a*t0.a + t1.a
    {2, {kSeed, kAdd}, {kMod, kAdd}, nullptr},
    // 9  Combiners_Mod_Mod2xNA            c*t0*t1*2                 | c.a*t0.a
    {2, {kSeed, kMod2x}, {kMod, kPass}, nullptr},
    // 10 Combiners_Mod_AddNA              c*t0 + t1                 | c.a*t0.a
    {2, {kSeed, kAdd}, {kMod, kPass}, nullptr},
    // 11 Combiners_Mod_Opaque             c*t0*t1                   | c.a*t0.a
    {2, {kSeed, kMod}, {kMod, kPass}, nullptr},
    // 12 Combiners_Opaque_Mod2xNA_Alpha   c*t0*lerp(t1*2,1,t0.a)    | c.a
    {2,
     {kSeed, kPass},
     {kPass, kPass},
     "unit 1 is a mod2x masked by unit 0's alpha, and no MDX pass reads another pass's alpha"},
    // 13 Combiners_Opaque_AddAlpha        c*t0 + t1*t1.a            | c.a
    {2, {kSeed, kAddA}, {kPass, kPass}, nullptr},
    // 14 Combiners_Opaque_AddAlpha_Alpha  c*t0 + t1*t1.a*t0.a       | c.a
    {2, {kSeed, kAddA}, {kPass, kPass}, "the add is scaled by unit 0's alpha as well"},
    // 15 Combiners_Opaque_Mod2xNA_Alpha_Add   ... + t2*t2.a         | c.a
    {3,
     {kSeed, kPass, kAddA},
     {kPass, kPass, kPass},
     "unit 1 is a mod2x masked by unit 0's alpha, and no MDX pass reads another pass's alpha"},
    // 16 Combiners_Mod_AddAlpha           c*t0 + t1*t1.a            | c.a*t0.a
    {2, {kSeed, kAddA}, {kMod, kPass}, nullptr},
    // 17 Combiners_Mod_AddAlpha_Alpha     c*t0 + t1*t1.a*t0.a       | c.a*(t0.a + t1.a*t1.a)
    {2, {kSeed, kAddA}, {kMod, kAdd}, "the add is scaled by unit 0's alpha as well"},
    // 18 Combiners_Opaque_Alpha_Alpha     lerp(lerp(c*t0,t1,t1.a),c*t0,t0.a) | c.a
    {2, {kSeed, kFade}, {kPass, kPass}, "unit 0's alpha fades the blend back out"},
    // 19 Combiners_Opaque_Mod2xNA_Alpha_3s    c*t0*lerp(t1*2,t2,t0.a) | c.a
    {3,
     {kSeed, kPass, kMod},
     {kPass, kPass, kPass},
     "unit 1 is a mod2x masked by unit 0's alpha, and unit 2 is what the mask reaches instead"},
    // 20 Combiners_Opaque_AddAlpha_Wgt    c*t0 + t1*t1.a*w          | c.a
    {2, {kSeed, kAddA}, {kPass, kPass}, "the add is also scaled by a batch weight"},
    // 21 Combiners_Mod_Add_Alpha          c*t0 + t1                 | c.a*t0.a + t1.a
    {2, {kSeed, kAdd}, {kMod, kAdd}, nullptr},
    // 22 Combiners_Opaque_ModNA_Alpha     c*t0*lerp(t1,1,t0.a)      | c.a
    {2,
     {kSeed, kPass},
     {kPass, kPass},
     "unit 1 is a modulate masked by unit 0's alpha, and no MDX pass reads another pass's alpha"},
    // 23 Combiners_Mod_AddAlpha_Wgt       c*t0 + t1*t1.a*w          | c.a*t0.a
    {2, {kSeed, kAddA}, {kMod, kPass}, "the add is also scaled by a batch weight"},
    // 24 Combiners_Opaque_Mod_Add_Wgt     c*lerp(t0,t1,t1.a) + t0*w | c.a
    //
    // The one row where the body and the name disagree about how many units
    // there are — the body reads two, the name spells three — and the body is
    // one of the DERIVED ones. The name wins on the count, because reading a
    // unit the batch declared is recoverable and dropping one is not.
    {3, {kSeed, kMod, kAdd}, {kPass, kPass, kPass}, "the add is batch-weighted"},
    // 25 Combiners_Opaque_Mod2xNA_Alpha_UnshAlpha
    //    lerp(c*t0*lerp(t1*2,1,t0.a), t0, glow*t2.a)                | c.a
    {3,
     {kSeed, kPass, kPass},
     {kPass, kPass, kPass},
     "unit 1 is a mod2x masked by unit 0's alpha, and unit 2's alpha fades the whole fold back "
     "to the unshaded base"},
    // 26 Combiners_Mod_Dual_Crossfade     c*lerp(lerp(t0,t1,a),t2,b) | c.a*(the same)
    {3,
     {kSeed, kFade, kFade},
     {kMod, kFade, kFade},
     "a crossfade by two batch weights, not a fold; MDX blends by the sampled alpha instead"},
    // 27 Combiners_Opaque_Mod2xNA_Alpha_Alpha  lerp(c*t0*mask, t2, t2.a) | c.a
    {3,
     {kSeed, kPass, kFade},
     {kPass, kPass, kPass},
     "unit 1 is a mod2x masked by unit 0's alpha, and no MDX pass reads another pass's alpha"},
    // 28 Combiners_Mod_Masked_Dual_Crossfade   the crossfade, masked by t3.a
    {4,
     {kSeed, kFade, kFade, kPass},
     {kMod, kFade, kFade, kMod},
     "a crossfade by two batch weights, masked by unit 3's alpha"},
    // 29 Combiners_Opaque_Alpha           lerp(c*t0, t1, t1.a)      | c.a
    {2, {kSeed, kFade}, {kPass, kPass}, nullptr},
    // 30 Guild             c*lerp(t0, lerp(t1,t2,t2.a), t1.a)       | c.a*t0.a
    {3, {kSeed, kFade, kFade}, {kMod, kPass, kPass}, "guild tabard recolour"},
    // 31 Guild_NoBorder    c*lerp(t0,t1,t1.a)                       | c.a*t0.a
    {2, {kSeed, kFade}, {kMod, kPass}, "guild tabard recolour"},
    // 32 Guild_Opaque      the same colour                          | c.a
    {3, {kSeed, kFade, kFade}, {kPass, kPass, kPass}, "guild tabard recolour, opaque"},
    // 33 Combiners_Mod_Depth              c*t0                      | c.a*t0.a
    {1, {kSeed}, {kMod}, "a depth-driven edge fade"},
    // 34 Illum                            c*t0 + t0*t1*w            | c.a*t0.a
    {2, {kSeed, kAdd}, {kMod, kPass}, "the emissive unit multiplies unit 0 and is batch-weighted"},
    // 35 Combiners_Unnamed_35             like the _3s member above
    {3,
     {kSeed, kPass, kMod},
     {kPass, kPass, kPass},
     "unit 1 is a mod2x masked by unit 0's alpha, and unit 2 is what the mask reaches instead"},
    // 36 Combiners_Mod_Mod_Depth          c*t0*t1                   | c.a*t0.a*t1.a
    {2, {kSeed, kMod}, {kMod, kMod}, "a depth-driven edge fade"},
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
