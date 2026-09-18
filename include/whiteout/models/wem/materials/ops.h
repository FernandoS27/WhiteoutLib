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
 * | `AnimChannel` | `target.material` | `(profile, slot, look)` + `sub` (§10.8) |
 * | `MeshSection` | `materialSlot` | `Model::materialSlots` — §6.3's coverage rule |
 * | node payload | `ForEachMaterialLink` | `Model::materialSlots` — an emitter's material (§10.9) |
 *
 * A new structure that stores a material-axis index adds itself to that table,
 * and `Validate` cross-checks every listed field. `CheckMaterialReferencers` is
 * the single registration point, the way `NodeReferencers` is for §10.6.
 *
 * ### The texture referencer table (§7.4) — normative
 *
 * `Document::textures` is document-wide, so its referencers span every set of
 * every model:
 *
 * | Referencer | Field | Note |
 * |---|---|---|
 * | common `TextureInput` | `texture` | one per ordinal of every material (`inputAt`) |
 * | `native::MdxMaterial` | `layers[].subTextures[].textureId`, and `layers[].textureId` of a layer with no sub-textures | document indices by the identity `fromMdx` builds; a layer with sub-textures keeps its map there and `textureId` is a zeroed leftover |
 * | node payload | `ForEachTextureLink` | a Warcraft III particle emitter's `texture` |
 * | `AnimChannel` on a Warcraft III material, `MaterialLayer` target, `Channel::TextureIndex` | every key value of every sub-track joining it, and `initValue` | MDX `KMTF`: a flipbook's frames are texture indices |
 *
 * **Not** a `Node` target's `TextureIndex`: that is a Warcraft III ribbon's
 * `KRTX`, a cell of the ribbon's own flipbook grid, not a texture. **Nor** an
 * `.m3` layer's: that is its `currentFrame`, a frame of the layer's own atlas.
 *
 * The other native kinds (`M2Material`, `M3Material`, `D3Material`) have not
 * been audited for index-addressed textures, so an operation over this table
 * refuses a document holding one rather than skipping a row it does not know.
 * `CheckTextureReferencers` is this table's registration point.
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
#include "../document.h"
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
    /// `RemoveSlot` / `RemoveTexture`: old index -> new, `kInvalidIndex` for the
    /// removed one. What a caller holding an index across the call reads.
    std::vector<u32> remap;
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
 * relative to. Channels targeting the dead id are invalidated here; the
 * sub-tracks that joined on those channels go with them, which is the whole
 * point of the id indirection (§10.8.1).
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

/**
 * @brief Removes material slot @p slot from the model.
 *
 * Its binding goes from every set, and the material bound there with it
 * (through @ref RemoveMaterial) when no other slot binds that material; then
 * the slot itself. Every referencer above it is renumbered. Sections and
 * payload links naming it take @p replacement; channels targeting it are
 * invalidated, one diagnostic each.
 *
 * Refused — `removed == false`, nothing written — when a section or a link
 * names the slot and @p replacement is `kInvalidIndex`, the slot itself, or not
 * bound at every look of every set its users draw in: a section's `profiles`,
 * and for a link every set that binds the removed slot. Invalidate-never-
 * repoint is the rule for a removal; a replacement the caller named is not a
 * silent repoint, and a section left naming no slot is not an option at all.
 */
RemovalResult RemoveSlot(Model& model, u32 slot, u32 replacement = kInvalidIndex);

/// §6.3's coverage rule for one section: whether every set @p section draws in
/// binds @p slot at every look. @p missing, when given, receives the first
/// profile that does not.
bool SlotCoversSection(const Model& model, const MeshSection& section, u32 slot,
                       ProfileId* missing = nullptr);

/// Whether `RemoveSlot(model, slot, replacement)` would accept @p replacement
/// for @p slot's users — asked before, so a caller can offer only the slots it
/// would take. True for a slot nothing uses, whatever the replacement. @p why,
/// when given, receives the refusal.
bool SlotCanReplace(const Model& model, u32 slot, u32 replacement, std::string* why = nullptr);

/**
 * @brief Removes `document.textures[texture]`, over the §7.4 table above.
 *
 * Every referencer naming it takes @p replacement, and every one above it is
 * renumbered. Refused, with nothing written, when something names it and
 * @p replacement is `kInvalidIndex` or the texture itself, and when any set
 * holds a native block kind the table has no rows for.
 */
RemovalResult RemoveTexture(Document& document, u32 texture, u32 replacement = kInvalidIndex);

/// Cross-checks every §7.5 referencer without changing anything — the `Validate`
/// half of the table.
void CheckMaterialReferencers(const Model& model, u32 modelIndex, Diagnostics& out);

/// The same for the §7.4 texture table, across the whole document.
void CheckTextureReferencers(const Document& document, Diagnostics& out);

/// How many §7.4 referencers name @p texture, by what they are — what a caller
/// asks before a removal ("used by 3 layers and 1 emitter"). A material's common
/// input mirrors its native block's layer, so a material with a block counts its
/// block's references and not the common's too.
struct TextureReferencerCount {
    u32 materialLayers = 0; ///< Layer, stage or slot references, across every set.
    u32 payloadLinks = 0;   ///< Emitters.
    u32 flipbookKeys = 0;   ///< Keys (and rest values) of `KMTF` channels.

    u32 total() const {
        return materialLayers + payloadLinks + flipbookKeys;
    }
};
TextureReferencerCount CountTextureReferencers(const Document& document, u32 texture);

} // namespace wem
} // namespace models
} // namespace whiteout
