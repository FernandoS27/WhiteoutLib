
#include <cassert>
#include <cstring>
#include <fstream>
#include <utility>
#include <whiteout/models/m2/bone_file.h>
#include <whiteout/models/m2/phys_file.h>
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
    using AnimDataBuffers = std::deque<BinaryWriterVisitor::AnimDataBuffer>;

    WriteOptions m_options;
    std::vector<std::string> m_issues;

    explicit Impl(WriteOptions options) : m_options(std::move(options)) {}

    /// True when the target keeps its skin profiles inside the `.m2` — every
    /// client before WotLK.
    bool inlineSkins() const {
        return m_options.m2Version < M2_VERSION_WOTLK;
    }

    /// True when the target streams external-flagged sequences from `.anim`
    /// siblings; earlier versions carry everything in the model.
    bool splitsAnimFiles() const {
        return m_options.m2Version >= M2_VERSION_WOTLK;
    }

    void validateOptions();

    /// The sequences whose keys the serialized base routes into `.anim`
    /// buffers, in the order the visitor creates those buffers.
    static std::vector<std::pair<u16, u16>> externalSequences(const std::vector<Sequence>& seqs);

    /// A retail model (GlobalFlag::UpgradedFormat) stores each `.anim` sibling
    /// as a chunked file — loadSequence unwraps its AFM2 chunk before reading
    /// keys — so the raw key buffers gain that header on the way out.
    static void wrapAnimBuffers(const Model& model, AnimDataBuffers& buffers) {
        if (!hasFlag(model.globalFlags.value, GlobalFlag::UpgradedFormat)) {
            return;
        }
        for (auto& buffer : buffers) {
            const u32 size = static_cast<u32>(buffer.data.size());
            u8 header[8];
            std::memcpy(header, &AFM2_TAG, sizeof(u32));
            std::memcpy(header + 4, &size, sizeof(u32));
            buffer.data.insert(buffer.data.begin(), header, header + 8);
        }
    }

    void decomposeBaseFile(BaseFile& base, std::vector<SkinFile>& skins,
                           std::optional<SkeletonFile>& skeleton);

    EXP2Chunk buildEXP2FromModel(const Model& model);

    BaseFile wrapModel(const Model& model) const;

    std::vector<u8> serializeBase(const BaseFile& base, AnimDataBuffers* animOut = nullptr);
    std::vector<u8> serializeSkin(const SkinFile& skin);
    std::vector<u8> serializeSkeleton(const SkeletonFile& skel, AnimDataBuffers* animOut = nullptr);

    void writeBase(BinaryWriter& writer, const BaseFile& model, AnimDataBuffers* animOut = nullptr);
    void writeSkin(BinaryWriter& writer, const SkinFile& model);
    void writeChunkedBase(BinaryWriter& writer, const BaseFile& model,
                          AnimDataBuffers* animOut = nullptr);
    void writeChunkedSkeleton(BinaryWriter& writer, const SkeletonFile& model,
                              AnimDataBuffers* animOut = nullptr);
    void writeChunkedAnim(BinaryWriter& writer, const AnimFile& model);

    void writeViaWfs(WoWFileSystem& wfs, const BaseFile& base, const std::vector<SkinFile>& skins,
                     const std::optional<SkeletonFile>& skeleton);
};

Writer::Writer(WriteOptions options) : pImpl(std::make_unique<Impl>(std::move(options))) {}

Writer::~Writer() = default;

bool Writer::hasIssues() const {
    return !pImpl->m_issues.empty();
}

const std::vector<std::string>& Writer::getIssues() const {
    return pImpl->m_issues;
}

void Writer::write(interfaces::VirtualPathFileSystem& fs, const std::string& filePath,
                   const Model& model) {
    pImpl->m_issues.clear();
    pImpl->validateOptions();

    BaseFile base = pImpl->wrapModel(model);
    std::vector<SkinFile> skins;
    std::optional<SkeletonFile> skeleton;
    pImpl->decomposeBaseFile(base, skins, skeleton);

    WoWFileSystem wfs(fs, filePath, WoWFileSystemMode::Create);
    pImpl->writeViaWfs(wfs, base, skins, skeleton);
}

