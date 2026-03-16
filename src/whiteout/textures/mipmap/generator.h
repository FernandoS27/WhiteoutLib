// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file generator.h
/// @brief High-level mipmap generator — selects the best pipeline per TextureKind.

#pragma once

#include <whiteout/textures/texture.h>

namespace whiteout::textures::mipmap {

/// Generate all mip levels for @p tex from its base (mip 0) image.
///
/// **Every mip level is generated directly from the original full-resolution
/// image** (mip 0) using a properly-sized filter kernel, rather than
/// cascading from the previous mip.  This eliminates cumulative blur and
/// rounding errors that iterative downsampling introduces.
///
/// The pipeline is selected automatically based on the texture's kind():
///
/// | Kind                                    | Pipeline                                 |
/// |-----------------------------------------|------------------------------------------|
/// | Diffuse / Albedo                        | Lanczos3; sRGB linearize/delinearize if isSrgb() |
/// | Normal                                  | Unpack → Kaiser(β=6) → Toksvig → Renormalize → Pack
/// | | Specular                                | Kaiser(β=6); sRGB if isSrgb()            | |
/// Roughness                               | Square → Kaiser(β=6.5) → √ (variance-preserving) | |
/// Gloss                                   | → Roughness → variance filter → Gloss    | | Metalness
/// | Kaiser(β=5.5) mean filtering             | | AmbientOcclusion                        |
/// Kaiser(β=6) mean filtering               | | Emissive                                | Lanczos3;
/// sRGB if isSrgb()               | | ORM                                     | Per-channel:
/// R=AO(Kaiser β=6), G=Roughness(variance), B=Metalness(Kaiser β=5.5) | | Other | Box filter; sRGB
/// if isSrgb()             |
///
/// The texture must use an uncompressed pixel format (R8 / R16 / R32F /
/// RG8 / RG16 / RG32F / RGBA8 / RGBA16 / RGBA32F).  BCn textures should
/// be decompressed first via `format()` or `copyAsFormat()`.
///
/// If the texture has only one mip level, this function is a no-op.
///
/// @param pool Optional WorkerPool used to parallelize independent
///             (layer, mip) jobs. If null, processing is single-threaded.
/// @return std::nullopt on success; std::optional<std::string> with error
///         message on failure. No exceptions are thrown.
std::optional<std::string> generateMipmaps(Texture& tex, interfaces::WorkerPool* pool = nullptr);

} // namespace whiteout::textures::mipmap
