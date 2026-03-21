// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

void BinaryWriterVisitor::visit(const Light& light, u32 version) {
    (void)version;
    writer.write(light.lightType);
    writer.write(light.boneIndex);
    writer.write(light.flags);
    writer.write(light.lodCut);
    writer.write(light.shadowLodCut);
    writer.write(light.diffuseColor);
    writer.write(light.intensityMultiplier);
    writer.write(light.specularColor);
    writer.write(light.specularMultiplier);
    writer.write(light.decay);
    writer.write(light.attenuationEnd);
    writer.write(light.attenuationStart);
    writer.write(light.hotSpot);
    writer.write(light.falloff);
}

void BinaryWriterVisitor::visit(const Camera& camera, u32 version) {
    writer.write(camera.boneIndex);
    visit(camera.name);

    if (version >= 2) {
        writer.write(camera.fieldOfView);
        writer.write(camera.useVerticalFOV);
    }

    if (version == 2) {
        // Beta v2: farClip/nearClip are plain f32 (written from AnimRef.initValue)
        writer.write(camera.farClip.initValue);
        writer.write(camera.nearClip.initValue);
    }

    if (version >= 5) {
        writer.write(camera.dofType);
    }

    if (version >= 3) {
        writer.write(camera.farClip);
        writer.write(camera.nearClip);
    }

    if (version >= 2) {
        writer.write(camera.shadowClipDistance);
        writer.write(camera.focusDistance);
        writer.write(camera.farFocusRange);
        writer.write(camera.nearFocusRange);
    }

    if (version >= 4) {
        writer.write(camera.nearFalloffStart);
        writer.write(camera.nearFalloffEnd);
    }

    if (version >= 2) {
        writer.write(camera.dofAmount);
    }

    if (version >= 5) {
        writer.write(camera.bokehFStop);
        writer.write(camera.bokehMaxCoCDiameter);
    }
}

void BinaryWriterVisitor::visit(const Event& event, u32 version) {
    visit(event.name);
    writer.write(event.unknown);
    writer.write(event.boneIndex);
    writer.write(event.padding);
    writer.write(event.transform);
    writer.write(event.eventType);
    visit(event.optionString);
    if (version >= 1) {
        writer.write(event.rttChannelIndex);
    }
    if (version >= 2) {
        writer.write(event.extraParameter);
    }
}

void BinaryWriterVisitor::visit(const ShadowBox& box, u32 version) {
    (void)version;
    writer.write(box.matrix);
}

void BinaryWriterVisitor::visit(const Projector& projector, u32 version) {
    (void)version;
    writer.write(projector.projectionType);
    writer.write(projector.bone);
    writer.write(projector.materialReferenceIndex);
    writer.write(projector.offset);
    writer.write(projector.pitch);
    writer.write(projector.yaw);
    writer.write(projector.roll);
    writer.write(projector.fieldOfView);
    writer.write(projector.aspectRatio);
    writer.write(projector.near);
    writer.write(projector.far);
    writer.write(projector.boxOffsetZBottom);
    writer.write(projector.boxOffsetZTop);
    writer.write(projector.boxOffsetXLeft);
    writer.write(projector.boxOffsetXRight);
    writer.write(projector.boxOffsetYFront);
    writer.write(projector.boxOffsetYBack);
    writer.write(projector.falloff);
    writer.write(projector.alphaInit);
    writer.write(projector.alphaMid);
    writer.write(projector.alphaEnd);
    writer.write(projector.lifetimeAttack);
    writer.write(projector.lifetimeAttackTo);
    writer.write(projector.lifetimeHold);
    writer.write(projector.lifetimeHoldTo);
    writer.write(projector.lifetimeDecay);
    writer.write(projector.lifetimeDecayTo);
    writer.write(projector.attenuationDistance);
    writer.write(projector.active);
    writer.write(projector.layer);
    writer.write(projector.lodReduce);
    writer.write(projector.lodCut);
    writer.write(projector.flags);
}

void BinaryWriterVisitor::visit(const Force& force, u32 version) {
    (void)version;
    writer.write(force.forceType);
    writer.write(force.forceShape);
    writer.write(force.unknown);
    writer.write(force.boneIndex);
    writer.write(force.flags);
    writer.write(force.localChannels);
    writer.write(force.strength);
    writer.write(force.width);
    writer.write(force.height);
    writer.write(force.length);
}

void BinaryWriterVisitor::visit(const Warp& warp, u32 version) {
    (void)version;
    writer.write(warp.warpType);
    writer.write(warp.boneIndex);
    writer.write(warp.unknown);
    writer.write(warp.radius);
    writer.write(warp.height);
    writer.write(warp.strength);
    writer.write(warp.angular);
    writer.write(warp.axial);
    writer.write(warp.radial);
}

