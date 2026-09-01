// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file channel.h
 * @brief The animatable-property table (WEM v3, design §10.8, §10.8.1).
 *
 * A channel is a *declaration*: this model can animate this property of this
 * thing, its values are of this type, and this is what it rests at. The motion
 * lives in a sub-track (`clip.h`) that names the channel **by id**.
 *
 * The indirection is M3's, and it is here for the three things only an id-keyed
 * table gives:
 *
 * - **External animation merges.** Nothing in a `.m3` names its `.m3a`; the join
 *   is the animId. A merge therefore *adds containers referencing channels that
 *   already exist* — no name matching and no target re-resolution.
 * - **Layering needs a per-channel rest value.** An opaque container with no
 *   sub-track for a channel contributes `initValue` at full weight, which is
 *   unstatable if the rest value lives on a track: it is precisely the value the
 *   absent track does not have.
 * - **Node removal stays one sweep.** `target.node` is a §10.6 referencer, so
 *   removing a node invalidates its channels and thereby every sub-track that
 *   joined on them — one indirection to fix instead of a walk over every
 *   container of every clip.
 *
 * ### The target is four shapes, not two
 *
 * Revision 9 of the design wrote `TrackTarget` with two kinds and folded the
 * channel into the material tuple. Two things the converters found:
 *
 * - **The material case is two kinds.** Splitting it is what makes `sub`
 *   unambiguous — an ordinal that §7.5's `RemoveLayer` rewrites, or an id that
 *   `RemoveFeature` invalidates and nothing ever renumbers — and the two
 *   removals need to tell those apart to do their jobs.
 * - **A section is a target.** WC3's `GeosetAnimation` keys alpha and colour on
 *   a *geoset*, not on a node and not on a material: two geosets can share a
 *   material and be hidden separately, which is how nearly every unit model
 *   sheathes a weapon. Folding it onto the material would tint the other geoset
 *   too, and dropping it would lose the most-used WC3 animation after bone TRS.
 *
 * The channel is one field on the target for all four kinds, because "which
 * property" is the same question whoever owns it.
 */

#include <string>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/compatibility.h>
#include <whiteout/models/wem/geometry/attributes.h>
#include <whiteout/models/wem/materials/features.h>
#include <whiteout/models/wem/nodes/node.h>
#include <whiteout/models/wem/profile.h>

namespace whiteout {
namespace models {
namespace wem {

// ============================================================================
// Channel
// ============================================================================

/**
 * @brief Which property, in the vocabulary all four formats share.
 *
 * Closed on purpose. A source property with no entry here is **dropped with an
 * `AnimTrackDropped` diagnostic**, not smuggled through under a free-text name:
 * a consumer that cannot enumerate what it might be handed cannot implement the
 * table, and M3 alone would contribute hundreds of AnimRefs on structures WEM
 * does not store at all (§18).
 */
enum class Channel : u8 {
    Translation, ///< F32x3. MDX KGTR, M2 `Bone::translation`, M3 BONE, D3 `TranslationCurve`.
    Rotation,    ///< Quat. Slerp by default; the source's interpolation still decides.
    Scale,       ///< F32x3, or F32 where the source ships one float (D3 `ScaleCurve`).

    Visibility, ///< F32 or Bool. MDX KATV/KLAV/KRVS, M2 attachment/light visibility,
                ///< M3's SDFG-keyed dynamic state (§10.8.2).
    Color,      ///< F32x3. A light's colour, a geoset animation's tint, a fresnel colour.
    Alpha,      ///< F32. MDX KMTA/KGAO, M2 texture weights, M3 layer alpha.

    Intensity,        ///< F32. Light.
    AttenuationStart, ///< F32. MDX KLAS, M2 `attenuationStart`.
    AttenuationEnd,   ///< F32. MDX KLAE, M2 `attenuationEnd`.

    UvTranslate, ///< F32x3 (MDX/M2 key three components even for a 2D transform).
    UvRotate,    ///< Quat on MDX/M2, F32 where the source keys an angle.
    UvScale,     ///< F32x3.

    Weight,       ///< F32. A blend factor with no better name: MDX's fresnel team-colour
                  ///< amount, M3's layer blend weights.
    TextureIndex, ///< U32. MDX KMTF's flipbook frame and KRTX's ribbon slot.
    Emissive,     ///< F32. MDX KMTE.

