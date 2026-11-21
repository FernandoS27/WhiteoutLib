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
 * An explicit bind (`.m3`, Diablo III) parents the placed model under the
 * attachment and moves its points and model-space poses by the attachment's
 * world bind, its keys as they were. A pivot rig has no rest rotation to parent
 * a placement under, so there the placement is baked: points move by it and
 * every key is conjugated by it,
 * which is exact for a uniform scale. That keeps an emitter's frame on the
 * model's axes, and its frame is where it emits, so an emitter (or any leaf
 * whose frame matters) takes the placement onto its own rotation instead, by a
 * key where it had none. Its particles spread and fly by the placement's scale
 * and keep their size, as 12.1's do.
 *
 * An attached model plays on its own clock, as a doodad does: the clip it plays
 * (its `Stand`, else its first) and its global loops become global loops of the
 * model, one each per attached model, which every placement keys.
 *
 * Or, as Warcraft III plays a building's birth and death, it plays where its
 * attachment shows (`InlineClock::ShownSpans`): its first clip from the start
 * of each span the attachment is visible in, inside the model's own clips, and
 * hidden outside them.
 */

#include <vector>

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

/// A stretch of a clip in which a node shows: its visibility above 0, Warcraft
/// III's test, or its rest where the clip keys none.
struct ShownSpan {
    u32 clip = 0;
    f32 start = 0.0f;
    f32 end = 0.0f;
    /// How far what the node carries has played by @ref start: 0 where it
    /// starts, and in a Decay the Death before it showed it through to its end
    /// (Warcraft III plays it on across the switch), the Death's share.
    f32 played = 0.0f;
    /// How long what starts here plays: to the span's end, and on through the
    /// Decay it carries into. 0 on a span that goes on from a Death.
    f32 life = 0.0f;
};

/// Every span @p node of model @p model shows in, by clip and then by time.
std::vector<ShownSpan> ShownSpans(const Document& document, u32 model, u32 node);

enum class InlineClock : u8 {
    OwnLoops,   ///< Global loops of its own, as a doodad plays.
    ShownSpans, ///< From each span its attachment shows in, hidden outside them.
};

struct InlineOptions {
    InlineClock clock = InlineClock::OwnLoops;
};

/// Copies the model every attachment of @p into names into @p into, at the
/// attachment, which then names none. Both must be rigs of one convention; a
/// model that is not stays out, and so do its physics and pose stages, and its cameras
/// become helpers. Its attachments keep their names behind the placing
/// attachment's, where no lookup by name finds them.
///
/// On `ShownSpans` each placement hangs from a helper under its attachment,
/// `<attachment>_Model`, whose visibility is its spans; its sections hide with
/// it, and each takes its materials of its own.
InlineReport InlineAttachedModels(Document& document, u32 into, const InlineOptions& options = {});

} // namespace wem
} // namespace models
} // namespace whiteout
