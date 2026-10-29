// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file soft.h
 * @brief Soft bodies: a region of a model, the lumps that move it and the
 *        effects that say how (EDIT_MODE_SOFTBODY_DESIGN.md §13).
 *
 * **Authoring state**, as `ClothRecipe` is: no game runs a soft body. A bake
 * writes each lump's motion as tracks, and an export makes bones of the lumps
 * (`ExpandSoftToBones`); nothing else reads these records.
 *
 * **A lump** is a control frame that hangs from a bone, its carrier: the bone
 * itself, a copy of it, or a frame the editor spread over the surface. A bone
 * and the lumps that hang from it are a family, and a point's weight on the
 * bone is shared out inside the family alone, so with every lump at rest the
 * model is its own skin, exactly (§6.3).
 *
 * **No stiffness, mass or weight on a lump**, on purpose (the design's
 * Provenance, rule 2). `swings` says which lumps Jiggle moves at all.
 *
 * **Lengths.** A setting the design gives as a percentage is a share: of the
 * region's size, of its volume, of a radius. The few model-space lengths here
 * (`anchor`, `rest`, `depth`, `clear`, a place's `radius`, a melt's `floor`,
 * a travel's `line`, a Drip's `strandAt` and `strandWidth`) are what
 * `RescaleDocument` scales.
 */

#include <optional>
#include <string>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/vector_types.h>

#include "../profile.h"

namespace whiteout {
namespace models {
namespace wem {

/// Appended, never reordered: the value is what a `SEFX` chunk stores.
enum class SoftEffectKind : u8 { Inflate, Bulge, Jiggle, Wobble, Blobify, Drip, SquashStretch, Melt, Ripple, Count };

const char* ToString(SoftEffectKind kind);

/// A bone used as it is, a copy that hangs from a bone, or a frame on the
/// surface (§6.2).
enum class SoftLumpKind : u8 { Bone, Copy, Surface, Count };

const char* ToString(SoftLumpKind kind);

/// The direction an effect moves a point in (§8.1): along the surface's
/// normal, or away from the bones, which is every lump scaling about its pivot.
/// (Under a Ripple a lump of the surface moves out with the wave instead, §8.9.)
enum class SoftAlong : u8 { Surface, Bones, Count };

const char* ToString(SoftAlong along);

/// An axis an effect works along. `Auto` is the region's longest; `Motion`
/// follows the carrier's travel (Squash & stretch); `Bone` names one.
enum class SoftAxis : u8 { Auto, Up, X, Y, Z, Bone, Motion, Count };

const char* ToString(SoftAxis axis);

/// What fires an effect that acts at moments (§8.2, §8.9): its spans, the
/// starts of its spans as marks, the clip's events of a name, or colliders.
enum class SoftComesFrom : u8 { Spans, Times, Events, Contact, Count };

const char* ToString(SoftComesFrom from);

/// A mesh, or the part of one that draws with a material.
struct SoftPart {
    u32 mesh = kInvalidIndex;
    u32 material = kInvalidIndex; ///< A material slot; none is the whole mesh.

    template <class V>
    void reflect(V& v) {
        v.field("mesh", mesh);
        v.field("material", material);
    }
};

/// A place on the region's surface, which rides it: a bulge's middle, a
/// ripple's source.
struct SoftPlace {
    u32 mesh = kInvalidIndex;
    u32 face = kInvalidIndex;
    Vector3f weights{1, 0, 0}; ///< Barycentric, in the face's first triangle.
    f32 radius = 0.0f;         ///< Model units; 0 is a point.

