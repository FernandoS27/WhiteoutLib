// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

void BinaryParseVisitor::visit(ConvexHullHalfEdge& value, u32 version) {
    (void)version;
    value.type = reader.read<u8>();
    value.faceIndex = reader.read<u8>();
    value.vertexIndex = reader.read<u8>();
    value.nextAroundVertex = reader.read<u8>();
}

void BinaryParseVisitor::visit(PhysicsMeshNormal& value, u32 version) {
    (void)version;
    value.normal = reader.read<Vector3f>();
}

void BinaryParseVisitor::visit(PhysicsMeshTriangle& value, u32 version) {
    (void)version;
    value.vertexIndex0 = reader.read<u32>();
    value.vertexIndex1 = reader.read<u32>();
    value.vertexIndex2 = reader.read<u32>();
    value.edgeIndex0 = reader.read<u32>();
    value.edgeIndex1 = reader.read<u32>();
    value.edgeIndex2 = reader.read<u32>();
    value.reserved = reader.read<u16>();
    value.flags = reader.read<u16>();
}

void BinaryParseVisitor::visit(PhysicsMeshEdge& value, u32 version) {
    (void)version;
    value.edgeType = reader.read<u32>();
    value.vertexA = reader.read<u32>();
    value.vertexB = reader.read<u32>();
    value.faceA = reader.read<u32>();
    value.faceB = reader.read<u32>();
}

void BinaryParseVisitor::visit(PhysicsShape& value, u32 version) {
    (void)version;
    value.transform = reader.read<Matrix44f>();
    if (version <= 1) {
        value.collisionMargin = reader.read<f32>();
        value.shapeType = static_cast<PhysicsShapeType>(reader.read<u8>());
        reader.skip(3);
        visit(value.deprecated.v1.legacyVertices);
        visit(value.deprecated.v1.unknown0);
        visit(value.deprecated.v1.faceIndices);
        visit(value.deprecated.v1.planeEquations);
        value.deprecated.v1.halfExtents = reader.read<Vector3f>();
        return;
    }

    // Version 2+
    value.shapeType = static_cast<PhysicsShapeType>(reader.read<u8>());
    reader.skip(3);

    value.oldSizes = reader.read<Vector3f>();

    value.reserved0 = readReferenceFunc();
    value.shapeDimensions = reader.read<Vector3f>();
    visit(value.hullFaceNormals);
    visit(value.hullVertexPositions);
    visit(value.hullHalfEdges);
    visit(value.hullVertexFaceIndices);
    value.hullCenter = reader.read<Vector3f>();
    value.hullFaceNormalCount = reader.read<u32>();
    value.hullVertexCount = reader.read<u32>();
    value.hullHalfEdgeCount = reader.read<u32>();
    value.hullUnknown0 = reader.read<f32>();
    value.hullUnknown1 = reader.read<f32>();

    if (version >= 3) {
        visit(value.meshFaceNormals);
        visit(value.meshVertexPositions);
        visit(value.meshFaceIndices16);
        visit(value.meshFaceIndices32);
    }
    value.meshBoundsCenter = reader.read<Vector3f>();
    value.meshBoundsExtent = reader.read<Vector3f>();
    value.meshTolerance = reader.read<Vector3f>();
    if (version == 2) {
        visit(value.deprecated.v2.meshFaceNormals);
        visit(value.deprecated.v2.meshVertexPositions);
        visit(value.deprecated.v2.unknown);
        visit(value.deprecated.v2.unknown2);
    }
    value.meshNormalCount = reader.read<u32>();
    value.meshVertexCount = reader.read<u32>();
    value.meshFaceIndex16Count = reader.read<u32>();
    value.meshFaceIndex32Count = reader.read<u32>();
    value.meshUnknown1 = reader.read<u32>();
    value.meshReserved = reader.read<u32>();
    value.meshTreeDepth = reader.read<u32>();
    value.meshCollisionMargin = reader.read<f32>();
}

