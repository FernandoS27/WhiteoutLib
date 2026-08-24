// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

void BinaryParseVisitor::visit(ParticleEmitter& value, u32 version) {
    // Identification
    value.boneIndex = reader.read<u32>();
    value.materialIndex = reader.read<u32>();
    if (version >= 17) {
        value.additionalFlags = reader.read<ParticleAdditionalFlag>();
    }

    // Initial Velocity
    value.initialSpeed = reader.read<AnimRef<f32>>();
    value.initialSpeedRandom = reader.read<AnimRef<f32>>();
    // The standalone randomize/world-space booleans exist through v16; the
    // SC2 upgrade folds them into additionalFlags bits 1/2/4/8.
    if (version <= 16) {
        if (reader.read<u32>())
            value.additionalFlags |= ParticleAdditionalFlag::EmitSpeedRandomize;
    }
    value.initialYaw = reader.read<AnimRef<f32>>();
    value.initialPitch = reader.read<AnimRef<f32>>();
    value.initialHorizontal = reader.read<AnimRef<f32>>();
    value.initialVertical = reader.read<AnimRef<f32>>();

    // Lifetime
    value.lifetime = reader.read<AnimRef<f32>>();
    value.lifetimeRandom = reader.read<AnimRef<f32>>();
    if (version <= 16) {
        if (reader.read<u32>())
            value.additionalFlags |= ParticleAdditionalFlag::LifespanRandomize;
    }

    // Distance & Gravity
    value.killRadius = reader.read<f32>();
    value.gravityX = reader.read<u32>();
    value.gravityY = reader.read<u32>();
    value.gravity = reader.read<f32>();

    // Midpoint Timing (since v12)
    if (version >= 12) {
        value.sizeMidTime = reader.read<f32>();
        value.colorMidTime = reader.read<f32>();
        value.alphaMidTime = reader.read<f32>();
        value.rotationMidTime = reader.read<f32>();
    }

    // Hold Times (since v13 per the SC2 loader; earliest corpus version is v14)
    if (version >= 13) {
        value.sizeMidHoldTime = reader.read<f32>();
        value.colorMidHoldTime = reader.read<f32>();
        value.alphaMidHoldTime = reader.read<f32>();
        value.rotationMidHoldTime = reader.read<f32>();
    }

    // Per-Particle Curves
    value.sizeAnimation = reader.read<AnimRef<Vector3f>>();
    if (version <= 11) {
        value.sizeMidTime = reader.read<f32>();
    }
    value.rotationAnimation = reader.read<AnimRef<Vector3f>>();
    if (version <= 11) {
        value.rotationMidTime = reader.read<f32>();
    }
    value.colorStart = reader.read<AnimRef<ColorBGRA>>();
    value.colorMid = reader.read<AnimRef<ColorBGRA>>();
    value.colorEnd = reader.read<AnimRef<ColorBGRA>>();
    if (version <= 11) {
        value.colorMidTime = reader.read<f32>();
        value.alphaMidTime = reader.read<f32>();
    }

    // Physics
    value.drag = reader.read<f32>();
    if (version <= 16) {
        if (reader.read<u32>())
            value.additionalFlags |= ParticleAdditionalFlag::MassRandomize;
    }
    value.mass = reader.read<f32>();
    value.massRandom = reader.read<f32>();
    if (version >= 12) {
        value.massSizeMultiplier = reader.read<f32>();
    }
    if (version <= 16) {
        if (reader.read<u32>())
            value.additionalFlags |= ParticleAdditionalFlag::WorldSpace;
    }

    // Forces
    value.localForces = reader.read<u16>();
    value.worldForces = reader.read<u16>();
    value.localForcesFallback = reader.read<u16>();
    value.worldForcesFallback = reader.read<u16>();
    if (version >= 24) {
        value.worldForcesMassMultiplier = reader.read<f32>();
    }

    // Noise
    value.noiseAmplitude = reader.read<f32>();
    value.noiseFrequency = reader.read<f32>();
    value.noiseCoherence = reader.read<f32>();
    value.noiseEdge = reader.read<f32>();
    if (version <= 11) {
        value.deprecated.noiseSmoothness = reader.read<f32>();
    }
    if (version >= 11) {
        value.indexPlusLength = reader.read<u32>();
    }

    // Emission
    value.maxParticles = reader.read<u32>();
    value.emissionRate = reader.read<AnimRef<f32>>();
    value.emitterShape = reader.read<EmitterShape>();
    value.shapeOuter = reader.read<AnimRef<Vector3f>>();
    value.shapeInner = reader.read<AnimRef<Vector3f>>();
    value.outerRadius = reader.read<AnimRef<f32>>();
    value.innerRadius = reader.read<AnimRef<f32>>();
    if (version >= 14) {
        visit(value.shapeRegions);
    }

    // Randomization
    value.velocityType = reader.read<u32>();
    value.sizeRandomEnable = reader.read<u32>();
    value.sizeRandomAnimation = reader.read<AnimRef<Vector3f>>();
    value.rotationRandomEnable = reader.read<u32>();
    value.rotationRandomAnimation = reader.read<AnimRef<Vector3f>>();
    value.colorRandomEnable = reader.read<u32>();
    value.colorStartRandom = reader.read<AnimRef<ColorBGRA>>();
    value.colorMidRandom = reader.read<AnimRef<ColorBGRA>>();
    value.colorEndRandom = reader.read<AnimRef<ColorBGRA>>();
    value.alphaRandomEnable = reader.read<u32>();

    // Squirt & Flipbook
    value.squirtAmount = reader.read<AnimRef<u16>>();
    value.flipbookStartInitIndex = reader.read<u8>();
    value.flipbookStartStopIndex = reader.read<u8>();
    value.flipbookEndInitIndex = reader.read<u8>();
    value.flipbookEndStopIndex = reader.read<u8>();
    value.flipbookMidTime = reader.read<f32>();
    value.flipbookColumns = reader.read<u16>();
    value.flipbookRows = reader.read<u16>();
    if (version >= 12) {
        value.flipbookColumnFraction = reader.read<f32>();
        value.flipbookRowFraction = reader.read<f32>();
    }

    // Collision
    value.bounce = reader.read<f32>();
    value.friction = reader.read<f32>();
    value.collisionSpawnIndex = reader.read<i32>();
    value.collisionSpawnMin = reader.read<u32>();
    value.collisionSpawnMax = reader.read<u32>();
    value.collisionSpawnChance = reader.read<f32>();
    value.collisionSpawnEnergy = reader.read<f32>();
    value.collisionDieBounce = reader.read<u32>();

    // Instance
    value.instanceType = reader.read<ParticleInstanceType>();
    value.tailLength = reader.read<f32>();
    value.instanceAngle = reader.read<Vector3f>();
    // The SC2 loader forces the value to 1.0 below v20 even when present
    if (version >= 16) {
        value.instanceDistance = reader.read<f32>();
    }

    // Variation Channels (order: pitch, yaw, speed, size, alpha, color, rotation, horizontal,
    // vertical)
    value.pitchType = reader.read<u32>();
    value.pitchAmplitude = reader.read<AnimRef<f32>>();
    value.pitchFrequency = reader.read<AnimRef<f32>>();
    value.yawType = reader.read<u32>();
    value.yawAmplitude = reader.read<AnimRef<f32>>();
    value.yawFrequency = reader.read<AnimRef<f32>>();
    value.speedType = reader.read<u32>();
    value.speedAmplitude = reader.read<AnimRef<f32>>();
    value.speedFrequency = reader.read<AnimRef<f32>>();
    value.sizeType = reader.read<u32>();
    value.sizeAmplitude = reader.read<AnimRef<f32>>();
    value.sizeFrequency = reader.read<AnimRef<f32>>();
    value.alphaType = reader.read<u32>();
    value.alphaAmplitude = reader.read<AnimRef<f32>>();
    value.alphaFrequency = reader.read<AnimRef<f32>>();
    value.colorType = reader.read<u32>();
    value.colorAmplitude = reader.read<AnimRef<f32>>();
    value.colorFrequency = reader.read<AnimRef<f32>>();
    value.rotationType = reader.read<u32>();
    value.rotationAmplitude = reader.read<AnimRef<f32>>();
    value.rotationFrequency = reader.read<AnimRef<f32>>();
    value.horizontalType = reader.read<u32>();
    value.horizontalAmplitude = reader.read<AnimRef<f32>>();
    value.horizontalFrequency = reader.read<AnimRef<f32>>();
    value.verticalType = reader.read<u32>();
    value.verticalAmplitude = reader.read<AnimRef<f32>>();
    value.verticalFrequency = reader.read<AnimRef<f32>>();

    // Parent Velocity & Phase
    value.particleVelocity = reader.read<AnimRef<f32>>();
    if (version >= 22) {
        value.phaseShift = reader.read<AnimRef<f32>>();
    }

    // Flags
    value.flags = reader.read<ParticleFlag>();
    if (version >= 18) {
        value.rotationFlags = reader.read<ParticleRotationFlag>();
    }

    // Smoothing (since v13 per the SC2 loader; earliest corpus version is v14)
    if (version >= 13) {
        value.colorSmoothing = reader.read<InterpolationMode>();
        value.sizeSmoothing = reader.read<InterpolationMode>();
        value.rotationSmoothing = reader.read<InterpolationMode>();
    } else {
        // Pre-v13 smoothing lived in the Old*Smooth/Bezier flag bits; the SC2
        // upgrade derives the enums from them. Size has no LinearSmooth
        // mapping in the client — OldSizeSmooth (0x800) is ignored.
        value.colorSmoothing =
            hasFlag(value.flags, ParticleFlag::OldColorBezier)   ? InterpolationMode::Bezier
            : hasFlag(value.flags, ParticleFlag::OldColorSmooth) ? InterpolationMode::LinearSmooth
                                                                 : InterpolationMode::Linear;
        value.sizeSmoothing = hasFlag(value.flags, ParticleFlag::OldSizeBezier)
                                  ? InterpolationMode::Bezier
                                  : InterpolationMode::Linear;
        value.rotationSmoothing = hasFlag(value.flags, ParticleFlag::OldRotationBezier)
                                      ? InterpolationMode::Bezier
                                  : hasFlag(value.flags, ParticleFlag::OldRotationSmooth)
                                      ? InterpolationMode::LinearSmooth
                                      : InterpolationMode::Linear;
    }

    // UV Screen Space (since v15 per the SC2 loader; earliest corpus version is v17)
    if (version >= 15) {
        value.alphaThreshold = reader.read<AnimRef<f32>>();
        value.uvOffset = reader.read<AnimRef<Vector2f>>();
        value.uvAngle = reader.read<AnimRef<Vector3f>>();
        value.uvTiling = reader.read<AnimRef<Vector2f>>();
    }

    // Spline (always present)
    visit(value.splineLineData);

    // Wind & LOD
    value.windMultiplier = reader.read<f32>();
    value.lodReduce = reader.read<u32>();
    value.lodCut = reader.read<u32>();

    // Bounds
    value.lowerBound = reader.read<AnimRef<f32>>();
    value.upperBound = reader.read<AnimRef<f32>>();

    // Trails
    value.trailLinkIndex = reader.read<i32>();
    value.trailChance = reader.read<f32>();
    value.trailEmissionRate = reader.read<AnimRef<f32>>();

    // Splat
    value.splatProjectionIndex = reader.read<i32>();
    value.splatChance = reader.read<f32>();

    // References
    visit(value.modelPaths);
    visit(value.copyIndices);

    // v23+ unknowns
    if (version >= 23) {
        value.spawnRibbonOnBounceChance = reader.read<f32>();
        value.ribbonLinkIndex = reader.read<i32>();
    }

    // --- SC2 version-upgrade fixups (M3_ProcessChunks PAR_ converter) ---
    // Only fields absent from the parsed version are synthesized; values the
    // file provides are never rewritten. (The client goes further at load —
    // it also ORs the rotation bits into v18-20 file values and forces
    // instanceDistance to 1.0 below v20.)
    if (version < 24) {
        value.worldForcesMassMultiplier = 1.0f;
    }
    if (version < 18) {
        if (value.rotationRandomEnable) {
            value.rotationFlags |= ParticleRotationFlag::Relative;
        }
        value.rotationFlags |= ParticleRotationFlag::Unknown6;
    }
    if (version <= 22 && hasFlag(value.flags, ParticleFlag::ModelParticles)) {
        // Legacy model particles store the linked ribbon index in the
        // instanceType slot; the client moves it into ribbonLinkIndex and
        // re-derives the visual type. ribbonLinkIndex keeps the raw value, so
        // the writer reconstructs the on-disk instanceType from it.
        value.ribbonLinkIndex = static_cast<i32>(value.instanceType);
        value.spawnRibbonOnBounceChance = 1.0f; // client stamps integer 1
        const u32 oldType = static_cast<u32>(value.instanceType);
        if (hasFlag(value.additionalFlags, ParticleAdditionalFlag::WorldSpace)) {
            switch (oldType) {
            case 0:
                value.instanceType = ParticleInstanceType::FaceWorldDir;
                if (version < 18)
                    value.rotationFlags |= ParticleRotationFlag::Unknown7;
                break;
            case 1:
            case 9:
            case 10:
                value.instanceType = ParticleInstanceType::FaceTravelDir;
                break;
            case 4:
                value.instanceType = ParticleInstanceType::FaceWorldDir;
                break;
            default:
                break;
            }
        } else if (oldType <= 10) {
            // Types {1,2,4,5,6,9,10} (mask 0x676) collapse to FaceWorldDir
            if ((0x676u >> oldType) & 1u) {
                value.instanceType = ParticleInstanceType::FaceWorldDir;
            } else if (oldType == 0) {
                value.instanceType = ParticleInstanceType::FaceWorldDir;
                if (version < 18)
                    value.rotationFlags |= ParticleRotationFlag::Unknown7;
            }
        }
    }
}

