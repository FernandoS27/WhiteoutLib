// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/geometry/uv/checks.h>

#include <whiteout/models/wem/geometry/uv/flatten.h>
#include <whiteout/models/wem/geometry/uv/layout.h>

#include "common.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace whiteout {
namespace models {
namespace wem {
namespace geom {
namespace uv {

UvChecks CheckUv(std::span<const UvCheckInput> inputs, const UvCheckOptions& options) {
    UvChecks out;
    const u32 resolution = std::max<u32>(1, options.resolution);

    // Every island of every input by one number, and the group each is in for
    // overlap: its stack, else itself.
    std::vector<u32> base(inputs.size() + 1, 0);
    for (u32 i = 0; i < inputs.size(); ++i) {
        const bool usable = inputs[i].mesh && inputs[i].islands && inputs[i].mesh->hasConnectivity();
        base[i + 1] = base[i] + (usable ? inputs[i].islands->count : 0);
    }
    const u32 total = base.back();
    std::vector<u32> groupOf(total, kInvalidId);
    for (u32 i = 0; i < inputs.size(); ++i) {
        if (base[i + 1] == base[i]) {
            continue;
        }
        for (const std::vector<u32>& stack : FindStacks(*inputs[i].mesh, *inputs[i].islands, inputs[i].set)) {
            ++out.stacks;
            for (const u32 island : stack) {
                groupOf[base[i] + island] = base[i] + stack.front();
            }
        }
    }
    for (u32 g = 0; g < total; ++g) {
        if (groupOf[g] == kInvalidId) {
            groupOf[g] = g;
        }
    }

    // --- overlap: one owner per texel, what the texture would show -----------
    std::vector<u32> owner(static_cast<std::size_t>(resolution) * resolution, kInvalidId);
    std::vector<u8> overlaps(total, 0);
    std::unordered_set<u64> pairs;
    const f32 texels = static_cast<f32>(resolution);
    for (u32 i = 0; i < inputs.size(); ++i) {
        if (base[i + 1] == base[i]) {
            continue;
        }
        const Mesh& mesh = *inputs[i].mesh;
        const UvIslands& islands = *inputs[i].islands;
        const std::span<const Vector2f> uvs =
            mesh.attributes.get<const Vector2f>(names::uv(inputs[i].set), Domain::Halfedge);
        if (uvs.empty()) {
            continue;
        }
        for (u32 island = 0; island < islands.count; ++island) {
            const u32 me = base[i] + island;
            for (const u32 face : islands.facesOf(island)) {
                for (const detail::Tri& tri : detail::TrianglesOf(mesh, FaceId(face))) {
                    const auto at = [&](u32 k) { return uvs[tri.corner[k].index()] * texels; };
                    detail::RasterTriangle(resolution, resolution, at(0), at(1), at(2), false, [&](u32 x, u32 y) {
                        u32& slot = owner[static_cast<std::size_t>(y) * resolution + x];
                        if (slot == kInvalidId) {
                            slot = me;
                        } else if (groupOf[slot] != groupOf[me]) {
                            overlaps[slot] = 1;
                            overlaps[me] = 1;
                            out.overlapTexels.push_back(y * resolution + x);
                            pairs.insert((static_cast<u64>(std::min(slot, me)) << 32) | std::max(slot, me));
                        }
                    });
                }
            }
        }
    }

    std::sort(out.overlapTexels.begin(), out.overlapTexels.end());
    out.overlapTexels.erase(std::unique(out.overlapTexels.begin(), out.overlapTexels.end()), out.overlapTexels.end());

    // Global numbers back to inputs, for the pairs.
    const auto refOf = [&](u32 global) {
        const u32 i = static_cast<u32>(std::upper_bound(base.begin(), base.end(), global) - base.begin()) - 1;
        return UvIslandRef{i, global - base[i]};
    };
    std::vector<u64> ordered(pairs.begin(), pairs.end());
    std::sort(ordered.begin(), ordered.end());
    for (const u64 pair : ordered) {
        out.overlapPairs.emplace_back(refOf(static_cast<u32>(pair >> 32)), refOf(static_cast<u32>(pair)));
    }

    // --- the rest, island by island ------------------------------------------
    std::vector<f32> densities(total, 0.0f);
    for (u32 i = 0; i < inputs.size(); ++i) {
        if (base[i + 1] == base[i]) {
            continue;
        }
        const Mesh& mesh = *inputs[i].mesh;
        const UvIslands& islands = *inputs[i].islands;
        const u32 set = inputs[i].set;
        const std::span<const Vector3f> positions =
            mesh.attributes.get<const Vector3f>(names::kPosition, Domain::Vertex);
        const std::span<const Vector2f> uvs = mesh.attributes.get<const Vector2f>(names::uv(set), Domain::Halfedge);
        const std::vector<f32> stretch = FaceStretch(mesh, islands, set);
        const Topology& topology = mesh.topology();
        for (u32 island = 0; island < islands.count; ++island) {
            const UvIslandRef ref{i, island};
            if (overlaps[base[i] + island]) {
                out.overlapping.push_back(ref);
            }
            const detail::IslandArea areas = detail::AreasOf(mesh, islands, island, positions, uvs);
            if (areas.uv <= 1e-12f) {
                out.noMap.push_back(ref);
                continue;
            }
            // Against the island's own majority, by area: a whole mirrored
            // island agrees with itself.
            f32 positive = 0.0f;
            f32 negative = 0.0f;
            bool outside = false;
            bool stretched = false;
            for (const u32 face : islands.facesOf(island)) {
                for (const detail::Tri& tri : detail::TrianglesOf(mesh, FaceId(face))) {
                    const f32 area = detail::TriAreaUv(uvs[tri.corner[0].index()], uvs[tri.corner[1].index()],
                                                       uvs[tri.corner[2].index()]);
                    (area > 0.0f ? positive : negative) += std::abs(area);
                }
                for (const HalfedgeId h : topology.fh(FaceId(face))) {
                    const Vector2f& p = uvs[h.index()];
                    outside = outside || p.x < -1e-3f || p.y < -1e-3f || p.x > 1.001f || p.y > 1.001f;
                }
                stretched = stretched || (face < stretch.size() && stretch[face] > options.stretchThreshold);
            }
            const f32 minority = std::min(positive, negative);
            if (minority > (positive + negative) * 1e-6f) {
                out.flipped.push_back(ref);
            }
            if (stretched) {
                out.stretched.push_back(ref);
            }
            if (outside && !options.wraps) {
                out.outside.push_back(ref);
            }
            if (island < islands.loops.size() && islands.loops[island] >= 2) {
                out.rings.push_back(ref);
            }
            densities[base[i] + island] = TexelDensity(mesh, islands, island, set, resolution);
        }
    }

    // --- density against the median of everything with a map ----------------
    std::vector<f32> sorted;
    for (const f32 d : densities) {
        if (d > 0.0f) {
            sorted.push_back(d);
        }
    }
    if (!sorted.empty()) {
        std::sort(sorted.begin(), sorted.end());
        const f32 median = sorted[sorted.size() / 2];
        for (u32 i = 0; i < inputs.size(); ++i) {
            for (u32 island = 0; island < base[i + 1] - base[i]; ++island) {
                const f32 d = densities[base[i] + island];
                if (d > 0.0f && (d < median * 0.5f || d > median * 2.0f)) {
                    out.density.push_back(UvIslandRef{i, island});
                }
            }
        }
    }
    return out;
}

} // namespace uv
} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
