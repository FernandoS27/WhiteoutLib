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

/// A host's mark (`NativeBag` key, any value) on a `PRE2` node that is a
/// Warcraft III ground splat written into the model: its quad crosses lying on
/// the ground (`TerrainOriented`). Warcraft III's own XY quad turns to its
/// velocity, and a splat has none to turn by.
inline constexpr const char* kWc3GroundSplat = "wc3GroundSplat";

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
    /// Per model an attachment point carries (`AttachmentPayload::asset`, as
    /// written there): the `.m3` the export wrote for it, as `spawnedModels`.
    /// A point whose model is missing here draws none.
    std::map<std::string, std::string> attachedModels;
    bool particles = true;
    bool ribbons = true;
};

struct Wc3EmitterReport {
    u32 particleRecords = 0;      ///< `Sc2ParticleEmitter` nodes (a head and a tail make two).
    u32 modelParticleRecords = 0; ///< Of those, model particles from `PREM`.
    u32 attachedModelRecords = 0; ///< Of those, the models attachment points carry.
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
/// the StarCraft II emitter that draws it, and an attachment point carrying a
/// model in `Wc3EmitterOptions::attachedModels` gains a model particle child
/// that shows it.
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
    /// The texture read mirrored (-1) along U or V, about its centre. Only a
    /// particle drawing one image takes it: its layers go on the explicit
    /// mapping, which the layer's transform reaches and the flipbook's never
    /// does.
    Vector2f uvTiling{1.0f, 1.0f};
    /// Above 0, the look bends the scene behind instead of drawing: a
    /// displacement (`DIS_`) of this strength, `texture` its normal map.
    f32 displace = 0.0f;
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
    /// Radians a second the record turns about the effect frame's z (0:
    /// none): it hangs, its offset with it, on a bone spinning so on a global
    /// loop, and its particles, local to it, turn with it (an orbit, D§12.2).
    f32 spin = 0.0f;
    /// Factors on the look's colour and coverage multiplies, keyed per clip
    /// (an f32 a key): a fade as the effect stops. Either keyed, the record
    /// draws with a material of its own.
    Sc2Keyed colourKeys;
    Sc2Keyed coverageKeys;
    /// The record its particles lay trail children of (the trail link, its
    /// rate and chance the record's own), by index into the crossing's
    /// `particles`; -1: none. The child takes each particle's place in this
    /// record's space, so it should sit where this one does.
    i32 trailChild = -1;
};

/// A `PAR_` at the rests Blizzard's tools write for every field nothing sets
/// (WC3_TO_SC2_COMPLETION_PLAN.md §4.6): where a caller starts a record.
m3::ParticleEmitter Sc2RestParticle();

/// A `RIB_` fitted to a PopcornFX ribbon (D§13), keyed as a particle spec is.
struct Sc2RibbonSpec {
    m3::RibbonEmitter record;
    std::vector<Sc2Keyed> keys;
    Sc2Look look;
    /// Quarter turns of the look's texture about its centre, on StarCraft II's
    /// strip (U across, V its points' age): -1 where the texture's U runs
    /// along the strip from its head, as a Warcraft III ribbon's does; 2
    /// where its V runs along it from its tail (PopcornFX's RotateTexture).
    i32 textureTurns = -1;
    /// Where the strip leaves, in the effect's frame.
    Vector3f offset{0.0f, 0.0f, 0.0f};
};

/// A `RIB_` at the rests Blizzard's tools write: a world-space trail laid by
/// time, planar, pre-rolled.
m3::RibbonEmitter Sc2RestRibbon();

/// One stream of a baked sprite over a loop of its own: a key a step of it,
/// evenly from its start (one key: it holds still), closed on its first key,
/// played on the global loop clip of its period.
template <class T>
struct Sc2Loop {
    f32 period = 0.0f; ///< Seconds.
    std::vector<T> keys;
};

