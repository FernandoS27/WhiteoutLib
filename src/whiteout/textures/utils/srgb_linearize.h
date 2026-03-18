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

/// Convert @p tex in-place so that its pixel data matches the requested
/// colour space.  If @p srgb is true the data is converted from linear to
/// sRGB; if false, from sRGB to linear.  No-op when the texture already
/// carries the requested flag.
///
/// Block-compressed textures are decompressed to RGBA8 first (in-place).
/// Only the first three colour channels (RGB) are affected; alpha is
/// preserved unchanged.
void ensureColorSpace(Texture& tex, bool srgb);

} // namespace whiteout::textures
