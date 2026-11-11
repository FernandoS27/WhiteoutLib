// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file boolean.h
 * @brief Union, intersection and subtraction of meshes
 *        (EDIT_MODE_BOOLEAN_DESIGN.md §2-§4).
 *
 * Every face is cut where it crosses ANOTHER mesh, never where a mesh crosses
 * itself. The pieces between the cuts are patches, and each patch is read on
 * its own against each other mesh: on its face (the same way or the opposite
 * way, decided exactly), or inside or outside it by that mesh's generalized
 * winding number at points within the patch. The operation keeps the patches
 * its table names (§2.3), turning a subtracted operand's.
 *
 * Every topological decision is an exact sign on one integer grid shared by
 * every mesh; every point the cut makes is placed once, from its key, so all
 * the faces that hold it hold the same bits. The surface is cut, never
 * resampled: corner layers and the skin are blended where a face is cut.
 *
 * A mesh that holds no volume (a sheet) is cut but never encloses anything:
 * a sheet target is trimmed with no walls, and a sheet operand is refused
 * for Subtract and Intersect (§2.4).
 */

#include <span>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/vector_types.h>

#include "mesh.h"
#include "kernel.h"

namespace whiteout {
namespace models {
namespace wem {
namespace geom {

enum class BooleanOp : u8 { Union, Intersect, Subtract };

/// Where an operand's kept faces go (Q3).
enum class OperandFaces : u8 {
    JoinTarget, ///< Into the target's mesh, in its section, sharing the cut's points.
    OwnMesh,    ///< Into the operand's own mesh, in its own sections.
};

enum class BooleanRefusal : u8 {
    None,
    NoTarget,            ///< No input is a target.
    NoOperand,           ///< No input is an operand.
    OperandHoldsNothing, ///< Subtract, Intersect: an operand closes round nothing.
    NothingToSubtract,   ///< Subtract: no operand reaches any target.
    NothingLeft,         ///< A target would keep no face.
    CutFailed,           ///< A cut face's triangulation dropped a segment.
};

const char* ToString(BooleanRefusal refusal);

struct BooleanInput {
    const Mesh* mesh = nullptr; ///< At rest.
    bool operand = false;
};

struct BooleanOptions {
    BooleanOp op = BooleanOp::Subtract;
    OperandFaces faces = OperandFaces::JoinTarget;
    /// Subtract and Intersect: keep the operands' faces inside the target. Off
    /// leaves the cut open, as a cookie cutter would.
    bool closeCut = true;
    /// The crease rule along the cut (EDIT_MODE_MODELLING_DESIGN.md §3.12).
    f32 creaseAngle = kDefaultShadingAngle;
};

struct BooleanResult {
    BooleanRefusal refusal = BooleanRefusal::None;
    /// The input a refusal names, or `kInvalidId`.
    u32 refusedInput = kInvalidId;
    /// Per input, what it becomes. A target: what it keeps, with the operands'
    /// faces under `JoinTarget`. An operand: its kept faces under `OwnMesh`
    /// (a Union's trimmed operand), else empty: its faces went to a target.
    /// A Union under `JoinTarget` puts everything in the first target.
    std::vector<Mesh> meshes;
    /// Per input: its faces all went to another mesh, so it is used up.
    std::vector<u8> consumed;
    /// Per input: it holds a volume (§2.4).
    std::vector<u8> holds;
    /// The cut, two points per segment, for a preview to draw.
    std::vector<Vector3f> cutLines;
    u32 cutFaces = 0;    ///< Source faces a cut passed through.
    u32 patches = 0;
    u32 coplanar = 0;    ///< Patches lying on another mesh.
    u32 nearOpening = 0; ///< Operand patches read between ¼ and ¾ against a target.
    u32 failedFaces = 0; ///< `CutFailed`'s count.
    bool met = false;    ///< Some mesh crosses or touches another.

    bool ok() const {
        return refusal == BooleanRefusal::None;
    }
};

/**
 * @brief The boolean of @p inputs (EDIT_MODE_BOOLEAN_DESIGN.md §3).
 *
 * Subtract and Intersect cut each target on its own against every operand;
 * a Union treats every input alike. Each result ends with `FinishTool`: the
 * crease rule along the cut, seams derived, the faces beside it re-shaded.
 * Nothing in @p inputs is changed.
 */
BooleanResult BooleanMeshes(std::span<const BooleanInput> inputs, const BooleanOptions& options);

} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
