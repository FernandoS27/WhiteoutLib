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

// ============================================================================
// RetargetSkeleton
// ============================================================================

/**
 * @brief Restates every model's rig in @p to's `RigConvention` (§10.5).
 *
 * The third operation, and the one the other two do not cover: `DeriveProfile`
 * and `AddProfileFromImport` rearrange material sets, while this rearranges the
 * *skeleton* — and until it existed `.m3 -> .mdx` wrote a skeleton of zero
 * pivots and `.mdx -> .m3` lost the pivot offset on every keyed bone.
 *
 * ### What is preserved, and why it is exact
 *
 * The skinning matrix, `skin(b, t) = inverseBind(b) * world(b, t)`, for every
 * bone at every keyed time. For **any** constant invertible `B` per node,
 *
 * ```
 * node'(b, t)     = B(b) * node(b, t) * inverse(B(parent))
 * inverseBind'(b) = inverseBind(b) * inverse(B(b))
 * ```
 *
 * leaves `skin` unchanged, because the two `B(b)` cancel. The convention is
 * therefore just a choice of `B`, and each direction has exactly one:
 *
 * - **To `PivotRelative`**, which forces `inverseBind' = I`: `B = inverseBind`,
 *   with the pivot set to the bone's model-space bind position — except on a
 *   split bone, where it has to be zero (see below). `worldBind` still answers
 *   with the bind position there; only a re-import through `.mdx`'s `PIVT`,
 *   which has nowhere else to keep it, would lose it.
 * - **To `ExplicitBind`**, from a rig whose bind is already the identity:
 *   `B = T(rest position)`, which makes `IREF = T(-pivot)` and turns every
 *   translation key into `key + pivot - parentPivot`. No shear can arise, so
 *   that direction is exact and needs no extra nodes.
 *
 * ### The one thing a pivot rig cannot hold
 *
 * `T(-p) * S * R * T(p + t)` has linear part `diag(s) * R`, so a conjugated
 * frame carrying **shear** does not fit in one node — 420 of 7,260 nodes across
 * a 400-model `.m3` sweep. Any linear part factors exactly as a pure rotation
 * followed by a scale-rotation (`A = U * (S V^T)` by SVD), so the default is to
 * put the second half on a helper parent rather than project the shear away.
 * Measured over that sweep, at the source's own key times:
 *
 * ```
 *                     models exact   over 0.1 units   worst
 *   split (default)   387 of 400            1         0.55 units
 *   projected          318 of 400           59        53,181 units
 * ```
 *
 * The projection is that bad because the error compounds: a bone whose ancestor
 * lost shear inherits it and adds its own. The other direction needs none of
 * this — no shear can arise — and reproduced 1,062 of 1,075 corpus `.mdx` files
 * and 135 of 142 `.m2` files exactly, with no node added at all. The `.m2`
 * remainder is not this operation: those models carry a rotation key that
 * decodes to `|q| = 2`, whose linear part is four times a rotation, and fifteen
 * of those in a chain put the frame at 1.5e9 — where the two compositions agree
 * to one ULP and disagree by two units.
 *
 * ### What it approximates
 *
 * Keys are rewritten at the times the source keyed, so every key is exact and
 * the curve *between* two keys is not: a conjugated slerp is not the slerp of
 * the conjugates, and a split node's two factors interpolate independently.
 * `refineKeys` is the lever for that, and it is spent only where it is needed.
 *
 * A pivot rig also has no way to hold a non-identity rest — `.m3`'s `IREF` is
 * not the inverse of its own rest chain, by up to 2.2 units on `Marine.m3` — so
 * a bone whose conjugated rest is not the identity gains a one-key track in each
 * container that had none for it.
 *
 * Neither direction can honour `DontInheritTranslation`/`Rotation`/`Scale`: they
 * modify the parent frame per component, and no single constant `B` cancels
 * that. `ModelSpace` *is* honoured, because it removes the parent entirely.
 * Everything here is reported rather than assumed.
 */
struct SkeletonRetargetOptions {
    /**
     * @brief Insert a helper parent where the conjugated frame shears.
     *
     * Clearing it projects the shear onto the nearest scale-rotation instead,
     * which keeps the node count but is only approximate — the measured cost is
     * in the file comment above. A caller that must not gain nodes (a fixed
     * bone palette, a rig another asset joins on by index) is the reason the
     * choice exists.
     */
    bool splitShearedNodes = true;

    /// How far a linear part may stray from `diag(s) * R` before it counts as
    /// sheared. Absolute, on matrix entries; bind frames are order-1.
    f32 shearTolerance = 1e-4f;

    /**
     * @brief Extra keys per source interval on a split node.
     *
     * The rewrite is exact at every key and only between them, because the
     * split's two factors interpolate independently and the product of two
     * interpolants is not the interpolant of the products. Subdividing is the
     * only lever, and it is spent only on the ~7% of nodes that split. Zero
     * disables it.
     */
    u32 refineKeys = 3;
};

struct SkeletonRetargetResult {
    bool ok = false;
    Diagnostics diagnostics;

    u32 nodesInserted = 0; ///< Helper parents the split added.
    u32 shearedNodes = 0;  ///< Nodes whose conjugated frame did not fit one node.

    /// (node, container) pairs whose TRS tracks were re-solved, and those that
    /// gained a one-key track holding a rest the target has no other way to say.
    u32 nodesRewritten = 0;
    u32 restKeysAdded = 0;

    /// Translation keys the `ExplicitBind` direction shifted by the pivot
    /// difference — that direction's whole edit, and exact.
    u32 keysOffset = 0;

    /// Worst residual between the source skinning matrix and the retargeted
    /// one, in units, sampled at each rewritten key at the bone origin and one
    /// unit off it. Zero when nothing needed approximating.
    f32 worstResidual = 0.0f;
};

/// Restates every model's rig in @p to's convention. A model already in it is
/// left alone, and that is a success with one `Info`.
SkeletonRetargetResult RetargetSkeleton(Document& document, ProfileId to,
                                        const SkeletonRetargetOptions& options = {});

} // namespace wem
} // namespace models
} // namespace whiteout
