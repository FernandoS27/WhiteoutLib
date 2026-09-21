// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file emitters.h
 * @brief The emitter-system node payloads (WEM v3, design §10.9).
 *
 * Warcraft III and StarCraft II each run their own particle and ribbon systems,
 * and the five share too little for one generic record to be anything but lossy:
 * a `PREM` spawns copies of a model, a `PRE2` is a flipbook quad with a head and
 * a tail, a `PAR_` has eleven instance types, a spline, forces, noise and
 * collision. So each is a node kind of its own whose payload is that system
 * whole, and a kind is carried only by the profiles of the game that runs it
 * (`ProfileDesc::nodeKinds`). The generic `ParticlePayload`/`RibbonPayload` stay
 * for the formats WEM still only names a system for (§18).
 *
 * Reforged's `CORN` is the one in between: its effect is a PopcornFX file WEM
 * does not hold, but the record around it — the multipliers, their tracks, the
 * flags — is the model's, so it is a kind of its own too (revision 12).
 *
 * The node rules hold unchanged:
 *
 * - **The placement is the node.** An MDX emitter is a node chunk already; an
 *   M3 emitter is a child of the bone its record names, at identity.
 * - **The payload holds the rest value; motion is a channel.** A property a
 *   clip keys is a `Channel::EmitterProperty` channel on the node whose `sub`
 *   is the property's enumerator below (`EmitterPropertySub`). A property the
 *   formats share keeps its shared channel — an emitter's `Visibility`, a
 *   Warcraft III ribbon's `Color`, `Alpha` and `TextureIndex`, a `CORN`'s
 *   `Color` and `Alpha`.
 * - **Cross-references are WEM indices.** A material is a `Model::materialSlots`
 *   index, a texture a `Document::textures` index, and another emitter or a bone
 *   a node index — each a §10.6 / §7.5 referencer, enumerated by the static
 *   `forEach*Link` members (and `ForEachNodeLink` over a whole payload, in
 *   `node.h`) so compaction and `Validate` walk one list.
 *
 * Values are in the source system's own frame and units. For Warcraft III that
 * is WEM's canonical space; a StarCraft II payload stays in StarCraft II's basis
 * (§6.4 rebases the node, not the system's parameters, which only StarCraft II
 * reads), so a consumer placing one in WEM space composes the node's world
 * transform with the basis change.
 */

#include <string>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/compatibility.h>
#include <whiteout/vector_types.h>

#include "../asset_key.h"
#include "../profile.h"