void BinaryWriterVisitor::visit(const ViewVolume& volume, u32 version) {
    (void)version;
    writer.write(volume.nodeIndex);
    writer.write(volume.size);
}

void BinaryWriterVisitor::visit(const TrailingModel& model, u32 version) {
    (void)version;
    visit(model.vectors);
    writer.write(model.param0);
    writer.write(model.param1);
    writer.write(model.animFloat0);
    writer.write(model.animFloat1);
    writer.write(model.flag);
    writer.write(model.reserved0);
    writer.write(model.reserved1);
}

void BinaryWriterVisitor::visit(const HitTestShape& shape, u32 version) {
    (void)version;
    writer.write(shape.shapeType);
    writer.write(shape.boneIndex);
    writer.write(shape.padding);
    writer.write(shape.transform);
    visit(shape.vertexPositions);
    visit(shape.faceIndices);
    writer.write(shape.sizeX);
    writer.write(shape.sizeY);
    writer.write(shape.sizeZ);
}

void BinaryWriterVisitor::visit(const AttachmentVolume& volume, u32 version) {
    (void)version;
    writer.write(volume.bone1);
    writer.write(volume.bone2);
    writer.write(volume.shapeType);
    writer.write(volume.boneIndex);
    writer.write(volume.padding);
    writer.write(volume.transform);
    visit(volume.vertexPositions);
    visit(volume.faceIndices);
    writer.write(volume.sizeX);
    writer.write(volume.sizeY);
    writer.write(volume.sizeZ);
}

void BinaryWriterVisitor::visit(const TriggerData& trigger, u32 version) {
    (void)version;
    visit(trigger.dataIndices);
    visit(trigger.name);
}

void BinaryWriterVisitor::visit(const TurretBehavior& behavior, u32 version) {
    writer.write(behavior.transform);
    if (version >= 4) {
        writer.write(behavior.unknown1);
        writer.write(behavior.unknown2);
    }
    writer.write(behavior.boneIndex);
    writer.write(behavior.useAsMainTurret);
    writer.write(behavior.turretGroupId);
    writer.write(behavior.yawLimited);
    writer.write(behavior.yawMin);
    writer.write(behavior.yawMax);
    if (version >= 4) {
        writer.write(behavior.yawWeight);
    }
    writer.write(behavior.pitchLimited);
    writer.write(behavior.pitchMin);
    writer.write(behavior.pitchMax);
    if (version >= 4) {
        writer.write(behavior.pitchWeight);
    }
    writer.write(behavior.unknown3);
    writer.write(behavior.unknown4);
    writer.write(behavior.mainBoneOffset);
}

void BinaryWriterVisitor::visit(const BillboardBehavior& behavior, u32 version) {
    (void)version;
    visit(behavior.dependents);
    writer.write(behavior.boneIndex);
    writer.write(behavior.billboardType);
    writer.write(behavior.cameraLookAt);
    writer.write(behavior.up);
    writer.write(behavior.forward);
}

void BinaryWriterVisitor::visit(const IKJoint& joint, u32 version) {
    (void)version;
    visit(joint.dependents);
    writer.write(joint.boneIndex1);
    writer.write(joint.boneIndex2);
    writer.write(joint.raycastUp);
    writer.write(joint.raycastDown);
    writer.write(joint.maxSpeed);
    writer.write(joint.goalThreshold);
}

void BinaryWriterVisitor::visit(const IKTwoJoint& joint, u32 version) {
    (void)version;
    visit(joint.dependents);
    writer.write(joint.boneBase);
    writer.write(joint.boneTarget);
    writer.write(joint.boneEnd);
    writer.write(joint.padding);
    writer.write(joint.hingeAxis);
    writer.write(joint.maxAngleInner);
    writer.write(joint.maxAngleOuter);
    writer.write(joint.searchUp);
    writer.write(joint.searchDown);
}

void BinaryWriterVisitor::visit(const IKCCD& ccd, u32 version) {
    (void)version;
    visit(ccd.dependents);
    writer.write(ccd.boneBase);
    writer.write(ccd.boneTarget);
    writer.write(ccd.searchUp);
    writer.write(ccd.searchDown);
}

void BinaryWriterVisitor::visit(const OneBoneSolver& solver, u32 version) {
    (void)version;
    visit(solver.dependents);
    writer.write(solver.bone);
    writer.write(solver.boneFallback);
    writer.write(solver.flags);
    writer.write(solver.maxAngle);
}
