#include "../m2_binary_writer_visitor.h"
#include "../include/common/binary_writer.h"

namespace m2 {

using common::BinaryWriter;

// ============================================================================
// PHYS Header
// ============================================================================

void M2BinaryWriterVisitor::visit(const PHYSHeader& header) {
    writer.write(header.version);
}

// ============================================================================
// PHYT - Physics Type (version 1+)
// ============================================================================

void M2BinaryWriterVisitor::visit(const PHYTEntry& entry) {
    writer.write(entry.phyt);
}

// ============================================================================
// BODY Structures
// ============================================================================

void M2BinaryWriterVisitor::visit(const BODYEntry& entry) {
    writer.write(entry.type);
    writer.write(std::array<u8, 2>{0, 0}); // padding
    writer.write(entry.position);
    writer.write(entry.modelBoneIndex);
    writer.write(std::array<u8, 2>{0, 0}); // padding
    writer.write(entry.shapes_base);
    writer.write(entry.shapes_count);
}

void M2BinaryWriterVisitor::visit(const BDY2Entry& entry) {
    writer.write(entry.type);
    writer.write(std::array<u8, 2>{0, 0}); // padding
    writer.write(entry.position);
    writer.write(entry.modelBoneIndex);
    writer.write(std::array<u8, 2>{0, 0}); // padding
    writer.write(entry.shapes_base);
    writer.write(entry.shapes_count);
    writer.write(entry.unk_1c);
}

void M2BinaryWriterVisitor::visit(const BDY3Entry& entry) {
    writer.write(entry.type);
    writer.write(entry.boneIndex);
    writer.write(entry.position);
    writer.write(entry.shapeIndex);
    writer.write(std::array<u8, 2>{0, 0}); // padding
    writer.write(entry.shapesCount);
    writer.write(entry.unk0);
    writer.write(entry.unk_1c);
    writer.write(entry.drag);
    writer.write(entry.unk1);
    writer.write(entry.unk_28);
}

void M2BinaryWriterVisitor::visit(const BDY4Entry& entry) {
    writer.write(entry.type);
    writer.write(entry.boneIndex);
    writer.write(entry.position);
    writer.write(entry.shapeIndex);
    writer.write(std::array<u8, 2>{0, 0}); // padding
    writer.write(entry.shapesCount);
    writer.write(entry.unk0);
    writer.write(entry.unk_1c);
    writer.write(entry.drag);
    writer.write(entry.unk1);
    writer.write(entry.unk_28);
    writer.write(std::array<u8, 4>{0, 0, 0, 0}); // unk_2c
}

// ============================================================================
// SHAP Structures - Shape Definitions
// ============================================================================

void M2BinaryWriterVisitor::visit(const SHAPEntry& entry) {
    writer.write(entry.shapeType);
    writer.write(entry.shapeIndex);
    writer.write(std::array<u8, 4>{0, 0, 0, 0}); // unk
    writer.write(entry.friction);
    writer.write(entry.restitution);
    writer.write(entry.density);
}

void M2BinaryWriterVisitor::visit(const SHP2Entry& entry) {
    writer.write(entry.shapeType);
    writer.write(entry.shapeIndex);
    writer.write(std::array<u8, 4>{0, 0, 0, 0}); // unk
    writer.write(entry.friction);
    writer.write(entry.restitution);
    writer.write(entry.density);
    writer.write(entry.unk_14);
    writer.write(entry.unk_18);
    writer.write(entry.unk_1c);
    writer.write(entry.unk_1e);
}

// ============================================================================
// Shape Data Structures
// ============================================================================

void M2BinaryWriterVisitor::visit(const BOXSEntry& entry) {
    writer.write(entry.matrix.data);
    writer.write(entry.center);
}

void M2BinaryWriterVisitor::visit(const CAPSEntry& entry) {
    writer.write(entry.localPosition1);
    writer.write(entry.localPosition2);
    writer.write(entry.radius);
}

void M2BinaryWriterVisitor::visit(const SPHSEntry& entry) {
    writer.write(entry.localPosition);
    writer.write(entry.radius);
}

void M2BinaryWriterVisitor::visit(const PLYTNode& node) {
    writer.write(node.unk_00);
    writer.write(node.vertexIndex);
    writer.write(node.unkIndex0);
    writer.write(node.unkIndex1);
}

void M2BinaryWriterVisitor::visit(const PLYTData& data, const PLYTHeader& header) {
    // Write vertices
    for (const auto& vertex : data.vertices) {
        writer.write(vertex);
    }
    
    // Write unk_1 array
    for (const auto& val : data.unk_1) {
        writer.write(val);
    }
    
    // Write unk_2 array
    for (const auto& val : data.unk_2) {
        writer.write(val);
    }
    
    // Write nodes
    for (const auto& node : data.nodes) {
        visit(node);
    }
}

void M2BinaryWriterVisitor::visit(const PLYTEntry& entry) {
    const PLYTHeader& header = entry.header;
    writer.write(header.vertexCount);
    writer.write(std::array<u8, 4>{0, 0, 0, 0}); // unk_04
    writer.write(header.runtime_08_ptr);
    writer.write(header.count_10);
    writer.write(std::array<u8, 4>{0, 0, 0, 0}); // unk_14
    writer.write(header.runtime_18_ptr);
    writer.write(header.runtime_20_ptr);
    writer.write(header.nodeCount);
    writer.write(std::array<u8, 4>{0, 0, 0, 0}); // unk_2c
    writer.write(header.runtime_30_ptr);
    for (i32 i = 0; i < 6; ++i) {
        writer.write(header.unk_38[i]);
    }
    
    visit(entry.data, header);
}

// ============================================================================
// JOIN - Joint Entry
// ============================================================================

void M2BinaryWriterVisitor::visit(const JOINEntry& entry) {
    writer.write(entry.bodyAIdx);
    writer.write(entry.bodyBIdx);
    writer.write(std::array<u8, 4>{0, 0, 0, 0}); // unk
    writer.write(entry.jointType);
    writer.write(entry.jointId);
}

// ============================================================================
// Joint Data Structures
// ============================================================================

void M2BinaryWriterVisitor::visit(const Matrix3x4& matrix) {
    writer.write(matrix.data);
}

void M2BinaryWriterVisitor::visit(const WELJEntry& entry) {
    visit(entry.frameA);
    visit(entry.frameB);
    writer.write(entry.angularFrequencyHz);
    writer.write(entry.angularDampingRatio);
}

void M2BinaryWriterVisitor::visit(const WLJ2Entry& entry) {
    visit(entry.frameA);
    visit(entry.frameB);
    writer.write(entry.angularFrequencyHz);
    writer.write(entry.angularDampingRatio);
    writer.write(entry.linearFrequencyHz);
    writer.write(entry.linearDampingRatio);
}

void M2BinaryWriterVisitor::visit(const WLJ3Entry& entry) {
    visit(entry.frameA);
    visit(entry.frameB);
    writer.write(entry.angularFrequencyHz);
    writer.write(entry.angularDampingRatio);
    writer.write(entry.linearFrequencyHz);
    writer.write(entry.linearDampingRatio);
    writer.write(entry.unk70);
}

void M2BinaryWriterVisitor::visit(const SPHJEntry& entry) {
    writer.write(entry.anchorA);
    writer.write(entry.anchorB);
    writer.write(entry.frictionTorque);
}

void M2BinaryWriterVisitor::visit(const SHOJEntry& entry) {
    visit(entry.frameA);
    visit(entry.frameB);
    writer.write(entry.lowerTwistAngle);
    writer.write(entry.upperTwistAngle);
    writer.write(entry.coneAngle);
    writer.write(entry.maxMotorTorque);
    writer.write(entry.motorMode);
}

void M2BinaryWriterVisitor::visit(const SHJ2Entry& entry) {
    visit(entry.frameA);
    visit(entry.frameB);
    writer.write(entry.lowerTwistAngle);
    writer.write(entry.upperTwistAngle);
    writer.write(entry.coneAngle);
    writer.write(entry.maxMotorTorque);
    writer.write(entry.motorMode);
    writer.write(entry.motorFrequencyHz);
    writer.write(entry.motorDampingRatio);
}

void M2BinaryWriterVisitor::visit(const PRSJEntry& entry) {
    visit(entry.frameA);
    visit(entry.frameB);
    writer.write(entry.lowerLimit);
    writer.write(entry.upperLimit);
    writer.write(entry.unk_68);
    writer.write(entry.maxMotorForce);
    writer.write(entry.unk_70);
    writer.write(entry.motorMode);
}

void M2BinaryWriterVisitor::visit(const PRS2Entry& entry) {
    visit(entry.frameA);
    visit(entry.frameB);
    writer.write(entry.lowerLimit);
    writer.write(entry.upperLimit);
    writer.write(entry.unk_68);
    writer.write(entry.maxMotorForce);
    writer.write(entry.unk_70);
    writer.write(entry.motorMode);
    writer.write(entry.motorFrequencyHz);
    writer.write(entry.motorDampingRatio);
}

void M2BinaryWriterVisitor::visit(const REVJEntry& entry) {
    visit(entry.frameA);
    visit(entry.frameB);
    writer.write(entry.lowerAngle);
    writer.write(entry.upperAngle);
    writer.write(entry.maxMotorTorque);
    writer.write(entry.motorMode);
}

void M2BinaryWriterVisitor::visit(const REV2Entry& entry) {
    visit(entry.frameA);
    visit(entry.frameB);
    writer.write(entry.lowerAngle);
    writer.write(entry.upperAngle);
    writer.write(entry.maxMotorTorque);
    writer.write(entry.motorMode);
    writer.write(entry.motorFrequencyHz);
    writer.write(entry.motorDampingRatio);
}

void M2BinaryWriterVisitor::visit(const DSTJEntry& entry) {
    writer.write(entry.localAnchorA);
    writer.write(entry.localAnchorB);
    writer.write(entry.some_distance_factor);
}

// ============================================================================
// PHYV - Physics Values (version 1+)
// ============================================================================

void M2BinaryWriterVisitor::visit(const PHYVEntry& entry) {
    for (i32 i = 0; i < 6; ++i) {
        writer.write(entry.unk[i]);
    }
}

} // namespace m2
