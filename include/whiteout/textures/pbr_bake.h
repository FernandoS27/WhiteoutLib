// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file pbr_bake.h
 * @brief Restating a specular/gloss material as a metal/rough one.
 *
 * The generation of art this library reads is split down the middle. StarCraft
 * II, World of Warcraft and Diablo III all light a surface from a **specular
 * colour** and a **gloss** term; Warcraft III Reforged is the only shipped
 * content among the six that is physically based, and it wants a base colour, a
 * roughness and a metalness. There is no lossless map between the two — a
 * spec/gloss material states reflectance directly and a metal/rough one infers
 * it — so the conversion is a *heuristic*, and the point of putting it here is
 * that it is one heuristic, written once, with the numbers visible.
 *
 * **What makes the crossing tractable is that Reforged's own shader is not
 * standard PBR.** `ps_ibl.slang`, reconstructed from the retail pixel shader,
 * splits the surface as
 *
 * ```
 * F0            = metalness * albedo
 * diffuseAlbedo = (1 - metalness) * albedo
 * ```
 *
 * with **no dielectric F0 floor** — the 0.04 every other engine adds is simply
 * absent. So a Reforged surface at metalness 0 has no head-on specular at all,
 * only the grazing term Schlick leaves behind, and `metalness` is not a
 * material classification but *the specular knob*. That single fact settles the
 * whole conversion: the source's specular map is not evidence to weigh, it is a
 * reflectance to reproduce, and reproducing it means solving
 *
 * ```
 * (1 - m) * albedo = diffuse        the source's diffuse response
 *       m * albedo = F0             the source's specular response
 * ```
 *
 * for both unknowns at once. Written on luminances so one scalar metalness can
 * carry it, and as a **gain on the albedo** rather than an addition to it:
 *
 * ```
 * m      = S / (D + S)
 * albedo = diffuse * (D + S) / D          with D, S the two luminances
 * ```
 *
 * because `(1 - m) * gain == 1` exactly, so every channel of the diffuse
 * survives untouched. Adding `F0` component-wise matches the same luminance and
 * drifts the hue toward the specular's — a Reaper whose specular map is blue
 * comes back blue in the shadows, where StarCraft II shows none of it. The
 * specular's own hue is lost either way, since Reforged reads `F0 = m * albedo`
 * and its highlight is the albedo's colour whatever we write.
 *
 * That is why @ref BakeBaseColor takes the specular too: half the answer lands
 * in the map this file bakes and the other half in the one beside it, and a
 * converter that writes only the metalness makes the model *darker* rather than
 * shinier, because it has taken `m` out of the diffuse and given nothing back.
 *
 * Nothing in this file does I/O or knows about a model. It takes decoded
 * textures and returns a decoded texture; the caller resolves the files, picks
 * the container and writes them.
 */

#include <optional>

#include <whiteout/common_types.h>
#include <whiteout/compatibility.h>
#include <whiteout/textures/texture.h>

namespace whiteout::textures::pbr {

// ============================================================================
// Inputs
// ============================================================================

/**
 * @brief One scalar term of the bake — a channel of a texture, or a constant.
 *
 * Absent is the common case: a StarCraft II material fills two or three of the
 * nine layers it can, and every term this file reads has a defined value when
 * its layer is missing. `constant` is that value, so a caller never has to
 * branch on presence.
 */
struct ScalarInput {
    /// Null means "not present" — @ref constant answers instead.
    const Texture* texture = nullptr;
    Channel channel = Channel::R;
    /// Read `1 - v`.
    bool invert = false;
    f32 constant = 0.0f;

    /// StarCraft II's per-layer pipeline, in the shader's own order
    /// (`ComputeLayerColorInternal`): channel select, `* alphaFactor`, invert,
    /// `* rgbMultiply + rgbAdd` — and the multiply-add applies to alpha too.
    /// @ref scale is the alphaFactor (before @ref invert); @ref postScale and
    /// @ref bias are the multiply-add (after it). The defaults are the
    /// identity, so a caller with no layer pipeline reads the raw channel.
    f32 scale = 1.0f;
    f32 postScale = 1.0f;
    f32 bias = 0.0f;