    template <class V>
    void reflect(V& v) {
        v.field("mesh", mesh);
        v.field("face", face);
        v.field("weights", weights);
        v.field("radius", radius);
    }
};

struct SoftLump {
    SoftLumpKind kind = SoftLumpKind::Surface;
    /// On the surface, or its bone's pivot, at the bind: what lanes are solved from.
    Vector3f anchor{0, 0, 0};
    Vector3f rest{0, 0, 0}; ///< Its pivot, model space, at the bind (§6.1).
    f32 depth = 0.0f;       ///< Half the body's thickness under it: Blobify's ball.
    /// The space between its points and the things inside, at the bind (§5.4);
    /// 0 is none to keep.
    f32 clear = 0.0f;
    u32 carrier = kInvalidNode; ///< The bone it hangs from (§6.3). `Bone`: its parent, or none.
    u32 node = kInvalidNode;    ///< `Bone`: the bone that is the lump. `Copy`: the bone it copies.
    u32 effect = 0;             ///< The effect whose place claimed it; 0 is none.
    u16 rider = 0;              ///< The part it carries rigidly: its place in the body's `riders` + 1 (§5.4).
    bool swings = true;         ///< Jiggle moves it (§8.3). A yes or no, never a stiffness.
    bool rigid = false;         ///< A rigid part's lump: it never scales (§5.4).

    /// The bone whose family the lump is in: the bone it is, or the one it
    /// hangs from.
    u32 family() const {
        return kind == SoftLumpKind::Surface ? carrier : node;
    }

    template <class V>
    void reflect(V& v) {
        v.field("kind", kind);
        v.field("anchor", anchor);
        v.field("rest", rest);
        v.field("depth", depth);
        v.field("clear", clear);
        v.field("carrier", carrier);
        v.field("node", node);
        v.field("effect", effect);
        v.field("rider", rider);
        v.field("swings", swings);
        v.field("rigid", rigid);
    }
};

// ============================================================================
// The effects' settings (§8). An effect's Amount, seed and place are on the
// effect; these are what only one kind means.
// ============================================================================

/// A rhythm on Inflate's Amount.
enum class SoftPulse : u8 { None, Breathe, Heartbeat, Rate, Count };

const char* ToString(SoftPulse pulse);

/// §8.1. The Amount is a share of the volume: 0.3 swells by 30 %.
struct SoftInflate {
    SoftAlong along = SoftAlong::Surface;
    bool towardPlace = false; ///< Swells most at the effect's place, fading over its radius.
    f32 round = 0.3f;         ///< 0 keeps the creases; 1 swells toward the body's blob. Along the surface only.
    bool keepVolume = false;
    SoftPulse pulse = SoftPulse::None;
    f32 rate = 0.25f; ///< Beats a second.
    f32 depth = 0.5f; ///< How far the pulse falls back between beats.
    u8 smooth = 3;    ///< Passes that even the normals. Along the surface only.

    template <class V>
    void reflect(V& v) {
        v.field("along", along);
        v.field("towardPlace", towardPlace);
        v.field("round", round);
        v.field("keepVolume", keepVolume);
        v.field("pulse", pulse);
        v.field("rate", rate);
        v.field("depth", depth);
        v.field("smooth", smooth);
    }
};

enum class SoftBulgeShape : u8 { Round, Ring, Count };

const char* ToString(SoftBulgeShape shape);

/// §8.2. The Amount is the height, a share of the place's radius; negative is
/// a dent.
struct SoftBulge {
    SoftAlong along = SoftAlong::Surface;
    SoftBulgeShape shape = SoftBulgeShape::Round;
    bool keepVolume = true;
    SoftComesFrom from = SoftComesFrom::Spans;
    std::string event; ///< `Events`: the clip events' name.
    f32 rise = 0.03f;  ///< An impact's rise and fall, seconds.
    f32 fall = 0.25f;
    bool travels = false;
    std::vector<u32> chain;     ///< `travels`: along these bones, in order; node indices.
    std::vector<Vector3f> line; ///< ... or along this line over the surface, model space at the bind.

