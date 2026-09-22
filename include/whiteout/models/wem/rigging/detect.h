// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file detect.h
 * @brief Filling the rig record (EDIT_MODE_AUTO_IK_DESIGN.md §3.2, §3.4).
 *
 * Each source fills only what the ones before it left empty, and records
 * itself in the field's source:
 *
 * 1. **The file's own labels**: WoW's key-bone ids and its limb attachment ids.
 * 2. **Names**, from one table per naming scheme (Warcraft III HD and SD, 3ds
 *    Max Biped, Diablo III, StarCraft II), with exclusions first so
 *    `L_hand_ribbon_01` is not a hand.
 * 3. **Attachment points**: `Hand Left Ref`, `Ref_Hand Left`, `@hp_lefthand`
 *    mark where a limb ends; shape then finds the joints above.
 * 4. **Shape**: passengers (no length, or running straight on) are Twist, pads
 *    hang off a joint onto the next one's point, and the two bending joints
 *    above a known End are its Lower and Upper.
 *
 * Then the Body (the roots a figure moves with) and `ridesWith` (§3.4): by
 * names, and where the names are silent by the clips.
 *
 * Device-free, and free of any sampler: the clips' poses come in through
 * `RigPoses` (EDIT_MODE_AUTO_IK_PLAN.md P4), so the editor measures the same
 * pose it draws and no third sampler is written.
 */

#include <functional>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/vector_types.h>

#include "../model.h"

namespace whiteout {
namespace models {
namespace wem {

/// The clips' poses, handed in (P4). Any function left empty means no clip
/// evidence, and the rides the clips would find are not looked for.
struct RigPoses {
    std::function<u32()> clipCount;
    /// The times worth looking at in @p clip, in milliseconds: its roots' keys.
    /// At most `kRigSamplesPerClip` of them are read.
    std::function<std::vector<f32>(u32 clip)> keyTimes;
    /// Every node's frame at @p ms of @p clip, in the model's space, row
    /// vectors: what a marker stands on.
    std::function<std::vector<Matrix44f>(u32 clip, f32 ms)> worldAt;

    bool empty() const {
        return !clipCount || !keyTimes || !worldAt;
    }
};

/// How many of a clip's times the ride test reads at most.
inline constexpr u32 kRigSamplesPerClip = 64;

struct RigDetectOptions {
    RigPoses poses;
    /// Keep every field the user set (source You). Off redoes those too.
    bool keepYours = true;
};

/**
 * @brief Detects @p model's rig into every node's `Node::rig`.
 *
 * With `keepYours`, every field whose source is You stays and the rest are
 * cleared and redone ("Detect again"). `plant` and `twistShare` are never
 * touched: nothing detects them (the plan's P2).
 */
void DetectRig(Model& model, const RigDetectOptions& options = {});

/// Detects only a model none of whose nodes has a record: a fresh import, or a
/// `.wem` saved before the record existed. True when it detected.
bool EnsureRig(Model& model, const RigDetectOptions& options = {});

/// Whether any node of @p model has a record.
bool HasRig(const Model& model);

} // namespace wem
} // namespace models
} // namespace whiteout
