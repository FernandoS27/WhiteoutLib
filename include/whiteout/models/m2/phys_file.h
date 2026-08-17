
#pragma once

// ============================================================================
// Standalone `.phys` files.
//
// A model's physics reaches it three ways, all of them the same bytes: a
// pre-Legion `.m2` names a `<stem>.phys` sibling by path, a chunked one names a
// file id in PFID, and a Shadowlands-era one carries the payload inline in
// PFDC. Parser and Writer already follow all three into Model::physics; these
// two functions are for handling a `.phys` on its own.
// ============================================================================

#include <span>
#include <string>
#include <vector>
#include "structures.h"

namespace whiteout {
namespace m2 {

/// @brief Parse a `.phys` file, or the payload of an M2's PFDC chunk.
///
/// Anything unrecognized is reported through @p issues (when given) and kept in
/// PhysicsData::unknownChunks, so writePhysics() can put it back. Returns
/// nullopt only when @p data holds no PHYS chunk at all.
std::optional<PhysicsData> parsePhysics(std::span<const u8> data,
                                        std::vector<std::string>* issues = nullptr);

/// @brief Serialize @p physics back to `.phys` bytes.
///
/// Which chunk names and record layouts are used follows PhysicsData::version.
/// The result is unpadded — PFDC's alignment padding is added by the M2 writer,
/// which is the only place it applies.
std::vector<u8> writePhysics(const PhysicsData& physics);

} // namespace m2
} // namespace whiteout
