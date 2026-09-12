// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file mdx_m3_effects.h
 * @brief Warcraft III particle and ribbon emitters into StarCraft II `PAR_` / `RIB_`.
 *
 * WEM stores an emitter's node -- where it is, how it moves, when it hides --
 * and deliberately not the system it runs (WEM_DESIGN §18). So these records
 * cross native to native, from the parsed `.mdx` into the `.m3` `toM3` just
 * wrote, joined through the node's `objectId` and `M3ExportMap`
 * (WC3_TO_SC2_COMPLETION_PLAN.md §2). The crossing never makes a bone or a
 * transform: every record rides a bone `toM3` made under
 * `M3ExportSettings::effectNodeBones`, and every keyed property is cut into the
 * same clip windows the node's own tracks were, then written through the one
 * stream writer the animation export uses.
 *
 * What each field becomes, and why, is the plan's §4.4 (particles) and §4.5
 * (ribbons); the fields no row names rest where Blizzard's tools write them
 * (§4.6), so the Galaxy editor sees a record of a shape it knows.
 */

#include <string>
#include <vector>

#include <whiteout/models/m3/structures.h>
#include <whiteout/models/mdx/structures.h>
#include <whiteout/models/wem/converters.h>
#include <whiteout/models/wem/diagnostics.h>
#include <whiteout/models/wem/document.h>

namespace whiteout {
namespace models {
namespace cross {

/// What the caller decides that the source does not say.
struct Wc3EffectOptions {
    /// Per `mdx::Model::textures` entry: the path the written model names that
    /// texture by (the driver's texture plan). An empty entry -- or a list
    /// shorter than the table -- leaves an emitter on that texture untextured,
    /// with a diagnostic.
    std::vector<std::string> texturePaths;
    /// Warcraft III's team glow with its shape in ALPHA, for an emitter whose
    /// replaceable id 2 takes its texture's place. Empty: such an emitter lights
    /// its own texture's alpha instead (Blizzard's Wisp).
    std::string teamGlowMaskPath;
    /// Warcraft III units to the written model's: the staging rescale, 0.01 for
    /// StarCraft II. Every length a record states is multiplied by it.
    f32 lengthScale = 0.01f;
    bool particles = true;
    bool ribbons = true;
    /// Each CAM_ aimed at its Warcraft III target, with Blizzard's field of view.
    bool cameras = true;
    /// Each sphere or box collision shape (CLID) as a fuzzy hit test.
    bool hitTests = true;
    /// Per model-spawning emitter (PREM, by index), the path of the `.m3` the
    /// export wrote for the model it spawns -- as the written model names it.
    /// An empty or missing entry drops that emitter with a diagnostic.
    std::vector<std::string> modelParticlePaths;
};

struct Wc3EffectReport {
    u32 particleRecords = 0; ///< PAR_ written (a head-and-tail emitter writes two).
    u32 modelParticleRecords = 0; ///< Of those, model particles from PREM.
    u32 ribbonRecords = 0;   ///< RIB_ written.
    u32 cameraRecords = 0;   ///< CAM_ bones turned toward their targets.
    u32 hitTests = 0;        ///< Fuzzy hit tests written for collision shapes.
    u32 materials = 0;       ///< MAT_ + MATM entries added for them.
    u32 keyedStreams = 0;    ///< SD blocks written for keyed properties.
    wem::Diagnostics diagnostics;
};

/// Before staging: the document edits the records need. A squirting emitter's
/// visibility track is removed -- Warcraft III's bursts ignore it and StarCraft
/// II's obey the bone's (§A9) -- with a diagnostic naming what was dropped.
///
/// And every node composes onto its parent again. The import reads a PRE2's
/// `ModelSpace` bit as WEM's `NodeFlags::ModelSpace` (the node's local is its
/// world), but Warcraft III's node update never reads that bit: the particle
/// system does, as the space its particles live in, which the crossing takes
/// off the source record. Staged with the flag, the bone keeps its parent and
/// a local that is already its world, and lands its parent's offset away
/// (War3 Wisp's emitters, 69 units above the body).
///
/// With @p ribbonOffsets, a ribbon whose two heights differ gets a helper node
/// half their difference across its width, carrying the ribbon's visibility
/// (§4.5 row 2): StarCraft II centres a strip on the bone it rides.
void PrepareWc3Effects(const mdx::Model& source, wem::Document& document,
                       wem::Diagnostics& diagnostics, bool ribbonOffsets = true);

/// After `toM3`: appends a `PAR_` per particle emitter (two for head and tail,
/// a model particle per model-spawning emitter whose model was written),
/// a `RIB_` per ribbon, their materials, and their keyed streams to @p out,
/// turns each camera's bone toward its target (its rest, its IREF, and a
/// rotation stream where the camera, its target or its roll is keyed), and
/// writes a fuzzy hit test per sphere or box collision shape on its node's bone.
///
/// @p staged is the document `toM3` was given and @p map what it reported; the
/// emitter nodes must have bones (`M3ExportSettings::effectNodeBones`).
Wc3EffectReport CrossWc3Effects(const mdx::Model& source, const wem::Document& staged,
                                const wem::M3ExportMap& map, const Wc3EffectOptions& options,
                                m3::Model& out);

} // namespace cross
} // namespace models
} // namespace whiteout
