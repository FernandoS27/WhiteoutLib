// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file d3_core.h
 * @brief Diablo III material conversion (design §7.2.6, §7.3).
 *
 * ### The render state is on a different asset
 *
 * A D3 material carries colours and textures and nothing about how it draws.
 * Blend, cull, depth and the alpha reference live on a `RenderPass` of a
 * `Shaders` asset, reached `SubObjectAppearance -> snoShaderMap -> .shm -> .shd`,
 * where the `.shm` is a **tag map with no index**: the runtime probes a fixed
 * chain and takes the first tag that resolves, so a map missing a tag falls
 * through to a more generic program rather than failing.
 *
 * Which means an id has to resolve for any of it to be known, and an id resolves
 * only through a storage. With no `AssetSource` at all, `ImportVariant` produces
 * the material's textures and colours and leaves the render state at its
 * defaults, silently — a caller that passed no provider knows it did, and one
 * diagnostic per material per look would bury everything else. A provider that
 * *has* one and fails to load it is the case that reports `AssetUnresolved`.
 *
 * What it never does is guess a blend mode from `dwMaterialFlags`, which is 0 on
 * Imperius's alpha-blended wings.
 *
 * ### Which kind it becomes
 *
 * §7.2.6: **`Combiners` when a Legacy stage block exists, else
 * `LegacyDeferred`.** The block is three groups of six tags on the pass —
 * `0xA0010 + i` the stage op, `0xA0016 + i` its colour combine, `0xA001C + i` its
 * alpha combine — and it really is an ordered fixed-function chain, so it maps
 * onto a stage chain and nothing else does. All 855 `Legacy.fx` passes carry it,
 * against 15 of 236 `ActorIrrad.fx`; where it is absent the shader-named texture
 * types are a slot map in all but name and project onto legacy slots.
 *
 * Of the combine alphabet only the **modulate** class (tens digit 2) is acted on,
 * because a replace or an add is a chain operation this shading model has no
 * chain to put in — Cain's plume adds its glow and Imperius's wings multiply
 * theirs, and both passes say so in their codes.
 */

#include <string>
#include <utility>
#include <vector>

#include <whiteout/models/wem/d3_converter.h>
#include <whiteout/models/wem/diagnostics.h>
#include <whiteout/models/wem/materials/material.h>
#include <whiteout/sno/d3/native/types.h>

namespace whiteout {
namespace models {
namespace wem {
namespace d3_core {

/// What the converter must supply that a `SubObjectAppearance` does not carry.
struct Context {
    /// The `.app` version the record was read from. 260 for the shipped corpus;
    /// the 2.6.2 binary disagrees about where the geoset descriptor lives, so a
    /// consumer cannot assume.
    u32 sourceVersion = 260;

    /// SNO texture id -> `Document::textures[]`. `.app` materials name textures
    /// by id, so the document's table is keyed the same way.
    ///
    /// Mutable because interning happens during an import that takes the context
    /// by const reference; see `internUnknownIds`.
    mutable std::vector<std::pair<u32, u32>> texturesBySno;

    /// Adds an unseen id to `texturesBySno` and returns its new index rather
    /// than reporting it unresolved. An importer wants this — it *is* the thing
    /// building the texture table; a fixture wants it off.
    bool internUnknownIds = false;

    /// Optional. Buys the `Shaders` resolution and nothing else, so a material
    /// imported without it has the same textures and the same colours.
    AssetSource* assets = nullptr;

    u32 toDocument(const native::D3AssetRef& texture) const;
};

/// One look's variant of one `AppearanceMaterial` -> one `wem::Material`.
///
/// The native block is attached **authoritative**, not in sync: D3 is import-only
/// (§18), the common material is a projection of a shader the game compiled, and
/// claiming the two are interchangeable would be claiming an export exists.
Material ImportVariant(const sno::d3::native::SubObjectAppearance& variant,
                       const std::string& name, const Context& context, Diagnostics& out);

/// The `Shaders` id a shader map resolves to, or -1.
///
/// `ShaderMap_ResolveShaderOpaque`'s chain. A map that carries none of the chain
/// falls back to its first valid entry, because shipped maps are small and
/// single-tagged often enough that refusing would drop real state — and a more
/// generic program is what the fall-through produces anyway.
i32 ResolveShadersId(const sno::d3::native::ShaderMap& map);

} // namespace d3_core
} // namespace wem
} // namespace models
} // namespace whiteout
