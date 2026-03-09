// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

void BinaryWriterVisitor::visit(const ParticleEmitter& emitter, u32 version) {
    writer.write(emitter.boneIndex);
    writer.write(emitter.materialIndex);
    if (version >= 17) {
        writer.write(emitter.additionalFlags);
    }

    writer.write(emitter.initialSpeed);
    writer.write(emitter.initialSpeedRandom);
    if (version <= 14) {
        writer.write<u32>(
            hasFlag(emitter.additionalFlags, ParticleAdditionalFlag::EmitSpeedRandomize) ? 1u : 0u);
    }
    writer.write(emitter.initialYaw);
    writer.write(emitter.initialPitch);
    writer.write(emitter.initialHorizontal);
    writer.write(emitter.initialVertical);

    writer.write(emitter.lifetime);
    writer.write(emitter.lifetimeRandom);
    if (version <= 14) {
        writer.write<u32>(
            hasFlag(emitter.additionalFlags, ParticleAdditionalFlag::LifespanRandomize) ? 1u : 0u);
    }

    writer.write(emitter.killRadius);
    writer.write(emitter.gravityX);
    writer.write(emitter.gravityY);
    writer.write(emitter.gravity);

    if (version >= 12) {
        writer.write(emitter.sizeMidTime);
        writer.write(emitter.colorMidTime);
        writer.write(emitter.alphaMidTime);
        writer.write(emitter.rotationMidTime);
    }

    if (version >= 14) {
        writer.write(emitter.sizeMidHoldTime);
        writer.write(emitter.colorMidHoldTime);
        writer.write(emitter.alphaMidHoldTime);
        writer.write(emitter.rotationMidHoldTime);
    }

    writer.write(emitter.sizeAnimation);
    if (version <= 11) {
        writer.write(emitter.sizeMidTime);
    }
    writer.write(emitter.rotationAnimation);
    if (version <= 11) {
        writer.write(emitter.rotationMidTime);
    }
    writer.write(emitter.colorStart);
    writer.write(emitter.colorMid);
    writer.write(emitter.colorEnd);
    if (version <= 11) {
        writer.write(emitter.colorMidTime);
        writer.write(emitter.alphaMidTime);
    }

    writer.write(emitter.drag);
    if (version <= 14) {
        writer.write<u32>(
            hasFlag(emitter.additionalFlags, ParticleAdditionalFlag::MassRandomize) ? 1u : 0u);
    }
    writer.write(emitter.mass);
    writer.write(emitter.massRandom);
    if (version >= 12) {
        writer.write(emitter.massSizeMultiplier);
    }
    if (version <= 14) {
        writer.write<u32>(
            hasFlag(emitter.additionalFlags, ParticleAdditionalFlag::WorldSpace) ? 1u : 0u);
    }

    writer.write(emitter.localForces);
    writer.write(emitter.worldForces);
    writer.write(emitter.localForcesFallback);
    writer.write(emitter.worldForcesFallback);
    if (version >= 24) {
        writer.write(emitter.worldForcesMassMultiplier);
    }

    writer.write(emitter.noiseAmplitude);
    writer.write(emitter.noiseFrequency);
    writer.write(emitter.noiseCoherence);
    writer.write(emitter.noiseEdge);
    if (version <= 11) {
        writer.write(emitter.deprecated.noiseSmoothness);
    }
    if (version >= 11) {
        writer.write(emitter.indexPlusLength);
    }

    writer.write(emitter.maxParticles);
    writer.write(emitter.emissionRate);
    writer.write(emitter.emitterShape);
    writer.write(emitter.shapeOuter);
    writer.write(emitter.shapeInner);
    writer.write(emitter.outerRadius);
    writer.write(emitter.innerRadius);
    if (version >= 14) {
        visit(emitter.shapeRegions);
    }

    writer.write(emitter.velocityType);
    writer.write(emitter.sizeRandomEnable);
    writer.write(emitter.sizeRandomAnimation);
    writer.write(emitter.rotationRandomEnable);
    writer.write(emitter.rotationRandomAnimation);
    writer.write(emitter.colorRandomEnable);
    writer.write(emitter.colorStartRandom);
    writer.write(emitter.colorMidRandom);
    writer.write(emitter.colorEndRandom);
    writer.write(emitter.alphaRandomEnable);

    writer.write(emitter.squirtAmount);
    writer.write(emitter.flipbookStartInitIndex);
    writer.write(emitter.flipbookStartStopIndex);
    writer.write(emitter.flipbookEndInitIndex);
    writer.write(emitter.flipbookEndStopIndex);
    writer.write(emitter.flipbookMidTime);
    writer.write(emitter.flipbookColumns);
    writer.write(emitter.flipbookRows);
    if (version >= 12) {
        writer.write(emitter.flipbookColumnFraction);
        writer.write(emitter.flipbookRowFraction);
    }

    writer.write(emitter.bounce);
    writer.write(emitter.friction);
    writer.write(emitter.collisionSpawnIndex);
    writer.write(emitter.collisionSpawnMin);
    writer.write(emitter.collisionSpawnMax);
    writer.write(emitter.collisionSpawnChance);
    writer.write(emitter.collisionSpawnEnergy);
    writer.write(emitter.collisionDieBounce);

    writer.write(emitter.instanceType);
    writer.write(emitter.tailLength);
    writer.write(emitter.instanceAngle);
    if (version >= 17) {
        writer.write(emitter.instanceDistance);
    }

    writer.write(emitter.pitchType);
    writer.write(emitter.pitchAmplitude);
    writer.write(emitter.pitchFrequency);
    writer.write(emitter.yawType);
    writer.write(emitter.yawAmplitude);
    writer.write(emitter.yawFrequency);
    writer.write(emitter.speedType);
    writer.write(emitter.speedAmplitude);
    writer.write(emitter.speedFrequency);
    writer.write(emitter.sizeType);
    writer.write(emitter.sizeAmplitude);
    writer.write(emitter.sizeFrequency);
    writer.write(emitter.alphaType);
    writer.write(emitter.alphaAmplitude);
    writer.write(emitter.alphaFrequency);
    writer.write(emitter.colorType);
    writer.write(emitter.colorAmplitude);
    writer.write(emitter.colorFrequency);
    writer.write(emitter.rotationType);
    writer.write(emitter.rotationAmplitude);
    writer.write(emitter.rotationFrequency);
    writer.write(emitter.horizontalType);
    writer.write(emitter.horizontalAmplitude);
    writer.write(emitter.horizontalFrequency);
    writer.write(emitter.verticalType);
    writer.write(emitter.verticalAmplitude);
    writer.write(emitter.verticalFrequency);

    writer.write(emitter.particleVelocity);
    if (version >= 22) {
        writer.write(emitter.phaseShift);
    }

    writer.write(emitter.flags);
    if (version >= 18) {
        writer.write(emitter.rotationFlags);
    }

    if (version >= 14) {
        writer.write(emitter.colorSmoothing);
        writer.write(emitter.sizeSmoothing);
        writer.write(emitter.rotationSmoothing);
    }

    if (version >= 17) {
        writer.write(emitter.alphaThreshold);
        writer.write(emitter.uvOffset);
        writer.write(emitter.uvAngle);
        writer.write(emitter.uvTiling);
    }

    visit(emitter.splatLineData);

    writer.write(emitter.windMultiplier);
    writer.write(emitter.lodReduce);
    writer.write(emitter.lodCut);

    writer.write(emitter.lowerBound);
    writer.write(emitter.upperBound);

    writer.write(emitter.trailLinkIndex);
    writer.write(emitter.trailChance);
    writer.write(emitter.trailEmissionRate);

    writer.write(emitter.splatProjectionIndex);
    writer.write(emitter.splatChance);

    visit(emitter.modelPaths);
    visit(emitter.copyIndices);

    if (version >= 23) {
        writer.write(emitter.spawnRibbonOnBounceChance);
        writer.write(emitter.ribbonLinkIndex);
    }
}

