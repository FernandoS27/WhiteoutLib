// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file m3.h
 * @brief Main header for the M3 (StarCraft II / Heroes of the Storm Model Format) library
 *
 * This is the primary include file for the M3 library. Include this single header
 * to access all M3 functionality including parsing, writing, and structure definitions.
 *
 * The M3 format (MD34) is used by StarCraft II and Heroes of the Storm for 3D models.
 * All chunks are 16-byte aligned and the file is little-endian. This library supports
 * all known MODL versions from v23 through v30.
 *
 * @example Basic Usage
 * @code
 * #include <m3/m3.h>
 *
 * // Parse an M3 file
 * m3::Parser parser;
 * m3::Model model = parser.parse("model.m3");
 *
 * // Modify the model...
 * model.name = "Modified Model";
 *
 * // Write it back
 * m3::Writer writer;
 * writer.write("output.m3", model);
 * @endcode
 */

#include "engine_compat.h"
#include "parser.h"
#include "structures.h"
#include "types.h"
#include "writer.h"

namespace whiteout {
namespace m3 {
/// Library major version number
constexpr int MAJOR_VERSION = 1;
/// Library minor version number
constexpr int MINOR_VERSION = 0;
/// Library patch version number
constexpr int PATCH_VERSION = 0;
/// Library version string
constexpr const char* LIBRARY_VERSION = "1.0.0";
} // namespace m3
} // namespace whiteout