void BinaryParseVisitor::visit(ParticleEmitterCopy& value, u32 version) {
    (void)version;
    value.emissionRate = reader.read<AnimRef<f32>>();
    value.squirtAmount = reader.read<AnimRef<u16>>();
    value.boneIndex = reader.read<u32>();
}

void BinaryParseVisitor::visit(SplineRibbon& value, u32 version) {
    (void)version;
    value.emissionOffset = reader.read<Vector3f>();
    value.emissionVector = reader.read<Vector3f>();
    value.velocity = reader.read<AnimRef<f32>>();
    value.reserved = reader.read<u32>();
    value.boneIndex = reader.read<u32>();
    value.velocityBaseFactor = reader.read<AnimRef<f32>>();
    value.velocityEndFactor = reader.read<AnimRef<f32>>();
    value.yawType = reader.read<u32>();
    value.yawAmplitude = reader.read<AnimRef<f32>>();
    value.yawFrequency = reader.read<AnimRef<f32>>();
    value.pitchType = reader.read<u32>();
    value.pitchAmplitude = reader.read<AnimRef<f32>>();
    value.pitchFrequency = reader.read<AnimRef<f32>>();
    value.velocityType = reader.read<u32>();
    value.velocityAmplitude = reader.read<AnimRef<f32>>();
    value.velocityFrequency = reader.read<AnimRef<f32>>();
    value.yaw = reader.read<AnimRef<f32>>();
    value.pitch = reader.read<AnimRef<f32>>();
    value.emissionVectorNormFactor = reader.read<f32>();
    value.velocityNormFactor = reader.read<f32>();
}

