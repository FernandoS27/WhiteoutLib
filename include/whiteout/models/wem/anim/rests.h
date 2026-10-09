// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file rests.h
 * @brief What a channel plays where nothing keys it: the rest the main
 *        profile's format writes (WEM_ANIMATION_RUNTIME_DESIGN.md §7.1).
 *
 * A storage policy, not part of a clip's read rule: it follows the game of the
 * format the document is written to. Warcraft III's two rests are the
 * editor's sampler's, moved here so an opaque layer of the `Animator` fills
 * what the file will fill; StarCraft II's is the AnimRef's `initValue`.
 */

#include <vector>

#include <whiteout/common_types.h>

#include "../document.h"
#include "clip.h"

namespace whiteout {
namespace models {
namespace wem {

/// A track's two rests (CURVE §7.1, §7.2): what an animation that does not key
/// it plays while no clip of the model keys it — the record's static field,
/// read where `toMdx` reads it — and while another clip does, which is not the
/// static: Warcraft III writes its caller's default then (`InterpolateRetained`),
/// 1 for a visibility, an alpha or an emissive gain, white for a fresnel
/// colour, and 0 for a light's, an emitter's and a ribbon's values, a fresnel's
/// opacity and team colour and a geoset's colour — but 1 for a PopcornFX
/// emitter's multipliers, whose alpha is 1 even unkeyed. A transform rests at
/// the identity offset either way, and a flipbook at its layer's texture. A
/// texture animation rests at its layer's fixed UV transform either way
/// (`TextureInput::uvTransform` as MDX's three values, `mdx_uv::FixedValues`):
/// the identity on a material with an MDX block, and elsewhere a value `toMdx`
/// keys wherever the document does not. One element of @p type each.
///
/// StarCraft II has one rest: an STC that does not key a channel falls back to
/// the AnimRef's `initValue`, which `toM3` writes from the bone's `local` (on
/// an explicit-bind rig; a pivot rig keys offsets, which rest at identity),
/// from `VisibilityRest`, and from the channel's own `initValue` or else the
/// static field otherwise.
struct TrackRests {
    std::vector<u8> unkeyed;
    std::vector<u8> keyedElsewhere;
    /// Whether the two differ: the tracks an edit in one animation could change
    /// another's picture through (§7.2).
    bool differ() const {
        return unkeyed != keyedElsewhere;
    }
};

/// @p target's rests under @p game's storage.
TrackRests RestsOf(const Document& document, u32 model, const TrackTarget& target,
                   geom::AttrType type, Game game);
/// The same under the main profile's game.
TrackRests RestsOf(const Document& document, u32 model, const TrackTarget& target,
                   geom::AttrType type);
/// @p channel's rests under @p game's storage: StarCraft II's reads its
/// `initValue` where it has one.
TrackRests RestsOf(const Document& document, u32 model, const AnimChannel& channel, Game game);

/// The rests an opaque layer of the `Animator` fills @p channel with under
/// @p storage: `RestsOf`, except that a bone of an explicit-bind rig rests at
/// its own local transform either way, whichever game stores it.
TrackRests RestsPlayed(const Document& document, u32 model, const AnimChannel& channel, Game storage);

/// The first of @p model's clips, in document order, that keys @p channelId.
/// MDX's export takes a merged track's clock from it (§1.4), which is what
/// makes a channel a global loop's or the sequences'.
const Clip* ClockOwner(const Document& document, u32 model, u32 channelId);

/// Whether any clip of @p model keys @p channelId — what makes the exported
/// track used, and a clip that does not key it read the keyed-elsewhere rest.
bool KeyedAnywhere(const Document& document, u32 model, u32 channelId);

/// What @p channel plays in an animation that does not key it: its
/// keyed-elsewhere rest once any clip keys it, its static one otherwise.
std::vector<u8> RestValue(const Document& document, u32 model, const AnimChannel& channel);

/// The rest `toM3` writes for @p node's visibility, which decides whether a
/// gated batch draws in a model nothing keys: the channel's declared rest (the
/// M3 import parks the file's AnimRef default there, visibility having no node
/// field), or visible when the document says nothing — a zero would read as
/// invisible, and Alexstrasza, whose batches all gate on one bone and whose
/// `.m3` has no sequence, would draw nothing.
f32 VisibilityRest(const Model& model, u32 node);

/// A section's static alpha as `toMdx` writes it: 0 when hidden, else its
/// partial alpha.
f32 SectionStaticAlpha(const MeshSection& section);

/// Whether Warcraft III steps @p channel whatever its controller says: a PRE2's
/// emission rate with its squirt flag set (`forceNoInterp`, CURVE §4.10).
bool HeldByRenderer(const Document& document, u32 model, const AnimChannel& channel);

} // namespace wem
} // namespace models
} // namespace whiteout
