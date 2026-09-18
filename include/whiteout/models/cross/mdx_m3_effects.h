// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file mdx_m3_effects.h
 * @brief Warcraft III cameras and collision shapes into StarCraft II camera aims and hit tests.
 *
 * The emitter systems cross inside WEM (`wc3_sc2_emitters.h`). What is left
 * here crosses native to native, from the parsed `.mdx` into the `.m3` `toM3`
 * just wrote, joined through the node's `objectId` and `M3ExportMap`
 * (WC3_TO_SC2_COMPLETION_PLAN.md §2): a camera's aim, which WEM does not hold
 * (a `CameraPayload` is a lens, not a target), and each collision shape's
 * fuzzy hit test. Nothing here makes a bone: a hit test rides the bone `toM3`
 * made under `M3ExportSettings::effectNodeBones`, and a keyed aim is cut into
 * the same clip windows the node's own tracks were, then written through the
 * one stream writer the animation export uses.
 */

#include <whiteout/models/m3/structures.h>
#include <whiteout/models/mdx/structures.h>
#include <whiteout/models/wem/converters.h>
#include <whiteout/models/wem/diagnostics.h>
#include <whiteout/models/wem/document.h>

namespace whiteout {
namespace models {
namespace cross {

/// What the caller decides that the source does not say.
struct Wc3EffectOptions {
    /// Warcraft III units to the written model's: the staging rescale, 0.01 for
    /// StarCraft II. Every length a record states is multiplied by it.
    f32 lengthScale = 0.01f;
    /// Each CAM_ aimed at its Warcraft III target, with Blizzard's field of view.
    bool cameras = true;
    /// Each sphere or box collision shape (CLID) as a fuzzy hit test.
    bool hitTests = true;
};

struct Wc3EffectReport {
    u32 cameraRecords = 0; ///< CAM_ bones turned toward their targets.
    u32 hitTests = 0;      ///< Fuzzy hit tests written for collision shapes.
    u32 keyedStreams = 0;  ///< SD blocks written for keyed aims.
    wem::Diagnostics diagnostics;
};

/// After `toM3`: turns each camera's bone toward its target (its rest, its
/// IREF, and a rotation stream where the camera, its target or its roll is
/// keyed), and writes a fuzzy hit test per sphere or box collision shape on its
/// node's bone.
///
/// @p staged is the document `toM3` was given and @p map what it reported; the
/// collision nodes must have bones (`M3ExportSettings::effectNodeBones`).
Wc3EffectReport CrossWc3Effects(const mdx::Model& source, const wem::Document& staged,
                                const wem::M3ExportMap& map, const Wc3EffectOptions& options,
                                m3::Model& out);

} // namespace cross
} // namespace models
} // namespace whiteout