    Count
};

const char* ToString(Channel channel);

/// The type a channel's values take when the source does not force another —
/// what an importer starts from and a consumer can assume nothing beyond.
/// `SubTrack` reads its element size from `AnimChannel::valueType`, never from
/// here, because a source that keys a single float into `Scale` is legal.
geom::AttrType DefaultValueType(Channel channel);

// ============================================================================
// TrackTarget
// ============================================================================

/**
 * @brief Which material a channel drives, along the profile and look axes.
 *
 * A material *index* alone is ambiguous the moment a model carries several sets
 * (§6.3) and several looks (§8), so the reference is spelled the way `Resolve`
 * is: profile, slot, look. It is deliberately not the resolved index — a look
 * removal renumbers those, and a slot name does not.
 */
struct MaterialChannelRef {
    ProfileId profile = ProfileId::Generic;
    u32 slot = kInvalidIndex; ///< -> `Model::materialSlots`.
    u32 look = 0;             ///< -> the set's `LookTable`.

    template <class V>
    void reflect(V& v) {
        v.field("profile", profile);
        v.field("slot", slot);
        v.field("look", look);
    }
};

struct TrackTarget {
    enum class Kind : u8 {
        Node,            ///< `node` + `sub`.
        MaterialLayer,   ///< `material` + `sub` as a layer/stage/slot **ordinal** (§7.5),
                         ///< or `kWholeMaterial` for a track that multiplies all of
                         ///< them — a WoW `M2Color` is the case that needs it.
        MaterialFeature, ///< `material` + `sub` as a `MaterialFeature::id` (§7.2.5).
        Section,         ///< `mesh` + `sub` as the section within it (§5.5).
        Count
    };

    Kind kind = Kind::Node;
    u32 node = kInvalidNode;  ///< `Kind::Node`: -> `Model::nodes`.
    u32 mesh = kInvalidIndex; ///< `Kind::Section`: -> `Model::meshes`.
    MaterialChannelRef material;

    /// What `kind` says it is. For a node it separates same-named properties of
    /// one node — an `.m2` light keys an ambient colour beside its diffuse one —
    /// and is 0 for everything with only one.
    u32 sub = 0;

    Channel channel = Channel::Translation;

    template <class V>
    void reflect(V& v) {
        v.field("kind", kind);
        v.field("node", node);
        v.field("mesh", mesh);
        v.field("material", material);
        v.field("sub", sub);
        v.field("channel", channel);
    }
};

const char* ToString(TrackTarget::Kind kind);

/// Whether @p kind addresses a material — the test §7.5's removals make, and one
/// worth spelling once because both of them and `Validate` ask it.
constexpr bool IsMaterialTarget(TrackTarget::Kind kind) {
    return kind == TrackTarget::Kind::MaterialLayer || kind == TrackTarget::Kind::MaterialFeature;
}

// ============================================================================
// AnimChannel
// ============================================================================

/**
 * @brief One animatable property, declared once. M3's AnimRef, hoisted out of
 *        the owner structs into a per-model table.
 */
struct AnimChannel {
    /// The join key, stable for the model's life and **never reused**. M3's
    /// animId arrives here unchanged, which is what makes an `.m3a` merge a
    /// concatenation rather than a re-resolution; the other three importers
    /// allocate ids and the value is theirs to choose.
    u32 id = 0;

    TrackTarget target;
    geom::AttrType valueType = geom::AttrType::F32x3;

    /// One element of `valueType`, or empty. What an **opaque** container
    /// contributes for this channel when it holds no sub-track for it — the
    /// asymmetry M3's split-body playback is built on (§10.8.1). Empty means the
    /// source declared no rest value, not "zero".
    std::vector<u8> initValue;

    bool hasInitValue() const {
        return initValue.size() == geom::AttrTypeSize(valueType);
    }

    template <class V>
    void reflect(V& v) {
        v.field("id", id);
        v.field("target", target);
        v.field("valueType", valueType);
        v.field("initValue", initValue);
    }
};

/**
 * @brief The animatable properties of one model (§10.8).
 *
 * Empty is a normal state — a model with no animation carries no channels.
 */
struct AnimChannelTable {
    std::vector<AnimChannel> channels;

    bool empty() const {
        return channels.empty();
    }

    /// The channel with id @p id, or null. Linear: tables run to hundreds of
    /// entries on the heaviest `.m3` and importers build them in one pass, so an
    /// index would be a map to maintain for a search nobody runs in a loop.
    const AnimChannel* find(u32 id) const;
    AnimChannel* find(u32 id);

    /// The position of id @p id in `channels`, or `kInvalidIndex`.
    u32 indexOf(u32 id) const;

    /// Appends @p channel and returns its id. An id already in the table is a
    /// caller error — `Validate` reports the duplicate — so this does not check.
    u32 add(const AnimChannel& channel);

    /// An id no channel uses. Importers that do not inherit ids call this.
    u32 nextFreeId() const;

    template <class V>
    void reflect(V& v) {
        v.field("channels", channels);
    }
};

} // namespace wem
} // namespace models
} // namespace whiteout