    template <class V>
    void reflect(V& v) {
        v.field("along", along);
        v.field("shape", shape);
        v.field("keepVolume", keepVolume);
        v.field("from", from);
        v.field("event", event);
        v.field("rise", rise);
        v.field("fall", fall);
        v.field("travels", travels);
        v.field("chain", chain);
        v.field("line", line);
    }
};

enum class SoftJigglePreset : u8 { Custom, Jelly, Fat, Muscle, Rubber, WaterBalloon, Water, Slime, Count };

const char* ToString(SoftJigglePreset preset);

/// §8.3. One model for the body: none of these is set per lump or per bone.
struct SoftJiggle {
    SoftJigglePreset preset = SoftJigglePreset::Fat;
    f32 looseness = 2.2f;  ///< The slowest swing's rate, swings a second.
    f32 bounce = 0.35f;    ///< 0 is no overshoot.
    f32 weight = 0.5f;     ///< Gravity's sag.
    f32 holdShape = 0.6f;  ///< How alike neighbouring points move.
    f32 keepVolume = 0.8f; ///< How stiff the one direction that changes the volume is.
    f32 limit = 0.25f;     ///< The farthest a lump may leave its goal, of the region's size.
    bool keepInPlace = false; ///< Takes out the swing in which the whole body lags as one.
    bool meetsFloor = true;
    bool worldForces = true; ///< The clip's winds and blasts push it.
    // Kicks, as parallel arrays: an instant change of speed at a place. Each
    // has an id in its body's effect ids, and its marks are that id's
    // `SoftAmount` keys.
    std::vector<u32> kickIds;
    std::vector<u32> kickMesh;
    std::vector<u32> kickFace;
    std::vector<Vector3f> kickWeights;
    std::vector<Vector3f> kickDirection; ///< Model space, unit; none is into the body at its place.
    std::vector<f32> kickStrength;       ///< In g.

    template <class V>
    void reflect(V& v) {
        v.field("preset", preset);
        v.field("looseness", looseness);
        v.field("bounce", bounce);
        v.field("weight", weight);
        v.field("holdShape", holdShape);
        v.field("keepVolume", keepVolume);
        v.field("limit", limit);
        v.field("keepInPlace", keepInPlace);
        v.field("meetsFloor", meetsFloor);
        v.field("worldForces", worldForces);
        v.field("kickIds", kickIds);
        v.field("kickMesh", kickMesh);
        v.field("kickFace", kickFace);
        v.field("kickWeights", kickWeights);
        v.field("kickDirection", kickDirection);
        v.field("kickStrength", kickStrength);
    }
};

/// @p preset's settings (§8.3's table); `Custom` gives the defaults.
SoftJiggle SoftJigglePresetOf(SoftJigglePreset preset);

enum class SoftWobblePattern : u8 { Jelly, Sway, Roll, Random, Count };

const char* ToString(SoftWobblePattern pattern);

/// §8.4. The Amount is how far, a share of the region's size.
struct SoftWobble {
    SoftWobblePattern pattern = SoftWobblePattern::Jelly;
    f32 speed = 1.0f;   ///< Swings a second.
    f32 size = 1.0f;    ///< Roll's wavelength, in regions.
    f32 overlap = 0.0f; ///< The far end trails the near end.
    SoftAxis axis = SoftAxis::Auto;
    u32 axisBone = kInvalidNode;
    bool keepVolume = true;

    template <class V>
    void reflect(V& v) {
        v.field("pattern", pattern);
        v.field("speed", speed);
        v.field("size", size);
        v.field("overlap", overlap);
        v.field("axis", axis);
        v.field("axisBone", axisBone);
        v.field("keepVolume", keepVolume);
    }
};

/// §8.5. The Amount runs from the model (0) to its blob (1).
struct SoftBlobify {
    f32 roundness = 0.4f; ///< How far the balls reach.
    f32 swell = 0.05f;    ///< A little more volume.
    bool keepVolume = true;

