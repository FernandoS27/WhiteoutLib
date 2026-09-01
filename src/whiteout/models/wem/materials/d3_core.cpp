// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "d3_core.h"

#include "../native/d3_copy.h"

#include <algorithm>
#include <cmath>

namespace whiteout {
namespace models {
namespace wem {
namespace d3_core {

namespace d3n = whiteout::sno::d3::native;

namespace {

// ---------------------------------------------------------------------------
// The shader tag vocabulary
// ---------------------------------------------------------------------------

/// `ShaderMap_ResolveShaderOpaque`'s probe order. The head is chosen by the
/// global view mode; only the tail plus its `0x30502` head is walked, and
/// `0x30500` — the last-resort base shader — is what shipped content
/// overwhelmingly carries. The MSAA band (`0x30861`) is skipped: taking its
/// program would be claiming a pass nothing here binds.
constexpr u32 kOpaqueTagChain[] = {0x30502u, 0x30850u, 0x30830u, 0x30600u, 0x30500u};

/// TAG_VS_LIGHTING, "Enable Lighting". Its neighbours are the five per-type
/// light counts; this one gates the lot, and 0 makes the vertex colour the light
/// rather than a tint.
constexpr u32 kTagLightingEnable = 0xA000Fu;

/// The Legacy stage block: three groups of six, one entry per texture stage.
/// The two combine groups track the stage count exactly (827 passes carry entry
/// 0, 659 entry 1, 435 entry 2, 255 entry 3, 78 entry 4, 25 entry 5); the op
/// group at `0xA0010 + i` does not, and is undecoded, which is why only the
/// combines are read.
constexpr u32 kTagColorCombine = 0xA0016u;
constexpr u32 kTagAlphaCombine = 0xA001Cu;
constexpr u32 kStageBlockWidth = 6;

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
 * Only the classes those two pin are decoded. Tens 3, 4, 7 and 8 are observed
 * (33, 34, 41, 43, 71, 75, 80..85) and undecoded, and the whole `0xA0010` op
 * group with them; an undecoded code reads as a replace and says so, which is
 * the one reading that cannot invent a term the original did not have.
 */
CombinerOp DecodeCombine(u32 code, const std::string& where, Diagnostics& out) {
    if (code == 0) {
        return CombinerOp::Pass;
    }
    const u32 tens = code / 10;
    const u32 units = code % 10;
    switch (tens) {
    case 0:
        return CombinerOp::Opaque; // replace
    case 1:
        return CombinerOp::Add;
    case 2:
        if (units == 4) {
            return CombinerOp::Mod2x;
        }
        if (units == 5) {
            // A x4 modulate, which the vocabulary stops one doubling short of.
            out.warn(DiagCode::LossyBlendMode,
                     where + ": combine " + std::to_string(code) +
                         " is a x4 modulate, written as mod2x");
            return CombinerOp::Mod2x;
        }
        return CombinerOp::Mod;
    default:
        out.warn(DiagCode::LossyBlendMode,
                 where + ": combine class " + std::to_string(tens) + " is undecoded (code " +
                     std::to_string(code) + "), written as a replace");
        return CombinerOp::Opaque;
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
 * Type 5 is a *substitution* worth naming: the shipped shader modulates an
 * environment reflection with it, and this slot vocabulary's nearest term is
 * specular strength. Type 8 (irradiance) and the three alpha masks have no
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
        return LegacySlot::Specular;
    case 6:
        return LegacySlot::Emissive;
    default:
        return LegacySlot::Count;
    }
}

// ---------------------------------------------------------------------------
// One texture entry -> a TextureInput
// ---------------------------------------------------------------------------

/// Bits 0 and 1 of the UV flags word are the U and V address modes, set = wrap.
/// The corpus bit population settles the field's shape on its own: bit 0 on
/// 67,577 entries, bit 1 on 66,604, bit 2 on **3** and bit 3 on 26 — a bitfield,
/// and nothing like the 62-valued enum this field was once read as.
WrapMode WrapOf(i32 uvFlags, u32 bit) {
    return ((uvFlags >> bit) & 1) != 0 ? WrapMode::Repeat : WrapMode::Clamp;
}

/**
 * @brief The static half of the entry's UV transform.
 *
 * `MatTex_BuildUvMatrix4x4` switches on the mode, 0..6, and the corpus
 * population is exactly the switch's own case list: 0 x 9,052, 1 x 32,139,
 * 2 x 159,000, 3 x 35, 5 x 14. Mode 1 is a verbatim matrix; mode 2 — 82.7% of
 * the corpus — is `scale * rotate` whose rotation and translation come from the
 * per-draw animation state, so only its scale is static and the motion becomes a
 * `UvAnimation` feature. Modes 3..6 are 49 entries in the whole corpus and are
 * not reproduced; identity is what an unreproduced one has to be.
 */
Matrix3x2f UvTransformOf(const native::D3TextureEntry& entry) {
    Matrix3x2f matrix;
    if (entry.uvTransformMode == 1) {
        matrix.m[0][0] = entry.uvRow0.x;
        matrix.m[0][1] = entry.uvRow0.y;
        matrix.m[0][2] = entry.uvRow3.x;
        matrix.m[1][0] = entry.uvRow1.x;
        matrix.m[1][1] = entry.uvRow1.y;
        matrix.m[1][2] = entry.uvRow3.y;
    } else if (entry.uvTransformMode == 2) {
        matrix.m[0][0] = entry.uvRow0.x;
        matrix.m[1][1] = entry.uvRow1.y;
    }
    return matrix;
}

TextureInput InputOf(const native::D3TextureEntry& entry, const Context& context) {
    TextureInput input;
    input.texture = context.toDocument(entry.texture);
    input.wrapU = WrapOf(entry.uvFlags, 0);
    input.wrapV = WrapOf(entry.uvFlags, 1);
    input.uvTransform = UvTransformOf(entry);
    return input;
}

bool HasUvAnimation(const native::D3TextureEntry& entry) {
    const auto live = [](const native::D3TexAnim& anim) {
        return anim.amount != 0.0f || anim.rate0 != 0.0f || anim.rate1 != 0.0f;
    };
    return live(entry.animU) || live(entry.animV) || live(entry.animRotate);
}

/// @p id must be unique within the material and is never reused: §10.8's
/// sub-tracks join a track to a feature on it, so two features sharing one
/// silently retarget an animation.
MaterialFeature UvAnimationOf(const native::D3TextureEntry& entry, u32 layer, u32 id) {
    MaterialFeature feature;
    feature.id = id;
    feature.layer = layer;
    UvAnimationFeature body;
    body.scrollRate = Vector2f{entry.animU.rate0, entry.animV.rate0};
    body.rotateRate = entry.animRotate.rate0;
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
    common.cull = params.dwCullMode == kCullNone      ? CullMode::None
                  : params.dwCullMode == kCullClockwise ? CullMode::Front
                                                        : CullMode::Back;
    common.depth.write = params.dwZWriteEnable != 0;
    common.depth.test = params.dwZFunc != 8; // D3DCMP_ALWAYS
    common.depth.bias = params.flDepthBias;
    common.alphaTestThreshold =
        params.dwAlphaTestEnable != 0 ? static_cast<f32>(params.bAlphaRef) * (1.0f / 255.0f) : 0.0f;
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
    CopyToNative(variant.tMaterial, block.uber);
    CopyToNative(variant.snoMaterial, block.baseMaterial);
    CopyToNative(variant.snoCloth, block.cloth);
    for (const d3n::TagMapEntry& entry : variant.arShaderParams) {
        native::D3TagValue value;
        CopyToNative(entry, value);
        block.shaderParams.push_back(value);
    }

    // --- the render state, which is on another asset -------------------------
    //
    // Two `Shaders` assets in the shipped record, opaque and translucent, and
    // the runtime swaps them on a scalar `alpha < 1.0`. Only the opaque chain is
    // recovered (`ShaderMap_ResolveShaderOpaque`); the translucent resolution's
    // tag chain is not, so `translucentPasses` stays empty rather than being
    // filled with a copy of the opaque one.
    if (context.assets != nullptr && block.uber.shaderMap.id >= 0) {
        const d3n::ShaderMap* map = context.assets->shaderMap(block.uber.shaderMap.id);
        if (map == nullptr) {
            out.warn(DiagCode::AssetUnresolved,
                     name + ": shader map " + std::to_string(block.uber.shaderMap.id) +
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

    const native::D3MaterialColors& colors = block.uber.colors;

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
        for (const native::D3TextureEntry& entry : block.uber.textures) {
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

    if (first != nullptr && findPassTag(kTagColorCombine) != nullptr) {
        // --- Combiners: the stage block IS an ordered chain -------------------
        CombinersBody body;
        body.diffuseFactor = colors.diffuse;
        body.emissiveFactor = colors.emissive;
        for (std::size_t s = 0; s < first->textureStages.size() && s < kStageBlockWidth; ++s) {
            const i32 type = first->textureStages[s].contentStage;
            if (type == 0) {
                continue; // an unused stage
            }
            entryOfType = findEntry(type);
            CombinerStage stage;
            if (entryOfType != nullptr) {
                stage.input = InputOf(*entryOfType, context);
                if (HasUvAnimation(*entryOfType)) {
                    common.features.push_back(
                        UvAnimationOf(*entryOfType, static_cast<u32>(body.stages.size()),
                                      static_cast<u32>(common.features.size())));
                }
            } else {
                out.warn(DiagCode::TextureUnresolved,
                         name + ": pass declares stage type " + std::to_string(type) +
                             ", which the material has no entry for");
            }
            const native::D3ShaderTagValue* rgb =
                findPassTag(kTagColorCombine + static_cast<u32>(s));
            const native::D3ShaderTagValue* alpha =
                findPassTag(kTagAlphaCombine + static_cast<u32>(s));
            const std::string where = name + " stage " + std::to_string(s);
            stage.rgb = rgb != nullptr ? DecodeCombine(rgb->value, where, out) : CombinerOp::Opaque;
            stage.alpha =
                alpha != nullptr ? DecodeCombine(alpha->value, where, out) : CombinerOp::Opaque;
            body.stages.push_back(std::move(stage));
        }
        common.body = std::move(body);
    } else {
        // --- LegacyDeferred: the shader-named types are a slot map -----------
        LegacyDeferredBody body;
        body.diffuseFactor = colors.diffuse;
        body.emissiveFactor = colors.emissive;
        body.specularFactor = colors.specular;
        body.specularExponent = colors.shininess;
        for (const native::D3TextureEntry& entry : block.uber.textures) {
            if (!declared(entry.type)) {
                continue;
            }
            const LegacySlot slot = SlotOfType(entry.type);
            if (slot == LegacySlot::Count) {
                out.warn(DiagCode::LayerDropped,
                         name + ": texture type " + std::to_string(entry.type) +
                             " has no legacy slot");
                continue;
            }
            if (body.find(slot) != nullptr) {
                out.warn(DiagCode::MaterialBodyInvalid,
                         name + ": two entries feed " + std::string(ToString(slot)));
                continue;
            }
            const u32 layer = static_cast<u32>(body.slots.size());
            body.set(slot, InputOf(entry, context));
            if (HasUvAnimation(entry)) {
                common.features.push_back(
                    UvAnimationOf(entry, layer, static_cast<u32>(common.features.size())));
            }
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
