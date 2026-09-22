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

} // namespace cross
} // namespace models
} // namespace whiteout
