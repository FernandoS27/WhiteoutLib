
#include "../include/m2/m2_writer.h"
#include "../include/common/binary_writer.h"
#include "../common/streams.h"
#include "m2_binary_writer_visitor.h"
#include <fstream>

namespace m2 {

using common::BinaryWriter;

M2Writer::M2Writer() = default;

void M2Writer::write(const std::string& filePath, const M2File& model) {
    std::ofstream file(filePath, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open file for writing: " + filePath);
    }
    
    BinaryWriter writer(file);
    
    switch (model.format) {
        case M2Format::ClassicMD20: {
            M2BinaryWriterVisitor visitor(writer);
            visitor.write(model.header);
            break;
        }
        case M2Format::LegionMD21:
            writeChunked(writer, model);
            break;
    }
}

std::vector<u8> M2Writer::writeToBuffer(const M2File& model) {
    std::vector<u8> buffer;
    buffer.reserve(2 * 1024 * 1024); // Reserve 2MB to avoid frequent reallocations
    common::vector_streambuf streambuf(buffer);
    std::ostream out(&streambuf);
    BinaryWriter writer(out);
    
    switch (model.format) {
        case M2Format::ClassicMD20: {
            M2BinaryWriterVisitor visitor(writer);
            visitor.write(model.header);
            break;
        }
        case M2Format::LegionMD21:
            writeChunked(writer, model);
            break;
    }

    return buffer;
}

void M2Writer::writeChunked(BinaryWriter& writer, const M2File& model) {
    const auto write_chunk = ([this, &writer]<typename T>(u32 tag,const T& header){
        writer.write(tag);
        u32 sizePos = writer.getPosition();
        writer.write<u32>(0); // placeholder for chunk size
        u32 chunkStart = writer.getPosition();
        
        M2BinaryWriterVisitor visitor(writer);
        visitor.write(header);
        
        u32 chunkEnd = writer.getPosition();
        u32 chunkSize = chunkEnd - chunkStart;
        
        // Go back and write the actual chunk size
        writer.setPosition(sizePos);
        writer.write(chunkSize);
        
        // Return to the end of the chunk
        writer.setPosition(chunkEnd);
    });
    
    write_chunk(MD21_TAG, model.header);
    if (model.pfid_chunk) {
        write_chunk(PFID_TAG, model.pfid_chunk.value());
    }
    if (model.sfid_chunk) {
        write_chunk(SFID_TAG, model.sfid_chunk.value());
    }
    if (model.afid_chunk) {
        write_chunk(AFID_TAG, model.afid_chunk.value());
    }
    if (model.bfid_chunk) {
        write_chunk(BFID_TAG, model.bfid_chunk.value());
    }
    if (model.txac_chunk) {
        write_chunk(TXAC_TAG, model.txac_chunk.value());
    }
    if (model.expt_chunk) {
        write_chunk(EXPT_TAG, model.expt_chunk.value());
    }
    if (model.exp2_chunk) {
        write_chunk(EXP2_TAG, model.exp2_chunk.value());
    }
    if (model.pabc_chunk) {
        write_chunk(PABC_TAG, model.pabc_chunk.value());
    }
    if (model.padc_chunk) {
        write_chunk(PADC_TAG, model.padc_chunk.value());
    }
    if (model.psbc_chunk) {
        write_chunk(PSBC_TAG, model.psbc_chunk.value());
    }
    if (model.pedc_chunk) {
        write_chunk(PEDC_TAG, model.pedc_chunk.value());
    }
    if (model.skid_chunk) {
        write_chunk(SKID_TAG, model.skid_chunk.value());
    }
    if (model.txid_chunk) {
        write_chunk(TXID_TAG, model.txid_chunk.value());
    }
    if (model.ldv1_chunk) {
        write_chunk(LDV1_TAG, model.ldv1_chunk.value());
    }
    if (model.rpid_chunk) {
        write_chunk(RPID_TAG, model.rpid_chunk.value());
    }
    if (model.gpid_chunk) {
        write_chunk(GPID_TAG, model.gpid_chunk.value());
    }
    if (model.wfv1_chunk) {
        write_chunk(WFV1_TAG, model.wfv1_chunk.value());
    }
    if (model.wfv2_chunk) {
        write_chunk(WFV2_TAG, model.wfv2_chunk.value());
    }
    if (model.pgd1_chunk) {
        write_chunk(PGD1_TAG, model.pgd1_chunk.value());
    }
    if (model.wfv3_chunk) {
        write_chunk(WFV3_TAG, model.wfv3_chunk.value());
    }
    if (model.pfdc_chunk) {
        writer.write(PFDC_TAG);
        writer.write<u32>(static_cast<u32>(model.pfdc_chunk->physicsData.size()));
        writer.write(model.pfdc_chunk->physicsData);
    }
    if (model.edgf_chunk) {
        write_chunk(EDGF_TAG, model.edgf_chunk.value());
    }
    if (model.nerf_chunk) {
        write_chunk(NERF_TAG, model.nerf_chunk.value());
    }
    if (model.detl_chunk) {
        write_chunk(DETL_TAG, model.detl_chunk.value());
    }
    if (model.dboc_chunk) {
        write_chunk(DBOC_TAG, model.dboc_chunk.value());
    }
    if (model.afra_chunk) {
        write_chunk(AFRA_TAG, model.afra_chunk.value());
    }
    if (model.pcol_chunk) {
        write_chunk(PCOL_TAG, model.pcol_chunk.value());
    }
    if (model.dpiv_chunk) {
        write_chunk(DPIV_TAG, model.dpiv_chunk.value());
    }
    if (model.texl_chunk) {
        write_chunk(TEXL_TAG, model.texl_chunk.value());
    }
}

} // namespace m2
