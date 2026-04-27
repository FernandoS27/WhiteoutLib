// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

void BinaryWriterVisitor::visit(const VertexBuffer& value, u32 /*version*/) {
    writer.write(value.flags);
    visit(value.data);
}

void BinaryWriterVisitor::visit(const Region& region, u32 version) {
    writer.write(region.index);
    if (version >= 3) {
        writer.write(region.unknown);
        writer.write(region.firstVertex);
        writer.write(region.vertexCount);
        writer.write(region.firstIndex);
        writer.write(region.indexCount);
    } else {
        writer.write(static_cast<u16>(region.firstVertex));
        writer.write(static_cast<u16>(region.vertexCount));
    }
    writer.write(region.unknown2);
    writer.write(region.firstBoneLookup);
    writer.write(region.boneLookupCount);
    writer.write(region.padding);
    writer.write(region.boneWeightPairs);
    writer.write(region.boneIndexPairs);
    writer.write(region.rootBone);
    if (version >= 4) {
        writer.write(region.flags);
    }
    if (version >= 5) {
        writer.write(region.uvScale);
        writer.write(region.uvOffset);
    }
}

void BinaryWriterVisitor::visit(const Batch& batch, u32 version) {
    (void)version;
    writer.write(batch.unknown);
    writer.write(batch.regionIndex);
    writer.write(batch.unknown2);
    writer.write(batch.materialIndex);
    writer.write(batch.boneCount);
}

void BinaryWriterVisitor::visit(const MeshSection& section, u32 version) {
    (void)version;
    writer.write(section.nodeIndex);
    writer.write(section.bounds);
}

void BinaryWriterVisitor::visit(const MeshDivision& division, u32 version) {
    (void)version;
    visit(division.faces);
    visit(division.regions);
    visit(division.batches);
    visit(division.msec);
    writer.write(division.instances);
}
