// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

BinaryWriterVisitor::BinaryWriterVisitor(BinaryWriter& writer, bool ismd33)
    : writer(writer), ismd33(ismd33) {}

void BinaryWriterVisitor::write(const Model& model) {
    MD3Header header{};

    header.indexOffset = 0; // Placeholder, will be updated later
    header.indexCount = 0;  // Placeholder, will be updated later

    if (!ismd33) { // "MD34" or "MD33"
        writeReferenceFunc = [this](Reference& ref) { this->writer.write(ref); };
        header.magic = TAG_MD34;
    } else {
        writeReferenceFunc = [this](Reference& ref) {
            writer.write(ref.entries);
            writer.write(ref.index);
        };
        header.magic = TAG_MD33;
    }

    const auto write_header = [this, &header]() {
        writer.setPosition(0);
        writer.write(header.magic);
        writer.write(header.indexOffset);
        writer.write(header.indexCount);
        writeReferenceFunc(header.modelRef);
        writer.AlignTo(16, 0xAA);
    };

    indexTable.reserve(
        1024); // Avoid too many reallocations, as we will be adding entries one by one

    indexTable.emplace_back(header.magic, 0, 1, 0); // Placeholder for header reference entry
    auto modelRefIndex = indexTable.size();
    indexTable.emplace_back(ChunkTagTraits<Model>::value, 0, 1,
                            detail::getStructureVersion(model)); // Placeholder for model reference entry
    header.modelRef.entries = 1;
    header.modelRef.index = static_cast<u32>(modelRefIndex);

    write_header();
    deferredWrites.push_back([this, &model, modelRefIndex]() {
        const u32 modelRootOffset = writer.getPosition();
        indexTable[modelRefIndex].offset = modelRootOffset;
        visit(model, indexTable[modelRefIndex].version);
        writer.AlignTo(16, 0xAA);
        transferDeferredWrites(); // Use pre-prder only at root
    });

    while (!deferredWrites.empty()) {
        auto writeFunc = std::move(deferredWrites.front());
        deferredWrites.pop_front();
        writeFunc();
    }

    const u32 currentOffset = writer.getPosition();
    writer.setPosition(0);
    header.indexOffset = currentOffset;
    header.indexCount = static_cast<u32>(indexTable.size());
    write_header();
    writer.setPosition(currentOffset);
    writer.write(indexTable);
}

void BinaryWriterVisitor::visit(const Model& model, u32 version) {
    visit(model.name);
    writer.write(model.flags);
    visit(model.sequences);
    visit(model.subTrackCollections);
    visit(model.animationGroups);
    visit(model.boneAnimationSets);
    writer.write(model.animationSplitCount);
    visit(model.animationStates);
    visit(model.bones);
    writer.write(model.skinBoneCount);
    visit(model.vertices, 0);
    visit(model.divisions);
    visit(model.boneLookup);
    writer.write(model.bounds);
    writer.write(model.collisionBounds);
    visit(model.collisionFaces);
    visit(model.collisionVerts);
    visit(model.collisionNormals);

    visit(model.attachmentPoints);
    visit(model.attachmentPointAddons);
    visit(model.lights);
    if (version >= 21) {
        visit(model.shadowBoxes);
    }
    visit(model.cameras);
    visit(model.camerasAddons);

    visit(model.materialMaps);
    visit(model.standardMaterials);
    visit(model.displacementMaterials);
    visit(model.compositeMaterials);
    visit(model.terrainMaterials);
    visit(model.volumeMaterials);
    visit(model.hairMaterials);
    visit(model.creepMaterials);

    if (version >= 25) {
        visit(model.volumeNoiseMaterials);
    }
    if (version >= 26) {
        visit(model.stbMaterials);
    }
    if (version >= 28) {
        visit(model.reflectionMaterials);
    }
    if (version >= 29) {
        visit(model.lensFlareMaterials);
    }
    if (version >= 30) {
        visit(model.materialAddData);
    }

    visit(model.particleEmitters);
    visit(model.particleEmitterCopies);
    visit(model.ribbonEmitters);
    visit(model.projections);
    visit(model.forces);
    visit(model.warps);
    visit(model.viewVolumes);

    visit(model.rigidBodies);
    visit(model.physicsConstraints);
    visit(model.physicsJoints);
    if (version >= 28) {
        visit(model.clothPhysics);
    }
    visit(model.ikTwoJoints);
    if (version >= 24) {
        visit(model.ikCCD);
    }
    visit(model.ikJoints);
    visit(model.oneBoneSolvers);

    visit(model.turretBehaviors);
    visit(model.triggerData);
    visit(model.initialReference);

    visit(model.tightHitTestObject, 1);
    visit(model.fuzzyHitTestObjects);
    visit(model.attachmentVolumes);
    if (version >= 23) {
        visit(model.attachmentVolumesAddon0);
        visit(model.attachmentVolumesAddon1);
    }
    visit(model.billboardBehaviors);

    if (version >= 21) {
        visit(model.trailingModels);
        writer.write(model.m3aAnimHash);
    }
    if (version >= 23) {
        visit(model.m3aAnimHashes);
    }
}

void BinaryWriterVisitor::visit(const std::string& str, u32 version) {
    (void)version;
    visit(str);
}

void BinaryWriterVisitor::visit(const std::string& str) {
    Reference ref = {};
    if (str.empty()) {
        writer.write(ref);
        return;
    }

    auto ref_position = writer.getPosition();
    writer.write(ref);
    currentLevelWrites.push_back([this, ref_position, &str]() {
        auto new_entry_index = indexTable.size();
        auto version = detail::getStructureVersion(str);
        const auto currentOffset = writer.getPosition();
        indexTable.emplace_back(ChunkTagTraits<char>::value, currentOffset,
                                static_cast<u32>(str.size()), version);
        auto& entry = indexTable[new_entry_index];
        writer.setPosition(ref_position);
        Reference ref = {};
        ref.entries = static_cast<u32>(str.size());
        ref.index = static_cast<u32>(new_entry_index);
        writer.write(ref);
        writer.setPosition(entry.offset);
        writer.writeString(str);
        writer.AlignTo(16, 0xAA);
    });
}

void BinaryWriterVisitor::transferDeferredWrites(bool pre_order) {
    // Insert children at the FRONT of deferredWrites (before remaining siblings)
    // to achieve DFS pre-order. Reverse-iterate so first child ends up at front.
    while (!currentLevelWrites.empty()) {
        if (!pre_order) {
            deferredWrites.push_back(std::move(currentLevelWrites.back()));
        } else {
            deferredWrites.push_front(std::move(currentLevelWrites.back()));
        }
        currentLevelWrites.pop_back();
    }
}
