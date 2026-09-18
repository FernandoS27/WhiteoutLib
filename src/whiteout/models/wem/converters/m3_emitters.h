// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file m3_emitters.h
 * @brief `PAR_`, `PARC` and `RIB_` <-> the StarCraft II emitter payloads (§10.9).
 *
 * Every AnimRef the three records carry is listed once, in the tables below,
 * beside the payload field it rests in and the `EmitterProperty` it keys as. The
 * import, the export, the channel declarations and the wiring all walk those
 * tables, so a property cannot cross in one direction and be forgotten in the
 * other.
 *
 * The records' plain fields cross verbatim, in StarCraft II's own basis — the
 * node carries the placement WEM rebased, and the system reads its parameters in
 * its bone's frame (see `emitters.h`). Only the cross-references change shape:
 * a material index is a slot (they are the same number, `toM3` writes one `MATM`
 * entry per slot), and an emitter, ribbon or bone index is a node.
 */

#include <vector>

#include <whiteout/models/m3/structures.h>
#include <whiteout/models/wem/nodes/node.h>

namespace whiteout {
namespace models {
namespace wem {
namespace m3_emitters {

// ============================================================================
// The AnimRef tables: (property, M3 field, payload field)
// ============================================================================

#define WHITEOUT_SC2_PARTICLE_REFS(X)                                                              \
    X(InitialSpeed, initialSpeed, initialSpeed)                                                    \
    X(InitialSpeedRandom, initialSpeedRandom, initialSpeedRandom)                                  \
    X(InitialYaw, initialYaw, initialYaw)                                                          \
    X(InitialPitch, initialPitch, initialPitch)                                                    \
    X(InitialHorizontal, initialHorizontal, initialHorizontal)                                     \
    X(InitialVertical, initialVertical, initialVertical)                                           \
    X(Lifetime, lifetime, lifetime)                                                                \
    X(LifetimeRandom, lifetimeRandom, lifetimeRandom)                                              \
    X(SizeAnimation, sizeAnimation, sizeAnimation)                                                 \
    X(RotationAnimation, rotationAnimation, rotationAnimation)                                     \
    X(ColorStart, colorStart, colorStart)                                                          \
    X(ColorMid, colorMid, colorMid)                                                                \
    X(ColorEnd, colorEnd, colorEnd)                                                                \
    X(EmissionRate, emissionRate, emissionRate)                                                    \
    X(ShapeOuter, shapeOuter, shapeOuter)                                                          \
    X(ShapeInner, shapeInner, shapeInner)                                                          \
    X(OuterRadius, outerRadius, outerRadius)                                                       \
    X(InnerRadius, innerRadius, innerRadius)                                                       \
    X(SizeRandomAnimation, sizeRandomAnimation, sizeRandomAnimation)                               \
    X(RotationRandomAnimation, rotationRandomAnimation, rotationRandomAnimation)                   \
    X(ColorStartRandom, colorStartRandom, colorStartRandom)                                        \
    X(ColorMidRandom, colorMidRandom, colorMidRandom)                                              \
    X(ColorEndRandom, colorEndRandom, colorEndRandom)                                              \
    X(SquirtAmount, squirtAmount, squirtAmount)                                                    \
    X(PitchAmplitude, pitchAmplitude, pitch.amplitude)                                             \
    X(PitchFrequency, pitchFrequency, pitch.frequency)                                             \
    X(YawAmplitude, yawAmplitude, yaw.amplitude)                                                   \
    X(YawFrequency, yawFrequency, yaw.frequency)                                                   \
    X(SpeedAmplitude, speedAmplitude, speed.amplitude)                                             \
    X(SpeedFrequency, speedFrequency, speed.frequency)                                             \
    X(SizeAmplitude, sizeAmplitude, size.amplitude)                                                \
    X(SizeFrequency, sizeFrequency, size.frequency)                                                \
    X(AlphaAmplitude, alphaAmplitude, alpha.amplitude)                                             \
    X(AlphaFrequency, alphaFrequency, alpha.frequency)                                             \
    X(ColorAmplitude, colorAmplitude, color.amplitude)                                             \
    X(ColorFrequency, colorFrequency, color.frequency)                                             \
    X(RotationAmplitude, rotationAmplitude, rotation.amplitude)                                    \
    X(RotationFrequency, rotationFrequency, rotation.frequency)                                    \
    X(HorizontalAmplitude, horizontalAmplitude, horizontal.amplitude)                              \
    X(HorizontalFrequency, horizontalFrequency, horizontal.frequency)                              \
    X(VerticalAmplitude, verticalAmplitude, vertical.amplitude)                                    \
    X(VerticalFrequency, verticalFrequency, vertical.frequency)                                    \
    X(ParticleVelocity, particleVelocity, particleVelocity)                                        \
    X(PhaseShift, phaseShift, phaseShift)                                                          \
    X(AlphaThreshold, alphaThreshold, alphaThreshold)                                              \
    X(UvOffset, uvOffset, uvOffset)                                                                \
    X(UvAngle, uvAngle, uvAngle)                                                                   \
    X(UvTiling, uvTiling, uvTiling)                                                                \
    X(LowerBound, lowerBound, lowerBound)                                                          \
    X(UpperBound, upperBound, upperBound)                                                          \
    X(TrailEmissionRate, trailEmissionRate, trailEmissionRate)

/// A `PARC` keys two: the same properties as its source's, on its own node.
#define WHITEOUT_SC2_COPY_REFS(X)                                                                  \
    X(EmissionRate, emissionRate, emissionRate)                                                    \
    X(SquirtAmount, squirtAmount, squirtAmount)

#define WHITEOUT_SC2_RIBBON_REFS(X)                                                                \
    X(InitialSpeed, initialSpeed, initialSpeed)                                                    \
    X(InitialSpeedRandom, initialSpeedRandom, initialSpeedRandom)                                  \
    X(InitialYaw, initialYaw, initialYaw)                                                          \
    X(InitialPitch, initialPitch, initialPitch)                                                    \
    X(InitialHorizontal, initialHorizontal, initialHorizontal)                                     \
    X(InitialVertical, initialVertical, initialVertical)                                           \
    X(Lifetime, lifetime, lifetime)                                                                \
    X(LifetimeRandom, lifetimeRandom, lifetimeRandom)                                              \
    X(SizeAnimation, sizeAnimation, sizeAnimation)                                                 \
    X(RotationAnimation, rotationAnimation, rotationAnimation)                                     \
    X(ColorStart, colorStart, colorStart)                                                          \
    X(ColorMid, colorMid, colorMid)                                                                \
    X(ColorEnd, colorEnd, colorEnd)                                                                \
    X(MaxLength, maxLength, maxLength)                                                             \
    X(Active, active, active)                                                                      \
    X(YawAmplitude, yawAmplitude, yaw.amplitude)                                                   \
    X(YawFrequency, yawFrequency, yaw.frequency)                                                   \
    X(PitchAmplitude, pitchAmplitude, pitch.amplitude)                                             \
    X(PitchFrequency, pitchFrequency, pitch.frequency)                                             \
    X(SpeedAmplitude, speedAmplitude, speed.amplitude)                                             \
    X(SpeedFrequency, speedFrequency, speed.frequency)                                             \
    X(SizeAmplitude, sizeAmplitude, size.amplitude)                                                \
    X(SizeFrequency, sizeFrequency, size.frequency)                                                \
    X(AlphaAmplitude, alphaAmplitude, alpha.amplitude)                                             \
    X(AlphaFrequency, alphaFrequency, alpha.frequency)                                             \
    X(ParticleVelocity, particleVelocity, particleVelocity)                                        \
    X(Overlay, overlay, overlay)

/// One `SRIB`'s, keyed per element of the ribbon's spline.
#define WHITEOUT_SC2_SPLINE_REFS(X)                                                                \
    X(SplineVelocity, velocity, velocity)                                                          \
    X(SplineVelocityBaseFactor, velocityBaseFactor, velocityBaseFactor)                            \
    X(SplineVelocityEndFactor, velocityEndFactor, velocityEndFactor)                               \
    X(SplineYawAmplitude, yawAmplitude, yawVariation.amplitude)                                    \
    X(SplineYawFrequency, yawFrequency, yawVariation.frequency)                                    \
    X(SplinePitchAmplitude, pitchAmplitude, pitchVariation.amplitude)                              \
    X(SplinePitchFrequency, pitchFrequency, pitchVariation.frequency)                              \
    X(SplineVelocityAmplitude, velocityAmplitude, velocityVariation.amplitude)                     \
    X(SplineVelocityFrequency, velocityFrequency, velocityVariation.frequency)                     \
    X(SplineYaw, yaw, yaw)                                                                         \
    X(SplinePitch, pitch, pitch)

/// Every AnimRef of a `PAR_` with the `EmitterProperty` sub it keys as:
/// `f(u32 sub, AnimRef<T>&)`. @p record may be const.
template <class Record, class F>
void ForEachParticleRef(Record& record, F&& f) {
#define WHITEOUT_VISIT(property, m3Field, wemField)                                                \
    f(EmitterPropertySub(static_cast<u32>(Sc2ParticleProperty::property)), record.m3Field);
    WHITEOUT_SC2_PARTICLE_REFS(WHITEOUT_VISIT)
#undef WHITEOUT_VISIT
    for (std::size_t i = 0; i < record.splineLineData.size(); ++i) {
        f(EmitterPropertySub(static_cast<u32>(Sc2ParticleProperty::SplinePoint),
                             static_cast<u32>(i)),
          record.splineLineData[i]);
    }
}

/// A `PARC`'s two.
template <class Record, class F>
void ForEachCopyRef(Record& record, F&& f) {
#define WHITEOUT_VISIT(property, m3Field, wemField)                                                \
    f(EmitterPropertySub(static_cast<u32>(Sc2ParticleProperty::property)), record.m3Field);
    WHITEOUT_SC2_COPY_REFS(WHITEOUT_VISIT)
#undef WHITEOUT_VISIT
}

/// A `RIB_`'s, its spline's included.
template <class Record, class F>
void ForEachRibbonRef(Record& record, F&& f) {
#define WHITEOUT_VISIT(property, m3Field, wemField)                                                \
    f(EmitterPropertySub(static_cast<u32>(Sc2RibbonProperty::property)), record.m3Field);
    WHITEOUT_SC2_RIBBON_REFS(WHITEOUT_VISIT)
#undef WHITEOUT_VISIT
    for (std::size_t i = 0; i < record.splineRibbons.size(); ++i) {
        auto& point = record.splineRibbons[i];
#define WHITEOUT_VISIT(property, m3Field, wemField)                                                \
    f(EmitterPropertySub(static_cast<u32>(Sc2RibbonProperty::property), static_cast<u32>(i)),      \
      point.m3Field);
        WHITEOUT_SC2_SPLINE_REFS(WHITEOUT_VISIT)
#undef WHITEOUT_VISIT
    }
}

// ============================================================================
// Records <-> payloads
// ============================================================================

/// Record index -> node, for the import. An index past its array, or the
/// format's -1, is no node.
struct ImportLinks {
    u32 boneCount = 0;
    u32 particleBase = 0;
    u32 particleCount = 0;
    u32 ribbonBase = 0;
    u32 ribbonCount = 0;

