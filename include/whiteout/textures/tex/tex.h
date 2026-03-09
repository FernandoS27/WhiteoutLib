// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file tex.h
 * @brief Main header for the TEX (Diablo III SNO texture) library
 *
 * This is the primary include file for the TEX library.  Include this single
 * header to access all TEX functionality including parsing, writing, and type
 * definitions.
 *
 * The TEX format is Diablo III's SNO-based texture container.  It wraps
 * standard GPU pixel data with game-specific metadata such as SNO IDs,
 * sampler hints, and optional flip-book frame definitions.
 *
 * @example Basic Usage
 * @code
 * #include <whiteout/textures/tex/tex.h>
 *
 * // Parse a TEX file
 * tex::Parser parser;
 * tex::TexInfo info;
 * auto texture = parser.parse(file_bytes, &info);
 *
 * // Re-encode as TEX
 * tex::Writer writer;
 * auto output = writer.write(*texture, {
 *     .snoId = info.snoId,
 *     .flags  = info.flags,
 * });
 * @endcode
 */

#include <whiteout/textures/tex/parser.h>
#include <whiteout/textures/tex/types.h>
#include <whiteout/textures/tex/writer.h>

namespace whiteout::textures::tex {

// ============================================================================
// TEX Library Version
// ============================================================================

/// Library major version number.
constexpr int MAJOR_VERSION = 1;
/// Library minor version number.
constexpr int MINOR_VERSION = 0;
/// Library patch version number.
constexpr int PATCH_VERSION = 0;

} // namespace whiteout::textures::tex

namespace whiteout {
namespace tex = textures::tex;
}
