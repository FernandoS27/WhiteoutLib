// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file blp.h
 * @brief Main header for the BLP (Blizzard Picture) texture library
 *
 * This is the primary include file for the BLP library.  Include this single
 * header to access all BLP functionality including parsing, writing, and type
 * definitions.
 *
 * The BLP format is used by Blizzard games for textures.  This library
 * supports BLP1 (Warcraft III) and BLP2 (World of Warcraft) variants.
 *
 * @example Basic Usage
 * @code
 * #include <whiteout/textures/blp/blp.h>
 *
 * // Parse a BLP file
 * blp::Parser parser;
 * auto texture = parser.parse("texture.blp");
 *
 * // Re-encode as BLP2
 * blp::Writer writer;
 * auto output = writer.write(*texture, {.version = blp::BlpVersion::BLP2});
 *
 * if (writer.hasIssues()) {
 *     for (const auto& issue : writer.getIssues()) {
 *         std::cerr << "Warning: " << issue << std::endl;
 *     }
 * }
 * @endcode
 */

#include "parser.h"
#include "types.h"
#include "writer.h"

namespace whiteout {
namespace blp = textures::blp;

namespace textures {
namespace blp {
/// Library major version number
constexpr int MAJOR_VERSION = 1;
/// Library minor version number
constexpr int MINOR_VERSION = 0;
/// Library patch version number
constexpr int PATCH_VERSION = 0;
} // namespace blp
} // namespace textures
} // namespace whiteout
