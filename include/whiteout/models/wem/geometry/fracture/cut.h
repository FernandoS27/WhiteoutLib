// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file cut.h
 * @brief The meshes cut into pieces along convex cells, with inside faces
 *        (EDIT_MODE_FRACTURE_DESIGN.md §5.4-§5.7).
 *
 * **The surface is cut, never resampled.** Each target's faces are clipped by
 * each cell's planes in doubles, a face wholly inside a cell kept as it was,
 * the cut ones triangle by triangle. Every corner value and the skin are
 * interpolated where a triangle is cut, so the broken mesh draws as the whole
 * one did.
 *
 * **Shared points.** A point a cut makes is keyed: a mesh edge and the plane
 * it crosses, or a triangle and the sites whose cell edge pierces it.
 * Its position is computed from the key alone, so the pieces either side of a
 * crack hold it at the same bits. A point is inside a plane by the exact sign
 * of its distance, a tie going to the plane's lower site: a tolerance would
 * split the fan of a vertex lying near a plane in two.
 *
 * **Inside faces.** Each cell face is triangulated with the segments the cut
 * left on it (`ConstrainedTriangulation2d`), and each region the segments
 * bound is classified by winding number: a region inside some target becomes
 * an inside face of both pieces, wound outward from each, in the material of
 * the target that winds round it most. Inside faces have points of their own,
 * so a piece is closed after a weld by position and never made non-manifold
 * where shells overlap.
 *
 * **A plane that does not reach.** A slice plane cuts within its rectangles
 * alone (`CellComplex::reach`), but the regions are the whole planes'. So a
 * region of a cell face that is not wholly inside one rectangle joins the
 * pieces either side of it again; the inside faces between a piece and itself
 * go; and each source triangle's fragments in one piece are made one face
 * again, a cut point no other piece holds dissolved, so what a plane did not
 * part is left as it was. A plane therefore parts what it cuts all the way
 * through, and nothing else.
 */

#include <span>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/vector_types.h>

#include "../mesh.h"
#include "cells.h"
#include "winding.h"

namespace whiteout {
namespace models {
namespace wem {
namespace geom {
namespace fracture {

struct CutTarget {
    const Mesh* mesh = nullptr;
    const WindingMesh* winding = nullptr; ///< Oriented (`OrientWinding`).
    bool whole = false;                   ///< *Pieces* 1: not cut, one piece of its own.
};

/// What makes two parts of one cell one piece (§5.6).
enum class Parts : u8 {
    Overlap, ///< Their boxes overlap, or nearly: a wall and its trim.
    /// Their faces touch, and where they do they ride the same bones: a
    /// shoulder and its pad, but not a hand resting on a thigh.
    Touch,
};

struct CutOptions {
    /// *Smallest piece*, a share of the average piece: a smaller one joins the
    /// neighbour it shares the most inside-face area with.
    f64 smallest = 0.1;
    u32 threads = 0; ///< 0 is one per hardware thread.
    Parts parts = Parts::Overlap;
    /// `Parts::Touch`: per node, the node above it that counts as its parent,
    /// so a bone and the one it hangs from ride together. None knows no
    /// parents.
    std::span<const u32> parents;
};

struct CutPiece {
    std::vector<u32> cells;       ///< The seeds whose cells it fills; none for a Whole target.
    u32 whole = kInvalidId;       ///< The Whole target it is, or none.
    std::vector<u32> targets;     ///< The targets it has faces in, ascending.
    Vector3d centroid{0, 0, 0};   ///< Of its volume when solid, else of its area.
    f64 volume = 0.0;
    f64 area = 0.0;               ///< Outside area.
    f64 insideArea = 0.0;
    bool solid = false;
    f64 measure = 0.0;            ///< Its volume, or for a sheet its area × the band (§5.6).
};

/// Two pieces that touch: across inside faces, or at cut points.
struct PieceLink {
    u32 a = 0;
    u32 b = 0;
    f64 insideArea = 0.0;
    u32 sharedPoints = 0;
};

struct CutResult {
    /// Per target, its pieces' faces: outside, then inside. Section settings,
    /// every layer and the source skin are carried; the rejoin layers but the
    /// skin's (`fracture.source`, `.made`, `.cut`, `.weld`) are written.
    std::vector<Mesh> meshes;
    std::vector<std::vector<u32>> facePiece; ///< Per target, per face, its piece.
    /// Per target, per face, the cell face (`PlaneKey`) an inside face lies on;
    /// 0 for the outside.
    std::vector<std::vector<u64>> facePlane;
    std::vector<CutPiece> pieces;            ///< By cell, then by lowest source triangle.
    std::vector<PieceLink> links;
    u32 failedFaces = 0; ///< Cell faces whose triangulation failed: no inside faces there.
    u32 merged = 0;      ///< Pieces under *Smallest* that joined a neighbour.
    u32 split = 0;       ///< Pieces more than cells: a cell through parts that do not touch.
    u32 empty = 0;       ///< Cells that held nothing, so made no piece.
    u32 joined = 0;      ///< Pieces fewer: two a plane's reach did not part.
};

/// Cuts @p targets along @p complex's cells: Voronoi cells, or the regions of
/// slice planes.
CutResult CutPieces(std::span<const CutTarget> targets, const CellComplex& complex,
                    const CutOptions& options);

/**
 * @brief *Open parts: Thicken* (§5.7): every open part of @p mesh made a slab.
 *
 * Each welded point moves inward along its normal by @p thickness (halved
 * where that turns a face over), the copy is wound inward, and each border
 * loop is closed by a strip of quads. The faces it adds carry
 * `fracture.made`. Closed parts are left alone.
 *
 * @return how many parts it thickened.
 */
u32 ThickenOpen(Mesh& mesh, f64 thickness);

} // namespace fracture
} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
