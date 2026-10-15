// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file m3_uv.h
 * @brief StarCraft II's texture transform arithmetic, in one place.
 *
 * A `MAT_` layer keys three values and the engine composes
 * `uv' = S * R * (uv - (0.5 + offset)) + 0.5` from them, where only
 * `uvAngle.z` reaches the 2D result. `m3_core` reads a layer's rest through it
 * and writes a common view's affine back through it; an editor's texture tool
 * moves the same three values. The Warcraft III counterpart is `mdx_uv`.
 */

#include <whiteout/common_types.h>
#include <whiteout/vector_types.h>

#include "../materials/common.h"

namespace whiteout {
namespace models {
namespace wem {
namespace m3_uv {

/// The three values a layer plays.
struct UvValues {
    Vector2f offset{0, 0}; ///< `uvOffset`.
    f32 angle = 0;         ///< `uvAngle.z`, radians.
    Vector2f tiling{1, 1}; ///< `uvTiling`, a zero axis already read as one.
};

/// The affine @p values compose.
Matrix3x2f MatrixOf(const UvValues& values);

/// The values @p matrix came from: exact for a true `tiling * rotation`, and
/// the closest scale-and-turn for anything else (row 0 names the angle).
UvValues ValuesOf(const Matrix3x2f& matrix);

} // namespace m3_uv
} // namespace wem
} // namespace models
} // namespace whiteout
