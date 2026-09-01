// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file model.h
 * @brief `Model` — one geometry, several material sets (WEM v3, design §6.3).
 *
 * This is the structure F5 says is impossible today: a document carrying a
 * classic material set *and* a Reforged one over one geometry, or SC2's and
 * Heroes' vocabularies over one `.m3`. Four rules make it well-defined, and all
 * four are here rather than spread across the converters:
 *
 * 1. **Geometry never mentions a profile except to opt out.** A face's `section`
 *    names a `materialSlot`; the section's `profiles` mask says which profiles
 *    draw it. Nothing else in `Mesh` is profile-aware.
 * 2. **Coverage is validated.** For every profile *p* and every slot referenced
 *    by a section whose mask includes *p*, the binding must exist. A hole is an
 *    error, not a fallback — falling back silently is how a character ships with
 *    one untextured shoulder.
 * 3. **Sets are independent.** Adding, editing or deleting a profile's set
 *    touches nothing else. That is the whole reason the slot list is a separate
 *    array of names.
 * 4. **Textures are shared.** `Document::textures[]` is document-wide, so two
 *    profiles that use the same file reference one entry.
 *
 * `Resolve` is the only place the profile axis and the look axis meet.
 */

#include <string>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/compatibility.h>

#include "bounds.h"
#include "geometry/mesh.h"
#include "materials/looks.h"
#include "materials/material.h"
#include "native_bag.h"
#include "nodes/tree.h"
#include "profile.h"

namespace whiteout {
namespace models {
namespace wem {

/**
 * @brief Which material each look picks, for one slot.
 *
 * Sized to the set's `LookTable`. An entry of `kInvalidIndex` is a hole the
 * coverage rule reports — §7.5's removal operations leave one rather than
 * silently repointing at a neighbour, because "which material did you mean" is
 * not a question this layer can answer.
 */
struct SlotBinding {
    std::vector<u32> byLook;

    bool bound(u32 look) const {
        return look < byLook.size() && byLook[look] != kInvalidIndex;
    }

    template <class V>
    void reflect(V& v) {
        v.field("byLook", byLook);
    }
};

/**
 * @brief The profile-scoped model-level record — the MDX model header's own
 *        fields, M2's global flags, the M3 `MODL` tail.
 *
 * The shared name/value bag (`native_bag.h`); see `NodeNative`.
 */
using ModelNative = NativeBag;

struct ProfileMaterialSet {
    ProfileId profile = ProfileId::Generic;
    LookTable looks; ///< Sized 1 for profiles without looks (§8).

    /// Which look this set draws when nobody says otherwise.
    ///
    /// On the set rather than the `Model` because the look axis is per set: D3's
    /// looks and WoW's texture variations are different vocabularies and share no
    /// index space. An importer that was *told* which look to build records it
    /// here, so the document says what it is rather than leaving the answer in
    /// the caller's arguments.
    u32 defaultLook = 0;

    std::vector<Material> materials; ///< Set-local array.
    std::vector<SlotBinding> slotBindings; ///< Parallel to `Model::materialSlots`.
    ModelNative native;

    /// Resizes `slotBindings` to @p slotCount and every `byLook` to the look
    /// count, filling new entries with `kInvalidIndex`. The one call that keeps
    /// the two parallel-array invariants true after a structural change.
    void resizeBindings(std::size_t slotCount);

    template <class V>
    void reflect(V& v) {
        v.field("profile", profile);
        v.field("looks", looks);
        v.field("defaultLook", defaultLook);
        v.field("materials", materials);
        v.field("slotBindings", slotBindings);
        v.field("native", native);
    }
};

/**
 * @brief The animatable properties of this model, declared once (§10.8).
 *
 * Empty until P7, and empty is a normal state — a model with no animation carries
 * no channels. It is declared now so `Model`'s shape is final and nothing has to
 * move when the semantics arrive.
 */
struct AnimChannelTable {
    bool empty() const {
        return true;
    }

    /// Empty until P7, and an empty body writes nothing -- which is what
    /// keeps `Model` byte-stable when the channels arrive.
    template <class V>
    void reflect(V&) {}
};

/// @bind methods
struct Model {
    std::string name;
    std::vector<Mesh> meshes;               ///< Shared across profiles.
    NodeTree nodes;                         ///< Shared: bones, lights, attachments, emitters.
    std::vector<std::string> materialSlots; ///< Shared: the join key.
    AnimChannelTable animChannels;          ///< Shared: the animatable properties.
    std::vector<ProfileMaterialSet> profileSets;
    Extent bounds;

    /// The set for @p profile, or null. Sets are unordered in the vector and
    /// there is at most one per profile — a second is a structural error.
    const ProfileMaterialSet* setFor(ProfileId profile) const;
    ProfileMaterialSet* setFor(ProfileId profile);

    /// The index of the slot named @p name, or `kInvalidIndex`.
    u32 slotIndex(const std::string& name) const;

    /// Appends a slot if it is not already there and returns its index.
    u32 addSlot(const std::string& name);

    /// The mask of profiles at least one section draws in. Cheap, and what a UI
    /// wants when it asks "what is actually in this file".
    ProfileMask drawnProfiles() const;

    template <class V>
    void reflect(V& v) {
        v.field("name", name);
        v.field("meshes", meshes);
        v.field("nodes", nodes);
        // A run of strings is a count and N references, not a chunk: `MSLT`
        // would have to be a chunk of `CHAR` chunks, which the container has no
        // way to say.
        v.inlineList("materialSlots", materialSlots);
        v.field("animChannels", animChannels);
        v.field("profileSets", profileSets);
        v.field("bounds", bounds);
    }
};

/**
 * @brief The material profile @p profile draws for @p slot at @p look.
 *
 * Two array reads and a bounds check. Returns null for an unbound slot, an
 * undeclared profile or an out-of-range look — every one of which `Validate`
 * reports at `ValidateLevel::Profile`, so a null here in production code means
 * the document was never validated.
 */
const Material* Resolve(const Model& model, u32 slot, ProfileId profile, u32 look = 0);

} // namespace wem
} // namespace models
} // namespace whiteout
