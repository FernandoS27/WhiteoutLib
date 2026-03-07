// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include <whiteout/m2/structures.h>
#include <whiteout/m2/types.h>
#include "../common/binary_writer.h"

#include <deque>
#include <functional>
#include <type_traits>

namespace whiteout {
namespace m2 {

class BinaryWriterVisitor {
public:
    explicit BinaryWriterVisitor(common::BinaryWriter& writer);

    template <typename T>
    void write(const T& header) {
        start();
        deferredWrites.clear();
        deferredWrites.push_back([this, &header]() { visit(header); });
        while (!deferredWrites.empty()) {
            auto writeFunc = std::move(deferredWrites.front());
            deferredWrites.pop_front();
            writeFunc();
        }
    }

protected:
    void start();

    void visit(const GlobalFlags& flags);
    void visit(const GlobalSequence& seq);
    void visit(const Sequence& seq);
    void visit(const Vertex& vertex);
    void visit(const Bone& bone);
    void visit(const Texture& texture);
    void visit(const Material& material);
    void visit(const TextureWeight& weight);
    void visit(const TextureTransform& transform);
    void visit(const ColorAnimation& color);
    void visit(const Light& light);
    void visit(const CameraSpline& spline);
    void visit(const Camera& camera);
    void visit(const Attachment& attachment);
    void visit(const RibbonEmitter& emitter);
    void visit(const ParticleEmitter& emitter);
    void visit(const Event& event);
    void visit(const MD20Header& header);

    // Chunk structure visit methods
    void visit(const TXACChunk& chunk);
    void visit(const PFIDChunk& chunk);
    void visit(const SFIDChunk& chunk);
    void visit(const AFIDEntry& entry);
    void visit(const AFIDChunk& chunk);
    void visit(const BFIDChunk& chunk);
    void visit(const EXPTEntry& entry);
    void visit(const EXPTChunk& chunk);
    void visit(const EXP2Particle& particle);
    void visit(const EXP2Chunk& chunk);
    void visit(const PABCChunk& chunk);
    void visit(const PADCChunk& chunk);
    void visit(const SequenceBounds& bounds);
    void visit(const PSBCChunk& chunk);
    void visit(const PEDCChunk& chunk);
    void visit(const SKIDChunk& chunk);
    void visit(const TXIDEntry& entry);
    void visit(const TXIDChunk& chunk);
    void visit(const LDV1Chunk& chunk);
    void visit(const M2RPIDEntry& entry);
    void visit(const M2RPIDChunk& chunk);
    void visit(const GPIDEntry& entry);
    void visit(const GPIDChunk& chunk);
    void visit(const WFV1Chunk& chunk);
    void visit(const WFV2Chunk& chunk);
    void visit(const PGD1Entry& entry);
    void visit(const PGD1Chunk& chunk);
    void visit(const WFV3Data& data);
    void visit(const WFV3Chunk& chunk);
    void visit(const PFDCChunk& chunk);
    void visit(const EDGFEntry& entry);
    void visit(const EDGFChunk& chunk);
    void visit(const NERFEntry& entry);
    void visit(const NERFChunk& chunk);
    void visit(const DETLEntry& entry);
    void visit(const DETLChunk& chunk);
    void visit(const DBOCEntry& entry);
    void visit(const DBOCChunk& chunk);
    void visit(const AFRAChunk& chunk);
    void visit(const PCOLChunk& chunk);
    void visit(const DPIVChunk& chunk);
    void visit(const TEXLEntry& entry);
    void visit(const TEXLChunk& chunk);

