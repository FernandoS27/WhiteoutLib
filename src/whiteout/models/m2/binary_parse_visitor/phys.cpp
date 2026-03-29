
#include "../../../common/binary_reader.h"
#include "../binary_parse_visitor.h"

namespace whiteout {
namespace m2 {

using common::BinaryReader;

void BinaryParseVisitor::visit(PHYSHeader& header) {
    header.version = reader.read<u16>();
}

void BinaryParseVisitor::visit(PHYTEntry& entry) {
    entry.phyt = reader.read<u32>();
}

void BinaryParseVisitor::visit(BODYEntry& entry) {
    entry.type = reader.read<u16>();
    reader.read<u8>(2);
    entry.position = reader.read<Vector3f>();
    entry.modelBoneIndex = reader.read<u16>();
    reader.read<u8>(2);
    entry.shapes_base = reader.read<i32>();
    entry.shapes_count = reader.read<i32>();
}

void BinaryParseVisitor::visit(BDY2Entry& entry) {
    entry.type = reader.read<u16>();
    reader.read<u8>(2);
    entry.position = reader.read<Vector3f>();
    entry.modelBoneIndex = reader.read<u16>();
    reader.read<u8>(2);
    entry.shapes_base = reader.read<i32>();
    entry.shapes_count = reader.read<i32>();
    entry.massScale = reader.read<f32>();
}

void BinaryParseVisitor::visit(BDY3Entry& entry) {
    entry.type = reader.read<u16>();
    entry.boneIndex = reader.read<u16>();
    entry.position = reader.read<Vector3f>();
    entry.shapeIndex = reader.read<u16>();
    reader.read<u8>(2);
    entry.shapesCount = reader.read<i32>();
    entry.mass = reader.read<f32>();
    entry.massScale = reader.read<f32>();
    entry.drag = reader.read<f32>();
    entry.angularDamping = reader.read<f32>();
    entry.linearDamping = reader.read<f32>();
}

void BinaryParseVisitor::visit(BDY4Entry& entry) {
    entry.type = reader.read<u16>();
    entry.boneIndex = reader.read<u16>();
    entry.position = reader.read<Vector3f>();
    entry.shapeIndex = reader.read<u16>();
    reader.read<u8>(2);
    entry.shapesCount = reader.read<i32>();
    entry.mass = reader.read<f32>();
    entry.massScale = reader.read<f32>();
    entry.drag = reader.read<f32>();
    entry.angularDamping = reader.read<f32>();
    entry.linearDamping = reader.read<f32>();
    reader.read<u8>(4);
}

void BinaryParseVisitor::visit(SHAPEntry& entry) {
    entry.shapeType = reader.read<i16>();
    entry.shapeIndex = reader.read<i16>();
    reader.read<u8>(4);
    entry.friction = reader.read<f32>();
    entry.restitution = reader.read<f32>();
    entry.density = reader.read<f32>();
}

void BinaryParseVisitor::visit(SHP2Entry& entry) {
    entry.shapeType = reader.read<i16>();
    entry.shapeIndex = reader.read<i16>();
    reader.read<u8>(4);
    entry.friction = reader.read<f32>();
    entry.restitution = reader.read<f32>();
    entry.density = reader.read<f32>();
    entry.unk_14 = reader.read<u32>();
    entry.unk_18 = reader.read<f32>();
    entry.unk_1c = reader.read<u16>();
    entry.padding_1e = reader.read<u16>();
}

void BinaryParseVisitor::visit(BOXSEntry& entry) {
    entry.matrix.data = reader.read<std::array<f32, 12>>();
    entry.center = reader.read<Vector3f>();
}

void BinaryParseVisitor::visit(CAPSEntry& entry) {
    entry.localPosition1 = reader.read<Vector3f>();
    entry.localPosition2 = reader.read<Vector3f>();
    entry.radius = reader.read<f32>();
}

void BinaryParseVisitor::visit(SPHSEntry& entry) {
    entry.localPosition = reader.read<Vector3f>();
    entry.radius = reader.read<f32>();
}

void BinaryParseVisitor::visit(PLYTNode& node) {
    node.byte0 = reader.read<u8>();
    node.byte1 = reader.read<u8>();
    node.byte2 = reader.read<u8>();
    node.byte3 = reader.read<u8>();
}

void BinaryParseVisitor::visit(PLYTData& data, const PLYTHeader& header) {

    data.vertices = reader.read<std::vector<Vector3f>>(header.vertexCount);

    data.facePlanes.resize(header.faceCount);
    for (u32 i = 0; i < header.faceCount; ++i) {
        data.facePlanes[i].x = reader.read<f32>();
        data.facePlanes[i].y = reader.read<f32>();
        data.facePlanes[i].z = reader.read<f32>();
        data.facePlanes[i].w = reader.read<f32>();
    }

    data.nodes.reserve(header.nodeCount);
    for (u32 i = 0; i < header.nodeCount; ++i) {
        PLYTNode node;
        visit(node);
        data.nodes.push_back(std::move(node));
    }

    data.faceIndices = reader.read<std::vector<u8>>(header.faceCount);
}

void BinaryParseVisitor::visit(PLYTEntry& entry) {
    PLYTHeader& header = entry.header;
    header.vertexCount = reader.read<u32>();
    header.unk_04 = reader.read<u32>();
    header.runtime_08_ptr = reader.read<u64>();
    header.faceCount = reader.read<u32>();
    header.unk_14 = reader.read<u32>();
    header.runtime_18_ptr = reader.read<u64>();
    header.runtime_20_ptr = reader.read<u64>();
    header.nodeCount = reader.read<u32>();
    header.unk_2c = reader.read<u32>();
    header.runtime_30_ptr = reader.read<u64>();
    for (i32 i = 0; i < 6; ++i) {
        header.bounds[i] = reader.read<f32>();
    }

    visit(entry.data, header);
}

void BinaryParseVisitor::visit(JOINEntry& entry) {
    entry.bodyAIdx = reader.read<u32>();
    entry.bodyBIdx = reader.read<u32>();
    reader.read<u8>(4);
    entry.jointType = reader.read<i16>();
    entry.jointId = reader.read<i16>();
}

void BinaryParseVisitor::visit(Matrix3x4& matrix) {
    matrix.data = reader.read<std::array<f32, 12>>();
}

void BinaryParseVisitor::visit(WELJEntry& entry) {
    entry.frameA = reader.read<Matrix3x4>();
    entry.frameB = reader.read<Matrix3x4>();
    entry.angularFrequencyHz = reader.read<f32>();
    entry.angularDampingRatio = reader.read<f32>();
}

void BinaryParseVisitor::visit(WLJ2Entry& entry) {
    entry.frameA = reader.read<Matrix3x4>();
    entry.frameB = reader.read<Matrix3x4>();
    entry.angularFrequencyHz = reader.read<f32>();
    entry.angularDampingRatio = reader.read<f32>();
    entry.linearFrequencyHz = reader.read<f32>();
    entry.linearDampingRatio = reader.read<f32>();
}

void BinaryParseVisitor::visit(WLJ3Entry& entry) {
    entry.frameA = reader.read<Matrix3x4>();
    entry.frameB = reader.read<Matrix3x4>();
    entry.angularFrequencyHz = reader.read<f32>();
    entry.angularDampingRatio = reader.read<f32>();
    entry.linearFrequencyHz = reader.read<f32>();
    entry.linearDampingRatio = reader.read<f32>();
    entry.unk70 = reader.read<f32>();
}

void BinaryParseVisitor::visit(SPHJEntry& entry) {
    entry.anchorA = reader.read<Vector3f>();
    entry.anchorB = reader.read<Vector3f>();
    entry.frictionTorque = reader.read<f32>();
}

void BinaryParseVisitor::visit(SHOJEntry& entry) {
    entry.frameA = reader.read<Matrix3x4>();
    entry.frameB = reader.read<Matrix3x4>();
    entry.lowerTwistAngle = reader.read<f32>();
    entry.upperTwistAngle = reader.read<f32>();
    entry.coneAngle = reader.read<f32>();
    entry.maxMotorTorque = reader.read<f32>();
    entry.motorMode = reader.read<u32>();
}

void BinaryParseVisitor::visit(SHJ2Entry& entry) {
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

void BinaryParseVisitor::visit(PRSJEntry& entry) {
    entry.frameA = reader.read<Matrix3x4>();
    entry.frameB = reader.read<Matrix3x4>();
    entry.lowerLimit = reader.read<f32>();
    entry.upperLimit = reader.read<f32>();
    entry.unk_68 = reader.read<f32>();
    entry.maxMotorForce = reader.read<f32>();
    entry.unk_70 = reader.read<f32>();
    entry.motorMode = reader.read<u32>();
}

void BinaryParseVisitor::visit(PRS2Entry& entry) {
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

void BinaryParseVisitor::visit(REVJEntry& entry) {
    entry.frameA = reader.read<Matrix3x4>();
    entry.frameB = reader.read<Matrix3x4>();
    entry.lowerAngle = reader.read<f32>();
    entry.upperAngle = reader.read<f32>();
    entry.maxMotorTorque = reader.read<f32>();
    entry.motorMode = reader.read<u32>();
}

void BinaryParseVisitor::visit(REV2Entry& entry) {
    entry.frameA = reader.read<Matrix3x4>();
    entry.frameB = reader.read<Matrix3x4>();
    entry.lowerAngle = reader.read<f32>();
    entry.upperAngle = reader.read<f32>();
    entry.maxMotorTorque = reader.read<f32>();
    entry.motorMode = reader.read<u32>();
    entry.motorFrequencyHz = reader.read<f32>();
    entry.motorDampingRatio = reader.read<f32>();
}

void BinaryParseVisitor::visit(DSTJEntry& entry) {
    entry.localAnchorA = reader.read<Vector3f>();
    entry.localAnchorB = reader.read<Vector3f>();
    entry.some_distance_factor = reader.read<f32>();
}

void BinaryParseVisitor::visit(PHYVEntry& entry) {
    for (i32 i = 0; i < 6; ++i) {
        entry.unk[i] = reader.read<f32>();
    }
}

} // namespace m2
} // namespace whiteout
