// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file wc3_sc2_emitters.h
 * @brief Warcraft III's emitter systems restated as StarCraft II's, inside WEM.
 *
 * `PRE2`, `PREM` and `RIBB` are node kinds of their own that only the Warcraft
 * III profiles carry (WEM_DESIGN §10.9). For a StarCraft II export each one is
 * restated as the `PAR_` or `RIB_` records that draw it most nearly: its node
 * becomes a `Sc2ParticleEmitter` or `Sc2RibbonEmitter`, its keyed properties
 * become that kind's `EmitterProperty` channels, and its look becomes a
 * StarCraft II material slot of its own. `toM3` then writes them the way it
 * writes any StarCraft II emitter, so the crossing reads nothing but the
 * document -- an `.mdx` import and a `.wem` of one cross alike.
 *
 * What each field becomes, and why, is WC3_TO_SC2_COMPLETION_PLAN.md §4.4
 * (particles) and §4.5 (ribbons); the fields no row names rest where
 * Blizzard's tools write them (§4.6), so the Galaxy editor sees a record of a
 * shape it knows.
 */

#include <map>
#include <string>
#include <utility>
#include <vector>

#include <whiteout/models/m3/structures.h>
#include <whiteout/models/wem/diagnostics.h>
#include <whiteout/models/wem/document.h>

