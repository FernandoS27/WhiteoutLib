// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file environment_map.h
 * @brief Environment-map projections between the shapes the games sample.
 */

#include <whiteout/textures/texture.h>

#include <optional>

namespace whiteout::textures::env {

/**
 * @brief Project a latitude-longitude panorama into a cube map.
 *
 * Reforged's environment map is a 2:1 equirectangular photograph (2048x1024,
 * a sky over grass); StarCraft II reflects off a cube (`ReflectCubicEnvio`,
 * 8,575 of the 10,569 shipped env layers). Faces follow the DDS / Direct3D
 * order and orientation (+X, -X, +Y, -Y, +Z, -Z) with +Z the zenith — the
 * panorama's top row — and +Y its centre column, so a reflection toward the
 * sky reads the sky. Mip 0 only, sampled bilinearly with the longitude
 * wrapping; the caller generates the chain. RGBA8 out, colour space kept.
 *
 * @return Empty when @p panorama has no pixels or @p faceSize is 0.
 */
std::optional<Texture> CubeFromPanorama(const Texture& panorama, u32 faceSize);

} // namespace whiteout::textures::env
