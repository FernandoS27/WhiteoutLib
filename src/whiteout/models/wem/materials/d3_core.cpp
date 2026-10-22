// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "d3_core.h"

#include "../native/d3_copy.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>

namespace whiteout {
namespace models {
namespace wem {
namespace d3_core {

namespace d3n = whiteout::sno::d3::native;

namespace {

// ---------------------------------------------------------------------------
// The shader tag vocabulary
// ---------------------------------------------------------------------------

/// The PC client's opaque probe (`sub_905830`) with reflections off, no MSAA
/// and D3D11: HIGH_PP_DEFAULT, HIGH, MED. `0x30830` is CONSOLE_DEFAULT, which
/// a PC never probes, and `0x30861` the MSAA alpha-mask band. The renderer
/// walks the same chain (`kD3OpaqueTagChain`).
constexpr u32 kOpaqueTagChain[] = {0x30850u, 0x30600u, 0x30500u};

/// TAG_VS_LIGHTING, "Enable Lighting". Its neighbours are the five per-type
/// light counts; this one gates the lot, and 0 makes the vertex colour the light
/// rather than a tint.
constexpr u32 kTagLightingEnable = 0xA000Fu;

/// TAG_VS_EDGEALPHA, "Vertex Alpha Function": 0 the vertex attribute, else a
/// facing term in COLOR0.a -- 1 and 4 the rim `dot(-V, N)^2`, 2 and 5 its
/// inverse (the renderer's `D3PassState::edgeAlpha`).
constexpr u32 kTagEdgeAlpha = 0xA0005u;

/// The Legacy stage block: three groups of six, one entry per texture stage.
/// The two combine groups track the stage count exactly (827 passes carry entry
/// 0, 659 entry 1, 435 entry 2, 255 entry 3, 78 entry 4, 25 entry 5). The group
/// at `0xA0010 + i` is `TAG_VS_TEXCOORD{i}_FUNC`, the coordinate stage i
/// samples at (on geometry the slot is the stage), with the registry's
/// defaults where a pass leaves it out.
constexpr u32 kTagTexcoordFunc = 0xA0010u;
constexpr u32 kTagColorCombine = 0xA0016u;
constexpr u32 kTagAlphaCombine = 0xA001Cu;
constexpr u32 kStageBlockWidth = 6;
constexpr u32 kTexcoordDefault[kStageBlockWidth] = {1u, 1u, 1u, 8u, 10u, 10u};

/// The packed UV1 stream decoded with UV0's range is sixteen times the one
/// `vertexTexCoord1` decodes; texcoord codes 3 and 8 read it that way.
constexpr f32 kUv1AtUv0Range = 16.0f;

/// D3D9 blend factors, the only ones the corpus uses.
constexpr i32 kBlendZero = 1;
constexpr i32 kBlendOne = 2;
constexpr i32 kBlendSrcAlpha = 5;
constexpr i32 kBlendInvSrcAlpha = 6;
constexpr i32 kBlendFactor = 11; ///< The premultiplied family's src.

constexpr i32 kCullNone = 1;
constexpr i32 kCullClockwise = 2;

// The presence of `kTagColorCombine` on a pass *is* §7.2.6's kind test, and it
// is clean rather than probabilistic: all 855 `Legacy.fx` passes carry the stage
// block against 15 of 236 `ActorIrrad.fx`, 12 of 155 `Prop.fx` and 6 of 179
// `Scene.fx`. So the kind follows the block, never the effect file's name.
//
// Nor the *program*'s name, which is worth saying because the renderer looks
// like it disagrees: `d3_surface_table.cpp` builds its stage chain only for
// `Legacy.fx`/`ps_legacy` and falls back to the shader-named slots for the other
// 71 entry points. But it reads the block even then — `slot.channels` comes
// straight out of the combine codes — so those passes are a chain there too,
// spelled as slots. Following the program instead cost the whole chain: the base
// map of a wing is OPAQUE and its shape is one to three alpha masks
// (`d3_standard.slang`), so Malthael's wings came out as white sheets over the
// model the moment the masks stopped being stages.

// ---------------------------------------------------------------------------
// The combine alphabet
// ---------------------------------------------------------------------------

/**
 * @brief One stage's combine code -> a chain op.
 *
 * The grammar, read off the compiled programs beside the assets: **a zero is the
 * stage saying it does not touch that channel**, the **tens digit is the op
 * class** and **the units digit carries the output gain** (4 is a x2, 5 a x4).
 * Imperius's wing pass is `20 20 0 24` / `0 20 20 25` over stages `(6, 1, 12,
 * 14)` and its program is four multiplies closing on `c[0] = {4, 2}`; Cain's
 * plume is `3 3 3 10` and adds its glow. Both said so.
 *
 * The rest is the current-register reading the retail programs bear out
 * (`D3_RENDER_AUDIT/legacy.md` F3, 712 of 741 passes): the units digit of a
 * modulate names its second argument or its gain, `TFACTOR` is the draw's
 * colour and `appearanceFX.x` its pulse, both white at rest, and the classes
 * that fold the light, a second layer or an alpha tail back in have no chain op.
 * A code with no reading here is a replace and says so, the one reading that
 * cannot invent a term the original did not have.
 */
CombinerOp DecodeCombine(u32 code, const std::string& where, Diagnostics& out) {
    const u32 tens = code / 10;
    const u32 units = code % 10;
    const auto lossy = [&](const char* what, CombinerOp as) {
        out.warn(DiagCode::LossyBlendMode,
                 where + ": combine " + std::to_string(code) + " " + what);
        return as;
    };
    switch (tens) {
    case 0:
        // 0 and 1 leave the register alone and 3 selects the texture.
        if (units <= 1) {
            return CombinerOp::Pass;
        }
        if (units == 2) {
            return lossy("selects the draw colour, which no op says; left as the identity",
                         CombinerOp::Pass);
        }
        if (units == 3) {
            return CombinerOp::Opaque;
        }
        break;
    case 1:
        return CombinerOp::Add;
    case 2:
        switch (units) {
        case 2: // x the draw colour, white at rest: the texture alone.
            return CombinerOp::Opaque;
        case 3:
            return lossy("squares the texture, written as a replace", CombinerOp::Opaque);
        case 4:
        case 6:
            return CombinerOp::Mod2x;
        case 5:
        case 7:
            // A x4 modulate, which the vocabulary stops one doubling short of.
            return lossy("is a x4 modulate, written as mod2x", CombinerOp::Mod2x);
        default:
            return CombinerOp::Mod;
        }
    case 4:
        // x the light (40) or the draw colour (41), which a consumer applies to
        // the whole material already; 42 squares the register and 43
        // premultiplies it by its own alpha, neither of which an op can say.
        if (units >= 2) {
            return lossy("folds the register into itself; left as the identity",
                         CombinerOp::Pass);
        }
        return CombinerOp::Pass;
    case 7:
        // x appearanceFX.x, of the register (71) or of the texture (75).
        return units == 5 ? CombinerOp::Opaque : CombinerOp::Pass;
    case 8:
        return lossy("opens or folds a second layer or an alpha tail, which a chain has no op "
                     "for; left as the identity",
                     CombinerOp::Pass);
    default:
        break;
    }
    return lossy("is undecoded, written as a replace", CombinerOp::Opaque);
}

/// What `TAG_VS_TEXCOORD{i}_FUNC` makes of a stage's coordinate (`legacy.md`
/// F1/F2, a bijection over the joined programs): the uv set, whether the
/// entry's matrix applies, the sphere map, and UV1 read at UV0's range.
void ApplyTexcoordFunc(u32 code, TextureInput& input, bool& animates,
                       const TextureInput* stage0, const std::string& where, Diagnostics& out) {
    const auto raw = [&] {
        input.uvTransform = Matrix3x2f{};
        animates = false;
    };
    const auto uv0Range = [&] {
        for (u32 r = 0; r < 2; ++r) {
            for (u32 c = 0; c < 2; ++c) {
                input.uvTransform.m[r][c] *= kUv1AtUv0Range;
            }
        }
    };
    switch (code) {
    case 0:
        raw();
        break;
    case 2:
        raw();
        input.mapping = UVMappingMode::EnvSphereNormal;
        break;
    case 3:
        raw();
        input.uvSet = 1;
        uv0Range();
        break;
    case 8:
        input.uvSet = 1;
        uv0Range();
        break;
    case 11:
        if (stage0 != nullptr) {
            input.uvTransform = stage0->uvTransform;
        }
        break;
    case 17:
        raw();
        input.uvSet = 1;
        break;
    case 18:
        input.uvSet = 1;
        break;
    case 9:
    case 16:
        out.info(DiagCode::LossyKindConversion,
                 where + ": an eye-linear or camera-plane coordinate has no mesh spelling; "
                         "sampled at uv set 0");
        break;
    default:
        break; // 1, and 10 (no slot): the entry's matrix over uv set 0.
    }
}

// ---------------------------------------------------------------------------
// Texture types
// ---------------------------------------------------------------------------

/**
 * @brief Which legacy slot an `EMaterialTextureType` feeds, or `Count`.
 *
 * Named from the shipped programs, not guessed: a `RenderPass` carries an
 * ordered stage list whose ids are these types, and the corpus ships the OpenGL
 * builds of those programs beside the assets, each with its sampler names in
 * texture-unit order. 1 `diffuseSampler`, 2 `lightMapSampler`, 4
 * `environmentMapSampler`, 5 `glossMapSampler`, 6 `glowSampler`, 12/14/19
 * `alphaMap0/1/2Sampler`, over 285 alignments.
 *
 * Type 5 lights nothing: the retail programs weight the type-4 reflection by
 * it and draw no highlight at all (`D3_RENDER_AUDIT.md` §2.1), so it is the
 * environment's mask. Type 8 (irradiance) and the three alpha masks have no
 * legacy slot at all — a mask multiplies into alpha, which is a chain operation,
 * so a material that needs them is one the stage block was going to claim.
 */
LegacySlot SlotOfType(i32 type) {
    switch (type) {
    case 1:
        return LegacySlot::Diffuse;
    case 2:
        return LegacySlot::Lightmap;
    case 3:
    case 47:
    case 48:
    case 49:
    case 50:
    case 51:
    case 52:
        return LegacySlot::Normal;
    case 4:
        return LegacySlot::Environment;
    case 5:
        return LegacySlot::EnvironmentMask;
    case 6:
        return LegacySlot::Emissive;
    // A second base map lerped over the diffuse by its own alpha
    // (`actor2_opaque_glow_skin`: `lerp(diffuse, t25, t25.a·k)`), on 3,992 of
    // 6,920 shipped variants — the largest population this table ever dropped.
    case 25:
        return LegacySlot::Detail;
    default:
        return LegacySlot::Count;
    }
}

/// A declared type whose term is zero on a model at rest, so a slot map loses
/// nothing by leaving it out: the irradiance cube and the two shadow maps
/// (scene inputs), the overlay bank, the death layers, and the hero tint's
/// mask and ramp, which tint nothing until a dye is set.
bool RestsAtZero(i32 type) {
    return type == 8 || type == 22 || type == 23 || (type >= 26 && type <= 38) || type == 40 ||
           type == 41 || type == 54 || type == 56;
}

/// The actor families, whose secondary maps sample at the diffuse's coordinate
/// (matTex0) and whose reflection rests at half weight (`D3_RENDER_AUDIT/actor.md`
/// A2, A11).
bool ActorFamily(const native::D3RenderState& pass) {
    return pass.effectFile == "ActorIrrad.fx" || pass.effectFile == "Actor.fx";
}

/// pvdo's `gloss * env * (0.5 + 0.5 * spec)`: the point lights' specular is
/// scaled by M2, which is zero on 93.8% of records, so the weight rests at a half.
constexpr f32 kActorReflectionWeight = 0.5f;

// ---------------------------------------------------------------------------
// One texture entry -> a TextureInput
// ---------------------------------------------------------------------------

/// The UV flags word. The corpus bit population settles the field's shape on
/// its own: bit 0 on 67,577 entries, bit 1 on 66,604, bit 2 on **3** and bit 3
/// on 26 — a bitfield, and nothing like the 62-valued enum this field was once
/// read as.
///
/// It is not the sampler's address mode, though it was read as one. Bits 0 and
/// 1 draw the U and V phase at random (`MatTex_InitUvState` @0x71000F7C60) and
/// bit 3 turns the transform about the tile's centre; the address modes are
/// the pass's own, `TextureStageParams` +0x04 and +0x08 (1 clamp, 2 wrap), and a
/// type the pass does not list wraps.
constexpr i32 kUvRandomPhaseU = 0x1;
constexpr i32 kUvRandomPhaseV = 0x2;
constexpr i32 kUvAboutCentre = 0x8;

WrapMode WrapOf(const native::D3RenderState* pass, i32 type, u32 axis) {
    if (pass != nullptr) {
        for (const native::D3TextureStage& stage : pass->textureStages) {
            if (stage.contentStage == type) {
                const i32 mode = axis == 0 ? stage.dwAddressU : stage.dwAddressV;
                return mode == 2 ? WrapMode::Repeat : WrapMode::Clamp;
            }
        }
    }
    return WrapMode::Repeat;
}

/// `tAnim3.flAmount`, tested as an integer by `MatTex_TickUvStateEntry`
/// @0x71000F8310 (so `-0.0f` is set): the scroll is clamped into [0,1] instead
/// of wrapped, a one-way reveal that stops at its end.
bool ClampsUv(const native::D3TextureEntry& entry) {
    u32 bits = 0;
    std::memcpy(&bits, &entry.anim3.amount, sizeof(bits));
    return bits != 0;
}

/// The engine's per-frame fold (`MatTex_TickUvStateEntry`), whose boundary
/// test is a strict `> 1`: a whole number folds to 1.
f32 FoldUv(f32 v) {
    if (v >= 0.0f && v <= 1.0f) {
        return v;
    }
    f32 r = std::fmod(v, 1.0f);
    if (r < 0.0f) {
        r += 1.0f;
    }
    return r == 0.0f ? (v > 0.0f ? 1.0f : 0.0f) : r;
}

/// `MatTex_InitUvState`'s angle: pinned to 8 turns, then folded into one.
f32 WrapAngle(f32 a) {
    constexpr f32 kTwoPi = 6.28318530717958647692f;
    a = std::clamp(a, -kTwoPi * 8.0f, kTwoPi * 8.0f);
    while (a < 0.0f) {
        a += kTwoPi;
    }
    while (a > kTwoPi) {
        a -= kTwoPi;
    }
    return a;
}

/**
 * @brief The static half of the entry's UV transform.
 *
 * `MatTex_BuildUvMatrix4x4` switches on the mode, 0..6, and the corpus
 * population is exactly the switch's own case list: 0 x 9,052, 1 x 32,139,
 * 2 x 159,000, 3 x 35, 5 x 14. The engine feeds `(u, v, 0, 1)` from the left,
 * so `u'` takes the rows' x and `v'` their y. Mode 1 is that matrix verbatim;
 * mode 2 — 82.7% of the corpus — is `scale * rotate` with the animation state's
 * phase as the translation, so its start is static here and its motion becomes
 * a `UvAnimation` feature. Modes 3..6 are 49 entries in the whole corpus and
 * are not reproduced; identity is what an unreproduced one has to be.
 */
Matrix3x2f UvTransformOf(const native::D3TextureEntry& entry) {
    Matrix3x2f matrix;
    if (entry.uvTransformMode == 1) {
        // All zero is an entry with no authored matrix, and would collapse
        // every coordinate onto the origin.
        const bool authored = entry.uvRow0 != Vector4f{} || entry.uvRow1 != Vector4f{} ||
                              entry.uvRow3 != Vector4f{};
        if (authored) {
            matrix.m[0][0] = entry.uvRow0.x;
            matrix.m[0][1] = entry.uvRow1.x;
            matrix.m[0][2] = entry.uvRow3.x;
            matrix.m[1][0] = entry.uvRow0.y;
            matrix.m[1][1] = entry.uvRow1.y;
            matrix.m[1][2] = entry.uvRow3.y;
        }
    } else if (entry.uvTransformMode == 2) {
        // Both scales fall back to 1 when either is not positive.
        f32 sx = 1.0f;
        f32 sy = 1.0f;
        if (entry.uvRow0.x > 0.0f && entry.uvRow1.y > 0.0f) {
            sx = entry.uvRow0.x;
            sy = entry.uvRow1.y;
        }
        const f32 angle = WrapAngle(entry.animRotate.amount);
        const f32 c = std::cos(angle);
        const f32 s = std::sin(angle);
        const f32 m00 = c * sx;
        const f32 m01 = s * sx;
        const f32 m10 = -s * sy;
        const f32 m11 = c * sy;
        // The phase the state starts at: `flAmount`, or a random draw this has
        // no instance to make. A one-way reveal is held where it stops.
        f32 tu = (entry.uvFlags & kUvRandomPhaseU) != 0 ? 0.0f : entry.animU.amount;
        f32 tv = (entry.uvFlags & kUvRandomPhaseV) != 0 ? 0.0f : entry.animV.amount;
        if (ClampsUv(entry)) {
            const auto end = [](f32 phase, f32 rate) {
                return rate > 0.0f ? 1.0f : rate < 0.0f ? 0.0f : std::clamp(phase, 0.0f, 1.0f);
            };
            tu = end(tu, entry.animU.rate0);
            tv = end(tv, entry.animV.rate0);
        } else {
            tu = FoldUv(tu);
            tv = FoldUv(tv);
        }
        if ((entry.uvFlags & kUvAboutCentre) != 0) {
            const f32 ou = tu - 0.5f;
            const f32 ov = tv - 0.5f;
            tu = ou * m00 + ov * m10 + 0.5f;
            tv = ov * m11 + ou * m01 + 0.5f;
        }
        matrix.m[0][0] = m00;
        matrix.m[0][1] = m10;
        matrix.m[0][2] = tu;
        matrix.m[1][0] = m01;
        matrix.m[1][1] = m11;
        matrix.m[1][2] = tv;
    }
    return matrix;
}

TextureInput InputOf(const native::D3TextureEntry& entry, const Context& context,
                     const native::D3RenderState* pass) {
    TextureInput input;
    input.texture = context.toDocument(entry.texture);
    input.wrapU = WrapOf(pass, entry.type, 0);
    input.wrapV = WrapOf(pass, entry.type, 1);
    input.uvTransform = UvTransformOf(entry);
    return input;
}

/// Only mode 2 reads the animation state; rates on any other entry are dead
/// data. A one-way reveal's scroll stops, so only its turn can run on.
bool HasUvAnimation(const native::D3TextureEntry& entry) {
    if (entry.uvTransformMode != 2) {
        return false;
    }
    const auto moves = [](const native::D3TexAnim& anim) {
        return anim.rate0 != 0.0f || anim.rate1 != 0.0f;
    };
    return moves(entry.animRotate) || (!ClampsUv(entry) && (moves(entry.animU) || moves(entry.animV)));
}

/// @p id must be unique within the material and is never reused: §10.8's
/// sub-tracks join a track to a feature on it, so two features sharing one
/// silently retarget an animation.
/// The engine's nominal tick, and the unit every authored Diablo III UV rate is
/// in. `ActorModel_ResolveSubObjectMaterials` steps with a literal 0.016667, and
/// over 13,857 non-zero rates 99.8% are exact to three decimals once multiplied
/// by 60 against 8.6% as authored -- so the stored value is per TICK and
/// `UvAnimationFeature::scrollRate` is per SECOND. Imperius's wings are the
/// case: -0.0116667 and (-0.00666667, -0.00166667) per tick are -0.7 and
/// (-0.4, -0.1) per second, which is a flame that moves rather than one that
/// takes a minute and a half to cross its own texture.
constexpr f32 kTicksPerSecond = 60.0f;

MaterialFeature UvAnimationOf(const native::D3TextureEntry& entry, u32 layer, u32 id) {
    MaterialFeature feature;
    feature.id = id;
    feature.layer = layer;
    // `flRate1` is a per-instance jitter, `rate0 + rate1 * U`; one document
    // has no instance to draw for, so it takes the mean.
    const auto rate = [](const native::D3TexAnim& anim) {
        return (anim.rate0 + 0.5f * anim.rate1) * kTicksPerSecond;
    };
    UvAnimationFeature body;
    if (!ClampsUv(entry)) {
        body.scrollRate = Vector2f{rate(entry.animU), rate(entry.animV)};
    }
    body.rotateRate = rate(entry.animRotate);
    feature.payload = body;
    return feature;
}

// ---------------------------------------------------------------------------
// The render pass -> the kind-independent state
// ---------------------------------------------------------------------------

/**
 * @brief The blend mode a pass's factor pair spells.
 *
 * The corpus never leaves the D3D9 enums and the pairs are led by (5, 6)
 * SrcAlpha/InvSrcAlpha on 1,029 passes and (5, 2) SrcAlpha/One — additive — on
 * 283. `BLENDFACTOR` as the source is the premultiplied family, which is the
 * reason `BlendMode` has that enumerator at all.
 */
BlendMode BlendOf(const native::D3RenderParams& params, Diagnostics& out,
                  const std::string& where) {
    if (params.dwAlphaBlendEnable == 0) {
        return BlendMode::Opaque;
    }
    const i32 src = params.dwSrcBlend;
    const i32 dst = params.dwDestBlend;
    if (src == kBlendFactor) {
        return BlendMode::PremultipliedAlpha;
    }
    if (src == kBlendSrcAlpha && dst == kBlendInvSrcAlpha) {
        return BlendMode::AlphaBlend;
    }
    if (src == kBlendSrcAlpha && dst == kBlendOne) {
        return BlendMode::AdditiveAlpha;
    }
    if (src == kBlendOne && dst == kBlendOne) {
        return BlendMode::Additive;
    }
    if (src == kBlendOne && dst == kBlendInvSrcAlpha) {
        return BlendMode::PremultipliedAlpha;
    }
    if (dst == kBlendZero) {
        return BlendMode::Opaque;
    }
    out.warn(DiagCode::LossyBlendMode, where + ": blend factors (" + std::to_string(src) + ", " +
                                           std::to_string(dst) + ") have no WEM spelling");
    return BlendMode::AlphaBlend;
}

/**
 * @brief Replays one pass onto the kind-independent state.
 *
 * The alpha reference is three fields and not one: the engine passes (0, 0) to
 * the alpha-func setter when the enable is clear, so a pass carrying a reference
 * with the test off tests nothing — 194 shipped passes do exactly that, and
 * reading the reference alone cuts holes in all of them.
 */
void ApplyPass(const native::D3RenderState& pass, CommonMaterial& common, Diagnostics& out,
               const std::string& where) {
    const native::D3RenderParams& params = pass.renderParams;
    common.blend = BlendOf(params, out, where);
    // D3DCULL_CW *culls* clockwise faces, so a pass that asks for it is a pass
    // whose front faces are counter-clockwise -- `CullMode::Back`, the default
    // everywhere. Reading it as `Front` inverted the facing of every ordinary
    // Diablo III surface: the census over all 1,507 corpus `.shd` is none=713,
    // CW=1,106, CCW=12, and the twelve are the second pass of a two-sided pair,
    // never a surface that leads with a reversed winding.
    common.cull = params.dwCullMode == kCullNone        ? CullMode::None
                  : params.dwCullMode == kCullClockwise ? CullMode::Back
                                                        : CullMode::Front;
    common.depth.write = params.dwZWriteEnable != 0;
    common.depth.test = params.dwZFunc != 8; // D3DCMP_ALWAYS
    common.depth.bias = params.flDepthBias;
    common.alphaTestThreshold =
        params.dwAlphaTestEnable != 0 ? static_cast<f32>(params.bAlphaRef) * (1.0f / 255.0f) : 0.0f;

    // A pass that alpha-TESTS and writes depth is a cutout, whatever its blend
    // factors are, and `BlendMode` is the contract a surface meets the scene by
    // rather than a copy of the D3D9 state. Diablo III leaves blending on and
    // sets the reference to 192/255 on every character body -- Tyrael's hood,
    // Imperius's armour -- so reading the factors alone made them translucent:
    // Warcraft III then drew them unsorted with no depth write and the model was
    // see-through. It is the same bucketing the D3 renderer does, which keys on
    // the depth write and not on the blend enable.
    if (common.blend == BlendMode::AlphaBlend && common.alphaTestThreshold > 0.0f &&
        common.depth.write) {
        common.blend = BlendMode::AlphaKey;
    }
}

/**
 * @brief The material the engine draws for @p variant.
 *
 * `Appearance_CreateRuntimeInstance` takes the variant's Material SNO when it
 * names one — its ShaderMap, colours and texture list — and lays the embedded
 * entries of five per-instance types over it (mask 0x4800000000C0004). 96% of
 * scene sub-objects and 35 of 402 sampled actor ones name one, and the two
 * routinely differ (`D3_RENDER_AUDIT.md` §1.4). The renderer builds the same
 * (`D3RuntimeMaterial`).
 */
d3n::UberMaterial RuntimeMaterial(const d3n::SubObjectAppearance& variant,
                                  const Context& context) {
    if (context.assets == nullptr || !variant.snoMaterial.valid()) {
        return variant.tMaterial;
    }
    const d3n::Material* base = context.assets->material(variant.snoMaterial.id);
    if (base == nullptr) {
        return variant.tMaterial;
    }
    d3n::UberMaterial out = base->tMaterial;
    for (const d3n::MaterialTextureEntry& entry : variant.tMaterial.arTextures) {
        // `dwSlotIndex` is the type (`reference_d3_material_fields`).
        const i32 type = entry.dwSlotIndex;
        if (type != 2 && type != 18 && type != 19 && type != 55 && type != 58) {
            continue;
        }
        const auto same = std::find_if(out.arTextures.begin(), out.arTextures.end(),
                                       [type](const auto& e) { return e.dwSlotIndex == type; });
        if (same != out.arTextures.end()) {
            *same = entry;
        } else {
            out.arTextures.push_back(entry);
        }
    }
    return out;
}

} // namespace

// ============================================================================
// Context
// ============================================================================

u32 Context::toDocument(const native::D3AssetRef& texture) const {
    if (texture.id < 0) {
        return kInvalidIndex;
    }
    const u32 id = static_cast<u32>(texture.id);
    for (const auto& [sno, index] : texturesBySno) {
        if (sno == id) {
            return index;
        }
    }
    if (!internUnknownIds) {
        return kInvalidIndex;
    }
    const u32 index = static_cast<u32>(texturesBySno.size());
    texturesBySno.emplace_back(id, index);
    return index;
}

// ============================================================================
// ResolveShadersId
// ============================================================================

i32 ResolveShadersId(const d3n::ShaderMap& map) {
    for (const u32 tag : kOpaqueTagChain) {
        for (const d3n::ShaderMapEntry& entry : map.arShaders) {
            if (entry.dwTagId == tag && entry.snoShader.valid()) {
                return entry.snoShader.id;
            }
        }
    }
    // Nothing on the chain. Shipped maps are small and single-tagged often
    // enough that refusing here would drop real state, and a more generic
    // program is exactly what the fall-through produces anyway.
    for (const d3n::ShaderMapEntry& entry : map.arShaders) {
        if (entry.snoShader.valid()) {
            return entry.snoShader.id;
        }
    }
    return -1;
}

// ============================================================================
// ImportVariant
// ============================================================================

Material ImportVariant(const d3n::SubObjectAppearance& variant, const std::string& name,
                       const Context& context, Diagnostics& out) {
    Material material;
    material.name = name;

    // --- the native block ---------------------------------------------------
    native::D3Material block;
    block.sourceVersion = context.sourceVersion;
    // Bit 0 of this word is the per-look visibility the render-record builder
    // opens with, so it is a fact about the material and not about the section.
    block.variantFlags = variant.dwUnknown00;
    CopyToNative(variant.tMaterial, block.uber);
    CopyToNative(variant.snoMaterial, block.baseMaterial);
    CopyToNative(variant.snoCloth, block.cloth);
    for (const d3n::TagMapEntry& entry : variant.arShaderParams) {
        native::D3TagValue value;
        CopyToNative(entry, value);
        block.shaderParams.push_back(value);
    }

    // The block keeps the embedded copy, which is what the appearance holds and
    // what a round trip writes back; everything below reads the material the
    // engine draws.
    native::D3UberMaterial live;
    CopyToNative(RuntimeMaterial(variant, context), live);

    // --- the render state, which is on another asset -------------------------
    //
    // Two `Shaders` assets in the shipped record, opaque and translucent, and
    // the runtime swaps them on a scalar `alpha < 1.0`. Only the opaque chain is
    // recovered (`ShaderMap_ResolveShaderOpaque`); the translucent resolution's
    // tag chain is not, so `translucentPasses` stays empty rather than being
    // filled with a copy of the opaque one.
    if (context.assets != nullptr && live.shaderMap.id >= 0) {
        const d3n::ShaderMap* map = context.assets->shaderMap(live.shaderMap.id);
        if (map == nullptr) {
            out.warn(DiagCode::AssetUnresolved,
                     name + ": shader map " + std::to_string(live.shaderMap.id) +
                         " did not load");
        } else {
            const i32 shadersId = ResolveShadersId(*map);
            const d3n::Shaders* shaders =
                shadersId >= 0 ? context.assets->shaders(shadersId) : nullptr;
            if (shaders == nullptr) {
                out.warn(DiagCode::AssetUnresolved,
                         name + ": shader map resolved to " + std::to_string(shadersId) +
                             ", which did not load");
            } else {
                for (const d3n::RenderPass& pass : shaders->arRenderPasses) {
                    native::D3RenderState state;
                    CopyToNative(pass, state);
                    block.opaquePasses.push_back(std::move(state));
                }
            }
        }
    }

    // --- the common projection ----------------------------------------------
    CommonMaterial& common = material.InitCommon();
    // The same bit, in the layer a derive keeps. `ActorModel_BuildSubObjectRenderRecords`
    // opens each record with `subObjectAppearance[0] & 1`, and a variant without
    // it is the look's way of saying this piece is not worn.
    if ((variant.dwUnknown00 & 1) == 0) {
        common.flags |= MaterialFlags::Invisible;
    }
    const native::D3RenderState* first =
        block.opaquePasses.empty() ? nullptr : &block.opaquePasses.front();
    if (first != nullptr) {
        ApplyPass(*first, common, out, name);
        // Two-sided is two passes, a CW one and a CCW one, so a pair says
        // `None` even when neither pass alone does.
        if (block.opaquePasses.size() > 1 &&
            block.opaquePasses[1].renderParams.dwCullMode != first->renderParams.dwCullMode) {
            common.cull = CullMode::None;
        }
        const native::D3ShaderTagValue* lighting = nullptr;
        for (const native::D3ShaderTagValue& tag : first->shaderParams) {
            if (tag.tagId == kTagLightingEnable) {
                lighting = &tag;
            }
        }
        if (lighting != nullptr && lighting->value == 0) {
            common.flags |= MaterialFlags::Unlit;
        }
    }

    // MaterialColors scale the LIGHTS (`D3_RENDER_AUDIT.md` §1.3) — ambient,
    // diffuse, specular, a base ambient, the power — and are never an albedo or
    // an emissive term, so no factor below is read off them but the power and
    // the specular scale a non-actor reflection rides.
    const native::D3MaterialColors& colors = live.colors;

    // Which texture entries the resolved pass actually declares. A material
    // routinely carries types its pass never asks for -- Cain's book ships a
    // lightmap its pass does not declare -- and dropping those can only remove a
    // term the original never applied. With no pass resolved, nothing is
    // dropped.
    const auto declared = [&](i32 type) {
        if (first == nullptr || first->textureStages.empty()) {
            return true;
        }
        for (const native::D3TextureStage& stage : first->textureStages) {
            if (stage.contentStage == type) {
                return true;
            }
        }
        return false;
    };

    const native::D3TextureEntry* entryOfType = nullptr;
    const auto findEntry = [&](i32 type) -> const native::D3TextureEntry* {
        for (const native::D3TextureEntry& entry : live.textures) {
            if (entry.type == type) {
                return &entry;
            }
        }
        return nullptr;
    };

    /// The Legacy stage block rides the pass's own tag list, and the mirror
    /// keeps it verbatim, so the test runs against the mirror rather than
    /// against the parsed pass we no longer hold.
    const auto findPassTag = [&](u32 id) -> const native::D3ShaderTagValue* {
        if (first == nullptr) {
            return nullptr;
        }
        for (const native::D3ShaderTagValue& tag : first->shaderParams) {
            if (tag.tagId == id) {
                return &tag;
            }
        }
        return nullptr;
    };

    // A facing term in the vertex alpha fades a glow to nothing at its
    // silhouette. The targets have only Reforged's rim, `lerp(lit, tint,
    // w (1 - n.v)^2)`, and on an additive pass a black tint fades the same way:
    // to nothing at the edge, if later towards the middle.
    if (const native::D3ShaderTagValue* edge = findPassTag(kTagEdgeAlpha);
        edge != nullptr && edge->value != 0) {
        const bool rim = edge->value == 1 || edge->value == 4;
        const bool adds =
            common.blend == BlendMode::Additive || common.blend == BlendMode::AdditiveAlpha;
        if (rim && adds) {
            FresnelFeature fresnel;
            fresnel.color = Vector3f{0.0f, 0.0f, 0.0f};
            fresnel.exponent = 2.0f;
            fresnel.outMin = 0.0f;
            fresnel.outMax = 1.0f;
            MaterialFeature feature;
            feature.id = NextFeatureId(common.features);
            feature.layer = kWholeMaterial;
            feature.payload = fresnel;
            common.features.push_back(feature);
            out.info(DiagCode::FresnelFolded,
                     name + ": its silhouette fade is a black rim, which fades later towards the middle");
        } else {
            out.info(DiagCode::FeatureDropped,
                     name + ": its facing alpha (TAG_VS_EDGEALPHA " + std::to_string(edge->value) +
                         ") has no carrier" + (rim ? " on a blended pass" : ", an inverse rim"));
        }
    }

    if (first != nullptr && findPassTag(kTagColorCombine) != nullptr) {
        // --- Combiners: the stage block IS an ordered chain -------------------
        CombinersBody body;
        // Stage 0's input, which code 11 samples every unit at (matTex0).
        std::optional<TextureInput> stage0;
        const native::D3TextureEntry* stage0Entry = nullptr;
        if (!first->textureStages.empty()) {
            stage0Entry = findEntry(first->textureStages.front().contentStage);
            if (stage0Entry != nullptr) {
                stage0 = InputOf(*stage0Entry, context, first);
            }
        }
        for (std::size_t s = 0; s < first->textureStages.size() && s < kStageBlockWidth; ++s) {
            const i32 type = first->textureStages[s].contentStage;
            if (type == 0) {
                continue; // an unused stage
            }
            const native::D3ShaderTagValue* rgb =
                findPassTag(kTagColorCombine + static_cast<u32>(s));
            const native::D3ShaderTagValue* alpha =
                findPassTag(kTagAlphaCombine + static_cast<u32>(s));
            if (rgb == nullptr && alpha == nullptr) {
                // A stage the block gives no combine code for is not a stage of
                // the chain -- the engine's own reader skips it, and a texture
                // can be declared for a program to use some other way. Malthael's
                // wings declare six and combine four; the last two are the flow
                // maps `ps_legacy_Malthael_wings_flow` warps by, and defaulting
                // them to a replace put two of them over his own wing.
                continue;
            }
            entryOfType = findEntry(type);
            const std::string where = name + " stage " + std::to_string(s);
            CombinerStage stage;
            if (entryOfType != nullptr) {
                stage.input = InputOf(*entryOfType, context, first);
                const native::D3ShaderTagValue* func =
                    findPassTag(kTagTexcoordFunc + static_cast<u32>(s));
                const u32 code = func != nullptr ? static_cast<u32>(func->value)
                                                 : kTexcoordDefault[s];
                bool animates = HasUvAnimation(*entryOfType);
                ApplyTexcoordFunc(code, stage.input, animates, stage0 ? &*stage0 : nullptr,
                                  where, out);
                // Code 11 scrolls with stage 0, whose matrix it samples through.
                const native::D3TextureEntry* clock =
                    code == 11 && stage0Entry != nullptr ? stage0Entry : entryOfType;
                if (code == 11) {
                    animates = HasUvAnimation(*clock);
                }
                if (animates) {
                    common.features.push_back(
                        UvAnimationOf(*clock, static_cast<u32>(body.stages.size()),
                                      static_cast<u32>(common.features.size())));
                }
            } else {
                out.warn(DiagCode::TextureUnresolved,
                         name + ": pass declares stage type " + std::to_string(type) +
                             ", which the material has no entry for");
            }
            // A missing code on one channel is a zero on that channel, which
            // `DecodeCombine` reads as the identity -- the same thing the engine
            // does with an absent tag.
            stage.rgb = rgb != nullptr ? DecodeCombine(rgb->value, where, out) : CombinerOp::Pass;
            stage.alpha =
                alpha != nullptr ? DecodeCombine(alpha->value, where, out) : CombinerOp::Pass;
            // A stage with no texture samples the white constant, so a decoded
            // REPLACE would make white the register -- the vertex-colour
            // argument forms (codes 40/42/8x, the light's own entry points)
            // land exactly there, and every consumer applies the light at the
            // material level already. The identity is the one reading that
            // invents no term (D3_TO_SC2_DESIGN.md §3).
            if (entryOfType == nullptr) {
                if (stage.rgb == CombinerOp::Opaque) {
                    stage.rgb = CombinerOp::Pass;
                }
                if (stage.alpha == CombinerOp::Opaque) {
                    stage.alpha = CombinerOp::Pass;
                }
            }
            body.stages.push_back(std::move(stage));
        }
        common.body = std::move(body);
    } else {
        // --- LegacyDeferred: the shader-named types are a slot map -----------
        LegacyDeferredBody body;
        // `SpecularPower`, 0 read as 1. No program draws a highlight with it.
        body.specularExponent = colors.shininess != 0.0f ? colors.shininess : 1.0f;
        const bool actor = first != nullptr && ActorFamily(*first);
        // The actor programs sample every secondary map at the diffuse's
        // coordinate (TEXCOORD0 = matTex0), whatever its own entry says.
        const native::D3TextureEntry* diffuse = actor ? findEntry(1) : nullptr;
        for (const native::D3TextureEntry& entry : live.textures) {
            if (!declared(entry.type)) {
                continue;
            }
            const LegacySlot slot = SlotOfType(entry.type);
            if (slot == LegacySlot::Count) {
                if (RestsAtZero(entry.type)) {
                    out.info(DiagCode::LayerDropped,
                             name + ": texture type " + std::to_string(entry.type) +
                                 " contributes nothing at rest");
                } else {
                    out.warn(DiagCode::LayerDropped,
                             name + ": texture type " + std::to_string(entry.type) +
                                 " has no legacy slot");
                }
                continue;
            }
            if (body.find(slot) != nullptr) {
                out.warn(DiagCode::MaterialBodyInvalid,
                         name + ": two entries feed " + std::string(ToString(slot)));
                continue;
            }
            const u32 layer = static_cast<u32>(body.slots.size());
            TextureInput input = InputOf(entry, context, first);
            const native::D3TextureEntry* clock = &entry;
            if (slot == LegacySlot::Environment) {
                // `0.5 - 0.5 * viewN.xy`: a normal-aimed sphere map, no matrix.
                input.mapping = UVMappingMode::EnvSphereNormal;
                input.uvTransform = Matrix3x2f{};
                clock = nullptr;
            } else if (slot == LegacySlot::Lightmap && !actor) {
                // The surface programs read it at raw UV set 1 (`surface.md` 3.1).
                input.uvSet = 1;
                input.uvTransform = Matrix3x2f{};
                clock = nullptr;
            } else if (diffuse != nullptr && slot != LegacySlot::Diffuse) {
                input.uvTransform = UvTransformOf(*diffuse);
                clock = diffuse;
            }
            body.set(slot, input);
            if (clock != nullptr && HasUvAnimation(*clock)) {
                common.features.push_back(
                    UvAnimationOf(*clock, layer, static_cast<u32>(common.features.size())));
            }
        }
        // The glow enters at appearanceFX.x, which rests at 1.
        if (body.find(LegacySlot::Emissive) != nullptr) {
            body.emissiveFactor = Vector4f{1, 1, 1, 1};
        }
        // The reflection: gloss x env. An actor weights it by a half at rest;
        // the surface programs by the specular scale (M2), zero almost always.
        if (body.find(LegacySlot::Environment) != nullptr &&
            body.find(LegacySlot::EnvironmentMask) != nullptr) {
            body.environmentFactor =
                actor ? kActorReflectionWeight
                      : std::max({colors.emissive.x, colors.emissive.y, colors.emissive.z});
        }
        common.body = std::move(body);
    }

    // Import-only (§18), so the common material is a projection of a shader the
    // game compiled and the block stays the truth. `InSync` would be claiming an
    // export exists to check it against.
    material.SetNativeAuthoritative(std::move(block));
    return material;
}

} // namespace d3_core
} // namespace wem
} // namespace models
} // namespace whiteout
