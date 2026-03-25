// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file effect.h
 * @brief Particle and ribbon effect structures — emitters, splines, projectors
 *
 * Defines the M3 particle and effect chunk types: ParticleEmitter (PAR_) with
 * extensive animation, physics, noise, collision, and variation channels;
 * ParticleEmitterCopy (PARC) for instanced emitters; SplineRibbon (SRIB) for
 * spline-based ribbon segments; RibbonEmitter (RIB_) for ribbon strip effects;
 * and Projector (PROJ) for decal / projection effects.
 *
 * @see M3_FILE_FORMAT_SPECIFICATION.md §12 Particle Systems
 */

#include "base.h"

namespace whiteout {
namespace m3 {

// ============================================================================
// Particle Systems
// ============================================================================

/**
 * @brief PAR_ — Particle emitter (v10–v24, 1300–1496 bytes)
 *
 * The most complex M3 chunk type. Contains bone/material binding, emission
 * shape/rate, per-particle lifetime/velocity/color/size/rotation curves,
 * physics (drag, mass, forces), noise, collision, flipbook, variation
 * channels, spline data, LOD, trails, and splat references. Version
 * extensions add additional flags, force multipliers, UV transforms,
 * phase shift, and ribbon-on-bounce parameters.
 */
struct ParticleEmitter {

    // Identification
    u32 boneIndex;     ///< Index into BONE array
    u32 materialIndex; ///< Index into MATM material map array
    ParticleAdditionalFlag additionalFlags =
        ParticleAdditionalFlag::None; ///< Additional flags (v17+)

    // Initial Velocity
    AnimRef<f32> initialSpeed;       ///< Initial particle speed
    AnimRef<f32> initialSpeedRandom; ///< Random speed variation
    AnimRef<f32> initialYaw;         ///< Initial yaw angle
    AnimRef<f32> initialPitch;       ///< Initial pitch angle
    AnimRef<f32> initialHorizontal;  ///< Initial horizontal spread
    AnimRef<f32> initialVertical;    ///< Initial vertical spread

    // Lifetime
    AnimRef<f32> lifetime;       ///< Base particle lifetime
    AnimRef<f32> lifetimeRandom; ///< Random lifetime variation

    // Distance & Gravity
    f32 killRadius = 0.0f; ///< Kill radius (particles beyond this are destroyed)
    u32 gravityX = 0;      ///< Gravity X component (expected 0)
    u32 gravityY = 0;      ///< Gravity Y component (expected 0)
    f32 gravity = 0.0f;    ///< Gravity Z component

    // Midpoint Timing (v12+)
    f32 sizeMidTime = 0.5f;     ///< Size midpoint time (0–1, v12+)
    f32 colorMidTime = 0.5f;    ///< Color midpoint time (0–1, v12+)
    f32 alphaMidTime = 0.5f;    ///< Alpha midpoint time (0–1, v12+)
    f32 rotationMidTime = 0.5f; ///< Rotation midpoint time (0–1, v12+)

    // Hold Times (v14+)
    f32 sizeMidHoldTime = 0.0f;     ///< Size hold time at midpoint (v14+)
    f32 colorMidHoldTime = 0.0f;    ///< Color hold time at midpoint (v14+)
    f32 alphaMidHoldTime = 0.0f;    ///< Alpha hold time at midpoint (v14+)
    f32 rotationMidHoldTime = 0.0f; ///< Rotation hold time at midpoint (v14+)

    // Per-Particle Curves
    AnimRef<Vector3f> sizeAnimation;     ///< Size curve (start, mid, end)
    AnimRef<Vector3f> rotationAnimation; ///< Rotation curve (start, mid, end)
    AnimRef<ColorBGRA> colorStart;       ///< Color at birth
    AnimRef<ColorBGRA> colorMid;         ///< Color at midpoint
    AnimRef<ColorBGRA> colorEnd;         ///< Color at death

    // Physics
    f32 drag = 0.0f;               ///< Air drag coefficient
    f32 mass = 0.001f;             ///< Particle mass
    f32 massRandom = 1.0f;         ///< Random mass variation multiplier
    f32 massSizeMultiplier = 0.0f; ///< Mass–size coupling (v12+)

