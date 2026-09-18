// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "m3_emitters.h"

namespace whiteout {
namespace models {
namespace wem {
namespace m3_emitters {

namespace {

// ---- one AnimRef value <-> one payload value ---------------------------------
//
// The same number both ways except where the channel convention differs from
// the file's: a colour is RGBA floats here and BGRA bytes there, and a squirt
// count widens from `u16`.

template <class T>
T ToWem(const T& value) {
    return value;
}

Vector4f ToWem(const m3::ColorBGRA& color) {
    return Vector4f{static_cast<f32>(color.r) / 255.0f, static_cast<f32>(color.g) / 255.0f,
                    static_cast<f32>(color.b) / 255.0f, static_cast<f32>(color.a) / 255.0f};
}

u32 ToWem(u16 value) {
    return value;
}

template <class T>
void ToM3(const T& value, T& out) {
    out = value;
}

void ToM3(const Vector4f& value, m3::ColorBGRA& out) {
    const auto byteOf = [](f32 v) {
        const f32 scaled = v * 255.0f;
        return static_cast<u8>(scaled <= 0.0f ? 0.0f : scaled >= 255.0f ? 255.0f : scaled + 0.5f);
    };
    out.r = byteOf(value.x);
    out.g = byteOf(value.y);
    out.b = byteOf(value.z);
    out.a = byteOf(value.w);
}

void ToM3(u32 value, u16& out) {
    out = static_cast<u16>(value);
}

template <class T, class U>
void Import(const m3::AnimRef<T>& ref, Sc2Property<U>& out) {
    out.initValue = ToWem(ref.initValue);
    out.nullValue = ToWem(ref.nullValue);
}

template <class T, class U>
void Export(const Sc2Property<U>& value, m3::AnimRef<T>& ref) {
    ToM3(value.initValue, ref.initValue);
    ToM3(value.nullValue, ref.nullValue);
}

/// The whole AnimRef table of each record, in one direction or the other.
template <class Record, class Payload, class Step>
void ZipParticle(Record& record, Payload& payload, Step step) {
#define WHITEOUT_ZIP(property, m3Field, wemField) step(record.m3Field, payload.wemField);
    WHITEOUT_SC2_PARTICLE_REFS(WHITEOUT_ZIP)
#undef WHITEOUT_ZIP
}

template <class Record, class Payload, class Step>
void ZipCopy(Record& record, Payload& payload, Step step) {
#define WHITEOUT_ZIP(property, m3Field, wemField) step(record.m3Field, payload.wemField);
    WHITEOUT_SC2_COPY_REFS(WHITEOUT_ZIP)
#undef WHITEOUT_ZIP
}

template <class Record, class Payload, class Step>
void ZipRibbon(Record& record, Payload& payload, Step step) {
#define WHITEOUT_ZIP(property, m3Field, wemField) step(record.m3Field, payload.wemField);
    WHITEOUT_SC2_RIBBON_REFS(WHITEOUT_ZIP)
#undef WHITEOUT_ZIP
}

template <class Record, class Payload, class Step>
void ZipSpline(Record& record, Payload& payload, Step step) {
#define WHITEOUT_ZIP(property, m3Field, wemField) step(record.m3Field, payload.wemField);
    WHITEOUT_SC2_SPLINE_REFS(WHITEOUT_ZIP)
#undef WHITEOUT_ZIP
}

const auto kImport = [](const auto& ref, auto& value) { Import(ref, value); };
const auto kExport = [](auto& ref, const auto& value) { Export(value, ref); };

} // namespace

i32 ExportLinks::particle(u32 node) const {
    if (node == kInvalidNode || particleOf == nullptr || node >= particleOf->size() ||
        (*particleOf)[node] == kInvalidIndex) {
        return -1;
    }
    return static_cast<i32>((*particleOf)[node]);
}

i32 ExportLinks::ribbon(u32 node) const {
    if (node == kInvalidNode || ribbonOf == nullptr || node >= ribbonOf->size() ||
        (*ribbonOf)[node] == kInvalidIndex) {
        return -1;
    }
    return static_cast<i32>((*ribbonOf)[node]);
}

u32 ExportLinks::bone(u32 node) const {
    if (node == kInvalidNode || boneOf == nullptr || node >= boneOf->size() ||
        (*boneOf)[node] == kInvalidIndex) {
        return 0;
    }
    return (*boneOf)[node];
}

// ============================================================================
// Import
// ============================================================================

Sc2ParticleEmitterPayload ImportParticle(const m3::ParticleEmitter& p, const ImportLinks& links) {
    Sc2ParticleEmitterPayload w;
    ZipParticle(p, w, kImport);
    w.splinePoints.resize(p.splineLineData.size());
    for (std::size_t i = 0; i < p.splineLineData.size(); ++i) {
        Import(p.splineLineData[i], w.splinePoints[i]);
    }

    w.materialSlot = p.materialIndex;
    w.additionalFlags = static_cast<u32>(p.additionalFlags);
    w.killRadius = p.killRadius;
    w.gravityX = p.gravityX;
    w.gravityY = p.gravityY;
    w.gravity = p.gravity;
    w.sizeMidTime = p.sizeMidTime;
    w.colorMidTime = p.colorMidTime;
    w.alphaMidTime = p.alphaMidTime;
    w.rotationMidTime = p.rotationMidTime;
    w.sizeMidHoldTime = p.sizeMidHoldTime;
    w.colorMidHoldTime = p.colorMidHoldTime;
    w.alphaMidHoldTime = p.alphaMidHoldTime;
    w.rotationMidHoldTime = p.rotationMidHoldTime;
    w.drag = p.drag;
    w.mass = p.mass;
    w.massRandom = p.massRandom;
    w.massSizeMultiplier = p.massSizeMultiplier;
    w.localForces = p.localForces;
    w.worldForces = p.worldForces;
    w.localForcesFallback = p.localForcesFallback;
    w.worldForcesFallback = p.worldForcesFallback;
    w.worldForcesMassMultiplier = p.worldForcesMassMultiplier;
    w.noiseAmplitude = p.noiseAmplitude;
    w.noiseFrequency = p.noiseFrequency;
    w.noiseCoherence = p.noiseCoherence;
    w.noiseEdge = p.noiseEdge;
    w.indexPlusLength = p.indexPlusLength;
    w.maxParticles = p.maxParticles;
    w.emitterShape = static_cast<Sc2EmitterShape>(p.emitterShape);
    w.shapeSections = p.shapeRegions;
    w.velocityType = p.velocityType;
    w.sizeRandomEnable = p.sizeRandomEnable;
    w.rotationRandomEnable = p.rotationRandomEnable;
    w.colorRandomEnable = p.colorRandomEnable;
    w.alphaRandomEnable = p.alphaRandomEnable;
    w.flipbookStartInitIndex = p.flipbookStartInitIndex;
    w.flipbookStartStopIndex = p.flipbookStartStopIndex;
    w.flipbookEndInitIndex = p.flipbookEndInitIndex;
    w.flipbookEndStopIndex = p.flipbookEndStopIndex;
    w.flipbookMidTime = p.flipbookMidTime;
    w.flipbookColumns = p.flipbookColumns;
    w.flipbookRows = p.flipbookRows;
    w.flipbookColumnFraction = p.flipbookColumnFraction;
    w.flipbookRowFraction = p.flipbookRowFraction;
    w.bounce = p.bounce;
    w.friction = p.friction;
    w.collisionSpawn = links.particle(p.collisionSpawnIndex);
    w.collisionSpawnMin = p.collisionSpawnMin;
    w.collisionSpawnMax = p.collisionSpawnMax;
    w.collisionSpawnChance = p.collisionSpawnChance;
    w.collisionSpawnEnergy = p.collisionSpawnEnergy;
    w.collisionDieBounce = p.collisionDieBounce;
    w.instanceType = static_cast<Sc2ParticleInstance>(p.instanceType);
    w.tailLength = p.tailLength;
    w.instanceAngle = p.instanceAngle;
    w.instanceDistance = p.instanceDistance;
    w.pitch.type = p.pitchType;
    w.yaw.type = p.yawType;
    w.speed.type = p.speedType;
    w.size.type = p.sizeType;
    w.alpha.type = p.alphaType;
    w.color.type = p.colorType;
    w.rotation.type = p.rotationType;
    w.horizontal.type = p.horizontalType;
    w.vertical.type = p.verticalType;
    w.flags = static_cast<u32>(p.flags);
    w.rotationFlags = static_cast<u32>(p.rotationFlags);
    w.colorSmoothing = static_cast<Sc2Smoothing>(p.colorSmoothing);
    w.sizeSmoothing = static_cast<Sc2Smoothing>(p.sizeSmoothing);
    w.rotationSmoothing = static_cast<Sc2Smoothing>(p.rotationSmoothing);
    w.windMultiplier = p.windMultiplier;
    w.lodReduce = p.lodReduce;
    w.lodCut = p.lodCut;
    w.trailLink = links.particle(p.trailLinkIndex);
    w.trailChance = p.trailChance;
    w.splatProjectionIndex = p.splatProjectionIndex;
    w.splatChance = p.splatChance;
    w.models.reserve(p.modelPaths.size());
    for (const std::string& path : p.modelPaths) {
        AssetKey key;
        // A shipped `.m3` string carries its terminator inside the string.
        key.path = path.substr(0, path.find('\0'));
        w.models.push_back(std::move(key));
    }
    w.spawnRibbonOnBounceChance = p.spawnRibbonOnBounceChance;
    w.ribbonLink = links.ribbon(p.ribbonLinkIndex);
    w.noiseSmoothness = p.deprecated.noiseSmoothness;
    return w;
}

Sc2ParticleEmitterPayload ImportCopy(const m3::ParticleEmitterCopy& record, u32 source) {
    Sc2ParticleEmitterPayload w;
    w.copyOf = source;
    ZipCopy(record, w, kImport);
    return w;
}

Sc2RibbonEmitterPayload ImportRibbon(const m3::RibbonEmitter& r, const ImportLinks& links) {
    Sc2RibbonEmitterPayload w;
    ZipRibbon(r, w, kImport);
    w.splinePoints.resize(r.splineRibbons.size());
    for (std::size_t i = 0; i < r.splineRibbons.size(); ++i) {
        const m3::SplineRibbon& source = r.splineRibbons[i];
        Sc2RibbonSplinePoint& point = w.splinePoints[i];
        ZipSpline(source, point, kImport);
        point.node = links.bone(source.boneIndex);
        point.emissionOffset = source.emissionOffset;
        point.emissionVector = source.emissionVector;
        point.reserved = source.reserved;
        point.yawVariation.type = source.yawType;
        point.pitchVariation.type = source.pitchType;
        point.velocityVariation.type = source.velocityType;
        point.emissionVectorNormFactor = source.emissionVectorNormFactor;
        point.velocityNormFactor = source.velocityNormFactor;
    }

    w.materialSlot = r.materialIndex;
    w.additionalFlags = static_cast<u32>(r.additionalFlags);
    w.killRadius = r.killRadius;
    w.gravityX = r.gravityX;
    w.gravityY = r.gravityY;
    w.gravity = r.gravity;
    w.sizeMidTime = r.sizeMidTime;
    w.colorMidTime = r.colorMidTime;
    w.alphaMidTime = r.alphaMidTime;
    w.rotationMidTime = r.rotationMidTime;
    w.sizeMidHoldTime = r.sizeMidHoldTime;
    w.colorMidHoldTime = r.colorMidHoldTime;
    w.alphaMidHoldTime = r.alphaMidHoldTime;
    w.rotationMidHoldTime = r.rotationMidHoldTime;
    w.drag = r.drag;
    w.mass = r.mass;
    w.massRandom = r.massRandom;
    w.massSizeMultiplier = r.massSizeMultiplier;
    w.localForces = r.localForces;
    w.worldForces = r.worldForces;
    w.localForcesFallback = r.localForcesFallback;
    w.worldForcesFallback = r.worldForcesFallback;
    w.worldForcesMassMultiplier = r.worldForcesMassMultiplier;
    w.noiseAmplitude = r.noiseAmplitude;
    w.noiseFrequency = r.noiseFrequency;
    w.noiseCoherence = r.noiseCoherence;
    w.noiseEdge = r.noiseEdge;
    w.indexPlusLength = r.indexPlusLength;
    w.emitterShape = r.emitterShape;
    w.ribbonType = static_cast<Sc2RibbonType>(r.ribbonType);
    w.divisions = r.divisions;
    w.edges = r.edges;
    w.innerRadius = r.innerRadius;
    w.flags = static_cast<u32>(r.flags);
    w.sizeSmoothing = static_cast<Sc2Smoothing>(r.sizeSmoothing);
    w.colorSmoothing = static_cast<Sc2Smoothing>(r.colorSmoothing);
    w.friction = r.friction;
    w.bounce = r.bounce;
    w.lodReduce = r.lodReduce;
    w.lodCut = r.lodCut;
    w.yaw.type = r.yawType;
    w.pitch.type = r.pitchType;
    w.speed.type = r.speedType;
    w.size.type = r.sizeType;
    w.alpha.type = r.alphaType;
    w.deprecatedUnknown = r.deprecated.unknown3fbae7d6;
    return w;
}

// ============================================================================
// Export
// ============================================================================

m3::ParticleEmitter ExportParticle(const Sc2ParticleEmitterPayload& w, const ExportLinks& links) {
    m3::ParticleEmitter p;
    ZipParticle(p, w, kExport);
    p.splineLineData.resize(w.splinePoints.size());
    for (std::size_t i = 0; i < w.splinePoints.size(); ++i) {
        Export(w.splinePoints[i], p.splineLineData[i]);
    }

    p.materialIndex = w.materialSlot;
    p.additionalFlags = static_cast<m3::ParticleAdditionalFlag>(w.additionalFlags);
    p.killRadius = w.killRadius;
    p.gravityX = w.gravityX;
    p.gravityY = w.gravityY;
    p.gravity = w.gravity;
    p.sizeMidTime = w.sizeMidTime;
    p.colorMidTime = w.colorMidTime;
    p.alphaMidTime = w.alphaMidTime;
    p.rotationMidTime = w.rotationMidTime;
    p.sizeMidHoldTime = w.sizeMidHoldTime;
    p.colorMidHoldTime = w.colorMidHoldTime;
    p.alphaMidHoldTime = w.alphaMidHoldTime;
    p.rotationMidHoldTime = w.rotationMidHoldTime;
    p.drag = w.drag;
    p.mass = w.mass;
    p.massRandom = w.massRandom;
    p.massSizeMultiplier = w.massSizeMultiplier;
    p.localForces = w.localForces;
    p.worldForces = w.worldForces;
    p.localForcesFallback = w.localForcesFallback;
    p.worldForcesFallback = w.worldForcesFallback;
    p.worldForcesMassMultiplier = w.worldForcesMassMultiplier;
    p.noiseAmplitude = w.noiseAmplitude;
    p.noiseFrequency = w.noiseFrequency;
    p.noiseCoherence = w.noiseCoherence;
    p.noiseEdge = w.noiseEdge;
    p.indexPlusLength = w.indexPlusLength;
    p.maxParticles = w.maxParticles;
    p.emitterShape = static_cast<m3::EmitterShape>(w.emitterShape);
    p.shapeRegions = w.shapeSections;
    p.velocityType = w.velocityType;
    p.sizeRandomEnable = w.sizeRandomEnable;
    p.rotationRandomEnable = w.rotationRandomEnable;
    p.colorRandomEnable = w.colorRandomEnable;
    p.alphaRandomEnable = w.alphaRandomEnable;
    p.flipbookStartInitIndex = w.flipbookStartInitIndex;
    p.flipbookStartStopIndex = w.flipbookStartStopIndex;
    p.flipbookEndInitIndex = w.flipbookEndInitIndex;
    p.flipbookEndStopIndex = w.flipbookEndStopIndex;
    p.flipbookMidTime = w.flipbookMidTime;
    p.flipbookColumns = w.flipbookColumns;
    p.flipbookRows = w.flipbookRows;
    p.flipbookColumnFraction = w.flipbookColumnFraction;
    p.flipbookRowFraction = w.flipbookRowFraction;
    p.bounce = w.bounce;
    p.friction = w.friction;
    p.collisionSpawnIndex = links.particle(w.collisionSpawn);
    p.collisionSpawnMin = w.collisionSpawnMin;
    p.collisionSpawnMax = w.collisionSpawnMax;
    p.collisionSpawnChance = w.collisionSpawnChance;
    p.collisionSpawnEnergy = w.collisionSpawnEnergy;
    p.collisionDieBounce = w.collisionDieBounce;
    p.instanceType = static_cast<m3::ParticleInstanceType>(w.instanceType);
    p.tailLength = w.tailLength;
    p.instanceAngle = w.instanceAngle;
    p.instanceDistance = w.instanceDistance;
    p.pitchType = w.pitch.type;
    p.yawType = w.yaw.type;
    p.speedType = w.speed.type;
    p.sizeType = w.size.type;
    p.alphaType = w.alpha.type;
    p.colorType = w.color.type;
    p.rotationType = w.rotation.type;
    p.horizontalType = w.horizontal.type;
    p.verticalType = w.vertical.type;
    p.flags = static_cast<m3::ParticleFlag>(w.flags);
    p.rotationFlags = static_cast<m3::ParticleRotationFlag>(w.rotationFlags);
    p.colorSmoothing = static_cast<m3::InterpolationMode>(w.colorSmoothing);
    p.sizeSmoothing = static_cast<m3::InterpolationMode>(w.sizeSmoothing);
    p.rotationSmoothing = static_cast<m3::InterpolationMode>(w.rotationSmoothing);
    p.windMultiplier = w.windMultiplier;
    p.lodReduce = w.lodReduce;
    p.lodCut = w.lodCut;
    p.trailLinkIndex = links.particle(w.trailLink);
    p.trailChance = w.trailChance;
    p.splatProjectionIndex = w.splatProjectionIndex;
    p.splatChance = w.splatChance;
    p.modelPaths.reserve(w.models.size());
    for (const AssetKey& key : w.models) {
        p.modelPaths.push_back(key.path);
    }
    p.spawnRibbonOnBounceChance = w.spawnRibbonOnBounceChance;
    p.ribbonLinkIndex = links.ribbon(w.ribbonLink);
    p.deprecated.noiseSmoothness = w.noiseSmoothness;
    return p;
}

m3::ParticleEmitterCopy ExportCopy(const Sc2ParticleEmitterPayload& w) {
    m3::ParticleEmitterCopy c;
    ZipCopy(c, w, kExport);
    return c;
}

m3::RibbonEmitter ExportRibbon(const Sc2RibbonEmitterPayload& w, const ExportLinks& links) {
    m3::RibbonEmitter r;
    ZipRibbon(r, w, kExport);
    r.splineRibbons.resize(w.splinePoints.size());
    for (std::size_t i = 0; i < w.splinePoints.size(); ++i) {
        const Sc2RibbonSplinePoint& point = w.splinePoints[i];
        m3::SplineRibbon& out = r.splineRibbons[i];
        ZipSpline(out, point, kExport);
        out.boneIndex = links.bone(point.node);
        out.emissionOffset = point.emissionOffset;
        out.emissionVector = point.emissionVector;
        out.reserved = point.reserved;
        out.yawType = point.yawVariation.type;
        out.pitchType = point.pitchVariation.type;
        out.velocityType = point.velocityVariation.type;
        out.emissionVectorNormFactor = point.emissionVectorNormFactor;
        out.velocityNormFactor = point.velocityNormFactor;
    }

    r.materialIndex = w.materialSlot;
    r.additionalFlags = static_cast<m3::RibbonAdditionalFlag>(w.additionalFlags);
    r.killRadius = w.killRadius;
    r.gravityX = w.gravityX;
    r.gravityY = w.gravityY;
    r.gravity = w.gravity;
    r.sizeMidTime = w.sizeMidTime;
    r.colorMidTime = w.colorMidTime;
    r.alphaMidTime = w.alphaMidTime;
    r.rotationMidTime = w.rotationMidTime;
    r.sizeMidHoldTime = w.sizeMidHoldTime;
    r.colorMidHoldTime = w.colorMidHoldTime;
    r.alphaMidHoldTime = w.alphaMidHoldTime;
    r.rotationMidHoldTime = w.rotationMidHoldTime;
    r.drag = w.drag;
    r.mass = w.mass;
    r.massRandom = w.massRandom;
    r.massSizeMultiplier = w.massSizeMultiplier;
    r.localForces = w.localForces;
    r.worldForces = w.worldForces;
    r.localForcesFallback = w.localForcesFallback;
    r.worldForcesFallback = w.worldForcesFallback;
    r.worldForcesMassMultiplier = w.worldForcesMassMultiplier;
    r.noiseAmplitude = w.noiseAmplitude;
    r.noiseFrequency = w.noiseFrequency;
    r.noiseCoherence = w.noiseCoherence;
    r.noiseEdge = w.noiseEdge;
    r.indexPlusLength = w.indexPlusLength;
    r.emitterShape = w.emitterShape;
    r.ribbonType = static_cast<m3::RibbonType>(w.ribbonType);
    r.divisions = w.divisions;
    r.edges = w.edges;
    r.innerRadius = w.innerRadius;
    r.flags = static_cast<m3::RibbonFlag>(w.flags);
    r.sizeSmoothing = static_cast<m3::InterpolationMode>(w.sizeSmoothing);
    r.colorSmoothing = static_cast<m3::InterpolationMode>(w.colorSmoothing);
    r.friction = w.friction;
    r.bounce = w.bounce;
    r.lodReduce = w.lodReduce;
    r.lodCut = w.lodCut;
    r.yawType = w.yaw.type;
    r.pitchType = w.pitch.type;
    r.speedType = w.speed.type;
    r.sizeType = w.size.type;
    r.alphaType = w.alpha.type;
    // The half of the one `u32` bone index the editor reads, never set by a
    // shipped ribbon (see `Sc2RibbonEmitterPayload`).
    r.boneIndexFallback = 0;
    r.deprecated.unknown3fbae7d6 = w.deprecatedUnknown;
    return r;
}

} // namespace m3_emitters
} // namespace wem
} // namespace models
} // namespace whiteout
