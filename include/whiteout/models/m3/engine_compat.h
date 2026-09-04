// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file engine_compat.h
 * @brief Which engine will load a model, and how to retarget one that won't
 *
 * MD34 is one container, but StarCraft II and Heroes of the Storm do not
 * implement the same chunk set and neither is a superset of the other. Heroes
 * added the data-driven material (MADD) plus the two version bumps that carry
 * it; StarCraft II went one version further on the standard material. Version
 * support is enforced per file rather than per chunk, so a single out-of-range
 * chunk makes the whole model unloadable on the other engine.
 *
 * @see M3_FILE_FORMAT_SPECIFICATION.md §18 Engine Support Divergence
 *
 * @example Retarget a Heroes model for StarCraft II
 * @code
 * m3::Model model = m3::Parser().parse("hots_model.m3");
 * if (m3::isHeroesOnly(model)) {
 *     m3::EngineConversion result = m3::toStarCraft2(model);
 *     if (result.converted)
 *         m3::Writer().write("sc2_model.m3", result.model);
 *     else
 *         std::cerr << result.blocker << '\n';
 * }
 * @endcode
 */

#include <string>
#include <vector>
#include "structures.h"

namespace whiteout {
namespace m3 {

/// Highest MODL version StarCraft II accepts; Heroes accepts 30.
constexpr i32 SC2_MAX_MODEL_VERSION = 29;
/// Highest MAT_ version Heroes accepts; StarCraft II accepts 20.
constexpr i32 HOTS_MAX_STANDARD_MATERIAL_VERSION = 19;
/// Highest REF_ version StarCraft II accepts; Heroes accepts 3.
constexpr i32 SC2_MAX_REFLECTION_MATERIAL_VERSION = 2;

/**
 * @brief Which engines will load a model, and what stops the other one
 *
 * A model that uses no divergent feature loads on both, so `starcraft2` and
 * `heroesOfTheStorm` are independent rather than exclusive. The reason lists
 * are empty exactly when the corresponding flag is true.
 */
struct EngineSupport {
    bool starcraft2 = false;                        ///< StarCraft II will load this model
    bool heroesOfTheStorm = false;                  ///< Heroes of the Storm will load this model
    std::vector<std::string> heroesOnlyReasons;     ///< Why StarCraft II rejects it
    std::vector<std::string> starcraft2OnlyReasons; ///< Why Heroes of the Storm rejects it
};

/**
 * @brief Determine which engines will load a model
 *
 * Checks the four divergent chunks of §18: MODL version, MADD presence,
 * MaterialMap entries of type DataDriven, REF_ version and MAT_ version.
 * A MODL v30 with no MADD records is still Heroes-only — StarCraft II caps
 * the root version regardless of what the file actually contains.
 */
EngineSupport checkEngineSupport(const Model& model);

/// @brief True when StarCraft II will not load this model but Heroes will
bool isHeroesOnly(const Model& model);

/// @brief How hard toStarCraft2() should try when a MADD has no exact standard form
struct StarCraft2ConversionOptions {
    /**
     * @brief Fall back to DataDrivenMaterial::approximateStandardMaterial()
     *
     * A shader-graph material never had a StandardMaterial form, so reversing
     * it exactly is impossible. With this set the converter infers a likeness
     * and records it in `lossy`; with it clear, one such material blocks the
     * whole model. Materials the engine converted from a Displacement or
     * Reflection material, and those using unrecovered fragments, block either
     * way — approximation does not reach them.
     */
    bool approximate = true;
};

/**
 * @brief Outcome of retargeting a model for a different engine
 *
 * `model` is only meaningful when `converted` is true. `lossy` is populated
 * even on success: reversing a data-driven material cannot recover animation
 * links, collapsed layer variants, or team-colour fragments.
 */
struct EngineConversion {
    bool converted = false;         ///< Whether `model` was produced
    std::string blocker;            ///< Why not, when `converted` is false
    std::vector<std::string> lossy; ///< What the converted model cannot represent
    Model model;                    ///< Only meaningful when `converted`
};

/**
 * @brief Rewrite a Heroes-only model into one StarCraft II will load
 *
 * Reverses every MADD record into a StandardMaterial, repoints the MaterialMap
 * entries that named them, drops the REF_ v3 back-reference field, and lowers
 * MODL to v29. A model that already loads on StarCraft II is returned
 * unchanged.
 *
 * The conversion is lossy in the ways DataDrivenMaterial::toStandardMaterial()
 * documents, and `lossy` names them per material.
 */
EngineConversion toStarCraft2(const Model& model, const StarCraft2ConversionOptions& options = {});

} // namespace m3
} // namespace whiteout