void Writer::write(interfaces::CascFileSystem& cascFs, const Model& model) {
    pImpl->m_issues.clear();
    pImpl->validateOptions();

    BaseFile base = pImpl->wrapModel(model);
    std::vector<SkinFile> skins;
    std::optional<SkeletonFile> skeleton;
    pImpl->decomposeBaseFile(base, skins, skeleton);

    WoWFileSystem wfs(cascFs, WoWFileSystemMode::Create);
    pImpl->writeViaWfs(wfs, base, skins, skeleton);
}

namespace {

/// Find the byte offset of a chunk's data region within a chunked M2 buffer.
/// The chunk layout is: [FourCC:4][size:4][data:size]. Returns the offset of
/// the first byte of 'data', or SIZE_MAX if the tag is not found.
size_t findChunkDataOffset(const std::vector<u8>& buffer, u32 tag) {
    if (buffer.size() < 8)
        return SIZE_MAX;
    for (size_t pos = 0; pos + 8 <= buffer.size();) {
        u32 chunkTag;
        u32 chunkSize;
        std::memcpy(&chunkTag, buffer.data() + pos, 4);
        std::memcpy(&chunkSize, buffer.data() + pos + 4, 4);
        if (chunkTag == tag)
            return pos + 8;
        size_t const next = pos + 8 + chunkSize;
        if (next <= pos)
            return SIZE_MAX;
        pos = next;
    }
    return SIZE_MAX;
}

} // anonymous namespace

