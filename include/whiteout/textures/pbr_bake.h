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
    /// Read `1 - v`. A gloss map is a roughness map spelled backwards.
    bool invert = false;
    f32 constant = 0.0f;

    bool present() const {
        return texture != nullptr;
    }
};

/**
 * @brief One colour term of the bake.
 *
 * `srgb` is the colour space the *samples* are in, not a request: a specular
 * map is display-referred on every source this library reads, and the
 * thresholds below are linear ones, so the bake decodes before it measures.
 */
struct ColorInput {
    const Texture* texture = nullptr;
    bool srgb = true;
    f32 constant[3] = {0.0f, 0.0f, 0.0f};

    bool present() const {
        return texture != nullptr;
    }
};

/**
 * @brief What one occlusion/roughness/metalness map is made of.
 *
 * The output packs Reforged's own layout, which shipped content settles:
 * `.x` occlusion, `.y` roughness, `.z` metalness, `.w` the team-colour mask.
 * (Arthas's cape reads 0.67 mean alpha and his sword 0.00; a footman's tabard
 * 0.12 and his helmet 0.00.)
 */
struct OrmRecipe {
    /// The specular colour. Both the roughness fallback and the whole of the
    /// metalness are read off it.
    ColorInput specular;

    /// Gloss, if the material has one. Set `invert` to read it as roughness.
    ScalarInput gloss;

    /// Ambient occlusion. Absent is 1 — unoccluded.
    ScalarInput occlusion{nullptr, Channel::R, false, 1.0f};

    /// The environment (reflection) mask. Absent is 0.
    ScalarInput environmentMask;

    /// Where the team colour lands, as the *source* states it — a blend weight
    /// on every game this reads. The output is coverage; see @ref BakeOrm.
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

    /// The albedo the team tint will modulate, if the caller has it.
    ///
    /// It is never written to the output. It decides where the source's blend
    /// weight becomes coverage: a texel is the team's when the team colour is
    /// the larger part of what the source would have shown there, and how large
    /// that part is depends on how dark the paint under it is. Absent thresholds
    /// the raw weight at a half instead, which over dark art is far too strict.
    ///
    /// Never counted when choosing the output size: the
    /// occlusion/roughness/metalness map follows the material maps, not the
    /// 2048 colour map that happens to carry the mask in its alpha.
    ColorInput baseColor;

    /// The output size. 0 takes the largest present source, which is what keeps
    /// a 2048 specular map from being resampled down to a 512 gloss.
    u32 width = 0;
    u32 height = 0;
};

/**
 * @brief What one team-masked base colour is made of.
 *
 * The two masks are the same two @ref OrmRecipe carries and are combined the
 * same way, by max: the map and the albedo have to agree texel for texel or the
 * tint lands where the art was not prepared for it.
 */
struct TeamBaseColorRecipe {
    /// The source albedo. Required — there is nothing to rewrite without it.
    ColorInput baseColor;

    /// Where the team colour lands. Absent is 0 — nothing is rewritten.
    ScalarInput teamMask;

    /// A second team mask, combined with @ref teamMask by **max**.
    ScalarInput teamMaskAlt;
};

// ============================================================================
// Operations
// ============================================================================

/**
 * @brief Bake @p recipe into one RGBA8 occlusion/roughness/metalness map.
 *
 * The two inferred terms, stated once so a reader does not have to reverse them
 * out of the code:
 *
 * ```
 * specLum   = dot(linear(specular.rgb), (0.2126, 0.7152, 0.0722))
 * saturation = maxS > 1e-5 ? (maxS - minS) / maxS : 0
 *
 * roughness  = gloss present ? 1 - gloss
 *                            : 0.1 + 0.8 * pow(1 - specLum, 0.7)
 *
 * metalEvidence = smoothstep(0.08, 0.30, specLum)
 *               * (0.25 + 0.75 * smoothstep(0.05, 0.30, saturation))
 * metallic      = metalEvidence * lerp(0.5, 1.0, 1 - environmentMask)
 * ```
 *
 * The metalness reads *two* pieces of evidence because either alone is a
 * false positive: a bright grey specular is a polished dielectric as often as
 * it is metal, and a saturated dim one is a tinted highlight. A conductor is
 * bright **and** coloured, which is what the product says.
 *
 * The team mask is the one channel that is not a measurement but a **decision**.
 * Warcraft III's is coverage — over a footman's five shipped maps the alpha sits
 * 88-100% at 0..15 and 7-11% at 240..255, with 0.4-1.3% anywhere between, which
 * is an antialiased edge and nothing else — while every source this reads states
 * a blend *weight*. So the bake thresholds, and @ref OrmRecipe::baseColor is
 * what it thresholds against. The shading of a team-coloured surface belongs in
 * the base colour, which is @ref BakeTeamBaseColor's job.
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
 * @brief Rewrite an albedo so that a *modulated* team tint reproduces a
 *        *replaced* one.
 *
 * The two engines colour a unit differently, and the difference is not
 * cosmetic. StarCraft II replaces the albedo — `psmateriallayer.fx` does
 * `lerp(teamColour, diffuse, a)`, so where `a` is low the art underneath is
 * never seen and it does not matter that it is dark. Warcraft III modulates:
 * measured on a real export, forcing the mask to 1 drives green and blue to
 * zero while red holds steady, which is `albedo * lerp(1, team, m)` and nothing
 * else. So the crossing has to solve for both the albedo and the mask:
 *
 * ```
 * want:  a*diffuse + (1-a)*team   ==   base * (1 - m + m*team)
 *
 * term by term:   base*(1-m) = a*diffuse        base*m = 1-a
 * their sum:      base = a*diffuse + (1-a)  =  lerp(white, diffuse, a)
 * their ratio:    m    = (1-a) / ((1-a) + a*luminance(diffuse))
 * ```
 *
 * The mask that goes beside it is *coverage* — 1 or 0, see @ref BakeOrm — so
 * everything about a team-coloured surface except where it is has to be here.
 * That is what this lerp keeps: at `a = 0` the texel is white and the swatch
 * supplies the colour outright, which is the whole point of a replaceable; as
 * `a` rises the art's own darkness comes back through, and it is that variation
 * the runtime multiplies to shade the tint. **Writing only the mask is what
 * "the team colour is barely there" looks like** — StarCraft II art under a
 * mask averages 26/255 against a Reforged footman's 93, because StarCraft II
 * was going to replace it, and a modulate over 26/255 is nearly black.
 *
 * Where the mask is 0 nothing moves. The lerp is done in the colour space
 * @ref ColorInput::srgb names, because that is where the engine's own lerp
 * happens.
 *
 * The result is opaque. StarCraft II never read the diffuse alpha as coverage
 * — its alpha-mask layers are the coverage — and Warcraft III does, so leaving
 * a team mask there would make every team-coloured texel of an alpha-tested
 * surface a hole.
 *
 * @return The rewritten albedo as RGBA8, or `std::nullopt` if the base colour
 *         is absent or empty.
 */
std::optional<Texture> BakeTeamBaseColor(const TeamBaseColorRecipe& recipe);

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
