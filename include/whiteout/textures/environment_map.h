// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file environment_map.h
 * @brief Environment-map projections between the shapes the games sample.
 */

#include <whiteout/textures/texture.h>

#include <array>
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

/**
 * @brief Re-express one cube of a cube map in another basis, every level kept.
 *
 * A pre-filtered probe carries its roughness blur in its mip chain, so a probe
 * moved between engines keeps that chain: each output level is read from the
 * same source level, never regenerated. @p sourceFromTarget maps a direction in
 * the output cube's space to where the source stores it, row by row
 * (`src[i] = m[3i] * d[0] + m[3i + 1] * d[1] + m[3i + 2] * d[2]`), and
 * `faceOrder[k]` is the stored layer holding Direct3D face k, for containers
 * that write the faces in another order. Nearest texel, which is exact when the
 * basis is a signed axis permutation -- a change of up axis is one. DDS /
 * Direct3D face order and orientation out, RGBA8, colour space kept.
 *
 * @param cubeIndex Which cube of a cube array; 0 for a plain cube.
 * @return Empty when @p source is not a cube map, @p cubeIndex is past its
 *         cubes, or @p faceOrder names a layer past 5.
 */
std::optional<Texture> CubeFromCube(const Texture& source, u32 cubeIndex,
                                    const std::array<f32, 9>& sourceFromTarget,
                                    const std::array<u32, 6>& faceOrder = {0, 1, 2, 3, 4, 5});

} // namespace whiteout::textures::env
