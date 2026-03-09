// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

BinaryParseVisitor::BinaryParseVisitor(BinaryReader& reader, bool ismd33) : reader(reader) {
    (void)ismd33;
}

void BinaryParseVisitor::read(Model& model) {
    MD3Header header = reader.read<MD3Header>();

    if (header.magic == TAG_MD34) { // "MD34" or "MD33"
        readReferenceFunc = [this]() {
            Reference ref = this->reader.read<Reference>();
            return ref;
        };
    } else {
        readReferenceFunc = [this]() {
            Reference ref;
            ref.entries = this->reader.read<u32>();
            ref.index = this->reader.read<u32>();
            // MD33 doesn't have flags
            return ref;
        };
    }

    reader.setPosition(header.indexOffset);
    indexTable = reader.read<std::vector<IndexEntry>>(header.indexCount);
    indexUsed.resize(header.indexCount, false);

    reader.setPosition(indexTable[header.modelRef.index].offset);
    indexUsed[0] = true; // Mark the MD34 header as used
    indexUsed[header.modelRef.index] = true;
    visit(model, indexTable[header.modelRef.index].version);
    for (size_t i = 0; i < indexUsed.size(); ++i) {
        if (!indexUsed[i]) {
            issues.emplace_back("Index entry " + std::to_string(i) +
                                " was not used during parsing.");
            issues.emplace_back("  Tag: " + tagToString(indexTable[i].tag) +
                                ", Count: " + std::to_string(indexTable[i].count) +
                                ", Version: " + std::to_string(indexTable[i].version));
        }
    }
}

void BinaryParseVisitor::visit(Model& model, u32 version) {
    detail::setStructureVersion(model, version);
    visit(model.name);
    model.flags = static_cast<ModelFlag>(reader.read<u32>());
    visit(model.sequences);
    visit(model.subTrackCollections);
    visit(model.animationGroups);
    visit(model.boneAnimationSets);
    model.animationSplitCount = reader.read<u32>();
    visit(model.animationStates);
    visit(model.bones);
    model.skinBoneCount = reader.read<u32>();
    visit(model.vertices, 0);
    visit(model.divisions);
    visit(model.boneLookup);
    model.bounds = reader.read<Extent>();
    model.collisionBounds = reader.read<Extent>();
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
        model.m3aAnimHash = reader.read<u32>();
    }
    if (version >= 23) {
        visit(model.m3aAnimHashes);
    }
}

void BinaryParseVisitor::visit(std::string& str) {
    Reference ref = readReferenceFunc();
    if (ref.entries == 0) {
        str.clear();
        return;
    }

    const auto currentPos = reader.getPosition();
    indexUsed[ref.index] = true;
    reader.setPosition(indexTable[ref.index].offset);
    assert(indexTable[ref.index].tag == ChunkTagTraits<char>::value);
    str = reader.readString(ref.entries, false);
    reader.setPosition(currentPos);
}

void BinaryParseVisitor::visit(std::string& str, u32 version) {
    (void)version;
    visit(str);
}
