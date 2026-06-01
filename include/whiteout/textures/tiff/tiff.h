// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file tiff.h
 * @brief Main header for the TIFF (Tagged Image File Format) texture library.
 *
 * Include this single header to access TIFF parsing and writing.
 * See docs/TIFF.md for the supported subset.
 */

#include "parser.h"
#include "writer.h"

namespace whiteout::textures::tiff {

constexpr int MAJOR_VERSION = 0;
constexpr int MINOR_VERSION = 1;
constexpr int PATCH_VERSION = 0;

} // namespace whiteout::textures::tiff

namespace whiteout {
namespace tiff = textures::tiff;
}
