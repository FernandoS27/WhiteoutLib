// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file cross_field.h
 * @brief The 4-direction field the layout is traced in
 *        (EDIT_MODE_RETOPOLOGY_DESIGN.md §1.5).
 *
 * One field per triangle in power form, u = e^{4i theta} in the triangle's own
 * frame (Knöppel, Crane, Pinkall & Schröder 2013): the four directions of a
 * cross are one complex number, so smoothness is a linear least-squares
 * problem. Triangles on a feature edge are held to it; elsewhere a soft term
 * pulls toward smoothed principal curvature directions, read from the authored
 * normals (Rusinkiewicz 2004) because a low-poly mesh's facets say little.
 *
 * Each frame lies in the tangent plane of the authored normals where they lean
 * less than about 30 degrees off the facet: a low-poly mesh's curvature sits in
 * its vertices' angle defects, which would call for a singularity at nearly
 * every one, while the shading spreads it over the faces round them, as the
 * surface it stands for does.
 */

#include <vector>

#include <whiteout/common_types.h>

#include "work_mesh.h"

namespace whiteout {
namespace models {
namespace wem {
namespace geom {
namespace retopo {

struct FieldOptions {
    /// Weight of the curvature pull against smoothness, at full anisotropy.
    /// Off: on game meshes the principal directions are the facets' noise, and
    /// the pull more than doubled the singular points of a low-poly cloth.
    f64 curvature = 0.0;
    /// Smoothing passes over the shape operator before its directions are read.
    u32 curvatureSmoothing = 3;
    /// The quad edge, to make curvature dimensionless.
    f64 scale = 1.0;
    /// Opposite singularities closer than this many quad edges are cancelled
    /// (`CancelSingularityPairs`); 0 keeps them.
    f64 cancelPairs = 1.0;
};

class CrossField {
public:
    /// Per triangle: its frame (unit `axisX`, `axisY` in the plane, unit
    /// `normal`) and the field's angle in it, in [0, pi/2).
    std::vector<V3> axisX;
    std::vector<V3> axisY;
    std::vector<V3> normals;
    std::vector<f64> angles;
    std::vector<u8> constrained; ///< Held to a feature edge.

    /// Per vertex, the field's index in quarter turns: +1 is a valence-3
    /// singularity, -1 a valence-5 one, 0 regular. Border vertices are 0.
    std::vector<i32> index;

    /// Direction @p k (0..3) of triangle @p t's cross, unit, in 3D.
    V3 direction(u32 t, u32 k) const;
    /// The angle of tangent vector @p v in triangle @p t's frame.
    f64 angleOf(u32 t, const V3& v) const;

    u32 singularityCount() const;
};

/// The frame-to-frame rotation across halfedge @p h: an angle in `t = h / 3`
/// becomes `angle + Transport(h)` in its twin's triangle.
f64 Transport(const WorkMesh& mesh, const CrossField& field, u32 h);

/// Wraps @p angle into (-pi/4, pi/4].
f64 WrapQuarter(f64 angle);

/// Solves the field on @p mesh, whose triangles have at most one feature edge
/// each (`SplitFeatureCorners`), and finds its singularities.
CrossField SolveCrossField(const WorkMesh& mesh, const Surface& surface, const FieldOptions& options);

/// Recomputes `field.index` from its angles.
void FindSingularities(const WorkMesh& mesh, CrossField& field);

/**
 * @brief Cancels pairs of a +1 and a -1 singularity closer than @p reach.
 *
 * A power field keeps such pairs cheaply where features or noise pull it
 * about, and each costs the layout a handful of patches. Round each pair a
 * small disk of free triangles is solved again by angle (a reference carried
 * over a spanning tree, the field round the disk held), which with no net
 * index inside has a field with no singularity at all. Returns the pairs
 * cancelled; `field.index` is up to date after.
 */
u32 CancelSingularityPairs(const WorkMesh& mesh, CrossField& field, f64 reach);

/// The direction a field holds to along work feature halfedge @p h: its
/// source curve's course there (`Surface::tangents`) where the surface has
/// one, else the edge itself; turned to run with the edge.
V3 FeatureDirection(const WorkMesh& mesh, const Surface& surface, u32 h);

/// Sets triangle @p t's frame: its normal from the authored normals at its
/// corners (the facet's where they cancel), its x axis along its first edge.
void SetFrame(const WorkMesh& mesh, const Surface& surface, CrossField& field, u32 t);

} // namespace retopo
} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