    u32 bone(u32 index) const {
        return index < boneCount ? index : kInvalidNode;
    }
    u32 particle(i32 index) const {
        return index >= 0 && static_cast<u32>(index) < particleCount
                   ? particleBase + static_cast<u32>(index)
                   : kInvalidNode;
    }
    u32 ribbon(i32 index) const {
        return index >= 0 && static_cast<u32>(index) < ribbonCount
                   ? ribbonBase + static_cast<u32>(index)
                   : kInvalidNode;
    }
};

/// Node -> record index, for the export. Each vector is parallel to
/// `Model::nodes`: the `PAR_` or `RIB_` a node became, and the bone its records
/// ride, `kInvalidIndex` where there is none.
struct ExportLinks {
    const std::vector<u32>* particleOf = nullptr;
    const std::vector<u32>* ribbonOf = nullptr;
    const std::vector<u32>* boneOf = nullptr;

    i32 particle(u32 node) const;
    i32 ribbon(u32 node) const;
    u32 bone(u32 node) const;
};

Sc2ParticleEmitterPayload ImportParticle(const m3::ParticleEmitter& record,
                                         const ImportLinks& links);
/// @p source is the node of the `PAR_` the copy belongs to.
Sc2ParticleEmitterPayload ImportCopy(const m3::ParticleEmitterCopy& record, u32 source);
Sc2RibbonEmitterPayload ImportRibbon(const m3::RibbonEmitter& record, const ImportLinks& links);

/// The record, less what only the caller knows: `boneIndex`, and `copyIndices`
/// on a `PAR_`. Every AnimRef rests at the payload's values with no animId; the
/// animation export binds them.
m3::ParticleEmitter ExportParticle(const Sc2ParticleEmitterPayload& payload,
                                   const ExportLinks& links);
m3::ParticleEmitterCopy ExportCopy(const Sc2ParticleEmitterPayload& payload);
m3::RibbonEmitter ExportRibbon(const Sc2RibbonEmitterPayload& payload, const ExportLinks& links);

} // namespace m3_emitters
} // namespace wem
} // namespace models
} // namespace whiteout
