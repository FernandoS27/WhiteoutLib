// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file asset_key.h
 * @brief A reference to something WEM does not contain (design §9.5, §18).
 *
 * A particle system, a sound, a second model, a texture in a game archive: WEM
 * names it and stops. Particle systems in particular are **referenced, not
 * embedded** — a faithful WEM particle model is its own design (D3 alone has
 * five motion models, nine distribution remaps and a `.prt`-owned trigger
 * array), and pretending otherwise would produce a lossy one.
 *
 * `group` and `id` mirror the SNO addressing D3 uses and are wide enough for a
 * WoW fileDataID or an M3 path index; `path` carries a name when the source had
 * one instead of a number. Either half may be empty, and a key with both empty
 * is simply absent.
 */

#include <string>

#include <whiteout/common_types.h>

namespace whiteout {
namespace models {
namespace wem {

struct AssetKey {
    static constexpr u32 kNoId = 0xFFFFFFFFu;

    u32 group = kNoId; ///< D3 SNO group, or a format-specific kind tag.
    u32 id = kNoId;    ///< SNO id / fileDataID / index.
    std::string path;  ///< The name, when the source addressed by name.

    bool empty() const {
        return id == kNoId && path.empty();
    }

    template <class V>
    void reflect(V& v) {
        v.field("group", group);
        v.field("id", id);
        v.field("path", path);
    }
};

} // namespace wem
} // namespace models
} // namespace whiteout