namespace whiteout {
namespace models {
namespace wem {

// ============================================================================
// Links and properties
// ============================================================================

/// What one of a payload's node links points at, so `Validate` can hold it to
/// the kind it has to be and say which link is wrong.
enum class EmitterLink : u8 {
    CopySource,     ///< `copyOf`: a `Sc2ParticleEmitter` that is not itself a copy.
    CollisionSpawn, ///< A `Sc2ParticleEmitter`.
    Trail,          ///< A `Sc2ParticleEmitter`.
    BounceRibbon,   ///< A `Sc2RibbonEmitter`.
    SplineBone,     ///< Any node; a bone in every shipped file.
    /// Not an emitter link: `NodeSkinSetup::mirror`, the skin setup's override
    /// of §7.5's mirror map. It rides this walk because the referencer table is
    /// one walk, and it must name a `Bone`.
    SkinMirror,
};

const char* ToString(EmitterLink link);

/// `TrackTarget::sub` of an `EmitterProperty` channel: the property in the low
/// half, and for a per-element property (a spline point's) the element in the
/// high half.
constexpr u32 EmitterPropertySub(u32 property, u32 element = 0) {
    return (property & 0xFFFFu) | (element << 16);
}
constexpr u32 EmitterPropertyOf(u32 sub) {
    return sub & 0xFFFFu;
}
constexpr u32 EmitterElementOf(u32 sub) {
    return sub >> 16;
}

/// MDX `PREM` tracks (KPEE, KPEG, KPLN, KPLT, KPEL, KPES).
enum class Wc3Particle1Property : u32 {
    EmissionRate,
    Gravity,
    Longitude,
    Latitude,
    Lifespan,
    Speed,
    Count
};

/// MDX `PRE2` tracks (KP2S, KP2R, KP2L, KP2G, KP2E, KP2N, KP2W).
enum class Wc3Particle2Property : u32 {
    Speed,
    Variation,
    Latitude,
    Gravity,
    EmissionRate,
    Width,
    Length,
    Count
};

/// MDX `RIBB` tracks beyond the shared channels (KRHA, KRHB).
enum class Wc3RibbonProperty : u32 { HeightAbove, HeightBelow, Count };

/// MDX `CORN` tracks beyond the shared channels (KPPL, KPPE, KPPS).
enum class Wc3CornProperty : u32 { Lifespan, EmissionRate, Speed, Count };

/// M3 `PAR_` AnimRefs, and a `PARC` copy's two. `SplinePoint` is per element.
enum class Sc2ParticleProperty : u32 {
    InitialSpeed,
    InitialSpeedRandom,
    InitialYaw,
    InitialPitch,
    InitialHorizontal,
    InitialVertical,
    Lifetime,
    LifetimeRandom,
    SizeAnimation,
    RotationAnimation,
    ColorStart,
    ColorMid,
    ColorEnd,
    EmissionRate,
    ShapeOuter,
    ShapeInner,
    OuterRadius,
    InnerRadius,
    SizeRandomAnimation,
    RotationRandomAnimation,
    ColorStartRandom,
    ColorMidRandom,
    ColorEndRandom,
    SquirtAmount,
    PitchAmplitude,
    PitchFrequency,
    YawAmplitude,
    YawFrequency,
    SpeedAmplitude,
    SpeedFrequency,
    SizeAmplitude,
    SizeFrequency,
    AlphaAmplitude,
    AlphaFrequency,
    ColorAmplitude,
    ColorFrequency,
    RotationAmplitude,
    RotationFrequency,
    HorizontalAmplitude,
    HorizontalFrequency,
    VerticalAmplitude,
    VerticalFrequency,
    ParticleVelocity,
    PhaseShift,
    AlphaThreshold,
    UvOffset,
    UvAngle,
    UvTiling,
    SplinePoint,
    LowerBound,
    UpperBound,
    TrailEmissionRate,
    Count
};

/// M3 `RIB_` AnimRefs; the `Spline*` ones are per `SRIB` element.
enum class Sc2RibbonProperty : u32 {
    InitialSpeed,
    InitialSpeedRandom,
    InitialYaw,
    InitialPitch,
    InitialHorizontal,
    InitialVertical,
    Lifetime,
    LifetimeRandom,
    SizeAnimation,
    RotationAnimation,
    ColorStart,
    ColorMid,
    ColorEnd,
    MaxLength,
    Active,
    YawAmplitude,
    YawFrequency,
    PitchAmplitude,
    PitchFrequency,
    SpeedAmplitude,
    SpeedFrequency,
    SizeAmplitude,
    SizeFrequency,
    AlphaAmplitude,
    AlphaFrequency,
    ParticleVelocity,
    Overlay,
    SplineVelocity,
    SplineVelocityBaseFactor,
    SplineVelocityEndFactor,
    SplineYawAmplitude,
    SplineYawFrequency,
    SplinePitchAmplitude,
    SplinePitchFrequency,
    SplineVelocityAmplitude,
    SplineVelocityFrequency,
    SplineYaw,
    SplinePitch,
    Count
};

// ============================================================================
// Warcraft III — carried by `Wc3Classic` and `Wc3Reforged`
// ============================================================================

/// `PREM`: every particle is a copy of `spawnModel`, flung from the node.
struct Wc3ParticleEmitter1Payload {
    f32 emissionRate = 0; ///< Particles a second.
    f32 gravity = 0;
    f32 longitude = 0; ///< Radians.
    f32 latitude = 0;  ///< Radians.
    f32 lifespan = 0;  ///< Seconds.
    f32 speed = 0;     ///< `initialVelocity`.
    AssetKey spawnModel; ///< The model each particle is, by path.
    bool usesMdl = false; ///< Node flag 0x8000.
    bool usesTga = false; ///< Node flag 0x10000.

    template <class V>
    void reflect(V& v) {
        v.field("emissionRate", emissionRate);
        v.field("gravity", gravity);
        v.field("longitude", longitude);
        v.field("latitude", latitude);
        v.field("lifespan", lifespan);
        v.field("speed", speed);
        v.field("spawnModel", spawnModel);
        v.field("usesMdl", usesMdl);
        v.field("usesTga", usesTga);
    }
};

/// `PRE2`'s own blend vocabulary — not a material's `filterMode`.
enum class Wc3ParticleFilter : u32 { Blend = 0, Additive = 1, Modulate = 2, Modulate2x = 3, AlphaKey = 4 };

enum class Wc3ParticleHeadOrTail : u32 { Head = 0, Tail = 1, Both = 2 };

/// A particle's look at one of its three lifetime points: birth, `time`, death.
struct Wc3ParticleSegment {
    Vector3f color{0, 0, 0}; ///< RGB, red first — the static-colour order.
    u8 alpha = 0;
    f32 scaling = 0; ///< The quad's size, in model units.

    template <class V>
    void reflect(V& v) {
        v.field("color", color);
        v.field("alpha", alpha);
        v.field("scaling", scaling);
    }
};

/// A flipbook run: cells `start..end`, `repeat` times over the span.
struct Wc3ParticleInterval {
    u32 start = 0;
    u32 end = 0;
    u32 repeat = 0;

    template <class V>
    void reflect(V& v) {
        v.field("start", start);
        v.field("end", end);
        v.field("repeat", repeat);
    }
};

/// `PRE2`: camera-facing flipbook quads — the Warcraft III particle.
struct Wc3ParticleEmitter2Payload {
    f32 speed = 0;
    f32 variation = 0; ///< Of the speed, as a fraction.
    f32 latitude = 0;  ///< Degrees, unlike `PREM`'s.
    f32 gravity = 0;
    f32 lifespan = 0; ///< Seconds.
    f32 emissionRate = 0;
    f32 width = 0;  ///< The emission area, in model units.
    f32 length = 0;

