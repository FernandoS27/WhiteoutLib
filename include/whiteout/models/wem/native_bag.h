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

        template <class V>
        void reflect(V& v) {
            v.field("name", name);
            v.field("value", value);
        }
    };

    std::vector<Entry> entries;

    const Entry* find(const std::string& name) const;

    /// Replaces the entry named @p name, or appends one. Insertion order is
    /// preserved, which is what makes a text dump of two documents comparable.
    void set(const std::string& name, i64 value);

    /// @p fallback when @p name is absent — the shape every converter read has.
    i64 value(const std::string& name, i64 fallback = 0) const;

    bool empty() const {
        return entries.empty();
    }

    template <class V>
    void reflect(V& v) {
        v.field("entries", entries);
    }
};

} // namespace wem
} // namespace models
} // namespace whiteout
