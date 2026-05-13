// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file png.h
 * @brief Main header for the PNG texture library
 *
 * This is the primary include file for the PNG library. Include this single
 * header to access PNG functionality including parsing and writing.
 *
 * Supports non-interlaced PNG images with 8/16-bit truecolor, grayscale,
 * indexed-color, and alpha channels.
 *
 * @example Basic Usage
 * @code
 * #include <whiteout/textures/png/png.h>
 *
 * // Parse PNG bytes into a texture
 * png::Parser parser;
 * auto texture = parser.parse(file_bytes);
 *
 * if (parser.hasIssues()) {
 *     for (const auto& issue : parser.getIssues()) {
 *         std::cerr << "Warning: " << issue << std::endl;
 *     }
 * }
 *
 * // Encode texture back into PNG
 * png::Writer writer;
 * auto output = writer.write(*texture);
 * @endcode
 */

#include "parser.h"
#include "writer.h"

namespace whiteout::textures::png {
// ============================================================================
// PNG Library Version
// ============================================================================

/// Library major version number
constexpr int MAJOR_VERSION = 1;
/// Library minor version number
constexpr int MINOR_VERSION = 0;
/// Library patch version number
constexpr int PATCH_VERSION = 0;
} // namespace whiteout::textures::png

namespace whiteout { namespace png = textures::png; }
