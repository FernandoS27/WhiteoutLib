// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

// Physics chunks are always written at their current version (the traits'
// `writes_current`): the parser upgrades every older one on read, so there is
// no older layout left to spell.

void BinaryWriterVisitor::visit(const ConvexHullHalfEdge& edge, u32 version) {
    (void)version;
    writer.write(edge.twinOffset);
    writer.write(edge.originVertex);
    writer.write(edge.face);
    writer.write(edge.nextInFace);
}

void BinaryWriterVisitor::visit(const PhysicsMeshBvhNode& normal, u32 version) {
    (void)version;
    writer.write(normal.octahedral.octX);
    writer.write(normal.octahedral.octY);
    writer.write(normal.octahedral.slabMin);
    writer.write(normal.octahedral.slabMax);
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
    (void)version;
    writer.write(shape.transform);
    writer.write(shape.shapeType);
    writer.write<u8>(0);
    writer.write<u8>(0);
    writer.write<u8>(0);
    visit(shape.sourcePoints);
    visit(shape.sourceTriangles);
    writer.write(shape.shapeDimensions);
    visit(shape.hullVertices);
    visit(shape.hullPlanes);
    visit(shape.hullHalfEdges);
    visit(shape.hullFaceFirstEdges);
    writer.write(shape.hullCentroid);
    writer.write(shape.hullVertexCount);
    writer.write(shape.hullFaceCount);
    writer.write(shape.hullHalfEdgeCount);
    writer.write(shape.hullVolume);
    writer.write(shape.hullSurfaceArea);
    visit(shape.meshBvhNodes);
    visit(shape.meshVertexPositions);
    visit(shape.meshFaceIndices16);
    visit(shape.meshFaceIndices32);
    writer.write(shape.meshBoundsCenter);
    writer.write(shape.meshBoundsExtent);
    writer.write(shape.meshTolerance);
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
    (void)version;
    writer.write(body.simulationType);
    writer.write(body.parentBoneIndex);
    writer.write(body.physicsType);
    writer.write(body.density);
    writer.write(body.friction);
    writer.write(body.restitution);
    writer.write(body.linearDamping);
    writer.write(body.angularDamping);
    writer.write(body.inertiaScale);
    writer.write(body.dynamicState);
    writer.write(body.dynamicBlendOut);
    visit(body.rigidBodyShape);
    writer.write(body.flags);
    writer.write(body.localForces);
    writer.write(body.worldForces);
    writer.write(body.priority);
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
    writer.write(cloth.cageRegion);
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

void BinaryWriterVisitor::visit(const ClothCollider& collider, u32 version) {
    (void)version;
    writer.write(collider.transform);
    writer.write(collider.radius);
    writer.write(collider.height);
    writer.write(collider.bone);
}

void BinaryWriterVisitor::visit(const ClothProxy& proxy, u32 version) {
    (void)version;
    writer.write(proxy.proxyIndex);
    writer.write(proxy.clothIndex);
    visit(proxy.proxyVertices);
    visit(proxy.proxyWeights);
}
