// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

void BinaryWriterVisitor::visit(const Sequence& seq, u32 version) {
    writer.write(seq.id);
    writer.write(seq.index);
    visit(seq.name);
    writer.write(seq.startFrame);
    writer.write(seq.endFrame);
    writer.write(seq.moveSpeed);
    writer.write(seq.flags);
    writer.write(seq.frequency);
    writer.write(seq.replayStart);
    writer.write(seq.replayEnd);
    writer.write(seq.blendTime);
    if (version <= 1) {
        writer.write(seq.deprecated.unknown);
    }
    writer.write(seq.bounds);
    visit(seq.animationSets);
}

void BinaryWriterVisitor::visit(const SubTrackContainer& container, u32 version) {
    (void)version;
    visit(container.name);
    writer.write(container.runsConcurrent);
    writer.write(container.animPriority);
    writer.write(container.animationStateIndex);
    writer.write(container.padding);
    visit(container.animIds);
    visit(container.animRefs);
    writer.write(container.unknown);
    visit(container.sdev);
    visit(container.sd2v);
    visit(container.sd3v);
    visit(container.sd4q);
    visit(container.sdcc);
    visit(container.sdr3);
    visit(container.sdu8);
    visit(container.sds6);
    visit(container.sdu6);
    visit(container.sds3);
    visit(container.sdu3);
    visit(container.sdfg);
    visit(container.sdmb);
}

void BinaryWriterVisitor::visit(const AnimationGroup& group, u32 version) {
    (void)version;
    visit(group.name);
    visit(group.subtrackIndices);
}

void BinaryWriterVisitor::visit(const AnimationState& state, u32 version) {
    (void)version;
    visit(state.animIds);
    writer.write(state.unknown);
}

void BinaryWriterVisitor::visit(const BoneAnimationSet& set, u32 version) {
    (void)version;
    writer.write(set.flags);
    writer.write(set.animationSequenceIndex);
    writer.write(set.fallbackSequenceIndex);
    visit(set.name);
    visit(set.splitItems);
}

void BinaryWriterVisitor::visit(const Bone& bone, u32 version) {
    (void)version;
    writer.write(bone.unknown);
    visit(bone.name);
    writer.write(bone.flags);
    writer.write(bone.parentIndex);
    writer.write(bone.padding);
    writer.write(bone.position);
    writer.write(bone.rotation);
    writer.write(bone.scale);
    writer.write(bone.visibility);
}

void BinaryWriterVisitor::visit(const InitialReference& ref, u32 version) {
    (void)version;
    writer.write(ref.matrix);
}

void BinaryWriterVisitor::visit(const AttachmentPoint& point, u32 version) {
    (void)version;
    writer.write(point.unknown);
    visit(point.name);
    writer.write(point.boneIndex);
}
