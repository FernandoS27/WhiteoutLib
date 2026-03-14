// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file bmp.h
 * @brief Main header for the BMP (Bitmap) texture library
 *
 * This is the primary include file for the BMP library. Include this single
 * header to access BMP functionality including parsing and writing.
 *
 * The BMP format is a widely supported uncompressed raster image format.
 *
 * @example Basic Usage
 * @code
 * #include <whiteout/textures/bmp/bmp.h>
 *
 * // Parse BMP bytes into a texture
 * bmp::Parser parser;
 * auto texture = parser.parse(file_bytes);
 *
 * if (parser.hasIssues()) {
 *     for (const auto& issue : parser.getIssues()) {
 *         std::cerr << "Warning: " << issue << std::endl;
 *     }
 * }
 *
 * // Encode texture back into BMP
 * bmp::Writer writer;
 * auto output = writer.write(*texture);
 * @endcode
 */

#include "parser.h"
#include "writer.h"

namespace whiteout {
namespace bmp = textures::bmp;

namespace textures {
namespace bmp {
// ============================================================================
// BMP Library Version
// ============================================================================

/// Library major version number
constexpr int MAJOR_VERSION = 1;
/// Library minor version number
constexpr int MINOR_VERSION = 0;
/// Library patch version number
constexpr int PATCH_VERSION = 0;
} // namespace bmp
} // namespace textures
} // namespace whiteout
