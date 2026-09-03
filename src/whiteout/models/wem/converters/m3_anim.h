// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file m3_anim.h
 * @brief SC2 / Heroes animation import (design §10.8.3, the Sc2/Heroes row).
 *
 * **1:1**, because WEM's animation model is M3's. SEQS is a `Clip`, the STG_ at
 * the same index lists that clip's STC_s, an STC_ is a `SubTrackContainer` and
 * an SD entry is a `SubTrack`. Nothing is flattened and nothing is invented.
 *
 * What the import still has to get right, all of it measured elsewhere in this
 * tree and none of it visible in a green parse:
 *
 * - **Interpolation is AnimRef flags bit 4**, step iff set — never the STC
 *   track-table's `interpType` row, which lies at runtime. Reading that row
 *   juddered every `.m3a`-driven hero.
 * - **A keyed discrete channel is the SDFG slot**, 11, never slot 10. The wrong
 *   slot silently returns `initValue` forever on shipped content.
 * - **`animId` is the join key and it is kept verbatim.** That is what makes a
 *   later `.m3a` merge a concatenation rather than a re-resolution — the
 *   external file names channels this document already declares, and matches
 *   nothing by name.
 * - **SEQS flag `AlwaysGlobal` (0x2)** is the auto-play loop, started at
 *   anim-state init. It is never identified by a `"GL"` name.
 * - **The basis change is part of the value.** §6.4 canonicalises geometry, and
 *   a translation key is a vector in the basis being changed — so a track's
 *   values go through the same rebase the bone's rest transform did, or the
 *   animation plays in SC2's basis over WEM's geometry.
 *
 * ### What is not imported
 *
 * An `.m3` puts an `AnimRef` on nearly every field it has, most of them on
 * structures §18 keeps out of WEM: particle systems, forces, warps, projectors,
 * physics. Those are dropped silently — a diagnostic per AnimRef would be
 * hundreds per model and would say only "M3 animates more than WEM stores",
 * which this comment says once.
 */

#include <vector>

#include <whiteout/models/m3/structures.h>
#include <whiteout/models/wem/diagnostics.h>
#include <whiteout/models/wem/document.h>
#include <whiteout/models/wem/profile.h>

namespace whiteout {
namespace models {
namespace wem {
namespace m3_anim {

/// Where the node import put each satellite array — `ImportNodes`' own order:
/// bones, attachment points, lights, cameras, particle emitters, ribbons.
struct NodeBases {
    u32 bone = 0;
    u32 attachment = 0;
    u32 light = 0;
    u32 camera = 0;

    static NodeBases Of(const m3::Model& source);
};

struct Context {
    ProfileId profile = ProfileId::Sc2;
    NodeBases bases;

    /// Per `materialMaps` entry: the ordinal each `m3_core::StandardLayer`
    /// became, or `kInvalidIndex`. `m3_core::ImportMaterial` fills it.
    std::vector<std::vector<u32>> layerOrdinals;
};

void Import(const m3::Model& source, const Context& context, Document& document, u32 model,
            Diagnostics& out);

/// Where each WEM node landed in the `.m3` being written, and which profile's
/// materials are being written.
///
/// Same shape as the other two converters', and for the same reason: an M3
/// keeps a property's `AnimRef` ON the record that owns the property, so the
/// export has to know which array each node went into.
struct ExportContext {
    enum class Slot : u8 { None = 0, Bone, Attachment, Light, Camera };

    struct NodeSlot {
        Slot slot = Slot::None;
        u32 index = 0;
    };

    ProfileId profile = ProfileId::Sc2;
    /// Parallel to `Model::nodes`.
    std::vector<NodeSlot> nodeSlots;
};

/// Writes `document`'s clips back onto `out` as SEQS / STG_ / STC_ and the SD
/// blocks — the inverse of @ref Import.
void Export(const Document& document, u32 model, const ExportContext& context, m3::Model& out,
            Diagnostics& diagnostics);

/// Merges an external animation file into @p document.
///
/// The `.m3a` join is `animId` and nothing else: @p external's containers are
/// appended under clips of their own, referencing channels @p document already
/// declares. A channel the base model never declared is **skipped** — an `.m3a`
/// cannot introduce a target, only new motion for one that exists.
///
/// Returns how many clips were added.
u32 Merge(const m3::Model& external, Document& document, u32 model, Diagnostics& out);

} // namespace m3_anim
} // namespace wem
} // namespace models
} // namespace whiteout
