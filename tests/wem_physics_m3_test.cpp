// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// StarCraft II physics through WEM (WEM_PHYSICS_DESIGN.md §6, §11): the exact
// basis change, a small model's records both ways, and the corpus gate G-P1 --
// every physics model `m3 -> WEM -> .wem -> WEM -> m3` comes back with its
// carried fields bit-equal and its dropped ones at the values the export
// writes.

#include <catch2/catch_all.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <whiteout/models/m3/parser.h>
#include <whiteout/models/m3/physics_cook.h>
#include <whiteout/models/m3/physics_upgrade.h>
#include <whiteout/models/m3/writer.h>
#include <whiteout/models/wem/converters.h>
#include <whiteout/models/wem/parser.h>
#include <whiteout/models/wem/writer.h>

#include "whiteout/models/wem/converters/m3_physics.h"
#include "m3_client_chunk_table.h"
#include "test_helpers.h"

namespace fs = std::filesystem;
using namespace whiteout;
using namespace whiteout::models::wem;
using Catch::Approx;

namespace {

bool SameBits(const Matrix44f& a, const Matrix44f& b) {
    return std::memcmp(&a, &b, sizeof(Matrix44f)) == 0;
}

bool SameBits(f32 a, f32 b) {
    return std::bit_cast<u32>(a) == std::bit_cast<u32>(b);
}

bool SameBits(const Vector3f& a, const Vector3f& b) {
    return SameBits(a.x, b.x) && SameBits(a.y, b.y) && SameBits(a.z, b.z);
}

Matrix44f Sample() {
    Matrix44f m = Matrix44f::identity();
    const f32 values[16] = {0.5f, -0.0f, 0.25f, 0.0f, -0.0f, 2.0f, -1.5f, 0.0f,
                            0.125f, 3.0f, -0.0f, 0.0f, 7.0f, -8.0f, 9.5f, 1.0f};
    std::memcpy(&m, values, sizeof values);
    return m;
}

m3::Bone RootBone(const char* name, u16 parent) {
    m3::Bone bone;
    bone.name = name;
    bone.parentIndex = parent;
    bone.rotation.initValue = Quaternion{0, 0, 0, 1};
    bone.scale.initValue = Vector3f{1, 1, 1};
    bone.visibility.initValue = 1;
    return bone;
}

/// A two-bone model with a body on each bone -- a box and a hull -- a joint
/// between them, and a force field.
m3::Model PhysicsFixture() {
    m3::Model model;
    model.forceVersion(29);
    model.bones.push_back(RootBone("root", 0xFFFF));
    model.bones.push_back(RootBone("arm", 0));
    model.initialReference.resize(2);
    model.initialReference[0].matrix = Matrix44f::identity();
    model.initialReference[1].matrix = Matrix44f::identity();

    m3::RigidBody root;
    root.simulationType = 1;
    root.parentBoneIndex = 0;
    root.physicsType = 4;
    root.density = 1500.0f;
    root.friction = 0.6f;
    root.restitution = 0.1f;
    root.inertiaScale = 1.0f;
    root.dynamicState = m3::UnsampledSwitch(0);
    root.flags = static_cast<m3::RigidBodyFlag>(0x61u);
    root.worldForces = 2;
    m3::PhysicsShape box;
    box.shapeType = m3::PhysicsShapeType::Box;
    box.transform = Sample();
    box.shapeDimensions = {1.0f, 2.0f, 3.0f};
    root.rigidBodyShape.push_back(box);
    model.rigidBodies.push_back(root);

    m3::RigidBody arm = root;
    arm.parentBoneIndex = 1;
    arm.dynamicState.flags = 0x6;
    arm.dynamicState.animId = 0x12345678u;
    arm.dynamicState.initValue = 1;
    arm.rigidBodyShape.clear();
    m3::PhysicsShape hull;
    hull.shapeType = m3::PhysicsShapeType::ConvexHull;
    std::vector<Vector3f> corners;
    for (int i = 0; i < 8; ++i) {
        corners.push_back({(i & 1) ? 0.3f : -0.3f, (i & 2) ? 0.2f : -0.2f, (i & 4) ? 0.9f : 0.0f});
    }
    m3::CookHull(hull, corners);
    arm.rigidBodyShape.push_back(hull);
    model.rigidBodies.push_back(arm);

    m3::PhysicsJoint joint;
    joint.jointType = 2;
    joint.boneIndex1 = 0;
    joint.boneIndex2 = 1;
    joint.matrixBody1 = Sample();
    joint.matrixBody2 = Matrix44f::identity();
    joint.enableLimits = 0x7F01u;
    joint.limitMin = -0.5f;
    joint.limitMax = 0.75f;
    joint.coneAngle = 1.0f;
    joint.enableFriction = 1;
    joint.friction = 0.25f;
    joint.breakThreshold = 1.0f;
    joint.enableShape = 1;
    model.physicsJoints.push_back(joint);

    m3::Force force;
    force.forceType = m3::ForceType::Vortex;
    force.forceShape = m3::ForceShape::Cone;
    force.unknown = 1;
    force.boneIndex = 1;
    force.flags = static_cast<m3::ForceFlag>(0x19u);
    force.localChannels = 0x4;
    force.strength.initValue = 5.0f;
    force.width.initValue = 2.0f;
    model.forces.push_back(force);
    m3::UpgradePhysics(model);
    return model;
}

} // namespace

