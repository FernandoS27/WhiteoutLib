// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file seeds.h
 * @brief Where the Fracture's pieces are seeded (EDIT_MODE_FRACTURE_DESIGN.md
 *        §5.1).
 *
 * Candidates are drawn in each target's box and kept when they are inside its
 * solid (winding at least ½) or in its sheet band, so solids seed inside and
 * sheets along their surface. *Smaller near the blast* thins the draw away
 * from the blast, and *Even* runs k-means rounds over the candidates. Every
 * draw is a counter-based hash of the seed, the target and the index, never a
 * global generator, so a seed gives the same pieces everywhere.
 */

#include <span>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/vector_types.h>

#include "cells.h"
#include "winding.h"

namespace whiteout {
namespace models {
namespace wem {
namespace geom {

class TriangleBvh;

namespace fracture {

/// SplitMix64 of @p seed, @p target and @p index.
u64 FractureHash(u32 seed, u32 target, u64 index);

/// A hash as a number in [0, 1).
f64 HashUnit(u64 hash);

struct SeedTarget {
    const WindingMesh* winding = nullptr;
    const TriangleBvh* surface = nullptr; ///< For the sheet band.
    u32 pieces = 1;                       ///< 1 is Whole: no seeds.
};

struct SeedOptions {
    u32 seed = 1;
    f64 nearBlast = 0.4; ///< *Smaller near the blast*, 0..1.
    Vector3d blastCentre{0, 0, 0};
    f64 blastRadius = 0.0; ///< 0: no blast, so no thinning.
    f64 even = 0.5;        ///< 0..1; round(4 × even) k-means rounds.
    Grain grain = Grain::None;
    f64 stretch = 3.0;
    u32 threads = 0;
};

/// The seeds of every target, target by target: `pieces` each when the draw
/// keeps that many candidates, fewer when it cannot.
std::vector<Vector3d> FractureSeeds(std::span<const SeedTarget> targets, const SeedOptions& options);

} // namespace fracture
} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
