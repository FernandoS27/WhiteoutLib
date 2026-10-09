// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file draw_order.h
 * @brief Whether a mesh's place among a model's draws can show
 *        (EDIT_MODE_OPTIMIZE_DESIGN.md §3.3, §3.4).
 *
 * Warcraft III draws a model's opaque geosets in file order under the depth
 * test, where order cannot show, and sorts the rest back to front by their own
 * centroid, where it does. Which of the two a geoset is, is not a field of its
 * material: the game asks its first **visible** layer, frame by frame, and an
 * HD layer that is opaque by its filter still goes with the sorted ones while
 * it is less than fully drawn (`IsOpaque`; WhiteoutFlakes' `ClassifyGeoset` is
 * the renderer's copy of the same rule, and the two are read together).
 *
 * This is that rule's answer over everything the document can play, so a
 * caller can ask it once.
 */

#include <whiteout/common_types.h>

#include "../document.h"

namespace whiteout {
namespace models {
namespace wem {

/**
 * @brief Whether section @p section of mesh @p mesh draws the same wherever it
 *        stands among the model's meshes, in every profile and look it is
 *        drawn in and at every time a clip can show.
 *
 * For a Warcraft III profile the material is read as `toMdx` writes it:
 * - every layer tests and writes depth;
 * - every layer that can be the first visible one -- the first, and each later
 *   one whose earlier layers can all be hidden -- has an opaque filter (`None`
 *   or `Transparent`);
 * - where such a layer shades as HD, neither its alpha nor the section's ever
 *   reads between hidden and full;
 * - and every blended layer lies over a `None` layer that is never hidden, so
 *   what it blends with is the geoset's own pixels and not the scene.
 *
 * For any other profile it is the common material's word: an opaque or
 * alpha-tested blend that tests and writes depth.
 */
bool DrawsOrderFree(const Document& document, u32 model, u32 mesh, u32 section = 0);

} // namespace wem
} // namespace models
} // namespace whiteout
