// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file m2_material.h
 * @brief `native::M2Material` — the WoW block, authored (design §7.3).
 *
 * The other three native blocks are generated from the parser headers. This one
 * cannot be, and the reason is not a limitation of the generator: **an M2
 * material is not a record.** It is a join, resolved at draw time across a
 * `Batch`, a two-field render record, and four combo tables, none of which
 * states the join. There is nothing to mirror, so WEM stores the *resolved*
 * batch and the converter rebuilds the combo tables on export.
 *
 * What the join reads — the render-flag vocabulary, the batch record, the
 * texture record — *is* generated, into `m2_tables.h`, and the converter and
 * the validator share it.
 *
 * ### `transform` is kept and `ribbonColorIndex` is not
 *
 * `textureTransformIndex` is live data that resolves through
 * `textureTransformCombos`; `ribbonColorIndex`, sitting right beside it in the
 * file, is dead. Exactly the kind of asymmetry that is invisible unless the
 * native block keeps both sides of it, which is why `transform` is per unit here
 * rather than dropped as "probably the same thing".
 *
 * ### `blendingMode` has no enum anywhere upstream
 *
 * `m2::Material::blendingMode` is a bare `u16`. The vocabulary below is authored
 * from the client's own blend-state table rather than generated, because no m2
 * header carries it — the same reason `M2Material` is authored.
 */

#include <cstddef>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/models/wem/chunk_traits.h>
#include <whiteout/models/wem/native/m2_tables.h>

namespace whiteout {
namespace models {
namespace wem {
namespace native {

/// `m2::Material::blendingMode`. Authored: the field is a bare `u16` upstream.
enum class M2BlendingMode : u16 {
    Opaque = 0,     ///< No blending, no alpha test
    AlphaKey = 1,   ///< Alpha test at the client's fixed cut-off
    Alpha = 2,      ///< src-alpha / inv-src-alpha
    NoAlphaAdd = 3, ///< Additive, alpha ignored
    Add = 4,        ///< Additive, weighted by alpha
    Mod = 5,        ///< Multiply
    Mod2x = 6,      ///< Multiply, doubled
    BlendAdd = 7,   ///< Premultiplied-alpha add
};

/// One resolved texture unit of a batch. Every index here has already been
/// pushed through its combo table: the value is what the unit draws with, not
/// where the file kept it.
struct M2TextureUnit {
    /// Texture index, resolved through `textureCombos`.
    u16 texture = 0;
    /// UV set, resolved through `textureCoordCombos`.
    u16 uvSet = 0;
    /// Texture-transform index, resolved through `textureTransformCombos`.
    /// Live data — see the file comment.
    u16 transform = 0;
    /// Weight index, resolved through `transparencyLookup`.
    u16 weight = 0;
    /// `m2::Texture::type`, 0..26. 0 is a file path; the rest are the
    /// replaceable slots, of which the fourth fills type 5.
    u8 textureType = 0;
    /// Which replaceable slot the caller must fill for `textureType != 0`.
    u8 replaceableId = 0;

    template <class V>
    void reflect(V& v) {
        v.field("texture", texture);
        v.field("uvSet", uvSet);
        v.field("transform", transform);
        v.field("weight", weight);
        v.field("textureType", textureType);
        v.field("replaceableId", replaceableId);
    }
};

/// The **resolved** batch; the converter rebuilds the combo tables on export.
struct M2Material {
    /// M2 version the record was read from.
    u32 sourceVersion = 0;

    /// `m2::Material::flags` — the `M2MaterialFlag` bits, kept raw because an
    /// undocumented bit in shipped content must survive the round trip.
    u16 renderFlags = 0;
    /// `m2::Material::blendingMode`, an `M2BlendingMode`.
    u16 blendingMode = 0;
    /// The `s_modelShaderEffect` selector. Rows past the transcribed range do
    /// occur in shipped content, so this is a `u16` and not an enum.
    u16 shaderId = 0;

    i16 priorityPlane = 0;
    u16 materialLayer = 0;
    /// Index of the colour / alpha track pair, or -1.
    i16 colorIndex = -1;
    /// `Batch::flags`, including 0x40 — which changes what `BeginDraw`
    /// multiplies, so it is not a rendering hint that can be inferred later.
    u8 batchFlags = 0;

    std::vector<M2TextureUnit> units;

    template <class V>
    void reflect(V& v) {
        v.field("sourceVersion", sourceVersion);
        v.field("renderFlags", renderFlags);
        v.field("blendingMode", blendingMode);
        v.field("shaderId", shaderId);
        v.field("priorityPlane", priorityPlane);
        v.field("materialLayer", materialLayer);
        v.field("colorIndex", colorIndex);
        v.field("batchFlags", batchFlags);
        v.inlineList("units", units);
    }
};

} // namespace native

template <>
struct ChunkTagTraits<native::M2Material> {
    static constexpr u32 value = kTag("NM2_");
    static constexpr u32 max_version = 1;
    static constexpr bool is_trivial = false;
};

} // namespace wem
} // namespace models
} // namespace whiteout