    // Physics structure visit methods
    void visit(const PHYSHeader& header);
    void visit(const PHYTEntry& entry);
    void visit(const BODYEntry& entry);
    void visit(const BDY2Entry& entry);
    void visit(const BDY3Entry& entry);
    void visit(const BDY4Entry& entry);
    void visit(const SHAPEntry& entry);
    void visit(const SHP2Entry& entry);
    void visit(const BOXSEntry& entry);
    void visit(const CAPSEntry& entry);
    void visit(const SPHSEntry& entry);
    void visit(const PLYTNode& node);
    void visit(const PLYTData& data, const PLYTHeader& header);
    void visit(const PLYTEntry& entry);
    void visit(const JOINEntry& entry);
    void visit(const Matrix3x4& matrix);
    void visit(const WELJEntry& entry);
    void visit(const WLJ2Entry& entry);
    void visit(const WLJ3Entry& entry);
    void visit(const SPHJEntry& entry);
    void visit(const SHOJEntry& entry);
    void visit(const SHJ2Entry& entry);
    void visit(const PRSJEntry& entry);
    void visit(const PRS2Entry& entry);
    void visit(const REVJEntry& entry);
    void visit(const REV2Entry& entry);
    void visit(const DSTJEntry& entry);
    void visit(const PHYVEntry& entry);

    // Bone structure visit methods
    void visit(const BONEHeader& header);
    void visit(const Matrix44f& matrix);
    void visit(const BIDAChunk& chunk);
    void visit(const BOMTChunk& chunk);

    // Anim structure visit methods
    void visit(const AFM2Chunk& chunk);
    void visit(const AFSAChunk& chunk);
    void visit(const AFSBChunk& chunk);

    // Skeleton structure visit methods
    void visit(const SKL1Chunk& chunk);
    void visit(const SKA1Chunk& chunk);
    void visit(const SKB1Chunk& chunk);
    void visit(const SKS1Chunk& chunk);
    void visit(const SKPDChunk& chunk);

    // Skin structure visit methods
    void visit(const SkinSection& section);
    void visit(const Batch& batch);
    void visit(const ShadowBatch& batch);
    void visit(const SkinProfile& profile);
    void visit(const SkinFile& file);

    void visit(const AnimationTrackBase& track);

    template <typename T>
    void visit(const lazy_vector<T>& lv);

    template <typename T>
    void visit(const AnimationTrack<T>& track);

    template <typename T>
    void visit(const std::vector<T>& array);

    void visit(const std::string& str);

    common::BinaryWriter& writer;
    uint32_t version = 0;
    std::deque<std::function<void()>> deferredWrites; // offset, write function
    u32 baseOffset = 0;
};

template <typename T>
void BinaryWriterVisitor::visit(const lazy_vector<T>& lv) {
    const auto count = static_cast<u32>(lv.backingData.size());
    writer.write<u32>(count);
    u32 offsetPos = writer.getPosition();
    writer.write<u32>(0);
    if (count == 0)
        return;

    deferredWrites.push_back([this, offsetPos, &lv]() {
        u32 currentPos = writer.getPosition();
        writer.setPosition(offsetPos);
        writer.write(currentPos - baseOffset);
        writer.setPosition(currentPos);

        for (const auto& subArray : lv.backingData) {
            visit(subArray);
        }

        writer.AlignTo(16);
    });
}

template <typename T>
void BinaryWriterVisitor::visit(const AnimationTrack<T>& track) {
    writer.write(track.interpolationType);
    writer.write(track.globalSequenceId);
    visit(track.timestamps);
    visit(track.values);
}

template <typename T>
void BinaryWriterVisitor::visit(const std::vector<T>& array) {
    writer.template write<u32>(static_cast<u32>(array.size()));
    u32 offsetPos = writer.getPosition();
    writer.template write<u32>(0);
    if (array.empty()) {
        return;
    }

    deferredWrites.push_back([this, offsetPos, &array]() {
        u32 currentPos = writer.getPosition();
        writer.setPosition(offsetPos);
        writer.write(currentPos - baseOffset);
        writer.setPosition(currentPos);

        if constexpr (std::is_trivially_copyable_v<T>) {
            writer.write(array);
        } else {
            for (const T& item : array) {
                visit(item);
            }
        }

        writer.AlignTo(16);
    });
}

} // namespace m2
} // namespace whiteout