    template <class V>
    void reflect(V& v) {
        v.field("roundness", roundness);
        v.field("swell", swell);
        v.field("keepVolume", keepVolume);
    }
};

/// Appended, never reordered: the value is what a record stores.
enum class SoftDripPreset : u8 { Custom, Wax, Slime, Blood, Paint, Chocolate, Honey, Water, Count };

const char* ToString(SoftDripPreset preset);

/// How a strand's neck thins over time (§8.6): in a straight line, slowly and
/// then at once, without end, or until it sets.
enum class SoftNeckLaw : u8 { Linear, Snap, Thread, Sets, Count };

const char* ToString(SoftNeckLaw law);

/// §8.6.
struct SoftDrip {
    SoftDripPreset preset = SoftDripPreset::Slime;
    SoftNeckLaw neck = SoftNeckLaw::Thread;
    bool sitesByHand = false;
    u8 sites = 6;             ///< Auto: how many.
    f32 overhang = 1.0471976f; ///< Radians from straight down past which a face sheds: 60 degrees.
    f32 spacing = 0.10f;      ///< Between sites, of the region's size.
    bool toGround = true;
    f32 length = 0.5f;    ///< When not to the ground: of the region's size.
    f32 speed = 0.4f;     ///< Of the height a second.
    f32 thickness = 0.03f; ///< Of the region's size.
    f32 neckWidth = 0.4f; ///< Of the thickness.
    f32 tip = 1.4f;       ///< Of the thickness.
    f32 stagger = 0.6f;
    f32 sway = 0.3f;       ///< How far a strand swings as it hangs: 0 is still, 1 twenty degrees.
    f32 pull = 0.3f;       ///< *Pull the surface*: how far the skin under a strand comes down with it.
    f32 pinchEvery = 0.0f; ///< Seconds between droplets; 0 is never.
    u32 material = kInvalidIndex; ///< A material slot; none is the surface's.
    // Sites placed by hand, as parallel arrays.
    std::vector<u32> siteMesh;
    std::vector<u32> siteFace;
    std::vector<Vector3f> siteWeights;
    std::vector<f32> siteThickness; ///< Of the region's size; 0 is the setting's.
    // The strands as built, as parallel arrays: three bones each (base, neck,
    // tip), where each hangs from at the bind, and its radius in model units.
    std::vector<u32> strandNodes;
    std::vector<Vector3f> strandAt;
    std::vector<f32> strandWidth;
    /// The droplets as built, where the effect pinches off: one bone a strand,
    /// in the strands' order. Empty where it does not.
    std::vector<u32> dropNodes;

    /// Whether its strands let droplets go: *Pinch off* is on, and the
    /// material is not one that sets (§8.6).
    bool pinches() const {
        return pinchEvery > 0.0f && neck != SoftNeckLaw::Sets;
    }

    template <class V>
    void reflect(V& v) {
        v.field("preset", preset);
        v.field("neck", neck);
        v.field("sitesByHand", sitesByHand);
        v.field("sites", sites);
        v.field("overhang", overhang);
        v.field("spacing", spacing);
        v.field("toGround", toGround);
        v.field("length", length);
        v.field("speed", speed);
        v.field("thickness", thickness);
        v.field("neckWidth", neckWidth);
        v.field("tip", tip);
        v.field("stagger", stagger);
        v.field("sway", sway);
        v.field("pull", pull);
        v.field("pinchEvery", pinchEvery);
        v.field("material", material);
        v.field("siteMesh", siteMesh);
        v.field("siteFace", siteFace);
        v.field("siteWeights", siteWeights);
        v.field("siteThickness", siteThickness);
        v.field("strandNodes", strandNodes);
        v.field("strandAt", strandAt);
        v.field("strandWidth", strandWidth);
        v.field("dropNodes", dropNodes);
    }
};

enum class SoftSquashMode : u8 { Auto, Keyed, Count };
enum class SoftAnchor : u8 { Bottom, Centre, Pivot, Count };

const char* ToString(SoftSquashMode mode);
const char* ToString(SoftAnchor anchor);

/// §8.7. Keyed, the Amount runs from a squash (-1) to a stretch (+1).
struct SoftSquash {
    SoftSquashMode mode = SoftSquashMode::Auto;
    SoftAxis axis = SoftAxis::Motion;
    u32 axisBone = kInvalidNode;
    f32 stretch = 0.3f; ///< At speed.
    f32 squash = 0.3f;  ///< On a stop, a landing or a turn back.
    f32 turning = 0.5f; ///< A swung part stretches away from the axis it turns about.
    f32 drag = 0.0f;    ///< The far parts trail the motion.
    f32 windUp = 0.0f;  ///< The body leans back before a move and carries past its end.
    SoftAnchor anchor = SoftAnchor::Bottom;
    f32 smoothing = 0.08f; ///< Seconds the speed is read over.