void BinaryParseVisitor::visit(RigidBody& value, u32 version) {
    if (version <= 2) {
        // Legacy Havok-era layout: 80 bytes base + Ref<PHSH> + 12 bytes post-ref
        value.density = reader.read<f32>();
        value.friction = reader.read<f32>();
        value.restitution = reader.read<f32>();
        value.linearDamping = reader.read<f32>();
        value.angularDamping = reader.read<f32>();
        value.gravityScale = reader.read<f32>();
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                value.deprecated.inertiaTensor[r][c] = reader.read<f32>();
        value.parentBoneIndex = reader.read<u16>();
        value.deprecated.boneIndex = reader.read<u16>();
        value.deprecated.reserved = reader.readArray<u32, 4>();
    } else {
        value.simulationType = reader.read<u16>();
        value.parentBoneIndex = reader.read<u16>();
        value.physicsType = reader.read<u32>();
        value.density = reader.read<f32>();
        value.friction = reader.read<f32>();
        value.restitution = reader.read<f32>();
        value.linearDamping = reader.read<f32>();
        value.angularDamping = reader.read<f32>();
        value.gravityScale = reader.read<f32>();
        if (version >= 4) {
            value.dynamicState = reader.read<AnimRef<u32>>();
            value.dynamicBlendOut = reader.read<f32>();
        }
    }
    visit(value.rigidBodyShape);
    value.flags = static_cast<RigidBodyFlag>(reader.read<u32>());
    value.localForces = reader.read<u16>();
    value.worldForces = reader.read<u16>();
    value.priority = reader.read<u32>();
}

void BinaryParseVisitor::visit(PhysicsJoint& value, u32 version) {
    (void)version;
    value.jointType = reader.read<u32>();
    value.boneIndex1 = reader.read<u32>();
    value.boneIndex2 = reader.read<u32>();
    value.matrixBody1 = reader.read<Matrix44f>();
    value.matrixBody2 = reader.read<Matrix44f>();
    value.enableLimits = reader.read<u32>();
    value.limitMin = reader.read<f32>();
    value.limitMax = reader.read<f32>();
    value.coneAngle = reader.read<f32>();
    value.enableFriction = reader.read<u32>();
    value.friction = reader.read<f32>();
    value.dampingRatio = reader.read<f32>();
    value.angularFrequency = reader.read<f32>();
    value.breakThreshold = reader.read<f32>();
    value.enableShape = reader.read<u8>();
    reader.skip(3);
}

void BinaryParseVisitor::visit(PhysicsConstraint& value, u32 version) {
    (void)version;
    visit(value.dependents);
    value.rigidBody1 = reader.read<u16>();
    value.rigidBody2 = reader.read<u16>();
    value.flags = reader.read<Flag>();
    value.breakForce = reader.read<f32>();
}

void BinaryParseVisitor::visit(ClothCollider& value, u32 version) {
    (void)version;
    value.transform = reader.read<Matrix44f>();
    value.radius = reader.read<f32>();
    value.height = reader.read<f32>();
    value.padding = reader.read<u32>();
}

void BinaryParseVisitor::visit(ClothProxy& value, u32 version) {
    (void)version;
    value.proxyIndex = reader.read<u32>();
    value.clothIndex = reader.read<u32>();
    visit(value.proxyVertices);
    visit(value.proxyWeights);
}

void BinaryParseVisitor::visit(ClothPhysics& value, u32 version) {
    (void)version;
    value.clothMeshCount = reader.read<u32>();
    value.skinBoneCount = reader.read<u32>();
    visit(value.skinBones);
    visit(value.simEnabled);
    visit(value.vertexBones);
    visit(value.vertexWeights);
    visit(value.colliders);
    visit(value.proxies);
    value.density = reader.read<f32>();
    value.tracking = reader.read<f32>();
    value.stretchStiffness = reader.read<f32>();
    value.horizontalStiffness = reader.read<f32>();
    value.bendingStiffness = reader.read<f32>();
    value.damping = reader.read<f32>();
    value.friction = reader.read<f32>();
    value.gravity = reader.read<f32>();
    value.explosionScale = reader.read<f32>();
    value.windScale = reader.read<f32>();
    value.shearStiffness = reader.read<f32>();
    value.dragFactor = reader.read<f32>();
    value.liftFactor = reader.read<f32>();
    value.sphereStiffness = reader.read<f32>();
    if (version >= 4) {
        value.flatten = reader.read<u32>();
        value.active = reader.read<AnimRef<u32>>();
        value.useSkinCollision = reader.read<u32>();
        value.skinOffset = reader.read<f32>();
        value.skinExponent = reader.read<f32>();
        value.skinStiffness = reader.read<f32>();
        value.localChannels = reader.read<u32>();
        value.localWind = reader.read<Vector3f>();
    }
}
