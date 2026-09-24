// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file islands.h
 * @brief UV islands and wedges (EDIT_MODE_UV_DESIGN.md §3, §4).
 *
 * An island is not stored. It is derived, every time, from the UV connectivity
 * of one set: two faces are the same island when they share an edge, the edge
 * is not cut, and their corners agree in that set at both of its ends. The
 * document holds the cuts (`uvSeamN`), not the islands, because a cut is what a
 * modeller says and an island is what follows from it.
 *
 * A UV vertex is a **wedge**: the corners round one mesh vertex that a cut has
 * not separated. One mesh vertex is one wedge where nothing cuts it, and as
 * many wedges as the cuts make where something does. Every UV operation moves
 * wedges, never vertices: that is what lets one position carry several UVs.
 *
 * Device-free, allocation-heavy and cheap: a rebuild is one union-find over the
 * faces and one over the corners, both O(n). Hosts cache it against a commit,
 * not against a frame (EDIT_MODE_UV_PLAN.md P11).
 */

#include <span>
#include <vector>

#include <whiteout/common_types.h>

#include "../ids.h"
#include "../mesh.h"

namespace whiteout {
namespace models {
namespace wem {
namespace geom {
namespace uv {

/// One mesh's islands and wedges for one UV set, all of it derived.
struct UvIslands {
    /// Which set this was built for, so a cache can say whether it is the one
    /// being asked about.
    u32 set = 0;
    u32 count = 0;

    /// Per face slot: its island, or `kInvalidId` for a deleted face.
    std::vector<u32> islandOfFace;
    /// Per island, its faces: `faces[faceOffsets[i] .. faceOffsets[i + 1])`.
    std::vector<u32> faceOffsets;
    std::vector<u32> faces;

    /// Per island, its boundary halfedges, chained into loops:
    /// `boundary[boundaryOffsets[i] .. boundaryOffsets[i + 1])`. Each is a
    /// halfedge of the island whose edge is cut, and consecutive entries follow
    /// one another round a loop; a loop ends where the walk returns to where it
    /// started, so an island with holes has its loops laid end to end.
    std::vector<u32> boundaryOffsets;
    std::vector<HalfedgeId> boundary;
    /// Per island, `1` when it has no boundary at all — a closed surface, which
    /// no flattener can lay flat without a cut.
    std::vector<u8> closed;
    /// Per island, how many separate boundary loops it has. One is a disc.
    std::vector<u32> loops;

    /// Per halfedge slot: its wedge, or `kInvalidId` when the halfedge carries
    /// no corner (a boundary halfedge, or a deleted one).
    std::vector<u32> wedgeOfCorner;
    /// Per wedge, its corners: `wedgeCorners[wedgeOffsets[w] .. [w + 1])`.
    std::vector<u32> wedgeOffsets;
    std::vector<u32> wedgeCorners;
    /// Per wedge: the mesh vertex it is a UV copy of, and the island it is in.
    std::vector<u32> wedgeVertex;
    std::vector<u32> wedgeIsland;
    u32 wedgeCount = 0;

    /// Face @p face's island, or `kInvalidId`.
    u32 islandOf(u32 face) const {
        return face < islandOfFace.size() ? islandOfFace[face] : kInvalidId;
    }
    std::span<const u32> facesOf(u32 island) const;
    std::span<const HalfedgeId> boundaryOf(u32 island) const;
    std::span<const u32> cornersOf(u32 wedge) const;
    /// The wedge halfedge @p corner belongs to, or `kInvalidId`.
    u32 wedgeOf(HalfedgeId corner) const {
        return corner.index() < wedgeOfCorner.size() ? wedgeOfCorner[corner.index()] : kInvalidId;
    }
};

/// Per edge slot: `1` where the edge cuts the UV surface of @p set.
///
/// An edge cuts when `uvSeamN` says so, when it is a mesh border, when its two
/// faces are in different sections (a section border is a draw boundary and the
/// export splits there anyway), or when the two faces' corners disagree in
/// `uvN` at either end of it. The last is what makes an imported file's own
/// layout read back as the islands it was laid out as, with no layer written.
std::vector<u8> CutsOf(const Mesh& mesh, u32 set);

/// The islands and wedges of @p set. Needs connectivity; a mesh without it
/// comes back empty.
UvIslands BuildUvIslands(const Mesh& mesh, u32 set);

/// The wedges of one island, ascending. Derived from its faces' corners.
std::vector<u32> IslandWedges(const UvIslands& islands, const Mesh& mesh, u32 island);

/// What a first visit to a set had to write (EDIT_MODE_UV_DESIGN.md §4).
struct UvFirstVisit {
    bool wroteUvs = false;  ///< `uvN` was absent; it is zero-filled now.
    bool wroteSeams = false; ///< `uvSeamN` was absent; it is `CutsOf`'s answer now.
    bool wroteFree = false;  ///< `uvFreeN` was absent; see @ref bornFree.
    /// The set had no UVs at all, so its faces are free: nothing placed them.
    /// A set the file placed is locked, every face of it.
    bool bornFree = false;
};

/// Makes @p set's four layers exist, without changing any that already does.
///
/// The cuts of a set the file brought are its own layout's (`CutsOf`), so a
/// first unwrap finds the islands the artist shipped, and every face of it is
/// **locked**: the file placed them and nothing automatic may move them until
/// someone says so. A set that did not exist is zero-filled and every face of
/// it is free.
UvFirstVisit EnsureUvSet(Mesh& mesh, u32 set);

} // namespace uv
} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
