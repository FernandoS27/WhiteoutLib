// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file mdx_anim.h
 * @brief WC3 animation import (design §10.8.3, the MDX row).
 *
 * ### One timeline, N clips
 *
 * MDX has no per-sequence tracks. Every track in the file spans **one global
 * timeline in milliseconds**, and a `Sequence` is a window `[intervalStart,
 * intervalEnd]` onto it. So the import is a slice, once per sequence per track.
 *
 * The slice **keeps the two bracketing keys** — the last key before the window
 * and the first one after it — with their times rebased and therefore negative
 * or past the clip's end. That is what makes the clip evaluate identically to
 * the global track: WC3 interpolates across the window's edges, and a slice that
 * dropped the neighbours would flatten the first and last spans to a hold.
 * §10.8.2 already says a sub-track's times are not clamped to its clip, and this
 * is the case that needs it. Shipped exporters put keys on the boundaries, so on
 * most content the bracket keys are the boundary keys and nothing is added.
 *
 * A track keyed to a `globalSequenceId` is **not** sliced into the sequences: it
 * runs on its own clock, so it becomes one auto-play clip per global sequence,
 * flagged `AutoPlay | WorldClocked` — the same three-format unification M2's
 * global sequences and M3's SEQS flag 0x2 arrive at.
 *
 * ### What is deliberately not imported
 *
 * Particle, ribbon and corn emitter **property** tracks — emission rate,
 * lifespan, latitude, widths. §18 puts the systems themselves out of scope: WEM
 * stores the emitter's placement and an `AssetKey`, and animating a property of
 * a system it does not hold would be storing half of one. Emitter *visibility*
 * is imported, because that is a property of the node.
 */

#include <unordered_map>
#include <vector>

#include <whiteout/models/mdx/structures.h>
#include <whiteout/models/wem/diagnostics.h>
#include <whiteout/models/wem/document.h>

namespace whiteout {
namespace models {
namespace wem {
namespace mdx_anim {

/// The joins the `.mdx` does not carry, all of them decided by the node and
/// material import that ran first.
struct Context {
    /// `mdx::Node::objectId` -> `Model::nodes` index.
    const std::unordered_map<u32, u32>* byObjectId = nullptr;

    /// `mdx::Model::cameras[i]` -> `Model::nodes` index. Cameras have no
    /// `objectId`, so they cannot go through the map above.
    std::vector<u32> cameraNodes;

    /// Per profile, per `mdx::Model::materials[]`, per layer: the WEM ordinal
    /// that layer became, or `kInvalidIndex`. `mdx_core::ImportMaterial` fills
    /// it, because the ordinal is a position in the *filtered* stack.
    struct ProfileLayers {
        ProfileId profile = ProfileId::Generic;
        std::vector<std::vector<u32>> byMaterial;
    };
    std::vector<ProfileLayers> layerOrdinals;
};

/// Appends the clips, the channel table and the auto-play clips to
/// `document.models[model]` and `document.clips`.
void Import(const mdx::Model& source, const Context& context, Document& document, u32 model,
            Diagnostics& out);

} // namespace mdx_anim
} // namespace wem
} // namespace models
} // namespace whiteout
