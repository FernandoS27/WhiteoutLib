// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file fracture.h
 * @brief A model's meshes broken into pieces, and put back
 *        (EDIT_MODE_FRACTURE_DESIGN.md §6, §13.2).
 *
 * `BreakModel` runs the whole break on a document: the seeds, the cells, the
 * cut, then each target replaced by its pieces, one bone per piece, and the
 * rejoin layers that lead back to the source. Physics is the viewer's.
 *
 * The way back is either the whole meshes, kept hidden and drawn by no profile
 * (*Keep the whole meshes*), or the pieces themselves: `RejoinPieces` drops the
 * faces the fracture made, merges each source face's fragments and dissolves
 * the cut points along its edges, and gives back the merge groups and the skin
 * the layers kept.
 */

#include <span>
#include <string>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/vector_types.h>

#include "../../document.h"
#include "cells.h"
#include "cut.h"

namespace whiteout {
namespace models {
namespace wem {
namespace geom {
namespace fracture {

/// Why a mesh cannot be broken (§4).
enum class Refusal : u8 {
    None,
    Missing,   ///< No such mesh, or no such model.
    Empty,     ///< It has no faces.
    Cloth,     ///< A section is simulated or driven by cloth.
    Billboard, ///< A section turns to the camera: it has no rigid pieces.
    Repeated,  ///< The same mesh twice.
    NoPieces,  ///< The break made nothing.
};

const char* ToString(Refusal refusal);

/// Why @p mesh of @p model cannot be a target, or `Refusal::None`.
Refusal RefuseTarget(const Model& model, u32 mesh);

/// *Open parts* (§5.7).
enum class OpenParts : u8 { Solid, Thicken };

struct FractureTarget {
    u32 mesh = kInvalidIndex;
    u16 pieces = 16; ///< 1 is Whole: not cut, one piece.
};

struct FractureSpec {
    std::string name; ///< The fracture's; empty takes the first target's.
    std::vector<FractureTarget> targets;
    u32 seed = 1;
    f32 nearBlast = 0.4f;
    Vector3f blastCentre{0, 0, 0};
    f32 blastRadius = 0.0f; ///< 0: no blast, so *Smaller near the blast* does nothing.
    f32 even = 0.5f;
    Grain grain = Grain::None;
    f32 stretch = 3.0f;
    f32 smallest = 0.1f;
    OpenParts openParts = OpenParts::Solid;
    f32 thickness = 0.0f;        ///< *Thicken*'s; 0 is 2 % of each target's size.
    u32 inside = kInvalidIndex;  ///< A material slot for the inside faces; none is the outside's.
    f32 uvScale = 1.0f;
    bool keepWhole = true;       ///< *Keep the whole meshes* (D1).
    /// The seeds to break along, model space; empty draws them. A re-break
    /// passes the recipe's, so the pieces do not move with the library.
    std::vector<Vector3f> seeds;
    u32 threads = 0;
};

struct FracturePiece {
    u32 node = kInvalidNode;   ///< Its bone.
    u32 parent = kInvalidNode; ///< The node its vertices rode most.
    Vector3f centroid{0, 0, 0};
    f32 volume = 0.0f;
    f32 area = 0.0f;
    bool solid = false;
    std::vector<u32> cells;    ///< The seeds whose cells it fills; none for a Whole target.
    std::vector<u32> targets;  ///< Into the spec's targets.
};

struct FractureResult {
    Refusal refusal = Refusal::None;
    u32 refusedTarget = kInvalidIndex; ///< Into the spec's targets.
    /// Per target, two: its outside mesh, then its inside mesh or `kInvalidIndex`.
    std::vector<u32> made;
    /// Per target, its whole mesh, or `kInvalidIndex` with `keepWhole` off.
    std::vector<u32> sources;
    std::vector<FracturePiece> pieces;
    u32 helper = kInvalidNode;  ///< Between the pieces and their parent, or none.
    /// The nodes `fracture.skin.node` names, by index + 1.
    std::vector<u32> skinNodes;
    std::vector<Vector3f> seeds; ///< Every seed broken along, model space.
    std::vector<PieceLink> links;
    u32 trianglesBefore = 0;
    u32 trianglesAfter = 0;
    u32 failedFaces = 0;
    u32 merged = 0;

    bool ok() const {
        return refusal == Refusal::None;
    }
};

/**
 * @brief Breaks @p spec's targets of model @p model.
 *
 * Each target becomes its pieces' outside, cut, in place: name, material and
 * section settings kept, `rigidNode` cleared, every vertex bound wholly to its
 * piece's bone, fresh merge groups, and the rejoin layers. Inside faces go in
 * the target, or in a mesh "<name> Inside" when @p spec names another
 * material. With `keepWhole` the targets as they were are added after, named
 * "<name> (whole)", every section's `profiles` 0. Nodes are appended: the
 * helper, then a bone per piece. Nothing is written when it refuses.
 */
FractureResult BreakModel(Document& document, u32 model, const FractureSpec& spec);

/// What `RejoinPieces` had to leave as it found it.
struct RejoinReport {
    u32 fragments = 0; ///< Fragments moved off their face's plane, left as faces.
    u32 cutPoints = 0; ///< Cut points that are no longer on a straight edge.
};

/**
 * @brief Rebuilds a broken mesh into the one it was cut from (§6.4).
 *
 * Drops the faces the fracture made; welds the cut points by position; merges
 * the fragments of each source face that still lie in one plane into it and
 * dissolves the cut points left on a straight edge whose corner values are
 * linear along it; gives back each point's merge group and its skin, through
 * @p skinNodes; clears the six layers. A mesh with no `fracture.source` layer
 * is left alone.
 */
RejoinReport RejoinPieces(Mesh& mesh, std::span<const u32> skinNodes);

/// Shows the kept whole mesh @p whole again, with @p made's name and section
/// profiles.
void RestoreSource(Mesh& whole, const Mesh& made);

} // namespace fracture
} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