TEST_CASE("wem physics the quarter turn is exact both ways", "[wem][physics][m3]") {
    const Matrix44f m = Sample();
    const Matrix44f frame = m3_physics::RebaseFrame(m);
    CHECK(SameBits(m3_physics::UnrebaseFrame(frame), m));
    const Matrix44f conjugate = m3_physics::RebaseConjugate(m);
    CHECK(SameBits(m3_physics::UnrebaseConjugate(conjugate), m));
    // `m * R` agrees with rebasing a point: p * (m * R) == Rebase(p * m).
    CHECK(frame.data[3][0] == -m.data[3][1]);
    CHECK(frame.data[3][1] == m.data[3][0]);
    // -0.0 survives a move: row 1 column 0 is -0.0 and becomes column 1.
    CHECK(std::signbit(frame.data[1][1]));
}

TEST_CASE("wem physics a small model's records cross both ways", "[wem][physics][m3]") {
    const m3::Model source = PhysicsFixture();
    M3Converter converter;
    const Result<Document> imported = converter.fromM3(source, ProfileId::Sc2);
    REQUIRE(imported.ok());
    const Model& model = imported->models.front();
    const PhysicsSet& physics = model.physics;
    REQUIRE(physics.bodies.size() == 2);
    REQUIRE(physics.joints.size() == 1);
    CHECK(physics.bodies[0].node == 0);
    CHECK(physics.bodies[1].node == 1);
    CHECK(physics.bodies[0].motion == BodyMotion::Kinematic);
    CHECK_FALSE(physics.bodies[0].simulates);
    CHECK(physics.bodies[1].simulates);
    CHECK(physics.bodies[0].inheritDynamic);
    REQUIRE(physics.bodies[0].sc2.has_value());
    CHECK(physics.bodies[0].sc2->physicsMaterial == 4);
    CHECK(physics.bodies[0].sc2->untracedFlags == 0x20u);
    CHECK(physics.bodies[0].forceChannels == (2u << 16));
    CHECK(physics.bodies[0].shapes[0].halfExtents.z == 3.0f);
    CHECK(physics.bodies[0].shapes[0].material.density == 1500.0f);
    CHECK(physics.bodies[1].shapes[0].kind == PhysicsShapeKind::ConvexHull);
    CHECK(physics.bodies[1].shapes[0].points.size() == 8);
    CHECK(physics.joints[0].kind == JointKind::ConeTwist);
    CHECK(physics.joints[0].limitEnabled);
    CHECK(physics.joints[0].friction == JointFriction::GravityHold);
    CHECK(physics.joints[0].bodyA == physics.bodies[0].id);
    CHECK(physics.joints[0].bodyB == physics.bodies[1].id);

    // The sampled switch is a channel; the unsampled one is not.
    u32 switches = 0;
    for (const AnimChannel& channel : model.animChannels.channels) {
        if (channel.target.kind == TrackTarget::Kind::Physics) {
            ++switches;
            CHECK(channel.id == 0x12345678u);
            CHECK(channel.target.sub == physics.bodies[1].id);
            CHECK(channel.target.channel == Channel::PhysicsDynamic);
        }
    }
    CHECK(switches == 1);

    // The force field is a node under its bone.
    const Node* field = nullptr;
    for (const Node& node : model.nodes.nodes) {
        if (node.kind == NodeKind::ForceField) {
            field = &node;
        }
    }
    REQUIRE(field != nullptr);
    CHECK(field->parent == 1);
    const auto& payload = std::get<ForceFieldPayload>(field->payload);
    CHECK(payload.kind == ForceKind::Vortex);
    CHECK(payload.volume == ForceVolume::Cone);
    CHECK(payload.falloff);
    CHECK(payload.affectsParticles);
    CHECK(payload.affectsBodies);
    CHECK(payload.scope == 1u);
    CHECK(payload.strength == 5.0f);

    // Through the `.wem` container and back out.
    Writer writer;
    const std::vector<u8> bytes = writer.write(*imported);
    Parser parser;
    const std::optional<Document> reread = parser.parse(std::span<const u8>(bytes));
    REQUIRE(reread.has_value());
    const Result<m3::Model> exported = converter.toM3(*reread, ProfileId::Sc2);
    REQUIRE(exported.ok());
    const m3::Model& out = *exported;
    REQUIRE(out.rigidBodies.size() == 2);
    CHECK(out.rigidBodies[0].getVersion() == 4);
    CHECK(SameBits(out.rigidBodies[0].rigidBodyShape[0].transform, source.rigidBodies[0].rigidBodyShape[0].transform));
    CHECK(out.rigidBodies[0].density == 1500.0f);
    CHECK(static_cast<u32>(out.rigidBodies[0].flags) == 0x61u);
    CHECK(out.rigidBodies[0].worldForces == 2);
    CHECK(out.rigidBodies[0].dynamicState.animId == 0xFFFFFFFFu);
    CHECK(out.rigidBodies[0].dynamicState.flags == 0);
    CHECK(out.rigidBodies[1].dynamicState.animId == 0x12345678u);
    CHECK((out.rigidBodies[1].dynamicState.flags & 0x2u) != 0);
    CHECK(out.rigidBodies[1].dynamicState.initValue == 1u);
    CHECK(out.rigidBodies[1].rigidBodyShape[0].hullVertices ==
          source.rigidBodies[1].rigidBodyShape[0].hullVertices);
    REQUIRE(out.physicsJoints.size() == 1);
    CHECK(SameBits(out.physicsJoints[0].matrixBody1, source.physicsJoints[0].matrixBody1));
    CHECK(out.physicsJoints[0].enableLimits == 1u); // the upper bytes are never read
    CHECK(out.physicsJoints[0].breakThreshold == 1.0f);
    REQUIRE(out.forces.size() == 1);
    CHECK(out.forces[0].forceType == m3::ForceType::Vortex);
    CHECK(out.forces[0].forceShape == m3::ForceShape::Cone);
    CHECK(static_cast<u32>(out.forces[0].flags) == 0x19u);
    CHECK(out.forces[0].unknown == 1u);
    CHECK(out.forces[0].localChannels == 0x4u);
}

