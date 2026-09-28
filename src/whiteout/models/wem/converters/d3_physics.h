// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file d3_physics.h
 * @brief A Diablo III appearance's rigs -> WEM (WEM_PHYSICS_DESIGN.md §8.2).
 *
 * **Import only**: D3 is never written back, so nothing only a D3 export would
 * need is kept. The two builders the client runs over one bone table are
 * replayed and what they hand Domino is stored: the anchored rig (lod 0, from
 * load) and the collapse (lod 1, an event), each with bodies of its own.
 *
 * **Cloth** is laid down as WEM topology while the geoset's mesh is built: the
 * particles are a hidden `ClothSimulated` cage section, a pinned particle is
 * anchored by its staple's bones and a free one by the staples of its driving
 * group, and the render sub-object is bound to the particle each of its
 * vertices draws from. D3's cooked constraint tables are not carried.
 */

#include <array>
#include <span>
#include <vector>

#include <whiteout/models/wem/d3_converter.h>
#include <whiteout/models/wem/diagnostics.h>
#include <whiteout/models/wem/geometry/builder.h>
#include <whiteout/models/wem/model.h>
#include <whiteout/sno/d3/native/types.h>

namespace whiteout {
namespace models {
namespace wem {
namespace d3_physics {

struct Sources {
    /// The whole `.app`, into whose payload a polytope cook points. Empty: the
    /// hulls are left out and reported.
    std::span<const u8> appearanceBytes;
    /// The actor's `.phy`; null takes the client's defaults.
    const sno::d3::native::Physics* physics = nullptr;
};

/// Fills `model.physics` with @p source's rigs. Bone `b` is node `boneToNode[b]`.
void ImportRigs(const sno::d3::native::Appearances& source, const Sources& sources,
                std::span<const u32> boneToNode, Model& model, Diagnostics& out);

/// Where one cloth sub-object's pieces went.
struct ClothSite {
    u32 mesh = kInvalidIndex;
    u32 cage = kInvalidIndex;  ///< The cage section.
    u32 bound = kInvalidIndex; ///< The render sub-object's section.
    const sno::d3::native::Cloth* tuning = nullptr;
};

/// The `.clt` the client builds @p sub's cloth with under look @p look, or
/// null where it builds none: no `.clt`, or one with no mass.
const sno::d3::native::Cloth* ClothTuning(const sno::d3::native::Appearances& source,
                                          const sno::d3::native::SubObject& sub, u32 look, AssetSource& assets);

/// Adds @p sub's cage to @p builder beside its render section @p bound, whose
/// vertices start at @p base, and records in @p bindVertex (by builder vertex)
/// the cage vertex each render vertex draws from. Returns the cage section.
u32 AddCage(geom::MeshBuilder& builder, const sno::d3::native::SubObject& sub, u32 bound, u32 base,
            std::span<const u32> boneToNode, std::vector<u32>& bindVertex, Diagnostics& out);

/// The cloths of @p sites and the colliders they use, once the meshes are in
/// place. Every `CollisionCapsule` becomes a collider, used or offered.
void ImportCloths(const sno::d3::native::Appearances& source, std::span<const ClothSite> sites,
                  std::span<const u32> boneToNode, Model& model, Diagnostics& out);

} // namespace d3_physics
} // namespace wem
} // namespace models
} // namespace whiteout