    // Forces
    u16 localForces = 0;                  ///< Local force channel bitmask
    u16 worldForces = 0;                  ///< World force channel bitmask
    u16 localForcesFallback = 0;          ///< Fallback local force channels
    u16 worldForcesFallback = 0;          ///< Fallback world force channels
    f32 worldForcesMassMultiplier = 1.0f; ///< World force mass multiplier (v24+)

    // Noise
    f32 noiseAmplitude = 0.0f; ///< Noise displacement amplitude
    f32 noiseFrequency = 0.0f; ///< Noise spatial frequency
    f32 noiseCoherence = 0.0f; ///< Noise temporal coherence
    f32 noiseEdge = 0.0f;      ///< Noise edge sharpness
    u32 indexPlusLength = 0;   ///< Index + length (v11+)

    // Emission
    u32 maxParticles = 0;                            ///< Maximum live particle count
    AnimRef<f32> emissionRate;                       ///< Animated emission rate (particles/sec)
    EmitterShape emitterShape = EmitterShape::Point; ///< Emission shape
    AnimRef<Vector3f> shapeOuter;                    ///< Animated outer shape dimensions
    AnimRef<Vector3f> shapeInner;                    ///< Animated inner shape dimensions
    AnimRef<f32> outerRadius;                        ///< Animated outer radius
    AnimRef<f32> innerRadius;                        ///< Animated inner radius
    std::vector<u32> shapeRegions;                   ///< Shape region indices (U32_, v14+), which mesh region from div to use

    // Randomization
    u32 velocityType = 0;                      ///< Velocity randomization type
    u32 sizeRandomEnable = 0;                  ///< Enable size randomization
    AnimRef<Vector3f> sizeRandomAnimation;     ///< Random size curve
    u32 rotationRandomEnable = 0;              ///< Enable rotation randomization
    AnimRef<Vector3f> rotationRandomAnimation; ///< Random rotation curve
    u32 colorRandomEnable = 0;                 ///< Enable color randomization
    AnimRef<ColorBGRA> colorStartRandom;       ///< Random color at birth
    AnimRef<ColorBGRA> colorMidRandom;         ///< Random color at midpoint
    AnimRef<ColorBGRA> colorEndRandom;         ///< Random color at death
    u32 alphaRandomEnable = 0;                 ///< Enable alpha randomization

    // Squirt & Flipbook
    AnimRef<u16> squirtAmount;         ///< Animated squirt burst count
    u8 flipbookStartInitIndex = 0;     ///< Flipbook start initial frame index
    u8 flipbookStartStopIndex = 0;     ///< Flipbook start stop frame index
    u8 flipbookEndInitIndex = 0;       ///< Flipbook end initial frame index
    u8 flipbookEndStopIndex = 0;       ///< Flipbook end stop frame index
    f32 flipbookMidTime = 0.0f;        ///< Flipbook midpoint time (0–1)
    u16 flipbookColumns = 0;           ///< Flipbook grid columns
    u16 flipbookRows = 0;              ///< Flipbook grid rows
    f32 flipbookColumnFraction = 0.0f; ///< Column fraction (v12+)
    f32 flipbookRowFraction = 0.0f;    ///< Row fraction (v12+)

    // Collision
    f32 bounce = 0.0f;               ///< Bounce coefficient
    f32 friction = 1.0f;             ///< Friction coefficient
    i32 collisionSpawnIndex = -1;    ///< Emitter index to spawn on collision (-1 = none)
    u32 collisionSpawnMin = 0;       ///< Minimum spawn count on collision
    u32 collisionSpawnMax = 0;       ///< Maximum spawn count on collision
    f32 collisionSpawnChance = 0.0f; ///< Spawn probability on collision
    f32 collisionSpawnEnergy = 0.0f; ///< Spawn energy transfer
    u32 collisionDieBounce = 0;      ///< Die after N bounces