    bool present() const {
        return texture != nullptr;
    }
};

/**
 * @brief One colour term of the bake.
 *
 * `srgb` is the colour space the *samples* are in, not a request: a specular
 * map is display-referred on every source this library reads and the app binds
 * it through an sRGB view, so the value the source's own shader multiplies is
 * the decoded one, and that is what the arithmetic below is written in.
 */
struct ColorInput {
    const Texture* texture = nullptr;
    bool srgb = true;
    f32 constant[3] = {0.0f, 0.0f, 0.0f};

    /// Read this one channel into all three, the way StarCraft II's
    /// `SelectChannels` splat does — a layer's channel select and a material's
    /// `SpecularMode` both reach for it. Absent reads the texture's own RGB.
    ///
    /// An sRGB *view* decodes RGB and never alpha, so a splat of
    /// @ref Channel::A is read encoded even when @ref srgb is set. That is not
    /// a shortcut: it is what the sampler hands the shader, and assuming
    /// otherwise is the same mistake that tilted every DXT5nm normal.
    std::optional<Channel> splat;

    /// Applied after the decode: the layer's own tint times its `rgbMultiply`,
    /// then `rgbAdd`. They are not cosmetic — a specular map authored at x2 is
    /// twice the reflectance, and the environment layer that ships with the
    /// golden Adept carries its whole look in an `add`.
    f32 scale[3] = {1.0f, 1.0f, 1.0f};
    f32 bias = 0.0f;

    bool present() const {
        return texture != nullptr;
    }
};

/**
 * @brief What a specular/gloss source says a surface reflects.
 *
 * Both bakes need it and they must agree texel for texel, so it is one struct
 * the caller fills once. The two terms it resolves are the reflectance @ref
 * BakeBaseColor adds back into the albedo and @ref BakeOrm turns into a
 * metalness, and the roughness that replaces the exponent.
 */
struct SpecularReflectance {
    /// The specular colour, as the source's shader would sample it.
    ColorInput specular;

    /// The material's Blinn-Phong exponent. StarCraft II's `specularExponent`;
    /// 20 is what the engine substitutes for an unauthored zero, and 20/40/80
    /// are 97.5% of the corpus.
    f32 exponent = 20.0f;

    /// Scales @ref exponent per texel, **squared**.
    ///
    /// A StarCraft II gloss layer is a *specularity* multiplier and not a
    /// smoothness: `psmaterial.fx:579` does `specPower *= g*g`, so a mid-grey
    /// gloss texel lands at a quarter of the exponent, not half. Reading it as
    /// `1 - g` — which is what "gloss" invites — makes the glossiest texels the
    /// roughest. Absent is 1.
    ScalarInput exponentScale{nullptr, Channel::A, false, 1.0f};

    /// `hdrSpecularMultiplier`. Absent is 1.
    f32 factor = 1.0f;

    /// Whether the source dims the highlight by its own relative area —
    /// StarCraft II's `FakeEnergyConservingSpec`, which is on unless the
    /// material sets `SimulateRoughness` (76 of 13,091 measured materials do).
    /// It is a normalisation, so it belongs in the reflectance and not in the
    /// lobe; see @ref ReflectanceScale.
    bool energyConserving = true;

    /// The envio layer, restated as reflectance — 26,894 shipped materials
    /// carry one, and in Reforged the probe reflection's amplitude is
    /// `F0*lut.x + lut.y`, so a real reflection CAN cross: as an F0 bump.
    ///
    /// @ref envReflectance is the whole per-material scale — the mean linear
    /// luminance of the env map times the layer's tint (which carries
    /// `hdrEnvironmentConstant`) times the calibration constant — and
    /// @ref envMask is the per-texel EnvioMask term (absent reads its
    /// constant; a maskless envio reflects everywhere, which is what
    /// `ApplyEnv` does too). Zero reflectance disables the whole term.
    ScalarInput envMask{nullptr, Channel::A, false, 1.0f};
    f32 envReflectance = 0.0f;

