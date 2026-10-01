// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file export_sections.h
 * @brief Which sections a static interchange export draws (GLTF_DESIGN §4,
 *        FBX_OBJ_DESIGN §4) — one verdict, shared by the glTF, FBX and OBJ
 *        exporters so they cannot disagree about what the model looks like.
 */

#include <string>
#include <utility>
#include <vector>

#include "whiteout/models/wem/diagnostics.h"
#include "whiteout/models/wem/document.h"

namespace whiteout {
namespace models {
namespace wem {

/// One scalar sub-track's value at @p time — hold outside the key range, the
/// left key for a step, a lerp for everything smoother. A visibility gate does
/// not need the Hermite curve between an alpha of 0 and an alpha of 1.
f32 EvalScalarTrack(const SubTrack& track, f32 time);

/// The alpha each section and material slot shows in the model's **default
/// look** — the first stand clip (else the first playable clip), sampled at
/// its start and midpoint.
///
/// MDX keys effect geosets invisible outside their own sequence — dissipate
/// orbs, hit flashes — through geoset and layer alpha, and a static export
/// cannot animate either. The export is the default look, so what that look
/// hides is skipped rather than shipped as permanently-visible white sheets
/// (Blizzard's own WC3→SC2 converter makes the same call at 192/255).
class DefaultLookAlpha {
public:
    DefaultLookAlpha(const Document& document, const Model& model, u32 modelIndex,
                     ProfileId profile, u32 look);

    f32 sectionAlpha(u32 mesh, u32 section) const;
    f32 slotAlpha(u32 slot) const;

private:
    std::vector<std::pair<u64, f32>> sections_;
    std::vector<std::pair<u64, f32>> slots_;
};

/// What an exporter knows about the material a slot exported to.
struct SlotDrawState {
    bool invisible = false;
    bool gameComposited = false;
};

enum class SectionSkip : u8 {
    None,
    NotInProfile,
    Hidden,         ///< An M3 cloth cage, an M2 disabled submesh.
    Invisible,      ///< The material draws nothing (D3's alternate bodies).
    GameComposited, ///< Coloured only by team colour/glow; no pixels to export.
    RestHidden,     ///< Alpha-keyed to nothing in the default look.
};

/// The verdict for one section of mesh @p meshOrdinal. @p slot may be null
/// (an unbound slot draws with the format's default material).
SectionSkip SkipSection(const Mesh& mesh, u32 meshOrdinal, u32 section, u32 materialSlot,
                        ProfileId profile, const SlotDrawState* slot,
                        const DefaultLookAlpha* defaultLook);

/// Per-mesh tallies of `SkipSection`, reported the way every exporter words them.
struct SectionSkipCounts {
    u32 undrawn = 0;
    u32 invisible = 0;
    u32 composited = 0;
    u32 restHidden = 0;

    void count(SectionSkip skip);
    void report(Diagnostics& diagnostics, const std::string& meshName, u32 meshOrdinal,
                ProfileId profile) const;
};

} // namespace wem
} // namespace models
} // namespace whiteout
