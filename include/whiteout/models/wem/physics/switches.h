// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file switches.h
 * @brief Which bodies the simulation owns at a moment of a clip, by one rule
 *        (EDIT_MODE_PHYSICS_BAKE_DESIGN.md §4.5), and the clip facts a bake
 *        reads around it: whether a clip loops, and what plays before and
 *        after it (§7).
 *
 * A rig's *Starts* says whether it runs in a clip; a body's State applies
 * while it runs. On top of that sit StarCraft II's switches: a body's
 * `PhysicsDynamic` keys, its *Inherit*, and a rig's `PhysicsRagdoll` ("ragdoll
 * now"), each keyed per clip. A document with no keys plays as it always did.
 *
 * The driver, the timeline, the `.m3` export and Problems all read the rule
 * here, so none of them can disagree about a body.
 */

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <whiteout/common_types.h>

#include "../document.h"

namespace whiteout {
namespace models {
namespace wem {

/// Why a body is in the state it is: what the timeline and the Edit tab say.
enum class SwitchWhy : u8 {
    Still,     ///< Static, held by no running rig, or its clip is baked.
    Rest,      ///< Its State, while its ragdoll runs; no key.
    Keyed,     ///< This clip's own switch key.
    Dropped,   ///< Its ragdoll's *Ragdoll now* key.
    Death,     ///< An *On death* ragdoll, in a clip named Death.
    Carried,   ///< A drop carried from the clip this one comes from.
    Inherited, ///< Its nearest bodied ancestor's state.
    Exempt,    ///< Its ragdoll is dropped, and it ignores the drop.
};

/// One body's state at one step.
struct BodySwitch {
    /// The simulation owns the body: it moves by physics, or is being handed
    /// back to its animation.
    bool active = false;
    /// 1 while it simulates freely; below 1 it is handed back (§4.4).
    f32 blend = 1.0f;
    SwitchWhy why = SwitchWhy::Still;

    bool dynamic() const {
        return active && blend >= 1.0f;
    }
    bool returning() const {
        return active && blend < 1.0f;
    }
};

/// Who asks (§4.5).
enum class SwitchScope : u8 {
    /// Animations, Game preview and the exports: a baked clip is its keys.
    Clip,
    /// Simulate and the baker: a baked clip runs from its source.
    Simulate,
    /// The Physics workspace's Test: every rig but a *Never* one runs, and so
    /// do the bodies no rig holds. A baked clip is its keys.
    Test,
};

/// Whether a rig of @p start runs in every clip, its bodies' own states
/// applying (§4.1).
constexpr bool RigRuns(RigStart start) {
    return start == RigStart::Always || start == RigStart::Animated;
}

/// What a physics switch plays where its clip does not key it (§9.1): a
/// body's `PhysicsDynamic` 1 when a rig holding it runs and it is Dynamic and
/// simulates, a cloth's `ClothActive` its `active`, a rig's `PhysicsRagdoll`
/// 0 and any `PhysicsBlend` 1. Two defaults depend on the clip and are the
/// rule's, not this: an *On death* drop, and a drop carried into the clip.
f32 PhysicsSwitchRest(const Model& model, const TrackTarget& target);

/// @p record's @p channel on @p model, or `kInvalidIndex` when it has none.
u32 FindPhysicsChannel(const Model& model, u32 record, Channel channel);

/**
 * @brief One clip's switches, read at any moment of it (§4.5).
 *
 * What a clip keys and defaults to is found once; `read` then answers per
 * step. A cursor over the document: it must not outlive it or an edit.
 */
class SwitchReader {
public:
    /// @p clip is a document clip index, or `kInvalidIndex` for no clip: the
    /// model at rest, which keys nothing. @p chained: the clip runs in a
    /// set-up clip's lead-in, so it carries a drop even with no settings of its
    /// own (§4.3).
    SwitchReader(const Document& document, u32 model, u32 clip, SwitchScope scope, bool chained = false);

    /// Every body's state at @p seconds of the clip, in body order. An
    /// *Inherit* body whose ancestor comes after it in that order reads the
    /// ancestor's state in @p previous, the last step's, as StarCraft II does;
    /// with none, the ancestor reads inactive.
    void read(f32 seconds, std::span<const BodySwitch> previous, std::vector<BodySwitch>& out) const;

    /// Whether @p rig (an index into the model's rigs) is dropped at
    /// @p seconds, and why: its key, the *On death* default or a carried drop.
    bool dropped(u32 rig, f32 seconds, SwitchWhy* why = nullptr) const;

