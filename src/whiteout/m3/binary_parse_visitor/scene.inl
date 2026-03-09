// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

void BinaryParseVisitor::visit(Light& value, u32 version) {
    (void)version;
    value.lightType = static_cast<LightType>(reader.read<u16>());
    value.boneIndex = reader.read<u16>();
    value.flags = static_cast<LightFlag>(reader.read<u32>());
    value.lodCut = reader.read<u32>();
    value.shadowLodCut = reader.read<u32>();
    value.diffuseColor = reader.read<AnimRef<Vector3f>>();
    value.intensityMultiplier = reader.read<AnimRef<f32>>();
    value.specularColor = reader.read<AnimRef<Vector3f>>();
    value.specularMultiplier = reader.read<AnimRef<f32>>();
    value.decay = reader.read<AnimRef<f32>>();
    value.attenuationEnd = reader.read<f32>();
    value.attenuationStart = reader.read<AnimRef<f32>>();
    value.hotSpot = reader.read<AnimRef<f32>>();
    value.falloff = reader.read<AnimRef<f32>>();
}

void BinaryParseVisitor::visit(Camera& value, u32 version) {
    value.boneIndex = reader.read<u32>();
    visit(value.name);

    if (version >= 2) {
        value.fieldOfView = reader.read<AnimRef<f32>>();
        value.useVerticalFOV = reader.read<u32>();
    }

    if (version == 2) {
        // Beta v2: farClip/nearClip are plain f32 (promoted to AnimRef for storage)
        f32 farClipStatic = reader.read<f32>();
        f32 nearClipStatic = reader.read<f32>();
        value.farClip = AnimRef<f32>{};
        value.farClip.initValue = farClipStatic;
        value.nearClip = AnimRef<f32>{};
        value.nearClip.initValue = nearClipStatic;
    }

    if (version >= 5) {
        value.dofType = reader.read<u32>();
    }

    if (version >= 3) {
        value.farClip = reader.read<AnimRef<f32>>();
        value.nearClip = reader.read<AnimRef<f32>>();
    }

    if (version >= 2) {
        value.shadowClipDistance = reader.read<AnimRef<f32>>();
        value.focusDistance = reader.read<AnimRef<f32>>();
        value.farFocusRange = reader.read<AnimRef<f32>>();
        value.nearFocusRange = reader.read<AnimRef<f32>>();
    }

    if (version >= 4) {
        value.nearFalloffStart = reader.read<AnimRef<f32>>();
        value.nearFalloffEnd = reader.read<AnimRef<f32>>();
    }

    if (version >= 2) {
        value.dofAmount = reader.read<AnimRef<f32>>();
    }

    if (version >= 5) {
        value.bokehFStop = reader.read<AnimRef<f32>>();
        value.bokehMaxCoCDiameter = reader.read<AnimRef<f32>>();
    }
}

void BinaryParseVisitor::visit(Event& value, u32 version) {
    visit(value.name);
    value.unknown = reader.read<u32>();
    value.boneIndex = reader.read<u16>();
    value.padding = reader.read<u16>();
    value.transform = reader.read<Matrix44f>();
    value.eventType = reader.read<u32>();
    visit(value.optionString);
    if (version >= 1) {
        value.rttChannelIndex = reader.read<u32>();
    }
    if (version >= 2) {
        value.extraParameter = reader.read<u32>();
    }
}

void BinaryParseVisitor::visit(ShadowBox& value, u32 version) {
    (void)version;
    value.matrix = reader.read<Matrix44f>();
}