    // Instance
    ParticleInstanceType instanceType = ParticleInstanceType::Billboard; ///< Visual type
    f32 tailLength = 1.0f;       ///< Tail length for Tail/Trail types
    Vector3f instanceAngle;      ///< Instance orientation angles
    f32 instanceDistance = 1.0f; ///< Instance distance (v17+)

    // Variation Channels (pitch, yaw, speed, size, alpha, color, rotation, horizontal, vertical)
    u32 pitchType = 0;                ///< Pitch variation type
    AnimRef<f32> pitchAmplitude;      ///< Pitch variation amplitude
    AnimRef<f32> pitchFrequency;      ///< Pitch variation frequency
    u32 yawType = 0;                  ///< Yaw variation type
    AnimRef<f32> yawAmplitude;        ///< Yaw variation amplitude
    AnimRef<f32> yawFrequency;        ///< Yaw variation frequency
    u32 speedType = 0;                ///< Speed variation type
    AnimRef<f32> speedAmplitude;      ///< Speed variation amplitude
    AnimRef<f32> speedFrequency;      ///< Speed variation frequency
    u32 sizeType = 0;                 ///< Size variation type
    AnimRef<f32> sizeAmplitude;       ///< Size variation amplitude
    AnimRef<f32> sizeFrequency;       ///< Size variation frequency
    u32 alphaType = 0;                ///< Alpha variation type
    AnimRef<f32> alphaAmplitude;      ///< Alpha variation amplitude
    AnimRef<f32> alphaFrequency;      ///< Alpha variation frequency
    u32 colorType = 0;                ///< Color variation type
    AnimRef<f32> colorAmplitude;      ///< Color variation amplitude
    AnimRef<f32> colorFrequency;      ///< Color variation frequency
    u32 rotationType = 0;             ///< Rotation variation type
    AnimRef<f32> rotationAmplitude;   ///< Rotation variation amplitude
    AnimRef<f32> rotationFrequency;   ///< Rotation variation frequency
    u32 horizontalType = 0;           ///< Horizontal variation type
    AnimRef<f32> horizontalAmplitude; ///< Horizontal variation amplitude
    AnimRef<f32> horizontalFrequency; ///< Horizontal variation frequency
    u32 verticalType = 0;             ///< Vertical variation type
    AnimRef<f32> verticalAmplitude;   ///< Vertical variation amplitude
    AnimRef<f32> verticalFrequency;   ///< Vertical variation frequency;

    // Parent Velocity & Phase
    AnimRef<f32> particleVelocity; ///< Animated parent velocity influence
    AnimRef<f32> phaseShift;       ///< Animated phase shift (v22+)

    // Flags
    ParticleFlag flags = ParticleFlag::None;                         ///< Main particle flags
    ParticleRotationFlag rotationFlags = ParticleRotationFlag::None; ///< Rotation flags (v18+)

    // Smoothing (v14+)
    InterpolationMode colorSmoothing = InterpolationMode::Linear; ///< Color interpolation mode
    InterpolationMode sizeSmoothing = InterpolationMode::Linear;  ///< Size interpolation mode
    InterpolationMode rotationSmoothing =
        InterpolationMode::Linear; ///< Rotation interpolation mode

    // UV Screen Space (v17+)
    AnimRef<f32> alphaThreshold; ///< Animated alpha threshold
    AnimRef<Vector2f> uvOffset;  ///< Animated UV offset
    AnimRef<Vector3f> uvAngle;   ///< Animated UV rotation angles
    AnimRef<Vector2f> uvTiling;  ///< Animated UV tiling

    // Spline
    std::vector<AnimRef<Vector3f>> splineLineData; ///< Spline control points (SVC3)

    // Wind & LOD
    f32 windMultiplier = 0.0f; ///< Wind influence multiplier
    u32 lodReduce = 2;         ///< LOD reduction level
    u32 lodCut = 0;            ///< LOD cut-off level

    // Bounds
    AnimRef<f32> lowerBound; ///< Animated lower bound
    AnimRef<f32> upperBound; ///< Animated upper bound

    // Trails
    i32 trailLinkIndex = -1;        ///< Linked trail emitter index (-1 = none) aka another particle emitter
    f32 trailChance = 0.0f;         ///< Trail spawn probability
    AnimRef<f32> trailEmissionRate; ///< Animated trail emission rate

