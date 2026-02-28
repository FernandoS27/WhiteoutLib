#pragma once

#include "../include/common/binary_reader.h"
#include "../include/m2/types.h"
#include "../include/m2/structures.h"

#include <type_traits>

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
    void read(T& header, M2File& file) {
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
    void visit(M2TXACChunk& chunk, M2File& file);

    // File reference chunks
    void visit(M2PFIDChunk& chunk, M2File& file);
    void visit(M2SFIDChunk& chunk, M2File& file);
    void visit(M2AFIDEntry& entry);
    void visit(M2AFIDChunk& chunk, M2File& file);
    void visit(M2BFIDChunk& chunk, M2File& file);

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

    template<typename T>
    void visit(std::vector<T>& array);

    void visit(std::string& str);

    template<typename T>
    void visit(AnimationTrack<T>& track);
    void visit(AnimationTrackBase& track);

    u32 version = 0;
    common::BinaryReader& reader;
    u32 baseOffset = 0;
    i32 maxSize = -1;
};

} // namespace m2
