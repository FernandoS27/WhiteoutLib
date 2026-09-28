// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

// Every physics record is read into its current layout. What an older version
// lacks is left for `UpgradePhysics` (physics_upgrade.cpp), which applies the
// SC2 client's defaults by the version stamped here; what it has that the
// current layout does not is consumed and dropped, as the client's converters
// drop it (M3_UpgradePHRB / PHSH / PHCL, SC2 5.0).

void BinaryParseVisitor::visit(ConvexHullHalfEdge& value, u32 version) {
    (void)version;
    value.twinOffset = reader.read<i8>();
    value.originVertex = reader.read<u8>();
    value.face = reader.read<u8>();
    value.nextInFace = reader.read<u8>();
}

void BinaryParseVisitor::visit(PhysicsMeshBvhNode& value, u32 version) {
    if (version >= 1) {
        value.octahedral.octX = reader.read<i16>();
        value.octahedral.octY = reader.read<i16>();
        value.octahedral.slabMin = reader.read<u16>();
        value.octahedral.slabMax = reader.read<u16>();
    } else {
        value.normal = reader.read<Vector3f>();
    }
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
    value.transform = reader.read<Matrix44f>();
    if (version <= 1) {
        // v0 96 bytes, v1 132: Havok's convex radius, the kind, the source
        // points, and on v1 a U8 table, the source triangle list and a plane
        // table. The client keeps the points, the triangles and the
        // dimensions; the rest is dropped.
        reader.skip(4);
        value.shapeType = static_cast<PhysicsShapeType>(reader.read<u8>());
        reader.skip(3);
        visit(value.sourcePoints);
        if (version == 1) {
            std::vector<u8> unused;
            visit(unused);
            visit(value.sourceTriangles);
            std::vector<Vector4f> planes;
            visit(planes);
        }
        value.shapeDimensions = reader.read<Vector3f>();
        return;
    }

    value.shapeType = static_cast<PhysicsShapeType>(reader.read<u8>());
    reader.skip(3);
    visit(value.sourcePoints);
    visit(value.sourceTriangles);
    value.shapeDimensions = reader.read<Vector3f>();
    visit(value.hullVertices);
    visit(value.hullPlanes);
    visit(value.hullHalfEdges);
    visit(value.hullFaceFirstEdges);
    value.hullCentroid = reader.read<Vector3f>();
    value.hullVertexCount = reader.read<u32>();
    value.hullFaceCount = reader.read<u32>();
    value.hullHalfEdgeCount = reader.read<u32>();
    value.hullVolume = reader.read<f32>();
    value.hullSurfaceArea = reader.read<f32>();

    if (version >= 3) {
        visit(value.meshBvhNodes);
        visit(value.meshVertexPositions);
        visit(value.meshFaceIndices16);
        visit(value.meshFaceIndices32);
    }
    value.meshBoundsCenter = reader.read<Vector3f>();
    value.meshBoundsExtent = reader.read<Vector3f>();
    value.meshTolerance = reader.read<Vector3f>();
    if (version == 2) {
        // v2 (292 bytes): a Havok tree, centre-relative VEC3 vertices, DMMT
        // triangles and DMME edges. The upgrade rebuilds the mesh from the
        // vertices (plus the centre) and each triangle's three indices, up
        // to the tail's counts; the tree and the edges are dropped.
        std::vector<PhysicsMeshBvhNode> havokTree;
        visit(havokTree);
        std::vector<Vector3f> positions;
        visit(positions);
        value.meshVertexPositions.reserve(positions.size());
        for (const auto& v : positions) {
            value.meshVertexPositions.push_back(Vector4f{v.x, v.y, v.z, 0.0f});
        }
        std::vector<PhysicsMeshTriangle> triangles;
        visit(triangles);
        value.meshFaceIndices32.reserve(triangles.size());
        for (const auto& t : triangles) {
            value.meshFaceIndices32.push_back(
                {t.vertexIndex0, t.vertexIndex1, t.vertexIndex2, 0, 0, 0, 0});
        }
        std::vector<PhysicsMeshEdge> edges;
        visit(edges);
        reader.skip(4);
        value.meshVertexCount = reader.read<u32>();
        value.meshFaceIndex32Count = reader.read<u32>();
        reader.skip(8);
        value.meshTreeDepth = reader.read<u32>();
        return;
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
        // Havok era: a material the upgrade discards (6 floats), on v1/v2 an
        // inertia tensor, the bone twice and four reserved words. v0 is 72
        // bytes, v1 96, v2 104.
        reader.skip(version == 0 ? 36 : 60);
        value.parentBoneIndex = reader.read<u16>();
        reader.skip(18);
        visit(value.rigidBodyShape);
        if (version <= 1) {
            // Four bytes: the local force channel's bit, whether it feels world
            // forces, and on v1 its flags (v0: collidable).
            const u8 localBit = reader.read<u8>();
            const u8 world = reader.read<u8>();
            const u8 flags = reader.read<u8>();
            reader.skip(1);
            value.localForces = localBit != 0 ? static_cast<u16>(1u << localBit) : u16{0};
            value.worldForces = world != 0 ? u16{1} : u16{0};
            value.flags = static_cast<RigidBodyFlag>(version == 1 ? flags : 1u);
            return;
        }
    } else {
        value.simulationType = reader.read<u16>();
        value.parentBoneIndex = reader.read<u16>();
        value.physicsType = reader.read<u32>();
        value.density = reader.read<f32>();
        value.friction = reader.read<f32>();
        value.restitution = reader.read<f32>();
        value.linearDamping = reader.read<f32>();
        value.angularDamping = reader.read<f32>();
        value.inertiaScale = reader.read<f32>();
        if (version >= 4) {
            value.dynamicState = reader.read<AnimRef<u32>>();
            value.dynamicBlendOut = reader.read<f32>();
        }
        visit(value.rigidBodyShape);
    }
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

void BinaryParseVisitor::visit(ClothCollider& value, u32 version) {
    (void)version;
    value.transform = reader.read<Matrix44f>();
    value.radius = reader.read<f32>();
    value.height = reader.read<f32>();
    value.bone = reader.read<u32>();
}

void BinaryParseVisitor::visit(ClothProxy& value, u32 version) {
    (void)version;
    value.proxyIndex = reader.read<u32>();
    value.clothIndex = reader.read<u32>();
    visit(value.proxyVertices);
    visit(value.proxyWeights);
}

void BinaryParseVisitor::visit(ClothPhysics& value, u32 version) {
    // Four older layouts (M3_UpgradePHCL): v0 140 bytes carries two extra Refs
    // after `skinBones` and no PHAC Ref; v1 116 has no PHAC Ref; v2 128 has an
    // extra dword after `tracking` and after `gravity` and ends at `windScale`;
    // v3 192 keeps the gravity dword and orders its tail differently.
    value.cageRegion = reader.read<u32>();
    value.skinBoneCount = reader.read<u32>();
    visit(value.skinBones);
    if (version == 0) {
        skipReference();
        skipReference();
    }
    visit(value.simEnabled);
    visit(value.vertexBones);
    visit(value.vertexWeights);
    visit(value.colliders);
    if (version >= 2) {
        visit(value.proxies);
    }
    value.density = reader.read<f32>();
    value.tracking = reader.read<f32>();
    if (version <= 2) {
        reader.skip(4);
    }
    value.stretchStiffness = reader.read<f32>();
    value.horizontalStiffness = reader.read<f32>();
    value.bendingStiffness = reader.read<f32>();
    value.damping = reader.read<f32>();
    value.friction = reader.read<f32>();
    value.gravity = reader.read<f32>();
    if (version <= 3) {
        reader.skip(4);
    }
    value.explosionScale = reader.read<f32>();
    value.windScale = reader.read<f32>();
    if (version <= 2) {
        return;
    }
    value.shearStiffness = reader.read<f32>();
    value.dragFactor = reader.read<f32>();
    value.liftFactor = reader.read<f32>();
    if (version == 3) {
        reader.skip(4);
        value.active = reader.read<AnimRef<u32>>();
        reader.skip(8);
        value.sphereStiffness = reader.read<f32>();
        value.useSkinCollision = reader.read<u32>();
        value.skinOffset = reader.read<f32>();
        value.skinExponent = reader.read<f32>();
        value.skinStiffness = reader.read<f32>();
        value.localChannels = reader.read<u32>();
        return;
    }
    value.sphereStiffness = reader.read<f32>();
    value.flatten = reader.read<u32>();
    value.active = reader.read<AnimRef<u32>>();
    value.useSkinCollision = reader.read<u32>();
    value.skinOffset = reader.read<f32>();
    value.skinExponent = reader.read<f32>();
    value.skinStiffness = reader.read<f32>();
    value.localChannels = reader.read<u32>();
    value.localWind = reader.read<Vector3f>();
}
