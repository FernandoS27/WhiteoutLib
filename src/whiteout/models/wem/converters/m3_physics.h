// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file m3_physics.h
 * @brief `PHRB`, `PHSH`, `PHYJ`, `PHCL`, `PHCC`, `PHAC`, `FOR_` and `WRP_` <->
 *        WEM (WEM_PHYSICS_DESIGN.md §6).
 *
 * The parser has already brought every record to its current version
 * (`m3::UpgradePhysics`), so the import reads one layout. The export writes
 * current versions only, and rebuilds every cooked table (`m3::CookHull`,
 * `m3::CookMesh`).
 *
 * **Frames.** Every physics matrix maps a record's own frame into its bone's,
 * and the bone frame is what the quarter turn changes: `M_wem = M_sc2 * R`.
 * The record's own frame -- box extents, a capsule's axis, hull points -- is
 * untouched. The multiply is a signed permutation done by index and sign, so
 * a round trip keeps every bit, `-0.0` included.
 *
 * **Cloth topology is mesh data.** The cage and its bound regions are sections
 * of division 0's mesh. A cage vertex's anchors are its skin, `simEnabled` is
 * `cloth.movable`, and a `PHAC` lane is the cage vertex it names in
 * `cloth.bind.vertex`; `PlanClothVertices` puts all three on the division's
 * vertices before the mesh is built, and the export reads them back per
 * emitted vertex.
 */

#include <array>
#include <functional>
#include <map>
#include <utility>
#include <vector>

#include <whiteout/models/m3/structures.h>
#include <whiteout/models/wem/diagnostics.h>
#include <whiteout/models/wem/model.h>

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

// ============================================================================
// Force fields and vertex warps
// ============================================================================

/// Every AnimRef of a `FOR_` with the `EmitterProperty` sub it keys as:
/// `f(u32 sub, AnimRef<f32>&)`. @p record may be const.
template <class Record, class F>
void ForEachForceRef(Record& record, F&& f) {
    f(EmitterPropertySub(static_cast<u32>(ForceFieldProperty::Strength)), record.strength);
    f(EmitterPropertySub(static_cast<u32>(ForceFieldProperty::Width)), record.width);
    f(EmitterPropertySub(static_cast<u32>(ForceFieldProperty::Height)), record.height);
    f(EmitterPropertySub(static_cast<u32>(ForceFieldProperty::Length)), record.length);
}

/// A `WRP_`'s.
template <class Record, class F>
void ForEachWarpRef(Record& record, F&& f) {
    f(EmitterPropertySub(static_cast<u32>(VertexWarpProperty::Radius)), record.radius);
    f(EmitterPropertySub(static_cast<u32>(VertexWarpProperty::Height)), record.height);
    f(EmitterPropertySub(static_cast<u32>(VertexWarpProperty::Strength)), record.strength);
    f(EmitterPropertySub(static_cast<u32>(VertexWarpProperty::Angular)), record.angular);
    f(EmitterPropertySub(static_cast<u32>(VertexWarpProperty::Axial)), record.axial);
    f(EmitterPropertySub(static_cast<u32>(VertexWarpProperty::Radial)), record.radial);
}

ForceFieldPayload ImportForce(const m3::Force& record);
VertexWarpPayload ImportWarp(const m3::Warp& record);

/// The record, less its `boneIndex`. Every AnimRef rests at the payload's value
/// with no animId; the animation export binds them.
m3::Force ExportForce(const ForceFieldPayload& payload);
m3::Warp ExportWarp(const VertexWarpPayload& payload);

// ============================================================================
// Import
// ============================================================================

/// What `PHCL` and `PHAC` say about division 0's vertices, indexed as the
/// division's mesh numbers them: global vertex minus the division's lowest.
struct ClothVertices {
    struct Anchors {
        std::array<u32, 4> bones{};
        std::array<f32, 4> weights{};
        u32 count = 0;
    };
    /// Per vertex, into `anchors`, or -1: a cage vertex's skin comes from here.
    std::vector<i32> anchorOf;
    std::vector<Anchors> anchors;
    /// Per vertex: 1 movable, 0 pinned, 0xFF not a cage vertex.
    std::vector<u8> movable;
    /// Per vertex, the four cage vertices that move it, or `geom::kInvalidId`.
    std::vector<std::array<u32, 4>> bindVertex;
    std::vector<std::array<f32, 4>> bindWeight;
    bool anyCage = false;
    bool anyBinding = false;

    bool hasAnchors(u32 vertex) const {
        return vertex < anchorOf.size() && anchorOf[vertex] >= 0;
    }
};

/// @p division is `divisions[0]`, whose vertices the mesh numbers from
/// @p lowest; @p vertexCount is how many it holds.
ClothVertices PlanClothVertices(const m3::Model& source, const m3::MeshDivision& division,
                                u32 lowest, u32 vertexCount, Diagnostics& out);

/// The record ids the import gave each source record, parallel to
/// `rigidBodies` and `clothPhysics` (0 where none was made).
struct ImportedIds {
    std::vector<u32> bodies;
    std::vector<u32> cloths;
};

/// Fills `model.physics` from @p source. Bones are nodes 0..n-1, and division
/// 0's regions are sections of mesh 0 in region order.
ImportedIds Import(const m3::Model& source, Model& model, Diagnostics& out);

// ============================================================================
// Export
// ============================================================================

/// A cage region as `toM3` emitted it: its `REGN`, its vertices in cage order,
/// and each one's anchors as the `PHCL` bytes -- global bone indices and
/// weights summing to 255.
struct CageRegion {
    u32 region = kInvalidIndex;
    std::vector<u32> vertices; ///< WEM vertex per cage particle.
    std::vector<u32> bones;    ///< Four bone bytes per particle, packed.
    std::vector<u32> weights;  ///< Four weight bytes per particle, packed.
};

/// A bound region as `toM3` emitted it.
struct BoundRegion {
    u32 region = kInvalidIndex;
    std::vector<u32> vertices; ///< WEM vertex per emitted vertex.
};

/// Which sections are cloth, and where `toM3` put them.
struct ClothEmission {
    /// (mesh, section) -> the cloth id whose cage it is.
    std::map<std::pair<u32, u32>, u32> cageOf;
    /// (mesh, section) -> the cloth ids that bind it.
    std::map<std::pair<u32, u32>, std::vector<u32>> boundBy;
    std::map<std::pair<u32, u32>, CageRegion> cages;
    std::map<std::pair<u32, u32>, BoundRegion> bound;

    bool isCage(u32 mesh, u32 section) const {
        return cageOf.count({mesh, section}) != 0;
    }
    bool isBound(u32 mesh, u32 section) const {
        return boundBy.count({mesh, section}) != 0;
    }
};

/// The cloth sections of @p model, before the geometry pass emits them.
ClothEmission PlanClothEmission(const Model& model);

/// Where the physics records landed, for the animation export.
struct ExportRecords {
    std::map<u32, u32> bodyRecord;  ///< Body id -> `PHRB` index.
    std::map<u32, u32> clothRecord; ///< Cloth id -> `PHCL` index.
};

/// Writes `model.physics` into @p out. @p boneOf maps a node to the bone its
/// records ride (0xFFFF: none). Run after the geometry pass filled
/// @p emission.
ExportRecords Export(const Model& model, ProfileId profile, const ClothEmission& emission,
                     const std::function<u32(u32)>& boneOf, m3::Model& out, Diagnostics& diagnostics);

} // namespace m3_physics
} // namespace wem
} // namespace models
} // namespace whiteout
