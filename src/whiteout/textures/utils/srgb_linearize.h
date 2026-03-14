// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file srgb_linearize.h
/// @brief Utility to produce a linearized copy of an sRGB-flagged Texture.
///
/// Internal header — not part of the public include path.

#pragma once

#include <whiteout/textures/texture.h>

namespace whiteout::textures {

/// If @p texture is marked as sRGB, return a linearized copy with the sRGB
/// flag cleared.  Otherwise return a plain copy.
///
/// For uncompressed formats (RGBA8, RG8, R8, etc.) the RGB channels are
/// converted via the standard sRGB transfer function.  For block-compressed
/// formats the texture is first decompressed to RGBA8 before linearising.
Texture linearizeSrgbCopy(const Texture& texture);

} // namespace whiteout::textures