/// A particle that never dies, as geometry (D§12.4, the bake): a quad on
/// bones of its own, each stream keyed over its own loop (an orbit and a
/// pulse of different periods each keep theirs; the global loops run on one
/// clock, so their phases hold).
struct Sc2BakedSprite {
    Sc2Look look;
    /// How the quad faces: the camera's plane, the eye, a plane of its own
    /// (`turn`), or the eye about an axis (`turn`'s x, its height along minus
    /// it): an axis quad's or a spheroid's, a bone billboard of type 5.
    enum class Facing : u8 { View, Eye, Plane, Axis };
    Facing facing = Facing::View;
    /// Its place in the effect's frame, as `Sc2ParticleSpec::offset` is.
    Sc2Loop<Vector3f> position;
    /// Its place keyed per clip instead, by clip (three floats a key), for a
    /// head that lives and dies (D§12.3); `position`'s first key is its rest.
    Sc2Keyed path;
    /// An orbit about the frame's z the quad turns on from its place:
    /// radians a second (0: none), and its arm at the clock's start. The
    /// arm's end is where it draws and its riders ride.
    f32 orbit = 0.0f;
    Vector3f arm{0.0f, 0.0f, 0.0f};
    /// Half its width and half its height.
    Sc2Loop<Vector2f> size;
    /// Radians, its width turned toward its height.
    Sc2Loop<f32> rotation;
    /// A plane's axes in the effect's frame: x its normal, y its width, z its
    /// height. An axis quad's: x minus its axis.
    Sc2Loop<Quaternion> turn;
    /// The colour's multiply and the coverage's, on the colour's loop.
    Sc2Loop<f32> brightness;
    Sc2Loop<f32> alpha;
    /// Its hue, the vertex colour.
    Vector3f color{1.0f, 1.0f, 1.0f};
    /// The texture's rectangle it draws: u0, v0, u1, v1.
    Vector4f cell{0.0f, 0.0f, 1.0f, 1.0f};
    /// The document's clips it is hidden in: the effect is off there.
    std::vector<u32> hidden;
    /// A particle that lives and dies, born at set times (D§12.4): the
    /// seconds it lives (0: it never dies), and each birth as the document's
    /// clip and the seconds into it, clip by clip in order. Each stream's
    /// loop is then one life long, a key a step from the birth, keyed in
    /// those clips from each birth and on no global loop, and the quad draws
    /// only while it lives.
    f32 life = 0.0f;
    std::vector<std::pair<u32, f32>> born;
    /// Records riding it, on its place alone (D§7.4: a keyed bone carries a
    /// world-space `PAR_`).
    std::vector<Sc2ParticleSpec> riders;
    /// Strips riding it the same way (D§13: a `RIB_` off a keyed head).
    std::vector<Sc2RibbonSpec> ribbons;
    /// Whether its riders lay, a key a step of its loop (0 or 1, held to the
    /// next); empty: throughout. A place that ends its loop away from where
    /// it starts flies back there, and what a world-space rider births on
    /// the way would be swept along it.
    Sc2Loop<f32> lays;
    /// False for a head that draws nothing (D§12.3): its place, orbit and
    /// riders alone, no quad; only `position` is read.
    bool draws = true;
    /// Its place keeps the axes the effect has while its node does not turn:
    /// it hangs on a child keyed per clip to undo the node's turn, as a
    /// `worldAxes` record's particles do, so it follows the node's place only.
    bool worldAxes = false;
};

/// What a `CORN` node became.
struct Sc2CornCrossing {
    u32 node = wem::kInvalidIndex; ///< The `CORN` node in the staged document's first model.
    std::vector<Sc2ParticleSpec> particles;
    std::vector<Sc2BakedSprite> sprites;
    std::vector<Sc2RibbonSpec> ribbons;
};

/// After staging and before `CrossWc3Emitters`: each crossing's node becomes a
/// helper that keeps its placement and children, and its records
/// hang on an identity child of it turned +90° about z, the frame the game
/// draws a `CORN` effect in (D§6): the first record on that child, the rest on
/// identity children of it that share its visibility (a `worldAxes` record
/// on one more child below, its rotation keyed); each ribbon on a child of
/// its own. A node the caller does not name stays a `CORN` node, which
/// `CrossWc3Emitters` then reports dropped.
///
/// Each sprite is three bones under that frame -- its place, the quad's
/// facing (a billboard, or a plane's turn) and the quad's spin and size --
/// with its riders on the first, and a quad skinned to the last whose
/// material keys its multiplies. Each stream that moves is keyed on a global
/// loop clip of its period (`GLstand`, `GLstand 02`, ..., one per period).
Wc3EmitterReport PlaceSc2Records(wem::Document& staged, const std::vector<Sc2CornCrossing>& crossings,
                                 const Wc3EmitterOptions& options);

} // namespace cross
} // namespace models
} // namespace whiteout
