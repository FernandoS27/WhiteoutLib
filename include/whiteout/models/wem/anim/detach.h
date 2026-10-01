// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file detach.h
 * @brief A clip taken off the MDX timeline it was imported on
 *        (EDIT_MODE_ANIMATIONS_DESIGN.md §1.5).
 *
 * An `.mdx` clip keeps its window, `[intervalStart, intervalEnd]` on the
 * model's one timeline, so the export writes its keys as they stand; the
 * MDX merge's first-claim rule then drops a key written into it anywhere
 * else. Any edit that writes keys detaches the clip first: the editor's
 * key edits, and the physics bake at export (EDIT_MODE_PHYSICS_BAKE_DESIGN.md
 * §8.2).
 */

#include <whiteout/common_types.h>

#include "../document.h"

namespace whiteout {
namespace models {
namespace wem {

/// Whether @p clip still sits on the window it was imported with.
bool OnImportedTimeline(const Clip& clip);

/// Detach @p clip of @p model, if it still has a window: every sub-track with
/// a key inside `[0, duration]` gets one at each edge it does not key,
/// holding what the renderer shows there — its wrap from the last key round
/// to the first, split exactly for a Hermite or Bezier stream — and the keys
/// outside go, bracket keys included; a sub-track with no key inside goes
/// whole; and the window is erased from the native bag. Global loops are
/// never detached.
void DetachClip(Document& document, u32 model, u32 clip);
/// The same on a clip of its own, whose channels @p table declares.
void DetachClip(const AnimChannelTable& table, Clip& clip);

} // namespace wem
} // namespace models
} // namespace whiteout
