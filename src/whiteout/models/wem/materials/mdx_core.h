// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file mdx_core.h
 * @brief WC3 material conversion, both directions (design §7.2.1, §7.2.2, §7.2.6).
 *
 * Free functions over `mdx::Material` and `wem::Material`, with no converter
 * around them, so the mapping can be exercised on hand-built records rather than
 * only on a corpus sweep. `MdxConverter` calls these at P5.
 *
 * ### One file, two profiles
 *
 * Which shaders a model may use is the rule, and it is not symmetric. A
 * Reforged (HD) model uses all four of Warcraft III's model shaders — HD,
 * Crystal, SD on HD and SD. A classic (SD) model uses SD and nothing else. So
 * one `.mdx` material can feed a `Wc3Classic` set and a `Wc3Reforged` set at
 * once: the Reforged set keeps every layer of it, and the classic set only its
 * plain SD layers. `ImportMaterial` takes the profile and filters by
 * @ref LayerAllowedIn; a material with nothing that profile may hold yields
 * nothing, which is how a classic-only file produces no Reforged set and an
 * all-HD file no classic one.
 *
 * ### What actually says "HD", measured
 *
 * `mdx::Layer::is_hd` is the field the format has for this, and **`mdx::Parser`
 * never writes it** — it is `false` on every parsed model, which makes it useless
 * on its own as the discriminator. What the parser does produce:
 *
 *   - **v1100+**: `Layer::shader`, a per-layer `ShaderType`. `HD` and `Crystal`
 *     shade as HD. `SDOnHD` does not — that is SD *content* drawn through the HD
 *     pipeline — but it is still a Reforged model's shader, never a classic one.
 *   - **v900..v1100**: no per-layer shader at all. The only signal is
 *     `Material::shader`, a string on the **material** — so below v1100 the split
 *     is per material in practice, whatever the format allows.
 *   - **v800**: never HD.
 *
 * `is_hd` is still honoured first, so a producer that does set it is believed.
 * WEM writes it into a native block as the layer's own shading — HD or not —
 * never as the set the block sits in: a Reforged block's SD layer says `false`.
 *
 * ### The first layer is not an op
 *
 * A WC3 material is N layers drawn as N blended passes — framebuffer
 * compositing, where the framebuffer is the accumulator. The **first** layer's
 * filter mode is therefore not a compositing op: it is how the whole stack meets
 * the scene. It becomes the header `blend`, the layer imports with op `Set`, and
 * only the later filter modes become `CompositeOp`s. Without that split the fold
 * and the multi-pass draw disagree on the very first pass.
 *
 * They still disagree when the header itself reads the scene — a later pass sees
 * the *scene* where the fold sees the *stack*. WEM's contract is the fold, so
 * that case imports and diagnoses rather than being silently reinterpreted.
 */

#include <string>
#include <vector>

#include <whiteout/models/mdx/structures.h>
#include <whiteout/models/wem/diagnostics.h>
#include <whiteout/models/wem/materials/material.h>
#include <whiteout/models/wem/materials/texture.h>

namespace whiteout {
namespace models {
namespace wem {
namespace mdx_core {

/// WC3's alpha-test reference. The engine's `Transparent` filter mode is a fixed
/// cut-off, not an authorable one, so it is a constant here rather than a field
/// nobody can fill.
inline constexpr f32 kTransparentAlphaRef = 0.75f;

/// How many layers a pre-1200 Reforged HD material is expected to carry — one
/// per `SlotType`, in `SlotType` order, because those versions have no
/// sub-texture array to name the slots with.
inline constexpr std::size_t kHdPositionalSlotCount = 6;

/// "there is no texture in this slot". `mdx::Layer::textureId` is a `u32` and
/// the renderer reads it as an `i32`, so this is the -1 every absent-texture
/// path already tests for. A written Reforged material should never contain one
/// — see @ref Context::stockTexture.
inline constexpr u32 kNoTexture = 0xFFFFFFFFu;

/// `Layer::textureAnimationId`'s "there is none".
///
/// The field has no separate presence bit and `mdx::Layer` defaults it to 0,
/// which is a perfectly good TXAN index — so a layer this export creates and
/// never fills in claims the model's *first* texture animation. `mdl_converter`
/// already reads and writes the sentinel; every layer written here says it too,
/// and `mdx_anim` then hands out one TXAN per animated layer instead of pouring
/// every scrolling UV in the model into entry 0.
inline constexpr u32 kNoTextureAnimation = 0xFFFFFFFFu;

/// What the converter must supply that a `mdx::Material` does not carry.
struct Context {
    /// `mdx::Model::version` — 800 | 900 | 1000 | 1100 | 1200. Decides whether
    /// HD slots are named (sub-textures) or positional.
    u32 modelVersion = 800;

