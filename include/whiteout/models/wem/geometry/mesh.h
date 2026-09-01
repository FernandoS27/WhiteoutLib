// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file mesh.h
 * @brief `Mesh` and `MeshSection` (WEM v3, design §5.5, §5.9).
 *
 * **A section is not a face range.** Revision 1 made a submesh a contiguous face
 * range and required faces to stay sorted by submesh — an invariant that does not
 * survive an editing kernel, because the first `deleteFace` or `splitEdge` breaks
 * it and re-sorting after every edit is not an option. So the binding inverts:
 * the `section` Face attribute is authoritative, a face knows its section, and
 * sections impose no ordering. A face created by a split inherits its parent's
 * value, which is the behaviour you want and costs nothing.
 *
 * **Connectivity is built, not stored** (§5.9). The mesh's own form is the
 * indexed face set; `ensureConnectivity()` builds the half-edge arrays from it
 * and `invalidateConnectivity()` drops them. The bulk conversion path
 * (`.m3` -> WEM -> `.m3` with no editing) touches neither, so it never pays the
 * ~52 B/triangle.
 */

#include <string>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/compatibility.h>

#include "../bounds.h"
#include "../native_bag.h"
#include "../profile.h"
#include "attributes.h"
#include "repair.h"
#include "skin.h"
#include "topology.h"

namespace whiteout {
namespace models {
namespace wem {

/**
 * @brief The per-profile native record of a draw section.
 *
 * P3 turns this into the profile-scoped variant of §7.3 — the D3 geoset
 * descriptor, the M3 batch's own fields, and so on. It is an empty struct until
 * then so that `MeshSection`'s shape is final from P1 and nothing has to move
 * when the variant arrives.
 */
/// A section's format leftovers -- MDX `selectionFlags`, M2 `skinSectionId`
/// and `materialLayer`. The shared bag (`native_bag.h`); see `NodeNative`.
using SectionNative = NativeBag;

enum class SectionFlags : u32 {
    None = 0,
    Hidden = 0x1,          ///< M3's per-batch visibility bit; the section is not submitted.
    ClothSimulated = 0x2,  ///< The section's vertices are driven by cloth.
    ClothInfluenced = 0x4, ///< The section is deformed by cloth it does not own.
    Billboard = 0x8,       ///< The section is oriented per view rather than by its node.
    ProjectedShadow = 0x10,
};

constexpr SectionFlags operator|(SectionFlags a, SectionFlags b) {
    return static_cast<SectionFlags>(static_cast<u32>(a) | static_cast<u32>(b));
}
constexpr SectionFlags operator&(SectionFlags a, SectionFlags b) {
    return static_cast<SectionFlags>(static_cast<u32>(a) & static_cast<u32>(b));
}
inline SectionFlags& operator|=(SectionFlags& a, SectionFlags b) {
    a = a | b;
    return a;
}
constexpr bool hasFlag(SectionFlags value, SectionFlags bit) {
    return (static_cast<u32>(value) & static_cast<u32>(bit)) != 0;
}

/// Metadata only; one per draw section. The faces that belong to it are the ones
/// whose `section` attribute names it.
struct MeshSection {
    std::string name;
    u32 materialSlot = 0;                ///< -> `Model::materialSlots[]`.
    ProfileMask profiles = kAllProfiles; ///< Which profiles draw this section (§6).
    std::optional<u32> rigidNode;        ///< Set: every vertex binds here at weight 1 (§5.6).
    u16 selectionGroup = 0;              ///< MDX geoset group / M2 skinSectionId.
    SectionFlags flags = SectionFlags::None;
    Extent bounds; ///< Derived, recomputed.
    SectionNative native;

    template <class V>
    void reflect(V& v) {
        v.field("name", name);
        v.field("materialSlot", materialSlot);
        v.field("profiles", profiles);
        v.optional("rigidNode", rigidNode);
        v.field("selectionGroup", selectionGroup);
        v.field("flags", flags);
        v.field("bounds", bounds);
        v.field("native", native);
    }
};

// ============================================================================
// Mesh
// ============================================================================

/// @bind methods
class Mesh {
public:
    std::string name;
    u32 lodLevel = 0;
    geom::AttributeSet attributes; ///< Includes the `section` Face layer.
    geom::SkinBinding skin;
    std::vector<MeshSection> sections;
    geom::RepairLog repairLog; ///< What import had to do to make this a manifold (§5.3).
    Extent bounds;

    // --- geometry ------------------------------------------------------------

    /**
     * @brief The indexed face set — the mesh's own form.
     *
     * Regenerated from the connectivity first when an edit has made it stale, so
     * this is always current even mid-edit.
     */
    const geom::FaceSet& faceSet() const;

    /// Replaces the geometry. Drops any connectivity and resizes the Vertex and
    /// Face attribute domains to match; Halfedge and Edge domains are sized by
    /// `ensureConnectivity`, since only the build knows how many there are.
    void setFaceSet(geom::FaceSet faces);

    bool hasConnectivity() const {
        return connectivity_;
    }

    /**
     * @brief Builds the half-edge arrays if they are not already there.
     *
     * Deterministic and idempotent. Fails only on a non-manifold face set, which
     * `MeshBuilder` cannot produce — a mesh that came through the builder always
     * builds.
     */
    geom::BuildResult ensureConnectivity();

    /// Drops the half-edge arrays, syncing the face set first if an edit made it
    /// stale. The mesh is unchanged; only the cache goes.
    void invalidateConnectivity();

    /// Mutable connectivity. Marks the face set stale, so the next `faceSet()`
    /// regenerates. Only valid after `ensureConnectivity()`.
    geom::Topology& topology();
    const geom::Topology& topology() const {
        return topology_;
    }

    // --- counts, whichever form is current -----------------------------------

    u32 vertexCount() const;
    u32 faceCount() const;

    // --- sections -------------------------------------------------------------

    /// The `section` Face layer, created if absent. Faces default to section 0.
    std::span<u32> faceSections();
    std::span<const u32> faceSections() const;

    /// Faces belonging to @p section, in face order. A bucket pass at the point
    /// of use, which is what replaces the old sorted-range invariant.
    std::vector<u32> facesOfSection(u32 section) const;

    // --- derived data ---------------------------------------------------------

    /// Recomputes `bounds` and every section's `bounds` from `position`.
    void recomputeBounds();

    /**
     * @brief The mesh's serialized form.
     *
     * Connectivity is **not** in it. The half-edge arrays are a derived cache
     * that `ensureConnectivity` rebuilds deterministically from the face set, so
     * storing them would be storing a second copy of the same information and
     * inviting the two to disagree. The face set is the on-disk form (§5.9).
     *
     * This is the one place a reflected struct has to know which direction it is
     * being visited in: writing has to flush a face set an edit left stale, and
     * reading has to drop a connectivity cache that describes the previous
     * contents.
     */
    template <class V>
    void reflect(V& v) {
        v.field("name", name);
        v.field("lodLevel", lodLevel);

        if constexpr (!V::kReading) {
            syncFaceSet();
        }
        v.field("faceSet", faces_);
        if constexpr (V::kReading) {
            topology_ = geom::Topology{};
            connectivity_ = false;
            facesStale_ = false;
        }

        v.field("attributes", attributes);
        v.field("skin", skin);
        v.field("sections", sections);
        v.field("repairLog", repairLog);
        v.field("bounds", bounds);
    }

private:
    void syncFaceSet() const;

    mutable geom::FaceSet faces_;
    geom::Topology topology_;
    bool connectivity_ = false;
    mutable bool facesStale_ = false;
};

} // namespace wem
} // namespace models
} // namespace whiteout
