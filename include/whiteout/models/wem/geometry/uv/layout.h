// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file layout.h
 * @brief Where an island sits in the tile (EDIT_MODE_UV_DESIGN.md §7, §8).
 *
 * Density first: an island's texel density is the only unit in which two
 * islands of a model are comparable, and matching it is what makes one texture
 * read evenly over a whole model. Then the tile: what the file placed is
 * locked and is never moved, so a pack is always a pack of the free islands
 * around the fixed ones.
 *
 * A new island is born in the **staging strip**, `u ∈ [1, 2)`, at the median
 * density of what is already placed: outside the tile, so nothing it lands on
 * is covered, and beside it, so it can be seen and dragged in.
 */

#include <span>
#include <vector>

#include <whiteout/common_types.h>

#include "../ids.h"
#include "../mesh.h"
#include "islands.h"

namespace whiteout {
namespace models {
namespace wem {
namespace geom {
namespace uv {

/// Texels per world unit for @p island at @p resolution: what "the same
/// density" means when two islands are compared. Zero for an island with no
/// area either side.
f32 TexelDensity(const Mesh& mesh, const UvIslands& islands, u32 island, u32 set,
                 u32 resolution);

/// The median of `TexelDensity` over the islands, or over the locked ones
/// alone when @p lockedOnly -- which is what a new island should be born at,
/// because the locked ones are what the file shipped. Zero when there are
/// none.
f32 MedianDensity(const Mesh& mesh, const UvIslands& islands, u32 set, u32 resolution,
                  bool lockedOnly);

/// Scales @p island to @p density (texels per world unit at @p resolution) and
/// moves it into the next free row of the staging strip, `u ∈ [1, 2)`.
///
/// The strip is outside the tile on purpose: an island that has just been made
/// covers nothing that was already there, and is somewhere a modeller can see
/// it and drag it in. @p resolution is the density's.
void PlaceInStrip(Mesh& mesh, const UvIslands& islands, u32 island, u32 set, f32 density,
                  u32 resolution);

/// The bounds of @p island in UV, or a zero box.
struct UvBounds {
    Vector2f low{0.0f, 0.0f};
    Vector2f high{0.0f, 0.0f};
    bool empty = true;

    f32 width() const {
        return high.x - low.x;
    }
    f32 height() const {
        return high.y - low.y;
    }
};

UvBounds BoundsOf(const Mesh& mesh, const UvIslands& islands, u32 island, u32 set);

/// True when every face of @p island is free (`uvFreeN`). An island the file
/// placed is locked whole; a mixed one counts as locked, because moving half an
/// island is not a thing a layout may do.
bool IslandIsFree(const Mesh& mesh, const UvIslands& islands, u32 island, u32 set);

/// Moves @p island by @p delta and scales it about its own centre by @p scale.
/// The one writer of a placement, so every caller turns the same way.
void TransformIsland(Mesh& mesh, const UvIslands& islands, u32 island, u32 set, f32 scale,
                     const Vector2f& delta);

/// Turns @p island about its own centre by @p radians, then moves it.
void TurnIsland(Mesh& mesh, const UvIslands& islands, u32 island, u32 set, f32 radians,
                const Vector2f& delta);

/// Brings every island of @p islandList to @p density (at @p resolution), each
/// about its own centre so nothing moves that did not have to.
void MatchDensity(Mesh& mesh, const UvIslands& islands, std::span<const u32> islandList, u32 set,
                  f32 density, u32 resolution);

struct PackOptions {
    /// The map the layout is for: the grid the tile is rasterised on and what
    /// `padding` is counted in.
    u32 resolution = 1024;
    /// Texels of clear space round every island, so a mip does not bleed one
    /// island into its neighbour.
    u32 padding = 4;
    /// Turn an island to make it fit. Off, every island keeps the way round it
    /// already is.
    bool rotate = true;
    /// Keep islands that share their UV points together, as one piece. Off for
    /// a map that must give every surface its own texels, which is what a
    /// baked occlusion map needs (EDIT_MODE_UV_DESIGN.md §9.2).
    bool keepStacks = true;
};

struct PackResult {
    /// A locked island lies outside the tile: the layout tiles on purpose, and
    /// packing it into `[0, 1]` would be undoing what someone meant.
    bool refusedTiling = false;
    /// What the free islands had to be scaled by, together, to fit. 1 is "they
    /// fitted as they were".
    f32 scaled = 1.0f;
    u32 placed = 0;
    /// The share of the tile the layout covers afterwards, `[0, 1]`.
    f32 coverage = 0.0f;
};

/// Lays the free islands of @p set out in the tile, around the locked ones.
///
/// What the file placed is never moved (EDIT_MODE_UV_DESIGN.md §4): the locked
/// islands are obstacles the pack goes around, so a modeller can unwrap one new
/// part of a model without the rest of it moving under a texture that is
/// already painted.
PackResult Pack(Mesh& mesh, const UvIslands& islands, u32 set, const PackOptions& options = {});

/// Islands whose UV point sets coincide, as groups of two or more: the pairs a
/// file stacked on purpose so two parts share one patch of texture. A pack
/// moves each group as one thing.
std::vector<std::vector<u32>> FindStacks(const Mesh& mesh, const UvIslands& islands, u32 set);

struct StackResult {
    /// Faces of @p other that found no face of @p primary. Zero is the answer;
    /// anything else and nothing was written.
    u32 refusedFaces = 0;
    u32 primaryFaces = 0;
    u32 otherFaces = 0;
    /// True when the pairing came from the point mirror rather than the walk.
    bool byMirror = false;

    bool ok() const {
        return refusedFaces == 0 && otherFaces > 0;
    }
};

/// Lays @p other on @p primary corner for corner, so the two share one patch of
/// texture exactly (EDIT_MODE_UV_DESIGN.md §7.4).
///
/// Through the point mirror when @p pointMirror pairs the two -- which is what
/// a left and a right arm are -- and otherwise through a walk of the two
/// surfaces side by side from their longest boundary edge. Either way every
/// face must pair, because half a stack is worse than none.
StackResult Stack(Mesh& mesh, const UvIslands& islands, u32 primary, u32 other, u32 set,
                  std::span<const u32> pointMirror);

} // namespace uv
} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
