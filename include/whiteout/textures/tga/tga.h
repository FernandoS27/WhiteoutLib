// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file tga.h
 * @brief Main header for the TGA (Targa) texture library
 *
 * This is the primary include file for the TGA library. Include this single
 * header to access TGA functionality including parsing and writing.
 *
 * The TGA format supports uncompressed and RLE-compressed true-color and
 * grayscale images, and is widely used in game asset pipelines.
 *
 * @example Basic Usage
 * @code
 * #include <whiteout/textures/tga/tga.h>
 *
 * // Parse TGA bytes into a texture
 * tga::Parser parser;
 * auto texture = parser.parse(file_bytes);
 *
 * if (parser.hasIssues()) {
 *     for (const auto& issue : parser.getIssues()) {
 *         std::cerr << "Warning: " << issue << std::endl;
 *     }
 * }
 *
 * // Encode texture back into TGA
 * tga::Writer writer;
 * auto output = writer.write(*texture);
 * @endcode
 */

#include "parser.h"
#include "writer.h"

namespace whiteout::textures::tga {
// ============================================================================
// TGA Library Version
// ============================================================================

/// Library major version number
constexpr int MAJOR_VERSION = 1;
/// Library minor version number
constexpr int MINOR_VERSION = 0;
/// Library patch version number
constexpr int PATCH_VERSION = 0;
} // namespace whiteout::textures::tga

namespace whiteout {
namespace tga = textures::tga;
}
