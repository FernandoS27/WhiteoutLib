// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/specular.h>

#include <algorithm>
#include <cmath>

namespace whiteout::textures::pbr {

f32 RoughnessFromExponent(f32 exponent) {
    const f32 n = std::max(1.0f, exponent);
    // alpha = sqrt(2 / (n + 2)); Reforged's `ggxNDF` squares what it is handed
    // before using it as alpha, so the map holds sqrt(alpha).
    return std::clamp(std::pow(2.0f / (n + 2.0f), 0.25f), 0.0f, 1.0f);
}

f32 ExponentFromRoughness(f32 roughness) {
    // Clamped at the rough end so a painted-white roughness cannot demand an
    // exponent below Blinn-Phong's floor, and at the glossy end so a black
    // texel does not explode the exponent past anything a `.m3` states.
    const f32 r = std::clamp(roughness, 0.05f, 1.0f);
    const f32 alpha2 = r * r * r * r;
    return std::clamp(2.0f / alpha2 - 2.0f, 1.0f, 4096.0f);
}

} // namespace whiteout::textures::pbr
