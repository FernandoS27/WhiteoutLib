// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file native_bag.h
 * @brief `NativeBag` — the format leftovers a generated block does not cover.
 *
 * Three places in the model carry format-scoped extras that are neither
 * geometry nor material: a node (MDX's `nodeFamilyId`, M2's attachment id), a
 * section (MDX's `selectionFlags`, M2's `skinSectionId`), and the model header
 * (M2's global flags, the M3 `MODL` tail). §7.3 gives *material* blocks a
 * generated struct per format; these three have no generator yet, so they share
 * one name/value bag.
 *
 * One type rather than three empty near-duplicates, because the alternative was
 * three structs with identical bodies and the same open question — and because
 * a converter needs somewhere to round-trip a field today rather than after the
 * generator grows a third backend.
 *
 * Values are `i64` so an unsigned 32-bit field and a signed one both survive
 * without a per-entry type tag. A float goes in scaled, and the converter that
 * put it there is the one that takes it out.
 *
 * ### A name on a node, a section or the model header is format-scoped
 *
 * Those three bags belong to the *model*, and a model carries every profile at
 * once — so a converter reading a bare name reads whatever the importer of some
 * other format left there. Three of them had picked `flagBits` for three
 * unrelated bit vocabularies, and `toMdx` was handing `.m3`'s `BoneFlag::Real`
 * to Warcraft III as `CollisionShape`. Prefix the format: `m3FlagBits`,
 * `m2AttachmentId`, `d3LightType`.
 *
 * A `ProfileMaterialSet`'s bag is the exception and needs no prefix: a set *is*
 * one profile, only that profile's converter writes it, and a derived set
 * starts empty. `sourceVersion` means the `.m2` header version on the WoW set
 * and the appearance version on the Diablo III one, and neither can reach the
 * other.
 *
 * An entry also carries **text**, because one shipped field is a string that
 * decides what draws: a D3 `SubObject::szMaterialName` is the Maya shape name,
 * and `ActorModel_ApplyLook` selects armour by a case-sensitive substring of it.
 * Reducing that to the parsed slot/weight/variant triple loses the spelling the
 * engine's own test reads, so the bag holds the string beside the number rather
 * than beside a second bag.
 */

#include <string>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/compatibility.h>

namespace whiteout {
namespace models {
namespace wem {

/// @bind methods
struct NativeBag {
    struct Entry {
        std::string name;
        i64 value = 0;
        /// Empty on every entry a converter stored a number in. `NBAG` v2; a v1
        /// chunk reads correctly by reading nothing, which is what "no entry
        /// carried text" means.
        std::string text;

        template <class V>
        void reflect(V& v) {
            v.field("name", name);
            v.field("value", value);
            v.since(2).field("text", text);
        }
    };

    std::vector<Entry> entries;

    const Entry* find(const std::string& name) const;

    /// Replaces the entry named @p name, or appends one. Insertion order is
    /// preserved, which is what makes a text dump of two documents comparable.
    void set(const std::string& name, i64 value);

    /// The same, for the entry's text half. An entry may carry both; the two
    /// halves are independent and neither clears the other.
    void setText(const std::string& name, std::string text);

    /// @p fallback when @p name is absent — the shape every converter read has.
    i64 value(const std::string& name, i64 fallback = 0) const;

    /// Empty when @p name is absent or carried no text.
    const std::string& text(const std::string& name) const;

    bool empty() const {
        return entries.empty();
    }

    template <class V>
    void reflect(V& v) {
        v.field("entries", entries);
    }

private:
    /// The entry named @p name, appended if it was not there.
    Entry& slot(const std::string& name);
};

} // namespace wem
} // namespace models
} // namespace whiteout