    /// What lobe width the reflection reads as, where the mask is live. The
    /// source samples its env map UNBLURRED (mip 0 in the renderer), which is
    /// a sharp mirror; the flat material roughness would smear it away. The
    /// baked roughness is `min(material, this)` wherever the masked env term
    /// is significant — the one legitimate per-texel width variation.
    f32 envRoughnessCap = 1.0f;
};

/// How a decal layer folds into the albedo — M3's `layerBlendMode`, the ops
/// `CombineLayerColor` actually implements. `AddScaled` is the plain `Add`
/// (rgb weighted by the sample's alpha); `Add` is `AddNoAlpha`.
enum class DecalOp : u8 { Mod, Mod2x, AddScaled, Add, Lerp };

/**
 * @brief What one occlusion/roughness/metalness map is made of.
 *
 * The output packs Reforged's own layout, which shipped content settles:
 * `.x` occlusion, `.y` roughness, `.z` metalness, `.w` the team-colour mask.
 * (Arthas's cape reads 0.67 mean alpha and his sword 0.00; a footman's tabard
 * 0.12 and his helmet 0.00.)
 */
struct OrmRecipe {
    /// Where the roughness and the metalness both come from.
    SpecularReflectance reflectance;

    /// Ambient occlusion. Absent is 1 — unoccluded.
    ScalarInput occlusion{nullptr, Channel::R, false, 1.0f};

    /// Where the team colour lands, as the *source* states it — a blend weight
    /// on every game this reads.
    /// Absent is 0 — no team colour anywhere.
    ScalarInput teamMask;

    /// A second team mask, combined with @ref teamMask by **max**.
    ///
    /// One material can carry two team-colour mechanisms at once: StarCraft II
    /// tints the albedo through the diffuse layer's alpha *and* adds a team
    /// emissive through a `TeamColor*Add` combine op, and the two name
    /// different textures. A single packed channel has to hold both, and the
    /// union is what "this texel is team-coloured" means.
    ScalarInput teamMaskAlt;

    /// The albedo the metalness splits, and the team tint modulates.
    ///
    /// Never written to the output, and required for a metalness that means
    /// anything: `m = F0 / (F0 + albedo)` has no answer without it, and a
    /// caller that omits it gets a surface with no specular at all rather than
    /// a guess. It is read *after* the team mask lightens it, because that is
    /// the albedo @ref BakeBaseColor writes and the runtime multiplies.
    ///
    /// Never counted when choosing the output size: the
    /// occlusion/roughness/metalness map follows the material maps, not the
    /// 2048 colour map that happens to carry the mask in its alpha.
    ColorInput baseColor;

    /// The decal, folded into @ref baseColor by @ref decalOp before anything
    /// else reads it — the same composite @ref BakeBaseColor writes, because
    /// the two bakes must split the same albedo. Absent is no decal.
    ColorInput decal;
    DecalOp decalOp = DecalOp::Mod;

    /// The output size. 0 takes the largest present source, which is what keeps
    /// a 2048 specular map from being resampled down to a 512 gloss.
    u32 width = 0;
    u32 height = 0;
};

/**
 * @brief What one rewritten base colour is made of.
 *
 * The masks are the same two @ref OrmRecipe carries and are combined the same
 * way, by max; the reflectance is the same one, for the same reason. The two
 * bakes are halves of one answer and disagreeing about either input puts the
 * split in a different place in each.
 */
struct BaseColorRecipe {
    /// The source albedo. Required — there is nothing to rewrite without it.
    ColorInput baseColor;

    /// Where the team colour lands. Absent is 0 — nothing is rewritten.
    ScalarInput teamMask;

    /// A second team mask, combined with @ref teamMask by **max**.
    ScalarInput teamMaskAlt;

    /// Per-texel coverage — StarCraft II's two alpha-mask layers, multiplied
    /// (`cFinal.a = mask1.a * mask2.a`, psmaterial.fx:380). Absent is 1 —
    /// opaque — which is also what it must be: the source never read the
    /// diffuse alpha as coverage (that channel is the team mask there), so an
    /// output alpha that is not composed from these is a leak, not data.
    ScalarInput coverage1{nullptr, Channel::A, false, 1.0f};

    /// The second mask, multiplied into @ref coverage1.
    ScalarInput coverage2{nullptr, Channel::A, false, 1.0f};

    /// The decal, folded into @ref baseColor by @ref decalOp before the team
    /// lerp and the metal gain — where `CombineLayerColor` folds it. The slot
    /// map holds one base colour, so the decal crosses as PIXELS or not at
    /// all; the Marine's chest insignia was the standing casualty.
    ColorInput decal;
    DecalOp decalOp = DecalOp::Mod;