    // Splat
    i32 splatProjectionIndex = -1; ///< Linked projector index (-1 = none)
    f32 splatChance = 0.0f;        ///< Splat spawn probability

    // References
    std::vector<std::string> modelPaths; ///< Model particle paths (SCHR)
    std::vector<u32> copyIndices;        ///< Emitter copy indices (U32_)

    // Ribbon-on-bounce (v23+)
    f32 spawnRibbonOnBounceChance = 0.0f; ///< Ribbon spawn probability on bounce (v23+)
    i32 ribbonLinkIndex = -1;             ///< Index into RIB_ array (-1 = none, v23+)

    /// Deprecated fields (cannot be migrated to canonical fields)
    struct {
        f32 noiseSmoothness = 0.0f; ///< Deprecated noise smoothness (removed after v11)
    } deprecated;
    M3_DEFINE_VERSION_ACCESSORS()
};

/**
 * @brief PARC — Particle emitter copy (v0, 40 bytes)
 *
 * Lightweight copy of a particle emitter with overridden emission rate,
 * squirt amount, and bone index. References the original PAR_ via
 * Model.copyIndices.
 */
struct ParticleEmitterCopy {
    AnimRef<f32> emissionRate;            ///< Overridden emission rate
    AnimRef<u16> squirtAmount;            ///< Overridden squirt burst count
    u32 boneIndex;                        ///< Index into BONE array
    M3_DEFINE_VERSION_ACCESSORS()
};

/**
 * @brief SRIB — Spline ribbon segment (v0, 272 bytes)
 *
 * Defines a single segment of a spline-based ribbon with emission
 * offset/vector, velocity, bone binding, and pitch/yaw/velocity
 * variation channels.
 */
struct SplineRibbon {
    Vector3f emissionOffset;              ///< Emission point offset from bone
    Vector3f emissionVector;              ///< Emission direction vector
    AnimRef<f32> velocity;                ///< Animated base velocity
    u32 reserved = 0;                     ///< Reserved (always 0)
    u32 boneIndex = 0;                    ///< Index into BONE array
    AnimRef<f32> velocityBaseFactor;      ///< Animated base velocity factor
    AnimRef<f32> velocityEndFactor;       ///< Animated end velocity factor
    u32 yawType;                          ///< Yaw variation type
    AnimRef<f32> yawAmplitude;            ///< Yaw variation amplitude
    AnimRef<f32> yawFrequency;            ///< Yaw variation frequency
    u32 pitchType;                        ///< Pitch variation type
    AnimRef<f32> pitchAmplitude;          ///< Pitch variation amplitude
    AnimRef<f32> pitchFrequency;          ///< Pitch variation frequency
    u32 velocityType;                     ///< Velocity variation type
    AnimRef<f32> velocityAmplitude;       ///< Velocity variation amplitude
    AnimRef<f32> velocityFrequency;       ///< Velocity variation frequency
    AnimRef<f32> yaw;                     ///< Animated yaw angle
    AnimRef<f32> pitch;                   ///< Animated pitch angle
    f32 emissionVectorNormFactor = 0.0f;  ///< Precomputed ≈ 0.01 / |emissionVector|
    f32 velocityNormFactor = 0.0f;        ///< Precomputed ≈ 0.01 / velocity.initValue
    M3_DEFINE_VERSION_ACCESSORS()
};

/**
 * @brief RIB_ — Ribbon emitter (v4–v9, 744–760 bytes)
 *
 * Ribbon strip effect with per-particle lifetime, velocity, color/size
 * curves, physics, noise, spline segments, variation channels, and
 * smoothing/collision settings. Shares many fields with ParticleEmitter.
 */
struct RibbonEmitter {

    // Identification
    u16 boneIndex = 0;         ///< Primary bone index
    u16 boneIndexFallback = 0; ///< Fallback bone index
    u32 materialIndex;         ///< Index into MATM material map array
    RibbonAdditionalFlag additionalFlags = RibbonAdditionalFlag::None; ///< Additional flags (v8+)

