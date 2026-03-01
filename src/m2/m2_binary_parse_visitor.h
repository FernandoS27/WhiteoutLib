// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include "../common/binary_reader.h"
#include "../include/m2/types.h"
#include "../include/m2/structures.h"

#include <type_traits>

namespace whiteout {
namespace m2 {

class M2BinaryParseVisitor {
public:
    explicit M2BinaryParseVisitor(common::BinaryReader& reader, i32 maxSize_ = -1);

    template<typename T>
    void read(T& header) {
        start();
        visit(header);
    }

    template<typename T>
    void read(T& header, M2BaseFile& file) {
        start();
        visit(header, file);
    }
    
protected:
    void start();

    void visit(GlobalFlags& flags);
    void visit(GlobalSequence& seq);
    void visit(Sequence& seq);
    void visit(Vertex& vertex);
    void visit(Bone& bone);
    void visit(Texture& texture);
    void visit(Material& material);
    void visit(TextureWeight& weight);
    void visit(TextureTransform& transform);
    void visit(ColorAnimation& color);
    void visit(Light& light);
    void visit(CameraSpline& spline);
    void visit(Camera& camera);
    void visit(Attachment& attachment);
    void visit(RibbonEmitter& emitter);
    void visit(ParticleEmitter& emitter);
    void visit(Event& event);
    void visit(MD20Header& header);

    // Chunk structure visit methods
    void visit(M2TXACChunk& chunk, M2BaseFile& file);

    // File reference chunks
    void visit(M2PFIDChunk& chunk, M2BaseFile& file);
    void visit(M2SFIDChunk& chunk, M2BaseFile& file);
    void visit(M2AFIDEntry& entry);
    void visit(M2AFIDChunk& chunk);
    void visit(M2BFIDChunk& chunk);

    void visit(M2EXPTEntry& entry);
    void visit(M2EXPTChunk& chunk);
    void visit(M2EXP2Particle& particle);
    void visit(M2EXP2Chunk& chunk);
    void visit(M2PABCChunk& chunk);
    void visit(M2PADCChunk& chunk);
    void visit(M2SequenceBounds& bounds);
    void visit(M2PSBCChunk& chunk);
    void visit(M2PEDCChunk& chunk);
    void visit(M2SKIDChunk& chunk);
    void visit(M2TXIDEntry& entry);
    void visit(M2TXIDChunk& chunk);
    void visit(M2LDV1Chunk& chunk);
    void visit(M2RPIDEntry& entry);
    void visit(M2RPIDChunk& chunk);
    void visit(M2GPIDEntry& entry);
    void visit(M2GPIDChunk& chunk);
    void visit(M2WFV1Chunk& chunk);
    void visit(M2WFV2Chunk& chunk);
    void visit(M2PGD1Entry& entry);
    void visit(M2PGD1Chunk& chunk);
    void visit(M2WFV3Data& data);
    void visit(M2WFV3Chunk& chunk);
    void visit(M2PFDCChunk& chunk);
    void visit(M2EDGFEntry& entry);
    void visit(M2EDGFChunk& chunk);
    void visit(M2NERFEntry& entry);
    void visit(M2NERFChunk& chunk);
    void visit(M2DETLEntry& entry);
    void visit(M2DETLChunk& chunk);
    void visit(M2DBOCEntry& entry);
    void visit(M2DBOCChunk& chunk);
    void visit(M2AFRAChunk& chunk);
    void visit(M2PCOLChunk& chunk);
    void visit(M2DPIVChunk& chunk);
    void visit(M2TEXLEntry& entry);
    void visit(M2TEXLChunk& chunk);

    // Physics structure visit methods
    void visit(PHYSHeader& header);
    void visit(PHYTEntry& entry);
    void visit(BODYEntry& entry);
    void visit(BDY2Entry& entry);
    void visit(BDY3Entry& entry);
    void visit(BDY4Entry& entry);
    void visit(SHAPEntry& entry);
    void visit(SHP2Entry& entry);
    void visit(BOXSEntry& entry);
    void visit(CAPSEntry& entry);
    void visit(SPHSEntry& entry);
    void visit(PLYTNode& node);
    void visit(PLYTData& data, const PLYTHeader& header);
    void visit(PLYTEntry& entry);
    void visit(JOINEntry& entry);
    void visit(Matrix3x4& matrix);
    void visit(WELJEntry& entry);
    void visit(WLJ2Entry& entry);
    void visit(WLJ3Entry& entry);
    void visit(SPHJEntry& entry);
    void visit(SHOJEntry& entry);
    void visit(SHJ2Entry& entry);
    void visit(PRSJEntry& entry);
    void visit(PRS2Entry& entry);
    void visit(REVJEntry& entry);
    void visit(REV2Entry& entry);
    void visit(DSTJEntry& entry);
    void visit(PHYVEntry& entry);

    // Bone structure visit methods
    void visit(BONEHeader& header);
    void visit(Matrix4x4& matrix);
    void visit(BIDAChunk& chunk);
    void visit(BOMTChunk& chunk);

    // Anim structure visit methods
    void visit(AFM2Chunk& chunk);
    void visit(AFSAChunk& chunk);
    void visit(AFSBChunk& chunk);
    void visit(M2AnimFile& file);

    // Skeleton structure visit methods
    void visit(M2SKL1Chunk& chunk);
    void visit(M2SKA1Chunk& chunk);
    void visit(M2SKB1Chunk& chunk);
    void visit(M2SKS1Chunk& chunk);
    void visit(M2SKPDChunk& chunk);

    // Skin structure visit methods
    void visit(M2SkinSection& section);
    void visit(M2Batch& batch);
    void visit(M2ShadowBatch& batch);
    void visit(M2SkinProfile& profile);
    void visit(M2SkinFile& file);

    template<typename T>
    void visit(std::vector<T>& array);

    template<typename T>
    void parse_chunked_vector(std::vector<T>& array);

    void visit(std::string& str);

    template<typename T>
    void visit(AnimationTrack<T>& track);
    void visit(AnimationTrackBase& track);

    u32 version = 0;
    common::BinaryReader& reader;
    u32 baseOffset = 0;
    i32 maxSize = -1;
};

template<typename T>
void M2BinaryParseVisitor::visit(std::vector<T>& array) {
    const auto count = reader.read<u32>();
    const auto offset = reader.read<u32>();
    if (count == 0) {
        array.clear();
        return;
    }

    const auto currentPos = reader.getPosition();
    reader.setPosition(offset + baseOffset);

    if constexpr (std::is_trivially_copyable_v<T>) {
        array = reader.read<std::vector<T>>(count);
    } else {
        array.clear();
        array.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            T element;
            this->visit(element);
            array.push_back(std::move(element));
        }
    }

    reader.setPosition(currentPos);
}

template<typename T>
void M2BinaryParseVisitor::parse_chunked_vector(std::vector<T>& array) {
    if (maxSize <= 0) {
        array.clear();
        return;
    }
    const size_t totalSize = maxSize / sizeof(T);
    array = reader.read<std::vector<T>>(totalSize);
}

template<typename T>
void M2BinaryParseVisitor::visit(AnimationTrack<T>& track) {
    track.interpolationType = reader.read<u16>();
    track.globalSequenceId = reader.read<u16>();
    visit(track.timestamps);
    visit(track.values);
}

} // namespace m2
} // namespace whiteout
