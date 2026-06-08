// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file d2r_texture.h
 * @brief Main header for the Diablo II: Resurrected `.texture` library.
 *
 * Include this single header to access parsing, writing, and type definitions
 * for the D2R `.texture` container.
 *
 * @example Basic Usage
 * @code
 * #include <whiteout/textures/d2r_texture/d2r_texture.h>
 *
 * d2r_texture::Parser parser;
 * d2r_texture::D2rTextureInfo info;
 * auto texture = parser.parse(file_bytes, &info);
 *
 * d2r_texture::Writer writer;
 * auto output = writer.write(*texture);
 * @endcode
 */

#include <whiteout/textures/d2r_texture/parser.h>
#include <whiteout/textures/d2r_texture/types.h>
#include <whiteout/textures/d2r_texture/writer.h>

namespace whiteout {
namespace d2r_texture = textures::d2r_texture;
}
