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
 * The classic/Reforged distinction is per **layer**, so one `.mdx` material can
 * feed a `Wc3Classic` set and a `Wc3Reforged` set at once. `ImportMaterial`
 * takes the profile and filters; a material with no layers for that profile
 * yields nothing, which is how a classic-only file produces no Reforged set.
 *
 * ### What actually says "HD", measured
 *
 * `mdx::Layer::is_hd` is the field the format has for this, and **`mdx::Parser`
 * never writes it** — it is `false` on every parsed model, which makes it useless
 * on its own as the discriminator. What the parser does produce:
 *
 *   - **v1100+**: `Layer::shader`, a per-layer `ShaderType`. `HD` and `Crystal`
 *     are the Reforged family. `SDOnHD` is not — that is SD *content* drawn
 *     through the HD pipeline, and its layers belong to the classic set.
 *   - **v900..v1100**: no per-layer shader at all. The only signal is
 *     `Material::shader`, a string on the **material** — so below v1100 the split
 *     is per material in practice, whatever the format allows.
 *   - **v800**: never HD.
 *
 * `is_hd` is still honoured first, so a producer that does set it is believed.
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

    /// The inverse, for export. Linear in the table — the map is one entry per
    /// `.mdx` texture, which is dozens, and a second index would be a second
    /// thing to keep in step.
    u32 toMdx(u32 documentTextureId) const;
};

/// Whether @p layer of @p material is a Reforged HD layer. See the file comment:
/// this is not `layer.is_hd`, because nothing sets that.
bool IsHdLayer(const mdx::Material& material, const mdx::Layer& layer, u32 modelVersion);

/// Whether @p material has any layer belonging to @p profile.
bool HasLayersFor(const mdx::Material& material, ProfileId profile, const Context& context);

/// One `mdx::Material` -> one `wem::Material` for @p profile, with the native
/// block attached `InSync` and holding only that profile's layers (§7.3).
///
/// Yields a `Composite` for `Wc3Classic` — or a `Combiners` when the stack
/// collapses (§7.2.2) — and a `PBRDeferred` for `Wc3Reforged`.
Material ImportMaterial(const mdx::Material& material, ProfileId profile, const Context& context,
                        Diagnostics& out);

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
mdx::Material ExportMaterial(const Material& material, ProfileId profile, const Context& context,
                             Diagnostics& out);

} // namespace mdx_core
} // namespace wem
} // namespace models
} // namespace whiteout