    /// @p rig's `PhysicsBlend` at @p seconds; 1 where the clip does not key it.
    f32 rigBlend(u32 rig, f32 seconds) const;

    /// Whether the clip keys @p body's own switch (an index into the bodies).
    bool keysBody(u32 body) const;
    /// Whether the clip keys @p rig's *Ragdoll now*.
    bool keysRig(u32 rig) const;

    /// Whether @p body could move in some clip: a member of a rig that is not
    /// *Never*, or a body no rig holds that some clip keys (§6).
    bool movable(u32 body) const;

private:
    struct Body {
        bool still = false;   ///< Static, or held only by *Never* rigs.
        bool rest = false;    ///< Its own state where nothing keys it.
        bool exempt = false;
        bool inherit = false;
        u32 ancestor = kInvalidIndex; ///< The nearest bodied ancestor, by index.
        std::vector<u32> rigs;         ///< The rigs holding it, in rig order.
        const SubTrack* dynamic = nullptr;
        const SubTrack* blend = nullptr;
        bool movable = false;
    };
    struct Rig {
        bool never = false;
        bool fallback = false; ///< Dropped where the clip does not key it.
        SwitchWhy fallbackWhy = SwitchWhy::Rest;
        const SubTrack* ragdoll = nullptr;
        const SubTrack* blend = nullptr;
    };

    f32 sample(const SubTrack* track, f32 seconds, f32 fallback, bool held) const;

    const Document* document_ = nullptr;
    const Clip* clip_ = nullptr;
    bool baked_ = false;
    std::vector<Body> bodies_;
    std::vector<Rig> rigs_;
};

/// Every body's state at @p seconds of @p clip, as a `SwitchReader` answers:
/// for a caller that asks once.
void PhysicsSwitches(const Document& document, u32 model, u32 clip, f32 seconds, SwitchScope scope,
                     std::span<const BodySwitch> previous, std::vector<BodySwitch>& out);

// ----------------------------------------------------------------------------
// The clips around a bake (§7)
// ----------------------------------------------------------------------------

/// A sequence's name without its comment (after a '-') or its trailing variant
/// number: "Stand - 2" and "Stand 2" are "Stand".
std::string BaseSequenceName(std::string_view name);

/// "Decay", or "Decay" and then a space: a corpse's clip, which continues a
/// death (§4.3).
bool IsDecayClipName(std::string_view name);

/// Whether the games play a clip of @p name once, whatever its loop flag says
/// (§7.1): Attack, Spell but Spell Channel, Death, Decay, Birth, Dissipate,
/// Morph and Stand Hit, by the base name's first words. CamelCase splits into
/// words, so World of Warcraft's `AttackUnarmed` is an Attack.
bool PlaysOnce(std::string_view name);

/// Whether @p clip's bake runs it as a loop: as set, or from its flag and its
/// name.
bool BakeLoops(const Clip& clip);

/// What runs before a clip's frame 0, resolved: the clip itself (a loop),
/// its first frame held, or another clip.
struct BakeLeadIn {
    BakeFrom kind = BakeFrom::FirstFrame; ///< `Itself`, `FirstFrame` or `Clip`.
    u32 clip = kInvalidIndex;             ///< `Clip`: the document clip.
};

/// @p clip's *Comes from* (§7.2). A loop comes from itself; a once-played
/// clip from Stand (the model's first looping clip whose base name is Stand),
/// else its first frame. Decay Flesh, or a lone Decay, comes from the first
/// death clip, and Decay Bone from Decay Flesh. A name the model lacks is its
/// first frame.
BakeLeadIn BakeComesFrom(const Document& document, u32 clip);

/// @p clip's *Goes to* (§7.4): for a once-played clip, Stand after an Attack,
/// a Spell, a Birth, a Morph or a Stand Hit, and nothing after a Death, a
/// Decay or a Dissipate. `kInvalidIndex` for nothing.
u32 BakeGoesTo(const Document& document, u32 clip);

/// The clips @p clip's lead-in runs, the earliest first: Decay Bone's is
/// Stand, Death, Decay Flesh. Empty for a loop or a first frame held. A chain
/// that comes back on itself stops before the repeat, and @p cycle says so.
std::vector<u32> BakeChain(const Document& document, u32 clip, bool* cycle = nullptr);

/// The document clip of @p model named @p name, or `kInvalidIndex`.
u32 FindClipNamed(const Document& document, u32 model, std::string_view name);

} // namespace wem
} // namespace models
} // namespace whiteout
