// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file deform.h
 * @brief Where a skinned vertex lands, on the CPU (EDIT_MODE_SKIN_DESIGN.md
 *        §3.5).
 *
 * The one implementation of the blend the renderer does: `sum(w_i * M_i)`, then
 * the position through it as a row vector. Picking, the tolerance set, the
 * stress test and the posed extents all go through here, so what the editor
 * measures is what the picture shows.
 *
 * The matrices are skin matrices — identity at rest for a `PivotRelative` rig —
 * indexed by node, as `boneWorldMatrices` and `PoseAt` produce them.
 */

#include <span>
#include <vector>

#include <whiteout/common_types.h>

#include "../geometry/mesh.h"

namespace whiteout {
namespace models {
namespace wem {
namespace skinning {

/// `sum(w_i * M_i)` over @p influences. A vertex with none gets the identity, so
/// it stays where the document put it.
Matrix44f BlendSkinMatrix(std::span<const geom::Influence> influences,
                          std::span<const Matrix44f> skin);

/// @p rest through @p influences and @p skin, row-vector, as the renderer skins.
Vector3f DeformPosition(const Vector3f& rest, std::span<const geom::Influence> influences,
                        std::span<const Matrix44f> skin);

/**
 * @brief Every vertex of @p mesh, posed by @p skin.
 *
 * @p written, when it is as long as the mesh's vertex count, is what each vertex
 * binds — the file's influences (`MdxConverter::writtenSkin`), which is what the
 * picture uses. Empty, the mesh's own are used instead: normalised, because a
 * file that ships weights summing to 0.998 keeps them (WEM_DESIGN §5.6) and the
 * renderer divides; and a vertex of a rigid section that binds nothing follows
 * that section's node whole.
 */
std::vector<Vector3f> DeformMesh(const Mesh& mesh, std::span<const Matrix44f> skin,
                                 std::span<const std::vector<geom::Influence>> written = {});

} // namespace skinning
} // namespace wem
} // namespace models
} // namespace whiteout
