// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <fstream>
#include <whiteout/models/m2/writer.h>
#include "../../common/binary_writer.h"
#include "../../common/streams.h"
#include "binary_writer_visitor.h"
#include "file_system.h"

namespace whiteout {
namespace m2 {

using common::BinaryWriter;

// ============================================================================
// WriterImpl - Implementation class using PImpl idiom
// ============================================================================

class Writer::Impl {
public:
    void writeBase(BinaryWriter& writer, const BaseFile& model);
    void writeSkin(BinaryWriter& writer, const SkinFile& model);
    void writeChunkedBase(BinaryWriter& writer, const BaseFile& model);
    void writeChunkedSkeleton(BinaryWriter& writer, const SkeletonFile& model);
    void writeChunkedBone(BinaryWriter& writer, const BoneFile& model);
    void writeChunkedAnim(BinaryWriter& writer, const AnimFile& model);
};

// ============================================================================
// Writer Public Interface (using PImpl)
// ============================================================================

Writer::Writer() : pImpl(std::make_unique<Impl>()) {}

Writer::~Writer() = default;

void Writer::write(const std::string& filePath, const FileSystem& model) {
    auto groupedFiles = fromFileSystem(model, filePath);

    {
        std::ofstream file(groupedFiles.m2, std::ios::binary);
        if (!file.is_open()) {
            throw std::runtime_error("Failed to open M2 file for writing: " +
                                     groupedFiles.m2.string());
        }
        BinaryWriter writer(file);
        pImpl->writeBase(writer, model.base);
    }
    for (const auto& skinFile : model.skins) {
        std::filesystem::path skinPath;
        if (skinFile.isLodSkin) {
            skinPath = groupedFiles.lodSkins.at(skinFile.lodLevel);
        } else {
            skinPath = groupedFiles.baseSkins.at(skinFile.index);
        }
        std::ofstream file(skinPath, std::ios::binary);
        if (!file.is_open()) {
            throw std::runtime_error("Failed to open skin file for writing: " + skinPath.string());
        }
        BinaryWriter writer(file);
        pImpl->writeSkin(writer, skinFile);
    }
    if (model.skeleton) {
        std::ofstream file(groupedFiles.skel.value(), std::ios::binary);
        if (!file.is_open()) {
            throw std::runtime_error("Failed to open skeleton file for writing: " +
                                     groupedFiles.skel.value().string());
        }
        BinaryWriter writer(file);
        pImpl->writeChunkedSkeleton(writer, model.skeleton.value());
    }
}

std::vector<u8> Writer::write(const BaseFile& model) {
    std::vector<u8> buffer;
    buffer.reserve(2 * 1024 * 1024); // Reserve 2MB to avoid frequent reallocations
    common::vector_streambuf streambuf(buffer);
    std::ostream out(&streambuf);
    BinaryWriter writer(out);

    switch (model.format) {
    case Format::ClassicMD20: {
        BinaryWriterVisitor visitor(writer);
        visitor.write(model.header);
        break;
    }
    case Format::LegionMD21:
        pImpl->writeChunkedBase(writer, model);
        break;
    }

    buffer.shrink_to_fit(); // Reduce capacity to actual size

    return buffer;
}

std::vector<u8> Writer::write(const SkinFile& model) {
    std::vector<u8> buffer;
    buffer.reserve(512 * 1024); // Reserve 512KB for skin files
    common::vector_streambuf streambuf(buffer);
    std::ostream out(&streambuf);
    BinaryWriter writer(out);

    pImpl->writeSkin(writer, model);

    buffer.shrink_to_fit();
    return buffer;
}

// ============================================================================
// WriterImpl Implementation - Moved all private methods here
// ============================================================================

void Writer::Impl::writeBase(BinaryWriter& writer, const BaseFile& model) {
    switch (model.format) {
    case Format::ClassicMD20: {
        BinaryWriterVisitor visitor(writer);
        visitor.write(model.header);
        break;
    }
    case Format::LegionMD21:
        writeChunkedBase(writer, model);
        break;
    }
}

void Writer::Impl::writeSkin(BinaryWriter& writer, const SkinFile& model) {
    BinaryWriterVisitor visitor(writer);
    visitor.write(model.profile);
}

void Writer::Impl::writeChunkedBase(BinaryWriter& writer, const BaseFile& model) {
    const auto write_chunk = ([&writer]<typename T>(u32 tag, const T& header) {
        writer.write(tag);
        u32 sizePos = writer.getPosition();
        writer.write<u32>(0); // placeholder for chunk size
        u32 chunkStart = writer.getPosition();

        BinaryWriterVisitor visitor(writer);
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
    if (model.ldv1_chunk) {
        write_chunk(LDV1_TAG, model.ldv1_chunk.value());
    }
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

void Writer::Impl::writeChunkedSkeleton(BinaryWriter& writer, const SkeletonFile& model) {
    const auto write_chunk = ([&writer]<typename T>(u32 tag, const T& chunk) {
        writer.write(tag);
        u32 sizePos = writer.getPosition();
        writer.write<u32>(0); // placeholder for chunk size
        u32 chunkStart = writer.getPosition();

        BinaryWriterVisitor visitor(writer);
        visitor.write(chunk);

        u32 chunkEnd = writer.getPosition();
        u32 chunkSize = chunkEnd - chunkStart;

        // Go back and write the actual chunk size
        writer.setPosition(sizePos);
        writer.write(chunkSize);

        // Return to the end of the chunk
        writer.setPosition(chunkEnd);
    });

    if (model.skl1_chunk) {
        write_chunk(SKL1_TAG, model.skl1_chunk.value());
    }
    if (model.ska1_chunk) {
        write_chunk(SKA1_TAG, model.ska1_chunk.value());
    }
    if (model.skb1_chunk) {
        write_chunk(SKB1_TAG, model.skb1_chunk.value());
    }
    if (model.sks1_chunk) {
        write_chunk(SKS1_TAG, model.sks1_chunk.value());
    }
    if (model.skpd_chunk) {
        write_chunk(SKPD_TAG, model.skpd_chunk.value());
    }
    if (model.afid_chunk) {
        write_chunk(AFID_TAG, model.afid_chunk.value());
    }
    if (model.bfid_chunk) {
        write_chunk(BFID_TAG, model.bfid_chunk.value());
    }
}

void Writer::Impl::writeChunkedBone(BinaryWriter& writer, const BoneFile& model) {
    // First, write the BONE header (4 bytes, should be 1)
    BinaryWriterVisitor headerWriter(writer);
    headerWriter.write(model.header);

    // Then write the chunked data
    const auto write_chunk = ([&writer]<typename T>(u32 tag, const T& chunk) {
        writer.write(tag);
        u32 sizePos = writer.getPosition();
        writer.write<u32>(0); // placeholder for chunk size
        u32 chunkStart = writer.getPosition();

        BinaryWriterVisitor visitor(writer);
        visitor.write(chunk);

        u32 chunkEnd = writer.getPosition();
        u32 chunkSize = chunkEnd - chunkStart;

        // Go back and write the actual chunk size
        writer.setPosition(sizePos);
        writer.write(chunkSize);

        // Return to the end of the chunk
        writer.setPosition(chunkEnd);
    });

    if (model.bida_chunk) {
        write_chunk(BIDA_TAG, model.bida_chunk.value());
    }
    if (model.bomt_chunk) {
        write_chunk(BOMT_TAG, model.bomt_chunk.value());
    }
}

void Writer::Impl::writeChunkedAnim(BinaryWriter& writer, const AnimFile& model) {
    if (model.profile.isChunked) {
        // Legion 24500+ chunked format
        const auto write_chunk = ([&writer]<typename T>(u32 tag, const T& chunk) {
            writer.write(tag);
            u32 sizePos = writer.getPosition();
            writer.write<u32>(0); // placeholder for chunk size
            u32 chunkStart = writer.getPosition();

            BinaryWriterVisitor visitor(writer);
            visitor.write(chunk);

            u32 chunkEnd = writer.getPosition();
            u32 chunkSize = chunkEnd - chunkStart;

            // Go back and write the actual chunk size
            writer.setPosition(sizePos);
            writer.write(chunkSize);

            // Return to the end of the chunk
            writer.setPosition(chunkEnd);
        });

        if (model.profile.afm2_chunk) {
            write_chunk(AFM2_TAG, model.profile.afm2_chunk.value());
        }
        if (model.profile.afsa_chunk) {
            write_chunk(AFSA_TAG, model.profile.afsa_chunk.value());
        }
        if (model.profile.afsb_chunk) {
            write_chunk(AFSB_TAG, model.profile.afsb_chunk.value());
        }
    } else {
        // Pre-Legion format: raw data
        writer.write(model.profile.afm2_chunk->animationData);
    }
}

} // namespace m2
} // namespace whiteout