void BinaryParseVisitor::visit(Projector& value, u32 version) {
    (void)version;
    value.projectionType = static_cast<ProjectionType>(reader.read<u32>());
    value.bone = reader.read<u32>();
    value.materialReferenceIndex = reader.read<u32>();
    value.offset = reader.read<AnimRef<Vector3f>>();
    value.pitch = reader.read<AnimRef<f32>>();
    value.yaw = reader.read<AnimRef<f32>>();
    value.roll = reader.read<AnimRef<f32>>();
    value.fieldOfView = reader.read<AnimRef<f32>>();
    value.aspectRatio = reader.read<AnimRef<f32>>();
    value.near = reader.read<AnimRef<f32>>();
    value.far = reader.read<AnimRef<f32>>();
    value.boxOffsetZBottom = reader.read<AnimRef<f32>>();
    value.boxOffsetZTop = reader.read<AnimRef<f32>>();
    value.boxOffsetXLeft = reader.read<AnimRef<f32>>();
    value.boxOffsetXRight = reader.read<AnimRef<f32>>();
    value.boxOffsetYFront = reader.read<AnimRef<f32>>();
    value.boxOffsetYBack = reader.read<AnimRef<f32>>();
    value.falloff = reader.read<f32>();
    value.alphaInit = reader.read<f32>();
    value.alphaMid = reader.read<f32>();
    value.alphaEnd = reader.read<f32>();
    value.lifetimeAttack = reader.read<f32>();
    value.lifetimeAttackTo = reader.read<f32>();
    value.lifetimeHold = reader.read<f32>();
    value.lifetimeHoldTo = reader.read<f32>();
    value.lifetimeDecay = reader.read<f32>();
    value.lifetimeDecayTo = reader.read<f32>();
    value.attenuationDistance = reader.read<f32>();
    value.active = reader.read<AnimRef<u32>>();
    value.layer = reader.read<u32>();
    value.lodReduce = reader.read<u32>();
    value.lodCut = reader.read<u32>();
    value.flags = static_cast<ProjectorFlag>(reader.read<u32>());
}

void BinaryParseVisitor::visit(Force& value, u32 version) {
    (void)version;
    value.forceType = static_cast<ForceType>(reader.read<u32>());
    value.forceShape = static_cast<ForceShape>(reader.read<u32>());
    value.unknown = reader.read<u32>();
    value.boneIndex = reader.read<u32>();
    value.flags = static_cast<ForceFlag>(reader.read<u32>());
    value.localChannels = reader.read<u32>();
    value.strength = reader.read<AnimRef<f32>>();
    value.width = reader.read<AnimRef<f32>>();
    value.height = reader.read<AnimRef<f32>>();
    value.length = reader.read<AnimRef<f32>>();
}

void BinaryParseVisitor::visit(Warp& value, u32 version) {
    (void)version;
    value.warpType = reader.read<u32>();
    value.boneIndex = reader.read<u32>();
    value.unknown = reader.read<u32>();
    value.radius = reader.read<AnimRef<f32>>();
    value.height = reader.read<AnimRef<f32>>();
    value.strength = reader.read<AnimRef<f32>>();
    value.angular = reader.read<AnimRef<f32>>();
    value.axial = reader.read<AnimRef<f32>>();
    value.radial = reader.read<AnimRef<f32>>();
}

void BinaryParseVisitor::visit(ViewVolume& value, u32 version) {
    (void)version;
    value.nodeIndex = reader.read<u32>();
    value.size = reader.read<AnimRef<Vector3f>>();
}

void BinaryParseVisitor::visit(TrailingModel& value, u32 version) {
    (void)version;
    visit(value.vectors);
    value.param0 = reader.read<f32>();
    value.param1 = reader.read<f32>();
    value.animFloat0 = reader.read<AnimRef<f32>>();
    value.animFloat1 = reader.read<AnimRef<f32>>();
    value.flag = reader.read<u32>();
    value.reserved0 = reader.read<u32>();
    value.reserved1 = reader.read<u32>();
}

void BinaryParseVisitor::visit(HitTestShape& value, u32 version) {
    detail::setStructureVersion(value, version);
    (void)version;
    value.shapeType = static_cast<HitTestShapeType>(reader.read<u32>());
    value.boneIndex = reader.read<u16>();
    value.padding = reader.read<u16>();
    value.transform = reader.read<Matrix44f>();
    visit(value.vertexPositions);
    visit(value.faceIndices);
    value.sizeX = reader.read<f32>();
    value.sizeY = reader.read<f32>();
    value.sizeZ = reader.read<f32>();
}