    Wc3ParticleFilter filter = Wc3ParticleFilter::Blend;
    u32 rows = 1;    ///< Flipbook grid.
    u32 columns = 1;
    Wc3ParticleHeadOrTail headOrTail = Wc3ParticleHeadOrTail::Head;
    f32 tailLength = 0;
    f32 time = 0; ///< Where `middle` sits in the lifespan, 0..1.

    Wc3ParticleSegment start;
    Wc3ParticleSegment middle;
    Wc3ParticleSegment end;

    Wc3ParticleInterval headLife;
    Wc3ParticleInterval headDecay;
    Wc3ParticleInterval tailLife;
    Wc3ParticleInterval tailDecay;

    u32 texture = kInvalidIndex; ///< -> `Document::textures`.
    u32 replaceableId = 0;
    bool squirt = false; ///< Emission is a burst per rate key, not a rate.
    i32 priorityPlane = 0;

    // The node flag bits PRE2 gives meanings of its own.
    bool unshaded = false;      ///< 0x8000.
    bool sortPrimsFarZ = false; ///< 0x10000.
    bool lineEmitter = false;   ///< 0x20000.
    bool unfogged = false;      ///< 0x40000.
    bool xyQuad = false;        ///< 0x100000.

    template <class Self, class F>
    static void forEachTextureLink(Self& self, F&& f) {
        f(self.texture);
    }

    template <class V>
    void reflect(V& v) {
        v.field("speed", speed);
        v.field("variation", variation);
        v.field("latitude", latitude);
        v.field("gravity", gravity);
        v.field("lifespan", lifespan);
        v.field("emissionRate", emissionRate);
        v.field("width", width);
        v.field("length", length);
        v.field("filter", filter);
        v.field("rows", rows);
        v.field("columns", columns);
        v.field("headOrTail", headOrTail);
        v.field("tailLength", tailLength);
        v.field("time", time);
        v.field("start", start);
        v.field("middle", middle);
        v.field("end", end);
        v.field("headLife", headLife);
        v.field("headDecay", headDecay);
        v.field("tailLife", tailLife);
        v.field("tailDecay", tailDecay);
        v.field("texture", texture);
        v.field("replaceableId", replaceableId);
        v.field("squirt", squirt);
        v.field("priorityPlane", priorityPlane);
        v.field("unshaded", unshaded);
        v.field("sortPrimsFarZ", sortPrimsFarZ);
        v.field("lineEmitter", lineEmitter);
        v.field("unfogged", unfogged);
        v.field("xyQuad", xyQuad);
    }
};

/// `RIBB`: a strip swept between `heightAbove` and `heightBelow` of the node.
///
/// Its colour, alpha and flipbook cell key on the shared `Color`, `Alpha` and
/// `TextureIndex` channels and rest here.
struct Wc3RibbonEmitterPayload {
    f32 heightAbove = 0;
    f32 heightBelow = 0;
    f32 alpha = 1;
    Vector3f color{1, 1, 1}; ///< RGB, red first.
    f32 lifespan = 0;        ///< Seconds a segment lives.
    u32 textureSlot = 0;     ///< The flipbook cell.
    u32 emissionRate = 0;    ///< Segments a second.
    u32 rows = 1;
    u32 columns = 1;
    u32 materialSlot = kInvalidIndex; ///< -> `Model::materialSlots`.
    f32 gravity = 0;

    template <class Self, class F>
    static void forEachMaterialLink(Self& self, F&& f) {
        f(self.materialSlot);
    }

    template <class V>
    void reflect(V& v) {
        v.field("heightAbove", heightAbove);
        v.field("heightBelow", heightBelow);
        v.field("alpha", alpha);
        v.field("color", color);
        v.field("lifespan", lifespan);
        v.field("textureSlot", textureSlot);
        v.field("emissionRate", emissionRate);
        v.field("rows", rows);
        v.field("columns", columns);
        v.field("materialSlot", materialSlot);
        v.field("gravity", gravity);
    }
};

/**
 * @brief `CORN`: a PopcornFX effect run at the node (Reforged).
 *
 * The effect itself is a `.pkb` WEM does not hold, named by `effect`; what the
 * model owns is the five numbers the game hands the effect every frame. They
 * are MULTIPLIERS on the effect's own values (the game's reader names each
 * track so: "lifespan multiplier keys", ...), which is why they rest at 1.
 *
 * Its colour, alpha and visibility key on the shared `Color`, `Alpha` and
 * `Visibility` channels and rest here; lifespan, emission rate and speed are
 * `Wc3CornProperty` channels. A keyed colour is blue first in the file, like
 * every other Warcraft III colour key (`ReadBinParticleEmitterPopcorn` keeps the
 * keys as read and reverses only the static colour); the channel is RGB.
 */
struct Wc3CornEmitterPayload {
    f32 lifespan = 1;        ///< Multiplies the effect's particle lifespan.
    f32 emissionRate = 1;    ///< Multiplies its emission rate.
    f32 speed = 1;           ///< Multiplies its particle speed.
    Vector3f color{1, 1, 1}; ///< Multiplies its colour. RGB, red first.
    f32 alpha = 1;           ///< Multiplies its alpha.
    u32 replaceableId = 0;   ///< The team colour/glow the effect's textures take.

