#pragma once

// ============================================================================
// The client's `AnimationData` names, keyed by the id an `.m2` sequence holds.
//
// A `.m2` sequence has no name in the file. It has an `AnimationData` id and a
// variation index, and the client turns the pair back into "Stand", "Run",
// "Attack1H" through its own copy of the table below. Anything reading a `.m2`
// without it reports bare numbers, and anything *writing* the model out to a
// format whose sequences do carry names has nothing to write.
// ============================================================================

#include <string>
#include <string_view>

#include <whiteout/common_types.h>

namespace whiteout {
namespace m2 {

/// @brief The `AnimationData` name for @p animationId, or an empty view when
///        the id is past the end of the table.
std::string_view animationName(u16 animationId);

/// @brief The full name for one sequence: its animation name, plus the
///        variation as a ` - <n>` suffix when it is not the first.
///
/// Variations share an id and differ only by index, so the number has to be in
/// the name or every list of them reads as the same entry repeated. The suffix
/// is Warcraft III's spelling of a variant (`"Stand - 1"`); StarCraft II writes
/// the same thing as `"Stand 01"`. Both games bind on the leading token, so
/// either spelling still reaches the right animation on the far side.
///
/// An id the table does not cover becomes `Anim<id>` rather than an empty
/// string: an unnamed sequence is worse than an unrecognised one.
std::string sequenceName(u16 animationId, u16 variationIndex);

} // namespace m2
} // namespace whiteout