void BinaryParseVisitor::visit(AttachmentVolume& value, u32 version) {
    (void)version;
    value.bone1 = reader.read<u32>();
    value.bone2 = reader.read<u32>();
    value.shapeType = static_cast<HitTestShapeType>(reader.read<u32>());
    value.boneIndex = reader.read<u16>();
    value.padding = reader.read<u16>();
    value.transform = reader.read<Matrix44f>();
    visit(value.vertexPositions);
    visit(value.faceIndices);
    value.sizeX = reader.read<f32>();
    value.sizeY = reader.read<f32>();
    value.sizeZ = reader.read<f32>();
}

void BinaryParseVisitor::visit(TriggerData& value, u32 version) {
    (void)version;
    visit(value.dataIndices);
    visit(value.name);
}

void BinaryParseVisitor::visit(TurretBehavior& value, u32 version) {
    value.transform = reader.read<Matrix44f>();
    if (version >= 4) {
        value.unknown1 = reader.read<Vector4f>();
        value.unknown2 = reader.read<Vector4f>();
    }
    value.boneIndex = reader.read<u16>();
    value.useAsMainTurret = reader.read<u8>();
    value.turretGroupId = reader.read<u8>();
    value.yawLimited = reader.read<u32>();
    value.yawMin = reader.read<f32>();
    value.yawMax = reader.read<f32>();
    if (version >= 4) {
        value.yawWeight = reader.read<f32>();
    }
    value.pitchLimited = reader.read<u32>();
    value.pitchMin = reader.read<f32>();
    value.pitchMax = reader.read<f32>();
    if (version >= 4) {
        value.pitchWeight = reader.read<f32>();
    }
    value.unknown3 = reader.read<f32>();
    value.unknown4 = reader.read<f32>();
    value.mainBoneOffset = reader.read<Vector3f>();
}

void BinaryParseVisitor::visit(BillboardBehavior& value, u32 version) {
    (void)version;
    visit(value.dependents);
    value.boneIndex = reader.read<u16>();
    value.billboardType = reader.read<u8>();
    value.cameraLookAt = reader.read<u8>();
    value.up = reader.read<Quaternion>();
    value.forward = reader.read<Quaternion>();
}

void BinaryParseVisitor::visit(IKJoint& value, u32 version) {
    (void)version;
    visit(value.dependents);
    value.boneIndex1 = reader.read<u16>();
    value.boneIndex2 = reader.read<u16>();
    value.raycastUp = reader.read<f32>();
    value.raycastDown = reader.read<f32>();
    value.maxSpeed = reader.read<f32>();
    value.goalThreshold = reader.read<f32>();
}

void BinaryParseVisitor::visit(IKTwoJoint& value, u32 version) {
    (void)version;
    visit(value.dependents);
    value.boneBase = reader.read<u16>();
    value.boneTarget = reader.read<u16>();
    value.boneEnd = reader.read<u16>();
    value.padding = reader.read<u16>();
    value.hingeAxis = reader.read<Vector3f>();
    value.maxAngleInner = reader.read<f32>();
    value.maxAngleOuter = reader.read<f32>();
    value.searchUp = reader.read<f32>();
    value.searchDown = reader.read<f32>();
}

void BinaryParseVisitor::visit(IKCCD& value, u32 version) {
    (void)version;
    visit(value.dependents);
    value.boneBase = reader.read<u16>();
    value.boneTarget = reader.read<u16>();
    value.searchUp = reader.read<f32>();
    value.searchDown = reader.read<f32>();
}

void BinaryParseVisitor::visit(OneBoneSolver& value, u32 version) {
    (void)version;
    visit(value.dependents);
    value.bone = reader.read<u16>();
    value.boneFallback = reader.read<u16>();
    value.flags = reader.read<Flag>();
    value.maxAngle = reader.read<f32>();
}