    AssetKey effect; ///< The PopcornFX effect, by path — a `.pkb`.
    /// The effect's animation-visibility guide, by name. Opaque here: the
    /// engine resolves it against the effect.
    std::string animVisibilityGuide;

    // The node flag bits CORN gives meanings of its own. 0x20000 and 0x40000
    // are NOT PRE2's: PRE2 reads them as `LineEmitter` and `Unfogged`.
    bool unshaded = false;       ///< 0x8000.
    bool sortPrimsFarZ = false;  ///< 0x10000.
    bool unfogged = false;       ///< 0x20000.
    bool popcornScaling = false; ///< 0x40000: the node's scale reaches the effect.

    template <class V>
    void reflect(V& v) {
        v.field("lifespan", lifespan);
        v.field("emissionRate", emissionRate);
        v.field("speed", speed);
        v.field("color", color);
        v.field("alpha", alpha);
        v.field("replaceableId", replaceableId);
        v.field("effect", effect);
        v.field("animVisibilityGuide", animVisibilityGuide);
        v.field("unshaded", unshaded);
        v.field("sortPrimsFarZ", sortPrimsFarZ);
        v.field("unfogged", unfogged);
        v.field("popcornScaling", popcornScaling);
    }
};

// ============================================================================
// StarCraft II — carried by `Sc2` and `Heroes`
// ============================================================================

/**
 * @brief One animatable property of a StarCraft II emitter, at rest.
 *
 * An M3 AnimRef without its link. `initValue` is what plays while no clip keys
 * the property; the link — the animId and the keys — is the node's
 * `Channel::EmitterProperty` channel, whose id is the AnimRef's own (§10.8.1).
 *
 * `nullValue` is live, not padding: the engine retires an unkeyed `PAR_` whose
 * rate and squirt, or a `RIB_` whose `active`, sit at their nulls before it
 * emits anything. Shipped emitters rest every null at zero.
 *
 * Colours are RGBA in 0..1, the channel convention; a `u16` squirt count widens
 * to `u32`.
 *
 * @bind value_template, instantiate=f32;u32;Vector2f;Vector3f;Vector4f
 */
template <class T>
struct Sc2Property {
    T initValue{};
    T nullValue{};

    template <class V>
    void reflect(V& v) {
        v.field("initValue", initValue);
        v.field("nullValue", nullValue);
    }
};

/// One of the per-particle variation channels: a curve type, and the amplitude
/// and frequency it runs at.
struct Sc2Variation {
    u32 type = 0; ///< 0 = none.
    Sc2Property<f32> amplitude;
    Sc2Property<f32> frequency;

    template <class V>
    void reflect(V& v) {
        v.field("type", type);
        v.field("amplitude", amplitude);
        v.field("frequency", frequency);
    }
};

/// `PAR_` emission shapes, by the file's own value.
enum class Sc2EmitterShape : u32 {
    Point = 0,
    Plane = 1,
    Sphere = 2,
    Box = 3,
    Cylinder = 4,
    Disc = 5,
    Spline = 6, ///< Along `splinePoints`.
    Mesh = 7,   ///< Off `shapeSections`.
};

/// `PAR_` instance types — what one particle is drawn as.
enum class Sc2ParticleInstance : u32 {
    Billboard = 0,
    Tail = 1,
    FaceTravelDir = 2,
    FaceWorldDir = 3,
    SingleAxis = 4,
    TerrainOriented = 5,
    TerrainDirOriented = 6,
    EmitterOriented = 7,
    PhysicsOriented = 8,
    Pinned = 9,
    Trail = 10,
};

/// How a birth-middle-death curve is blended.
enum class Sc2Smoothing : u32 {
    Linear = 0,
    LinearSmooth = 1,
    Bezier = 2,
    LinearWithHold = 3,
    BezierWithHold = 4,
};

enum class Sc2RibbonType : u32 { Billboard = 0, Planar = 1, Cylinder = 2, Star = 3 };

/**
 * @brief `PAR_` — the StarCraft II particle system — or one `PARC` copy of it.
 *
 * Field names are the M3 record's. The bit words keep the file's own bits:
 * `flags` is `m3::ParticleFlag`, `additionalFlags` `m3::ParticleAdditionalFlag`,
 * `rotationFlags` `m3::ParticleRotationFlag`.
 *
 * **A copy is a node too.** A `PARC` is another emission point of one system —
 * its own bone, emission rate and squirt, the source's everything else — so it
 * imports as a `Sc2ParticleEmitter` node under that bone whose `copyOf` names the
 * source, and reads nothing of its payload but `emissionRate` and `squirtAmount`.
 * The engine numbers copies as slots 1..n in node order.
 */
struct Sc2ParticleEmitterPayload {
    u32 copyOf = kInvalidNode;        ///< Set on a `PARC`: the emitter it copies.
    u32 materialSlot = kInvalidIndex; ///< -> `Model::materialSlots`.
    u32 additionalFlags = 0;

    Sc2Property<f32> initialSpeed;
    Sc2Property<f32> initialSpeedRandom;
    Sc2Property<f32> initialYaw;
    Sc2Property<f32> initialPitch;
    Sc2Property<f32> initialHorizontal;
    Sc2Property<f32> initialVertical;
    Sc2Property<f32> lifetime;
    Sc2Property<f32> lifetimeRandom;

