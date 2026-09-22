// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file m2_wc3_sequences.h
 * @brief World of Warcraft's animations named as Warcraft III's, inside WEM.
 *
 * A `.m2` sequence is an `AnimationData` id, which the import names with the
 * client's own words: `Attack1H`, `ReadyBow`, `EmoteTalk`. Warcraft III plays a
 * sequence by the words in its name — `Attack`, `Stand Ready`, `Portrait
 * Talk` — so none of those would ever play in the game. The table behind this
 * gives each animation Warcraft III's name, the tags that tell the variants
 * apart (`First` for one-handed, `Fourth` for a bow) and whether it plays once,
 * which the `.m2` does not say. Anything the table does not name is left for
 * cinematics and triggers, as it was.
 *
 * StarCraft II reads the same names: the M3 export restates them through the
 * nomenclature it already applies to a Warcraft III model.
 */

#include <optional>
#include <string_view>

#include <whiteout/common_types.h>
#include <whiteout/models/wem/diagnostics.h>
#include <whiteout/models/wem/document.h>

namespace whiteout {
namespace models {
namespace cross {

/// One World of Warcraft animation as Warcraft III spells it.
struct M2Wc3Sequence {
    std::string_view name;   ///< The sequence name, without a variant suffix.
    bool nonLooping = false; ///< Plays once: `Sequence::Flag::NonLooping`.
};

/// @brief Warcraft III's sequence for `AnimationData` id @p animationId, or
///        nothing when the table does not name it.
std::optional<M2Wc3Sequence> M2Wc3SequenceFor(u16 animationId);

struct M2SequenceReport {
    u32 renamed = 0; ///< Clips that took a Warcraft III name.
    u32 kept = 0;    ///< `.m2` clips the table does not name, which kept theirs.
    wem::Diagnostics diagnostics;
};

/// Every clip a `.m2` import made — the ones carrying an `animationId` — takes
/// its Warcraft III name and looping. Warcraft III plays same-named sequences
/// as variants of one another; the second and later get its comment spelling,
/// `Stand - 1`, so that anything listing them can still tell them apart.
M2SequenceReport CrossM2Sequences(wem::Document& document);

} // namespace cross
} // namespace models
} // namespace whiteout
