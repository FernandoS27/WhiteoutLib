// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

void BinaryParseVisitor::visit(VertexBuffer& value, u32 version) {
    (void)version;
    value.flags = reader.read<VertexFormatFlag>();
    visit(value.data);
    value.initialize();
}

void BinaryParseVisitor::visit(Region& value, u32 version) {
    value.index = reader.read<u32>();
    if (version >= 3) {
        value.unknown = reader.read<u32>();
        value.firstVertex = reader.read<u32>();
        value.vertexCount = reader.read<u32>();
        value.firstIndex = reader.read<u32>();
        value.indexCount = reader.read<u32>();
    } else {
        value.firstVertex = reader.read<u16>();
        value.vertexCount = reader.read<u16>();
        value.firstIndex = reader.read<u32>();
        value.indexCount = reader.read<u32>();
    }
    value.unknown2 = reader.read<u16>();
    value.firstBoneLookup = reader.read<u16>();
    value.boneLookupCount = reader.read<u16>();
    value.padding = reader.read<u16>();
    value.boneWeightPairs = reader.read<u8>();
    value.boneIndexPairs = reader.read<u8>();
    value.rootBone = reader.read<u16>();
    if (version >= 4) {
        value.flags = reader.read<RegionFlag>();
    }
    if (version >= 5) {
        value.uvScale = reader.read<f32>();
        value.uvOffset = reader.read<f32>();
    }
}

void BinaryParseVisitor::visit(Batch& value, u32 version) {
    (void)version;
    value.unknown = reader.read<u32>();
    value.regionIndex = reader.read<u16>();
    value.unknown2 = reader.read<u32>();
    value.materialIndex = reader.read<u16>();
    value.boneCount = reader.read<u16>();
}

void BinaryParseVisitor::visit(MeshSection& value, u32 version) {
    (void)version;
    value.nodeIndex = reader.read<u32>();
    value.bounds = reader.read<AnimRef<Extent>>();
}

void BinaryParseVisitor::visit(MeshDivision& value, u32 version) {
    (void)version;
    visit(value.faces);
    visit(value.regions);
    visit(value.batches);
    visit(value.msec);
    value.instances = reader.read<u32>();

    // A REGN v2 (beta) face names a vertex of the whole buffer, from v3 one of
    // its region's slice (2,750 of 2,750 beta regions past vertex 0). Rebased
    // here so every reader sees the v3 form; the writer adds it back.
    for (const Region& region : value.regions) {
        if (region.getVersion() >= 3)
            continue;
        const size_t end = std::min<size_t>(size_t{region.firstIndex} + region.indexCount, value.faces.size());
        for (size_t i = region.firstIndex; i < end; ++i)
            value.faces[i] = static_cast<u16>(value.faces[i] - region.firstVertex);
    }
}

