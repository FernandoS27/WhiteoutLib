// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file jpeg.h
 * @brief Main header for the JPEG texture library
 *
 * This is the primary include file for the JPEG library. Include this single
 * header to access JPEG functionality including parsing and writing.
 *
 * Supports baseline sequential DCT (SOF0) JPEG images with standard Y'CbCr
 * colour encoding (JFIF) as well as grayscale.
 *
 * @example Basic Usage
 * @code
 * #include <whiteout/textures/jpeg/jpeg.h>
 *
 * // Parse JPEG bytes into a texture
 * jpeg::Parser parser;
 * auto texture = parser.parse(file_bytes);
 *
 * if (parser.hasIssues()) {
 *     for (const auto& issue : parser.getIssues()) {
 *         std::cerr << "Warning: " << issue << std::endl;
 *     }
 * }
 *
 * // Encode texture back into JPEG
 * jpeg::Writer writer(90); // quality 1-100
 * auto output = writer.write(*texture);
 * @endcode
 */

#include "parser.h"
#include "writer.h"

namespace whiteout::textures::jpeg {
// ============================================================================
// JPEG Library Version
// ============================================================================

/// Library major version number
constexpr int MAJOR_VERSION = 1;
/// Library minor version number
constexpr int MINOR_VERSION = 0;
/// Library patch version number
constexpr int PATCH_VERSION = 0;
} // namespace whiteout::textures::jpeg

namespace whiteout { namespace jpeg = textures::jpeg; }
