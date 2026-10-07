// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file fracture.h
 * @brief A model's meshes broken into pieces, and put back
 *        (EDIT_MODE_FRACTURE_DESIGN.md §6, §13.2).
 *
 * `BreakModel` runs the whole break on a document: the seeds and their cells,
 * or the regions of slice planes; the cut; then each target replaced by its
 * pieces, one bone per piece, and the rejoin layers that lead back to the
 * source. Physics is the viewer's.
 *
 * A piece is rigid, bound wholly to its bone; or, with the skeleton cut, it
 * keeps its skin: its bone is a copy of the one that carries most of it, with
 * copies of the bones under that one (EDIT_MODE_FRACTURE_NEXT_DESIGN.md §12).
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
    TooMany,   ///< *Slices* made more pieces, or regions, than the limit.
};

const char* ToString(Refusal refusal);

/// Why @p mesh of @p model cannot be a target, or `Refusal::None`.
Refusal RefuseTarget(const Model& model, u32 mesh);

/// *Open parts* (§5.7).
enum class OpenParts : u8 { Solid, Thicken };

/// How the targets are broken (EDIT_MODE_FRACTURE_NEXT_DESIGN.md §3).
enum class Method : u8 {
    Fracture, ///< Along Voronoi cells of drawn seeds.
    Slices,   ///< Along planes.
};

/// The filling's UVs (EDIT_MODE_FRACTURE_NEXT_DESIGN.md §5.3).
enum class FillingUvs : u8 {
    Tiled,  ///< Each cell face flat at the outside's density, for a texture that tiles.
    Square, ///< Every crack an island, all packed into 0-1 at one scale.
};

struct FractureTarget {
    u32 mesh = kInvalidIndex;
    /// 1 is Whole: not cut, one piece. 0 takes a share of `FractureSpec::total`.
    /// *Slices* reads only whether it is 1.
    u16 pieces = 16;
};

/// Parallel planes *Slices* cuts along (EDIT_MODE_FRACTURE_NEXT_DESIGN.md §3.1).
struct SliceSet {
    Vector3f direction{0, 0, 1}; ///< The planes' normal; any length but none.
    u16 count = 3;      ///< Spread evenly across the targets along `direction`.
    f32 jitter = 0.0f;  ///< 0..1: each plane moved by up to this share of half a gap, by the seed.
    f32 tilt = 0.0f;    ///< Radians: each plane leaned by up to this, by the seed.
    f32 shift = 0.0f;   ///< All of them moved along `direction`, model units.
};

/// The points with normal · x = offset, and how far along them the cut goes.
struct SlicePlane {
    Vector3f normal{0, 0, 1};
    f32 offset = 0.0f;
    /// Where it cuts: a rectangle on the plane about `centre`, `width` to each
    /// side along `along` and `height` across it. A width or height of 0 cuts
    /// everywhere, as does a rectangle that holds all the plane has of the
    /// targets' box.
    Vector3f centre{0, 0, 0};
    Vector3f along{0, 0, 0};
    f32 width = 0.0f;
    f32 height = 0.0f;
    bool operator==(const SlicePlane&) const = default;
};

/// The most pieces *Slices* makes, and the most regions it cuts along.
inline constexpr u32 kMostSlicePieces = 250;
inline constexpr u32 kMostSliceRegions = 4096;

/// The planes @p sets make across the box from @p low to @p high: each set's
/// spread evenly along its direction, then shifted, jittered and tilted by
/// @p seed. A tilted plane turns about its own point on the box's axis.
std::vector<SlicePlane> SlicePlanes(std::span<const SliceSet> sets, u32 seed, const Vector3f& low,
                                    const Vector3f& high);

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
    Method method = Method::Fracture;
    /// *Pieces*: what the targets whose `pieces` is 0 share, by size.
    u16 total = 16;
    std::vector<SliceSet> slices;
    /// The planes to slice along, model space; empty makes them of `slices`.
    /// A re-break passes the recipe's, as it passes its seeds.
    std::vector<SlicePlane> planes;
    /// `Square` needs `inside`: laid over the outside's own UVs it would cover
    /// them, so without it the filling is tiled.
    FillingUvs fillingUvs = FillingUvs::Tiled;
    u32 mapSize = 1024; ///< The texture `Square` counts its padding for.
    /// The most vertices one section may be written with. A file indexes a
    /// geoset's, a region's or a skin section's in 16 bits and writes a point
    /// once per set of corner values it carries; a made section past this is
    /// divided into sections like it, whole pieces to each. 0 never divides.
    u32 mostVertices = 0xFFFFu;
    /// The skeleton cut (EDIT_MODE_FRACTURE_NEXT_DESIGN.md §12): a piece keeps
    /// its skin, on a copy of the bone that carries it and of the bones under
    /// that one, each with its keys. Off, a piece is rigid on one bone.
    bool cutSkeleton = false;
    u32 threads = 0;
};

