// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file retarget.h
 * @brief Adding and retargeting profiles (WEM v3, design §6.6).
 *
 * Two distinct operations, and keeping them distinct is the point.
 * `AddProfileFromImport` brings a *second file's* material set over an existing
 * geometry and is lossless when the geometries agree; `DeriveProfile` invents a
 * set from one the document already has and is **always** lossy.
 *
 * > `DeriveProfile` returning 400 diagnostics is a *successful* call — the caller
 * > now knows exactly what a Wc3Classic set derived from a Diablo3 one costs.
 *
 * That is why the report is the return value rather than a side channel, and why
 * neither of these is a converter. A converter reads and writes files; these two
 * rearrange what a document already holds.
 *
 * The cheap path is worth naming: when source and target **share a native block
 * kind** — the WC3 pair on `MdxMaterial`, the SC2/Heroes pair on `M3Material` —
 * the block is carried and *filtered* rather than dropped, which is what makes
 * `Wc3Reforged -> Wc3Classic` a layer filter instead of a re-derivation through
 * the common material.
 *
 * A whole-document `Retarget` — the case where the geometry itself must change
 * (triangulation, welding, index width, a different canonical space) — is
 * `DeriveProfile` plus the geometry work and lands with the converters.
 */

#include <whiteout/common_types.h>
#include <whiteout/compatibility.h>

#include "diagnostics.h"
#include "document.h"
#include "profile.h"

namespace whiteout {
namespace models {
namespace wem {

struct RetargetOptions {
    /**
     * @brief Apply the two profiles' `sceneScale` ratio to the geometry.
     *
     * Off by default, and deliberately so: `sceneScale` is documentation, not an
     * operation (§6.2). WoW's and SC2's 100 are unit conversions while D3's 17 is
     * a *framing* constant fitted so a 7.3-unit Barbarian lands where WC3's
     * camera constants expect a character, and a retarget that treats them as
     * interchangeable gets the scale wrong by 6x. Ask for it explicitly or not at
     * all.
     *
     * **`DeriveProfile` never honours it**, and says so with one diagnostic when
     * it is set. §6.3's third rule is that adding a profile's material set
     * touches nothing else, and geometry is shared by every set in the model —
     * rescaling it would move the profile the caller was not retargeting.
     * Rescaling belongs to `Retarget`, which produces a *new* document and is
     * therefore allowed to change the geometry.
     */
    bool rescale = false;

    /// Carry and filter a shared native block instead of dropping it (§7.3's
    /// cheap-derive path). Clearing it forces the re-derivation through
    /// `CommonMaterial`, which is what a test wants when it is checking the
    /// expensive path.
    bool keepSharedNative = true;

    /// The look that survives when the target profile has none. The rest are
    /// reported, not silently dropped.
    u32 keepLook = 0;
};

struct DeriveResult {
    bool ok = false;
    Diagnostics diagnostics;
};

/**
 * @brief Derives @p to's material set from @p from's, for every model.
 *
 * Kind first — along §7.2's refinements (`Flatten` where the body is degenerate)
 * when the target's `commonKinds` demands one — then blend mode, then slots,
 * unsupported ones reported rather than silently dropped; collapses the look
 * table if the target has no looks.
 *
 * Influence width and UV set count are **reported, not clamped**, for the same
 * reason `rescale` is ignored: they are properties of the shared geometry, and a
 * material-set operation does not get to edit it. The report is the product.
 *
 * Fails only when @p from is not carried. Producing a set full of approximations
 * is a success with a report, which is the whole contract.
 */
DeriveResult DeriveProfile(Document& document, ProfileId from, ProfileId to,
                           const RetargetOptions& options = {});

/**
 * @brief Brings @p imported's material sets over @p document's geometry as
 *        profile @p profile.
 *
 * Fails if the geometry does not match — vertex count, face count and section
 * names, per model, in order. That check is the whole safety of the operation: a
 * second file's material set over a *different* mesh binds slots to sections that
 * mean something else, and the result renders without complaining.
 */
DeriveResult AddProfileFromImport(Document& document, ProfileId profile, const Document& imported);

} // namespace wem
} // namespace models
} // namespace whiteout