// ============================================================================
// G-P1
// ============================================================================

namespace {

std::vector<fs::path> PhysicsCorpus() {
    std::vector<fs::path> files;
    if (const char* list = std::getenv("M3_PHYSICS_LIST"); list && fs::exists(list)) {
        std::ifstream in(list);
        for (std::string line; std::getline(in, line);) {
            const auto tab = line.find('\t');
            if (tab != std::string::npos) {
                line.resize(tab);
            }
            if (!line.empty()) {
                files.emplace_back(line);
            }
        }
    }
    return files;
}

struct Tally {
    std::map<std::string, std::size_t> counts;
    /// Known deviations, reported and not failed: a near-degenerate hull the
    /// cooker cleans up, and a bound region the mesh kernel's repair shortened
    /// (WEM_PHYSICS_DESIGN.md §10.3).
    std::map<std::string, std::size_t> known;
    std::vector<std::string> samples;
    std::size_t models = 0;
    void fail(const std::string& what, const fs::path& file) {
        if (counts[what]++ < 3) {
            samples.push_back(what + ": " + file.filename().string());
        }
    }
};

/// A particle's anchors as a sorted (bone, weight) list, zero weights dropped.
std::vector<std::pair<u8, u8>> Anchors(u32 bones, u32 weights) {
    std::vector<std::pair<u8, u8>> out;
    for (int k = 0; k < 4; ++k) {
        const u8 w = static_cast<u8>(weights >> (8 * k));
        if (w != 0) {
            out.emplace_back(static_cast<u8>(bones >> (8 * k)), w);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

/// A bound vertex's lanes as a sorted (lane, weight) list, unused lanes dropped.
std::vector<std::pair<u16, u8>> Lanes(u64 lanes, u32 weights) {
    std::vector<std::pair<u16, u8>> out;
    for (int k = 0; k < 4; ++k) {
        const u16 lane = static_cast<u16>(lanes >> (16 * k));
        const u8 w = static_cast<u8>(weights >> (8 * k));
        if (lane != 0xFFFFu && w != 0) {
            out.emplace_back(lane, w);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool SameRef(const m3::AnimRef<u32>& a, const m3::AnimRef<u32>& b) {
    const bool sampledA = (a.flags & 0x2u) != 0;
    const bool sampledB = (b.flags & 0x2u) != 0;
    return a.initValue == b.initValue && sampledA == sampledB && (!sampledA || a.animId == b.animId);
}

void CompareBodies(const m3::Model& a, const m3::Model& b, const fs::path& path, Tally& tally) {
    if (a.rigidBodies.size() != b.rigidBodies.size()) {
        tally.fail("PHRB count", path);
        return;
    }
    for (std::size_t i = 0; i < a.rigidBodies.size(); ++i) {
        const m3::RigidBody& x = a.rigidBodies[i];
        const m3::RigidBody& y = b.rigidBodies[i];
        constexpr u32 kCarried = 0x1u | 0x2u | 0x4u | 0x8u | 0x10u | 0x20u | 0x40u | 0x80u | 0x100u | 0x200u;
        if (x.simulationType != y.simulationType || x.parentBoneIndex != y.parentBoneIndex ||
            x.physicsType != y.physicsType) {
            tally.fail("PHRB type, bone or material id", path);
        }
        if (!SameBits(x.density, y.density) || !SameBits(x.friction, y.friction) ||
            !SameBits(x.restitution, y.restitution) || !SameBits(x.linearDamping, y.linearDamping) ||
            !SameBits(x.angularDamping, y.angularDamping) || !SameBits(x.inertiaScale, y.inertiaScale)) {
            tally.fail("PHRB material or damping", path);
        }
        if ((static_cast<u32>(x.flags) & kCarried) != static_cast<u32>(y.flags) ||
            x.localForces != y.localForces || x.worldForces != y.worldForces) {
            tally.fail("PHRB flags or forces", path);
        }
        if (!SameRef(x.dynamicState, y.dynamicState)) {
            tally.fail("PHRB dynamicState", path);
        }
        if (y.priority != 0 || y.dynamicBlendOut != 1.0f) {
            tally.fail("PHRB dropped fields not at retail values", path);
        }
        if (x.rigidBodyShape.size() != y.rigidBodyShape.size()) {
            tally.fail("PHSH count", path);
            continue;
        }
        for (std::size_t s = 0; s < x.rigidBodyShape.size(); ++s) {
            const m3::PhysicsShape& p = x.rigidBodyShape[s];
            const m3::PhysicsShape& q = y.rigidBodyShape[s];
            if (p.shapeType != q.shapeType) {
                tally.fail("PHSH type", path);
                continue;
            }
            if (!SameBits(p.transform, q.transform)) {
                tally.fail("PHSH matrix", path);
            }
            switch (p.shapeType) {
            case m3::PhysicsShapeType::Box:
                if (!SameBits(p.shapeDimensions, q.shapeDimensions)) {
                    tally.fail("PHSH box dimensions", path);
                }
                break;
            case m3::PhysicsShapeType::Sphere:
                if (!SameBits(p.shapeDimensions.x, q.shapeDimensions.x)) {
                    tally.fail("PHSH sphere radius", path);
                }
                break;
            case m3::PhysicsShapeType::Capsule:
            case m3::PhysicsShapeType::Cylinder:
                if (!SameBits(p.shapeDimensions.x, q.shapeDimensions.x) ||
                    !SameBits(p.shapeDimensions.y, q.shapeDimensions.y)) {
                    tally.fail("PHSH capsule/cylinder dimensions", path);
                }
                break;
            case m3::PhysicsShapeType::ConvexHull:
                if (p.hullVertices != q.hullVertices) {
                    // Only ever by dropping a point the source kept.
                    const bool subset = std::all_of(q.hullVertices.begin(), q.hullVertices.end(), [&](const Vector3f& v) {
                        return std::find(p.hullVertices.begin(), p.hullVertices.end(), v) != p.hullVertices.end();
                    });
                    if (subset && q.hullVertices.size() < p.hullVertices.size()) {
                        ++tally.known["near-degenerate hull cleaned"];
                    } else {
                        tally.fail("PHSH hull vertices", path);
                    }
                }
                if (std::abs(p.hullVolume - q.hullVolume) > 1e-4f * std::max(1.0f, std::abs(p.hullVolume))) {
                    tally.fail("PHSH hull volume", path);
                }
                break;
            case m3::PhysicsShapeType::Mesh: {
                const std::size_t n = std::min<std::size_t>(p.meshVertexCount, p.meshVertexPositions.size());
                if (q.meshVertexPositions.size() != n) {
                    tally.fail("PHSH mesh vertex count", path);
                    break;
                }
                for (std::size_t v = 0; v < n; ++v) {
                    const Vector4f& u = p.meshVertexPositions[v];
                    const Vector4f& w = q.meshVertexPositions[v];
                    const f32 ex = (u.x + p.meshBoundsCenter.x) - (w.x + q.meshBoundsCenter.x);
                    const f32 ey = (u.y + p.meshBoundsCenter.y) - (w.y + q.meshBoundsCenter.y);
                    const f32 ez = (u.z + p.meshBoundsCenter.z) - (w.z + q.meshBoundsCenter.z);
                    if (std::abs(ex) + std::abs(ey) + std::abs(ez) > 1e-4f) {
                        tally.fail("PHSH mesh vertices", path);
                        break;
                    }
                }
                break;
            }
            }
        }
    }
}

void CompareJoints(const m3::Model& a, const m3::Model& b, const fs::path& path, Tally& tally) {
    if (a.physicsJoints.size() != b.physicsJoints.size()) {
        tally.fail("PHYJ count", path);
        return;
    }
    for (std::size_t i = 0; i < a.physicsJoints.size(); ++i) {
        const m3::PhysicsJoint& x = a.physicsJoints[i];
        const m3::PhysicsJoint& y = b.physicsJoints[i];
        if (x.jointType != y.jointType || x.boneIndex1 != y.boneIndex1 || x.boneIndex2 != y.boneIndex2) {
            tally.fail("PHYJ type or bones", path);
        }
        if (!SameBits(x.matrixBody1, y.matrixBody1) || !SameBits(x.matrixBody2, y.matrixBody2)) {
            tally.fail("PHYJ frames", path);
        }
        if ((x.enableLimits & 0xFFu) != y.enableLimits || (x.enableFriction & 0xFFu) != y.enableFriction ||
            x.enableShape != y.enableShape) {
            tally.fail("PHYJ switches", path);
        }
        if (!SameBits(x.limitMin, y.limitMin) || !SameBits(x.limitMax, y.limitMax) ||
            !SameBits(x.coneAngle, y.coneAngle) || !SameBits(x.friction, y.friction) ||
            !SameBits(x.dampingRatio, y.dampingRatio) || !SameBits(x.angularFrequency, y.angularFrequency)) {
            tally.fail("PHYJ values", path);
        }
        if (y.breakThreshold != 1.0f) {
            tally.fail("PHYJ breakThreshold", path);
        }
    }
}

void CompareColliders(const m3::ClothCollider& x, const m3::ClothCollider& y, const fs::path& path,
                      Tally& tally) {
    if (!SameBits(x.transform, y.transform) || !SameBits(x.radius, y.radius) ||
        !SameBits(x.height, y.height) || x.bone != y.bone) {
        tally.fail("PHCC", path);
    }
}

void CompareCloth(const m3::Model& a, const m3::Model& b, const fs::path& path, Tally& tally) {
    std::vector<const m3::ClothPhysics*> cagesA, cagesB;
    std::vector<const m3::ClothCollider*> offeredA, offeredB;
    for (const auto& c : a.clothPhysics) {
        if (c.simEnabled.empty()) {
            for (const auto& collider : c.colliders) {
                offeredA.push_back(&collider);
            }
        } else {
            cagesA.push_back(&c);
        }
    }
    for (const auto& c : b.clothPhysics) {
        if (c.simEnabled.empty()) {
            for (const auto& collider : c.colliders) {
                offeredB.push_back(&collider);
            }
        } else {
            cagesB.push_back(&c);
        }
    }
    if (offeredA.size() != offeredB.size()) {
        tally.fail("collider-only PHCC count", path);
    } else {
        for (std::size_t i = 0; i < offeredA.size(); ++i) {
            CompareColliders(*offeredA[i], *offeredB[i], path, tally);
        }
    }
    if (cagesA.size() != cagesB.size()) {
        tally.fail("PHCL count", path);
        return;
    }
    for (std::size_t i = 0; i < cagesA.size(); ++i) {
        const m3::ClothPhysics& x = *cagesA[i];
        const m3::ClothPhysics& y = *cagesB[i];
        if (x.cageRegion != y.cageRegion) {
            tally.fail("PHCL cage region", path);
        }
        if (x.simEnabled.size() != y.simEnabled.size()) {
            tally.fail("PHCL particle count", path);
            continue;
        }
        for (std::size_t p = 0; p < x.simEnabled.size(); ++p) {
            if ((x.simEnabled[p] & 1u) != (y.simEnabled[p] & 1u)) {
                tally.fail("PHCL movable", path);
                break;
            }
        }
        for (std::size_t p = 0; p < x.vertexBones.size() && p < y.vertexBones.size(); ++p) {
            if (Anchors(x.vertexBones[p], x.vertexWeights[p]) != Anchors(y.vertexBones[p], y.vertexWeights[p])) {
                tally.fail("PHCL anchors", path);
                break;
            }
        }
        if (!SameBits(x.density, y.density) || !SameBits(x.tracking, y.tracking) ||
            !SameBits(x.stretchStiffness, y.stretchStiffness) ||
            !SameBits(x.horizontalStiffness, y.horizontalStiffness) ||
            !SameBits(x.bendingStiffness, y.bendingStiffness) || !SameBits(x.damping, y.damping) ||
            !SameBits(x.friction, y.friction) || !SameBits(x.gravity, y.gravity) ||
            !SameBits(x.explosionScale, y.explosionScale) || !SameBits(x.windScale, y.windScale) ||
            !SameBits(x.shearStiffness, y.shearStiffness) || !SameBits(x.dragFactor, y.dragFactor) ||
            !SameBits(x.liftFactor, y.liftFactor) || !SameBits(x.sphereStiffness, y.sphereStiffness) ||
            x.flatten != y.flatten || x.useSkinCollision != y.useSkinCollision ||
            !SameBits(x.skinOffset, y.skinOffset) || !SameBits(x.skinExponent, y.skinExponent) ||
            !SameBits(x.skinStiffness, y.skinStiffness) || !SameBits(x.localWind, y.localWind)) {
            tally.fail("PHCL parameters", path);
        }
        if (!SameRef(x.active, y.active)) {
            tally.fail("PHCL active", path);
        }
        if (x.colliders.size() != y.colliders.size()) {
            tally.fail("PHCL collider count", path);
        } else {
            for (std::size_t c = 0; c < x.colliders.size(); ++c) {
                CompareColliders(x.colliders[c], y.colliders[c], path, tally);
            }
        }
        if (x.proxies.size() != y.proxies.size()) {
            tally.fail("PHAC count", path);
            continue;
        }
        for (std::size_t r = 0; r < x.proxies.size(); ++r) {
            const m3::ClothProxy& u = x.proxies[r];
            const m3::ClothProxy& v = y.proxies[r];
            if (u.proxyIndex != v.proxyIndex || u.clothIndex != v.clothIndex) {
                tally.fail("PHAC regions", path);
            }
            if (u.proxyVertices.size() != v.proxyVertices.size()) {
                const bool shortened = u.proxyIndex < b.divisions[0].regions.size() &&
                                       v.proxyVertices.size() == b.divisions[0].regions[u.proxyIndex].vertexCount &&
                                       v.proxyVertices.size() < u.proxyVertices.size();
                if (shortened) {
                    ++tally.known["bound region shortened by the mesh repair"];
                } else {
                    tally.fail("PHAC vertex count", path);
                }
                continue;
            }
            for (std::size_t k = 0; k < u.proxyVertices.size(); ++k) {
                if (Lanes(u.proxyVertices[k], u.proxyWeights[k]) != Lanes(v.proxyVertices[k], v.proxyWeights[k])) {
                    tally.fail("PHAC lanes", path);
                    break;
                }
            }
        }
    }
}

template <class T>
bool SameF32Ref(const m3::AnimRef<T>& a, const m3::AnimRef<T>& b) {
    return SameBits(a.initValue, b.initValue) && (a.animId == 0) == (b.animId == 0) &&
           (a.animId == 0 || a.animId == b.animId);
}

void CompareFields(const m3::Model& a, const m3::Model& b, const fs::path& path, Tally& tally) {
    if (a.forces.size() != b.forces.size()) {
        tally.fail("FOR_ count", path);
    } else {
        for (std::size_t i = 0; i < a.forces.size(); ++i) {
            const m3::Force& x = a.forces[i];
            const m3::Force& y = b.forces[i];
            if (x.forceType != y.forceType || x.forceShape != y.forceShape || x.unknown != y.unknown ||
                x.boneIndex != y.boneIndex || x.flags != y.flags || x.localChannels != y.localChannels) {
                tally.fail("FOR_ fields", path);
            }
            if (!SameF32Ref(x.strength, y.strength) || !SameF32Ref(x.width, y.width) ||
                !SameF32Ref(x.height, y.height) || !SameF32Ref(x.length, y.length)) {
                tally.fail("FOR_ properties", path);
            }
        }
    }
    if (a.warps.size() != b.warps.size()) {
        tally.fail("WRP_ count", path);
    } else {
        for (std::size_t i = 0; i < a.warps.size(); ++i) {
            const m3::Warp& x = a.warps[i];
            const m3::Warp& y = b.warps[i];
            if (x.warpType != y.warpType || x.boneIndex != y.boneIndex || x.unknown != y.unknown ||
                !SameF32Ref(x.radius, y.radius) || !SameF32Ref(x.height, y.height) ||
                !SameF32Ref(x.strength, y.strength) || !SameF32Ref(x.angular, y.angular) ||
                !SameF32Ref(x.axial, y.axial) || !SameF32Ref(x.radial, y.radial)) {
                tally.fail("WRP_ fields", path);
            }
        }
    }
}

} // namespace

TEST_CASE("wem physics G-P1 every physics model round-trips", "[wem][physics][m3][corpus]") {
    const auto files = PhysicsCorpus();
    if (files.empty()) {
        SKIP("M3_PHYSICS_LIST unset");
    }
    std::size_t limit = files.size();
    if (const char* env = std::getenv("M3_CORPUS_LIMIT"); env && *env) {
        limit = std::min<std::size_t>(limit, std::strtoull(env, nullptr, 10));
    }
    const bool throughWem = std::getenv("M3_SKIP_WEM") == nullptr;
    Tally tally;
    M3Converter converter;
    for (std::size_t f = 0; f < limit; ++f) {
        const fs::path& path = files[f];
        m3::Parser m3parser;
        const m3::Model source = m3parser.parse(path.string());
        if (source.bones.empty()) {
            continue;
        }
        const Result<Document> imported = converter.fromM3(source);
        if (!imported.ok()) {
            tally.fail("import failed", path);
            continue;
        }
        const ProfileId profile = imported->defaultProfile;
        std::optional<Document> document = *imported;
        if (throughWem) {
            Writer writer;
            const std::vector<u8> bytes = writer.write(*imported);
            Parser parser;
            document = parser.parse(std::span<const u8>(bytes));
            if (!document.has_value()) {
                tally.fail(".wem reread failed", path);
                continue;
            }
        }
        const Result<m3::Model> exported =
            converter.toM3(*document, profile, static_cast<u32>(std::max(source.getVersion(), 0)));
        if (!exported.ok()) {
            tally.fail("export failed", path);
            continue;
        }
        ++tally.models;
        // G-V: the written file against the client's own chunk table.
        m3::Writer m3writer;
        const m3client::Report gv = m3client::Check(m3writer.write(*exported), profile == ProfileId::Sc2);
        for (const std::string& problem : gv.problems) {
            tally.fail("G-V " + problem, path);
        }
        // IREF crosses by index and sign, so it keeps every bit (§10.2).
        const std::size_t bones = std::min(source.initialReference.size(), exported->initialReference.size());
        for (std::size_t b = 0; b < bones; ++b) {
            if (!SameBits(source.initialReference[b].matrix, exported->initialReference[b].matrix)) {
                tally.fail("IREF bits", path);
                break;
            }
        }
        CompareBodies(source, *exported, path, tally);
        CompareJoints(source, *exported, path, tally);
        CompareCloth(source, *exported, path, tally);
        CompareFields(source, *exported, path, tally);
    }
    std::cout << "G-P1: " << tally.models << " models of " << limit << "\n";
    for (const auto& [what, count] : tally.counts) {
        std::cout << "  " << what << ": " << count << "\n";
    }
    for (const auto& [what, count] : tally.known) {
        std::cout << "  known, " << what << ": " << count << "\n";
    }
    for (const auto& s : tally.samples) {
        std::cout << "  e.g. " << s << "\n";
    }
    CHECK(tally.models > 0);
    CHECK(tally.counts.empty());
}