namespace whiteout {
namespace models {
namespace cross {

/// What the caller decides that the document does not say.
struct Wc3EmitterOptions {
    /// The StarCraft II profile being written: the set the records' materials
    /// join.
    wem::ProfileId profile = wem::ProfileId::Sc2;
    /// Warcraft III units to the staged document's: the rescale staging applied.
    /// The payloads' lengths are already in the staged units; this restates the
    /// thresholds Warcraft III's engine states in its own.
    f32 lengthScale = 0.01f;
    /// The `Document::textures` entry holding Warcraft III's team glow with its
    /// shape in ALPHA, for an emitter whose replaceable id 2 takes its
    /// texture's place. `kInvalidIndex`: such an emitter lights its own
    /// texture's alpha instead (Blizzard's Wisp).
    u32 teamGlowMask = wem::kInvalidIndex;
    /// Per model a model-spawning emitter names (`Wc3ParticleEmitter1Payload::
    /// spawnModel`, as written there): the path of the `.m3` the export wrote
    /// for it, as the written model names it. An emitter whose model is missing
    /// here keeps only its placement, with a diagnostic.
    std::map<std::string, std::string> spawnedModels;
    bool particles = true;
    bool ribbons = true;
};

struct Wc3EmitterReport {
    u32 particleRecords = 0;      ///< `Sc2ParticleEmitter` nodes (a head and a tail make two).
    u32 modelParticleRecords = 0; ///< Of those, model particles from `PREM`.
    u32 ribbonRecords = 0;        ///< `Sc2RibbonEmitter` nodes.
    u32 materials = 0;            ///< Material slots added for them.
    u32 keyedChannels = 0;        ///< `EmitterProperty` channels keyed on them.
    wem::Diagnostics diagnostics;
};

/// Before staging: the document edits the crossing needs.
///
/// Every node composes onto its parent again. The import reads a node's
/// `ModelSpace` bit (0x80000) as WEM's `NodeFlags::ModelSpace` -- the node's
/// local is its world -- but Warcraft III's node update never reads that bit:
/// the particle system does, as the space its particles live in, which the
/// crossing takes off the node's `mdxFlagBits`. Staged with the flag, the bone
/// keeps its parent and a local that is already its world, and lands its
/// parent's offset away (War3 Wisp's emitters, 69 units above the body).
///
/// A squirting emitter's visibility track is removed -- Warcraft III's bursts
/// ignore it and StarCraft II's obey the bone's (§A9) -- with a diagnostic, as
/// is a `CORN` node's, which Warcraft III never reads.
///
/// With @p ribbonOffsets, a ribbon whose two heights differ gets a helper child
/// half their difference across its width, carrying the ribbon's visibility
/// (§4.5 row 2): StarCraft II centres a strip on the bone it rides, and the
/// crossing puts the ribbon's record there.
void PrepareWc3Effects(wem::Document& document, wem::Diagnostics& diagnostics,
                       bool ribbonOffsets = true);

/// After staging -- the StarCraft II set derived, the rig restated, every length
/// in StarCraft II's units -- and before `toM3` with
/// `M3ExportSettings::effectNodeBones`: every `Wc3ParticleEmitter2`,
/// `Wc3ParticleEmitter1` and `Wc3RibbonEmitter` node of the first model becomes
/// the StarCraft II emitter that draws it.
///
/// The node itself becomes the first record, so it keeps its transform, its
/// visibility, its children and the bone `toM3` gives it. A second record of
/// the same system -- a head's tail, the camera-facing share of an XY quad
/// emitter -- is an identity child that shares its visibility
/// (`kNodeSharesParentVisibility`) and rides its bone. A system with nothing to
/// draw keeps its placement as a helper.
Wc3EmitterReport CrossWc3Emitters(wem::Document& staged, const Wc3EmitterOptions& options);

// ---------------------------------------------------------------------------
// PopcornFX effects (WC3_CORN_TO_SC2_PLAN.md P0.7): the records a caller fitted
// to a `CORN` node's effect, placed as the crossing places its own.
// ---------------------------------------------------------------------------

/// The look of a placed record: its StarCraft II material.
struct Sc2Look {
    /// As `Document::textures` or the game names it; empty draws untextured.
    std::string texture;
    m3::BlendMode blend = m3::BlendMode::AlphaBlend;
    bool unshaded = true;
    bool unfogged = false;
    bool twoSided = false;
    i32 priority = 0;
    /// `TEXS` 0x1 wrap U, 0x2 wrap V.
    u32 wrap = 0;
    /// The depth blend's falloff (soft particles), in model units; 0 draws
    /// hard.
    f32 softness = 0.04f;
    /// The diffuse's multiply, unclamped: a colour above what a vertex byte
    /// holds (the material's HDR gain).
    f32 gain = 1.0f;
    /// The colour is the player's: the texture's alpha lights the team colour
    /// on the emissive team op, times the vertex colour, `gain` riding the
    /// emissive multiplier.
    bool teamGlow = false;
};

/// One property's keys: per clip of the document, its sub-track.
using Sc2Keyed = std::vector<std::pair<u32, wem::SubTrack>>;

struct Sc2ParticleSpec {
    /// The record at its rests. An AnimRef with a non-zero `animId` is keyed by
    /// `keys[animId - 1]`.
    m3::ParticleEmitter record;
    std::vector<Sc2Keyed> keys;
    Sc2Look look;
    /// Where the record sits in the effect's frame: its shape's centre, or a
    /// point birth's place off the emitter. Non-zero hangs it on a child
    /// moved there.
    Vector3f offset{0.0f, 0.0f, 0.0f};
    /// The record keeps the axes the effect has while its node does not turn:
    /// it hangs on a child whose rotation is keyed per clip to undo the turn
    /// of the node and its ancestors, so it follows the node's place only.
    bool worldAxes = false;
    /// With `worldAxes`, a cone's yaw and pitch are keyed per clip to turn
    /// its axis with the node, as a birth drawn in the node's frame turns.
    bool coneTurns = false;
};

/// A `PAR_` at the rests Blizzard's tools write for every field nothing sets
/// (WC3_TO_SC2_COMPLETION_PLAN.md §4.6): where a caller starts a record.
m3::ParticleEmitter Sc2RestParticle();

/// What a `CORN` node became.
struct Sc2CornCrossing {
    u32 node = wem::kInvalidIndex; ///< The `CORN` node in the staged document's first model.
    std::vector<Sc2ParticleSpec> particles;
};

/// After staging and before `CrossWc3Emitters`: each crossing's node becomes a
/// helper that keeps its placement and children, and its records
/// hang on an identity child of it turned +90° about z, the frame the game
/// draws a `CORN` effect in (D§6): the first record on that child, the rest on
/// identity children of it that share its visibility (a `worldAxes` record
/// on one more child below, its rotation keyed). A node the caller does
/// not name stays a `CORN` node, which `CrossWc3Emitters` then reports dropped.
Wc3EmitterReport PlaceSc2Records(wem::Document& staged, const std::vector<Sc2CornCrossing>& crossings,
                                 const Wc3EmitterOptions& options);

} // namespace cross
} // namespace models
} // namespace whiteout