    /// `mdx::Model::textures[i]` -> `Document::textures[]`. The document owns
    /// textures (§6.3), so the format's own index means nothing here. An index
    /// past the end resolves to "no texture" plus a `TextureUnresolved`.
    std::vector<u32> textureIndexMap;

    /// The document texture index for @p mdxTextureId, or `kInvalidIndex`.
    u32 toDocument(u32 mdxTextureId) const;

    /// The document's texture table, when the caller has one: Warcraft III
    /// states the address mode on the TEXTURE (`TEXS`'s flag word, kept in
    /// `TextureRef::flags`), and the import reads it there.
    const std::vector<TextureRef>* textureRefs = nullptr;

    /// The inverse, for export. Linear in the table — the map is one entry per
    /// `.mdx` texture, which is dozens, and a second index would be a second
    /// thing to keep in step.
    u32 toMdx(u32 documentTextureId) const;

    /// Where a texture the *export* needs and the document never held is put.
    ///
    /// A Reforged HD material is six slots and shipped content fills all six:
    /// across the 12,893 six-slot HD layers in `war3.w3mod` not one slot is
    /// empty. Where a material has no map of its own it names the stock neutral
    /// of that slot (@ref StockSlotTexture), and those files are not in any
    /// document — they are Warcraft III's, and the material only learns it needs
    /// one while it is being written.
    ///
    /// Entries appended here are written after the document's own textures,
    /// starting at @ref stockBase. Null is the hand-built context a test makes:
    /// an absent slot then stays `-1`, which is what this used to write always.
    std::vector<mdx::Texture>* stockTextures = nullptr;

    /// The `.mdx` texture id `(*stockTextures)[0]` will be given — that is, the
    /// number of textures the document itself contributed.
    u32 stockBase = 0;