void BinaryParseVisitor::visit(RibbonEmitter& value, u32 version) {
    // Identification
    value.boneIndex = reader.read<u16>();
    value.boneIndexFallback = reader.read<u16>();
    value.materialIndex = reader.read<u32>();
    if (version >= 8) {
        value.additionalFlags = static_cast<RibbonAdditionalFlag>(reader.read<u32>());
    }

    // Initial velocity
    value.initialSpeed = reader.read<AnimRef<f32>>();
    value.initialSpeedRandom = reader.read<AnimRef<f32>>();
    // The standalone randomize/world-space booleans exist through v7; the
    // SC2 upgrade folds them into additionalFlags bits 1/2/4/8.
    if (version <= 7) {
        if (reader.read<u32>())
            value.additionalFlags |= RibbonAdditionalFlag::SpeedRandomize;
    }

    // till v6: pitch then yaw, since v8: yaw then pitch
    if (version <= 6) {
        value.initialPitch = reader.read<AnimRef<f32>>();
        value.initialYaw = reader.read<AnimRef<f32>>();
    } else {
        value.initialYaw = reader.read<AnimRef<f32>>();
        value.initialPitch = reader.read<AnimRef<f32>>();
    }
    value.initialHorizontal = reader.read<AnimRef<f32>>();
    value.initialVertical = reader.read<AnimRef<f32>>();

    // Lifetime
    value.lifetime = reader.read<AnimRef<f32>>();
    value.lifetimeRandom = reader.read<AnimRef<f32>>();
    if (version <= 7) {
        if (reader.read<u32>())
            value.additionalFlags |= RibbonAdditionalFlag::LifespanRandomize;
    }

    // Distance & gravity
    value.killRadius = reader.read<u32>();
    value.gravityX = reader.read<f32>();
    value.gravityY = reader.read<f32>();
    value.gravity = reader.read<f32>();

    // Midpoint timing
    if (version >= 6) {
        value.sizeMidTime = reader.read<f32>();
        value.colorMidTime = reader.read<f32>();
        value.alphaMidTime = reader.read<f32>();
        value.rotationMidTime = reader.read<f32>();
    }
    if (version >= 7) {
        value.sizeMidHoldTime = reader.read<f32>();
        value.colorMidHoldTime = reader.read<f32>();
        value.alphaMidHoldTime = reader.read<f32>();
        value.rotationMidHoldTime = reader.read<f32>();
    }

    // Curves
    value.sizeAnimation = reader.read<AnimRef<Vector3f>>();
    if (version <= 5) {
        value.sizeMidTime = reader.read<f32>();
    }
    value.rotationAnimation = reader.read<AnimRef<Vector3f>>();
    if (version <= 5) {
        value.rotationMidTime = reader.read<f32>();
    }
    value.colorStart = reader.read<AnimRef<ColorBGRA>>();
    value.colorMid = reader.read<AnimRef<ColorBGRA>>();
    value.colorEnd = reader.read<AnimRef<ColorBGRA>>();
    if (version <= 5) {
        value.colorMidTime = reader.read<f32>();
        value.alphaMidTime = reader.read<f32>();
    }

    // Physics
    value.drag = reader.read<f32>();
    if (version <= 7) {
        if (reader.read<u32>())
            value.additionalFlags |= RibbonAdditionalFlag::MassRandomize;
    }
    value.mass = reader.read<f32>();
    value.massRandom = reader.read<f32>();
    value.massSizeMultiplier = reader.read<f32>();
    if (version <= 7) {
        if (reader.read<u32>())
            value.additionalFlags |= RibbonAdditionalFlag::WorldSpace;
    }

    // Forces
    value.localForces = reader.read<u16>();
    value.worldForces = reader.read<u16>();
    value.localForcesFallback = reader.read<u16>();
    value.worldForcesFallback = reader.read<u16>();
    if (version >= 9) {
        value.worldForcesMassMultiplier = reader.read<f32>();
    }

    // Noise
    value.noiseAmplitude = reader.read<f32>();
    value.noiseFrequency = reader.read<f32>();
    value.noiseCoherence = reader.read<f32>();
    value.noiseEdge = reader.read<f32>();
    if (version >= 5) {
        value.indexPlusLength = reader.read<u32>();
    }

    // Shape
    value.emitterShape = reader.read<u32>();
    value.ribbonType = reader.read<RibbonType>();
    value.divisions = reader.read<f32>();
    value.edges = reader.read<u32>();
    value.innerRadius = reader.read<f32>();
    value.maxLength = reader.read<AnimRef<f32>>();
    if (version <= 6) {
        value.deprecated.unknown3fbae7d6 = reader.read<i32>();
    }

    // References
    visit(value.splineRibbons);
    value.active = reader.read<AnimRef<u32>>();

    // Flags & smoothing
    // Note: the SC2 upgrade also force-sets bit 31 of flags on every upgraded
    // ribbon — a runtime-internal marker with no reader in the binary, not
    // replicated here.
    value.flags = static_cast<RibbonFlag>(reader.read<u32>());
    if (version >= 7) {
        value.sizeSmoothing = reader.read<InterpolationMode>();
        value.colorSmoothing = reader.read<InterpolationMode>();
    } else {
        // Pre-v7 size smoothing lived in the SmoothSize/BezierSmoothSize flag
        // bits; the SC2 upgrade derives the enum from them.
        value.sizeSmoothing =
            hasFlag(value.flags, RibbonFlag::BezierSmoothSize) ? InterpolationMode::Bezier
            : hasFlag(value.flags, RibbonFlag::SmoothSize)     ? InterpolationMode::LinearSmooth
                                                               : InterpolationMode::Linear;
        value.colorSmoothing = InterpolationMode::Linear;
    }

    // Collision & LOD
    value.friction = reader.read<f32>();
    value.bounce = reader.read<f32>();
    value.lodReduce = reader.read<u32>();
    value.lodCut = reader.read<u32>();

    // Variation channels
    // till v6: pitch then yaw, since v8: yaw then pitch
    if (version <= 6) {
        value.pitchType = reader.read<u32>();
        value.pitchAmplitude = reader.read<AnimRef<f32>>();
        value.pitchFrequency = reader.read<AnimRef<f32>>();
        value.yawType = reader.read<u32>();
        value.yawAmplitude = reader.read<AnimRef<f32>>();
        value.yawFrequency = reader.read<AnimRef<f32>>();
    } else {
        value.yawType = reader.read<u32>();
        value.yawAmplitude = reader.read<AnimRef<f32>>();
        value.yawFrequency = reader.read<AnimRef<f32>>();
        value.pitchType = reader.read<u32>();
        value.pitchAmplitude = reader.read<AnimRef<f32>>();
        value.pitchFrequency = reader.read<AnimRef<f32>>();
    }

    // length, scale, alpha variation
    value.speedType = reader.read<u32>();
    value.speedAmplitude = reader.read<AnimRef<f32>>();
    value.speedFrequency = reader.read<AnimRef<f32>>();
    value.sizeType = reader.read<u32>();
    value.sizeAmplitude = reader.read<AnimRef<f32>>();
    value.sizeFrequency = reader.read<AnimRef<f32>>();
    value.alphaType = reader.read<u32>();
    value.alphaAmplitude = reader.read<AnimRef<f32>>();
    value.alphaFrequency = reader.read<AnimRef<f32>>();

    // Parent velocity & phase
    value.particleVelocity = reader.read<AnimRef<f32>>();
    value.overlay = reader.read<AnimRef<f32>>();

    // SC2 version-upgrade fixup: the field only exists from v9; the client
    // defaults it to 1.0 (not 0) for older ribbons.
    if (version <= 8) {
        value.worldForcesMassMultiplier = 1.0f;
    }
}
