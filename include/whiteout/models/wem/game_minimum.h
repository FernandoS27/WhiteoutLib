// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file game_minimum.h
 * @brief What a game's model FILE needs and an authored model may not have
 *        yet: a bone every vertex is skinned to, and an animation.
 *
 * A document can hold a mesh bound to nothing (a primitive made before any
 * bone, an `.obj` read in) and no clip at all. Warcraft III then gets a geoset
 * whose matrix groups name nothing and StarCraft II a region of no bones whose
 * weights sum to zero, and neither gets a sequence, while `Stand` is what both
 * play a model in at rest. So a file is given them, and only a file: the
 * converters are also what a host draws a document through, and a bone added
 * there would move every node number the host holds.
 *
 * A material is not among them. A section that wears none is the converters'
 * own question, because it must draw: `toMdx` and `toM3` each write one blank
 * material for every such section.
 */

#include <whiteout/common_types.h>

#include "diagnostics.h"
#include "document.h"

namespace whiteout {
namespace models {
namespace wem {

/// The name of the clip @ref AddGameMinimum makes, and its length in seconds.
inline constexpr const char* kGameMinimumClip = "Stand";
inline constexpr f32 kGameMinimumClipSeconds = 1.0f;

struct GameMinimumReport {
    u32 boneAdded = kInvalidNode; ///< The root bone made; invalid when the model had one.
    u32 verticesBound = 0;        ///< Vertices that bound nothing and now bind a bone.
    bool clipAdded = false;       ///< A `Stand` was made: the model had no animation.
};

/// Whether `document.models[model]`, written as @p profile, lacks what
/// @ref AddGameMinimum gives it.
bool NeedsGameMinimum(const Document& document, ProfileId profile, u32 model = 0);

/// Gives `document.models[model]`, for a FILE of @p profile, what every game
/// needs of one, each addition said in @p out:
///
/// - Each vertex that binds no bone the file writes is bound whole: to the
///   heaviest bone of its mesh's first bound vertex, which is `toMdx`'s rule
///   for such a vertex, or, in a mesh that binds nothing, to the model's first
///   bone. A model with no bone gets one, a root at the origin, appended so no
///   node moves.
/// - A model with no clip, its global loops aside, gets a one-second looping
///   `Stand` with nothing keyed: the model at its rest.
GameMinimumReport AddGameMinimum(Document& document, ProfileId profile, Diagnostics& out,
                                 u32 model = 0);

} // namespace wem
} // namespace models
} // namespace whiteout
