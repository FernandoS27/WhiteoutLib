// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file mdx_uv.h
 * @brief Warcraft III's texture animation arithmetic, in one place.
 *
 * A `TextureAnimation` keys three values and the game composes
 * `uv' = R * S * (uv + T - 0.5) + 0.5` from them (`AnimateTextureMap`). WEM
 * keeps a layer's fixed UV state as a flat affine (`TextureInput::uvTransform`)
 * and may hold its motion as a constant rate or as another game's keys, so
 * `toMdx`, the rests and an editor all need the same three readings: the
 * affine as those values, a rate as keys, and one key in whatever type its
 * channel has. The StarCraft II counterpart is `m3_core::UvTransformOf`.
 */

#include <array>
#include <optional>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/vector_types.h>

#include "../materials/common.h"
#include "channel.h"

namespace whiteout {
namespace models {
namespace wem {
namespace mdx_uv {

/// A texture matrix MDX can hold: a turn and a scale about the texture
/// centre, and a translation applied ahead of both.
struct UvState {
    Vector2f scale{1, 1};
    f32 angle = 0;         ///< Radians, counter-clockwise about z.
    Vector2f column{0, 0}; ///< The affine's own translation column.
};

/// `TextureInput::uvTransform` read as one, or nothing when it shears.
///
/// Each COLUMN of the linear block carries one axis' scale and both name the
/// same angle. A matrix whose columns disagree -- a shear, or StarCraft II's
/// own `S * R` under a tiling that differs per axis -- has no MDX spelling.
std::optional<UvState> StandingUv(const Matrix3x2f& matrix);

/// MDX's `KTAT` value for a wanted translation COLUMN under @p state's turn
/// and scale: the column a layer gets is `R * S * (T - 0.5) + 0.5`, and this
/// is that read backwards. Unturned and unscaled it is the column itself.
Vector3f TranslationFor(const UvState& state, const Vector2f& column);

/// The three values a `TextureAnimation` plays.
struct UvValues {
    Vector2f translation{0, 0}; ///< `KTAT`'s x and y.
    f32 angle = 0;              ///< `KTAR` as a turn about z, in radians.
    Vector2f scale{1, 1};       ///< `KTAS`'s x and y.
};

/// @p matrix as the values that play it, or nothing when it shears.
std::optional<UvValues> FixedValues(const Matrix3x2f& matrix);

/// The affine @p values play: `uv' = R * S * (uv + T - 0.5) + 0.5`.
Matrix3x2f MatrixOf(const UvValues& values);

/// Whether @p values' @p channel component (`UvTranslate`, `UvRotate` or
/// `UvScale`) is the identity. A tolerance, not an equality: a turn puts a
/// cosine of 6e-17 through every term of the translation.
bool AtRest(Channel channel, const UvValues& values);

/// The period over which @p rate covers a whole number of UV tiles on every
/// axis, in seconds: the slowest moving axis sets it, one tile, and the
/// faster ones are rounded to the nearest whole tile within it. 0 when
/// nothing scrolls.
f32 SeamlessPeriod(const Vector2f& rate);

/// A constant scroll as the two Linear `KTAT` keys of one seamless period.
struct ScrollKeys {
    u32 milliseconds = 0; ///< The period, and the second key's time. 0: no scroll.
    Vector3f start{0, 0, 0};
    Vector3f end{0, 0, 0};
};
ScrollKeys ScrollKeysOf(const UvState& standing, const Vector2f& scrollRate);

/// The period of one whole turn at @p radiansPerSecond, in milliseconds: what
/// a turning layer loops on when no scroll sets a period.
u32 TurnPeriod(f32 radiansPerSecond);

/// A constant turn over @p milliseconds as five Linear `KTAR` keys a quarter
/// of the way apart, which is what a slerp needs to know which way round: the
/// turn is rounded to a whole number of turns within the period.
struct TurnKeys {
    std::array<u32, 5> times{};
    std::array<f32, 5> angles{}; ///< Radians about z.
};
TurnKeys TurnKeysOf(f32 radiansPerSecond, f32 standingAngle, u32 milliseconds);

/// A turn of @p angle radians about z as `KTAR` keys it.
Quaternion TurnAboutZ(f32 angle);

/// Whether @p type is how a `TextureAnimation` itself keys @p channel: a
/// Vector3 translation and scale, a quaternion rotation. Anything else is an
/// `.m3` layer's spelling (a two-float offset, a three-float euler angle, a
/// two-float tiling), which a set derived from StarCraft II keeps.
bool IsMdxSpelling(Channel channel, geom::AttrType type);

/// One key of @p channel stored as @p type, as the value Warcraft III plays:
/// x and y of a translation or a scale, or a turn about z in radians in `x`.
///
/// StarCraft II SUBTRACTS its offset where Warcraft III adds its translation
/// (`M3ComposeUvTransform` against `AnimateTextureMap`), so an `.m3` offset is
/// the translation negated, exactly, whatever the turn and the tiling. Its
/// rotation is `uvAngle.z`, the only euler angle a UV matrix keeps.
Vector2f DecodeKey(Channel channel, geom::AttrType type, const u8* key);

/// The inverse: @p value as one key of @p type. The component MDX ignores is
/// 0 on a translation and 1 on a scale, and a quaternion is about z.
std::vector<u8> EncodeKey(Channel channel, geom::AttrType type, const Vector2f& value);

/// @p values' @p channel component as @ref DecodeKey gives a key.
Vector2f ComponentOf(Channel channel, const UvValues& values);

} // namespace mdx_uv
} // namespace wem
} // namespace models
} // namespace whiteout
