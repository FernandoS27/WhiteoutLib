
#include <whiteout/models/m2/parser.h>
#include "../../common/binary_reader.h"
#include "../../common/streams.h"
#include "binary_parse_visitor.h"
#include "chunk_parser.h"
#include "wow_file_system.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <unordered_map>

namespace whiteout {
namespace m2 {

using common::BinaryReader;

class Parser::Impl {
public:
    ParseMode parseMode;
    std::vector<std::string> issues;
    ChunkParser chunkParser;

    explicit Impl(ParseMode mode) : parseMode(mode), chunkParser(mode) {}

    void parse(WoWFileSystem& wfs, Model& result);
    void parseBase(BinaryReader& reader, BaseFile& file, WoWFileSystem* wfs);
    void parseSkin(BinaryReader& reader, SkinFile& skinFile, WoWFileSystem* wfs);

    void reportIssue(const std::string& message);
};

Parser::Parser(ParseMode mode) : pImpl(std::make_unique<Impl>(mode)) {
}

Parser::~Parser() = default;

Model Parser::parse(interfaces::VirtualPathFileSystem& fs, const std::string& filePath) {
    pImpl->issues.clear();

    WoWFileSystem wfs(fs, filePath);
    Model model;
    wfs.exploratorySearch();

    pImpl->parse(wfs, model);

    pImpl->chunkParser.drainIssues(pImpl->issues);
    return model;
}

Model Parser::parse(interfaces::CascFileSystem& cascFs,
                         std::span<const uint8_t> buffer) {
    pImpl->issues.clear();

    WoWFileSystem wfs(cascFs, buffer);
    Model model;

    pImpl->parse(wfs, model);

    pImpl->chunkParser.drainIssues(pImpl->issues);
    return model;
}

const std::vector<std::string>& Parser::getIssues() const {
    return pImpl->issues;
}

bool Parser::hasIssues() const {
    return !pImpl->issues.empty();
}

void Parser::Impl::reportIssue(const std::string& message) {
    if (parseMode == ParseMode::Strict) {
        throw std::runtime_error(message);
    }
    issues.push_back(message);
}

void Parser::Impl::parse(WoWFileSystem& wfs, Model& result) {
    BaseFile m2;

    {
        auto m2Data = wfs.getM2Base();
        common::span_streambuf sbuf(m2Data);
        std::istream in(&sbuf);
        BinaryReader reader(in);
        parseBase(reader, m2, &wfs);
    }
    result = std::move(m2.header.model);
    const auto readSkinProfile = [&](std::vector<SkinProfile>& profiles, std::span<const u8> skinData, size_t i) {
        try {
            common::span_streambuf sbuf(skinData);
            std::istream in(&sbuf);
            BinaryReader reader(in);
            SkinFile skinFile;
            skinFile.version = m2.header.version;
            parseSkin(reader, skinFile, &wfs);
            profiles.emplace_back(std::move(skinFile.profile));
        } catch (const std::exception& e) {
            reportIssue(std::string("Error parsing skin ") + std::to_string(i) + ": " + e.what());
        }
    };

    if (m2.sfid_chunk) {
        const auto& sfid = *m2.sfid_chunk;
        for (u32 i = 0; i < static_cast<u32>(sfid.skinFileDataIds.size()); ++i) {
            auto skinData = wfs.getSkin(i, false);
            if (skinData.empty()) continue;
            readSkinProfile(result.skinProfiles, skinData, i);
        }
        for (u32 i = 0; i < static_cast<u32>(sfid.lodSkinFileDataIds.size()); ++i) {
            auto skinData = wfs.getSkin(i, true);
            if (skinData.empty()) continue;
            readSkinProfile(result.lodProfiles, skinData, i);
        }
    }

    std::optional<SkeletonFile> skeleton;
    bool hasParent = false;
    do {
        hasParent = false;
        auto skelData = wfs.getSkeleton();
        if (!skelData.empty()) {
            try {
                common::span_streambuf sbuf(skelData);
                std::istream in(&sbuf);
                BinaryReader reader(in);
                skeleton.emplace();
                chunkParser.parseChunkedSkeleton(reader, skeleton.value(), &wfs);
                if (skeleton->skpd_chunk) {
                    hasParent = true;
                    wfs.setParentSkeletonChunk(*skeleton->skpd_chunk);
                    skeleton.reset();
                }
            } catch (const std::exception& e) {
                reportIssue(std::string("Error parsing skeleton: ") + e.what());
                skeleton.reset();
            }
        }
    } while (hasParent);

    if (skeleton) {
        if (skeleton->skpd_chunk) {
            reportIssue("Parent skeleton reference found, but parent skeleton parsing is not yet implemented");
        }
        if (skeleton->skb1_chunk) {
            result.bones = std::move(skeleton->skb1_chunk->bones);
            result.keyBoneIds = std::move(skeleton->skb1_chunk->keyBoneLookup);
        }
        if (skeleton->sks1_chunk) {
            result.globalLoops = std::move(skeleton->sks1_chunk->globalLoops);
            result.sequences = std::move(skeleton->sks1_chunk->sequences);
            result.sequenceIdxHashById = std::move(skeleton->sks1_chunk->sequenceLookups);
        }
        if (skeleton->ska1_chunk) {
            result.attachments = std::move(skeleton->ska1_chunk->attachments);
            result.attachmentIndicesById = std::move(skeleton->ska1_chunk->attachmentLookupTable);
        }
    }

    if (m2.expt_chunk) {
        size_t numParticleEmitters = result.particleEmitters.size();
        for (size_t i = 0; i < numParticleEmitters; ++i) {
            auto& emitter_extension = result.particleEmitters[i].extension;
            emitter_extension.emplace();
            emitter_extension->zSource = m2.expt_chunk->extendedParticles[i].zSource;
            emitter_extension->colorMult = m2.expt_chunk->extendedParticles[i].colorMult;
            emitter_extension->alphaMult = m2.expt_chunk->extendedParticles[i].alphaMult;
        }
    }

    if (m2.exp2_chunk) {
        size_t numParticleEmitters = result.particleEmitters.size();
        for (size_t i = 0; i < numParticleEmitters; ++i) {
            auto emitter_externsion = m2.exp2_chunk->emitterExtensions[i];
            result.particleEmitters[i].extension = std::move(emitter_externsion);
        }
    }

    if (m2.txid_chunk) {
        result.texture_ids = m2.txid_chunk->textureIds;
    }

    if (m2.ldv1_chunk) {
        result.lodProfile = *m2.ldv1_chunk;
    }

    // Merge remaining chunk data into Model
    if (m2.txac_chunk) result.textureCombinerHints = std::move(m2.txac_chunk->entries);
    if (m2.pabc_chunk) result.parentSequenceReplacements = std::move(m2.pabc_chunk->replacementParentSequenceLookups);
    if (m2.padc_chunk) result.parentTextureWeights = std::move(m2.padc_chunk->textureWeights);
    if (m2.psbc_chunk) result.parentSequenceBounds = std::move(m2.psbc_chunk->parentSequenceBounds);
    if (m2.pedc_chunk) result.parentEventData = std::move(m2.pedc_chunk->parentEventData);
    if (m2.rpid_chunk) {
        result.recursiveParticleModelIds.reserve(m2.rpid_chunk->recursiveParticleModels.size());
        for (const auto& e : m2.rpid_chunk->recursiveParticleModels)
            result.recursiveParticleModelIds.push_back(e.fileDataId);
    }
    if (m2.gpid_chunk) {
        result.geometryParticleModelIds.reserve(m2.gpid_chunk->geometryParticleModels.size());
        for (const auto& e : m2.gpid_chunk->geometryParticleModels)
            result.geometryParticleModelIds.push_back(e.fileDataId);
    }
    if (m2.wfv3_chunk) result.waterData = m2.wfv3_chunk->data;
    if (m2.pgd1_chunk) result.particleGeosets = std::move(m2.pgd1_chunk->particleGeosetData);
    if (m2.pfdc_chunk) result.physicsFileData = std::move(m2.pfdc_chunk->physicsData);
    if (m2.edgf_chunk) result.edgeFadeEntries = std::move(m2.edgf_chunk->entries);
    if (m2.nerf_chunk) result.nerfEntries = std::move(m2.nerf_chunk->entries);
    if (m2.detl_chunk) result.detailedLightEntries = std::move(m2.detl_chunk->records);
    if (m2.dboc_chunk) result.debugOcclusionEntries = std::move(m2.dboc_chunk->entries);
    if (m2.afra_chunk) result.animFrameData = std::move(m2.afra_chunk->data);
    if (m2.pcol_chunk) {
        PhysicsCollision pc;
        pc.vertexPositions = std::move(m2.pcol_chunk->vertexPositions);
        pc.faceNormals = std::move(m2.pcol_chunk->faceNormals);
        pc.indices = std::move(m2.pcol_chunk->indices);
        pc.flags = std::move(m2.pcol_chunk->flags);
        result.physicsCollision = std::move(pc);
    }
    if (m2.dpiv_chunk) result.dpivData = m2.dpiv_chunk->data;
    if (m2.texl_chunk) result.texturedLightEntries = std::move(m2.texl_chunk->texturedLights);
}

void Parser::Impl::parseBase(BinaryReader& reader, BaseFile& file, WoWFileSystem* wfs) {
    u32 magic = reader.read<u32>();
    reader.setPosition(0);

    if (magic == MD20_TAG) {
        file.format = Format::ClassicMD20;
        wfs->exploratorySearch();
        BinaryParseVisitor parser(reader, wfs);
        parser.read(file.header);
        return;
    } else if (magic == MD21_TAG) {
        file.format = Format::LegionMD21;
        chunkParser.parseChunkedBase(reader, file, wfs);
        return;
    }
    std::string error = "Invalid M2 magic: expected MD20 or MD21, got '" +
                        std::string(reinterpret_cast<char*>(&magic), 4) + "'";
    if (parseMode == ParseMode::Strict) {
        throw std::runtime_error(error);
    }
    issues.push_back(error);
}

void Parser::Impl::parseSkin(BinaryReader& reader, SkinFile& skinFile, WoWFileSystem* wfs) {
    u32 magic = reader.read<u32>();
    reader.setPosition(0);

    if (magic == SKIN_TAG) {
        skinFile.profile = SkinProfile();
        BinaryParseVisitor parser(reader, wfs);
        parser.setVersion(skinFile.version);
        parser.read(skinFile.profile);
        return;
    }
    std::string error = "Invalid M2 magic: expected SKIN, got '" +
                        std::string(reinterpret_cast<char*>(&magic), 4) + "'";
    if (parseMode == ParseMode::Strict) {
        throw std::runtime_error(error);
    }
    issues.push_back(error);
}

}
}
