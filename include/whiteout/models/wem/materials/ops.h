// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file ops.h
 * @brief Material-axis removal (WEM v3, design §7.5).
 *
 * §10.6 gives node indices a normative referencer table and two-phase removal;
 * §5 gave the geometry the same treatment. The material axis has referencers too,
 * and ordinals made them easy to dangle silently — deleting a `CompositeLayer`
 * shifts every later layer's ordinal, which would quietly retarget features and
 * animation sub-tracks onto their neighbours. Same discipline.
 *
 * ### The referencer table (§7.5) — normative
 *
 * | Referencer | Field | Points at |
 * |---|---|---|
 * | `SlotBinding` | `byLook[]` | `ProfileMaterialSet::materials`, by index |
 * | `MaterialFeature` | `layer` | a kind-body layer / stage / slot ordinal |
 * | `AnimChannel` | `target.material` | `(profile, slot, look, sub, channel)` (P7) |
 * | `Actor` | `defaultLook`; `ActorEvent::lookName` | the set's `LookTable` (P6) |
 * | `MeshSection` | `materialSlot` | `Model::materialSlots` — §6.3's coverage rule |
 *
 * A new structure that stores a material-axis index adds itself to that table,
 * and `Validate` cross-checks every listed field. `CheckMaterialReferencers` is
 * the single registration point, the way `NodeReferencers` is for §10.6.
 *
 * Two rules the operations below implement and nothing else may bypass:
 *
 * - A removal **rewrites every ordinal above the removed element** and
 *   **invalidates — with one diagnostic each — every referencer whose target
 *   died**. Invalidating means `kInvalidIndex`, never a silent repoint: "which
 *   material did you mean" is not a question this layer can answer.
 * - Feature `id`s are **never rewritten**; that is what they are for. A feature
 *   survives its neighbours' deletion precisely because the operation fixes
 *   `layer` while sub-tracks keep joining on the id.
 *
 * Unlike node removal these are immediate rather than two-phase. A node's
 * referencers are spread over meshes, actors and clips and there is real value in
 * batching; a material set's referencers are all inside the set, and marking
 * would buy nothing but a second lifecycle to learn.
 */

#include <string>

#include <whiteout/common_types.h>
#include <whiteout/compatibility.h>

#include "../diagnostics.h"
#include "../model.h"

namespace whiteout {
namespace models {
namespace wem {

struct RemovalResult {
    bool removed = false;
    /// How many referencers the call pointed at nothing — each one also produced
    /// a diagnostic. Zero is the common case and the one worth asserting.
    u32 invalidated = 0;
    /// How many referencers had an ordinal rewritten because they sat above the
    /// removed element.
    u32 rewritten = 0;
    Diagnostics diagnostics;
};

/**
 * @brief Removes `set.materials[material]`.
 *
 * Every `SlotBinding` entry above it is decremented; every entry pointing at it
 * becomes `kInvalidIndex` with a `SlotNotBound` diagnostic, which
 * `ValidateLevel::Profile` then reports as a coverage hole until the caller binds
 * something.
 */
RemovalResult RemoveMaterial(Model& model, ProfileId profile, u32 material);

/**
 * @brief Removes one layer / stage / slot of a material's body, by ordinal.
 *
 * The ordinal space is `CommonMaterial::ordinalCount()` — layers for `Composite`,
 * stages for `Combiners`, the `slots` vector's order for the two deferred kinds.
 * Features on the removed ordinal go with it (`FeatureDropped`); features above
 * it have `layer` decremented, keeping their ids.
 *
 * The native block, if any, is now stale in a way WEM cannot repair, so the
 * material goes to `NativeSync::CommonEdited` — which is exactly §7.1's contract
 * and forces the exporter to re-derive or refuse rather than write a block whose
 * layer count disagrees with the common material's.
 */
RemovalResult RemoveLayer(Model& model, ProfileId profile, u32 material, u32 ordinal);

/**
 * @brief Removes a feature by id.
 *
 * Ordinals are untouched: a feature is not something other features are numbered
 * relative to. P7's sub-tracks joining on the dead id are invalidated here.
 */
RemovalResult RemoveFeature(Model& model, ProfileId profile, u32 material, u32 featureId);

/**
 * @brief Removes a look from a set, and its column from every `SlotBinding`.
 *
 * Refuses to remove the last look: a set always has at least one, and "no looks"
 * and "one look" are different shapes, not a degenerate pair.
 */
RemovalResult RemoveLook(Model& model, ProfileId profile, u32 look);

/**
 * @brief Removes a whole profile's material set.
 *
 * Also clears the profile's bit from every section mask, because a section that
 * declares it draws in a profile the model no longer carries is a coverage hole
 * by construction. A section left with an empty mask — one nothing draws — is
 * reported, not removed: dropping geometry is not a side effect this operation
 * gets to have.
 */
RemovalResult RemoveProfileSet(Model& model, ProfileId profile);

/// Cross-checks every §7.5 referencer without changing anything — the `Validate`
/// half of the table.
void CheckMaterialReferencers(const Model& model, u32 modelIndex, Diagnostics& out);

} // namespace wem
} // namespace models
} // namespace whiteout
