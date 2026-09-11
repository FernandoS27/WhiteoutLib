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

#include <map>
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

    /// Section-visibility gate bones. A Warcraft III geoset animation hides a
    /// whole SECTION, and the `.m3` spelling of that is a bone whose animated
    /// visibility gates the batch (`Batch::boneCount` -- Blizzard's own
    /// conversions gate the Footman's gore exactly this way). `toM3` allocates
    /// one bone per gated visibility channel and records it here; the anim
    /// export then wires the channel's SDFG stream to that bone's AnimRef.
    std::map<u32 /*channel id*/, u32 /*bone index*/> sectionGateBones;

    /// The FADES: a section alpha that is not a binary step cannot gate a
    /// batch, so it rides the material instead -- a Color-flag alpha layer
    /// whose `mapAlpha` carries the keys (float, the same Sdr3 stream the
    /// channel already writes; Blizzard's own conversions animate exactly
    /// these layers). `toM3` plants the carrier layer and records which
    /// standard material and slot (1 = alphaLayer1, 2 = alphaLayer2) it
    /// took; the anim export wires the AnimRef.
    /// One carrier per standard material the section draws with -- a
    /// composite slot has one per section.
    std::map<u32 /*channel id*/, std::vector<std::pair<u32 /*standard material*/, u8 /*slot*/>>>
        sectionAlphaLayers;

    /// The live/dead coverage switch (WC3_SD_MATERIAL_TO_SC2_DESIGN.md §5.3
    /// R6b): per slot, the ordinal whose alpha track drives the first
    /// section's `alphaLayer1.rgbAdd` instead of a carrier.
    std::map<u32 /*slot*/, u32 /*ordinal*/> coverageSwitches;

    /// Per exported material map entry: the source-body ordinal each
    /// `m3_core::StandardLayer` took, as `ExportMaterial` reported it
    /// (`kInvalidIndex` where none). An empty entry falls back to recovering
    /// the map by re-importing what was written — exact only while slot
    /// enumeration order matches the body's ordinal order, which a combiner
    /// chain seeded by an env-mapped stage breaks.
    std::vector<std::vector<u32>> materialOrdinals;
};

/// Derives `BONE.flags`' animation bits from the bound AnimRefs on each bone —
/// `sub_141EAD8F0`, the Galaxy editor's own solver, transcribed. Bits are OR'd
/// in, never assigned, so an `.m3` source keeps whatever it restored.
///
/// The editor runs this itself only while `MODL.flags` lacks
/// `BoneAnimatedFlagSolved`, and it runs it BEFORE the pass that repairs
/// `AnimRef.flags` — then latches the result. So a file that leaves both at
/// zero has every bone marked un-animated for good, and the model stands in
/// its bind pose whatever its tracks say, mesh and shadow alike.
void SolveBoneAnimFlags(m3::Model& out);

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
