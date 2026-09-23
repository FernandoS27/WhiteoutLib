// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file remove.h
 * @brief The mesh axis: who holds a mesh index, merging meshes and removing
 *        them (WEM design §5.12; EDIT_MODE_MESH_DESIGN.md §7.5;
 *        EDIT_MODE_MODELLING_DESIGN.md §8.1).
 *
 * Nothing removed or renumbered a mesh before a merge did, so the axis had no
 * table. It is written here rather than walked by an editor for the reason the
 * node and material tables are: a second list stops agreeing with WEM's the
 * first time WEM adds a row.
 *
 * ### The referencer table (§5.12) — normative
 *
 * | Referencer | Field | On a merge |
 * |---|---|---|
 * | `AnimChannel` of kind `Section` | `target.mesh`, `target.sub` | the primary's renumbered; an absorbed mesh's **invalidated** (`mesh = kInvalidIndex`), declaration and sub-tracks kept, as a removed node's are |
 * | a bone's geoset link | `BonePayload::gateMesh` | renumbered; an absorbed mesh's moved to the merged mesh |
 * | StarCraft II mesh emitters | `Sc2ParticleEmitterPayload::shapeSections` (sections of `meshes[0]`) | renumbered with `meshes[0]`'s sections; cleared when `meshes[0]` is absorbed |
 * | the face `section` layer | mesh-local | renumbered by `MergeSections` |
 * | the repair log | `FaceRecord::section`, mesh-local | renumbered by `MergeSections` |
 * | an MDX bone's raw `geosetId` in its native bag | an in-range value | renumbered; one naming a gone mesh becomes the file's "no geoset"; any other value is the file's own and kept |
 *
 * Out of this table, and named so the next profile sees them: M2's bone bag
 * `submeshId`, passed through raw; `AddProfileFromImport`'s `geometryMatches`,
 * which compares mesh order and section names rather than holding an index.
 *
 * A new structure that stores a mesh index must add itself here, and
 * `CheckMeshReferencers` cross-checks every listed field.
 */

#include <span>
#include <vector>

#include <whiteout/common_types.h>

#include "../diagnostics.h"
#include "../document.h"
#include "../model.h"

namespace whiteout {
namespace models {
namespace wem {

/**
 * @brief Renumbers every row of the §5.12 table through @p meshRemap
 *        (`meshRemap[old]` = new index, or `kInvalidIndex` for a mesh that is
 *        gone).
 *
 * A `Section` channel or a bone link left naming a mesh that is gone is
 * invalidated and reported, never repointed: which mesh was meant is not a
 * question this layer can answer. A caller that knows (a merge) moves them first.
 */
void RemapMeshReferencers(Model& model, std::span<const u32> meshRemap, Diagnostics& out);

/// Cross-checks the table's typed rows against the model — the `Validate` half.
/// (`Section` channels are checked beside the other channel rules.)
void CheckMeshReferencers(const Model& model, Diagnostics& out);

/// Erases the channels @p ids from model @p model's table, and their sub-tracks
/// from every clip that drives that model. The one definition of taking a
/// channel away.
void EraseChannels(Document& document, u32 model, std::span<const u32> ids);

/**
 * @brief Removes the meshes @p drop marks (one byte per mesh; nonzero goes)
 *        from model @p model, and every referencer follows.
 *
 * In this order: each `Section` channel of a mesh that goes is erased with its
 * sub-tracks (an invalidated one fails `Validate`, and nothing would ever
 * repoint it); the meshes go; `RemapMeshReferencers` renumbers the rest.
 *
 * @return how many meshes went. Nothing marked is nothing done.
 */
u32 RemoveMeshes(Document& document, u32 model, std::span<const u8> drop, Diagnostics& out);

/**
 * @brief Removes every mesh at level of detail 1-3 or above, keeping LOD 0 and
 *        `kAllLods` (EDIT_MODE_MODELLING_DESIGN.md §8.1, the user's U7).
 *
 * A document holds LOD 0 only; the `.mdx` export generates a ladder when the
 * model's `LodExport` asks for one. Before anything goes, each model with a
 * ladder records its ratios (each level's triangles over LOD 0's) and
 * `LodExport::sourceHadLevels`. A model whose every mesh is at a level keeps
 * them, and says so: an empty model is a worse answer than a coarse one.
 *
 * Runs at the tail of every importer that sets levels (`fromMdx`, `fromM3`) and
 * in the `.wem` reader. Notes `LevelOfDetailDropped` per model.
 *
 * @return how many meshes it removed.
 */
u32 DropLevelsOfDetail(Document& document, Diagnostics& out);

struct MeshMergeResult {
    bool ok = false;
    /// Old mesh -> new; `kInvalidIndex` for an absorbed one.
    std::vector<u32> meshRemap;
    /// The merged mesh's index after the merge.
    u32 merged = kInvalidIndex;
    /// Absorbed meshes' `Section` channels, invalidated (§7.3 named them).
    u32 channelsInvalidated = 0;
    /// Bone links moved from an absorbed mesh onto the merged one.
    u32 linksMoved = 0;
    Diagnostics diagnostics;
};

/**
 * @brief EDIT_MODE_MESH_DESIGN.md §7's operation: @p meshes merged into
 *        @p keep, which must be one of them.
 *
 * The result is ONE mesh with ONE section — a Warcraft III geoset has one
 * material, one colour, one record. Everything but the absorbed faces is the
 * primary's: its section, name, LOD, repair log and channels. The absorbed
 * faces keep their positions, corner attributes and skin exactly.
 *
 * On copies of the inputs, in this order: connectivity built (a mesh read from
 * `.wem` has none, and `MergeMeshes` copies no corner of a mesh without it);
 * tangents computed for an input that lacks them where the primary has them;
 * rigid sections baked where the inputs disagree; `MergeMeshes`, primary first;
 * `MergeSections` down to the primary's; the primary's name and LOD; bounds.
 * Then the model is changed and every referencer follows.
 *
 * Refused, model untouched, for fewer than two meshes, an index out of range,
 * @p keep not among them, different levels of detail, a mesh of more than one
 * section, or sections gated on different nodes. What `toMdx` could not write
 * is the converter's question (`MdxConverter::checkGeoset`), not this one.
 */
MeshMergeResult MergeMeshesInto(Model& model, std::span<const u32> meshes, u32 keep);

} // namespace wem
} // namespace models
} // namespace whiteout