struct FracturePiece {
    u32 node = kInvalidNode;   ///< Its bone.
    /// The node its vertices rode most; with the skeleton cut, its bone's.
    u32 parent = kInvalidNode;
    /// The skeleton cut: the copies under its bone, of the nodes its points
    /// rode there and those between. Its bone is itself a copy, of the node
    /// that carries the piece.
    std::vector<u32> bones;
    /// The skeleton cut: the node its bone copies, or none for a bone of its
    /// own at the root; and per bone of `bones`, the node that one copies.
    u32 carrier = kInvalidNode;
    std::vector<u32> sources;
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
    /// Per target, the pieces asked of it: its own count, or its share.
    std::vector<u16> shares;
    /// What was asked: the seeds drawn and one per Whole target, or for
    /// *Slices* the regions. The pieces are `asked + split - empty - merged`.
    u32 asked = 0;
    u32 split = 0; ///< Pieces more than cells: a cell through parts that do not touch.
    u32 empty = 0; ///< Cells that held nothing.
    std::vector<SlicePlane> planes; ///< *Slices*: every plane asked for.
    /// *Slices*: per plane, 1 when it parted something, a crack lying on it.
    /// One that misses the targets, or ends inside what it crosses, parts
    /// nothing.
    std::vector<u8> planeParts;
    /// *One square*: how many islands were packed, and UV units per model unit
    /// (times the map's size, texels per unit). 0 when the filling is tiled.
    u32 fillingIslands = 0;
    f32 fillingDensity = 0.0f;
    u32 sections = 0; ///< Sections added to keep each within `mostVertices`.
    u32 joined = 0;   ///< Pieces fewer: two a plane's reach did not part.
    /// The skeleton cut: influences on a bone their piece has no copy of,
    /// given to the piece's own bone. With none, every piece draws as the
    /// whole mesh did, in every clip, until its bone is moved.
    u32 rebound = 0;

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
 * material, where `FillingUvs::Square` packs them into one tile. With
 * `keepWhole` the targets as they were are added after, named
 * "<name> (whole)", every section's `profiles` 0. Nodes are appended: the
 * helper, then a bone per piece. A made section that would be written with
 * more than `mostVertices` is divided, its `Section` tracks stated again for
 * each part. Nothing is written when it refuses.
 *
 * With `cutSkeleton` there is no helper, and a piece keeps its skin. Its bone
 * is a copy of the node that carries it, the lowest whose bones hold three
 * quarters of the piece, under that node's parent; under it, copies of the
 * nodes down to the bones its points rode, each with its source's transform
 * tracks in every clip. So one bone moves the whole piece, and its limbs play
 * on under it. What a point rode of a bone outside that node goes to the
 * piece's bone (`rebound`): at the rim of a cut a piece is rigid with its own
 * side, and draws as the whole mesh did everywhere else. Where no one node
 * carries the piece, or a point rode nothing, its bone is a new one at the
 * root and every bone it rode is copied under it. An inside face's point
 * takes the skin of the piece's nearest point on the outside.
 */
FractureResult BreakModel(Document& document, u32 model, const FractureSpec& spec);

/// Per node of @p count, the piece of @p result it carries, as its bone or as
/// a bone of its cut skeleton; `kInvalidIndex` for any other.
std::vector<u32> PieceOfNode(const FractureResult& result, u32 count);

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
 * @p skinNodes; makes the sections `BreakModel` divided one again; clears the
 * fracture's layers. A mesh with no `fracture.source` layer is left alone.
 * The sections' referencers are the caller's (`SectionsBefore`).
 */
RejoinReport RejoinPieces(Mesh& mesh, std::span<const u32> skinNodes);

/// Where each of @p made's sections is once the fracture is out: its number
/// then, or `kInvalidIndex` for one `BreakModel` added. What
/// `FollowSectionRemap` takes, for a target rejoined or given back whole.
std::vector<u32> SectionsBefore(const Mesh& made);

/// Shows the kept whole mesh @p whole again, with @p made's name and section
/// profiles.
void RestoreSource(Mesh& whole, const Mesh& made);

} // namespace fracture
} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