    /// The source's alpha-test cut-off, in [0,1], for a target whose own test
    /// is FIXED. Warcraft III's `Transparent` filter tests at 0.75 whatever
    /// the source tested at, so a composed coverage written as-is is over-cut
    /// wherever the source threshold was lower. Non-zero makes the bake write
    /// the test's RESULT instead — 255 where `coverage >= cutoff`, else 0 —
    /// which reproduces the source's compare exactly under any fixed ref.
    /// Zero (the default) writes the coverage continuously, which is right
    /// for a blending target.
    f32 coverageCutoff = 0.0f;

    /// What the metalness will take back out. Absent leaves the albedo alone,
    /// which is only right for a caller that is also writing metalness 0.
    SpecularReflectance reflectance;
};

// ============================================================================
// Operations
// ============================================================================

/**
 * @brief The GGX roughness that stands in for a Blinn-Phong exponent.
 *
 * `alpha = sqrt(2 / (n + 2))` is the usual lobe-width match, and Reforged's
 * `ggxNDF` is the Disney remap — it squares the roughness it is handed before
 * using it as alpha — so the value the map holds is `sqrt(alpha)`.
 *
 * The corpus lands where it should: exponent 20 (72.7% of materials) is
 * roughness 0.55, 40 is 0.47, 80 is 0.39. The old fallback — inferred from how
 * bright the specular map was — pinned StarCraft II near **0.88**, which is a
 * surface with no highlight to see.
 */
f32 RoughnessFromExponent(f32 exponent);

/**
 * @brief The Blinn-Phong exponent a GGX roughness stands in for — the exact
 *        inverse of @ref RoughnessFromExponent (`n = 2/r^4 - 2`, the fourth
 *        power because the map stores sqrt(alpha) and `ggxNDF` squares what
 *        it is handed). The reverse crossing (Reforged -> StarCraft II)
 *        recovers `specularExponent` with it.
 */
f32 ExponentFromRoughness(f32 roughness);

/**
 * @brief What one unit of authored specular is worth as an F0.
 *
 * A Blinn-Phong lobe reflects `specColor * dim * pow(NdotH, n)`. The scale
 * equates the two engines at the highlight's PEAK — Reforged's trailing
 * `PI * accum` multiplies the specular too, so its rendered direct lobe is
 * `pi*D*F*V`, whose peak is `(n+2)/8 * F0` under the matched width
 * `alpha^2 = 2/(n+2)` — giving
 *
 * ```
 * F0 = specColor * factor * dim(n) * 8 / (n + 2)
 * ```
 *
 * where `dim` is StarCraft II's own `FakeEnergyConservingSpec` polynomial
 * (`psmaterial.fx:203`), itself very nearly proportional to `n` — which is why
 * the whole scale comes out **flat across the shipped exponents**: 0.032 at
 * 20, 0.032 at 40, 0.031 at 80. A StarCraft II specular white is about a
 * thirtieth of a mirror.
 *
 * Peak-referenced rather than energy-referenced because the gray-sphere
 * harness measured it: the energy form (`8*pi/(n+8)`) rendered the exported
 * highlight at 1.8x native peak and 3.5-4.5x native integrated energy, and
 * the geometric mean of those errors equals the ratio between the two
 * references, `pi*(n+2)/(n+8)`, to within 11% at every shipped exponent.
 * GGX's fatter tail means one constant cannot match both peak and energy;
 * this one balances them (~0.75x peak, ~1.5x energy against native).
 */
f32 ReflectanceScale(f32 exponent, bool energyConserving);

/**
 * @brief Bake @p recipe into one RGBA8 occlusion/roughness/metalness map.
 *
 * The two inferred terms, stated once so a reader does not have to reverse them
 * out of the code:
 *
 * ```
 * n         = max(1, exponent * exponentScale^2)
 * roughness = RoughnessFromExponent(n)
 *
 * F0        = linear(specular) * factor * ReflectanceScale(n, ...)
 * metalness = luminance(F0) / (luminance(F0) + luminance(teamAlbedo))
 * ```
 *
 * The roughness comes from the **exponent** and not from the specular map,
 * because that is what the exponent is: a StarCraft II material's highlight is
 * the same width everywhere unless a gloss layer says otherwise, and its map
 * varies the highlight's *strength*, which is the metalness's job. Reading the
 * map as a roughness gets both terms wrong at once and is what a spec/gloss
 * converter reaches for when it has not looked at the source shader.
 *
 * It also makes the packed map compressible. Three uncorrelated channels in
 * BC3's BC1-grade RGB block share one interpolation line and cannot be fitted;
 * the artefacts land in all three, which is why a *constant* occlusion channel
 * comes back out speckled. With the roughness constant per material and the
 * occlusion usually so too, the block reduces to a line along metalness and the
 * error collapses. Shipped Reforged ORMs are all BC3 and get the same benefit
 * from the other direction — their metalness is nearly binary (69.8% below
 * 16/255, 14.1% above 240).
 *
 * The team mask is the one channel that is not a measurement but a **decision**
 * — Warcraft III's is coverage where every source this reads states a blend
 * weight — so the bake weighs the team's share against the paint under it, and
 * @ref OrmRecipe::baseColor is what it weighs against. The shading of a
 * team-coloured surface belongs in the base colour, which is @ref
 * BakeBaseColor's job.
 *
 * Sources of different sizes are sampled bilinearly at the output resolution.
 * The result is marked `Multikind` with per-channel kinds, so `generateMipmaps`
 * filters the roughness with the variance-preserving kernel and the mask with a
 * box — a plain box filter over a roughness map is how a distant surface turns
 * to mirror.
 *
 * @return The baked map, or `std::nullopt` when no source names a size and none
 *         was given.
 */
std::optional<Texture> BakeOrm(const OrmRecipe& recipe);

/**
 * @brief Rewrite an albedo so a metal/rough engine reproduces a spec/gloss
 *        one — and so a *modulated* team tint reproduces a *replaced* one.
 *
 * Two rewrites, both additive, both in linear light.
 *
 * **The metal split.** Reforged spends the albedo on the two lobes:
 * `diffuseAlbedo = (1 - m) * albedo` and `F0 = m * albedo`. Asking that those
 * equal the source's diffuse and specular scales the albedo by
 * `(D + S) / D` — up by exactly what the metalness will take out, so the
 * diffuse response is preserved channel for channel and the highlight is paid
 * for. Leaving it out is how "we added metalness and the model got darker"
 * happens. Where the art is black there is nothing to scale — `F0 = m * albedo`
 * is zero however large `m` grows — so the reflectance is added outright
 * instead, which is the only way such a texel reflects at all.
 *
 * **The team lerp.** The two engines colour a unit differently, and the
 * difference is not cosmetic. StarCraft II replaces the albedo —
 * `psmateriallayer.fx` does `lerp(teamColour, diffuse, a)`, so where `a` is low
 * the art underneath is never seen and it does not matter that it is dark.
 * Warcraft III modulates: measured on a real export, forcing the mask to 1
 * drives green and blue to zero while red holds steady, which is
 * `albedo * lerp(1, team, m)` and nothing else. So the crossing has to solve
 * for both the albedo and the mask:
 *
 * ```
 * want:  a*diffuse + (1-a)*team   ==   base * (1 - m + m*team)
 *
 * term by term:   base*(1-m) = a*diffuse        base*m = 1-a
 * their sum:      base = a*diffuse + (1-a)  =  lerp(white, diffuse, a)
 * ```
 *
 * At `a = 0` the texel is white and the swatch supplies the colour outright,
 * which is the whole point of a replaceable; as `a` rises the art's own
 * darkness comes back through, and it is that variation the runtime multiplies
 * to shade the tint. **Writing only the mask is what "the team colour is barely
 * there" looks like** — StarCraft II art under a mask averages 26/255 against a
 * Reforged footman's 93, because StarCraft II was going to replace it, and a
 * modulate over 26/255 is nearly black.
 *
 * Where neither term applies nothing moves. The lerp and the add are done in
 * linear light and re-encoded into the space @ref ColorInput::srgb names,
 * because that is where the engine's own lerp happens.
 *
 * The alpha is the composed coverage. StarCraft II never read the diffuse
 * alpha as coverage — its alpha-mask layers are the coverage, and Warcraft III
 * reads `albedo.w` — so the output alpha is `coverage1 * coverage2` and nothing
 * else. With no masks in the recipe that is opaque, which keeps a team mask
 * from becoming holes; with them it is the cutout the source actually drew,
 * which no unbaked export could carry at all (the mask lives in its own
 * texture there, and Reforged has no slot for it).
 *
 * @return The rewritten albedo as RGBA8, or `std::nullopt` if the base colour
 *         is absent or empty.
 */
std::optional<Texture> BakeBaseColor(const BaseColorRecipe& recipe);

/**
 * @brief Rewrite one Reforged team texel so StarCraft II's *replaced* team
 *        tint reproduces Reforged's *blended* one — the team half of
 *        @ref BakeBaseColor run the other way.
 *
 * Reforged neither modulates nor replaces. Its multi-layer material blends
 * hue and brightness apart (`effects/multi_layer.slang`): the hue moves to
 * the team's by `sqrt(w)`, and the brightness — the peak channel — stays the
 * art's own unless the team colour is darker. For a team colour at least as
 * bright as the texel, which every stock swatch but the browns and greys is,
 *
 * ```
 * blended = lerp(base, teamHue * peak, sqrt(w)),    peak = max(base.rgb)
 * ```
 *
 * StarCraft II replaces — `lerp(team, diffuse, a)` (`psmateriallayer.fx`) —
 * a flat plate wherever `a` is low, and writing the mask over as `1 - w` is
 * exactly what "the team colour has no shading" looks like. Matching the two
 * term by term,
 *
 * ```
 * 1 - a   = sqrt(w) * peak
 * diffuse = (1 - sqrt(w)) * base / a
 * ```
 *
 * so the team's share is the art's brightness, and the shading painted under
 * the mask comes through as shades of the team colour, which is what Reforged
 * shows. A texel Reforged cannot tint — black under the mask — crosses as no
 * team at all, which is what it shows too. Both blends happen in linear
 * light, so the solve does; @p srgb names the space @p albedo is in and @p out
 * is written in, as @ref ColorInput::srgb does.
 *
 * @param albedo  The base colour texel.
 * @param weight  Reforged's blend weight (ORM.w, the team mask): high = team.
 * @param srgb    Whether @p albedo is encoded, and @p out is to be.
 * @param out     The texel StarCraft II reads.
 * @return The alpha to write @p out under — StarCraft II's mask, low = team.
 */
f32 TeamReplaceFromBlend(const f32 albedo[3], f32 weight, bool srgb, f32 out[3]);

/**
 * @brief Sum two additive emissive layers into one map.
 *
 * StarCraft II folds both emissive layers into one accumulator before
 * `fEmissiveMultiplier`; Reforged has one emissive slot. When both layers are
 * textured and additive the composite is a plain (alpha-weighted) add, and
 * only baked pixels can say it. `weightByAlpha` is per layer: M3's `Add` op
 * contributes `rgb * a`, `AddNoAlpha` contributes `rgb`.
 *
 * The output is sized to the larger source, sRGB like its inputs, alpha 255.
 *
 * @return The summed map, or `std::nullopt` when neither input has a texture.
 */
std::optional<Texture> BakeEmissiveSum(const ColorInput& first, bool firstWeightByAlpha,
                                       const ColorInput& second, bool secondWeightByAlpha);

/**
 * @brief How a restated normal's two components are laid out.
 *
 * Both are properties of the *art*, not of either container, which is why they
 * are options and not constants: the packing is fixed (x in alpha, y in green
 * on the way in; x in red, y in green on the way out) and everything past that
 * is a convention the two engines happen to disagree on.
 */
struct NormalRestatement {
    /// Write the source's **y** into red and its **x** into green.
    bool swapXY = false;
    /// Write `1 - y` — the green channel of the *result*, whichever source
    /// component @ref swapXY put there.
    bool invertY = false;
};

/**
 * @brief Restate a DXT5nm normal map as a two-channel one.
 *
 * StarCraft II packs a tangent-space normal the way DXT5nm does — **x in
 * alpha, y in green** — because DXT5's alpha block is the one part of the
 * format that survives a gradient. Its own shader decodes `.ag` and
 * reconstructs z (`psmaterial.fx` DecodeNormal). Reforged ships BC5, which is
 * x in red and y in green, so the conversion is a channel move and nothing else
 * — except the two conventions in @ref NormalRestatement, which belong to the
 * art rather than to either container.
 *
 * Z is reconstructed into blue on the way out so that mip generation has a
 * whole normal to renormalise; a caller encoding to BC5 drops it again, which
 * is free.
 *
 * @param source   Any decodable texture. BCn is decompressed first.
 * @param options  The two conventions the packing does not settle.
 * @return The converted map as RGBA8, or `std::nullopt` if @p source is empty.
 */
std::optional<Texture> ConvertNormalXInAlpha(const Texture& source,
                                             const NormalRestatement& options);

} // namespace whiteout::textures::pbr
