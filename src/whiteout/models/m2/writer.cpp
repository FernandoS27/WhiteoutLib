
#include <cassert>
#include <fstream>
#include <whiteout/models/m2/writer.h>
#include "../../common/binary_writer.h"
#include "../../common/streams.h"
#include "binary_writer_visitor.h"
#include "file_system.h"
#include "wow_file_system.h"

namespace whiteout {
namespace m2 {

using common::BinaryWriter;

class Writer::Impl {
public:
    WriteOptions m_options;
    std::vector<std::string> m_issues;

    explicit Impl(WriteOptions options) : m_options(std::move(options)) {}

    void decomposeBaseFile(BaseFile& base, std::vector<SkinFile>& skins,
                           std::optional<SkeletonFile>& skeleton);

    EXP2Chunk buildEXP2FromModel(const Model& model);

    BaseFile wrapModel(const Model& model);

    std::vector<u8> serializeBase(const BaseFile& base);
    std::vector<u8> serializeSkin(const SkinFile& skin);
    std::vector<u8> serializeSkeleton(const SkeletonFile& skel);

    void writeBase(BinaryWriter& writer, const BaseFile& model);
    void writeSkin(BinaryWriter& writer, const SkinFile& model);
    void writeChunkedBase(BinaryWriter& writer, const BaseFile& model);
    void writeChunkedSkeleton(BinaryWriter& writer, const SkeletonFile& model);
    void writeChunkedBone(BinaryWriter& writer, const BoneFile& model);
    void writeChunkedAnim(BinaryWriter& writer, const AnimFile& model);

