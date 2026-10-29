// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file mdx_bag.h
 * @brief A geoset's static colour, alpha and flag words, as an MDX section
 *        keeps them in its native bag and flags.
 *
 * The section keeps them in the import's own representation, so an edit back
 * to the imported value compares equal. `MdxConverter` is the only writer
 * (`fromMdx`, `toMdx` and the Mesh workspace go through it); reading them needs
 * no converter.
 */

#include <whiteout/common_types.h>
#include <whiteout/vector_types.h>

#include "../geometry/mesh.h"

namespace whiteout {
namespace models {
namespace wem {

/// A geoset's static colour and alpha as the Mesh workspace edits them
/// (EDIT_MODE_MESH_DESIGN.md §6.2). Hidden and alpha are two values: the file
/// writes 0 for a hidden geoset, and the document keeps its alpha for when it
/// is shown again.
struct GeosetTint {
    Vector3f color{1.0f, 1.0f, 1.0f}; ///< Red first, as the file's static colour.
    f32 alpha = 1.0f;                 ///< The opacity when shown.
    bool hidden = false;              ///< `SectionFlags::Hidden`: written as alpha 0.

    bool operator==(const GeosetTint&) const = default;
};

/// A geoset's two flag words as the Mesh workspace edits them
/// (EDIT_MODE_MESH_DESIGN.md §6.3): every bit, named or not, so one nobody has
/// named can still be reached and still round-trips.
struct GeosetFlags {
    /// The geoset's `selectionFlags`. 0x4 is Unselectable: the game's click
    /// ray skips the geoset (`IModelTestRay`).
    u32 selection = 0;
    /// Its geoset animation's flags. 0x1 is `DropShadow` (the section's
    /// `ProjectedShadow`), 0x2 `Color`, which every record the export makes
    /// carries; the rest are kept as the file had them.
    u32 animation = 0x2;

    static constexpr u32 kUnselectable = 0x4;
    static constexpr u32 kDropShadow = 0x1;

    bool operator==(const GeosetFlags&) const = default;
};

// A section's geoset-animation leftovers in its native bag, spelled here and
// nowhere else (`GeosetTintOf` / `SetGeosetTint`). Floats are their bit patterns.
inline constexpr const char* kGeosetColorR = "geosetColorR";
inline constexpr const char* kGeosetColorG = "geosetColorG";
inline constexpr const char* kGeosetColorB = "geosetColorB";
inline constexpr const char* kGeosetAlpha = "geosetAlpha";
/// The record's flags word without `DropShadow`, when it is not plain `Color`.
inline constexpr const char* kGeosetAnimFlags = "geosetAnimFlags";

/// White, 1 and shown for a section that carries none of them.
GeosetTint GeosetTintOf(const MeshSection& section);

/// Writes @p tint as the import would have. White and an alpha of 1 carry
/// no bag entry. An alpha of 0 or less is Hidden, and leaves the stored
/// alpha as it was, so showing the geoset again gives its opacity back.
void SetGeosetTint(MeshSection& section, const GeosetTint& tint);

/// The section's two flag words; `selection` 0 and `animation` `Color`
/// for a section that carries neither.
GeosetFlags GeosetFlagsOf(const MeshSection& section);

/// Writes @p flags as the import would have: `DropShadow` as the section's
/// `ProjectedShadow`, the rest of the animation word in the bag only when
/// it is not plain `Color`.
void SetGeosetFlags(MeshSection& section, const GeosetFlags& flags);

} // namespace wem
} // namespace models
} // namespace whiteout