    f32 killRadius = 0;
    u32 gravityX = 0; ///< Stored as the file does; 0 everywhere shipped.
    u32 gravityY = 0;
    f32 gravity = 0;

    f32 sizeMidTime = 0.5f;
    f32 colorMidTime = 0.5f;
    f32 alphaMidTime = 0.5f;
    f32 rotationMidTime = 0.5f;
    f32 sizeMidHoldTime = 0;
    f32 colorMidHoldTime = 0;
    f32 alphaMidHoldTime = 0;
    f32 rotationMidHoldTime = 0;

    Sc2Property<Vector3f> sizeAnimation;     ///< Birth, middle, death.
    Sc2Property<Vector3f> rotationAnimation; ///< Birth, middle, death.
    Sc2Property<Vector4f> colorStart;
    Sc2Property<Vector4f> colorMid;
    Sc2Property<Vector4f> colorEnd;

    f32 drag = 0;
    f32 mass = 0.001f;
    f32 massRandom = 1;
    f32 massSizeMultiplier = 0;
    u16 localForces = 0; ///< Force channel masks.
    u16 worldForces = 0;
    u16 localForcesFallback = 0;
    u16 worldForcesFallback = 0;
    f32 worldForcesMassMultiplier = 1;

    f32 noiseAmplitude = 0;
    f32 noiseFrequency = 0;
    f32 noiseCoherence = 0;
    f32 noiseEdge = 0;
    u32 indexPlusLength = 0;

    u32 maxParticles = 0;
    Sc2Property<f32> emissionRate;
    Sc2EmitterShape emitterShape = Sc2EmitterShape::Point;
    Sc2Property<Vector3f> shapeOuter;
    Sc2Property<Vector3f> shapeInner;
    Sc2Property<f32> outerRadius;
    Sc2Property<f32> innerRadius;
    /// The mesh sections a `Mesh` shape emits from: sections of
    /// `Model::meshes[0]`, which is the one division StarCraft II draws, so a
    /// section index is the region index the file stores.
    std::vector<u32> shapeSections;

    u32 velocityType = 0;
    u32 sizeRandomEnable = 0;
    Sc2Property<Vector3f> sizeRandomAnimation;
    u32 rotationRandomEnable = 0;
    Sc2Property<Vector3f> rotationRandomAnimation;
    u32 colorRandomEnable = 0;
    Sc2Property<Vector4f> colorStartRandom;
    Sc2Property<Vector4f> colorMidRandom;
    Sc2Property<Vector4f> colorEndRandom;
    u32 alphaRandomEnable = 0;

    Sc2Property<u32> squirtAmount; ///< A burst count, keyed as a step.
    u8 flipbookStartInitIndex = 0;
    u8 flipbookStartStopIndex = 0;
    u8 flipbookEndInitIndex = 0;
    u8 flipbookEndStopIndex = 0;
    f32 flipbookMidTime = 0;
    u16 flipbookColumns = 0;
    u16 flipbookRows = 0;
    f32 flipbookColumnFraction = 0;
    f32 flipbookRowFraction = 0;

    f32 bounce = 0;
    f32 friction = 1;
    u32 collisionSpawn = kInvalidNode; ///< The emitter a collision spawns from.
    u32 collisionSpawnMin = 0;
    u32 collisionSpawnMax = 0;
    f32 collisionSpawnChance = 0;
    f32 collisionSpawnEnergy = 0;
    u32 collisionDieBounce = 0;

    Sc2ParticleInstance instanceType = Sc2ParticleInstance::Billboard;
    f32 tailLength = 1;
    Vector3f instanceAngle{0, 0, 0};
    f32 instanceDistance = 1;

    Sc2Variation pitch;
    Sc2Variation yaw;
    Sc2Variation speed;
    Sc2Variation size;
    Sc2Variation alpha;
    Sc2Variation color;
    Sc2Variation rotation;
    Sc2Variation horizontal;
    Sc2Variation vertical;

    Sc2Property<f32> particleVelocity;
    Sc2Property<f32> phaseShift;

    u32 flags = 0;
    u32 rotationFlags = 0;
    Sc2Smoothing colorSmoothing = Sc2Smoothing::Linear;
    Sc2Smoothing sizeSmoothing = Sc2Smoothing::Linear;
    Sc2Smoothing rotationSmoothing = Sc2Smoothing::Linear;

    Sc2Property<f32> alphaThreshold;
    Sc2Property<Vector2f> uvOffset;
    Sc2Property<Vector3f> uvAngle;
    Sc2Property<Vector2f> uvTiling;

    std::vector<Sc2Property<Vector3f>> splinePoints; ///< A `Spline` shape's path.

    f32 windMultiplier = 0;
    u32 lodReduce = 2;
    u32 lodCut = 0;

    Sc2Property<f32> lowerBound;
    Sc2Property<f32> upperBound;

    u32 trailLink = kInvalidNode; ///< The emitter this one leaves trails with.
    f32 trailChance = 0;
    Sc2Property<f32> trailEmissionRate;

