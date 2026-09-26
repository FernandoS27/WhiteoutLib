// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file mdx_math.h
 * @brief The arithmetic Warcraft III poses a model with: the window rule a
 *        track is read by, and the step that composes one node onto its
 *        parent.
 *
 * Moved here from the viewer's `io/mdx_eval_math.h`, which now forwards, so
 * the renderer, the editor's sampler and the `Animator` share one copy
 * (WEM_ANIMATION_RUNTIME_DESIGN.md §3.1). The curves are in `curves.h`.
 */

#include <whiteout/common_types.h>
#include <whiteout/vector_types.h>

#include <algorithm>
#include <cmath>

namespace whiteout {
namespace models {
namespace wem {

// ---- The window rule ----------------------------------------------------------

/// Which two keys a time falls between, and how far along. `lo == hi` is one
/// key held; `lo < 0` is no key in the window at all.
struct KeyBracket {
    i32 lo = -1;
    i32 hi = -1;
    f32 t = 0.0f;
};

/// Brackets @p timeMs against @p timestamps (ascending), reading only the keys
/// inside `[seqStart, seqEnd]`, as Warcraft III does: a neighbouring sequence's
/// keys are never read. Before the first of them or past the last, the span is
/// the WRAP from the last key round to the first, over the time the two leave
/// between them and the window's edges.
///
/// Past the last key the wrap is measured from the last key. Before the first,
/// the game measures the same span from the FIRST key
/// (`CKeyFrameTrackBase::SetAnimTime`, `InterpolateVolatile_*`, 3.0), so the
/// fraction is negative: the span is extrapolated backwards, and the value
/// jumps to the first key's own when the time reaches it. Neither end clamps,
/// because the game clamps neither (EDIT_MODE_ANIMATIONS_DESIGN.md §1.5).
///
/// @p Time is the renderer's `u32` or the editor's signed clip-local time: the
/// comparisons are made as `i32` either way.
template <class Time>
inline KeyBracket FindBracket(const Time* timestamps, i32 count, i32 timeMs, i32 seqStart,
                              i32 seqEnd) {
    KeyBracket b;
    if (count == 0)
        return b;

    {
        i32 lo = 0, hi = count;
        while (lo < hi) {
            i32 m = (lo + hi) >> 1;
            if ((i32)timestamps[m] < seqStart)
                lo = m + 1;
            else
                hi = m;
        }
        b.lo = lo;
    }
    i32 rangeLo = b.lo;
    if (rangeLo >= count || (i32)timestamps[rangeLo] > seqEnd) {
        b.lo = -1;
        return b;
    }

    {
        i32 lo = rangeLo, hi = count;
        while (lo < hi) {
            i32 m = (lo + hi) >> 1;
            if ((i32)timestamps[m] <= seqEnd)
                lo = m + 1;
            else
                hi = m;
        }
        b.hi = lo - 1;
    }
    i32 rangeHi = b.hi;

    if (rangeLo == rangeHi) {
        b.lo = b.hi = rangeLo;
        return b;
    }

    i32 firstFrame = (i32)timestamps[rangeLo];
    i32 lastFrame = (i32)timestamps[rangeHi];

    if (timeMs < firstFrame || timeMs >= lastFrame) {
        i32 loopLen = seqEnd - seqStart;
        i32 segLen = (firstFrame - lastFrame) + loopLen;
        b.lo = rangeHi;
        b.hi = rangeLo;
        if (segLen <= 0)
            return b;
        const i32 pos = (timeMs >= lastFrame) ? timeMs - lastFrame : timeMs - firstFrame;
        b.t = (f32)pos / (f32)segLen;
        return b;
    }

    {
        i32 lo = rangeLo, hi = rangeHi;
        while (lo < hi) {
            i32 m = (lo + hi + 1) >> 1;
            if ((i32)timestamps[m] <= timeMs)
                lo = m;
            else
                hi = m - 1;
        }
        b.lo = lo;
        b.hi = lo + 1;
        i32 denom = (i32)timestamps[b.hi] - (i32)timestamps[b.lo];
        b.t = denom > 0 ? (f32)(timeMs - (i32)timestamps[b.lo]) / (f32)denom : 0.0f;
    }
    return b;
}

// ---- The node composition ---------------------------------------------------

/// The Warcraft III node flags the composition reads (`mdx::Node::NodeFlag`).
inline constexpr u32 kMdxDontInheritTranslation = 0x1;
inline constexpr u32 kMdxDontInheritRotation = 0x2;
inline constexpr u32 kMdxDontInheritScaling = 0x4;

/// One node composed onto its parent, and the two matrices its tracks reach the
/// world through — which differ once a don't-inherit flag strips the parent
/// between them (EDIT_MODE_ANIMATIONS_DESIGN.md §5.1).
struct ComposedNode {
    /// The node's frame: rows 0..2 its axes, row 3 its pivot in model space.
    /// `MdxHierarchy`'s `stackM`, before any billboarding.
    Matrix44f frame = Matrix44f::identity();
    /// What the translation track is applied through: its linear part moves the
    /// node's origin by `t · A`.
    Matrix44f translateBasis = Matrix44f::identity();
    /// What the rotation track is expressed in: the frame before the node's own
    /// rotation and scale.
    Matrix44f rotateBasis = Matrix44f::identity();
};

/// Composes one node: @p t, @p r and @p s are its track values (offsets from
/// the rest, which the pivots carry), @p pivot its pivot, @p flags its
/// `mdx::Node::NodeFlag` bits, and @p parentFrame / @p parentPivot the parent's
/// composed frame and pivot — identity and zero for a root.
inline ComposedNode ComposeNode(const Matrix44f& parentFrame, const Vector3f& parentPivot,
                                const Vector3f& pivot, const Vector3f& t, const Quaternion& r,
                                const Vector3f& s, u32 flags) {
    const bool rmT = flags & kMdxDontInheritTranslation;
    const bool rmR = flags & kMdxDontInheritRotation;
    const bool rmS = flags & kMdxDontInheritScaling;

    const Vector3f& currPivot = pivot;
    Matrix44f M = parentFrame;
    ComposedNode out;

    auto applyTranslate = [&](f32 tx, f32 ty, f32 tz) {
        M.data[3][0] += tx * M.data[0][0] + ty * M.data[1][0] + tz * M.data[2][0];
        M.data[3][1] += tx * M.data[0][1] + ty * M.data[1][1] + tz * M.data[2][1];
        M.data[3][2] += tx * M.data[0][2] + ty * M.data[1][2] + tz * M.data[2][2];
    };

    auto stripTranslation = [&]() {
        M.data[3][0] = 0;
        M.data[3][1] = 0;
        M.data[3][2] = 0;
    };
    auto stripRotationKeepScale = [&]() {
        for (i32 row = 0; row < 3; ++row) {
            f32 mx = M.data[row][0], my = M.data[row][1], mz = M.data[row][2];
            f32 mag = std::sqrt(mx * mx + my * my + mz * mz);
            M.data[row][0] = 0;
            M.data[row][1] = 0;
            M.data[row][2] = 0;
            M.data[row][row] = mag;
        }
    };
    auto stripScaleKeepRotation = [&]() {
        for (i32 row = 0; row < 3; ++row) {
            f32 mx = M.data[row][0], my = M.data[row][1], mz = M.data[row][2];
            f32 mag = std::sqrt(mx * mx + my * my + mz * mz);
            if (mag > 1e-8f) {
                f32 inv = 1.0f / mag;
                M.data[row][0] = mx * inv;
                M.data[row][1] = my * inv;
                M.data[row][2] = mz * inv;
            }
        }
    };
    auto stripRotationAndScale = [&]() {
        for (i32 row = 0; row < 3; ++row) {
            M.data[row][0] = 0;
            M.data[row][1] = 0;
            M.data[row][2] = 0;
            M.data[row][row] = 1.0f;
        }
    };

    Vector3f tTrans = t;
    if (rmT) {
        stripTranslation();
        if (rmR && rmS)
            stripRotationAndScale();
        else if (rmR)
            stripRotationKeepScale();
        else if (rmS)
            stripScaleKeepRotation();
        tTrans.x += parentPivot.x;
        tTrans.y += parentPivot.y;
        tTrans.z += parentPivot.z;
    }
    tTrans.x += currPivot.x - parentPivot.x;
    tTrans.y += currPivot.y - parentPivot.y;
    tTrans.z += currPivot.z - parentPivot.z;
    out.translateBasis = M;
    applyTranslate(tTrans.x, tTrans.y, tTrans.z);

    if (rmR && !rmT && !rmS) {
        stripRotationKeepScale();
    }
    out.rotateBasis = M;
    if (r.x != 0.0f || r.y != 0.0f || r.z != 0.0f || r.w != 1.0f) {

        M = Matrix44f::rotation(r).transpose() * M;
    }

    if (rmS && !rmT) {
        if (rmR)
            stripRotationAndScale();
        else
            stripScaleKeepRotation();
    }
    if (s.x != 1.0f || s.y != 1.0f || s.z != 1.0f) {
        for (i32 c = 0; c < 4; ++c)
            M.data[0][c] *= s.x;
        for (i32 c = 0; c < 4; ++c)
            M.data[1][c] *= s.y;
        for (i32 c = 0; c < 4; ++c)
            M.data[2][c] *= s.z;
    }
    out.frame = M;
    return out;
}

/// The frame a skin matrix gives a node at @p pivot, `T(pivot) · skin`: the
/// inverse of `SkinFromFrame`, and what a host's `replace` override sets
/// (pose_request.h).
inline Matrix44f FrameFromSkin(const Matrix44f& skin, const Vector3f& pivot) {
    Matrix44f frame = skin;
    frame.data[3][0] +=
        pivot.x * skin.data[0][0] + pivot.y * skin.data[1][0] + pivot.z * skin.data[2][0];
    frame.data[3][1] +=
        pivot.x * skin.data[0][1] + pivot.y * skin.data[1][1] + pivot.z * skin.data[2][1];
    frame.data[3][2] +=
        pivot.x * skin.data[0][2] + pivot.y * skin.data[1][2] + pivot.z * skin.data[2][2];
    return frame;
}

/// A node's skinning matrix from its frame: `T(-pivot) · frame`, which is the
/// identity at rest on a pivot rig. `MdxHierarchy`'s `allNodeMatrices`.
inline Matrix44f SkinFromFrame(const Matrix44f& frame, const Vector3f& pivot) {
    Matrix44f worldM = frame;
    worldM.data[3][0] -=
        pivot.x * worldM.data[0][0] + pivot.y * worldM.data[1][0] + pivot.z * worldM.data[2][0];
    worldM.data[3][1] -=
        pivot.x * worldM.data[0][1] + pivot.y * worldM.data[1][1] + pivot.z * worldM.data[2][1];
    worldM.data[3][2] -=
        pivot.x * worldM.data[0][2] + pivot.y * worldM.data[1][2] + pivot.z * worldM.data[2][2];
    return worldM;
}

} // namespace wem
} // namespace models
} // namespace whiteout
