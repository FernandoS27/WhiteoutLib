
#pragma once

#include <vector>
#include "../../../compatibility.h"
#include "../types.h"

namespace whiteout {
namespace m2 {

/// @brief One bone's replacement transform, from a `.bone` file.
struct BoneOverride {
    /// Indexes Model::bones. Every id in the WoW corpus is below its model's
    /// bone count, and the ids within a file are strictly ascending.
    u16 boneIndex = 0;
    /// Row-major, translation in the last row — the fourth column is
    /// `(0, 0, 0, 1)` in all 86174 corpus matrices.
    Matrix44f matrix;
};

/// @brief A whole `.bone` file: the skeleton edits one customization choice
///        needs.
///
/// On disk this is two parallel chunks — `BIDA` holds the bone ids and `BOMT`
/// the matrices — but their lengths match in every corpus file, so they are
/// paired here and split again on write.
struct BoneOverrideSet {
    /// 1 in every known file.
    u32 version = 1;
    /// Ascending by @ref BoneOverride::boneIndex, which is the order the files
    /// store and what lets the client binary-search a bone.
    std::vector<BoneOverride> overrides;
};

} // namespace m2
} // namespace whiteout
