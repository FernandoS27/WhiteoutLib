// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

void BinaryWriterVisitor::visit(const ConvexHullHalfEdge& edge, u32 version) {
    (void)version;
    writer.write(edge.type);
    writer.write(edge.faceIndex);
    writer.write(edge.vertexIndex);
    writer.write(edge.nextAroundVertex);
}

void BinaryWriterVisitor::visit(const PhysicsMeshBvhNode& normal, u32 version) {
    if (version >= 1) {
        writer.write(normal.octahedral.octX);
        writer.write(normal.octahedral.octY);
        writer.write(normal.octahedral.slabMin);
        writer.write(normal.octahedral.slabMax);
    } else {
        writer.write(normal.normal);
    }
}

void BinaryWriterVisitor::visit(const PhysicsMeshTriangle& triangle, u32 version) {
    (void)version;
    writer.write(triangle.vertexIndex0);
    writer.write(triangle.vertexIndex1);
    writer.write(triangle.vertexIndex2);
    writer.write(triangle.edgeIndex0);
    writer.write(triangle.edgeIndex1);
    writer.write(triangle.edgeIndex2);
    writer.write(triangle.reserved);
    writer.write(triangle.flags);
}

void BinaryWriterVisitor::visit(const PhysicsMeshEdge& edge, u32 version) {
    (void)version;
    writer.write(edge.edgeType);
    writer.write(edge.vertexA);
    writer.write(edge.vertexB);
    writer.write(edge.faceA);
    writer.write(edge.faceB);
}

void BinaryWriterVisitor::visit(const PhysicsShape& shape, u32 version) {
    writer.write(shape.transform);
    if (version <= 1) {
        writer.write(shape.collisionMargin);
        writer.write(shape.shapeType);
        writer.write<u8>(0);
        writer.write<u8>(0);
        writer.write<u8>(0);
        visit(shape.deprecated.v1.legacyVertices);
        visit(shape.deprecated.v1.unknown0);
        visit(shape.deprecated.v1.faceIndices);
        visit(shape.deprecated.v1.planeEquations);
        writer.write(shape.deprecated.v1.halfExtents);
        return;
    }

    // Version 2+
    writer.write(shape.shapeType);
    writer.write<u8>(0);
    writer.write<u8>(0);
    writer.write<u8>(0);
    writer.write(shape.oldSizes);

    writer.write(shape.reserved0);
    writer.write(shape.shapeDimensions);
    visit(shape.hullFaceNormals);
    visit(shape.hullVertexPositions);
    visit(shape.hullHalfEdges);
    visit(shape.hullVertexFaceIndices);
    writer.write(shape.hullCenter);
    writer.write(shape.hullFaceNormalCount);
    writer.write(shape.hullVertexCount);
    writer.write(shape.hullHalfEdgeCount);
    writer.write(shape.hullUnknown0);
    writer.write(shape.hullUnknown1);

    if (version >= 3) {
        visit(shape.meshBvhNodes);
        visit(shape.meshVertexPositions);
        visit(shape.meshFaceIndices16);
        visit(shape.meshFaceIndices32);
    }
    writer.write(shape.meshBoundsCenter);
    writer.write(shape.meshBoundsExtent);
    writer.write(shape.meshTolerance);
    if (version == 2) {
        visit(shape.deprecated.v2.meshBvhNodes);
        visit(shape.deprecated.v2.meshVertexPositions);
        visit(shape.deprecated.v2.unknown);
        visit(shape.deprecated.v2.unknown2);
    }
    writer.write(shape.meshNormalCount);
    writer.write(shape.meshVertexCount);
    writer.write(shape.meshFaceIndex16Count);
    writer.write(shape.meshFaceIndex32Count);
    writer.write(shape.meshUnknown1);
    writer.write(shape.meshReserved);
    writer.write(shape.meshTreeDepth);
    writer.write(shape.meshCollisionMargin);
}

