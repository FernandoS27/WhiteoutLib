// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file dds.h
 * @brief Main header for the DDS (DirectDraw Surface) texture library
 *
 * This is the primary include file for the DDS library. Include this single
 * header to access DDS functionality including parsing and writing.
 *
 * The DDS format is commonly used for GPU-compressed textures and supports
 * both legacy headers and DX10 extended headers.
 *
 * @example Basic Usage
 * @code
 * #include <whiteout/textures/dds/dds.h>
 *
 * // Parse DDS bytes into a texture
 * dds::Parser parser;
 * auto texture = parser.parse(file_bytes);
 *
 * if (parser.hasIssues()) {
 *     for (const auto& issue : parser.getIssues()) {
 *         std::cerr << "Warning: " << issue << std::endl;
 *     }
 * }
 *
 * // Encode texture back into DDS
 * dds::Writer writer;
 * auto output = writer.write(*texture);
 * @endcode
 */

#include "parser.h"
#include "writer.h"

namespace whiteout {
namespace dds = textures::dds;

namespace textures {
namespace dds {
// ============================================================================
// DDS Library Version
// ============================================================================

/// Library major version number
constexpr int MAJOR_VERSION = 1;
/// Library minor version number
constexpr int MINOR_VERSION = 0;
/// Library patch version number
constexpr int PATCH_VERSION = 0;
} // namespace dds
} // namespace textures
} // namespace whiteout
