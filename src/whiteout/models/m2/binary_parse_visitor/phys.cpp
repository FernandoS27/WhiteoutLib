
#include "../../../common/binary_reader.h"
#include "../binary_parse_visitor.h"

namespace whiteout {
namespace m2 {

using common::BinaryReader;

void BinaryParseVisitor::visit(PhysicsFrame& frame) {
    frame.axisX = reader.read<Vector3f>();
    frame.axisY = reader.read<Vector3f>();
    frame.axisZ = reader.read<Vector3f>();
    frame.origin = reader.read<Vector3f>();
}

void BinaryParseVisitor::visit(PhysicsBody& body, PhysBodyLayout layout) {
    body.type = static_cast<PhysicsBodyType>(reader.read<u16>());
    // BODY/BDY2 keep the bone index down at 0x10 and address shapes with a
    // 32-bit base; BDY3 moved it up into this padding and shrank the base.
    const bool legacy = layout == PhysBodyLayout::Body || layout == PhysBodyLayout::Body2;
    if (legacy) {
        reader.skip(2);
        body.position = reader.read<Vector3f>();
        body.boneIndex = reader.read<u16>();
        reader.skip(2);
        body.shapeIndex = reader.read<i32>();
        body.shapeCount = reader.read<i32>();
        if (layout == PhysBodyLayout::Body2) {
            body.inertiaScale = reader.read<f32>();
        }
        return;
    }

    body.boneIndex = reader.read<u16>();
    body.position = reader.read<Vector3f>();
    body.shapeIndex = reader.read<u16>();
    reader.skip(2);
    body.shapeCount = reader.read<i32>();
    body.gravityScale = reader.read<f32>();
    body.inertiaScale = reader.read<f32>();
    body.linearDamping = reader.read<f32>();
    body.angularDamping = reader.read<f32>();
    body.unknown28 = reader.read<f32>();
    if (layout == PhysBodyLayout::Body4) {
        body.unknown2c = reader.read<u16>();
        body.padding2e = reader.read<u16>();
    }
}

void BinaryParseVisitor::visit(PhysicsShape& shape, PhysShapeLayout layout) {
    shape.shapeType = static_cast<PhysicsShapeType>(reader.read<u16>());
    shape.shapeIndex = reader.read<i16>();
    shape.padding04 = reader.read<u32>();
    shape.friction = reader.read<f32>();
    shape.restitution = reader.read<f32>();
    shape.density = reader.read<f32>();
    if (layout == PhysShapeLayout::Shape2) {
        shape.unknown14 = reader.read<f32>();
        shape.scale = reader.read<f32>();
        shape.unknown1c = reader.read<u16>();
        shape.padding1e = reader.read<u16>();
    }
}

void BinaryParseVisitor::visit(BoxShape& shape) {
    visit(shape.frame);
    shape.halfExtents = reader.read<Vector3f>();
}

void BinaryParseVisitor::visit(CapsuleShape& shape) {
    shape.localPosition1 = reader.read<Vector3f>();
    shape.localPosition2 = reader.read<Vector3f>();
    shape.radius = reader.read<f32>();
}

void BinaryParseVisitor::visit(SphereShape& shape) {
    shape.localPosition = reader.read<Vector3f>();
    shape.radius = reader.read<f32>();
}

void BinaryParseVisitor::visit(PhysicsJoint& joint) {
    joint.bodyAIndex = reader.read<u32>();
    joint.bodyBIndex = reader.read<u32>();
    joint.padding08 = reader.read<u32>();
    joint.jointType = static_cast<PhysicsJointType>(reader.read<u16>());
    joint.jointId = reader.read<i16>();
}

void BinaryParseVisitor::visit(WeldJoint& joint, PhysWeldLayout layout) {
    visit(joint.frameA);
    visit(joint.frameB);
    joint.angularFrequencyHz = reader.read<f32>();
    joint.angularDampingRatio = reader.read<f32>();
    if (layout == PhysWeldLayout::Weld) {
        return;
    }
    joint.linearFrequencyHz = reader.read<f32>();
    joint.linearDampingRatio = reader.read<f32>();
    if (layout == PhysWeldLayout::Weld3) {
        joint.unknown70 = reader.read<f32>();
    }
}

void BinaryParseVisitor::visit(SphericalJoint& joint) {
    joint.anchorA = reader.read<Vector3f>();
    joint.anchorB = reader.read<Vector3f>();
    joint.frictionTorque = reader.read<f32>();
}

void BinaryParseVisitor::visit(ShoulderJoint& joint, PhysShoulderLayout layout) {
    visit(joint.frameA);
    visit(joint.frameB);
    joint.lowerTwistAngle = reader.read<f32>();
    joint.upperTwistAngle = reader.read<f32>();
    joint.coneAngle = reader.read<f32>();
    if (layout == PhysShoulderLayout::Shoulder) {
        return;
    }
    joint.maxMotorTorque = reader.read<f32>();
    joint.motorMode = reader.read<u32>();
    if (layout == PhysShoulderLayout::Shoulder2) {
        joint.motorFrequencyHz = reader.read<f32>();
        joint.motorDampingRatio = reader.read<f32>();
    }
}

void BinaryParseVisitor::visit(PrismaticJoint& joint, PhysMotorLayout layout) {
    visit(joint.frameA);
    visit(joint.frameB);
    joint.lowerLimit = reader.read<f32>();
    joint.upperLimit = reader.read<f32>();
    joint.unknown68 = reader.read<f32>();
    joint.maxMotorForce = reader.read<f32>();
    joint.unknown70 = reader.read<f32>();
    joint.motorMode = reader.read<u32>();
    if (layout == PhysMotorLayout::Sprung) {
        joint.motorFrequencyHz = reader.read<f32>();
        joint.motorDampingRatio = reader.read<f32>();
    }
}

void BinaryParseVisitor::visit(RevoluteJoint& joint, PhysMotorLayout layout) {
    visit(joint.frameA);
    visit(joint.frameB);
    joint.lowerAngle = reader.read<f32>();
    joint.upperAngle = reader.read<f32>();
    joint.maxMotorTorque = reader.read<f32>();
    joint.motorMode = reader.read<u32>();
    if (layout == PhysMotorLayout::Sprung) {
        joint.motorFrequencyHz = reader.read<f32>();
        joint.motorDampingRatio = reader.read<f32>();
    }
}

void BinaryParseVisitor::visit(DistanceJoint& joint) {
    joint.localAnchorA = reader.read<Vector3f>();
    joint.localAnchorB = reader.read<Vector3f>();
    joint.distance = reader.read<f32>();
}

void BinaryParseVisitor::visit(PhysicsTuning& tuning) {
    tuning.values = reader.readArray<f32, 6>();
}

// PLYT is the one chunk that sizes itself: a count, then that many fixed-size
// headers, then that many payloads whose lengths come from the headers. The
// two halves are read together so the payload split is never guessed.
void BinaryParseVisitor::visit(std::vector<PolytopeShape>& shapes) {
    const u32 count = reader.read<u32>();
    shapes.clear();
    shapes.resize(count);

    struct Counts {
        u32 vertices;
        u32 faces;
        u32 edges;
    };
    std::vector<Counts> counts(count);

    for (u32 i = 0; i < count; ++i) {
        auto& shape = shapes[i];
        counts[i].vertices = reader.read<u32>();
        shape.padding04 = reader.read<u32>();
        reader.skip(8); // runtime vertex pointer, always 0 on disk
        counts[i].faces = reader.read<u32>();
        shape.padding14 = reader.read<u32>();
        reader.skip(8); // runtime face-plane pointer
        reader.skip(8); // runtime face-edge pointer
        counts[i].edges = reader.read<u32>();
        shape.padding2c = reader.read<u32>();
        reader.skip(8); // runtime half-edge pointer
        shape.centroid = reader.read<Vector3f>();
        shape.volume = reader.read<f32>();
        shape.surfaceArea = reader.read<f32>();
        shape.padding4c = reader.read<u32>();
    }

    for (u32 i = 0; i < count; ++i) {
        auto& shape = shapes[i];
        shape.vertices = reader.read<std::vector<Vector3f>>(counts[i].vertices);
        shape.facePlanes = reader.read<std::vector<Vector4f>>(counts[i].faces);
        shape.faceFirstEdges = reader.read<std::vector<u8>>(counts[i].faces);
        shape.edges.resize(counts[i].edges);
        for (auto& edge : shape.edges) {
            edge.twinOffset = reader.read<i8>();
            edge.originVertex = reader.read<u8>();
            edge.faceIndex = reader.read<u8>();
            edge.nextEdge = reader.read<u8>();
        }
    }
}

} // namespace m2
} // namespace whiteout