void BinaryWriterVisitor::visit(const ParticleEmitterCopy& copy, u32 version) {
    (void)version;
    writer.write(copy.emissionRate);
    writer.write(copy.squirtAmount);
    writer.write(copy.boneIndex);
}

void BinaryWriterVisitor::visit(const SplineRibbon& ribbon, u32 version) {
    (void)version;
    writer.write(ribbon.emissionOffset);
    writer.write(ribbon.emissionVector);
    writer.write(ribbon.velocity);
    writer.write(ribbon.reserved);
    writer.write(ribbon.boneIndex);
    writer.write(ribbon.velocityBaseFactor);
    writer.write(ribbon.velocityEndFactor);
    writer.write(ribbon.yawType);
    writer.write(ribbon.yawAmplitude);
    writer.write(ribbon.yawFrequency);
    writer.write(ribbon.pitchType);
    writer.write(ribbon.pitchAmplitude);
    writer.write(ribbon.pitchFrequency);
    writer.write(ribbon.velocityType);
    writer.write(ribbon.velocityAmplitude);
    writer.write(ribbon.velocityFrequency);
    writer.write(ribbon.yaw);
    writer.write(ribbon.pitch);
    writer.write(ribbon.emissionVectorNormFactor);
    writer.write(ribbon.velocityNormFactor);
}

