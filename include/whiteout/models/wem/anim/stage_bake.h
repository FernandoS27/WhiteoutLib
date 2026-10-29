// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file stage_bake.h
 * @brief The export's stage bake with its physics fitted first: the editing
 *        module's half of `BakeStageKeys` (`stages.h`).
 */

#include <whiteout/common_types.h>

#include "../diagnostics.h"
#include "../document.h"
#include "../profile.h"
#include "stages.h"

namespace whiteout {
namespace models {
namespace wem {

/// `BakeStageKeys` after `FitPhysicsToProfile`: the rigs and cages first, so a
/// stage over a rig overrides its start. Returns the clips rewritten by both.
u32 BakeStages(Document& document, ProfileId target, const StageHooks* hooks,
               Diagnostics& diagnostics);

} // namespace wem
} // namespace models
} // namespace whiteout
