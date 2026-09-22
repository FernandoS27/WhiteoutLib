// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file mirror.h
 * @brief The two maps mirroring needs (EDIT_MODE_SKIN_DESIGN.md §7.5), each
 *        built once per command rather than per point.
 *
 * - **The bone map**: by name, then by position, then itself. A saved override
 *   (`Node::skin.mirror`) beats both.
 * - **The point map**: the point nearest a point's mirrored position, through a
 *   hash grid, so it works whatever the vertex indices are.
 */

#include <span>
#include <string>
#include <utility>
#include <vector>

#include <whiteout/common_types.h>

#include "../geometry/mesh.h"
#include "../nodes/tree.h"
#include "points.h"

namespace whiteout {
namespace models {
namespace wem {
namespace skinning {

/// Which way a model is mirrored. Warcraft III faces +X with +Y to the left
/// (WEM's `Blizzard` space), so a character's own mirror is across the XZ plane.
enum class MirrorAxis : u8 { X, Y, Z };

/// How each bone's mirror was found, for the panel's list.
enum class MirrorSource : u8 {
    None,     ///< No mirror found.
    Override, ///< `Node::skin.mirror`.
    Name,     ///< The L/R token swap.
    Position, ///< The nearest pivot to the mirrored one.
    Self,     ///< A bone on the plane.
};

struct BoneMirror {
    u32 node = kInvalidNode;
    MirrorSource source = MirrorSource::None;
};

/// @p name with its left/right token swapped, or an empty string when it holds
/// none. The name is split at `_`, space, `.` and `-` and at case changes, and
/// `L`/`R`, `Left`/`Right` and `Lf`/`Rt` are swapped where a token is whole, a
/// prefix or a suffix: `Bone_Arm_L`, `Bip01 L Thigh`, `LeftHand`, `hand.l`.
std::string MirroredName(const std::string& name);

/// Which side @p name's left/right token says, read by exactly the rules
/// `MirroredName` swaps by, so the rig and the mirror map agree on a name.
enum class NameSide : u8 { None, Left, Right };
NameSide SideOfName(const std::string& name);

/// Per node, its mirror (§7.5). @p axis is the plane the model is mirrored
/// across; the position fallback accepts a pivot within 1 % of the model's size.
std::vector<BoneMirror> BuildBoneMirror(const NodeTree& nodes, MirrorAxis axis = MirrorAxis::Y);

/// Per point of @p points, the point nearest its mirrored position within
/// `1e-3 x` the mesh's diagonal, or `kInvalidIndex`. A point on the plane maps
/// to itself.
std::vector<u32> BuildPointMirror(const Mesh& mesh, const PointTable& points,
                                  MirrorAxis axis = MirrorAxis::Y);

/**
 * @brief The topology fallback (§7.5), for a mesh whose halves are not
 *        positionally symmetric.
 *
 * `BuildPointMirror` asks where a point's mirror OUGHT to be and looks there.
 * A mesh whose halves were modelled apart, or sculpted after the mirror, has no
 * point within a thousandth of the diagonal of that place, and the position map
 * comes back empty -- even though the two halves are the same mesh with the
 * same ring, and every point does have a mirror.
 *
 * So the correspondence is walked instead of looked up: from a pair that IS
 * matched, each unmatched neighbour of one side is paired with the neighbour of
 * the other whose EDGE best mirrors it, and those pairs are walked out in turn.
 * The comparison is of two short vectors rather than two positions, so a half
 * moved, scaled or jittered as a whole never accumulates an error.
 *
 * It starts from the position map's own pairs and fills only what they missed,
 * so a symmetric mesh gets the exact answer and pays one ring walk for nothing.
 * With no positional pair at all it seeds from the closest mirrored point
 * within 2 % of the diagonal -- the best guess available, which the walk then
 * either spreads from or stops at.
 */
std::vector<u32> BuildPointMirrorTopology(const Mesh& mesh, const PointTable& points,
                                          MirrorAxis axis = MirrorAxis::Y);

} // namespace skinning
} // namespace wem
} // namespace models
} // namespace whiteout
