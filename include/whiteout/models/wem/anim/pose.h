// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file pose.h
 * @brief Reading a clip back: a track's value at a time, and the frame a node
 *        stands in while it plays.
 *
 * WEM stores animation and does not play it, so for a long while nothing here
 * had to ask "where is this bone at 0.4 s": the converters rewrite keys at
 * their own times and the renderer brought its own evaluator. Two callers do
 * ask. `RetargetSkeleton` samples a source rig to restate it, and `Rebind`'s
 * **favour the animations** fits a rest mesh to the frames it will be seen in
 * (EDIT_MODE_TPOSE_DESIGN.md §5.5). This is the one answer both read rather
 * than a second and a third interpolator kept in step by hand.
 *
 * It only reads. A `Hermite` or `Bezier` track is sampled on its values and
 * loses its tangents, which is what `AnimTrackApproximated` reports.
 */

#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/vector_types.h>

#include "../document.h"

namespace whiteout {
namespace models {
namespace wem {

/// @p track's value at @p time, as @p count floats. Honours `Step`, takes the
/// shortest arc between two quaternion keys, and holds the end values outside
/// the track's own span.
void SampleTrack(const SubTrack& track, geom::AttrType type, f32 time, f32* out, u32 count);

/// `T(-p) * S * R * T(p + t)` — the local matrix a pivot rig composes, in row
/// vectors. The pivot cancels out of the translation row, which is why a rig is
/// free to keep the bind position there and why `.mdx`'s `PIVT` does.
Matrix44f PivotComposition(const Transform& trs, const Vector3f& pivot);

/**
 * @brief One model's nodes as one clip poses them.
 *
 * The tracks are found once, at construction; after that any number of times
 * may be sampled. Holding the document by reference is deliberate — this is a
 * cursor over it, not a copy of it — so it must not outlive the document or
 * survive an edit that moves the clip's tracks.
 */
class ClipPose {
public:
    ClipPose(const Document& document, u32 model, u32 clip);

    /// No model, no clip, or a model with no nodes.
    bool empty() const {
        return tree_ == nullptr;
    }

    /// Every time this clip keys any node channel at, ascending and without
    /// repeats: the frames a sampler cannot skip.
    const std::vector<f32>& keyTimes() const {
        return times_;
    }

    /// @p node's local transform at @p seconds: its sub-track where there is
    /// one, then the channel's declared rest, then the rig's own — the identity
    /// for a pivot rig, whose rest *is* "no track at all".
    Transform local(u32 node, f32 seconds) const;

    /// @p node's model-space frame at @p seconds, composed up the chain.
    Matrix44f frame(u32 node, f32 seconds) const;

    /// `A(b,t)`: the matrix the runtime skins a vertex with, the identity at
    /// rest. For a pivot rig that is the composed chain itself; otherwise it is
    /// the inverse bind ahead of it.
    Matrix44f skinning(u32 node, f32 seconds) const;

    /// Every node's `A(b,t)` at @p seconds at once, indexed by node — one walk
    /// of the tree instead of one per node, which is what a per-vertex fit over
    /// tens of frames needs.
    void skinningAt(f32 seconds, std::vector<Matrix44f>& out) const;

private:
    struct Slot {
        const SubTrack* track = nullptr;
        const AnimChannel* channel = nullptr;
        geom::AttrType type = geom::AttrType::F32x3;
    };

    const NodeTree* tree_ = nullptr;
    /// Three slots a node — translation, rotation, scale — in that order.
    std::vector<Slot> slots_;
    /// Parents before children: an `.mdx` numbers by `objectId`, which is per
    /// chunk, so the array's order is not the hierarchy's.
    std::vector<u32> order_;
    std::vector<f32> times_;
};

} // namespace wem
} // namespace models
} // namespace whiteout