    template <class V>
    void reflect(V& v) {
        v.field("mode", mode);
        v.field("axis", axis);
        v.field("axisBone", axisBone);
        v.field("stretch", stretch);
        v.field("squash", squash);
        v.field("turning", turning);
        v.field("drag", drag);
        v.field("windUp", windUp);
        v.field("anchor", anchor);
        v.field("smoothing", smoothing);
    }
};

enum class SoftMeltOrder : u8 { Top, Bottom, AllAtOnce, Count };

const char* ToString(SoftMeltOrder order);

/// §8.8. Its span is a progress: 0 at the start, 1 at the end.
struct SoftMelt {
    SoftMeltOrder order = SoftMeltOrder::Top;
    f32 solidity = 0.5f; ///< Jelly 0, wax 0.5, ice 1: how long the middle stands.
    bool onGrid = true;  ///< The floor is the grid; else `floor`.
    f32 floor = 0.0f;    ///< A height, model units.
    f32 puddle = 2.5f;   ///< The widest it spreads, times the footprint.
    f32 uneven = 0.3f;
    bool backward = false;    ///< The body rises out of its puddle.
    bool staysInDecay = true; ///< The clips named Decay hold the end.

    template <class V>
    void reflect(V& v) {
        v.field("order", order);
        v.field("solidity", solidity);
        v.field("onGrid", onGrid);
        v.field("floor", floor);
        v.field("puddle", puddle);
        v.field("uneven", uneven);
        v.field("backward", backward);
        v.field("staysInDecay", staysInDecay);
    }
};

enum class SoftRipplePlays : u8 { Once, Loops, Count };
enum class SoftThrough : u8 { Skin, Body, Count };

const char* ToString(SoftRipplePlays plays);
const char* ToString(SoftThrough through);

/// §8.9. The Amount is the height, a share of the region's size.
struct SoftRipple {
    SoftAlong along = SoftAlong::Surface;
    SoftRipplePlays plays = SoftRipplePlays::Once;
    SoftComesFrom from = SoftComesFrom::Spans; ///< `Once`: spans, times or events.
    std::string event;
    f32 wavelength = 1.0f; ///< Of the region's size.
    f32 speed = 1.5f;      ///< Regions a second.
    SoftThrough through = SoftThrough::Skin;
    bool water = false;  ///< The envelope moves at its own speed.
    f32 envelope = 0.5f; ///< ... this many times the crests'.
    f32 fadeDistance = 0.5f;
    f32 fadeTime = 0.6f; ///< Seconds.

