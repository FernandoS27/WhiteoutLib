// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file m2_wc3_lights.h
 * @brief World of Warcraft's point lights in Warcraft III's terms, inside WEM.
 *
 * 12.1 adds a point light as `(colour · intensity · att)²`, with
 * `att = 1 − saturate((d − start) / (end − start))`, and takes the square root
 * of the sum (`lightbufferomni`, WMO_RENDER_RE.md; the M2 body's
 * `sqrt(((amb + dir)·A)² + t11·A²)`). Warcraft III 3.0 adds
 * `colour · intensity · exp(−damping·d²) / (1 + linear·d + quadratic·d²)` to
 * linear light and reads no start or end (`CGxLightToShaderLight`). Taken as
 * they stood, the colour and intensity were a square root off, and Warcraft
 * III's default falloff, 0.0005 per square yard once the rescale had restated
 * it, reached across the whole model.
 */

#include <optional>

#include <whiteout/common_types.h>
#include <whiteout/models/wem/diagnostics.h>
#include <whiteout/models/wem/document.h>

namespace whiteout {
namespace models {
namespace cross {

/// Warcraft III's three falloff terms.
struct Wc3Falloff {
    f32 quadratic = 0.0f;
    f32 linear = 0.0f;
    f32 damping = 0.0f;
};

/// @brief The falloff that falls to half where 12.1's square of @p start ..
///        @p end does, `start + (1 − 1/√2)(end − start)`, and to one 8-bit step
///        at @p end. One that cannot, a light flat for much of its reach, is the
///        Gaussian through the half alone. Nothing for a light that reaches
///        nothing.
std::optional<Wc3Falloff> Wc3FalloffFor(f32 start, f32 end);

struct M2LightReport {
    u32 restated = 0; ///< Point lights in Warcraft III's terms.
    u32 spots = 0;    ///< Of them, spots written with their cone's share of the light.
    u32 dark = 0;     ///< Lights whose reach is nothing, turned off.
    wem::Diagnostics diagnostics;
};

/// Every point light a `.m2` or `.wmo` import made, the ones carrying
/// `m2LightType` 1 or `wmoLightSource`, takes its colour squared, its intensity
/// squared and the falloff of its reach, in the document's own units. A `.m2`
/// light's keys are squared where they stand and its reach is its first key.
M2LightReport CrossM2Lights(wem::Document& document);

} // namespace cross
} // namespace models
} // namespace whiteout
