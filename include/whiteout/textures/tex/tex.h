// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file tex.h
 * @brief Main header for the TEX (Diablo III / IV SNO texture) library
 *
 * This is the primary include file for the TEX library.  Include this single
 * header to access all TEX functionality including parsing, writing, and type
 * definitions.
 *
 * The TEX format is used by both Diablo III and Diablo IV.  D3 TEX files
 * are monolithic (metadata + pixel data in one file).  D4 TEX files separate
 * the SNO metadata from the pixel payload, so parsing requires two inputs.
 *
 * @example Basic Usage
 * @code
 * #include <whiteout/textures/tex/tex.h>
 *
 * // Parse a D3 TEX file
 * tex::Parser parser;
 * tex::TexInfo info;
 * auto texture = parser.parse(file_bytes, &info);
 *
 * // Parse a D4 TEX file (metadata + payload)
 * tex::D4TexInfo d4info;
 * auto d4texture = parser.parse(tex_bytes, payload_bytes, &d4info);
 *
 * // Re-encode as D3 TEX
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
