// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/// StarCraft II's bone basis and WEM's, exactly: what the M3 physics converter
/// and the pose math (`m3_math.h`) both turn frames by.

#include <whiteout/vector_types.h>

namespace whiteout {
namespace models {
namespace wem {
namespace m3_physics {

// ============================================================================
// The basis, exactly
// ============================================================================

/// A record frame from StarCraft II's bone basis into WEM's: `m * R`.
Matrix44f RebaseFrame(const Matrix44f& m);
/// The inverse: `m * R^T`.
Matrix44f UnrebaseFrame(const Matrix44f& m);
/// A model-space matrix conjugated into WEM's basis: `R^T * m * R` (`IREF`).
Matrix44f RebaseConjugate(const Matrix44f& m);
/// The inverse: `R * m * R^T`.
Matrix44f UnrebaseConjugate(const Matrix44f& m);

} // namespace m3_physics
} // namespace wem
} // namespace models
} // namespace whiteout