    /// A `PROJ` index. WEM holds no projectors, so the number is carried as the
    /// file had it; -1 is none, which is every shipped emitter measured.
    i32 splatProjectionIndex = -1;
    f32 splatChance = 0;

    std::vector<AssetKey> models; ///< Model particles, by path (`SCHR`).

    f32 spawnRibbonOnBounceChance = 0;
    u32 ribbonLink = kInvalidNode; ///< The ribbon a bounce spawns.

    f32 noiseSmoothness = 0; ///< Pre-v12 only; the upgrade has nowhere to put it.

    bool isCopy() const {
        return copyOf != kInvalidNode;
    }

    /// Every node index held, and what it has to point at.
    template <class Self, class F>
    static void forEachNodeLink(Self& self, F&& f) {
        f(self.copyOf, EmitterLink::CopySource);
        f(self.collisionSpawn, EmitterLink::CollisionSpawn);
        f(self.trailLink, EmitterLink::Trail);
        f(self.ribbonLink, EmitterLink::BounceRibbon);
    }

    template <class Self, class F>
    static void forEachMaterialLink(Self& self, F&& f) {
        f(self.materialSlot);
    }

    template <class V>
    void reflect(V& v) {
        v.field("copyOf", copyOf);
        v.field("materialSlot", materialSlot);
        v.field("additionalFlags", additionalFlags);
        v.field("initialSpeed", initialSpeed);
        v.field("initialSpeedRandom", initialSpeedRandom);
        v.field("initialYaw", initialYaw);
        v.field("initialPitch", initialPitch);
        v.field("initialHorizontal", initialHorizontal);
        v.field("initialVertical", initialVertical);
        v.field("lifetime", lifetime);
        v.field("lifetimeRandom", lifetimeRandom);
        v.field("killRadius", killRadius);
        v.field("gravityX", gravityX);
        v.field("gravityY", gravityY);
        v.field("gravity", gravity);
        v.field("sizeMidTime", sizeMidTime);
        v.field("colorMidTime", colorMidTime);
        v.field("alphaMidTime", alphaMidTime);
        v.field("rotationMidTime", rotationMidTime);
        v.field("sizeMidHoldTime", sizeMidHoldTime);
        v.field("colorMidHoldTime", colorMidHoldTime);
        v.field("alphaMidHoldTime", alphaMidHoldTime);
        v.field("rotationMidHoldTime", rotationMidHoldTime);
        v.field("sizeAnimation", sizeAnimation);
        v.field("rotationAnimation", rotationAnimation);
        v.field("colorStart", colorStart);
        v.field("colorMid", colorMid);
        v.field("colorEnd", colorEnd);
        v.field("drag", drag);
        v.field("mass", mass);
        v.field("massRandom", massRandom);
        v.field("massSizeMultiplier", massSizeMultiplier);
        v.field("localForces", localForces);
        v.field("worldForces", worldForces);
        v.field("localForcesFallback", localForcesFallback);
        v.field("worldForcesFallback", worldForcesFallback);
        v.field("worldForcesMassMultiplier", worldForcesMassMultiplier);
        v.field("noiseAmplitude", noiseAmplitude);
        v.field("noiseFrequency", noiseFrequency);
        v.field("noiseCoherence", noiseCoherence);
        v.field("noiseEdge", noiseEdge);
        v.field("indexPlusLength", indexPlusLength);
        v.field("maxParticles", maxParticles);
        v.field("emissionRate", emissionRate);
        v.field("emitterShape", emitterShape);
        v.field("shapeOuter", shapeOuter);
        v.field("shapeInner", shapeInner);
        v.field("outerRadius", outerRadius);
        v.field("innerRadius", innerRadius);
        v.field("shapeSections", shapeSections);
        v.field("velocityType", velocityType);
        v.field("sizeRandomEnable", sizeRandomEnable);
        v.field("sizeRandomAnimation", sizeRandomAnimation);
        v.field("rotationRandomEnable", rotationRandomEnable);
        v.field("rotationRandomAnimation", rotationRandomAnimation);
        v.field("colorRandomEnable", colorRandomEnable);
        v.field("colorStartRandom", colorStartRandom);
        v.field("colorMidRandom", colorMidRandom);
        v.field("colorEndRandom", colorEndRandom);
        v.field("alphaRandomEnable", alphaRandomEnable);
        v.field("squirtAmount", squirtAmount);
        v.field("flipbookStartInitIndex", flipbookStartInitIndex);
        v.field("flipbookStartStopIndex", flipbookStartStopIndex);
        v.field("flipbookEndInitIndex", flipbookEndInitIndex);
        v.field("flipbookEndStopIndex", flipbookEndStopIndex);
        v.field("flipbookMidTime", flipbookMidTime);
        v.field("flipbookColumns", flipbookColumns);
        v.field("flipbookRows", flipbookRows);
        v.field("flipbookColumnFraction", flipbookColumnFraction);
        v.field("flipbookRowFraction", flipbookRowFraction);
        v.field("bounce", bounce);
        v.field("friction", friction);
        v.field("collisionSpawn", collisionSpawn);
        v.field("collisionSpawnMin", collisionSpawnMin);
        v.field("collisionSpawnMax", collisionSpawnMax);
        v.field("collisionSpawnChance", collisionSpawnChance);
        v.field("collisionSpawnEnergy", collisionSpawnEnergy);
        v.field("collisionDieBounce", collisionDieBounce);
        v.field("instanceType", instanceType);
        v.field("tailLength", tailLength);
        v.field("instanceAngle", instanceAngle);
        v.field("instanceDistance", instanceDistance);
        v.field("pitch", pitch);
        v.field("yaw", yaw);
        v.field("speed", speed);
        v.field("size", size);
        v.field("alpha", alpha);
        v.field("color", color);
        v.field("rotation", rotation);
        v.field("horizontal", horizontal);
        v.field("vertical", vertical);
        v.field("particleVelocity", particleVelocity);
        v.field("phaseShift", phaseShift);
        v.field("flags", flags);
        v.field("rotationFlags", rotationFlags);
        v.field("colorSmoothing", colorSmoothing);
        v.field("sizeSmoothing", sizeSmoothing);
        v.field("rotationSmoothing", rotationSmoothing);
        v.field("alphaThreshold", alphaThreshold);
        v.field("uvOffset", uvOffset);
        v.field("uvAngle", uvAngle);
        v.field("uvTiling", uvTiling);
        v.field("splinePoints", splinePoints);
        v.field("windMultiplier", windMultiplier);
        v.field("lodReduce", lodReduce);
        v.field("lodCut", lodCut);
        v.field("lowerBound", lowerBound);
        v.field("upperBound", upperBound);
        v.field("trailLink", trailLink);
        v.field("trailChance", trailChance);
        v.field("trailEmissionRate", trailEmissionRate);
        v.field("splatProjectionIndex", splatProjectionIndex);
        v.field("splatChance", splatChance);
        v.field("models", models);
        v.field("spawnRibbonOnBounceChance", spawnRibbonOnBounceChance);
        v.field("ribbonLink", ribbonLink);
        v.field("noiseSmoothness", noiseSmoothness);
    }
};

/// `SRIB`: one control point of a `RIB_`'s spline, riding a bone of its own.
struct Sc2RibbonSplinePoint {
    u32 node = kInvalidNode; ///< The bone it rides.
    Vector3f emissionOffset{0, 0, 0};
    Vector3f emissionVector{0, 0, 0};
    Sc2Property<f32> velocity;
    u32 reserved = 0;
    Sc2Property<f32> velocityBaseFactor;
    Sc2Property<f32> velocityEndFactor;
    Sc2Variation yawVariation;
    Sc2Variation pitchVariation;
    Sc2Variation velocityVariation;
    Sc2Property<f32> yaw;
    Sc2Property<f32> pitch;
    f32 emissionVectorNormFactor = 0; ///< Shipped as ~0.01 / |emissionVector|.
    f32 velocityNormFactor = 0;       ///< Shipped as ~0.01 / velocity.

