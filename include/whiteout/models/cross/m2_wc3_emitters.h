// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file m2_wc3_emitters.h
 * @brief World of Warcraft's particle emitters restated as Warcraft III's, inside WEM.
 *
 * WoW's `M2Particle` descends from Warcraft III's `PRE2` and still reads like
 * one: a cone of textured quads with a head, a tail and a sprite-sheet
 * flipbook, coloured, faded and sized along the particle's life. So each
 * `M2ParticleEmitter` node becomes the `Wc3ParticleEmitter2` that draws it most
 * nearly, and its keyed properties become that kind's `EmitterProperty`
 * channels. `toMdx` then writes it as it writes any `PRE2`.
 *
 * The record's lifetime curves have any number of keys and `PRE2` has three
 * segments, so they are sampled at birth, at the middle key and at death. What
 * `PRE2` has no field for — a sphere or a spline shape, a second and third
 * texture, spin, twinkle, drag, wind, a random cell per particle — is
 * approximated or dropped, each with a diagnostic.
 */

#include <whiteout/models/wem/diagnostics.h>
#include <whiteout/models/wem/document.h>

#include <functional>
#include <optional>
#include <vector>

namespace whiteout {
namespace models {
namespace cross {

struct M2EmitterReport {
    u32 particles = 0;     ///< `Wc3ParticleEmitter2` nodes written.
    u32 keyedChannels = 0; ///< Of their `EmitterProperty` channels, the ones kept.
    wem::Diagnostics diagnostics;
};

struct M2EmitterOptions {
    /// The mean RGBA (0..1) of a document texture, or nothing where the host
    /// cannot read it. A multi-texture particle multiplies its second and
    /// third textures in at 2x or 4x and `PRE2` draws only the first, so what
    /// they average to folds into its colour and alpha; unset, it is dropped.
    /// The fallback: a host that can bake the product into one sheet first
    /// leaves the crossing a single-texture particle.
    std::function<std::optional<Vector4f>(const wem::TextureRef& texture)> textureMean;
};

/// After staging for a Warcraft III profile — every length in its units — and
/// before `toMdx`: every `M2ParticleEmitter` node becomes a
/// `Wc3ParticleEmitter2` in place, keeping its bone, its visibility and its
/// children.
M2EmitterReport CrossM2Emitters(wem::Document& staged, const M2EmitterOptions& options = {});

/// Where on the particle's life (0..1) the crossing puts `PRE2`'s middle
/// segment: the middle key of the first three-key curve of alpha, colour and
/// size, or halfway. `PRE2` spreads a segment's cells evenly over it, so a
/// flipbook baked for the crossing keys a frame there, and none straddles it.
f32 M2EmitterMiddleTime(const wem::M2ParticleEmitterPayload& emitter);

/// The cell a particle's cell track shows at @p t (0..1) of its life, before
/// the emitter's base cell and mask: retail `CParticleEmitter2_InterpolateTrackU16`
/// (0x141962440) over the keys `FindTrackKeys` picks — two keys read @p t as
/// is, three split at the middle key, more search — linear between them and
/// ROUNDED to nearest, ties to even (`cvtss2si`), never truncated. Nothing is
/// clamped: past the keys the line runs on, as the client's does. 0 for an
/// empty track.
i32 M2CellAt(const std::vector<f32>& times, const std::vector<u32>& cells, f32 t);

/// Whether the client draws the emitter through its simple path:
/// `CParticleEmitter2_DetermineIfSimple` (0x141967180), in its order —
/// colour, alpha and scale each with 1 to 3 keys; head and tail cells with at
/// most 3, or 4 when the middle two share a time; no lifespan variation; not
/// an empty head track with ChooseRandomTexture; no scale variation; every
/// scale key square; and every three-key track (three or more for cells)
/// split at one middle time. Such an emitter reads its cells through
/// `M2SimpleCellAt`, every other through `M2CellAt`.
bool M2IsSimple(const wem::M2ParticleEmitterPayload& emitter);

/// The simple path's split (0..1) as the client stores it: the middle key time
/// of the first three-key track of colour, alpha and scale, then of head and
/// tail cells with three or more, as `u16 * (1/32767)` in f32. 0 when no
/// track has three keys.
f32 M2SimpleMiddle(const wem::M2ParticleEmitterPayload& emitter);

/// A cell track restated for the simple path: two segments split at the
/// middle time, each a 16.16 start and a slope in whole cells
/// (`CParticleEmitter2_SetSimpleHeadCellKeys`, 0x141966E00).
struct M2SimpleCellKeys {
    i32 start[2] = {0, 0}; ///< 16.16; segment 2 starts at c0 + slope 1, not at c2.
    i32 slope[2] = {0, 0}; ///< Cells across the segment.
    i32 middle = 0;        ///< The split, 16.16.
};

/// @p cells keyed for the simple path, split at @p middle (`M2SimpleMiddle`).
/// Slopes: one key 0, 0; two keys (c1-c0)*m and (c1-c0)*(1-m), ROUNDED
/// (`cvtss2si`); three keys c1-c0, c2-c1; four keys c1-c0, c3-c2.
M2SimpleCellKeys M2SimpleCells(const std::vector<u32>& cells, f32 middle);

/// The cell the simple path shows at @p t (0..1) of the particle's life, before
/// the base cell and mask: `InterpolateAllTracks_Simple` (0x1419631F0) — t in
/// 16.16, clamped to [0, 1], the segment's own 16.16 fraction by integer
/// division, and `(start + slope * fraction) >> 16`, TRUNCATED (floored).
i32 M2SimpleCellAt(const M2SimpleCellKeys& keys, f32 t);

/// The one reader a cell track of an emitter goes through, chosen once per
/// emitter as the client chooses it at load (`M2IsSimple`). Every consumer of
/// a WoW cell track — the PRE2 crossing, the multi-texture bake, the CORN
/// writer — reads through this.
class M2CellReader {
public:
    static M2CellReader Head(const wem::M2ParticleEmitterPayload& emitter);
    static M2CellReader Tail(const wem::M2ParticleEmitterPayload& emitter);

    bool simple() const {
        return simple_;
    }
    /// Nothing to read: an unkeyed track on the general path, which the client
    /// leaves at cell 0 with no base cell added. On the simple path an unkeyed
    /// track still reads 0, and the client adds the base cell to it.
    bool empty() const {
        return !simple_ && cells_.empty();
    }
    const M2SimpleCellKeys& simpleKeys() const {
        return simpleKeys_;
    }
    /// The cell at @p t (0..1), before the base cell and mask.
    i32 at(f32 t) const;

private:
    M2CellReader(const wem::M2ParticleEmitterPayload& emitter, const std::vector<f32>& times,
                 const std::vector<u32>& cells);

    bool simple_ = false;
    std::vector<f32> times_;
    std::vector<u32> cells_;
    M2SimpleCellKeys simpleKeys_;
};

} // namespace cross
} // namespace models
} // namespace whiteout