void BinaryWriterVisitor::visit(const RigidBody& body, u32 version) {
    if (version <= 2) {
        // Legacy Havok-era layout: 80 bytes base + Ref<PHSH> + 12 bytes post-ref
        writer.write(body.density);
        writer.write(body.friction);
        writer.write(body.restitution);
        writer.write(body.linearDamping);
        writer.write(body.angularDamping);
        writer.write(body.gravityScale);
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                writer.write(body.deprecated.inertiaTensor[r][c]);
        writer.write(body.parentBoneIndex);
        writer.write(body.deprecated.boneIndex);
        writer.write(body.deprecated.reserved);
    } else {
        writer.write(body.simulationType);
        writer.write(body.parentBoneIndex);
        writer.write(body.physicsType);
        writer.write(body.density);
        writer.write(body.friction);
        writer.write(body.restitution);
        writer.write(body.linearDamping);
        writer.write(body.angularDamping);
        writer.write(body.gravityScale);
        if (version >= 4) {
            writer.write(body.dynamicState);
            writer.write(body.dynamicBlendOut);
        }
    }
    visit(body.rigidBodyShape);
    writer.write(body.flags);
    writer.write(body.localForces);
    writer.write(body.worldForces);
    writer.write(body.priority);
}

void BinaryWriterVisitor::visit(const PhysicsConstraint& constraint, u32 version) {
    (void)version;
    visit(constraint.dependents);
    writer.write(constraint.rigidBody1);
    writer.write(constraint.rigidBody2);
    writer.write(constraint.flags);
    writer.write(constraint.breakForce);
}

void BinaryWriterVisitor::visit(const PhysicsJoint& joint, u32 version) {
    (void)version;
    writer.write(joint.jointType);
    writer.write(joint.boneIndex1);
    writer.write(joint.boneIndex2);
    writer.write(joint.matrixBody1);
    writer.write(joint.matrixBody2);
    writer.write(joint.enableLimits);
    writer.write(joint.limitMin);
    writer.write(joint.limitMax);
    writer.write(joint.coneAngle);
    writer.write(joint.enableFriction);
    writer.write(joint.friction);
    writer.write(joint.dampingRatio);
    writer.write(joint.angularFrequency);
    writer.write(joint.breakThreshold);
    writer.write(joint.enableShape);
    writer.write<u8>(0);
    writer.write<u8>(0);
    writer.write<u8>(0);
}

void BinaryWriterVisitor::visit(const ClothPhysics& cloth, u32 version) {
    (void)version;
    writer.write(cloth.clothMeshCount);
    writer.write(cloth.skinBoneCount);
    visit(cloth.skinBones);
    visit(cloth.simEnabled);
    visit(cloth.vertexBones);
    visit(cloth.vertexWeights);
    visit(cloth.colliders);
    visit(cloth.proxies);
    writer.write(cloth.density);
    writer.write(cloth.tracking);
    writer.write(cloth.stretchStiffness);
    writer.write(cloth.horizontalStiffness);
    writer.write(cloth.bendingStiffness);
    writer.write(cloth.damping);
    writer.write(cloth.friction);
    writer.write(cloth.gravity);
    writer.write(cloth.explosionScale);
    writer.write(cloth.windScale);
    writer.write(cloth.shearStiffness);
    writer.write(cloth.dragFactor);
    if (version >= 4) {
        writer.write(cloth.liftFactor);
        writer.write(cloth.sphereStiffness);
        writer.write(cloth.flatten);
        writer.write(cloth.active);
        writer.write(cloth.useSkinCollision);
        writer.write(cloth.skinOffset);
        writer.write(cloth.skinExponent);
        writer.write(cloth.skinStiffness);
        writer.write(cloth.localChannels);
        writer.write(cloth.localWind);
    }
}

void BinaryWriterVisitor::visit(const ClothCollider& collider, u32 version) {
    (void)version;
    writer.write(collider.transform);
    writer.write(collider.radius);
    writer.write(collider.height);
    writer.write(collider.padding);
}

void BinaryWriterVisitor::visit(const ClothProxy& proxy, u32 version) {
    (void)version;
    writer.write(proxy.proxyIndex);
    writer.write(proxy.clothIndex);
    visit(proxy.proxyVertices);
    visit(proxy.proxyWeights);
}
