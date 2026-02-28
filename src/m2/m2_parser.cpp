
#include "../include/m2/m2_parser.h"
#include "../include/common/binary_reader.h"
#include "../common/streams.h"
#include "m2_binary_parse_visitor.h"

#include <fstream>
#include <cstring>

namespace m2 {

using common::BinaryReader;

M2Parser::M2Parser(ParseMode mode) 
    : parseMode(mode) {}

M2File M2Parser::parse(const std::string& filePath) {
    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open M2 file: " + filePath);
    }
    BinaryReader reader(file);
    return parse(reader);
}

M2File M2Parser::parse(std::span<const u8> buffer) {
    common::span_streambuf streambuf(buffer);
    std::istream in(&streambuf);
    BinaryReader reader(in);
    return parse(reader);
}

M2File M2Parser::parse(BinaryReader& reader) {
    M2File m2file;
    u32 magic = reader.read<u32>();
    reader.setPosition(0);
    
    if (magic == MD20_TAG) {
        m2file.format = M2Format::ClassicMD20;
        M2BinaryParseVisitor parser(reader);
        parser.read(m2file.header);
    } else if (magic == MD21_TAG) {
        m2file.format = M2Format::LegionMD21;
        parseChunked(reader, m2file);
    } else {
        std::string error = "Invalid M2 magic: expected MD20 or MD21, got '" 
                           + std::string(reinterpret_cast<char*>(&magic), 4) + "'";
        if (parseMode == ParseMode::Strict) {
            throw std::runtime_error(error);
        }
        issues.push_back(error);
        return m2file;
    }
    return m2file;
}