    /// The `.mdx` texture id naming @p path, or replaceable @p replaceableId,
    /// interning it. @ref kNoTexture when there is no sink.
    u32 stockTexture(const std::string& path, u32 replaceableId = 0) const;
};

/// Warcraft III's neutral for one Reforged HD slot, as shipped content spells
/// it — see @ref Context::stockTexture. `replaceableId` is non-zero only for the
/// team colour, which no file backs.
struct StockSlotTexture {
    const char* path = "";
    u32 replaceableId = 0;
};

/// The stock neutral for @p slot. Empty for a slot Warcraft III has none for.
StockSlotTexture StockTextureFor(mdx::Layer::SlotType slot);

/// Whether @p layer of @p material shades as Reforged HD: the `HD` or `Crystal`
/// shader. See the file comment: this is not `layer.is_hd`, because nothing sets
/// that.
bool IsHdLayer(const mdx::Material& material, const mdx::Layer& layer, u32 modelVersion);

/// Whether @p layer is SD content drawn through the HD pipeline: `SDOnHD` from
/// v1100, a material named `Shader_SD_FixedFunction` below it.
bool IsSdOnHdLayer(const mdx::Material& material, const mdx::Layer& layer, u32 modelVersion);

/// Whether a @p profile material may hold @p layer. `Wc3Reforged` holds all four
/// model shaders (HD, Crystal, SD on HD, SD); `Wc3Classic` holds plain SD only.
bool LayerAllowedIn(const mdx::Material& material, const mdx::Layer& layer, u32 modelVersion,
                    ProfileId profile);

/// Whether a file of this version serves @p profile with @p material: it has a
/// layer @ref LayerAllowedIn allows, and for `Wc3Reforged` the file is a
/// Reforged-era one (v900+, where layers carry Reforged fields) or the layer
/// shades as HD. A v800 file is an SD model and declares no Reforged set.
bool HasLayersFor(const mdx::Material& material, ProfileId profile, const Context& context);

/// One `mdx::Material` -> one `wem::Material` for @p profile, with the native
/// block attached `InSync` and holding only the layers that profile may hold
/// (§7.3).
///
/// Yields a `Composite` for `Wc3Classic` — or a `Combiners` when the stack
/// collapses (§7.2.2). For `Wc3Reforged` it yields a `PBRDeferred` over the HD
/// layers, or the classic projection when the material has none: a Reforged
/// material of SD or SD-on-HD layers has no PBR slots to fill. In a stack that
/// mixes the two — no shipped material does — the SD layers are kept in the
/// block and drawn, have no ordinal, and are reported.
///
/// @p layerOrdinals, when given, is resized to `material.layers.size()` and
/// filled with the WEM ordinal each `.mdx` layer became, or `kInvalidIndex` for
/// one this profile filtered out. Animation needs it: a layer's alpha track
/// targets an **ordinal** (§10.8), and the ordinal is the layer's position in
/// the *filtered* stack, not in the file. That holds for `PBRDeferred` too,
/// whose ordinals are body slots: an HD layer sets several, a later one re-sets
/// the slots before it, so no slot tells two layers apart — and for the
/// one-layer stack nearly every model has, position 0 is its first slot anyway.
/// A layer past the last slot has no ordinal and is reported.
Material ImportMaterial(const mdx::Material& material, ProfileId profile, const Context& context,
                        Diagnostics& out, std::vector<u32>* layerOrdinals = nullptr);

/// Whether the stack collapses to a single-draw combiner chain: the first layer
/// must be opaque (filter `None`), every later layer must be `Modulate`,
/// `Modulate2x` or `Additive`, and there must be more than one layer — a single
/// layer has nothing to collapse.
///
/// An alpha-keyed first layer is deliberately ineligible: its keyed-out pixels
/// keep the scene, which the later passes then read, while one alpha-tested draw
/// discards those fragments.
bool StackCollapses(const std::vector<const mdx::Layer*>& layers);

/// The inverse of @ref ImportMaterial.
///
/// With the native block present and not `CommonEdited` (§7.1) this reproduces
/// it and the kind mapping is never exercised. Otherwise the common material is
/// projected back: `Composite` layer-by-layer, `Combiners` by the §7.2.2 inverse
/// (stage 0 -> an opaque first layer), `PBRDeferred` through the sub-texture
/// slots. `textureIndexMap` runs the other way here — document index -> `.mdx`
/// texture id.
/// @p layerOfOrdinal, when given, is resized to the material's ordinal count and
/// filled with the `.mdx` layer each ordinal became, or `kInvalidIndex` for one
/// that did not survive. It is the inverse of @ref ImportMaterial's
/// `layerOrdinals` and it is not the identity: a chain drops a stage that draws
/// nothing, and a stage that replaces the register clears every layer written
/// before it. Animation needs it -- a layer track and a UV feature both name an
/// ordinal -- and assuming the identity attached Imperius's second scrolling
/// stage to a layer index one past the end of the stack, which is to say to
/// nothing at all.
mdx::Material ExportMaterial(const Material& material, ProfileId profile, const Context& context,
                             Diagnostics& out, std::vector<u32>* layerOfOrdinal = nullptr);

} // namespace mdx_core
} // namespace wem
} // namespace models
} // namespace whiteout
