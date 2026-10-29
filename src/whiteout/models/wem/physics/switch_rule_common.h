// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/// What the physics switch sources share.

#include <algorithm>

#include <whiteout/models/wem/physics/physics.h>

namespace whiteout {
namespace models {
namespace wem {
namespace switchdetail {

inline bool Holds(const PhysicsRig& rig, u32 body) {
    return std::find(rig.bodies.begin(), rig.bodies.end(), body) != rig.bodies.end();
}

} // namespace switchdetail
} // namespace wem
} // namespace models
} // namespace whiteout
