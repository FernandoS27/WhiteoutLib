
#include "../include/m2/m2_parser.h"
#include "../common/binary_reader.h"
#include "../common/streams.h"
#include "m2_binary_parse_visitor.h"
#include "m2_file_system.h"

#include <fstream>
#include <cstring>
#include <stdexcept>

namespace whiteout {
namespace m2 {

using common::BinaryReader;

M2Parser::M2Parser(ParseMode mode) 
    : parseMode(mode) {}

M2FileSystem M2Parser::parse(const std::string& filePath) {
    auto groupedFiles = m2::collectM2Bundle(filePath);
    if (!groupedFiles) {
        throw std::runtime_error("Failed to collect M2 bundle: " + filePath);
    }
    M2FileSystem fileSystem;
    // Parse main M2 file
    {
        std::ifstream file(groupedFiles->m2, std::ios::binary);
        if (!file.is_open()) {
            throw std::runtime_error("Failed to open M2 file: " + groupedFiles->m2.string());
        }
        BinaryReader reader(file);
        parseM2Base(reader, fileSystem.base);
        fileSystem.baseName = groupedFiles->m2.stem().string();
    }
    // Parse skin files
    for (const auto& [index, skinPath] : groupedFiles->baseSkins) {
        std::ifstream file(skinPath, std::ios::binary);
        if (!file.is_open()) {
            reportIssue("Failed to open skin file: " + skinPath.string());
            continue;
        }
        
        try {
            BinaryReader reader(file);
            M2SkinFile skinFile;
            parseM2Skin(reader, skinFile);
            skinFile.isLodSkin = false;
            skinFile.index = index;
            fileSystem.skins.push_back(std::move(skinFile));
        } catch (const std::exception& e) {
            reportIssue("Error parsing skin file '" + skinPath.string() + "': " + e.what());
        }
    }
    for (const auto& [lodLevel, skinPath] : groupedFiles->lodSkins) {
        std::ifstream file(skinPath, std::ios::binary);
        if (!file.is_open()) {
            reportIssue("Failed to open skin file: " + skinPath.string());
            continue;
        }
        try {
            BinaryReader reader(file);
            M2SkinFile skinFile;
            parseM2Skin(reader, skinFile);
            skinFile.isLodSkin = true;
            skinFile.lodLevel = lodLevel;
            fileSystem.skins.push_back(std::move(skinFile));
        } catch (const std::exception& e) {
            reportIssue("Error parsing skin file '" + skinPath.string() + "': " + e.what());
        }
    }
    if (groupedFiles->skel) {
        std::ifstream file(groupedFiles->skel.value(), std::ios::binary);
        if (!file.is_open()) {
            reportIssue("Failed to open skeleton file: " + groupedFiles->skel.value().string());
        } else {
            try {
                BinaryReader reader(file);
                fileSystem.skeleton.emplace();
                parseChunkedM2Skeleton(reader, fileSystem.skeleton.value());
            } catch (const std::exception& e) {
                reportIssue("Error parsing skeleton file '" + groupedFiles->skel.value().string() + "': " + e.what());
            }
        }
    }
    for (const auto& [boneId, bonePath] : groupedFiles->bones) {
        std::ifstream file(bonePath, std::ios::binary);
        if (!file.is_open()) {
            reportIssue("Failed to open bone file: " + bonePath.string());
            continue;
        }
        try {
            BinaryReader reader(file);
            M2BoneFile boneFile;
            boneFile.boneId = boneId;
            parseChunkedM2Bone(reader, boneFile);
            fileSystem.bones.push_back(std::move(boneFile));
        } catch (const std::exception& e) {
            reportIssue("Error parsing bone file '" + bonePath.string() + "': " + e.what());
        }
    }
    for (const auto& [animId, animVariants] : groupedFiles->anims) {
        for (const auto& [variant, animPath] : animVariants.variants) {
            std::ifstream file(animPath, std::ios::binary);
            if (!file.is_open()) {
                reportIssue("Failed to open anim file: " + animPath.string());
                continue;
            }
            try {
                BinaryReader reader(file);
                M2AnimFile animFile;
                animFile.animId = animId;
                animFile.variant = variant;
                parseChunkedM2Anim(reader, animFile);
                fileSystem.anims.push_back(std::move(animFile));
            } catch (const std::exception& e) {
                reportIssue("Error parsing anim file '" + animPath.string() + "': " + e.what());
            }
        }
    }
    return fileSystem;
}

void M2Parser::parse(std::span<const uint8_t> buffer, M2FileSystem& fileSystem, M2FileType fileType) {
    common::span_streambuf streambuf(buffer);
    std::istream in(&streambuf);
    BinaryReader reader(in);
    switch (fileType) {
        case M2FileType::Base:
            parseM2Base(reader, fileSystem.base);
            break;
        case M2FileType::Skin:
            fileSystem.skins.emplace_back();
            parseM2Skin(reader, fileSystem.skins.back());
            break;
        case M2FileType::Skeleton:
            if (fileSystem.skeleton.has_value()) {
                reportIssue("Multiple skeleton files found in buffer, skipping additional ones");
                return;
            }
            fileSystem.skeleton.emplace();
            parseChunkedM2Skeleton(reader, fileSystem.skeleton.value());
            break;
        // Add cases for other file types as needed
        default:
            throw std::runtime_error("Unsupported M2 file type for parsing");
    }
}

void M2Parser::parseM2Base(BinaryReader& reader,  M2BaseFile& file) {
    u32 magic = reader.read<u32>();
    reader.setPosition(0);
    
    if (magic == MD20_TAG) {
        file.format = M2Format::ClassicMD20;
        M2BinaryParseVisitor parser(reader);
        parser.read(file.header);
        return;
    } else if (magic == MD21_TAG) {
        file.format = M2Format::LegionMD21;
        parseChunkedM2Base(reader, file);
        return;
    }
    std::string error = "Invalid M2 magic: expected MD20 or MD21, got '" 
                        + std::string(reinterpret_cast<char*>(&magic), 4) + "'";
    if (parseMode == ParseMode::Strict) {
        throw std::runtime_error(error);
    }
    issues.push_back(error);
}

void M2Parser::parseM2Skin(BinaryReader& reader, M2SkinFile& skinFile) {
    u32 magic = reader.read<u32>();
    reader.setPosition(0);
    
    if (magic == SKIN_TAG) {
        skinFile.profile = M2SkinProfile();
        M2BinaryParseVisitor parser(reader);
        parser.read(skinFile.profile);
        return;
    }
    std::string error = "Invalid M2 magic: expected SKIN, got '" 
                        + std::string(reinterpret_cast<char*>(&magic), 4) + "'";
    if (parseMode == ParseMode::Strict) {
        throw std::runtime_error(error);
    }
    issues.push_back(error);
}

void M2Parser::parseChunkedM2Base(BinaryReader& reader, M2BaseFile& m2file) {
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
                parser.read(m2file.afid_chunk.value());
                break;
            }
            case BFID_TAG: {
                M2BinaryParseVisitor parser(reader, chunkSize);
                m2file.bfid_chunk.emplace();
                parser.read(m2file.bfid_chunk.value());
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

void M2Parser::parseChunkedM2Skeleton(BinaryReader& reader, M2SkeletonFile& skeletonFile) {
    while (reader.hasRemaining()) {
        u32 chunkTag = reader.read<u32>();
        u32 chunkSize = reader.read<u32>();
        u32 chunkStart = reader.getPosition();
        
        switch (chunkTag) {
            case SKL1_TAG: {
                M2BinaryParseVisitor parser(reader, chunkSize);
                skeletonFile.skl1_chunk.emplace();
                parser.read(skeletonFile.skl1_chunk.value());
                break;
            }
            case SKA1_TAG: {
                M2BinaryParseVisitor parser(reader, chunkSize);
                skeletonFile.ska1_chunk.emplace();
                parser.read(skeletonFile.ska1_chunk.value());
                break;
            }
            case SKB1_TAG: {
                M2BinaryParseVisitor parser(reader, chunkSize);
                skeletonFile.skb1_chunk.emplace();
                parser.read(skeletonFile.skb1_chunk.value());
                break;
            }
            case SKS1_TAG: {
                M2BinaryParseVisitor parser(reader, chunkSize);
                skeletonFile.sks1_chunk.emplace();
                parser.read(skeletonFile.sks1_chunk.value());
                break;
            }
            case SKPD_TAG: {
                M2BinaryParseVisitor parser(reader, chunkSize);
                skeletonFile.skpd_chunk.emplace();
                parser.read(skeletonFile.skpd_chunk.value());
                break;
            }
            case AFID_TAG: {
                M2BinaryParseVisitor parser(reader, chunkSize);
                skeletonFile.afid_chunk.emplace();
                parser.read(skeletonFile.afid_chunk.value());
                break;
            }
            case BFID_TAG: {
                M2BinaryParseVisitor parser(reader, chunkSize);
                skeletonFile.bfid_chunk.emplace();
                parser.read(skeletonFile.bfid_chunk.value());
                break;
            }
            default:
                skipUnknownChunk(reader, chunkTag, chunkSize);
                break;
        }
        reader.setPosition(chunkStart + chunkSize);
    }
}

void M2Parser::parseChunkedM2Bone(BinaryReader& reader, M2BoneFile& boneFile) {
    // First, read the BONE header (4 bytes, should be 1)
    M2BinaryParseVisitor headerParser(reader);
    headerParser.read(boneFile.header);
    
    // Then read the chunked data
    while (reader.hasRemaining()) {
        u32 chunkTag = reader.read<u32>();
        u32 chunkSize = reader.read<u32>();
        u32 chunkStart = reader.getPosition();
        
        switch (chunkTag) {
            case BIDA_TAG: {
                M2BinaryParseVisitor parser(reader, chunkSize);
                boneFile.bida_chunk.emplace();
                parser.read(boneFile.bida_chunk.value());
                break;
            }
            case BOMT_TAG: {
                M2BinaryParseVisitor parser(reader, chunkSize);
                boneFile.bomt_chunk.emplace();
                parser.read(boneFile.bomt_chunk.value());
                break;
            }
            default:
                skipUnknownChunk(reader, chunkTag, chunkSize);
                break;
        }
        reader.setPosition(chunkStart + chunkSize);
    }
}

void M2Parser::parseChunkedM2Anim(BinaryReader& reader, M2AnimFile& animFile) {
    // Check if the file is chunked by peeking at the first 4 bytes
    u32 firstTag = reader.read<u32>();
    reader.setPosition(0);
    
    // If it starts with AFM2, AFSA, or AFSB, it's chunked format
    if (firstTag != AFM2_TAG && firstTag != AFSA_TAG && firstTag != AFSB_TAG) {
        // Pre-Legion format: raw data
        u32 fileSize = reader.getRemainingBytes();

        // Update it to use the new chunked structure for consistency
        animFile.profile.afm2_chunk.emplace();
        animFile.profile.isChunked = true;
        animFile.profile.afm2_chunk->animationData = reader.read<std::vector<u8>>(fileSize);
        return;
    }
    // Chunked format
    animFile.profile.isChunked = true;
    
    while (reader.hasRemaining()) {
        u32 chunkTag = reader.read<u32>();
        u32 chunkSize = reader.read<u32>();
        u32 chunkStart = reader.getPosition();
        
        switch (chunkTag) {
            case AFM2_TAG: {
                M2BinaryParseVisitor parser(reader, chunkSize);
                animFile.profile.afm2_chunk.emplace();
                parser.read(animFile.profile.afm2_chunk.value());
                break;
            }
            case AFSA_TAG: {
                M2BinaryParseVisitor parser(reader, chunkSize);
                animFile.profile.afsa_chunk.emplace();
                parser.read(animFile.profile.afsa_chunk.value());
                break;
            }
            case AFSB_TAG: {
                M2BinaryParseVisitor parser(reader, chunkSize);
                animFile.profile.afsb_chunk.emplace();
                parser.read(animFile.profile.afsb_chunk.value());
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
} // namespace whiteout
