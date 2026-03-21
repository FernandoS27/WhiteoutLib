// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

void BinaryParseVisitor::visit(Sequence& value, u32 version) {
    value.id = reader.read<i32>();
    value.index = reader.read<i32>();
    visit(value.name);
    value.startFrame = reader.read<u32>();
    value.endFrame = reader.read<u32>();
    value.moveSpeed = reader.read<f32>();
    value.flags = reader.read<SequenceFlag>();
    value.frequency = reader.read<u32>();
    value.replayStart = reader.read<u32>();
    value.replayEnd = reader.read<u32>();
    value.blendTime = reader.read<u32>();
    if (version <= 1) {
        value.deprecated.unknown = reader.read<u32>();
    }
    value.bounds = reader.read<Extent>();
    visit(value.animationSets);
}

void BinaryParseVisitor::visit(SubTrackContainer& value, u32 version) {
    (void)version;

    visit(value.name);
    value.runsConcurrent = reader.read<u16>();
    value.animPriority = reader.read<u16>();
    value.animationStateIndex = reader.read<u16>();
    value.padding = reader.read<u16>();
    visit(value.animIds);
    visit(value.animRefs);
    value.unknown = reader.read<u32>();
    visit(value.sdev);
    visit(value.sd2v);
    visit(value.sd3v);
    visit(value.sd4q);
    visit(value.sdcc);
    visit(value.sdr3);
    visit(value.sdu8);
    visit(value.sds6);
    visit(value.sdu6);
    visit(value.sds3);
    visit(value.sdu3);
    visit(value.sdfg);
    visit(value.sdmb);
}

void BinaryParseVisitor::visit(AnimationGroup& value, u32 version) {
    (void)version;
    visit(value.name);
    visit(value.subtrackIndices);
}

void BinaryParseVisitor::visit(AnimationState& value, u32 version) {
    (void)version;
    visit(value.animIds);
    value.unknown = reader.readArray<u8, 16>();
}

void BinaryParseVisitor::visit(BoneAnimationSet& value, u32 version) {
    (void)version;
    value.flags = reader.read<Flag>();
    value.animationSequenceIndex = reader.read<u16>();
    value.fallbackSequenceIndex = reader.read<u16>();
    visit(value.name);
    visit(value.splitItems);
}

void BinaryParseVisitor::visit(Bone& value, u32 version) {
    (void)version;
    value.unknown = reader.read<u32>();
    visit(value.name);
    value.flags = reader.read<BoneFlag>();
    value.parentIndex = reader.read<u16>();
    value.padding = reader.read<u16>();
    value.position = reader.read<AnimRef<Vector3f>>();
    value.rotation = reader.read<AnimRef<Quaternion>>();
    value.scale = reader.read<AnimRef<Vector3f>>();
    value.visibility = reader.read<AnimRef<u32>>();
}

void BinaryParseVisitor::visit(InitialReference& value, u32 version) {
    (void)version;
    value.matrix = reader.read<Matrix44f>();
}

void BinaryParseVisitor::visit(AttachmentPoint& value, u32 version) {
    (void)version;
    value.unknown = reader.read<u32>();
    visit(value.name);
    value.boneIndex = reader.read<u32>();
}
