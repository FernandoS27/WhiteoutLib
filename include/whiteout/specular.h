// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file specular.h
 * @brief A Blinn-Phong exponent as a GGX roughness and back: the two scalar
 *        formulas the texture bake (`pbr_bake.h`) and the WEM material
 *        crossings share, so neither needs the other.
 */

#include <whiteout/common_types.h>

namespace whiteout::textures::pbr {

/**
 * @brief The GGX roughness that stands in for a Blinn-Phong exponent.
 *
 * `alpha = sqrt(2 / (n + 2))` is the usual lobe-width match, and Reforged's
 * `ggxNDF` is the Disney remap — it squares the roughness it is handed before
 * using it as alpha — so the value the map holds is `sqrt(alpha)`.
 *
 * The corpus lands where it should: exponent 20 (72.7% of materials) is
 * roughness 0.55, 40 is 0.47, 80 is 0.39. The old fallback — inferred from how
 * bright the specular map was — pinned StarCraft II near **0.88**, which is a
 * surface with no highlight to see.
 */
f32 RoughnessFromExponent(f32 exponent);

/**
 * @brief The Blinn-Phong exponent a GGX roughness stands in for — the exact
 *        inverse of @ref RoughnessFromExponent (`n = 2/r^4 - 2`, the fourth
 *        power because the map stores sqrt(alpha) and `ggxNDF` squares what
 *        it is handed). The reverse crossing (Reforged -> StarCraft II)
 *        recovers `specularExponent` with it.
 */
f32 ExponentFromRoughness(f32 roughness);

} // namespace whiteout::textures::pbr
