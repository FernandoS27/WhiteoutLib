// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "m2_physics.h"

#include <whiteout/models/m2/phys_file.h>
#include <whiteout/models/m3/physics_cook.h>
#include <whiteout/models/m3/physics_upgrade.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace whiteout {
namespace models {
namespace wem {
namespace m2_physics {

namespace {

constexpr u32 kNoBone = 0xFFFFu;

Vector3f Row(const Matrix44f& m, int r) {
    return Vector3f{m.data[r][0], m.data[r][1], m.data[r][2]};
}

void SetRow(Matrix44f& m, int r, const Vector3f& v) {
    m.data[r][0] = v.x;
    m.data[r][1] = v.y;
    m.data[r][2] = v.z;
}

Matrix44f FrameMatrix(const m2::PhysicsFrame& frame) {
    Matrix44f m = Matrix44f::identity();
    SetRow(m, 0, frame.axisX);
    SetRow(m, 1, frame.axisY);
    SetRow(m, 2, frame.axisZ);
    SetRow(m, 3, frame.origin);
    return m;
}

m2::PhysicsFrame ToFrame(const Matrix44f& m) {
    return m2::PhysicsFrame{Row(m, 0), Row(m, 1), Row(m, 2), Row(m, 3)};
}

Matrix44f At(const Vector3f& p) {
    Matrix44f m = Matrix44f::identity();
    SetRow(m, 3, p);
    return m;
}

/// Whether @p m turns nothing: an anchor joint has no frame to turn.
bool Unrotated(const Matrix44f& m) {
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            if (m.data[r][c] != (r == c ? 1.0f : 0.0f)) {
                return false;
            }
        }
    }
    return true;
}

JointMotorMode MotorOf(u32 raw) {
    // Only the low byte is read.
    switch (raw & 0xFFu) {
    case 1:
        return JointMotorMode::Position;
    case 2:
        return JointMotorMode::Velocity;
    default:
        return JointMotorMode::Off;
    }
}

u32 RawMotor(JointMotorMode mode) {
    return mode == JointMotorMode::Position ? 1u : mode == JointMotorMode::Velocity ? 2u : 0u;
}

WowPhysicsKind KindOf(u32 raw) {
    switch (raw) {
    case 0:
    case 1:
        return WowPhysicsKind::WornItem;
    case 2:
        return WowPhysicsKind::Vegetation;
    case 4:
        return WowPhysicsKind::PrivateWorld;
    default:
        return WowPhysicsKind::Ragdoll;
    }
}

u32 RawKind(WowPhysicsKind kind) {
    switch (kind) {
    case WowPhysicsKind::WornItem:
        return 0;
    case WowPhysicsKind::Vegetation:
        return 2;
    case WowPhysicsKind::PrivateWorld:
        return 4;
    default:
        return 3;
    }
}

/// Whether @p kind has a limit range, which the file enables as `upper > lower`.
bool HasRange(JointKind kind) {
    return kind == JointKind::ConeTwist || kind == JointKind::Revolute || kind == JointKind::Prismatic;
}

m2::PolytopeShape CookPolytope(std::span<const Vector3f> points, m3::HullCookReport& report) {
    // The same Domino polytope as a StarCraft II hull: vertices, planes, each
    // face's first half-edge and the twin-paired half-edges.
    m3::PhysicsShape cooked;
    report = m3::CookHull(cooked, points);
    m2::PolytopeShape hull;
    hull.vertices = cooked.hullVertices;
    hull.facePlanes = cooked.hullPlanes;
    hull.faceFirstEdges = cooked.hullFaceFirstEdges;
    hull.edges.reserve(cooked.hullHalfEdges.size());
    for (const m3::ConvexHullHalfEdge& e : cooked.hullHalfEdges) {
        hull.edges.push_back(m2::PolytopeHalfEdge{e.twinOffset, e.originVertex, e.face, e.nextInFace});
    }
    hull.centroid = cooked.hullCentroid;
    hull.volume = cooked.hullVolume;
    hull.surfaceArea = cooked.hullSurfaceArea;
    return hull;
}

} // namespace

f32 ToRadians(f32 degrees) {
    return degrees * m2::kPhysicsDegreesToRadians;
}

f32 ToDegrees(f32 radians) {
    const f32 guess = static_cast<f32>(static_cast<double>(radians) * (180.0 / 3.14159265358979323846));
    // The multiply rounds, so a few neighbours of the quotient may land on it.
    f32 below = guess;
    f32 above = guess;
    for (int step = 0; step < 8; ++step) {
        if (ToRadians(above) == radians) {
            return above;
        }
        if (ToRadians(below) == radians) {
            return below;
        }
        above = std::nextafter(above, std::numeric_limits<f32>::infinity());
        below = std::nextafter(below, -std::numeric_limits<f32>::infinity());
    }
    return guess;
}

// ============================================================================
// Import
// ============================================================================

void Import(const m2::Model& source, Model& model, Diagnostics& out) {
    if (!source.physics.has_value()) {
        return;
    }
    const m2::PhysicsData& data = *source.physics;
    const ElementRef whole(ElementKind::Document, 0);
    if (data.version > 6) {
        out.warn(DiagCode::PhysicsVersionRefused,
                 ".phys version " + std::to_string(data.version) + ", past the 6 the client reads; dropped", whole,
                 ProfileId::Wow);
        return;
    }
    for (const m2::PhysicsUnknownChunk& chunk : data.unknownChunks) {
        out.info(DiagCode::PhysicsChunkDropped,
                 "a .phys chunk '" + std::string(chunk.tag.begin(), chunk.tag.end()) + "' nothing reads", whole,
                 ProfileId::Wow);
    }

    PhysicsSet& physics = model.physics;
    const u32 bones = static_cast<u32>(source.bones.size());

    // --- bodies and their shapes ---------------------------------------------
    std::vector<u32> idOf(data.bodies.size(), 0);
    for (std::size_t i = 0; i < data.bodies.size(); ++i) {
        const m2::PhysicsBody& record = data.bodies[i];
        if (record.boneIndex >= bones) {
            out.warn(DiagCode::PhysicsReferenceInvalid,
                     "body " + std::to_string(i) + " rides bone " + std::to_string(record.boneIndex) + " of " +
                         std::to_string(bones) + "; dropped",
                     whole, ProfileId::Wow);
            continue;
        }
        PhysicsBody body;
        body.id = physics.allocateId();
        idOf[i] = body.id;
        body.node = record.boneIndex;
        body.motion = record.type == m2::PhysicsBodyType::Kinematic ? BodyMotion::Kinematic
                      : record.type == m2::PhysicsBodyType::Dynamic ? BodyMotion::Dynamic
                                                                    : BodyMotion::Static;
        body.linearDamping = record.linearDamping;
        body.angularDamping = record.angularDamping;
        body.gravityScale = record.gravityScale;
        body.inertiaScale = record.inertiaScale;
        WowBodyExtension wow;
        wow.followFactor = record.followFactor;
        wow.hasChildren = (record.attachment & m2::kPhysicsAttachmentHasChildren) != 0;
        wow.ragdollRoot = (record.attachment & m2::kPhysicsAttachmentRagdollRoot) != 0;
        body.wow = wow;

        const ElementRef where(ElementKind::PhysicsRecord, body.id);
        const i64 first = std::max<i64>(0, record.shapeIndex);
        const i64 last = first + std::max<i64>(0, record.shapeCount);
        for (i64 s = first; s < last; ++s) {
            if (static_cast<std::size_t>(s) >= data.shapes.size()) {
                out.warn(DiagCode::PhysicsReferenceInvalid, "a body's shapes run past the shape table", where,
                         ProfileId::Wow);
                break;
            }
            const m2::PhysicsShape& ref = data.shapes[static_cast<std::size_t>(s)];
            const std::size_t k = static_cast<std::size_t>(std::max<i16>(0, ref.shapeIndex));
            PhysicsShape shape;
            shape.material.density = ref.density;
            shape.material.friction = ref.friction;
            shape.material.restitution = ref.restitution;
            shape.gameFlags = ref.gameFlags;
            bool found = true;
            switch (ref.shapeType) {
            case m2::PhysicsShapeType::Box:
                if ((found = k < data.boxShapes.size())) {
                    shape.kind = PhysicsShapeKind::Box;
                    shape.transform = FrameMatrix(data.boxShapes[k].frame);
                    shape.halfExtents = data.boxShapes[k].halfExtents;
                }
                break;
            case m2::PhysicsShapeType::Capsule:
                if ((found = k < data.capsuleShapes.size())) {
                    const m2::CapsuleShape& c = data.capsuleShapes[k];
                    shape.kind = PhysicsShapeKind::Capsule;
                    shape.points = {c.localPosition1, c.localPosition2};
                    shape.radius = c.radius;
                    shape.material.density = m2::capsuleDensity(ref.density, c, data.version);
                }
                break;
            case m2::PhysicsShapeType::Sphere:
                if ((found = k < data.sphereShapes.size())) {
                    shape.kind = PhysicsShapeKind::Sphere;
                    shape.transform = At(data.sphereShapes[k].localPosition);
                    shape.radius = data.sphereShapes[k].radius;
                }
                break;
            case m2::PhysicsShapeType::Polytope:
                if ((found = k < data.polytopeShapes.size())) {
                    shape.kind = PhysicsShapeKind::ConvexHull;
                    shape.points = data.polytopeShapes[k].vertices;
                }
                break;
            default:
                found = false;
                break;
            }
            if (!found) {
                out.warn(DiagCode::PhysicsReferenceInvalid, "a shape names no record of its type; dropped", where,
                         ProfileId::Wow);
                continue;
            }
            body.shapes.push_back(std::move(shape));
        }
        physics.bodies.push_back(std::move(body));
    }
    // A dynamic body's word is the index of the body it hangs off.
    for (std::size_t i = 0; i < data.bodies.size(); ++i) {
        PhysicsBody* body = idOf[i] != 0 ? physics.body(idOf[i]) : nullptr;
        if (body != nullptr && body->motion == BodyMotion::Dynamic) {
            const u32 index = data.bodies[i].attachment & m2::kPhysicsAttachmentParentMask;
            body->wow->parent = index < idOf.size() ? idOf[index] : 0;
        }
    }

    // --- joints --------------------------------------------------------------
    for (const m2::PhysicsJoint& link : data.joints) {
        const u32 a = link.bodyAIndex < idOf.size() ? idOf[link.bodyAIndex] : 0;
        const u32 b = link.bodyBIndex < idOf.size() ? idOf[link.bodyBIndex] : 0;
        if (a == 0 || b == 0) {
            out.warn(DiagCode::PhysicsReferenceInvalid, "a joint names a body that is not there; dropped", whole,
                     ProfileId::Wow);
            continue;
        }
        PhysicsJoint joint;
        joint.bodyA = a;
        joint.bodyB = b;
        const std::size_t k = static_cast<std::size_t>(std::max<i16>(0, link.jointId));
        bool found = true;
        switch (link.jointType) {
        case m2::PhysicsJointType::Spherical:
            if ((found = k < data.sphericalJoints.size())) {
                const m2::SphericalJoint& r = data.sphericalJoints[k];
                joint.kind = JointKind::Spherical;
                joint.frameA = At(r.anchorA);
                joint.frameB = At(r.anchorB);
                if (r.frictionTorque != 0.0f) {
                    joint.friction = JointFriction::Torque;
                    joint.frictionAmount = r.frictionTorque;
                }
            }
            break;
        case m2::PhysicsJointType::Shoulder:
            if ((found = k < data.shoulderJoints.size())) {
                const m2::ShoulderJoint& r = data.shoulderJoints[k];
                joint.kind = JointKind::ConeTwist;
                joint.frameA = FrameMatrix(r.frameA);
                joint.frameB = FrameMatrix(r.frameB);
                joint.lower = ToRadians(r.lowerTwistAngle);
                joint.upper = ToRadians(r.upperTwistAngle);
                joint.limitEnabled = r.upperTwistAngle > r.lowerTwistAngle;
                joint.cone = ToRadians(r.coneAngle);
                joint.motor = MotorOf(r.motorMode);
                joint.maxMotorForce = r.maxMotorTorque;
                joint.angularSpring = JointSpring{r.motorFrequencyHz, r.motorDampingRatio};
            }
            break;
        case m2::PhysicsJointType::Weld:
            if ((found = k < data.weldJoints.size())) {
                const m2::WeldJoint& r = data.weldJoints[k];
                joint.kind = JointKind::Weld;
                joint.frameA = FrameMatrix(r.frameA);
                joint.frameB = FrameMatrix(r.frameB);
                joint.angularSpring = JointSpring{r.angularFrequencyHz, r.angularDampingRatio};
                joint.linearSpring = JointSpring{r.linearFrequencyHz, r.linearDampingRatio};
            }
            break;
        case m2::PhysicsJointType::Revolute:
            if ((found = k < data.revoluteJoints.size())) {
                const m2::RevoluteJoint& r = data.revoluteJoints[k];
                joint.kind = JointKind::Revolute;
                joint.frameA = FrameMatrix(r.frameA);
                joint.frameB = FrameMatrix(r.frameB);
                joint.lower = ToRadians(r.lowerAngle);
                joint.upper = ToRadians(r.upperAngle);
                joint.limitEnabled = r.upperAngle > r.lowerAngle;
                joint.motor = MotorOf(r.motorMode);
                joint.maxMotorForce = r.maxMotorTorque;
                joint.angularSpring = JointSpring{r.motorFrequencyHz, r.motorDampingRatio};
            }
            break;
        case m2::PhysicsJointType::Prismatic:
            if ((found = k < data.prismaticJoints.size())) {
                const m2::PrismaticJoint& r = data.prismaticJoints[k];
                joint.kind = JointKind::Prismatic;
                joint.frameA = FrameMatrix(r.frameA);
                joint.frameB = FrameMatrix(r.frameB);
                joint.lower = r.lowerLimit;
                joint.upper = r.upperLimit;
                joint.limitEnabled = r.upperLimit > r.lowerLimit;
                joint.referenceTranslation = r.referenceTranslation;
                joint.motor = MotorOf(r.motorMode);
                joint.maxMotorForce = r.maxMotorForce;
                joint.motorSpeed = r.motorSpeed;
                joint.linearSpring = JointSpring{r.motorFrequencyHz, r.motorDampingRatio};
            }
            break;
        case m2::PhysicsJointType::Distance:
            if ((found = k < data.distanceJoints.size())) {
                const m2::DistanceJoint& r = data.distanceJoints[k];
                joint.kind = JointKind::Distance;
                joint.frameA = At(r.localAnchorA);
                joint.frameB = At(r.localAnchorB);
                joint.restLength = r.distance;
            }
            break;
        default:
            found = false;
            break;
        }
        if (!found) {
            out.warn(DiagCode::PhysicsReferenceInvalid, "a joint names no record of its type; dropped", whole,
                     ProfileId::Wow);
            continue;
        }
        joint.id = physics.allocateId();
        physics.joints.push_back(joint);
    }

    // --- the object the model becomes -------------------------------------------
    // Every kind simulates from creation; a phantom has no bodies at all.
    if (physics.bodies.empty() && !data.phyt.has_value() && data.tuning.empty() && !data.allowList.has_value()) {
        return;
    }
    WowRigExtension wow;
    wow.kind = KindOf(data.phyt.value_or(0));
    if (data.phyt.has_value() && *data.phyt > 4) {
        out.warn(DiagCode::PhysicsUnsupported,
                 "PHYT " + std::to_string(*data.phyt) + " is no kind the client builds; read as a ragdoll", whole,
                 ProfileId::Wow);
    }
    if (!data.tuning.empty()) {
        const m2::PhysicsTuning& t = data.tuning.front();
        wow.vegetation = WowVegetation{t.posMaxPush, t.posPushAmt, t.posRelaxSpeed, t.velMaxPush, t.velSpeed, t.minPushDist};
    }
    if (data.allowList.has_value()) {
        wow.allowList = data.allowList->keys;
    }
    PhysicsRig rig;
    rig.id = physics.allocateId();
    rig.name = ToString(wow.kind);
    rig.start = RigStart::Always;
    for (const PhysicsBody& body : physics.bodies) {
        rig.bodies.push_back(body.id);
    }
    rig.wow = std::move(wow);
    physics.rigs.push_back(std::move(rig));
}

// ============================================================================
// Export
// ============================================================================

std::optional<m2::PhysicsData> Export(const Model& model, ProfileId profile,
                                      const std::function<u32(u32)>& boneOf, Diagnostics& out) {
    const PhysicsSet& physics = model.physics;
    if (physics.empty()) {
        return std::nullopt;
    }
    const ElementRef whole(ElementKind::Document, 0);
    const PhysicsCaps& caps = Profile(profile).physics;
    if (!caps.any()) {
        out.warn(DiagCode::PhysicsUnsupported, "the profile carries no physics; the records are not written", whole,
                 profile);
        return std::nullopt;
    }
    if (!physics.cloths.empty() || !physics.colliders.empty()) {
        out.warn(DiagCode::PhysicsUnsupported, "cloth and its colliders, which a .phys cannot hold; not written",
                 whole, profile);
    }
    if (physics.bodies.empty() && physics.rigs.empty()) {
        return std::nullopt;
    }

    m2::PhysicsData data;
    data.version = 6;

    // --- bodies and their shapes ---------------------------------------------
    const auto writeShape = [&](const PhysicsShape& shape, u32 bodyId) {
        const ElementRef where(ElementKind::PhysicsRecord, bodyId);
        if ((caps.shapeKinds & (1u << static_cast<u32>(shape.kind))) == 0) {
            out.warn(DiagCode::PhysicsUnsupported, std::string("a ") + ToString(shape.kind) + " shape; not written",
                     where, profile);
            return false;
        }
        m2::PhysicsShape ref;
        ref.gameFlags = shape.gameFlags;
        ref.friction = shape.material.friction;
        ref.restitution = shape.material.restitution;
        ref.density = shape.material.density;
        switch (shape.kind) {
        case PhysicsShapeKind::Box:
            ref.shapeType = m2::PhysicsShapeType::Box;
            ref.shapeIndex = static_cast<i16>(data.boxShapes.size());
            data.boxShapes.push_back(m2::BoxShape{ToFrame(shape.transform), shape.halfExtents});
            break;
        case PhysicsShapeKind::Sphere:
            ref.shapeType = m2::PhysicsShapeType::Sphere;
            ref.shapeIndex = static_cast<i16>(data.sphereShapes.size());
            data.sphereShapes.push_back(m2::SphereShape{Row(shape.transform, 3), shape.radius});
            break;
        case PhysicsShapeKind::Capsule: {
            const auto [p1, p2] = CapsuleEnds(shape);
            ref.shapeType = m2::PhysicsShapeType::Capsule;
            ref.shapeIndex = static_cast<i16>(data.capsuleShapes.size());
            data.capsuleShapes.push_back(m2::CapsuleShape{p1, p2, shape.radius});
            break;
        }
        default: {
            // A polytope has no matrix: it goes into the points. An identity
            // matrix skips the multiply, so shipped points stay bit-exact.
            std::vector<Vector3f> points;
            points.reserve(shape.points.size());
            const bool bake = !(Unrotated(shape.transform) && Row(shape.transform, 3) == Vector3f{0, 0, 0});
            for (const Vector3f& p : shape.points) {
                points.push_back(bake ? m3::TransformPointRowMajor(shape.transform, p) : p);
            }
            m3::HullCookReport report;
            ref.shapeType = m2::PhysicsShapeType::Polytope;
            ref.shapeIndex = static_cast<i16>(data.polytopeShapes.size());
            data.polytopeShapes.push_back(CookPolytope(points, report));
            if (report.simplified) {
                out.warn(DiagCode::PhysicsHullSimplified,
                         "a hull of " + std::to_string(points.size()) + " points merged faces to fit the cooked tables",
                         where, profile);
            }
            if (!report.ok) {
                out.warn(DiagCode::PhysicsShapeDegenerate,
                         "a hull of " + std::to_string(points.size()) + " points has no volume; written empty", where,
                         profile);
            }
            break;
        }
        }
        data.shapes.push_back(ref);
        return true;
    };

    std::map<u32, u32> indexOf; // body id -> its place in the file
    for (const PhysicsBody& body : physics.bodies) {
        const ElementRef where(ElementKind::PhysicsRecord, body.id);
        const u32 bone = body.node < model.nodes.size() ? boneOf(body.node) : kNoBone;
        if (bone == kNoBone || bone > 0xFFFFu) {
            out.warn(DiagCode::PhysicsReferenceInvalid, "a body rides no bone; not written", where, profile);
            continue;
        }
        m2::PhysicsBody record;
        record.type = body.motion == BodyMotion::Dynamic   ? m2::PhysicsBodyType::Dynamic
                      : body.motion == BodyMotion::Kinematic ? m2::PhysicsBodyType::Kinematic
                                                             : m2::PhysicsBodyType::Static;
        record.boneIndex = static_cast<u16>(bone);
        // Where the body is created; its first step puts it on its bone
        // whatever this says (G-W2 runs bit-identical without it).
        record.position = model.nodes.rig == RigConvention::PivotRelative
                              ? model.nodes.nodes[body.node].pivot
                              : model.nodes.worldBind(body.node).translation;
        record.shapeIndex = static_cast<i32>(data.shapes.size());
        for (const PhysicsShape& shape : body.shapes) {
            writeShape(shape, body.id);
        }
        record.shapeCount = static_cast<i32>(data.shapes.size()) - record.shapeIndex;
        record.gravityScale = body.gravityScale;
        record.inertiaScale = body.inertiaScale;
        record.linearDamping = body.linearDamping;
        record.angularDamping = body.angularDamping;
        record.followFactor = body.wow.has_value() ? body.wow->followFactor : 0.9f;
        if (!body.simulates) {
            out.info(DiagCode::PhysicsUnsupported, "a body switched off, which World of Warcraft cannot say; it simulates",
                     where, profile);
        }
        indexOf.emplace(body.id, static_cast<u32>(data.bodies.size()));
        data.bodies.push_back(record);
    }
    for (const PhysicsBody& body : physics.bodies) {
        const auto self = indexOf.find(body.id);
        if (self == indexOf.end() || !body.wow.has_value()) {
            continue;
        }
        const WowBodyExtension& wow = *body.wow;
        u16 word = static_cast<u16>((wow.hasChildren ? m2::kPhysicsAttachmentHasChildren : 0u) |
                                    (wow.ragdollRoot ? m2::kPhysicsAttachmentRagdollRoot : 0u));
        if (body.motion == BodyMotion::Dynamic) {
            const auto parent = indexOf.find(wow.parent);
            word |= parent != indexOf.end() ? static_cast<u16>(parent->second & m2::kPhysicsAttachmentParentMask) : 0u;
        }
        data.bodies[self->second].attachment = word;
    }

    // --- joints --------------------------------------------------------------
    for (const PhysicsJoint& joint : physics.joints) {
        const ElementRef where(ElementKind::PhysicsRecord, joint.id);
        const auto a = indexOf.find(joint.bodyA);
        const auto b = indexOf.find(joint.bodyB);
        if (a == indexOf.end() || b == indexOf.end()) {
            out.warn(DiagCode::PhysicsReferenceInvalid, "a joint names a body that was not written; not written",
                     where, profile);
            continue;
        }
        if ((caps.jointKinds & (1u << static_cast<u32>(joint.kind))) == 0) {
            out.warn(DiagCode::PhysicsJointKindUnsupported, std::string("a ") + ToString(joint.kind) + " joint; not written",
                     where, profile);
            continue;
        }
        const bool anchors = joint.kind == JointKind::Spherical || joint.kind == JointKind::Distance;
        const bool rangeLost = HasRange(joint.kind) && joint.limitEnabled != (joint.upper > joint.lower);
        if (joint.collideConnected || joint.breakForce != 0.0f || joint.breakTorque != 0.0f ||
            joint.friction == JointFriction::GravityHold || rangeLost ||
            (anchors && !(Unrotated(joint.frameA) && Unrotated(joint.frameB)))) {
            out.info(DiagCode::PhysicsJointFieldDropped,
                     "a joint's collision, breaking, gravity-hold friction, limit switch or anchor rotation", where,
                     profile);
        }
        // The file enables a limit as `upper > lower`: a switched-off one is
        // written as an empty range.
        const bool limit = joint.limitEnabled && joint.upper > joint.lower;
        const f32 lower = limit ? joint.lower : 0.0f;
        const f32 upper = limit ? joint.upper : 0.0f;

        m2::PhysicsJoint link;
        link.bodyAIndex = a->second;
        link.bodyBIndex = b->second;
        switch (joint.kind) {
        case JointKind::Spherical:
            link.jointType = m2::PhysicsJointType::Spherical;
            link.jointId = static_cast<i16>(data.sphericalJoints.size());
            data.sphericalJoints.push_back(m2::SphericalJoint{
                Row(joint.frameA, 3), Row(joint.frameB, 3),
                joint.friction == JointFriction::Torque ? joint.frictionAmount : 0.0f});
            break;
        case JointKind::ConeTwist: {
            m2::ShoulderJoint r;
            r.frameA = ToFrame(joint.frameA);
            r.frameB = ToFrame(joint.frameB);
            r.lowerTwistAngle = limit ? ToDegrees(lower) : 0.0f;
            r.upperTwistAngle = limit ? ToDegrees(upper) : 0.0f;
            r.coneAngle = ToDegrees(joint.cone);
            r.maxMotorTorque = joint.maxMotorForce;
            r.motorMode = RawMotor(joint.motor);
            r.motorFrequencyHz = joint.angularSpring.hz;
            r.motorDampingRatio = joint.angularSpring.damping;
            link.jointType = m2::PhysicsJointType::Shoulder;
            link.jointId = static_cast<i16>(data.shoulderJoints.size());
            data.shoulderJoints.push_back(r);
            break;
        }
        case JointKind::Weld: {
            m2::WeldJoint r;
            r.frameA = ToFrame(joint.frameA);
            r.frameB = ToFrame(joint.frameB);
            r.angularFrequencyHz = joint.angularSpring.hz;
            r.angularDampingRatio = joint.angularSpring.damping;
            r.linearFrequencyHz = joint.linearSpring.hz;
            r.linearDampingRatio = joint.linearSpring.damping;
            link.jointType = m2::PhysicsJointType::Weld;
            link.jointId = static_cast<i16>(data.weldJoints.size());
            data.weldJoints.push_back(r);
            break;
        }
        case JointKind::Revolute: {
            m2::RevoluteJoint r;
            r.frameA = ToFrame(joint.frameA);
            r.frameB = ToFrame(joint.frameB);
            r.lowerAngle = limit ? ToDegrees(lower) : 0.0f;
            r.upperAngle = limit ? ToDegrees(upper) : 0.0f;
            r.maxMotorTorque = joint.maxMotorForce;
            r.motorMode = RawMotor(joint.motor);
            r.motorFrequencyHz = joint.angularSpring.hz;
            r.motorDampingRatio = joint.angularSpring.damping;
            link.jointType = m2::PhysicsJointType::Revolute;
            link.jointId = static_cast<i16>(data.revoluteJoints.size());
            data.revoluteJoints.push_back(r);
            break;
        }
        case JointKind::Prismatic: {
            m2::PrismaticJoint r;
            r.frameA = ToFrame(joint.frameA);
            r.frameB = ToFrame(joint.frameB);
            r.lowerLimit = lower;
            r.upperLimit = upper;
            r.referenceTranslation = joint.referenceTranslation;
            r.maxMotorForce = joint.maxMotorForce;
            r.motorSpeed = joint.motorSpeed;
            r.motorMode = RawMotor(joint.motor);
            r.motorFrequencyHz = joint.linearSpring.hz;
            r.motorDampingRatio = joint.linearSpring.damping;
            link.jointType = m2::PhysicsJointType::Prismatic;
            link.jointId = static_cast<i16>(data.prismaticJoints.size());
            data.prismaticJoints.push_back(r);
            break;
        }
        case JointKind::Distance:
            link.jointType = m2::PhysicsJointType::Distance;
            link.jointId = static_cast<i16>(data.distanceJoints.size());
            data.distanceJoints.push_back(m2::DistanceJoint{Row(joint.frameA, 3), Row(joint.frameB, 3), joint.restLength});
            break;
        case JointKind::Count:
            continue;
        }
        data.joints.push_back(link);
    }

    // --- the object the model becomes -------------------------------------------
    const PhysicsRig* owner = nullptr;
    for (const PhysicsRig& rig : physics.rigs) {
        if (rig.wow.has_value() && owner == nullptr) {
            owner = &rig;
        }
        if (rig.start == RigStart::OnDeath || rig.start == RigStart::Never) {
            out.info(DiagCode::PhysicsUnsupported,
                     "rig '" + rig.name + "' starts later, but World of Warcraft simulates from creation",
                     ElementRef(ElementKind::PhysicsRecord, rig.id), profile);
        }
    }
    const WowRigExtension wow = owner != nullptr ? *owner->wow : WowRigExtension{};
    data.phyt = RawKind(wow.kind);
    if (wow.vegetation.has_value()) {
        const WowVegetation& v = *wow.vegetation;
        data.tuning.push_back(m2::PhysicsTuning{v.posMaxPush, v.posPushAmt, v.posRelaxSpeed, v.velMaxPush, v.velSpeed,
                                                v.minPushDist});
    }
    if (!wow.allowList.empty()) {
        data.allowList = m2::PhysicsAllowList{0, wow.allowList};
    }
    return data;
}

} // namespace m2_physics
} // namespace wem
} // namespace models
} // namespace whiteout