    template <class V>
    void reflect(V& v) {
        v.field("node", node);
        v.field("emissionOffset", emissionOffset);
        v.field("emissionVector", emissionVector);
        v.field("velocity", velocity);
        v.field("reserved", reserved);
        v.field("velocityBaseFactor", velocityBaseFactor);
        v.field("velocityEndFactor", velocityEndFactor);
        v.field("yawVariation", yawVariation);
        v.field("pitchVariation", pitchVariation);
        v.field("velocityVariation", velocityVariation);
        v.field("yaw", yaw);
        v.field("pitch", pitch);
        v.field("emissionVectorNormFactor", emissionVectorNormFactor);
        v.field("velocityNormFactor", velocityNormFactor);
    }
};

/**
 * @brief `RIB_` — the StarCraft II ribbon.
 *
 * Its bone is the node's parent. The record's second `u16` beside the bone is
 * the high half of one `u32` bone index to the editor, zero on every shipped
 * ribbon, and is written as zero rather than carried. `flags` is
 * `m3::RibbonFlag`, `additionalFlags` `m3::RibbonAdditionalFlag`.
 */
struct Sc2RibbonEmitterPayload {
    u32 materialSlot = kInvalidIndex; ///< -> `Model::materialSlots`.
    u32 additionalFlags = 0;

    Sc2Property<f32> initialSpeed;
    Sc2Property<f32> initialSpeedRandom;
    Sc2Property<f32> initialYaw;
    Sc2Property<f32> initialPitch;
    Sc2Property<f32> initialHorizontal;
    Sc2Property<f32> initialVertical;
    Sc2Property<f32> lifetime;
    Sc2Property<f32> lifetimeRandom;
    u32 killRadius = 0; ///< A `u32` in the ribbon record, unlike the particle's.

    f32 gravityX = 0;
    f32 gravityY = 0;
    f32 gravity = 0;

    f32 sizeMidTime = 0;
    f32 colorMidTime = 0;
    f32 alphaMidTime = 0;
    f32 rotationMidTime = 0;
    f32 sizeMidHoldTime = 0;
    f32 colorMidHoldTime = 0;
    f32 alphaMidHoldTime = 0;
    f32 rotationMidHoldTime = 0;

    Sc2Property<Vector3f> sizeAnimation;
    Sc2Property<Vector3f> rotationAnimation;
    Sc2Property<Vector4f> colorStart;
    Sc2Property<Vector4f> colorMid;
    Sc2Property<Vector4f> colorEnd;

