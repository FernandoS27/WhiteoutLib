// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file d3_material.h
 * @brief `native::D3Material` — the Diablo III block (design §7.3).
 *
 * The pieces are generated (`d3_native.h`); pairing them is not, because the
 * pairing is the whole finding:
 *
 * ### The render state is not on the material
 *
 * It is on the `Shaders` asset's `RenderPass`, and a sub-object carries **two
 * whole `Shaders` assets** — opaque and translucent — swapped by a scalar
 * `alpha < 1.0`. So this block holds two vectors of resolved passes rather than
 * one state, and that is also why two-sided is not a cull flag: it is *two
 * passes*, a CW one and a CCW one (12 shaders, 263 appearances across the
 * corpus). A single `cull` field could not represent it.
 *
 * ### Which material wins
 *
 * A sub-object may carry an embedded `UberMaterial` *and* name a standalone
 * `Material` asset (group 57). The embedded one wins. Both are kept: the
 * reference is what a later re-resolve would need, and dropping it would make
 * the round trip lossy for a field the file actually has.
 *
 * `D3Converter` is import-only (§18), so this block's gate is WEM write →
 * re-read rather than a re-export, and its converter lands with the actors at
 * P6.
 */

#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/models/wem/chunk_traits.h>
#include <whiteout/models/wem/native/d3_native.h>

namespace whiteout {
namespace models {
namespace wem {
namespace native {

/// The slot map plus the render state, which in D3 lives on a *different asset*.
struct D3Material {
    /// 260 for the shipped `.app` corpus. The 2.6.2 binary disagrees about
    /// where the geoset descriptor lives, so a converter cannot assume.
    u32 sourceVersion = 0;

    /// The per-look embedded material. Wins over `baseMaterial` when present.
    D3UberMaterial uber;

    /// `snoMaterial`, group 57 — the standalone material this sub-object names.
    D3AssetRef baseMaterial;
    /// `snoCloth`.
    D3AssetRef cloth;

    /// The sub-object's own tag/value parameters.
    std::vector<D3TagValue> shaderParams;

    /// Resolved from the sub-object's two `Shaders` assets. Two-sided is a CW
    /// pass plus a CCW one, which is why these are vectors.
    std::vector<D3RenderState> opaquePasses;
    std::vector<D3RenderState> translucentPasses;

    template <class V>
    void reflect(V& v) {
        v.field("sourceVersion", sourceVersion);
        v.field("uber", uber);
        v.field("baseMaterial", baseMaterial);
        v.field("cloth", cloth);
        v.inlineList("shaderParams", shaderParams);
        v.inlineList("opaquePasses", opaquePasses);
        v.inlineList("translucentPasses", translucentPasses);
    }
};

} // namespace native

template <>
struct ChunkTagTraits<native::D3Material> {
    static constexpr u32 value = kTag("ND3_");
    static constexpr u32 max_version = 1;
    static constexpr bool is_trivial = false;
};

} // namespace wem
} // namespace models
} // namespace whiteout
