// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file looks.h
 * @brief Looks — the second axis (WEM v3, design §8).
 *
 * A profile says which material *set*; a look says which entry within it. The
 * table therefore lives on the `ProfileMaterialSet` and not on the `Model`: D3's
 * looks and WoW's texture variations are different vocabularies, and nothing good
 * comes of sharing an index space between them.
 *
 * The shape is D3's own, verified: every `AppearanceMaterial` holds exactly
 * `arLooks.size()` variants (233/233 across the 14 player appearances), so the
 * look index **is** the variant index and `byLook.size() == looks.size()` is an
 * invariant `Validate` can assert. A mismatch is a parse bug, not content
 * variation.
 *
 * A table is never empty. One look degenerates exactly to "one material per
 * slot", which is what the four profiles without looks have.
 */

#include <string>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/compatibility.h>

namespace whiteout {
namespace models {
namespace wem {

struct Look {
    std::string name; ///< D3 `AppearanceLook::szName`; "A" is the D3 default.
    i32 weight = 0;   ///< D3 `WeightedLook`; 0 elsewhere.

    template <class V>
    void reflect(V& v) {
        v.field("name", name);
        v.field("weight", weight);
    }
};

/// @bind methods
struct LookTable {
    std::vector<Look> looks;

    template <class V>
    void reflect(V& v) {
        v.field("looks", looks);
    }

    std::size_t size() const {
        return looks.size();
    }
    bool empty() const {
        return looks.empty();
    }

    /// The index of the look named @p name, or `kInvalidIndex`.
    u32 find(const std::string& name) const;

    /// Appends and returns its index.
    u32 add(const std::string& name, i32 weight = 0);

    /// The single-look table every profile without looks carries.
    static LookTable Single();
};

} // namespace wem
} // namespace models
} // namespace whiteout