    // Initial Velocity
    AnimRef<f32> initialSpeed;       ///< Initial ribbon segment speed
    AnimRef<f32> initialSpeedRandom; ///< Random speed variation
    AnimRef<f32> initialYaw;         ///< Initial yaw angle
    AnimRef<f32> initialPitch;       ///< Initial pitch angle
    AnimRef<f32> initialHorizontal;  ///< Initial horizontal spread
    AnimRef<f32> initialVertical;    ///< Initial vertical spread

    // Lifetime
    AnimRef<f32> lifetime;       ///< Base segment lifetime
    AnimRef<f32> lifetimeRandom; ///< Random lifetime variation
    u32 killRadius = 0;          ///< Kill radius

    // Gravity
    f32 gravityX = 0.0f; ///< Gravity X component
    f32 gravityY = 0.0f; ///< Gravity Y component
    f32 gravity;         ///< Gravity Z component

    // Midpoint timing
    f32 sizeMidTime;     ///< Size midpoint time (0–1)
    f32 colorMidTime;    ///< Color midpoint time (0–1)
    f32 alphaMidTime;    ///< Alpha midpoint time (0–1)
    f32 rotationMidTime; ///< Rotation midpoint time (0–1)

    // Hold timing
    f32 sizeMidHoldTime;     ///< Size hold time at midpoint
    f32 colorMidHoldTime;    ///< Color hold time at midpoint
    f32 alphaMidHoldTime;    ///< Alpha hold time at midpoint
    f32 rotationMidHoldTime; ///< Rotation hold time at midpoint

    // Curves
    AnimRef<Vector3f> sizeAnimation;     ///< Size curve (start, mid, end)
    AnimRef<Vector3f> rotationAnimation; ///< Rotation curve (start, mid, end)
    AnimRef<ColorBGRA> colorStart;       ///< Color at birth
    AnimRef<ColorBGRA> colorMid;         ///< Color at midpoint
    AnimRef<ColorBGRA> colorEnd;         ///< Color at death

    // Physics
    f32 drag;                             ///< Air drag coefficient
    f32 mass;                             ///< Segment mass
    f32 massRandom;                       ///< Random mass variation
    f32 massSizeMultiplier;               ///< Mass–size coupling
    u16 localForces = 0;                  ///< Local force channel bitmask
    u16 worldForces = 0;                  ///< World force channel bitmask
    u16 localForcesFallback = 0;          ///< Fallback local force channels
    u16 worldForcesFallback = 0;          ///< Fallback world force channels
    f32 worldForcesMassMultiplier = 0.0f; ///< World force mass multiplier

    // Noise
    f32 noiseAmplitude;      ///< Noise displacement amplitude
    f32 noiseFrequency;      ///< Noise spatial frequency
    f32 noiseCoherence;      ///< Noise temporal coherence
    f32 noiseEdge;           ///< Noise edge sharpness
    u32 indexPlusLength = 1; ///< Index + length

    // Ribbon shape
    u32 emitterShape;                              ///< Emitter shape type
    RibbonType ribbonType = RibbonType::Billboard; ///< Ribbon cross-section type
    f32 divisions = 0.0f;                          ///< Number of ribbon divisions
    u32 edges;                                     ///< Number of cross-section edges
    f32 innerRadius;                               ///< Inner radius
    AnimRef<f32> maxLength;                        ///< Animated maximum ribbon length

    // References
    std::vector<SplineRibbon> splineRibbons; ///< Spline ribbon segments (SRIB)
    AnimRef<u32> active;                     ///< Animated active state

    // Flags
    RibbonFlag flags = RibbonFlag::None; ///< Ribbon emitter flags

    // Smoothing
    InterpolationMode sizeSmoothing = InterpolationMode::Linear;  ///< Size interpolation mode
    InterpolationMode colorSmoothing = InterpolationMode::Linear; ///< Color interpolation mode

    // Collision & LOD
    f32 friction;  ///< Friction coefficient
    f32 bounce;    ///< Bounce coefficient
    u32 lodReduce; ///< LOD reduction level
    u32 lodCut;    ///< LOD cut-off level

