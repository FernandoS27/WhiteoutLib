
#include "../../common/binary_reader.h"
#include "binary_parse_visitor.h"
#include "chunk_parser.h"
#include "wow_file_system.h"

#include <cstring>

namespace whiteout {
namespace m2 {

using common::BinaryReader;

ChunkParser::ChunkParser() = default;

void ChunkParser::reportIssue(const std::string& message) {
    issues.push_back(message);
}

void ChunkParser::skipUnknownChunk(BinaryReader& reader, u32 tag, u32 size) {
    std::string const error = "Unknown chunk: " + std::string(reinterpret_cast<char*>(&tag), 4) +
                              " (size: " + std::to_string(size) + ")";
    reportIssue(error);
    reader.skip(size);
}

void ChunkParser::drainIssues(std::vector<std::string>& target) {
    target.insert(target.end(), std::make_move_iterator(issues.begin()),
                  std::make_move_iterator(issues.end()));
    issues.clear();
}

std::vector<ChunkParser::ChunkEntry> ChunkParser::collectChunks(BinaryReader& reader) {
    std::vector<ChunkEntry> chunks;
    while (reader.hasRemaining()) {
        u32 const tag = reader.read<u32>();
        u32 const size = reader.read<u32>();
        u32 const dataOffset = reader.getPosition();
        chunks.push_back({tag, size, dataOffset});
        reader.skip(size);
    }
    return chunks;
}

void ChunkParser::parseChunkedBase(BinaryReader& reader, BaseFile& m2file, WoWFileSystem* wfs) {
    auto chunks = collectChunks(reader);

    std::vector<ChunkEntry> nextChunks;

    for (const auto& chunk : chunks) {
        reader.setPosition(chunk.dataOffset);

        switch (chunk.tag) {
        case MD21_TAG:
        case SFID_TAG:
        case EXP2_TAG: {
            nextChunks.push_back(chunk);
            break;
        }
        case PFID_TAG: {
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            m2file.pfid_chunk.emplace();
            parser.read(m2file.pfid_chunk.value(), m2file);
            break;
        }
        case AFID_TAG: {
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            m2file.afid_chunk.emplace();
            parser.read(m2file.afid_chunk.value());
            break;
        }
        case BFID_TAG: {
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            m2file.bfid_chunk.emplace();
            parser.read(m2file.bfid_chunk.value());
            break;
        }
        case TXAC_TAG: {
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            m2file.txac_chunk.emplace();
            parser.read(m2file.txac_chunk.value(), m2file);
            break;
        }
        case EXPT_TAG: {
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            m2file.expt_chunk.emplace();
            parser.read(m2file.expt_chunk.value());
            break;
        }
        case PABC_TAG: {
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            m2file.pabc_chunk.emplace();
            parser.read(m2file.pabc_chunk.value());
            break;
        }
        case PADC_TAG: {
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            m2file.padc_chunk.emplace();
            parser.read(m2file.padc_chunk.value());
            break;
        }
        case PSBC_TAG: {
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            m2file.psbc_chunk.emplace();
            parser.read(m2file.psbc_chunk.value());
            break;
        }
        case PEDC_TAG: {
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            m2file.pedc_chunk.emplace();
            parser.read(m2file.pedc_chunk.value());
            break;
        }
        case SKID_TAG: {
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            m2file.skid_chunk.emplace();
            parser.read(m2file.skid_chunk.value());
            break;
        }
        case TXID_TAG: {
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            m2file.txid_chunk.emplace();
            parser.read(m2file.txid_chunk.value());
            break;
        }
        case LDV1_TAG: {
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            m2file.ldv1_chunk.emplace();
            parser.read(m2file.ldv1_chunk.value());
            break;
        }
        case RPID_TAG: {
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            m2file.rpid_chunk.emplace();
            parser.read(m2file.rpid_chunk.value());
            break;
        }
        case GPID_TAG: {
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            m2file.gpid_chunk.emplace();
            parser.read(m2file.gpid_chunk.value());
            break;
        }
        case WFV1_TAG:
        case WFV2_TAG:
            break; // Obsolete markers — dropped on parse
        case PGD1_TAG: {
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            m2file.pgd1_chunk.emplace();
            parser.read(m2file.pgd1_chunk.value());
            break;
        }
        case WFV3_TAG: {
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            m2file.wfv3_chunk.emplace();
            parser.read(m2file.wfv3_chunk.value());
            break;
        }
        case PFDC_TAG: {
            reportIssue("Parsing PFDC chunk is not fully implemented yet");
            m2file.pfdc_chunk.emplace();
            m2file.pfdc_chunk->physicsData = reader.read<std::vector<u8>>(chunk.size);
            break;
        }
        case EDGF_TAG: {
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            m2file.edgf_chunk.emplace();
            parser.read(m2file.edgf_chunk.value());
            break;
        }
        case NERF_TAG: {
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            m2file.nerf_chunk.emplace();
            parser.read(m2file.nerf_chunk.value());
            break;
        }
        case DETL_TAG: {
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            m2file.detl_chunk.emplace();
            parser.read(m2file.detl_chunk.value());
            break;
        }
        case DBOC_TAG: {
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            m2file.dboc_chunk.emplace();
            parser.read(m2file.dboc_chunk.value());
            break;
        }
        case AFRA_TAG: {
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            m2file.afra_chunk.emplace();
            parser.read(m2file.afra_chunk.value());
            break;
        }
        case PCOL_TAG: {
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            m2file.pcol_chunk.emplace();
            parser.read(m2file.pcol_chunk.value());
            break;
        }
        case DPIV_TAG: {
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            m2file.dpiv_chunk.emplace();
            parser.read(m2file.dpiv_chunk.value());
            break;
        }
        case TEXL_TAG: {
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            m2file.texl_chunk.emplace();
            parser.read(m2file.texl_chunk.value());
            break;
        }
        default:
            skipUnknownChunk(reader, chunk.tag, chunk.size);
            break;
        }
    }

    if (m2file.afid_chunk) {
        wfs->setAnimChunk(*m2file.afid_chunk);
    }

    for (const auto& chunk : nextChunks) {
        if (chunk.tag == MD21_TAG) {
            reader.setPosition(chunk.dataOffset);
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            parser.read(m2file.header);
        } else if (chunk.tag == SFID_TAG) {
            reader.setPosition(chunk.dataOffset);
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            m2file.sfid_chunk.emplace();
            parser.read(m2file.sfid_chunk.value(), m2file);
        } else if (chunk.tag == EXP2_TAG) {
            reader.setPosition(chunk.dataOffset);
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            m2file.exp2_chunk.emplace();
            parser.read(m2file.exp2_chunk.value());
        }
    }
    if (m2file.skid_chunk) {
        wfs->setSkeletonChunk(*m2file.skid_chunk);
    }
    if (m2file.sfid_chunk) {
        wfs->setSkinChunk(*m2file.sfid_chunk);
    }
}

void ChunkParser::parseChunkedSkeleton(BinaryReader& reader, SkeletonFile& skeletonFile,
                                       WoWFileSystem* wfs) {
    auto chunks = collectChunks(reader);

    for (const auto& chunk : chunks) {
        reader.setPosition(chunk.dataOffset);

        switch (chunk.tag) {
        case AFID_TAG: {
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            skeletonFile.afid_chunk.emplace();
            parser.read(skeletonFile.afid_chunk.value());
            break;
        }
        case BFID_TAG: {
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            skeletonFile.bfid_chunk.emplace();
            parser.read(skeletonFile.bfid_chunk.value());
            break;
        }
        case SKL1_TAG:
        case SKA1_TAG:
        case SKB1_TAG:
        case SKS1_TAG:
        case SKPD_TAG:
            break; // These will be parsed in the second pass to ensure we have AFID/BFID data
                   // available if needed
        default:
            skipUnknownChunk(reader, chunk.tag, chunk.size);
            break;
        }
    }

    if (skeletonFile.afid_chunk) {
        wfs->setAnimChunk(*skeletonFile.afid_chunk);
    }

    for (const auto& chunk : chunks) {
        reader.setPosition(chunk.dataOffset);

        switch (chunk.tag) {
        case SKL1_TAG: {
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            skeletonFile.skl1_chunk.emplace();
            parser.read(skeletonFile.skl1_chunk.value());
            break;
        }
        case SKA1_TAG: {
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            skeletonFile.ska1_chunk.emplace();
            parser.read(skeletonFile.ska1_chunk.value());
            break;
        }
        case SKB1_TAG: {
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            skeletonFile.skb1_chunk.emplace();
            parser.read(skeletonFile.skb1_chunk.value());
            break;
        }
        case SKS1_TAG: {
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            skeletonFile.sks1_chunk.emplace();
            parser.read(skeletonFile.sks1_chunk.value());
            break;
        }
        case SKPD_TAG: {
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            skeletonFile.skpd_chunk.emplace();
            parser.read(skeletonFile.skpd_chunk.value());
            break;
        }
        case AFID_TAG:
        case BFID_TAG:
        default:
            break;
        }
    }
}

void ChunkParser::parseChunkedBone(BinaryReader& reader, BoneFile& boneFile, WoWFileSystem* wfs) {

    BinaryParseVisitor headerParser(reader, wfs);
    headerParser.read(boneFile.header);

    auto chunks = collectChunks(reader);

    for (const auto& chunk : chunks) {
        reader.setPosition(chunk.dataOffset);

        switch (chunk.tag) {
        case BIDA_TAG: {
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            boneFile.bida_chunk.emplace();
            parser.read(boneFile.bida_chunk.value());
            break;
        }
        case BOMT_TAG: {
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            boneFile.bomt_chunk.emplace();
            parser.read(boneFile.bomt_chunk.value());
            break;
        }
        default:
            skipUnknownChunk(reader, chunk.tag, chunk.size);
            break;
        }
    }
}

void ChunkParser::parseChunkedAnim(BinaryReader& reader, AnimFile& animFile, WoWFileSystem* wfs,
                                   bool isChunked) {

    if (!isChunked) {

        u32 const fileSize = reader.getRemainingBytes();
        animFile.profile.afm2_chunk.emplace();
        animFile.profile.isChunked = true;
        animFile.profile.afm2_chunk->animationData = reader.read<std::vector<u8>>(fileSize);
        return;
    }

    animFile.profile.isChunked = true;

    auto chunks = collectChunks(reader);

    for (const auto& chunk : chunks) {
        reader.setPosition(chunk.dataOffset);

        switch (chunk.tag) {
        case AFM2_TAG: {
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            animFile.profile.afm2_chunk.emplace();
            parser.read(animFile.profile.afm2_chunk.value());
            break;
        }
        case AFSA_TAG: {
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            animFile.profile.afsa_chunk.emplace();
            parser.read(animFile.profile.afsa_chunk.value());
            break;
        }
        case AFSB_TAG: {
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            animFile.profile.afsb_chunk.emplace();
            parser.read(animFile.profile.afsb_chunk.value());
            break;
        }
        default:
            skipUnknownChunk(reader, chunk.tag, chunk.size);
            break;
        }
    }
}

} // namespace m2
} // namespace whiteout
