// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file refine.h
 * @brief Subdivide, Smooth and Orient Outward: the presses that refine a mesh's
 *        shape rather than cut its topology by hand.
 *
 * Each plans as the modelling tools do (modelling.h): the topology is written
 * at Plan, the plan says what changed, and `FinishTool` ends it, so an editor
 * journals each one as a single press.
 */

#include <whiteout/common_types.h>

#include "modelling.h"

namespace whiteout {
namespace models {
namespace wem {
namespace geom {

enum class SubdivideScheme : u8 {
    CatmullClark, ///< The points move by Catmull-Clark's rules: the surface rounds.
    Linear,       ///< The points stay on the old faces: more faces, the same shape.
};

struct SubdivideParams {
    u32 levels = 1;
    SubdivideScheme scheme = SubdivideScheme::CatmullClark;
    /// Sharp edges hold their line as borders do (Catmull-Clark's crease
    /// rule); off, only borders do.
    bool creases = true;
};

/**
 * @brief Subdivide: each of @p faces (every face when the set holds none) cut
 *        into one quad per corner, about its centre and its edges' midpoints.
 *
 * A face outside the set that shares an edge with it gains that edge's
 * midpoint, so nothing cracks. Catmull-Clark then moves the points: the set's
 * interior edges and vertices by the smooth rules; borders and creases by the
 * crease rule, a vertex on three creases or more not at all; and anything
 * touching a face outside the set stays where it was, so what was not chosen
 * keeps its shape. Corner values (UVs, colours) blend linearly inside each
 * face, so a seam stays a seam; skin is blended and renormalised; every new
 * face keeps its old face's section.
 *
 * Output selection: the new faces. `changed` is how many there are.
 */
ModelPlan PlanSubdivide(Mesh& mesh, const ElementSet& faces, const SubdivideParams& params = {});

enum class SmoothScheme : u8 {
    Laplacian, ///< Each pass pulls every vertex toward its neighbours' mean.
    Taubin,    ///< Each pass is followed by a push back out: no shrinking.
};

struct SmoothParams {
    u32 iterations = 1;
    f32 factor = 0.5f; ///< How far toward the mean a pass moves a vertex, 0 to 1.
    SmoothScheme scheme = SmoothScheme::Laplacian;
    bool pinBorders = true; ///< A border vertex stays where it is.
};

/**
 * @brief Smooth: @p elements' vertices (its faces' when it names none) each
 *        moved toward the mean of the vertices it shares an edge with,
 *        `iterations` times, every vertex of a pass read before any is
 *        written. Positions only: nothing renumbers.
 */
ModelPlan PlanSmooth(Mesh& mesh, const ElementSet& elements, const SmoothParams& params = {});

/**
 * @brief Orient Outward: every connected part @p faces reach (every part when
 *        the set holds none) whose faces enclose a negative volume reversed,
 *        so each part's faces look out.
 *
 * A built mesh's parts are each wound one way already (§5.3), so a part is
 * right or reversed whole. The volume is taken about the part's own centre, so
 * an open part reads by which way most of it faces. Corner normals reverse
 * with their faces; re-shade after. `changed` counts the faces reversed.
 */
ModelPlan PlanOrientOutward(Mesh& mesh, const ElementSet& faces);

} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
