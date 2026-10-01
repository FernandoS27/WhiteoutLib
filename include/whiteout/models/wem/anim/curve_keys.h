// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file curve_keys.h
 * @brief A sub-track's keys decoded, and the edits that keep its curve: a
 *        span's value as Warcraft III plays it, the split that inserts a key
 *        without changing the curve, squad's inner points and TCB's tangents
 *        (EDIT_MODE_ANIMATIONS_DESIGN.md §3).
 *
 * The editor's Animations workspace reshapes curves with these, and the
 * physics bake splices its keys into a clip with them
 * (EDIT_MODE_PHYSICS_BAKE_DESIGN.md §8.2), so an edge key inserted by either
 * leaves the curve outside it as it was.
 */

#include <utility>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/vector_types.h>

#include "clip.h"

namespace whiteout {
namespace models {
namespace wem {

/// One key, decoded: its time in seconds, and its value with the two tangents
/// a smooth controller carries — the sub-track's `{value, inTan, outTan}`. On
/// the other controllers the tangents are the value, and unused.
template <class T>
struct CurveKey {
    f32 time = 0.0f;
    T value{};
    T in{};
    T out{};
    /// Tension, continuity and bias, on a TCB sub-track (§3.3); zero otherwise.
    Vector3f tcb{};
};

// Each template below is defined for f32, Vector3f, Vector4f, Quaternion and
// u32: the value types a Warcraft III channel keys. A u32 is held from key to
// key — its span value is the "from" key, and it has no tangents (CURVE §8).

/// @p track's keys, with their TCB parameters when it carries them. Empty when
/// its values are not sized for @p T.
template <class T>
std::vector<CurveKey<T>> DecodeKeys(const SubTrack& track);

/// Writes @p keys into @p track, with as many values per key as its `interp`
/// carries. A TCB sub-track (one that carries `tcb`) gets its parameters from
/// the keys and its tangents derived from them first: §3.3 recomputes them on
/// every write to the channel's keys, and this is the one place every write
/// passes through.
template <class T>
void EncodeKeys(SubTrack& track, const std::vector<CurveKey<T>>& keys);

/// The span from @p a to @p b at @p u in [0, 1], as the renderer plays it:
/// held, a straight line (the short arc for a rotation), Hermite, Bezier, or —
/// for a rotation's Hermite or Bezier — squad.
template <class T>
T SpanValue(Interpolation interp, const CurveKey<T>& a, const CurveKey<T>& b, f32 u);

/// The key that splits the span @p a -> @p b at @p u without changing the curve
/// (§3.5), with @p a's out-tangent and @p b's in-tangent rewritten to their
/// halves: a Hermite span's derivatives scaled by `u` and `1 - u`, a Bezier
/// span by de Casteljau. A squad span gets the curve's value, and its control
/// points are the caller's to re-derive (`DeriveSquadPoints`) — that split is
/// not exact (§12). The returned key's time is left to the caller.
template <class T>
CurveKey<T> SplitSpan(Interpolation interp, CurveKey<T>& a, CurveKey<T>& b, f32 u);

/// The span @p a -> @p b's curve over its own parameter from @p f0 to @p f1, any
/// reals, outside `[0, 1]` included, restated as a span of its own: the two
/// ends' values, the first's out-tangent and the second's in-tangent. A Hermite
/// span's derivatives scale by `f1 - f0`; a Bezier span's handles are the
/// blossom's. What Warcraft III plays before a sequence's first key is its wrap
/// span at a negative parameter (§1.5), and this is how a detach writes it down
/// exactly. A squad span gets the values alone, as `SplitSpan` gives it.
template <class T>
std::pair<CurveKey<T>, CurveKey<T>> SubSpan(Interpolation interp, const CurveKey<T>& a, const CurveKey<T>& b,
                                            f32 f0, f32 f1);

/// The tangent a hold takes (§1.5): zero where tangents are derivatives (a
/// Hermite vector), the key's own value where they are positions (a Bezier
/// handle, a squad control point).
template <class T>
T FlatTangent(Interpolation interp, const T& value);

/// Key @p i's tangents set to follow its neighbours smoothly (§3.5): the
/// Catmull–Rom derivatives — TCB at (0, 0, 0) — adjusted for uneven spacing,
/// since each span runs its own parameter from 0 to 1; an end key takes the
/// one-sided difference. A Bezier key gets them as handles, `value ± tangent/3`.
/// A rotation's smooth controllers derive their points instead
/// (`DeriveSquadPoints`), and the other controllers have no tangents.
template <class T>
void SmoothTangents(Interpolation interp, std::vector<CurveKey<T>>& keys, std::size_t i);

/// Squad's inner control points for key @p i (§3.4), from its neighbours
/// flipped into its hemisphere: `s = q · exp(-(log(q⁻¹q₊) + log(q⁻¹q₋)) / 4)`.
/// An end key uses itself for the neighbour it lacks. Both tangents take it.
void DeriveSquadPoints(std::vector<CurveKey<Quaternion>>& keys, std::size_t i);

/// Every key's tangents from its TCB parameters (§3.3): Kochanek–Bartels',
/// scaled by `2Δ₋/(Δ₋+Δ₊)` and `2Δ₊/(Δ₋+Δ₊)` for uneven spacing, since each
/// span runs its own parameter from 0 to 1; an end key takes the one-sided
/// `(1 − T)` difference. A rotation applies the same weights to the short-arc
/// logs and turns them into squad's control quaternions, which at (0, 0, 0)
/// are its inner points.
template <class T>
void TcbTangents(std::vector<CurveKey<T>>& keys);

} // namespace wem
} // namespace models
} // namespace whiteout
