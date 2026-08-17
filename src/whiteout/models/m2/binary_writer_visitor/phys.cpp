
#include "../../../common/binary_writer.h"
#include "../binary_writer_visitor.h"

namespace whiteout {
namespace m2 {

using common::BinaryWriter;

void BinaryWriterVisitor::visit(const PhysicsFrame& frame) {
    writer.write(frame.axisX);
    writer.write(frame.axisY);
    writer.write(frame.axisZ);
    writer.write(frame.origin);
}

void BinaryWriterVisitor::visit(const PhysicsBody& body, PhysBodyLayout layout) {
    writer.write(static_cast<u16>(body.type));
    const bool legacy = layout == PhysBodyLayout::Body || layout == PhysBodyLayout::Body2;
    if (legacy) {
        writer.writePadding(2);
        writer.write(body.position);
        writer.write(body.boneIndex);
        writer.writePadding(2);
        writer.write(body.shapeIndex);
        writer.write(body.shapeCount);
        if (layout == PhysBodyLayout::Body2) {
            writer.write(body.inertiaScale);
        }
        return;
    }

    writer.write(body.boneIndex);
    writer.write(body.position);
    writer.write(static_cast<u16>(body.shapeIndex));
    writer.writePadding(2);
    writer.write(body.shapeCount);
    writer.write(body.gravityScale);
    writer.write(body.inertiaScale);
    writer.write(body.linearDamping);
    writer.write(body.angularDamping);
    writer.write(body.unknown28);
    if (layout == PhysBodyLayout::Body4) {
        writer.write(body.unknown2c);
        writer.write(body.padding2e);
    }
}

void BinaryWriterVisitor::visit(const PhysicsShape& shape, PhysShapeLayout layout) {
    writer.write(static_cast<u16>(shape.shapeType));
    writer.write(shape.shapeIndex);
    writer.write(shape.padding04);
    writer.write(shape.friction);
    writer.write(shape.restitution);
    writer.write(shape.density);
    if (layout == PhysShapeLayout::Shape2) {
        writer.write(shape.unknown14);
        writer.write(shape.scale);
        writer.write(shape.unknown1c);
        writer.write(shape.padding1e);
    }
}

void BinaryWriterVisitor::visit(const BoxShape& shape) {
    visit(shape.frame);
    writer.write(shape.halfExtents);
}

void BinaryWriterVisitor::visit(const CapsuleShape& shape) {
    writer.write(shape.localPosition1);
    writer.write(shape.localPosition2);
    writer.write(shape.radius);
}

void BinaryWriterVisitor::visit(const SphereShape& shape) {
    writer.write(shape.localPosition);
    writer.write(shape.radius);
}

void BinaryWriterVisitor::visit(const PhysicsJoint& joint) {
    writer.write(joint.bodyAIndex);
    writer.write(joint.bodyBIndex);
    writer.write(joint.padding08);
    writer.write(static_cast<u16>(joint.jointType));
    writer.write(joint.jointId);
}

void BinaryWriterVisitor::visit(const WeldJoint& joint, PhysWeldLayout layout) {
    visit(joint.frameA);
    visit(joint.frameB);
    writer.write(joint.angularFrequencyHz);
    writer.write(joint.angularDampingRatio);
    if (layout == PhysWeldLayout::Weld) {
        return;
    }
    writer.write(joint.linearFrequencyHz);
    writer.write(joint.linearDampingRatio);
    if (layout == PhysWeldLayout::Weld3) {
        writer.write(joint.unknown70);
    }
}

void BinaryWriterVisitor::visit(const SphericalJoint& joint) {
    writer.write(joint.anchorA);
    writer.write(joint.anchorB);
    writer.write(joint.frictionTorque);
}

void BinaryWriterVisitor::visit(const ShoulderJoint& joint, PhysShoulderLayout layout) {
    visit(joint.frameA);
    visit(joint.frameB);
    writer.write(joint.lowerTwistAngle);
    writer.write(joint.upperTwistAngle);
    writer.write(joint.coneAngle);
    if (layout == PhysShoulderLayout::Shoulder) {
        return;
    }
    writer.write(joint.maxMotorTorque);
    writer.write(joint.motorMode);
    if (layout == PhysShoulderLayout::Shoulder2) {
        writer.write(joint.motorFrequencyHz);
        writer.write(joint.motorDampingRatio);
    }
}

void BinaryWriterVisitor::visit(const PrismaticJoint& joint, PhysMotorLayout layout) {
    visit(joint.frameA);
    visit(joint.frameB);
    writer.write(joint.lowerLimit);
    writer.write(joint.upperLimit);
    writer.write(joint.unknown68);
    writer.write(joint.maxMotorForce);
    writer.write(joint.unknown70);
    writer.write(joint.motorMode);
    if (layout == PhysMotorLayout::Sprung) {
        writer.write(joint.motorFrequencyHz);
        writer.write(joint.motorDampingRatio);
    }
}

void BinaryWriterVisitor::visit(const RevoluteJoint& joint, PhysMotorLayout layout) {
    visit(joint.frameA);
    visit(joint.frameB);
    writer.write(joint.lowerAngle);
    writer.write(joint.upperAngle);
    writer.write(joint.maxMotorTorque);
    writer.write(joint.motorMode);
    if (layout == PhysMotorLayout::Sprung) {
        writer.write(joint.motorFrequencyHz);
        writer.write(joint.motorDampingRatio);
    }
}

void BinaryWriterVisitor::visit(const DistanceJoint& joint) {
    writer.write(joint.localAnchorA);
    writer.write(joint.localAnchorB);
    writer.write(joint.distance);
}

void BinaryWriterVisitor::visit(const PhysicsTuning& tuning) {
    writer.write(tuning.values);
}

// Counts come back from the vectors, and the four pointer fields go out zero —
// the client fills those in once the payload is in memory.
void BinaryWriterVisitor::visit(const std::vector<PolytopeShape>& shapes) {
    writer.write(static_cast<u32>(shapes.size()));

    for (const auto& shape : shapes) {
        writer.write(static_cast<u32>(shape.vertices.size()));
        writer.write(shape.padding04);
        writer.write<u64>(0);
        writer.write(static_cast<u32>(shape.facePlanes.size()));
        writer.write(shape.padding14);
        writer.write<u64>(0);
        writer.write<u64>(0);
        writer.write(static_cast<u32>(shape.edges.size()));
        writer.write(shape.padding2c);
        writer.write<u64>(0);
        writer.write(shape.centroid);
        writer.write(shape.volume);
        writer.write(shape.surfaceArea);
        writer.write(shape.padding4c);
    }

    for (const auto& shape : shapes) {
        writer.write(shape.vertices);
        writer.write(shape.facePlanes);
        writer.write(shape.faceFirstEdges);
        for (const auto& edge : shape.edges) {
            writer.write(edge.twinOffset);
            writer.write(edge.originVertex);
            writer.write(edge.faceIndex);
            writer.write(edge.nextEdge);
        }
    }
}

} // namespace m2
} // namespace whiteout
