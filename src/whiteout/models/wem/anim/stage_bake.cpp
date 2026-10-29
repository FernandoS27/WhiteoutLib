// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/anim/stage_bake.h>

#include <whiteout/models/wem/physics/crossing.h>

namespace whiteout {
namespace models {
namespace wem {

u32 BakeStages(Document& document, ProfileId target, const StageHooks* hooks, Diagnostics& diagnostics) {
    u32 rewritten = 0;
    // The rigs and cages first, so a stage over a rig overrides its start.
    rewritten += FitPhysicsToProfile(document, target, diagnostics);
    return rewritten + BakeStageKeys(document, target, hooks, diagnostics);
}

} // namespace wem
} // namespace models
} // namespace whiteout