M2SerializeResult Writer::write(const Model& model) {
    pImpl->m_issues.clear();
    pImpl->validateOptions();

    BaseFile base = pImpl->wrapModel(model);
    std::vector<SkinFile> skins;
    std::optional<SkeletonFile> skeleton;
    pImpl->decomposeBaseFile(base, skins, skeleton);

    M2SerializeResult result;

    u32 numBaseSkins = 0;
    if (!pImpl->inlineSkins()) {
        SFIDChunk sfid;
        for (const auto& s : skins) {
            if (s.isLodSkin) {
                sfid.lodSkinFileDataIds.push_back(0);
            } else {
                sfid.skinFileDataIds.push_back(0);
                ++numBaseSkins;
            }
        }
        base.sfid_chunk = std::move(sfid);
    }

    // The AFID slots have to exist in whichever file is serialized below, so
    // the external sequence list is derived up front; the visitor fills the
    // matching buffers in the same order while serializing.
    const auto externals = pImpl->splitsAnimFiles()
                               ? Impl::externalSequences(skeleton && skeleton->sks1_chunk
                                                             ? skeleton->sks1_chunk->sequences
                                                             : base.header.model.sequences)
                               : std::vector<std::pair<u16, u16>>{};
    if (!externals.empty() && base.format == Format::LegionMD21) {
        AFIDChunk afid;
        for (const auto& [animId, subAnimId] : externals) {
            afid.animFileIds.push_back(AFIDEntry{animId, subAnimId, 0});
        }
        if (skeleton) {
            skeleton->afid_chunk = std::move(afid);
        } else {
            base.afid_chunk = std::move(afid);
        }
    }

    Impl::AnimDataBuffers animBuffers;
    if (skeleton) {
        M2SerializeResult::SkeletonFileEntry skelEntry;
        skelEntry.data = pImpl->serializeSkeleton(skeleton.value(), &animBuffers);
        result.skeletonData = std::move(skelEntry);
        SKIDChunk skid;
        skid.skeletonFileDataId = 0;
        base.skid_chunk = skid;
    }

    result.m2Data = pImpl->serializeBase(base, skeleton ? nullptr : &animBuffers);

    Impl::wrapAnimBuffers(base.header.model, animBuffers);
    for (auto& buffer : animBuffers) {
        M2SerializeResult::AnimDataEntry entry;
        entry.animId = static_cast<u16>(buffer.anim_id);
        entry.subAnimId = static_cast<u16>(buffer.sub_anim_id);
        entry.data = std::move(buffer.data);
        result.animData.push_back(std::move(entry));
    }

    if (base.pfid_chunk && base.header.model.physics) {
        M2SerializeResult::PhysicsFileEntry entry;
        entry.fileDataId = base.pfid_chunk->physFileDataId;
        entry.data = writePhysics(*base.header.model.physics);
        result.physicsData = std::move(entry);
    }

    for (const auto& skinFile : skins) {
        M2SerializeResult::SkinFileEntry entry;
        entry.data = pImpl->serializeSkin(skinFile);
        if (skinFile.isLodSkin) {
            result.skinlodData.push_back(std::move(entry));
        } else {
            result.skinData.push_back(std::move(entry));
        }
    }

    // ── Populate pathOffset fields ──────────────────────────────────────────
    // For MD21 (chunked) format, find the byte offsets within the serialized
    // buffers where each file-data-ID u32 lives so callers can patch them.

    if (base.format == Format::LegionMD21) {
        // SFID chunk layout: [skinFileDataIds: u32 × N][lodSkinFileDataIds: u32 × M]
        size_t const sfidData = findChunkDataOffset(result.m2Data, SFID_TAG);
        if (sfidData != SIZE_MAX) {
            for (u32 i = 0; i < result.skinData.size(); ++i) {
                result.skinData[i].pathOffset = sfidData + i * sizeof(u32);
            }
            size_t const lodBase = sfidData + numBaseSkins * sizeof(u32);
            for (u32 i = 0; i < result.skinlodData.size(); ++i) {
                result.skinlodData[i].pathOffset = lodBase + i * sizeof(u32);
            }
        }

        // PFID chunk layout: [physFileDataId: u32]
        if (result.physicsData.has_value()) {
            size_t const pfidData = findChunkDataOffset(result.m2Data, PFID_TAG);
            if (pfidData != SIZE_MAX) {
                result.physicsData->pathOffset = pfidData;
            }
        }

        // SKID chunk layout: [skeletonFileDataId: u32]
        if (result.skeletonData.has_value()) {
            size_t const skidData = findChunkDataOffset(result.m2Data, SKID_TAG);
            if (skidData != SIZE_MAX) {
                result.skeletonData->pathOffset = skidData;
            }
        }

        // AFID chunk layout: [AFIDEntry × N] where each entry is {u16 animId, u16 subAnimId, u32
        // fileDataId} AFID can live in m2Data or (when skeleton is emitted) in skeletonData.
        std::vector<u8> const& afidBuffer =
            result.skeletonData.has_value() ? result.skeletonData->data : result.m2Data;
        size_t const afidData = findChunkDataOffset(afidBuffer, AFID_TAG);
        if (afidData != SIZE_MAX) {
            constexpr size_t kAFIDEntrySize = sizeof(u16) + sizeof(u16) + sizeof(u32); // 8
            constexpr size_t kFileDataIdOffset = sizeof(u16) + sizeof(u16);            // 4
            for (size_t i = 0; i < result.animData.size(); ++i) {
                result.animData[i].pathOffset = afidData + i * kAFIDEntrySize + kFileDataIdOffset;
            }
        }
    }

    return result;
}

void Writer::Impl::validateOptions() {
    if (m_options.format == Format::LegionMD21 && m_options.m2Version < M2_VERSION_LEGION) {
        m_issues.push_back("MD21 (chunked) output requires version >= 272; version " +
                           std::to_string(m_options.m2Version) + " belongs in a plain MD20 file");
    }
    if (m_options.format == Format::ClassicMD20 && m_options.m2Version > M2_VERSION_LEGION) {
        m_issues.push_back("version " + std::to_string(m_options.m2Version) +
                           " models are chunked (MD21); writing plain MD20 anyway");
    }
}