void M2Parser::parseChunked(BinaryReader& reader, M2File& m2file) {
    while (reader.hasRemaining()) {
        u32 chunkTag = reader.read<u32>();
        u32 chunkSize = reader.read<u32>();
        u32 chunkStart = reader.getPosition();
        
        switch (chunkTag) {
            case MD21_TAG: {
                M2BinaryParseVisitor parser(reader, chunkSize);
                parser.read(m2file.header);
                break;
            }
            case PFID_TAG: {
                M2BinaryParseVisitor parser(reader, chunkSize);
                m2file.pfid_chunk.emplace();
                parser.read(m2file.pfid_chunk.value(), m2file);
                break;
            }
            case SFID_TAG: {
                M2BinaryParseVisitor parser(reader, chunkSize);
                m2file.sfid_chunk.emplace();
                parser.read(m2file.sfid_chunk.value(), m2file);
                break;
            }
            case AFID_TAG: {
                M2BinaryParseVisitor parser(reader, chunkSize);
                m2file.afid_chunk.emplace();
                parser.read(m2file.afid_chunk.value(), m2file);
                break;
            }
            case BFID_TAG: {
                M2BinaryParseVisitor parser(reader, chunkSize);
                m2file.bfid_chunk.emplace();
                parser.read(m2file.bfid_chunk.value(), m2file);
                break;
            }
            case TXAC_TAG: {
                M2BinaryParseVisitor parser(reader, chunkSize);
                m2file.txac_chunk.emplace();
                parser.read(m2file.txac_chunk.value(), m2file);
                break;
            }
            case EXPT_TAG: {
                M2BinaryParseVisitor parser(reader, chunkSize);
                m2file.expt_chunk.emplace();
                parser.read(m2file.expt_chunk.value());
                break;
            }
            case EXP2_TAG: {
                M2BinaryParseVisitor parser(reader, chunkSize);
                m2file.exp2_chunk.emplace();
                parser.read(m2file.exp2_chunk.value());
                break;
            }
            case PABC_TAG: {
                M2BinaryParseVisitor parser(reader, chunkSize);
                m2file.pabc_chunk.emplace();
                parser.read(m2file.pabc_chunk.value());
                break;
            }
            case PADC_TAG: {
                M2BinaryParseVisitor parser(reader, chunkSize);
                m2file.padc_chunk.emplace();
                parser.read(m2file.padc_chunk.value());
                break;
            }
            case PSBC_TAG: {
                M2BinaryParseVisitor parser(reader, chunkSize);
                m2file.psbc_chunk.emplace();
                parser.read(m2file.psbc_chunk.value());
                break;
            }
            case PEDC_TAG: {
                M2BinaryParseVisitor parser(reader, chunkSize);
                m2file.pedc_chunk.emplace();
                parser.read(m2file.pedc_chunk.value());
                break;
            }
            case SKID_TAG: {
                M2BinaryParseVisitor parser(reader, chunkSize);
                m2file.skid_chunk.emplace();
                parser.read(m2file.skid_chunk.value());
                break;
            }
            case TXID_TAG: {
                M2BinaryParseVisitor parser(reader, chunkSize);
                m2file.txid_chunk.emplace();
                parser.read(m2file.txid_chunk.value());
                break;
            }
            case LDV1_TAG: {
                M2BinaryParseVisitor parser(reader, chunkSize);
                m2file.ldv1_chunk.emplace();
                parser.read(m2file.ldv1_chunk.value());
                break;
            }
            case RPID_TAG: {
                M2BinaryParseVisitor parser(reader, chunkSize);
                m2file.rpid_chunk.emplace();
                parser.read(m2file.rpid_chunk.value());
                break;
            }
            case GPID_TAG: {
                M2BinaryParseVisitor parser(reader, chunkSize);
                m2file.gpid_chunk.emplace();
                parser.read(m2file.gpid_chunk.value());
                break;
            }
            case WFV1_TAG: {
                M2BinaryParseVisitor parser(reader, chunkSize);
                m2file.wfv1_chunk.emplace();
                parser.read(m2file.wfv1_chunk.value());
                break;
            }
            case WFV2_TAG: {
                M2BinaryParseVisitor parser(reader, chunkSize);
                m2file.wfv2_chunk.emplace();
                parser.read(m2file.wfv2_chunk.value());
                break;
            }
            case PGD1_TAG: {
                M2BinaryParseVisitor parser(reader, chunkSize);
                m2file.pgd1_chunk.emplace();
                parser.read(m2file.pgd1_chunk.value());
                break;
            }
            case WFV3_TAG: {
                M2BinaryParseVisitor parser(reader, chunkSize);
                m2file.wfv3_chunk.emplace();
                parser.read(m2file.wfv3_chunk.value());
                break;
            }
            case PFDC_TAG: {
                reportIssue("Parsing PFDC chunk is not fully implemented yet");
                m2file.pfdc_chunk.emplace();
                m2file.pfdc_chunk->physicsData = reader.read<std::vector<u8>>(chunkSize);
                break;
            }
            case EDGF_TAG: {
                M2BinaryParseVisitor parser(reader, chunkSize);
                m2file.edgf_chunk.emplace();
                parser.read(m2file.edgf_chunk.value());
                break;
            }
            case NERF_TAG: {
                M2BinaryParseVisitor parser(reader, chunkSize);
                m2file.nerf_chunk.emplace();
                parser.read(m2file.nerf_chunk.value());
                break;
            }
            case DETL_TAG: {
                M2BinaryParseVisitor parser(reader, chunkSize);
                m2file.detl_chunk.emplace();
                parser.read(m2file.detl_chunk.value());
                break;
            }
            case DBOC_TAG: {
                M2BinaryParseVisitor parser(reader, chunkSize);
                m2file.dboc_chunk.emplace();
                parser.read(m2file.dboc_chunk.value());
                break;
            }
            case AFRA_TAG: {
                M2BinaryParseVisitor parser(reader, chunkSize);
                m2file.afra_chunk.emplace();
                parser.read(m2file.afra_chunk.value());
                break;
            }
            case PCOL_TAG: {
                M2BinaryParseVisitor parser(reader, chunkSize);
                m2file.pcol_chunk.emplace();
                parser.read(m2file.pcol_chunk.value());
                break;
            }
            case DPIV_TAG: {
                M2BinaryParseVisitor parser(reader, chunkSize);
                m2file.dpiv_chunk.emplace();
                parser.read(m2file.dpiv_chunk.value());
                break;
            }
            case TEXL_TAG: {
                M2BinaryParseVisitor parser(reader, chunkSize);
                m2file.texl_chunk.emplace();
                parser.read(m2file.texl_chunk.value());
                break;
            }
            default:
                skipUnknownChunk(reader, chunkTag, chunkSize);
                break;
        }
        reader.setPosition(chunkStart + chunkSize);
    }
}

void M2Parser::skipUnknownChunk(BinaryReader& reader, u32 tag, u32 size) {
    std::string tagStr(reinterpret_cast<char*>(&tag), 4);
    reportIssue("Unknown M2 chunk: " + tagStr + " (size: " + std::to_string(size) + ")");
}

void M2Parser::reportIssue(const std::string& message) {
    if (parseMode == ParseMode::Strict) {
        throw std::runtime_error(message);
    }
    issues.push_back(message);
}

} // namespace m2