void BinaryWriterVisitor::visit(const RibbonEmitter& emitter, u32 version) {
    writer.write(emitter.boneIndex);
    writer.write(emitter.boneIndexFallback);
    writer.write(emitter.materialIndex);
    if (version >= 8) {
        writer.write(emitter.additionalFlags);
    }

    writer.write(emitter.initialSpeed);
    writer.write(emitter.initialSpeedRandom);
    if (version <= 6) {
        writer.write<u32>(
            hasFlag(emitter.additionalFlags, RibbonAdditionalFlag::SpeedRandomize) ? 1u : 0u);
    }

    // till v6: pitch then yaw, since v8: yaw then pitch
    if (version <= 6) {
        writer.write(emitter.initialPitch);
        writer.write(emitter.initialYaw);
    } else {
        writer.write(emitter.initialYaw);
        writer.write(emitter.initialPitch);
    }
    writer.write(emitter.initialHorizontal);
    writer.write(emitter.initialVertical);

    writer.write(emitter.lifetime);
    writer.write(emitter.lifetimeRandom);
    if (version <= 6) {
        writer.write<u32>(
            hasFlag(emitter.additionalFlags, RibbonAdditionalFlag::LifespanRandomize) ? 1u : 0u);
    }

    writer.write(emitter.killRadius);
    writer.write(emitter.gravityX);
    writer.write(emitter.gravityY);
    writer.write(emitter.gravity);

    if (version >= 6) {
        writer.write(emitter.sizeMidTime);
        writer.write(emitter.colorMidTime);
        writer.write(emitter.alphaMidTime);
        writer.write(emitter.rotationMidTime);
    }
    if (version >= 8) {
        writer.write(emitter.sizeMidHoldTime);
        writer.write(emitter.colorMidHoldTime);
        writer.write(emitter.alphaMidHoldTime);
        writer.write(emitter.rotationMidHoldTime);
    }

    writer.write(emitter.sizeAnimation);
    if (version <= 5) {
        writer.write(emitter.sizeMidTime);
    }
    writer.write(emitter.rotationAnimation);
    if (version <= 5) {
        writer.write(emitter.rotationMidTime);
    }
    writer.write(emitter.colorStart);
    writer.write(emitter.colorMid);
    writer.write(emitter.colorEnd);
    if (version <= 5) {
        writer.write(emitter.colorMidTime);
        writer.write(emitter.alphaMidTime);
    }

    writer.write(emitter.drag);
    if (version <= 6) {
        writer.write<u32>(
            hasFlag(emitter.additionalFlags, RibbonAdditionalFlag::MassRandomize) ? 1u : 0u);
    }
    writer.write(emitter.mass);
    writer.write(emitter.massRandom);
    writer.write(emitter.massSizeMultiplier);
    if (version <= 6) {
        writer.write<u32>(hasFlag(emitter.additionalFlags, RibbonAdditionalFlag::WorldSpace) ? 1u
                                                                                             : 0u);
    }

    writer.write(emitter.localForces);
    writer.write(emitter.worldForces);
    writer.write(emitter.localForcesFallback);
    writer.write(emitter.worldForcesFallback);
    if (version >= 9) {
        writer.write(emitter.worldForcesMassMultiplier);
    }

    writer.write(emitter.noiseAmplitude);
    writer.write(emitter.noiseFrequency);
    writer.write(emitter.noiseCoherence);
    writer.write(emitter.noiseEdge);
    if (version >= 5) {
        writer.write(emitter.indexPlusLength);
    }

    writer.write(emitter.emitterShape);
    writer.write(emitter.ribbonType);
    writer.write(emitter.divisions);
    writer.write(emitter.edges);
    writer.write(emitter.innerRadius);
    writer.write(emitter.maxLength);
    if (version <= 6) {
        writer.write(emitter.deprecated.unknown3fbae7d6);
    }

    visit(emitter.splineRibbons);
    writer.write(emitter.active);

    writer.write(emitter.flags);
    if (version >= 8) {
        writer.write(emitter.sizeSmoothing);
        writer.write(emitter.colorSmoothing);
    }

    writer.write(emitter.friction);
    writer.write(emitter.bounce);
    writer.write(emitter.lodReduce);
    writer.write(emitter.lodCut);

    if (version <= 6) {
        writer.write(emitter.pitchType);
        writer.write(emitter.pitchAmplitude);
        writer.write(emitter.pitchFrequency);
        writer.write(emitter.yawType);
        writer.write(emitter.yawAmplitude);
        writer.write(emitter.yawFrequency);
    } else {
        writer.write(emitter.yawType);
        writer.write(emitter.yawAmplitude);
        writer.write(emitter.yawFrequency);
        writer.write(emitter.pitchType);
        writer.write(emitter.pitchAmplitude);
        writer.write(emitter.pitchFrequency);
    }

    writer.write(emitter.speedType);
    writer.write(emitter.speedAmplitude);
    writer.write(emitter.speedFrequency);
    writer.write(emitter.sizeType);
    writer.write(emitter.sizeAmplitude);
    writer.write(emitter.sizeFrequency);
    writer.write(emitter.alphaType);
    writer.write(emitter.alphaAmplitude);
    writer.write(emitter.alphaFrequency);

    writer.write(emitter.particleVelocity);
    writer.write(emitter.overlay);
}
