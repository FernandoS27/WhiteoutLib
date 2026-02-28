#pragma once

#include "../include/m2/types.h"
#include "../include/m2/structures.h"

#include <deque>
#include <functional>

namespace common {
    class BinaryWriter;
}

namespace m2 {

class M2BinaryWriterVisitor {
public:
    explicit M2BinaryWriterVisitor(common::BinaryWriter& writer);
    
    template<typename T>
    void write(const T& header) {
        start();
        deferredWrites.clear();
        deferredWrites.push_back([this, &header]() {
            visit(header);
        });
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
    void visit(const M2TXACChunk& chunk);
    void visit(const M2PFIDChunk& chunk);
    void visit(const M2SFIDChunk& chunk);
    void visit(const M2AFIDEntry& entry);
    void visit(const M2AFIDChunk& chunk);
    void visit(const M2BFIDChunk& chunk);
    void visit(const M2EXPTEntry& entry);
    void visit(const M2EXPTChunk& chunk);
    void visit(const M2EXP2Particle& particle);
    void visit(const M2EXP2Chunk& chunk);
    void visit(const M2PABCChunk& chunk);
    void visit(const M2PADCChunk& chunk);
    void visit(const M2SequenceBounds& bounds);
    void visit(const M2PSBCChunk& chunk);
    void visit(const M2PEDCChunk& chunk);
    void visit(const M2SKIDChunk& chunk);
    void visit(const M2TXIDEntry& entry);
    void visit(const M2TXIDChunk& chunk);
    void visit(const M2LDV1Chunk& chunk);
    void visit(const M2RPIDEntry& entry);
    void visit(const M2RPIDChunk& chunk);
    void visit(const M2GPIDEntry& entry);
    void visit(const M2GPIDChunk& chunk);
    void visit(const M2WFV1Chunk& chunk);
    void visit(const M2WFV2Chunk& chunk);
    void visit(const M2PGD1Entry& entry);
    void visit(const M2PGD1Chunk& chunk);
    void visit(const M2WFV3Data& data);
    void visit(const M2WFV3Chunk& chunk);
    void visit(const M2PFDCChunk& chunk);
    void visit(const M2EDGFEntry& entry);
    void visit(const M2EDGFChunk& chunk);
    void visit(const M2NERFEntry& entry);
    void visit(const M2NERFChunk& chunk);
    void visit(const M2DETLEntry& entry);
    void visit(const M2DETLChunk& chunk);
    void visit(const M2DBOCEntry& entry);
    void visit(const M2DBOCChunk& chunk);
    void visit(const M2AFRAChunk& chunk);
    void visit(const M2PCOLChunk& chunk);
    void visit(const M2DPIVChunk& chunk);
    void visit(const M2TEXLEntry& entry);
    void visit(const M2TEXLChunk& chunk);

    void visit(const AnimationTrackBase& track);
    
    template<typename T>
    void visit(const AnimationTrack<T>& track);
    
    template<typename T>
    void visit(const std::vector<T>& array);

    void visit(const std::string& str);

    common::BinaryWriter& writer;
    uint32_t version = 0;
    std::deque<std::function<void()>> deferredWrites; // offset, write function
    u32 baseOffset = 0;
};

} // namespace m2