    f32 drag = 0;
    f32 mass = 0;
    f32 massRandom = 0;
    f32 massSizeMultiplier = 0;
    u16 localForces = 0;
    u16 worldForces = 0;
    u16 localForcesFallback = 0;
    u16 worldForcesFallback = 0;
    f32 worldForcesMassMultiplier = 0;

    f32 noiseAmplitude = 0;
    f32 noiseFrequency = 0;
    f32 noiseCoherence = 0;
    f32 noiseEdge = 0;
    u32 indexPlusLength = 1;

    u32 emitterShape = 0;
    Sc2RibbonType ribbonType = Sc2RibbonType::Billboard;
    f32 divisions = 0;
    u32 edges = 0;
    f32 innerRadius = 0;
    Sc2Property<f32> maxLength;

    std::vector<Sc2RibbonSplinePoint> splinePoints; ///< `SRIB`.
    /// A flag, keyed as one. Rests at null 0 on every shipped ribbon.
    Sc2Property<u32> active;

    u32 flags = 0;
    Sc2Smoothing sizeSmoothing = Sc2Smoothing::Linear;
    Sc2Smoothing colorSmoothing = Sc2Smoothing::Linear;

    f32 friction = 0;
    f32 bounce = 0;
    u32 lodReduce = 0;
    u32 lodCut = 0;

    Sc2Variation yaw;
    Sc2Variation pitch;
    Sc2Variation speed;
    Sc2Variation size;
    Sc2Variation alpha;

    Sc2Property<f32> particleVelocity;
    Sc2Property<f32> overlay;

    i32 deprecatedUnknown = 0; ///< Pre-v7 only (`unknown3fbae7d6`).

    template <class Self, class F>
    static void forEachNodeLink(Self& self, F&& f) {
        for (auto& point : self.splinePoints) {
            f(point.node, EmitterLink::SplineBone);
        }
    }

    template <class Self, class F>
    static void forEachMaterialLink(Self& self, F&& f) {
        f(self.materialSlot);
    }

    template <class V>
    void reflect(V& v) {
        v.field("materialSlot", materialSlot);
        v.field("additionalFlags", additionalFlags);
        v.field("initialSpeed", initialSpeed);
        v.field("initialSpeedRandom", initialSpeedRandom);
        v.field("initialYaw", initialYaw);
        v.field("initialPitch", initialPitch);
        v.field("initialHorizontal", initialHorizontal);
        v.field("initialVertical", initialVertical);
        v.field("lifetime", lifetime);
        v.field("lifetimeRandom", lifetimeRandom);
        v.field("killRadius", killRadius);
        v.field("gravityX", gravityX);
        v.field("gravityY", gravityY);
        v.field("gravity", gravity);
        v.field("sizeMidTime", sizeMidTime);
        v.field("colorMidTime", colorMidTime);
        v.field("alphaMidTime", alphaMidTime);
        v.field("rotationMidTime", rotationMidTime);
        v.field("sizeMidHoldTime", sizeMidHoldTime);
        v.field("colorMidHoldTime", colorMidHoldTime);
        v.field("alphaMidHoldTime", alphaMidHoldTime);
        v.field("rotationMidHoldTime", rotationMidHoldTime);
        v.field("sizeAnimation", sizeAnimation);
        v.field("rotationAnimation", rotationAnimation);
        v.field("colorStart", colorStart);
        v.field("colorMid", colorMid);
        v.field("colorEnd", colorEnd);
        v.field("drag", drag);
        v.field("mass", mass);
        v.field("massRandom", massRandom);
        v.field("massSizeMultiplier", massSizeMultiplier);
        v.field("localForces", localForces);
        v.field("worldForces", worldForces);
        v.field("localForcesFallback", localForcesFallback);
        v.field("worldForcesFallback", worldForcesFallback);
        v.field("worldForcesMassMultiplier", worldForcesMassMultiplier);
        v.field("noiseAmplitude", noiseAmplitude);
        v.field("noiseFrequency", noiseFrequency);
        v.field("noiseCoherence", noiseCoherence);
        v.field("noiseEdge", noiseEdge);
        v.field("indexPlusLength", indexPlusLength);
        v.field("emitterShape", emitterShape);
        v.field("ribbonType", ribbonType);
        v.field("divisions", divisions);
        v.field("edges", edges);
        v.field("innerRadius", innerRadius);
        v.field("maxLength", maxLength);
        v.field("splinePoints", splinePoints);
        v.field("active", active);
        v.field("flags", flags);
        v.field("sizeSmoothing", sizeSmoothing);
        v.field("colorSmoothing", colorSmoothing);
        v.field("friction", friction);
        v.field("bounce", bounce);
        v.field("lodReduce", lodReduce);
        v.field("lodCut", lodCut);
        v.field("yaw", yaw);
        v.field("pitch", pitch);
        v.field("speed", speed);
        v.field("size", size);
        v.field("alpha", alpha);
        v.field("particleVelocity", particleVelocity);
        v.field("overlay", overlay);
        v.field("deprecatedUnknown", deprecatedUnknown);
    }
};

} // namespace wem
} // namespace models
} // namespace whiteout
