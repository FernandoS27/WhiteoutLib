#include "../m2_binary_parse_visitor.h"
#include "../../common/binary_reader.h"

namespace whiteout {
namespace m2 {

using common::BinaryReader;

// ============================================================================
// PHYS Header
// ============================================================================

void M2BinaryParseVisitor::visit(PHYSHeader& header) {
    header.version = reader.read<u16>();
}

// ============================================================================
// PHYT - Physics Type (version 1+)
// ============================================================================

void M2BinaryParseVisitor::visit(PHYTEntry& entry) {
    entry.phyt = reader.read<u32>();
}

// ============================================================================
// BODY Structures
// ============================================================================

void M2BinaryParseVisitor::visit(BODYEntry& entry) {
    entry.type = reader.read<u16>();
    reader.read<u8>(2); // padding
    entry.position = reader.read<Vector3f>();
    entry.modelBoneIndex = reader.read<u16>();
    reader.read<u8>(2); // padding
    entry.shapes_base = reader.read<i32>();
    entry.shapes_count = reader.read<i32>();
}

void M2BinaryParseVisitor::visit(BDY2Entry& entry) {
    entry.type = reader.read<u16>();
    reader.read<u8>(2); // padding
    entry.position = reader.read<Vector3f>();
    entry.modelBoneIndex = reader.read<u16>();
    reader.read<u8>(2); // padding
    entry.shapes_base = reader.read<i32>();
    entry.shapes_count = reader.read<i32>();
    entry.unk_1c = reader.read<f32>();
}

void M2BinaryParseVisitor::visit(BDY3Entry& entry) {
    entry.type = reader.read<u16>();
    entry.boneIndex = reader.read<u16>();
    entry.position = reader.read<Vector3f>();
    entry.shapeIndex = reader.read<u16>();
    reader.read<u8>(2); // padding
    entry.shapesCount = reader.read<i32>();
    entry.unk0 = reader.read<f32>();
    entry.unk_1c = reader.read<f32>();
    entry.drag = reader.read<f32>();
    entry.unk1 = reader.read<f32>();
    entry.unk_28 = reader.read<f32>();
}

void M2BinaryParseVisitor::visit(BDY4Entry& entry) {
    entry.type = reader.read<u16>();
    entry.boneIndex = reader.read<u16>();
    entry.position = reader.read<Vector3f>();
    entry.shapeIndex = reader.read<u16>();
    reader.read<u8>(2); // padding
    entry.shapesCount = reader.read<i32>();
    entry.unk0 = reader.read<f32>();
    entry.unk_1c = reader.read<f32>();
    entry.drag = reader.read<f32>();
    entry.unk1 = reader.read<f32>();
    entry.unk_28 = reader.read<f32>();
    reader.read<u8>(4); // unk_2c
}

// ============================================================================
// SHAP Structures - Shape Definitions
// ============================================================================

void M2BinaryParseVisitor::visit(SHAPEntry& entry) {
    entry.shapeType = reader.read<i16>();
    entry.shapeIndex = reader.read<i16>();
    reader.read<u8>(4); // unk
    entry.friction = reader.read<f32>();
    entry.restitution = reader.read<f32>();
    entry.density = reader.read<f32>();
}

void M2BinaryParseVisitor::visit(SHP2Entry& entry) {
    entry.shapeType = reader.read<i16>();
    entry.shapeIndex = reader.read<i16>();
    reader.read<u8>(4); // unk
    entry.friction = reader.read<f32>();
    entry.restitution = reader.read<f32>();
    entry.density = reader.read<f32>();
    entry.unk_14 = reader.read<u32>();
    entry.unk_18 = reader.read<f32>();
    entry.unk_1c = reader.read<u16>();
    entry.unk_1e = reader.read<u16>();
}

// ============================================================================
// Shape Data Structures
// ============================================================================

void M2BinaryParseVisitor::visit(BOXSEntry& entry) {
    entry.matrix.data = reader.read<std::array<f32, 12>>();
    entry.center = reader.read<Vector3f>();
}

void M2BinaryParseVisitor::visit(CAPSEntry& entry) {
    entry.localPosition1 = reader.read<Vector3f>();
    entry.localPosition2 = reader.read<Vector3f>();
    entry.radius = reader.read<f32>();
}

void M2BinaryParseVisitor::visit(SPHSEntry& entry) {
    entry.localPosition = reader.read<Vector3f>();
    entry.radius = reader.read<f32>();
}

void M2BinaryParseVisitor::visit(PLYTNode& node) {
    node.unk_00 = reader.read<i8>();
    node.vertexIndex = reader.read<i8>();
    node.unkIndex0 = reader.read<i8>();
    node.unkIndex1 = reader.read<i8>();
}

void M2BinaryParseVisitor::visit(PLYTData& data, const PLYTHeader& header) {
    // Read vertices
    data.vertices = reader.read<std::vector<Vector3f>>(header.vertexCount);
    data.unk_1 = reader.read<std::vector<u8>>(header.count_10);
    data.unk_2 = reader.read<std::vector<u8>>(header.count_10);
    // Read nodes (nodeCount elements)
    data.nodes.reserve(header.nodeCount);
    for (u32 i = 0; i < header.nodeCount; ++i) {
        PLYTNode node;
        visit(node);
        data.nodes.push_back(std::move(node));
    }
}

void M2BinaryParseVisitor::visit(PLYTEntry& entry) {
    PLYTHeader& header = entry.header;
    header.vertexCount = reader.read<u32>();
    reader.read<u8>(4); // unk_04
    header.runtime_08_ptr = reader.read<u64>();
    header.count_10 = reader.read<u32>();
    reader.read<u8>(4); // unk_14
    header.runtime_18_ptr = reader.read<u64>();
    header.runtime_20_ptr = reader.read<u64>();
    header.nodeCount = reader.read<u32>();
    reader.read<u8>(4); // unk_2c
    header.runtime_30_ptr = reader.read<u64>();
    for (i32 i = 0; i < 6; ++i) {
        header.unk_38[i] = reader.read<f32>();
    }
    
    visit(entry.data, header);
}

// ============================================================================
// JOIN - Joint Entry
// ============================================================================

void M2BinaryParseVisitor::visit(JOINEntry& entry) {
    entry.bodyAIdx = reader.read<u32>();
    entry.bodyBIdx = reader.read<u32>();
    reader.read<u8>(4); // unk
    entry.jointType = reader.read<i16>();
    entry.jointId = reader.read<i16>();
}

// ============================================================================
// Joint Data Structures
// ============================================================================

void M2BinaryParseVisitor::visit(Matrix3x4& matrix) {
    matrix.data = reader.read<std::array<f32, 12>>();
}

void M2BinaryParseVisitor::visit(WELJEntry& entry) {
    entry.frameA = reader.read<Matrix3x4>();
    entry.frameB = reader.read<Matrix3x4>();
    entry.angularFrequencyHz = reader.read<f32>();
    entry.angularDampingRatio = reader.read<f32>();
}

void M2BinaryParseVisitor::visit(WLJ2Entry& entry) {
    entry.frameA = reader.read<Matrix3x4>();
    entry.frameB = reader.read<Matrix3x4>();
    entry.angularFrequencyHz = reader.read<f32>();
    entry.angularDampingRatio = reader.read<f32>();
    entry.linearFrequencyHz = reader.read<f32>();
    entry.linearDampingRatio = reader.read<f32>();
}

void M2BinaryParseVisitor::visit(WLJ3Entry& entry) {
    entry.frameA = reader.read<Matrix3x4>();
    entry.frameB = reader.read<Matrix3x4>();
    entry.angularFrequencyHz = reader.read<f32>();
    entry.angularDampingRatio = reader.read<f32>();
    entry.linearFrequencyHz = reader.read<f32>();
    entry.linearDampingRatio = reader.read<f32>();
    entry.unk70 = reader.read<f32>();
}

void M2BinaryParseVisitor::visit(SPHJEntry& entry) {
    entry.anchorA = reader.read<Vector3f>();
    entry.anchorB = reader.read<Vector3f>();
    entry.frictionTorque = reader.read<f32>();
}

void M2BinaryParseVisitor::visit(SHOJEntry& entry) {
    entry.frameA = reader.read<Matrix3x4>();
    entry.frameB = reader.read<Matrix3x4>();
    entry.lowerTwistAngle = reader.read<f32>();
    entry.upperTwistAngle = reader.read<f32>();
    entry.coneAngle = reader.read<f32>();
    entry.maxMotorTorque = reader.read<f32>();
    entry.motorMode = reader.read<u32>();
}

void M2BinaryParseVisitor::visit(SHJ2Entry& entry) {
    entry.frameA = reader.read<Matrix3x4>();
    entry.frameB = reader.read<Matrix3x4>();
    entry.lowerTwistAngle = reader.read<f32>();
    entry.upperTwistAngle = reader.read<f32>();
    entry.coneAngle = reader.read<f32>();
    entry.maxMotorTorque = reader.read<f32>();
    entry.motorMode = reader.read<u32>();
    entry.motorFrequencyHz = reader.read<f32>();
    entry.motorDampingRatio = reader.read<f32>();
}

void M2BinaryParseVisitor::visit(PRSJEntry& entry) {
    entry.frameA = reader.read<Matrix3x4>();
    entry.frameB = reader.read<Matrix3x4>();
    entry.lowerLimit = reader.read<f32>();
    entry.upperLimit = reader.read<f32>();
    entry.unk_68 = reader.read<f32>();
    entry.maxMotorForce = reader.read<f32>();
    entry.unk_70 = reader.read<f32>();
    entry.motorMode = reader.read<u32>();
}

void M2BinaryParseVisitor::visit(PRS2Entry& entry) {
    entry.frameA = reader.read<Matrix3x4>();
    entry.frameB = reader.read<Matrix3x4>();
    entry.lowerLimit = reader.read<f32>();
    entry.upperLimit = reader.read<f32>();
    entry.unk_68 = reader.read<f32>();
    entry.maxMotorForce = reader.read<f32>();
    entry.unk_70 = reader.read<f32>();
    entry.motorMode = reader.read<u32>();
    entry.motorFrequencyHz = reader.read<f32>();
    entry.motorDampingRatio = reader.read<f32>();
}

void M2BinaryParseVisitor::visit(REVJEntry& entry) {
    entry.frameA = reader.read<Matrix3x4>();
    entry.frameB = reader.read<Matrix3x4>();
    entry.lowerAngle = reader.read<f32>();
    entry.upperAngle = reader.read<f32>();
    entry.maxMotorTorque = reader.read<f32>();
    entry.motorMode = reader.read<u32>();
}

void M2BinaryParseVisitor::visit(REV2Entry& entry) {
    entry.frameA = reader.read<Matrix3x4>();
    entry.frameB = reader.read<Matrix3x4>();
    entry.lowerAngle = reader.read<f32>();
    entry.upperAngle = reader.read<f32>();
    entry.maxMotorTorque = reader.read<f32>();
    entry.motorMode = reader.read<u32>();
    entry.motorFrequencyHz = reader.read<f32>();
    entry.motorDampingRatio = reader.read<f32>();
}

void M2BinaryParseVisitor::visit(DSTJEntry& entry) {
    entry.localAnchorA = reader.read<Vector3f>();
    entry.localAnchorB = reader.read<Vector3f>();
    entry.some_distance_factor = reader.read<f32>();
}

// ============================================================================
// PHYV - Physics Values (version 1+)
// ============================================================================

void M2BinaryParseVisitor::visit(PHYVEntry& entry) {
    for (i32 i = 0; i < 6; ++i) {
        entry.unk[i] = reader.read<f32>();
    }
}

} // namespace m2
} // namespace whiteout
