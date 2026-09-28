// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/m3/physics_cook.h>
#include <whiteout/models/m3/physics_upgrade.h>

#include <algorithm>

// The source points are transformed in the client's float order.
#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(_MSC_VER)
#pragma fp_contract(off)
#endif

namespace whiteout {
namespace m3 {
namespace {

/// `ModelAsset_CookPhysicsAndCloth` for a shape whose source Refs are
/// filled: the matrix goes into the points, the matrix becomes identity, and a
/// hull or a mesh is cooked from them. Other kinds only lose the sources.
void CookSources(PhysicsShape& shape, std::size_t body, std::vector<std::string>& issues) {
    const bool hull = shape.shapeType == PhysicsShapeType::ConvexHull;
    const bool mesh = shape.shapeType == PhysicsShapeType::Mesh;
    if (hull || mesh) {
        std::vector<Vector3f> points;
        points.reserve(shape.sourcePoints.size());
        for (const Vector3f& p : shape.sourcePoints) {
            points.push_back(TransformPointRowMajor(shape.transform, p));
        }
        if (hull) {
            const HullCookReport report = CookHull(shape, points, HullCook::ClientLoad);
            if (!report.ok) {
                issues.push_back("PHRB " + std::to_string(body) + ": a hull's " +
                                 std::to_string(points.size()) + " source points span no volume.");
            }
        } else {
            std::vector<u32> triangles;
            const std::size_t count = shape.sourceTriangles.size() / 3 * 3;
            triangles.reserve(count);
            for (std::size_t i = 0; i < count; ++i) {
                triangles.push_back(shape.sourceTriangles[i]);
            }
            CookMesh(shape, points, triangles);
        }
        shape.transform = Matrix44f::identity();
    }
    shape.sourcePoints.clear();
    shape.sourceTriangles.clear();
}

/// A v2 mesh: vertices stored about the tree centre, triangles as the first
/// three indices of Havok's, both limited by the record's counts.
void RebuildHavokMesh(PhysicsShape& shape) {
    const std::size_t vertexCount = std::min<std::size_t>(shape.meshVertexCount, shape.meshVertexPositions.size());
    const std::size_t faceCount = std::min<std::size_t>(shape.meshFaceIndex32Count, shape.meshFaceIndices32.size());
    std::vector<Vector3f> vertices;
    vertices.reserve(vertexCount);
    const Vector3f c = shape.meshBoundsCenter;
    for (std::size_t i = 0; i < vertexCount; ++i) {
        const Vector4f& v = shape.meshVertexPositions[i];
        vertices.push_back({v.x + c.x, v.y + c.y, v.z + c.z});
    }
    std::vector<u32> triangles;
    triangles.reserve(faceCount * 3);
    for (std::size_t i = 0; i < faceCount; ++i) {
        triangles.push_back(shape.meshFaceIndices32[i][0]);
        triangles.push_back(shape.meshFaceIndices32[i][1]);
        triangles.push_back(shape.meshFaceIndices32[i][2]);
    }
    CookMesh(shape, vertices, triangles);
}

} // namespace

Vector3f TransformPointRowMajor(const Matrix44f& m, const Vector3f& p) {
    const f32 x = (m.data[2][0] * p.z + (m.data[1][0] * p.y + m.data[0][0] * p.x)) + m.data[3][0];
    const f32 y = (m.data[2][1] * p.z + (m.data[1][1] * p.y + m.data[0][1] * p.x)) + m.data[3][1];
    const f32 z = (m.data[2][2] * p.z + (m.data[1][2] * p.y + m.data[0][2] * p.x)) + m.data[3][2];
    return {x, y, z};
}

AnimRef<u32> UnsampledSwitch(u32 init) {
    AnimRef<u32> ref;
    ref.interpType = 0;
    ref.flags = 0;
    ref.animId = 0xFFFFFFFFu;
    ref.initValue = init;
    ref.nullValue = 0;
    ref.unused = 0;
    return ref;
}

std::vector<std::string> UpgradePhysics(Model& model) {
    std::vector<std::string> issues;

    for (std::size_t b = 0; b < model.rigidBodies.size(); ++b) {
        RigidBody& body = model.rigidBodies[b];
        const i32 version = body.getVersion();
        if (version >= 0 && version <= 2) {
            // The Havok material is discarded for Domino's defaults.
            body.simulationType = 0;
            body.physicsType = 24;
            body.density = 1000.0f;
            body.friction = 0.3f;
            body.restitution = 0.0f;
            body.linearDamping = 0.0f;
            body.angularDamping = 0.0f;
            body.inertiaScale = 1.0f;
        }
        if (version >= 0 && version <= 3) {
            body.dynamicState = UnsampledSwitch(0);
            body.dynamicBlendOut = 0.0f;
        }
        body.forceVersion(kCurrentRigidBodyVersion);

        for (PhysicsShape& shape : body.rigidBodyShape) {
            const i32 shapeVersion = shape.getVersion();
            if (shapeVersion == 2 && shape.shapeType == PhysicsShapeType::Mesh) {
                RebuildHavokMesh(shape);
            }
            if (!shape.sourcePoints.empty() || !shape.sourceTriangles.empty()) {
                CookSources(shape, b, issues);
            }
            shape.forceVersion(kCurrentPhysicsShapeVersion);
        }
    }

    for (ClothPhysics& cloth : model.clothPhysics) {
        const i32 version = cloth.getVersion();
        if (version >= 0 && version <= 2) {
            cloth.shearStiffness = 0.1f;
            cloth.dragFactor = 1.0f;
            cloth.liftFactor = 0.5f;
            cloth.sphereStiffness = 0.1f;
            cloth.flatten = 0;
            cloth.active = UnsampledSwitch(1);
            cloth.useSkinCollision = 0;
            cloth.skinOffset = 0.0f;
            cloth.skinExponent = 0.0f;
            cloth.skinStiffness = 0.1f;
            cloth.localChannels = 1;
            cloth.localWind = {};
        } else if (version == 3) {
            cloth.flatten = 0;
            cloth.localWind = {};
        }
        cloth.forceVersion(kCurrentClothVersion);
    }

    for (Force& force : model.forces) {
        const i32 version = force.getVersion();
        if (version >= 0 && version <= 1) {
            force.flags |= ForceFlag::AffectsParticles | ForceFlag::AffectsBodies;
        }
        if (version == 0) {
            const u32 shape = static_cast<u32>(force.forceShape);
            if (shape == 2) {
                force.forceType = ForceType::Vortex;
                force.forceShape = ForceShape::Cylinder;
            } else if (shape >= 3) {
                force.forceShape = static_cast<ForceShape>(shape - 1);
            }
        }
        force.forceVersion(kCurrentForceVersion);
    }

    const std::size_t warps = model.warps.size();
    std::erase_if(model.warps, [](const Warp& warp) { return warp.getVersion() == 0; });
    if (model.warps.size() != warps) {
        issues.push_back("WRP_ v0: " + std::to_string(warps - model.warps.size()) +
                         " vertex warps dropped; the client refuses the version.");
    }
    for (Warp& warp : model.warps) {
        warp.forceVersion(kCurrentWarpVersion);
    }
    return issues;
}

} // namespace m3
} // namespace whiteout
