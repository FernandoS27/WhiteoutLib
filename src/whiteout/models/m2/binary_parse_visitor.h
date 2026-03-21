// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include <whiteout/models/m2/structures.h>
#include <whiteout/models/m2/types.h>
#include "../../common/binary_reader.h"
#include "../../common/streams.h"


#include <type_traits>

namespace whiteout {
namespace m2 {

class WoWFileSystem;

class BinaryParseVisitor {
public:
    explicit BinaryParseVisitor(common::BinaryReader& reader, WoWFileSystem* wfs = nullptr, i32 maxSize_ = -1);

    template <typename T>
    void read(T& header) {
        start();
        visit(header);
    }

    template <typename T>
    void read(T& header, BaseFile& file) {
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
    void visit(Model& header);
    void visit(MD20Header& header);

    // Chunk structure visit methods
    void visit(TXACChunk& chunk, BaseFile& file);

    // File reference chunks
    void visit(PFIDChunk& chunk, BaseFile& file);
    void visit(SFIDChunk& chunk, BaseFile& file);
    void visit(AFIDEntry& entry);
    void visit(AFIDChunk& chunk);
    void visit(BFIDChunk& chunk);

    void visit(EXPTEntry& entry);
    void visit(EXPTChunk& chunk);
    void visit(ParticleEmitterExtension& particle);
    void visit(EXP2Chunk& chunk);
    void visit(PABCChunk& chunk);
    void visit(PADCChunk& chunk);
    void visit(SequenceBounds& bounds);
    void visit(PSBCChunk& chunk);
    void visit(PEDCChunk& chunk);
    void visit(SKIDChunk& chunk);
    void visit(TXIDEntry& entry);
    void visit(TXIDChunk& chunk);
    void visit(LDV1Chunk& chunk);
    void visit(M2RPIDEntry& entry);
    void visit(M2RPIDChunk& chunk);
    void visit(GPIDEntry& entry);
    void visit(GPIDChunk& chunk);
    void visit(WFV1Chunk& chunk);
    void visit(WFV2Chunk& chunk);
    void visit(PGD1Entry& entry);
    void visit(PGD1Chunk& chunk);
    void visit(WFV3Data& data);
    void visit(WFV3Chunk& chunk);
    void visit(PFDCChunk& chunk);
    void visit(EDGFEntry& entry);
    void visit(EDGFChunk& chunk);
    void visit(NERFEntry& entry);
    void visit(NERFChunk& chunk);
    void visit(DETLEntry& entry);
    void visit(DETLChunk& chunk);
    void visit(DBOCEntry& entry);
    void visit(DBOCChunk& chunk);
    void visit(AFRAChunk& chunk);
    void visit(PCOLChunk& chunk);
    void visit(DPIVChunk& chunk);
    void visit(TEXLEntry& entry);
    void visit(TEXLChunk& chunk);

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
    void visit(Matrix44f& matrix);
    void visit(BIDAChunk& chunk);
    void visit(BOMTChunk& chunk);

    // Anim structure visit methods
    void visit(AFM2Chunk& chunk);
    void visit(AFSAChunk& chunk);
    void visit(AFSBChunk& chunk);
    void visit(AnimFile& file);

    // Skeleton structure visit methods
    void visit(SKL1Chunk& chunk);
    void visit(SKA1Chunk& chunk);
    void visit(SKB1Chunk& chunk);
    void visit(SKS1Chunk& chunk);
    void visit(SKPDChunk& chunk);

    // Skin structure visit methods
    void visit(SkinSection& section);
    void visit(Batch& batch);
    void visit(ShadowBatch& batch);
    void visit(SkinProfile& profile);
    void visit(SkinFile& file);

    template <typename T>
    void visit(std::vector<T>& array);

    template <typename T>
    void parse_chunked_vector(std::vector<T>& array);

    void visit(std::string& str);

    template <typename T>
    void visit(AnimationTrack<T>& track);
    void visit(AnimationTrackBase& track);

    template <typename T>
    void visit(AnimationTrackSimple<T>& track);

    template <typename T>
    void readAnimationVector(std::vector<std::vector<T>>& keys);

    u32 version = 0;
    GlobalFlag globalFlags = GlobalFlag::None;
    common::BinaryReader& reader;
    WoWFileSystem* wfs = nullptr;
    u32 baseOffset = 0;
    i32 maxSize = -1;
    std::vector<std::span<u8>> animBuffers; // for deferred loading of AFID animation data
};

template <typename T>
void BinaryParseVisitor::visit(std::vector<T>& array) {
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

template <typename T>
void BinaryParseVisitor::parse_chunked_vector(std::vector<T>& array) {
    if (maxSize <= 0) {
        array.clear();
        return;
    }
    const size_t totalSize = maxSize / sizeof(T);
    array = reader.read<std::vector<T>>(totalSize);
}

template <typename T>
void BinaryParseVisitor::readAnimationVector(std::vector<std::vector<T>>& keys) {
    struct Reference {
        u32 count;
        u32 offset;
    };
    Reference ref = reader.read<Reference>();
    if (ref.count == 0) {
        keys.clear();
        return;
    }

    const auto currentPos = reader.getPosition();
    reader.setPosition(ref.offset + baseOffset);
    keys.resize(ref.count);
    for (size_t i = 0; i < ref.count; ++i) {
        const auto ref = reader.read<Reference>();
        auto& buffer = animBuffers[i];
        if (buffer.empty()) {
            const auto savePos = reader.getPosition();
            reader.setPosition(ref.offset + baseOffset);
            keys[i] = reader.read<std::vector<T>>(ref.count);
            reader.setPosition(savePos);
            continue;
        }
        common::span_streambuf sbuf(buffer);
        common::BinaryReader animReader(sbuf);
        animReader.setPosition(ref.offset);
        keys[i] = animReader.read<std::vector<T>>(ref.count);
    }
    reader.setPosition(currentPos);
}

template <typename T>
void BinaryParseVisitor::visit(AnimationTrack<T>& track) {
    track.interpolationType = static_cast<InterpolationType>(reader.read<u16>());
    track.globalSequenceId = reader.read<u16>();

    visit(track.timestamps);
    visit(track.values);
}

template <typename T>
void BinaryParseVisitor::visit(AnimationTrackSimple<T>& track) {
    visit(track.timestamps);
    visit(track.values);
}

} // namespace m2
} // namespace whiteout
