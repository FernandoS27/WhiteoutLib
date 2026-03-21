// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file m2.h
 * @brief Main header for the M2 (World of Warcraft Model Format) library
 *
 * This is the primary include file for the M2 library. Include this single header
 * to access all M2 functionality including parsing, writing, and structure definitions.
 *
 * The M2 format is used by World of Warcraft for all 3D models rendered in-engine:
 * player characters, creatures, spell visuals, doodads, weapons, and UI elements.
 * An M2 model is a bundle of related files (.m2, .skin, .anim, .bone, .skel, .phys),
 * each carrying a different aspect of the model data.
 *
 * This library supports both classic (MD20) and modern chunked (MD21) formats,
 * covering versions from Vanilla (256) through Shadowlands (274).
 *
 * @example Basic Usage
 * @code
 * #include <whiteout/m2/m2.h>
 *
 * // Parse an M2 file bundle
 * whiteout::m2::Parser parser(whiteout::m2::Parser::ParseMode::Lenient);
 * whiteout::m2::FileSystem model = parser.parse("creature/scorpion/scorpion.m2");
 *
 * // Inspect the model
 * auto& hdr = model.base.header;
 * std::cout << "Version: " << hdr.version << "\n";
 * std::cout << "Bones: " << hdr.bones.size() << "\n";
 * std::cout << "Skins: " << model.skins.size() << "\n";
 *
 * // Write it back (round-trip)
 * whiteout::m2::Writer writer;
 * writer.write("output/scorpion.m2", model);
 * @endcode
 */

#include "parser.h"
#include "structures.h"
#include "types.h"
#include "writer.h"

namespace whiteout {
namespace m2 {
/// Library major version number
constexpr int MAJOR_VERSION = 1;
/// Library minor version number
constexpr int MINOR_VERSION = 0;
/// Library patch version number
constexpr int PATCH_VERSION = 0;
/// Library version string
constexpr const char* LIBRARY_VERSION = "1.0.0";
} // namespace m2
} // namespace whiteout
