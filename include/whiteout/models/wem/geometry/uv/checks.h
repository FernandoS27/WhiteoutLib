// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file checks.h
 * @brief What is wrong with a layout (EDIT_MODE_UV_REDESIGN.md §11).
 *
 * Each row names the islands it is about, so an editor can select them and a
 * Fix can run on them. Every mesh that draws one image is checked on one map,
 * because two meshes on one texture overlap as surely as two islands of one
 * mesh do.
 *
 * Two things a file does on purpose are not problems: islands whose UV points
 * coincide are a stack (`FindStacks`), counted apart and never an overlap, and
 * a whole island mirrored in the map is not flipped -- only a triangle turned
 * against the rest of its own island is.
 */

#include <span>
#include <utility>
#include <vector>

#include <whiteout/common_types.h>

#include "../mesh.h"
#include "islands.h"

namespace whiteout {
namespace models {
namespace wem {
namespace geom {
namespace uv {

struct UvCheckInput {
    const Mesh* mesh = nullptr;
    const UvIslands* islands = nullptr;
    u32 set = 0;
};

struct UvCheckOptions {
    /// The grid overlap is rasterised on: the texture's texels, or the pack's.
    u32 resolution = 1024;
    /// Faces stretched past this against their island's own scale.
    f32 stretchThreshold = 1.5f;
    /// The texture tiles, so a UV outside `[0, 1]` is where it means to be.
    bool wraps = false;
};

/// One island of one input.
struct UvIslandRef {
    u32 input = 0;
    u32 island = 0;

    friend bool operator==(const UvIslandRef&, const UvIslandRef&) = default;
};

struct UvChecks {
    /// Islands with a texel another island's covers too, stacks excepted.
    std::vector<UvIslandRef> overlapping;
    /// Which with which, each pair once: what a Fix needs to tell the file's
    /// own overlaps from one an edit made.
    std::vector<std::pair<UvIslandRef, UvIslandRef>> overlapPairs;
    /// The texels a second island lands on, `y * resolution + x`, sorted and once
    /// each: the overlap as the texture would show it.
    std::vector<u32> overlapTexels;
    /// Islands with a triangle turned against the island's own majority.
    std::vector<UvIslandRef> flipped;
    /// Islands with a face past `stretchThreshold`.
    std::vector<UvIslandRef> stretched;
    /// Islands with a UV outside the tile. Empty when the texture wraps.
    std::vector<UvIslandRef> outside;
    /// Islands with no map: every corner at the origin, as new geometry and a
    /// cleared set have them (`IslandHasNoMap`).
    std::vector<UvIslandRef> noMap;
    /// Islands with no UV area anywhere else: faces laid on one texel for a
    /// flat colour, or on a line for a gradient -- on purpose, so
    /// informational, and nothing unwraps them unasked (EDIT_MODE_UV_AUDIT.md U3).
    std::vector<UvIslandRef> flatColour;
    /// Islands with two boundary loops or more: a tube open at both ends,
    /// which flattens into an annulus.
    std::vector<UvIslandRef> rings;
    /// Islands under half or over twice the median density. Informational.
    std::vector<UvIslandRef> density;
    /// How many stacks the file laid on purpose.
    u32 stacks = 0;

    /// The first five rows: what the Check badge counts.
    u32 Problems() const {
        return static_cast<u32>(overlapping.size() + flipped.size() + stretched.size() + outside.size() +
                                noMap.size());
    }
};

/// Every row over @p inputs, which draw one image and so share one map.
UvChecks CheckUv(std::span<const UvCheckInput> inputs, const UvCheckOptions& options = {});

/// No map (EDIT_MODE_UV_AUDIT.md U3): every corner of @p island at the origin,
/// what `EnsureUvSet` zero-fills and Clear leaves. Zero area anywhere else is
/// a flat colour, which an artist laid out.
bool IslandHasNoMap(const Mesh& mesh, const UvIslands& islands, u32 island, u32 set);

} // namespace uv
} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
