// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file wem.h
 * @brief Main header for the WEM (Whiteout Edit Model) intermediate format
 *
 * Include this single header to access all WEM types and data structures.
 *
 * WEM is a format-agnostic intermediate representation for 3D model data,
 * designed as a superset of the MDX, M2, and M3 model formats.
 *
 * v1 scope: mesh and material (static properties).
 *
 * @example Basic Usage
 * @code
 * #include <whiteout/models/wem/wem.h>
 *
 * // Create a WEM model
 * whiteout::models::wem::Model model;
 * model.name = "My Model";
 *
 * // Add a texture reference
 * whiteout::models::wem::TextureRef tex;
 * tex.path = "diffuse.blp";
 * model.textures.push_back(tex);
 *
 * // Add a material
 * whiteout::models::wem::Material mat;
 * mat.type = whiteout::models::wem::MaterialType::Standard;
 * mat.blendMode = whiteout::models::wem::BlendMode::Opaque;
 * model.materials.push_back(mat);
 * @endcode
 */

#include "converters.h"
#include "parser.h"
#include "structures.h"
#include "types.h"
#include "writer.h"