    template <class V>
    void reflect(V& v) {
        v.field("along", along);
        v.field("plays", plays);
        v.field("from", from);
        v.field("event", event);
        v.field("wavelength", wavelength);
        v.field("speed", speed);
        v.field("through", through);
        v.field("water", water);
        v.field("envelope", envelope);
        v.field("fadeDistance", fadeDistance);
        v.field("fadeTime", fadeTime);
    }
};

/// One effect of a soft body. Exactly the settings its kind names are set.
struct SoftEffect {
    u32 id = 0; ///< Stable in its body, from 1: its `SoftAmount` channel's sub.
    SoftEffectKind kind = SoftEffectKind::Jiggle;
    std::string name;
    bool enabled = true;
    f32 amount = 1.0f;        ///< What a span at full is.
    bool everywhere = false;  ///< `SoftAmount`'s rest: on where nothing keys it.
    bool drivesJiggle = true; ///< §7. Off by default for Wobble and Ripple.
    u32 seed = 1;
    SoftPlace place;
    std::optional<SoftInflate> inflate;
    std::optional<SoftBulge> bulge;
    std::optional<SoftJiggle> jiggle;
    std::optional<SoftWobble> wobble;
    std::optional<SoftBlobify> blobify;
    std::optional<SoftDrip> drip;
    std::optional<SoftSquash> squash;
    std::optional<SoftMelt> melt;
    std::optional<SoftRipple> ripple;

    /// Whether the settings its kind names are the ones set, and no other.
    bool settingsMatchKind() const;

    template <class V>
    void reflect(V& v) {
        v.field("id", id);
        v.field("kind", kind);
        v.field("name", name);
        v.field("enabled", enabled);
        v.field("amount", amount);
        v.field("everywhere", everywhere);
        v.field("drivesJiggle", drivesJiggle);
        v.field("seed", seed);
        v.field("place", place);
        v.optional("inflate", inflate);
        v.optional("bulge", bulge);
        v.optional("jiggle", jiggle);
        v.optional("wobble", wobble);
        v.optional("blobify", blobify);
        v.optional("drip", drip);
        v.optional("squash", squash);
        v.optional("melt", melt);
        v.optional("ripple", ripple);
    }
};

/// An effect of @p kind as the editor adds it (§8): its Amount, its settings
/// and *Drives Jiggle* where they show at once on a model that plays. A record
/// read from a file holds its own.
SoftEffect DefaultSoftEffect(SoftEffectKind kind);

struct SoftBody {
    u32 id = 0;              ///< From `PhysicsSet::allocateId`: a channel names it.
    std::string name;        ///< Also the prefix of the bones an export makes.
    std::vector<u32> meshes; ///< The meshes that hold its points.
    u32 detail = 0;          ///< Surface lumps asked for, apart from those places claim (§6.2).
    bool copyAll = false;         ///< Copy every bone, shared or not (§6.2).
    bool chainsAsTheyAre = false; ///< Bones with bones below them are used, and only turn (§6.2).
    std::vector<SoftPart> inside; ///< What the body keeps clear of (§5.4).
    /// Parts in the region that ride rigidly (§5.4): each has a lump for every
    /// bone it rides, and its points are those lumps' whole.
    std::vector<SoftPart> riders;
    f32 blend = 0.15f;            ///< Of the region's size (§5.2).
    u8 influences = 4;            ///< The weights a point the lanes were solved for (§6.4).
    bool equalShares = false;     ///< Solved for classic groups.
    std::vector<SoftLump> lumps;
    std::vector<SoftEffect> effects; ///< In the order they run.
    std::vector<u32> colliders;      ///< `ClothCollider::id`s.
    u32 drips = kInvalidIndex;       ///< The mesh Drip added, or none.
    u32 nextEffect = 1;              ///< The next effect id: never reused.

    const SoftEffect* effect(u32 id) const;
    SoftEffect* effect(u32 id);

    template <class V>
    void reflect(V& v) {
        v.field("id", id);
        v.field("name", name);
        v.field("meshes", meshes);
        v.field("detail", detail);
        v.field("copyAll", copyAll);
        v.field("chainsAsTheyAre", chainsAsTheyAre);
        v.field("inside", inside);
        v.field("riders", riders);
        v.field("blend", blend);
        v.field("influences", influences);
        v.field("equalShares", equalShares);
        v.field("lumps", lumps);
        v.field("effects", effects);
        v.field("colliders", colliders);
        v.field("drips", drips);
        v.field("nextEffect", nextEffect);
    }
};

} // namespace wem
} // namespace models
} // namespace whiteout
