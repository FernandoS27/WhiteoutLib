// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file display.h
 * @brief How an editor draws a node (EDIT_MODE_RIGGING_DESIGN.md §2, §8): a
 *        bone's or a helper's shape, size and colour, and whether a link joins
 *        it to its parent.
 *
 * Authoring state, like the rig record: filled when a model enters the editor
 * (from its skin, §3-§4), saved with the `.wem`, and read by no exporter.
 */

#include <whiteout/common_types.h>
#include <whiteout/vector_types.h>

namespace whiteout {
namespace models {
namespace wem {

/// The body a bone is drawn as.
enum class BoneShape : u8 {
    Fin,   ///< An octahedron from the joint to its end: 3ds Max's classic bone.
    Box,   ///< Max's bone object: a box, tapering to its end.
    Stick, ///< A line.
};

/// Max's bone fins, as bits of `NodeDisplay::fins`.
enum BoneFin : u8 {
    kBoneFinSide = 1u << 0,
    kBoneFinFront = 1u << 1,
    kBoneFinBack = 1u << 2,
};

/// Who set a display record.
enum class DisplaySource : u8 {
    None, ///< Never set: the editor reads it off the skin on entry.
    Skin, ///< Read off the skin (§3-§4); Fit to skin redoes it.
    You,  ///< Changed by hand.
};

/// The gizmo a helper is drawn as (EDIT_MODE_RIGGING_DESIGN.md §8).
enum class HelperShape : u8 {
    Box,     ///< 3ds Max's dummy: width along X, length along Y, height along Z.
    Square,  ///< A rectangle across `facing`.
    Circle,  ///< A ring across `facing`.
    Sphere,  ///< Three rings, one per plane.
    Star,    ///< A star across `facing`.
    Cross,   ///< Max's point helper: a line along each axis.
    Diamond, ///< A point on each axis, joined.
};

struct HelperDisplay {
    HelperShape shape = HelperShape::Box;
    /// End to end in model units: a box's along X, Y and Z, a flat shape's
    /// across its plane, a round one's its diameter. A zero width keeps one
    /// size on screen; a zero length or height is the width.
    f32 width = 0;
    f32 length = 0;
    f32 height = 0;
    /// The axis a flat shape faces, 0 X to 2 Z: it lies across it.
    u8 facing = 2;
    /// A star's points, and its inner radius as a share of its outer one.
    u8 points = 5;
    f32 inner = 0.5f;

    bool operator==(const HelperDisplay&) const = default;

    template <class V>
    void reflect(V& v) {
        v.field("shape", shape);
        v.field("width", width);
        v.field("length", length);
        v.field("height", height);
        v.field("facing", facing);
        v.field("points", points);
        v.field("inner", inner);
    }
};

struct NodeDisplay {
    BoneShape shape = BoneShape::Fin;
    /// The node's own ink, packed as `IM_COL32` packs one (0xAABBGGRR); 0 is
    /// its kind's.
    u32 color = 0;
    /// Across the body, in model units; 0 takes it from the length. A zero
    /// height is the width.
    f32 width = 0;
    f32 height = 0;
    /// How far a `Box` narrows to its end: 0 square, 1 to a point.
    f32 taper = 0.9f;
    /// `BoneFin` bits, and how far the fins stand off the body, model units.
    u8 fins = 0;
    f32 finSize = 0;
    /// Where the bone's own body ends, in its frame, when no linked child
    /// gives it one. Zero: it has none.
    Vector3f tip{0, 0, 0};
    /// The connection gizmo: a link, or the parent's body, runs from the
    /// parent to this node.
    bool linked = true;
    DisplaySource source = DisplaySource::None;
    /// How the node is drawn when it is a helper.
    HelperDisplay helper;

    bool operator==(const NodeDisplay&) const = default;

    template <class V>
    void reflect(V& v) {
        v.field("shape", shape);
        v.field("color", color);
        v.field("width", width);
        v.field("height", height);
        v.field("taper", taper);
        v.field("fins", fins);
        v.field("finSize", finSize);
        v.field("tip", tip);
        v.field("linked", linked);
        v.field("source", source);
        // NODE v21: v20 files were written before helpers had a gizmo.
        v.since(21).field("helper", helper);
    }
};

} // namespace wem
} // namespace models
} // namespace whiteout
