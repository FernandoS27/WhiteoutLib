// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file native.h
 * @brief The format-scoped native material records (WEM v3, design §7.3).
 *
 * These are **WEM-owned mirrors**, not the parser structs. The cost is real
 * duplication; what it buys is that the WEM binary format is not hostage to every
 * refactor of the parsers' structs — a chunk version can describe a mirror and
 * cannot describe someone else's header. It also keeps `structures.h` from
 * pulling in every format's headers, which the bindings and the wasm build would
 * pay for.
 *
 * Each block carries `sourceVersion`, because the same field means different
 * things at different versions and a converter needs to know.
 *
 * ### Where the bodies come from
 *
 * Three of the four blocks are **generated from the format headers** by the
 * codegen's `wem-native` backend (§15.2); `M2Material` is authored by hand
 * because an M2 per-batch material is a four-table join and there is nothing to
 * mirror. This header is the assembly point: it includes each block and declares
 * the variant, and nothing here changes when a mirror is regenerated.
 *
 * Which is which: `MdxMaterial` is generated outright; `M3Material`,
 * `M2Material` and `D3Material` are authored wrappers over generated parts,
 * because each pairs things no single parser struct pairs — a union that drops
 * HAI_ and keeps MADD twice, a four-table join, and a material whose render
 * state lives on a different asset.
 */

#include <variant>

#include <whiteout/common_types.h>
#include <whiteout/compatibility.h>

#include "../native/d3_material.h"
#include "../native/m2_material.h"
#include "../native/m3_material.h"
#include "../native/mdx_native.h"
#include "../profile.h"

namespace whiteout {
namespace models {
namespace wem {
namespace native {

// `MdxMaterial` — generated, in `native/mdx_native.h`. One block covers both WC3
// profiles: on disk the classic/Reforged distinction is per *layer*
// (`MdxLayer::isHd`), not per material, which is what makes
// `DeriveProfile(Wc3Reforged -> Wc3Classic)` a layer filter rather than a
// re-derivation through `CommonMaterial`.

// `M2Material` — authored in `native/m2_material.h`, over generated ingredient
// tables. An M2 material is a join across four combo tables and no header states
// the join, so there is nothing to mirror; WEM stores the resolved batch.

// `M3Material` — authored in `native/m3_material.h`, over generated bodies. The
// union is a decision (HAI_ is dropped; MADD is kept twice), so it is not
// generated; the eleven bodies under it are.

// `D3Material` — authored in `native/d3_material.h`, over generated pieces. The
// render state is on a *different asset* (`Shaders`), and a sub-object carries
// two whole ones, so the pairing is a decision and not a mirror.

} // namespace native

using NativeMaterial = std::variant<std::monostate, native::MdxMaterial, native::M2Material,
                                    native::M3Material, native::D3Material>;

/// Which alternative @p kind selects. `NativeKind::None` is `monostate`, so the
/// enum and the variant index are the same number by construction.
constexpr std::size_t NativeIndexFor(NativeKind kind) {
    return static_cast<std::size_t>(kind);
}

inline NativeKind NativeKindOf(const NativeMaterial& native) {
    return static_cast<NativeKind>(native.index());
}

} // namespace wem
} // namespace models
} // namespace whiteout