    void writeViaWfs(WoWFileSystem& wfs, const BaseFile& base,
                     const std::vector<SkinFile>& skins,
                     const std::optional<SkeletonFile>& skeleton);
};

Writer::Writer(WriteOptions options) : pImpl(std::make_unique<Impl>(std::move(options))) {}

Writer::~Writer() = default;

bool Writer::hasIssues() const { return !pImpl->m_issues.empty(); }

const std::vector<std::string>& Writer::getIssues() const { return pImpl->m_issues; }

void Writer::write(interfaces::VirtualPathFileSystem& fs, const std::string& filePath,
                   const Model& model) {
    pImpl->m_issues.clear();

    BaseFile base = pImpl->wrapModel(model);
    std::vector<SkinFile> skins;
    std::optional<SkeletonFile> skeleton;
    pImpl->decomposeBaseFile(base, skins, skeleton);

    WoWFileSystem wfs(fs, filePath, WoWFileSystemMode::Create);
    pImpl->writeViaWfs(wfs, base, skins, skeleton);
}

void Writer::write(interfaces::CascFileSystem& cascFs, const Model& model) {
    pImpl->m_issues.clear();

    BaseFile base = pImpl->wrapModel(model);
    std::vector<SkinFile> skins;
    std::optional<SkeletonFile> skeleton;
    pImpl->decomposeBaseFile(base, skins, skeleton);

    WoWFileSystem wfs(cascFs, WoWFileSystemMode::Create);
    pImpl->writeViaWfs(wfs, base, skins, skeleton);
}

M2SerializeResult Writer::write(const Model& model) {
    pImpl->m_issues.clear();

    BaseFile base = pImpl->wrapModel(model);
    std::vector<SkinFile> skins;
    std::optional<SkeletonFile> skeleton;
    pImpl->decomposeBaseFile(base, skins, skeleton);

    M2SerializeResult result;

    {
        SFIDChunk sfid;
        for (const auto& s : skins) {
            if (s.isLodSkin) {
                sfid.lodSkinFileDataIds.push_back(0);
            } else {
                sfid.skinFileDataIds.push_back(0);
            }
        }
        base.sfid_chunk = std::move(sfid);
    }

    if (skeleton) {
        M2SerializeResult::SkeletonFileEntry skelEntry;
        skelEntry.data = pImpl->serializeSkeleton(skeleton.value());
        result.skeletonData = std::move(skelEntry);
        SKIDChunk skid;
        skid.skeletonFileDataId = 0;
        base.skid_chunk = skid;
    }

    result.m2Data = pImpl->serializeBase(base);

    for (const auto& skinFile : skins) {
        M2SerializeResult::SkinFileEntry entry;
        entry.data = pImpl->serializeSkin(skinFile);
        if (skinFile.isLodSkin) {
            result.skinlodData.push_back(std::move(entry));
        } else {
            result.skinData.push_back(std::move(entry));
        }
    }

    return result;
}

void Writer::Impl::decomposeBaseFile(BaseFile& base, std::vector<SkinFile>& skins,
                                      std::optional<SkeletonFile>& skeleton) {
    const auto& model = base.header.model;

    for (size_t i = 0; i < model.skinProfiles.size(); ++i) {
        SkinFile sf;
        sf.profile = model.skinProfiles[i];
        sf.isLodSkin = false;
        sf.index = static_cast<int>(i);
        skins.push_back(std::move(sf));
    }
    for (size_t i = 0; i < model.lodProfiles.size(); ++i) {
        SkinFile sf;
        sf.profile = model.lodProfiles[i];
        sf.isLodSkin = true;
        sf.lodLevel = static_cast<int>(i);
        skins.push_back(std::move(sf));
    }

    base.header.model.skinProfiles.clear();
    base.header.model.lodProfiles.clear();
    base.header.model.numSkinProfiles = static_cast<u32>(model.skinProfiles.size());

    if (m_options.emitSkeleton) {
        if (base.format != Format::LegionMD21) {
            m_issues.push_back("emitSkeleton requires MD21 format; skeleton will be inline");
        } else {
            SkeletonFile skel;

            if (!model.bones.empty()) {
                SKB1Chunk skb1;
                skb1.bones = model.bones;
                skb1.keyBoneLookup = model.keyBoneIds;
                skel.skb1_chunk = std::move(skb1);

                base.header.model.bones.clear();
                base.header.model.keyBoneIds.clear();
            }

            if (!model.sequences.empty() || !model.globalLoops.empty()) {
                SKS1Chunk sks1;
                sks1.globalLoops = model.globalLoops;
                sks1.sequences = model.sequences;
                sks1.sequenceLookups = model.sequenceIdxHashById;
                skel.sks1_chunk = std::move(sks1);

                base.header.model.globalLoops.clear();
                base.header.model.sequences.clear();
                base.header.model.sequenceIdxHashById.clear();
            }

            if (!model.attachments.empty()) {
                SKA1Chunk ska1;
                ska1.attachments = model.attachments;
                ska1.attachmentLookupTable = model.attachmentIndicesById;
                skel.ska1_chunk = std::move(ska1);

                base.header.model.attachments.clear();
                base.header.model.attachmentIndicesById.clear();
            }

            SKL1Chunk skl1;
            skl1.flags = 0x100;
            skl1.name = m_options.baseStem.empty() ? model.modelName : m_options.baseStem;
            skel.skl1_chunk = std::move(skl1);

            skeleton = std::move(skel);
        }
    }

    if (!base.exp2_chunk) {
        const auto& emitters = base.header.model.particleEmitters;
        bool hasExtensions = false;
        for (const auto& e : emitters) {
            if (e.extension) {
                hasExtensions = true;
                break;
            }
        }
        if (hasExtensions) {
            base.exp2_chunk = buildEXP2FromModel(base.header.model);
        }
    }

    base.expt_chunk = std::nullopt;
}

BaseFile Writer::Impl::wrapModel(const Model& model) {
    BaseFile base;
    base.format = m_options.format;
    base.header.magic = MD20_TAG;
    base.header.version = m_options.m2Version;
    base.header.model = model;

    // Populate chunk fields from Model (reverse of Parser::Impl::parse merge)
    if (!model.texture_ids.empty()) {
        TXIDChunk txid;
        txid.textureIds = model.texture_ids;
        base.txid_chunk = std::move(txid);
    }

    if (model.lodProfile) {
        base.ldv1_chunk = *model.lodProfile;
    }

    if (!model.textureCombinerHints.empty()) {
        TXACChunk txac;
        txac.entries = model.textureCombinerHints;
        base.txac_chunk = std::move(txac);
    }
    if (!model.parentSequenceReplacements.empty()) {
        PABCChunk pabc;
        pabc.replacementParentSequenceLookups = model.parentSequenceReplacements;
        base.pabc_chunk = std::move(pabc);
    }
    if (!model.parentTextureWeights.empty()) {
        PADCChunk padc;
        padc.textureWeights = model.parentTextureWeights;
        base.padc_chunk = std::move(padc);
    }
    if (!model.parentSequenceBounds.empty()) {
        PSBCChunk psbc;
        psbc.parentSequenceBounds = model.parentSequenceBounds;
        base.psbc_chunk = std::move(psbc);
    }
    if (!model.parentEventData.empty()) {
        PEDCChunk pedc;
        pedc.parentEventData = model.parentEventData;
        base.pedc_chunk = std::move(pedc);
    }
    if (!model.recursiveParticleModelIds.empty()) {
        M2RPIDChunk rpid;
        for (u32 id : model.recursiveParticleModelIds)
            rpid.recursiveParticleModels.push_back({id});
        base.rpid_chunk = std::move(rpid);
    }
    if (!model.geometryParticleModelIds.empty()) {
        GPIDChunk gpid;
        for (u32 id : model.geometryParticleModelIds)
            gpid.geometryParticleModels.push_back({id});
        base.gpid_chunk = std::move(gpid);
    }
    if (model.waterData) {
        WFV3Chunk wfv3;
        wfv3.data = *model.waterData;
        base.wfv3_chunk = std::move(wfv3);
    }
    if (!model.particleGeosets.empty()) {
        PGD1Chunk pgd1;
        pgd1.particleGeosetData = model.particleGeosets;
        base.pgd1_chunk = std::move(pgd1);
    }
    if (!model.physicsFileData.empty()) {
        PFDCChunk pfdc;
        pfdc.physicsData = model.physicsFileData;
        base.pfdc_chunk = std::move(pfdc);
    }
    if (!model.edgeFadeEntries.empty()) {
        EDGFChunk edgf;
        edgf.entries = model.edgeFadeEntries;
        base.edgf_chunk = std::move(edgf);
    }
    if (!model.nerfEntries.empty()) {
        NERFChunk nerf;
        nerf.entries = model.nerfEntries;
        base.nerf_chunk = std::move(nerf);
    }
    if (!model.detailedLightEntries.empty()) {
        DETLChunk detl;
        detl.records = model.detailedLightEntries;
        base.detl_chunk = std::move(detl);
    }
    if (!model.debugOcclusionEntries.empty()) {
        DBOCChunk dboc;
        dboc.entries = model.debugOcclusionEntries;
        base.dboc_chunk = std::move(dboc);
    }
    if (!model.animFrameData.empty()) {
        AFRAChunk afra;
        afra.data = model.animFrameData;
        base.afra_chunk = std::move(afra);
    }
    if (model.physicsCollision) {
        PCOLChunk pcol;
        pcol.vertexPositions = model.physicsCollision->vertexPositions;
        pcol.faceNormals = model.physicsCollision->faceNormals;
        pcol.indices = model.physicsCollision->indices;
        pcol.flags = model.physicsCollision->flags;
        base.pcol_chunk = std::move(pcol);
    }
    if (model.dpivData) {
        DPIVChunk dpiv;
        dpiv.data = *model.dpivData;
        base.dpiv_chunk = std::move(dpiv);
    }
    if (!model.texturedLightEntries.empty()) {
        TEXLChunk texl;
        texl.texturedLights = model.texturedLightEntries;
        base.texl_chunk = std::move(texl);
    }

    return base;
}

EXP2Chunk Writer::Impl::buildEXP2FromModel(const Model& model) {
    EXP2Chunk exp2;
    exp2.emitterExtensions.reserve(model.particleEmitters.size());
    for (const auto& emitter : model.particleEmitters) {
        if (emitter.extension) {
            exp2.emitterExtensions.push_back(*emitter.extension);
        }
    }
    return exp2;
}

std::vector<u8> Writer::Impl::serializeBase(const BaseFile& base) {
    std::vector<u8> buffer;
    buffer.reserve(2 * 1024 * 1024);
    common::vector_streambuf streambuf(buffer);
    std::ostream out(&streambuf);
    BinaryWriter writer(out);

    writeBase(writer, base);

    buffer.shrink_to_fit();
    return buffer;
}

std::vector<u8> Writer::Impl::serializeSkin(const SkinFile& skin) {
    std::vector<u8> buffer;
    buffer.reserve(512 * 1024);
    common::vector_streambuf streambuf(buffer);
    std::ostream out(&streambuf);
    BinaryWriter writer(out);

    writeSkin(writer, skin);

    buffer.shrink_to_fit();
    return buffer;
}

std::vector<u8> Writer::Impl::serializeSkeleton(const SkeletonFile& skel) {
    std::vector<u8> buffer;
    buffer.reserve(256 * 1024);
    common::vector_streambuf streambuf(buffer);
    std::ostream out(&streambuf);
    BinaryWriter writer(out);

    writeChunkedSkeleton(writer, skel);

    buffer.shrink_to_fit();
    return buffer;
}

void Writer::Impl::writeViaWfs(WoWFileSystem& wfs, const BaseFile& base,
                                const std::vector<SkinFile>& skins,
                                const std::optional<SkeletonFile>& skeleton) {
    assert(wfs.mode() == WoWFileSystemMode::Create);

    std::vector<u32> skinHandles;
    std::vector<u32> lodSkinHandles;
    for (const auto& skinFile : skins) {
        if (skinFile.isLodSkin) {
            lodSkinHandles.push_back(wfs.newLodSkinFileEntry());
        } else {
            skinHandles.push_back(wfs.newSkinFileEntry());
        }
    }

    u32 skelHandle = 0;
    if (skeleton) {
        skelHandle = wfs.newSkeletonFileEntry();
    }

    SFIDChunk sfid = wfs.buildSFIDChunk();
    SKIDChunk skid = wfs.buildSKIDChunk();

    {
        BaseFile baseCopy = base;
        baseCopy.sfid_chunk = sfid;
        if (skid.skeletonFileDataId != 0) {
            baseCopy.skid_chunk = skid;
        }
        wfs.setM2Base(serializeBase(baseCopy));
    }

    if (skeleton) {
        wfs.writeSkeletonFile(skelHandle, serializeSkeleton(skeleton.value()));
    }

    {
        u32 skinIdx = 0;
        u32 lodIdx = 0;
        for (const auto& skinFile : skins) {
            auto skinBuf = serializeSkin(skinFile);
            if (skinFile.isLodSkin) {
                wfs.writeSkinFile(lodSkinHandles[lodIdx++], std::move(skinBuf));
            } else {
                wfs.writeSkinFile(skinHandles[skinIdx++], std::move(skinBuf));
            }
        }
    }

    wfs.flush();
}

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
    writer.write<u32>(SKIN_TAG);
    visitor.write(model.profile);
}

void Writer::Impl::writeChunkedBase(BinaryWriter& writer, const BaseFile& model) {
    const auto write_chunk = ([&writer]<typename T>(u32 tag, const T& header) {
        writer.write(tag);
        u32 sizePos = writer.getPosition();
        writer.write<u32>(0);
        u32 chunkStart = writer.getPosition();

        BinaryWriterVisitor visitor(writer);
        visitor.write(header);

        u32 chunkEnd = writer.getPosition();
        u32 chunkSize = chunkEnd - chunkStart;

        writer.setPosition(sizePos);
        writer.write(chunkSize);

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
        writer.write<u32>(0);
        u32 chunkStart = writer.getPosition();

        BinaryWriterVisitor visitor(writer);
        visitor.write(chunk);

        u32 chunkEnd = writer.getPosition();
        u32 chunkSize = chunkEnd - chunkStart;

        writer.setPosition(sizePos);
        writer.write(chunkSize);

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

    BinaryWriterVisitor headerWriter(writer);
    headerWriter.write(model.header);

    const auto write_chunk = ([&writer]<typename T>(u32 tag, const T& chunk) {
        writer.write(tag);
        u32 sizePos = writer.getPosition();
        writer.write<u32>(0);
        u32 chunkStart = writer.getPosition();

        BinaryWriterVisitor visitor(writer);
        visitor.write(chunk);

        u32 chunkEnd = writer.getPosition();
        u32 chunkSize = chunkEnd - chunkStart;

        writer.setPosition(sizePos);
        writer.write(chunkSize);

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

        const auto write_chunk = ([&writer]<typename T>(u32 tag, const T& chunk) {
            writer.write(tag);
            u32 sizePos = writer.getPosition();
            writer.write<u32>(0);
            u32 chunkStart = writer.getPosition();

            BinaryWriterVisitor visitor(writer);
            visitor.write(chunk);

            u32 chunkEnd = writer.getPosition();
            u32 chunkSize = chunkEnd - chunkStart;

            writer.setPosition(sizePos);
            writer.write(chunkSize);

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

        writer.write(model.profile.afm2_chunk->animationData);
    }
}

}
}
