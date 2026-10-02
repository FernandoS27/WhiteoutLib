// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file inline_models.h
 * @brief The models a model's attachments carry, written into it, for a format
 *        that holds one model.
 *
 * A document keeps what rides an attachment as a model of its own (§9.1): a
 * WMO's doodads, appended by the host. `.mdx` holds one model, and an `ATCH`
 * path names a file the game loads, never a `.m2`, so every placement is copied
 * in: its nodes, meshes and emitters where the attachment puts them, its
 * materials once per model.
 *
 * A pivot rig has no rest rotation to parent a placement under, so the
 * placement is baked: points move by it and every key is conjugated by it,
 * which is exact for a uniform scale. That keeps an emitter's frame on the
 * model's axes, and its frame is where it emits, so an emitter (or any leaf
 * whose frame matters) takes the placement onto its own rotation instead, by a
 * key where it had none. Its particles spread and fly by the placement's scale
 * and keep their size, as 12.1's do.
 *
 * An attached model plays on its own clock, as a doodad does: the clip it plays
 * (its `Stand`, else its first) and its global loops become global loops of the
 * model, one each per attached model, which every placement keys.
 */

#include <whiteout/common_types.h>

#include "diagnostics.h"
#include "document.h"

namespace whiteout {
namespace models {
namespace wem {

struct InlineReport {
    u32 placements = 0; ///< Attachments whose model was copied in.
    u32 models = 0;     ///< Distinct models they carried.
    u32 loops = 0;      ///< Global loops the models' clips became.
    Diagnostics diagnostics;
};

/// Copies the model every attachment of @p into names into @p into, at the
/// attachment, which then names none. Both must be pivot rigs; a model that is
/// not stays out, and so do its physics and pose stages, and its cameras
/// become helpers. Its attachments keep their names behind the placing
/// attachment's, where no lookup by name finds them.
InlineReport InlineAttachedModels(Document& document, u32 into);

} // namespace wem
} // namespace models
} // namespace whiteout
