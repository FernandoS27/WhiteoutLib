
#pragma once

// ============================================================================
// Standalone `.bone` files.
//
// A character or creature model ships one `.bone` per customization choice,
// named either by position — `<stem>_00.bone`, `_01`, … — or by file id through
// the BFID chunk of the `.m2` or its `.skel`. Parser and Writer follow both
// into Model::boneOverrides; these two functions are for handling a `.bone` on
// its own.
// ============================================================================

#include <span>
#include <string>
#include <vector>
#include "structures.h"

namespace whiteout {
namespace m2 {

/// @brief Parse a `.bone` file.
///
/// Anything unrecognized is reported through @p issues (when given) and then
/// skipped. Returns nullopt when @p data is too short to hold the version word
/// and one chunk header, or when the two chunks disagree on how many entries
/// they hold.
std::optional<BoneOverrideSet> parseBoneOverrides(std::span<const u8> data,
                                                  std::vector<std::string>* issues = nullptr);

/// @brief Serialize @p overrides back to `.bone` bytes.
///
/// Writes the version word, then `BIDA` and `BOMT` in that order — the layout
/// every corpus file uses.
std::vector<u8> writeBoneOverrides(const BoneOverrideSet& overrides);

} // namespace m2
} // namespace whiteout