std::vector<std::pair<u16, u16>> Writer::Impl::externalSequences(
    const std::vector<Sequence>& seqs) {
    std::vector<std::pair<u16, u16>> out;
    for (const auto& seq : seqs) {
        if ((static_cast<u32>(seq.flags) & 0x130u) == 0) {
            out.emplace_back(seq.id, seq.variationIndex);
        }
    }
    return out;
}

void Writer::Impl::decomposeBaseFile(BaseFile& base, std::vector<SkinFile>& skins,
                                     std::optional<SkeletonFile>& skeleton) {
    const auto& model = base.header.model;

    if (inlineSkins()) {
        // ≤TBC keeps its skin profiles ("views") inside the .m2; nothing to
        // split off, and the LOD list has nowhere to go.
        if (!model.lodProfiles.empty()) {
            m_issues.push_back("version < 264 cannot carry LOD skin profiles; dropping " +
                               std::to_string(model.lodProfiles.size()));
            base.header.model.lodProfiles.clear();
        }
        base.header.model.numSkinProfiles = static_cast<u32>(model.skinProfiles.size());
        if (m_options.emitSkeleton) {
            m_issues.push_back("emitSkeleton requires MD21 format; skeleton will be inline");
        }
        return;
    }

    u32 const numSkinProfiles = static_cast<u32>(model.skinProfiles.size());
    for (size_t i = 0; i < model.skinProfiles.size(); ++i) {
        SkinFile sf;
        sf.version = base.header.version;
        sf.profile = model.skinProfiles[i];
        sf.isLodSkin = false;
        sf.index = static_cast<int>(i);
        skins.push_back(std::move(sf));
    }
    for (size_t i = 0; i < model.lodProfiles.size(); ++i) {
        SkinFile sf;
        sf.version = base.header.version;
        sf.profile = model.lodProfiles[i];
        sf.isLodSkin = true;
        sf.lodLevel = static_cast<int>(i);
        skins.push_back(std::move(sf));
    }

    // `model` aliases base.header.model, so the count has to be taken before
    // the vectors are emptied.
    base.header.model.skinProfiles.clear();
    base.header.model.lodProfiles.clear();
    base.header.model.numSkinProfiles = numSkinProfiles;

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

BaseFile Writer::Impl::wrapModel(const Model& model) const {
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
        for (u32 const id : model.recursiveParticleModelIds)
            rpid.recursiveParticleModels.push_back({id});
        base.rpid_chunk = std::move(rpid);
    }
    if (!model.geometryParticleModelIds.empty()) {
        GPIDChunk gpid;
        for (u32 const id : model.geometryParticleModelIds)
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
    // A model that named its physics by file id keeps doing so — the payload
    // goes back out as a `.phys` of its own. Only an inline one becomes PFDC.
    if (model.physicsFileId) {
        PFIDChunk pfid;
        pfid.physFileDataId = *model.physicsFileId;
        base.pfid_chunk = pfid;
    } else if (model.physics) {
        PFDCChunk pfdc;
        pfdc.physics = *model.physics;
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

std::vector<u8> Writer::Impl::serializeBase(const BaseFile& base, AnimDataBuffers* animOut) {
    std::vector<u8> buffer;
    buffer.reserve(2 * 1024 * 1024);
    common::vector_streambuf streambuf(buffer);
    std::ostream out(&streambuf);
    BinaryWriter writer(out);

    writeBase(writer, base, animOut);

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

std::vector<u8> Writer::Impl::serializeSkeleton(const SkeletonFile& skel,
                                                AnimDataBuffers* animOut) {
    std::vector<u8> buffer;
    buffer.reserve(256 * 1024);
    common::vector_streambuf streambuf(buffer);
    std::ostream out(&streambuf);
    BinaryWriter writer(out);

    writeChunkedSkeleton(writer, skel, animOut);

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

    // External-flagged sequences get their `.anim` siblings registered before
    // anything serializes, so an AFID chunk can carry real handles and the
    // buffers produced during serialization land in files 1:1.
    std::vector<std::pair<u16, u16>> externals;
    std::vector<u32> animHandles;
    if (splitsAnimFiles()) {
        externals =
            externalSequences(skeleton && skeleton->sks1_chunk ? skeleton->sks1_chunk->sequences
                                                               : base.header.model.sequences);
        for (const auto& [animId, subAnimId] : externals) {
            animHandles.push_back(wfs.newAnimFileEntry(animId, subAnimId));
        }
    }

    SFIDChunk const sfid = wfs.buildSFIDChunk();
    SKIDChunk const skid = wfs.buildSKIDChunk();
    AFIDChunk const afid = wfs.buildAFIDChunk();

    AnimDataBuffers animBuffers;
    std::optional<SkeletonFile> skeletonCopy;
    if (skeleton) {
        skeletonCopy = skeleton;
        if (!afid.animFileIds.empty()) {
            skeletonCopy->afid_chunk = afid;
        }
    }

    {
        BaseFile baseCopy = base;
        if (!inlineSkins()) {
            baseCopy.sfid_chunk = sfid;
        }
        if (skid.skeletonFileDataId != 0) {
            baseCopy.skid_chunk = skid;
        }
        if (!skeleton && base.format == Format::LegionMD21 && !afid.animFileIds.empty()) {
            baseCopy.afid_chunk = afid;
        }
        wfs.setM2Base(serializeBase(baseCopy, skeleton ? nullptr : &animBuffers));
    }

    if (skeletonCopy) {
        wfs.writeSkeletonFile(skelHandle, serializeSkeleton(skeletonCopy.value(), &animBuffers));
    }

    wrapAnimBuffers(base.header.model, animBuffers);
    for (size_t i = 0; i < animBuffers.size() && i < animHandles.size(); ++i) {
        wfs.writeAnimFile(animHandles[i], std::move(animBuffers[i].data));
    }

    // Written as a sibling file whenever it is not inline: always for a plain
    // MD20, and for a chunked model that carries PFID instead of PFDC.
    const auto& sourceModel = base.header.model;
    if (sourceModel.physics && (base.format == Format::ClassicMD20 || base.pfid_chunk)) {
        wfs.writePhysicsFile(writePhysics(*sourceModel.physics),
                             sourceModel.physicsFileId.value_or(0));
    }

    // `.bone` siblings are always separate files; BFID rides along on whichever
    // of the `.m2` or `.skel` carried it, untouched.
    for (u32 i = 0; i < sourceModel.boneOverrides.size(); ++i) {
        wfs.writeBoneFile(i, writeBoneOverrides(sourceModel.boneOverrides[i]));
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

void Writer::Impl::writeBase(BinaryWriter& writer, const BaseFile& model,
                             AnimDataBuffers* animOut) {
    switch (model.format) {
    case Format::ClassicMD20: {
        BinaryWriterVisitor visitor(writer);
        visitor.write(model.header);
        if (animOut) {
            *animOut = std::move(visitor.getAnimDataBuffers());
        }
        break;
    }
    case Format::LegionMD21:
        writeChunkedBase(writer, model, animOut);
        break;
    case Format::Invalid:
        break;
    }
}

void Writer::Impl::writeSkin(BinaryWriter& writer, const SkinFile& model) {
    BinaryWriterVisitor visitor(writer);
    // The visitor writes the SKIN magic itself for standalone-profile
    // versions; it needs the version for that and for the section layout.
    visitor.setVersion(model.version != 0 ? model.version : m_options.m2Version);
    visitor.write(model.profile);
}

void Writer::Impl::writeChunkedBase(BinaryWriter& writer, const BaseFile& model,
                                    AnimDataBuffers* animOut) {
    const auto write_chunk = ([&writer]<typename T>(u32 tag, const T& header) {
        writer.write(tag);
        u32 const sizePos = writer.getPosition();
        writer.write<u32>(0);
        u32 const chunkStart = writer.getPosition();

        BinaryWriterVisitor visitor(writer);
        visitor.write(header);

        u32 const chunkEnd = writer.getPosition();
        u32 const chunkSize = chunkEnd - chunkStart;

        writer.setPosition(sizePos);
        writer.write(chunkSize);

        writer.setPosition(chunkEnd);
    });

    // The MD21 chunk gets a dedicated visitor: it is the one whose sequence
    // list routes key data into `.anim` buffers, which the caller flushes.
    // Later chunks that hold tracks (PADC/PEDC) reference the parent model's
    // sequences, not this one's, so their fresh visitors keep data inline.
    {
        writer.write(MD21_TAG);
        u32 const sizePos = writer.getPosition();
        writer.write<u32>(0);
        u32 const chunkStart = writer.getPosition();

        BinaryWriterVisitor visitor(writer);
        visitor.write(model.header);
        if (animOut) {
            *animOut = std::move(visitor.getAnimDataBuffers());
        }

        u32 const chunkEnd = writer.getPosition();
        writer.setPosition(sizePos);
        writer.write(chunkEnd - chunkStart);
        writer.setPosition(chunkEnd);
    }
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
        const auto payload = writePhysics(model.pfdc_chunk->physics);
        // The chunk size covers zero padding out to a 16-byte multiple. Every
        // PFDC in the corpus is padded that way, including the ones already
        // aligned, which carry none.
        u32 const padded = (static_cast<u32>(payload.size()) + 15u) & ~15u;
        writer.write(PFDC_TAG);
        writer.write<u32>(padded);
        writer.write(payload);
        writer.writePadding(padded - static_cast<u32>(payload.size()));
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

void Writer::Impl::writeChunkedSkeleton(BinaryWriter& writer, const SkeletonFile& model,
                                        AnimDataBuffers* animOut) {
    // One visitor across the chunks: the sequence list SKS1 writes decides,
    // per sequence, where SKA1/SKB1 track keys go, so it must be visited
    // before them by the same visitor. Chunk order is free — readers dispatch
    // on the tag.
    BinaryWriterVisitor visitor(writer);

    const auto write_chunk = ([&writer, &visitor]<typename T>(u32 tag, const T& chunk) {
        writer.write(tag);
        u32 const sizePos = writer.getPosition();
        writer.write<u32>(0);
        u32 const chunkStart = writer.getPosition();

        visitor.write(chunk);

        u32 const chunkEnd = writer.getPosition();
        u32 const chunkSize = chunkEnd - chunkStart;

        writer.setPosition(sizePos);
        writer.write(chunkSize);

        writer.setPosition(chunkEnd);
    });

    if (model.skl1_chunk) {
        write_chunk(SKL1_TAG, model.skl1_chunk.value());
    }
    if (model.sks1_chunk) {
        write_chunk(SKS1_TAG, model.sks1_chunk.value());
    }
    if (model.ska1_chunk) {
        write_chunk(SKA1_TAG, model.ska1_chunk.value());
    }
    if (model.skb1_chunk) {
        write_chunk(SKB1_TAG, model.skb1_chunk.value());
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

    if (animOut) {
        *animOut = std::move(visitor.getAnimDataBuffers());
    }
}

void Writer::Impl::writeChunkedAnim(BinaryWriter& writer, const AnimFile& model) {
    if (model.profile.isChunked) {

        const auto write_chunk = ([&writer]<typename T>(u32 tag, const T& chunk) {
            writer.write(tag);
            u32 const sizePos = writer.getPosition();
            writer.write<u32>(0);
            u32 const chunkStart = writer.getPosition();

            BinaryWriterVisitor visitor(writer);
            visitor.write(chunk);

            u32 const chunkEnd = writer.getPosition();
            u32 const chunkSize = chunkEnd - chunkStart;

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

} // namespace m2
} // namespace whiteout
