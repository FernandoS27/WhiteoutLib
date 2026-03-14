// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file gif.h
 * @brief Main header for the GIF texture library (write-only)
 *
 * This is the primary include file for GIF support. Include this single
 * header to access all GIF functionality.
 *
 * The GIF89a format supports single or animated images with up to 256 colors.
 *
 * @example Basic Usage
 * @code
 * #include <whiteout/textures/gif/gif.h>
 *
 * // Encode a sequence of textures to an animated GIF
 * gif::Writer writer;
 * auto output = writer.write(frames, gif::SaveOptions{.delayCs = 5});
 * @endcode
 */

#include "writer.h"

namespace whiteout {
namespace gif = textures::gif;

namespace textures {
namespace gif {
// ============================================================================
// GIF Library Version
// ============================================================================

/// Library major version number
constexpr int MAJOR_VERSION = 1;
/// Library minor version number
constexpr int MINOR_VERSION = 0;
/// Library patch version number
constexpr int PATCH_VERSION = 0;
} // namespace gif
} // namespace textures
} // namespace whiteout