    // Variation channels
    u32 yawType;                 ///< Yaw variation type
    AnimRef<f32> yawAmplitude;   ///< Yaw variation amplitude
    AnimRef<f32> yawFrequency;   ///< Yaw variation frequency
    u32 pitchType;               ///< Pitch variation type
    AnimRef<f32> pitchAmplitude; ///< Pitch variation amplitude
    AnimRef<f32> pitchFrequency; ///< Pitch variation frequency
    u32 speedType;               ///< Speed variation type
    AnimRef<f32> speedAmplitude; ///< Speed variation amplitude
    AnimRef<f32> speedFrequency; ///< Speed variation frequency
    u32 sizeType;                ///< Size variation type
    AnimRef<f32> sizeAmplitude;  ///< Size variation amplitude
    AnimRef<f32> sizeFrequency;  ///< Size variation frequency
    u32 alphaType;               ///< Alpha variation type
    AnimRef<f32> alphaAmplitude; ///< Alpha variation amplitude
    AnimRef<f32> alphaFrequency; ///< Alpha variation frequency

    // Parent velocity & phase
    AnimRef<f32> particleVelocity; ///< Animated parent velocity influence
    AnimRef<f32> overlay;          ///< Animated overlay effect

    /// Deprecated fields
    struct {
        i32 unknown3fbae7d6 = 0; ///< Removed after v6
    } deprecated;
    M3_DEFINE_VERSION_ACCESSORS()
};

/**
 * @brief PROJ — Projector / decal (v0–v5, 388 bytes)
 *
 * Projects a material onto scene geometry with animated offset, orientation,
 * field of view, aspect ratio, clipping planes, alpha lifecycle, and
 * attenuation distance.
 */
struct Projector {
    ProjectionType projectionType;             ///< Projection type (ortho/perspective)
    u32 bone;                                  ///< Index into BONE array
    u32 materialReferenceIndex;                ///< Index into MATM material map
    AnimRef<Vector3f> offset;                  ///< Animated position offset
    AnimRef<f32> pitch;                        ///< Animated pitch angle
    AnimRef<f32> yaw;                          ///< Animated yaw angle
    AnimRef<f32> roll;                         ///< Animated roll angle
    AnimRef<f32> fieldOfView;                  ///< Animated field of view
    AnimRef<f32> aspectRatio;                  ///< Animated aspect ratio
    AnimRef<f32> near;                         ///< Animated near clip plane
    AnimRef<f32> far;                          ///< Animated far clip plane
    AnimRef<f32> boxOffsetZBottom;             ///< Animated box Z bottom offset
    AnimRef<f32> boxOffsetZTop;                ///< Animated box Z top offset
    AnimRef<f32> boxOffsetXLeft;               ///< Animated box X left offset
    AnimRef<f32> boxOffsetXRight;              ///< Animated box X right offset
    AnimRef<f32> boxOffsetYFront;              ///< Animated box Y front offset
    AnimRef<f32> boxOffsetYBack;               ///< Animated box Y back offset
    f32 falloff;                               ///< Projection falloff distance
    f32 alphaInit;                             ///< Alpha at creation
    f32 alphaMid;                              ///< Alpha at midpoint
    f32 alphaEnd;                              ///< Alpha at end
    f32 lifetimeAttack;                        ///< Attack phase duration
    f32 lifetimeAttackTo;                      ///< Attack target time
    f32 lifetimeHold;                          ///< Hold phase duration
    f32 lifetimeHoldTo;                        ///< Hold target time
    f32 lifetimeDecay;                         ///< Decay phase duration
    f32 lifetimeDecayTo;                       ///< Decay target time
    f32 attenuationDistance;                   ///< Distance-based attenuation
    AnimRef<u32> active;                       ///< Animated active state
    u32 layer;                                 ///< Render layer
    u32 lodReduce;                             ///< LOD reduction level
    u32 lodCut;                                ///< LOD cut-off level
    ProjectorFlag flags = ProjectorFlag::None; ///< Projector flags
    M3_DEFINE_VERSION_ACCESSORS()
};

} // namespace m3
} // namespace whiteout
