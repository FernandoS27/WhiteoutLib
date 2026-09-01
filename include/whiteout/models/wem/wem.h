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
 * designed as a superset of the MDX, M2, M3 and Diablo III model formats.
 *
 * The data model is a half-edge geometry kernel, profiles, a self-contained node
 * model and per-game native material blocks alongside the common ones — see
 * WEM_DESIGN.md.
 *
 * @example Basic Usage
 * @code
 * #include <whiteout/models/wem/wem.h>
 *
 * namespace wem = whiteout::models::wem;
 *
 * wem::Document document;
 * document.declare(wem::ProfileId::Sc2);
 * document.defaultProfile = wem::ProfileId::Sc2;
 *
 * wem::Model model;
 * model.materialSlots.push_back("body");
 *
 * // One material set per profile, over the shared geometry and slot list.
 * wem::ProfileMaterialSet set;
 * set.profile = wem::ProfileId::Sc2;
 * set.looks = wem::LookTable::Single();
 * set.materials.emplace_back();
 * set.resizeBindings(model.materialSlots.size());
 * set.slotBindings[0].byLook[0] = 0;
 * model.profileSets.push_back(std::move(set));
 * document.models.push_back(std::move(model));
 *
 * const wem::Diagnostics report =
 *     wem::Validate(document, wem::ValidateLevel::Profile);
 * @endcode
 */

#include "document.h"
#include "geometry/builder.h"
#include "geometry/ops.h"
#include "geometry/render_view.h"
#include "materials/ops.h"
#include "model.h"
#include "nodes/remove.h"
#include "nodes/visitor.h"
#include "retarget.h"

#include "converters.h"
#include "d3_converter.h"
#include "diagnostics.h"
#include "parser.h"
#include "profile.h"
#include "validate.h"
#include "writer.h"
