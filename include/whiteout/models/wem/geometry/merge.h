// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file merge.h
 * @brief Meshes concatenated, split by section, and sections merged.
 */

#include <span>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/vector_types.h>

#include "mesh.h"

namespace whiteout {
namespace models {
namespace wem {
namespace geom {

/// Concatenates @p meshes into one. Sections, attribute layers, skin bindings,
/// merge groups and stored triangulations are all renumbered into the combined
/// space.
Mesh MergeMeshes(std::span<const Mesh> meshes);

/// One mesh per section, in section order. A section with no faces still yields a
/// mesh, so the result indexes like `Mesh::sections`.
std::vector<Mesh> SplitMesh(const Mesh& mesh);

/**
 * @brief Relabels every face of @p sections to @p keep and removes the others,
 *        renumbering the rest — the face layer and the repair log alike.
 *
 * What `MergeMeshes` leaves undone when the goal is one draw section: it keeps
 * every input's section, and an exporter writes one draw per section. The kept
 * section's metadata is the result's.
 *
 * @return the section remap (`remap[old]` = new index, `kInvalidId` for a
 *         removed section, whose faces are now @p keep's). Empty, mesh
 *         untouched, when the sections disagree on `rigidNode` or on the
 *         visibility gate (`kSectionVisibilityNode`), or an index is out of
 *         range.
 */
std::vector<u32> MergeSections(Mesh& mesh, std::span<const u32> sections, u32 keep);

} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
