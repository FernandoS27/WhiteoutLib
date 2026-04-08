// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <whiteout/models/mdx/writer.h>
#include "../../common/binary_writer.h"
#include "../../common/streams.h"
#include "mdl_writer.h"

namespace whiteout {
namespace mdx {

using common::BinaryWriter;

namespace {

struct SizeEnclosure {
    u32 startPos;
    u32 sizePos;
    BinaryWriter& writer;

    SizeEnclosure(BinaryWriter& writer, u32 tag = 0) : writer(writer) {

        if (tag != 0) {
            writer.write(tag);
        }
        startPos = writer.getPosition();
        writer.write<u32>(0); // Placeholder for size
        if (tag == 0) {
            sizePos = startPos; // this is inclusive
            return;
        }
        sizePos = writer.getPosition();
    }

    ~SizeEnclosure() {
        u32 endPos = writer.getPosition();
        u32 actualSize = endPos - sizePos;
        writer.setPosition(startPos); // Move back to size field
        writer.write(actualSize);
        writer.setPosition(endPos); // Move back to end
    }
};

} // namespace

template <typename T>
static void writeTrackChunk(BinaryWriter& writer, u32 tag, const Track<T>& track) {
    if (!track.isUsed)
        return; // Skip unused tracks

    // Write track header
    writer.write(tag);
    writer.write(static_cast<u32>(track.keyCount));
    writer.write(static_cast<u32>(track.interpolationType));
    writer.write(track.globalSequenceId);

    // Write track data
    writer.write(track.keys_data);
}

// ============================================================================
// WriterImpl - Implementation class using PImpl idiom
// ============================================================================

class Writer::Impl {
public:
    struct ChunkHeader {
        u32 tag;  ///< Chunk identifier
        u32 size; ///< Chunk data size in bytes
    };

    void writeChunkHeader(BinaryWriter& writer, u32 tag, u32 size);

    void write(BinaryWriter& writer, const Model& mdx);

    // Chunk writers - each writes one MDX chunk type
    void writeVERS(BinaryWriter& writer, const Model& mdx);
    void writeMODL(BinaryWriter& writer, const Model& mdx);
    void writeSEQS(BinaryWriter& writer, const Model& mdx);
    void writeGLBS(BinaryWriter& writer, const Model& mdx);
    void writeTEXS(BinaryWriter& writer, const Model& mdx);
    void writeSNDS(BinaryWriter& writer, const Model& mdx);
    void writeSNEM(BinaryWriter& writer, const Model& mdx);
    void writeMTLS(BinaryWriter& writer, const Model& mdx);
    void writeTXAN(BinaryWriter& writer, const Model& mdx);
    void writeGEOS(BinaryWriter& writer, const Model& mdx);
    void writeGEOA(BinaryWriter& writer, const Model& mdx);
    void writeBONE(BinaryWriter& writer, const Model& mdx);
    void writeLITE(BinaryWriter& writer, const Model& mdx);
    void writeHELP(BinaryWriter& writer, const Model& mdx);
    void writeATCH(BinaryWriter& writer, const Model& mdx);
    void writePIVT(BinaryWriter& writer, const Model& mdx);
    void writePREM(BinaryWriter& writer, const Model& mdx);
    void writePRE2(BinaryWriter& writer, const Model& mdx);
    void writeRIBB(BinaryWriter& writer, const Model& mdx);
    void writeEVTS(BinaryWriter& writer, const Model& mdx);
    void writeCAMS(BinaryWriter& writer, const Model& mdx);
    void writeCLID(BinaryWriter& writer, const Model& mdx);
    void writeBPOS(BinaryWriter& writer, const Model& mdx);
    void writeFFX(BinaryWriter& writer, const Model& mdx);
    void writeCORN(BinaryWriter& writer, const Model& mdx);

    // Structure writers - write individual structure types
    void writeMaterial(BinaryWriter& writer, const Material& mat, const Model& mdx);
    void writeLayer(BinaryWriter& writer, const Layer& layer, const Model& mdx);
    void writeTextureAnimation(BinaryWriter& writer, const TextureAnimation& anim);
    void writeGeoset(BinaryWriter& writer, const Geoset& geoset, const Model& mdx);
    void writeGeosetAnimation(BinaryWriter& writer, const GeosetAnimation& anim);
    void writeBone(BinaryWriter& writer, const Bone& bone);
    void writeNode(BinaryWriter& writer, const Node& node);
    void writeLight(BinaryWriter& writer, const Light& light, const Model& mdx);
    void writeHelper(BinaryWriter& writer, const Helper& helper);
    void writeAttachment(BinaryWriter& writer, const Attachment& att);
    void writeParticleEmitter(BinaryWriter& writer, const ParticleEmitter& pem);
    void writeParticleEmitter2(BinaryWriter& writer, const ParticleEmitter2& pem2);
    void writeRibbonEmitter(BinaryWriter& writer, const RibbonEmitter& ribb);
    void writeEventObject(BinaryWriter& writer, const EventObject& evt);
    void writeCamera(BinaryWriter& writer, const Camera& cam);
    void writeCollisionShape(BinaryWriter& writer, const CollisionShape& shape);
    void writeSoundEmitter(BinaryWriter& writer, const SoundEmitter& snem);
    void writeCornEmitter(BinaryWriter& writer, const CornEmitter& corn);

    /**
     * @brief Write an animation track
     * @tparam T Track value type (f32, Vector3f, etc.)
     * @param writer Binary writer
     * @param tag Track chunk tag
     * @param track Track data to write
     */
    template <typename T>
    void writeTrack(BinaryWriter& writer, u32 tag, const Track<T>& track);

    void writeNodeTracks(BinaryWriter& writer, const Node& node);
};

void Writer::Impl::writeChunkHeader(BinaryWriter& writer, u32 tag, u32 size) {
    writer.write(tag);
    writer.write(size);
}

void Writer::Impl::write(BinaryWriter& writer, const Model& mdx) {
    // Write magic number
    writer.write(MDLX_TAG);

    // Write chunks
    writeVERS(writer, mdx);
    writeMODL(writer, mdx);

    if (!mdx.sequences.empty())
        writeSEQS(writer, mdx);
    if (!mdx.globalSequences.empty())
        writeGLBS(writer, mdx);
    if (!mdx.materials.empty())
        writeMTLS(writer, mdx);
    if (!mdx.textures.empty())
        writeTEXS(writer, mdx);
    if (!mdx.sounds.empty())
        writeSNDS(writer, mdx);
    if (!mdx.soundEmitters.empty())
        writeSNEM(writer, mdx);
    if (!mdx.textureAnimations.empty())
        writeTXAN(writer, mdx);
    if (!mdx.geosets.empty())
        writeGEOS(writer, mdx);
    if (!mdx.geosetAnimations.empty())
        writeGEOA(writer, mdx);
    if (!mdx.bones.empty())
        writeBONE(writer, mdx);
    if (!mdx.lights.empty())
        writeLITE(writer, mdx);
    if (!mdx.helpers.empty())
        writeHELP(writer, mdx);
    if (!mdx.attachments.empty())
        writeATCH(writer, mdx);
    if (!mdx.pivotPoints.empty())
        writePIVT(writer, mdx);
    if (!mdx.particleEmitters.empty())
        writePREM(writer, mdx);
    if (!mdx.particleEmitters2.empty())
        writePRE2(writer, mdx);
    if (!mdx.ribbonEmitters.empty())
        writeRIBB(writer, mdx);
    if (!mdx.eventObjects.empty())
        writeEVTS(writer, mdx);
    if (!mdx.cameras.empty())
        writeCAMS(writer, mdx);
    if (!mdx.collisionShapes.empty())
        writeCLID(writer, mdx);
    if (!mdx.bindPoses.empty())
        writeBPOS(writer, mdx);
    if (!mdx.faceEffects.empty())
        writeFFX(writer, mdx);
    if (!mdx.cornEmitters.empty())
        writeCORN(writer, mdx);
}

// ============================================================================
// Writer Public Interface (using PImpl)
// ============================================================================

Writer::Writer() : pImpl(std::make_unique<Impl>()) {}

Writer::~Writer() = default;

void Writer::write(const std::string& filePath, const Model& mdx) {
    // Detect MDL text format from extension
    bool isMdl = false;
    if (filePath.size() >= 4) {
        auto ext = filePath.substr(filePath.size() - 4);
        isMdl = (ext == ".mdl" || ext == ".MDL" || ext == ".Mdl");
    }

    if (isMdl) {
        std::string text = writeModelToMdl(mdx);
        std::ofstream file(filePath, std::ios::binary);
        if (!file.is_open()) {
            throw std::runtime_error("Failed to open file: " + filePath);
        }
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
        return;
    }

    std::ofstream file;
    file.open(filePath, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open file: " + filePath);
    }
    BinaryWriter writer(file);

    pImpl->write(writer, mdx);
}

std::vector<u8> Writer::write(const Model& mdx, MDLXFormat format) {
    if (format == MDLXFormat::MDL) {
        std::string text = writeModelToMdl(mdx);
        return std::vector<u8>(text.begin(), text.end());
    }

    std::vector<u8> buffer;
    buffer.reserve(1 * 1024 * 1024); // Reserve 1MB to avoid frequent reallocations
    common::vector_streambuf streambuf(buffer);
    std::ostream out(&streambuf);
    BinaryWriter writer(out);

    pImpl->write(writer, mdx);

    buffer.shrink_to_fit(); // Reduce capacity to actual size
    return buffer;
}

// ============================================================================
// WriterImpl Implementation - Moved all private methods here
// ============================================================================

void Writer::Impl::writeVERS(BinaryWriter& writer, const Model& mdx) {
    writeChunkHeader(writer, VERS_TAG, 4);
    writer.write(mdx.version);
}

void Writer::Impl::writeMODL(BinaryWriter& writer, const Model& mdx) {
    writeChunkHeader(writer, MODL_TAG, 372);
    writer.writeString(mdx.modelName, 80);
    writer.writeString(mdx.animationFileName, 260);
    writer.write(mdx.modelExtent);
    writer.write(mdx.blendTime);
}

void Writer::Impl::writeSEQS(BinaryWriter& writer, const Model& mdx) {
    u32 size = mdx.sequences.size() * 132;
    writeChunkHeader(writer, SEQS_TAG, size);

    for (const auto& seq : mdx.sequences) {
        writer.writeString(seq.name, 80);
        writer.write(seq.intervalStart);
        writer.write(seq.intervalEnd);
        writer.write(seq.moveSpeed);
        writer.write(seq.flags);
        writer.write(seq.rarity);
        writer.write(seq.syncPoint);
        writer.write(seq.extent);
    }
}

void Writer::Impl::writeGLBS(BinaryWriter& writer, const Model& mdx) {
    u32 size = mdx.globalSequences.size() * 4;
    writeChunkHeader(writer, GLBS_TAG, size);
    writer.write(mdx.globalSequences);
}

void Writer::Impl::writeTEXS(BinaryWriter& writer, const Model& mdx) {
    u32 size = mdx.textures.size() * 268;
    writeChunkHeader(writer, TEXS_TAG, size);

    for (const auto& tex : mdx.textures) {
        writer.write(tex.replaceableId);
        writer.writeString(tex.fileName, 260);
        writer.write(tex.flags);
    }
}

void Writer::Impl::writeSNDS(BinaryWriter& writer, const Model& mdx) {
    u32 size = mdx.sounds.size() * 56;
    writeChunkHeader(writer, SNDS_TAG, size);

    for (const auto& snd : mdx.sounds) {
        writer.writeString(snd.soundFile, 44);
        writer.write(snd.maximumDistance);
        writer.write(snd.minimumDistance);
        writer.write(snd.soundChannel);
    }
}

void Writer::Impl::writeSNEM(BinaryWriter& writer, const Model& mdx) {
    SizeEnclosure sizeEnclosure(writer, SNEM_TAG);

    for (const auto& snem : mdx.soundEmitters) {
        writeSoundEmitter(writer, snem);
    }
}

void Writer::Impl::writeSoundEmitter(BinaryWriter& writer, const SoundEmitter& snem) {
    SizeEnclosure sizeEnclosure(writer);

    writeNode(writer, snem.node);

    writeTrackChunk(writer, KSEK_TAG, snem.soundTrack);
}

void Writer::Impl::writeMTLS(BinaryWriter& writer, const Model& mdx) {
    SizeEnclosure sizeEnclosure(writer, MTLS_TAG);

    for (const auto& mat : mdx.materials) {
        writeMaterial(writer, mat, mdx);
    }
}

void Writer::Impl::writeMaterial(BinaryWriter& writer, const Material& mat, const Model& mdx) {
    SizeEnclosure sizeEnclosure(writer);

    writer.write(mat.priorityPlane);
    writer.write(mat.flags);

    if (mdx.version > 800 && mdx.version < 1100) {
        writer.writeString(mat.shader, 80);
    }

    // Write LAYS chunk
    writer.write(LAYS_TAG);
    writer.write(static_cast<u32>(mat.layers.size()));

    for (const auto& layer : mat.layers) {
        writeLayer(writer, layer, mdx);
    }
}

void Writer::Impl::writeLayer(BinaryWriter& writer, const Layer& layer, const Model& mdx) {
    SizeEnclosure sizeEnclosure(writer);

    writer.write(static_cast<u32>(layer.filterMode));
    writer.write(static_cast<u32>(layer.shadingFlags));
    writer.write(layer.textureId);
    writer.write(layer.textureAnimationId);
    writer.write(layer.coordId);
    writer.write(layer.alpha);

    if (mdx.version > 800) {
        writer.write(layer.emissiveGain);
        writer.write(layer.fresnelColor);
        writer.write(layer.fresnelOpacity);
        writer.write(layer.fresnelTeamColor);
    }

    if (mdx.version >= 1100) {
        writer.write(layer.is_hd ? 1 : 0);
        writer.write(static_cast<u32>(layer.subTextures.size()));

        for (const auto& subTex : layer.subTextures) {
            writer.write(subTex.textureId);
            writer.write(static_cast<u32>(subTex.slot));
            writeTrackChunk(writer, KMTF_TAG, subTex.tracks);
        }
    }

    // Write animation tracks
    if (mdx.version < 1100) {
        writeTrackChunk(writer, KMTF_TAG, layer.textureIdTracks);
    }
    writeTrackChunk(writer, KMTA_TAG, layer.alphaTracks);
    if (mdx.version > 800) {
        writeTrackChunk(writer, KMTE_TAG, layer.emissiveGainTracks);
    }
    if (mdx.version > 900) {
        writeTrackChunk(writer, KFC3_TAG, layer.fresnelColorTracks);
        writeTrackChunk(writer, KFCA_TAG, layer.fresnelAlphaTracks);
        writeTrackChunk(writer, KFTC_TAG, layer.fresnelTeamColorTracks);
    }
}

void Writer::Impl::writeTXAN(BinaryWriter& writer, const Model& mdx) {
    SizeEnclosure sizeEnclosure(writer, TXAN_TAG);

    for (const auto& anim : mdx.textureAnimations) {
        writeTextureAnimation(writer, anim);
    }
}

void Writer::Impl::writeTextureAnimation(BinaryWriter& writer, const TextureAnimation& anim) {
    SizeEnclosure sizeEnclosure(writer);

    writeTrackChunk(writer, KTAT_TAG, anim.translationTracks);
    writeTrackChunk(writer, KTAR_TAG, anim.rotationTracks);
    writeTrackChunk(writer, KTAS_TAG, anim.scalingTracks);
}

void Writer::Impl::writeGEOS(BinaryWriter& writer, const Model& mdx) {
    SizeEnclosure sizeEnclosure(writer, GEOS_TAG);

    for (const auto& geo : mdx.geosets) {
        writeGeoset(writer, geo, mdx);
    }
}

void Writer::Impl::writeGeoset(BinaryWriter& writer, const Geoset& geo, const Model& mdx) {
    SizeEnclosure sizeEnclosure(writer);

    // Write VRTX
    writeChunkHeader(writer, VRTX_TAG, geo.vertexPositions.size());
    writer.write(geo.vertexPositions);

    // Write NRMS
    writeChunkHeader(writer, NRMS_TAG, geo.vertexNormals.size());
    writer.write(geo.vertexNormals);

    // Write PTYP
    writeChunkHeader(writer, PTYP_TAG, geo.faceTypeGroups.size());
    writer.write(geo.faceTypeGroups);

    // Write PCNT
    writeChunkHeader(writer, PCNT_TAG, geo.faceGroups.size());
    writer.write(geo.faceGroups);

    // Write PVTX
    writeChunkHeader(writer, PVTX_TAG, geo.faces.size());
    writer.write(geo.faces);

    // Write GNDX
    writeChunkHeader(writer, GNDX_TAG, geo.vertexGroups.size());
    writer.write(geo.vertexGroups);

    // Write MTGC
    writeChunkHeader(writer, MTGC_TAG, geo.matrixGroups.size());
    writer.write(geo.matrixGroups);

    // Write MATS
    writeChunkHeader(writer, MATS_TAG, geo.matrixIndices.size());
    writer.write(geo.matrixIndices);

    writer.write(geo.materialId);
    writer.write(geo.selectionGroup);
    writer.write(geo.selectionFlags);

    if (mdx.version > 800) {
        writer.write(geo.lod);
        writer.writeString(geo.lodName, 80);
    }

    writer.write(geo.extent);

    writer.write(static_cast<u32>(geo.sequenceExtents.size()));
    for (const auto& ext : geo.sequenceExtents) {
        writer.write(ext);
    }

    if (mdx.version > 800) {
        // Write optional TANG
        if (!geo.tangents.empty()) {
            writeChunkHeader(writer, TANG_TAG, geo.tangents.size());
            writer.write(geo.tangents);
        }

        // Write optional SKIN
        if (!geo.skinData.empty()) {
            writeChunkHeader(writer, SKIN_TAG, geo.skinData.size());
            writer.write(geo.skinData);
        }
    }

    // Write optional UVAS
    if (!geo.textureCoordinateSets.empty()) {
        writeChunkHeader(writer, UVAS_TAG, geo.textureCoordinateSets.size());

        for (const auto& uvSet : geo.textureCoordinateSets) {
            writeChunkHeader(writer, UVBS_TAG, uvSet.size());
            writer.write(uvSet);
        }
    }
}

void Writer::Impl::writeGEOA(BinaryWriter& writer, const Model& mdx) {
    SizeEnclosure sizeEnclosure(writer, GEOA_TAG);

    for (const auto& anim : mdx.geosetAnimations) {
        writeGeosetAnimation(writer, anim);
    }
}

void Writer::Impl::writeGeosetAnimation(BinaryWriter& writer, const GeosetAnimation& anim) {
    SizeEnclosure sizeEnclosure(writer);

    writer.write(anim.alpha);
    writer.write(anim.flags);
    writer.write(anim.color);
    writer.write(anim.geosetId);

    writeTrackChunk(writer, KGAO_TAG, anim.alphaTracks);
    writeTrackChunk(writer, KGAC_TAG, anim.colorTracks);
}

void Writer::Impl::writeBONE(BinaryWriter& writer, const Model& mdx) {
    SizeEnclosure sizeEnclosure(writer, BONE_TAG);

    for (const auto& bone : mdx.bones) {
        writeBone(writer, bone);
    }
}

void Writer::Impl::writeBone(BinaryWriter& writer, const Bone& bone) {
    writeNode(writer, bone.node);
    writer.write(bone.geosetId);
    writer.write(bone.geosetAnimationId);
}

void Writer::Impl::writeNode(BinaryWriter& writer, const Node& node) {
    SizeEnclosure sizeEnclosure(writer);

    writer.writeString(node.name, 80);
    writer.write(node.objectId);
    writer.write(node.parentId);
    writer.write(static_cast<u32>(node.flags));

    writeNodeTracks(writer, node);
}

void Writer::Impl::writeNodeTracks(BinaryWriter& writer, const Node& node) {
    writeTrackChunk(writer, KGTR_TAG, node.translationTracks);
    writeTrackChunk(writer, KGRT_TAG, node.rotationTracks);
    writeTrackChunk(writer, KGSC_TAG, node.scalingTracks);
}

void Writer::Impl::writeLITE(BinaryWriter& writer, const Model& mdx) {
    SizeEnclosure sizeEnclosure(writer, LITE_TAG);

    for (const auto& light : mdx.lights) {
        writeLight(writer, light, mdx);
    }
}

void Writer::Impl::writeLight(BinaryWriter& writer, const Light& light, const Model& mdx) {
    SizeEnclosure sizeEnclosure(writer);

    writeNode(writer, light.node);
    writer.write(static_cast<u32>(light.type));
    writer.write(light.attenuationStart);
    writer.write(light.attenuationEnd);
    writer.write(light.color);
    writer.write(light.intensity);
    writer.write(light.ambientColor);
    writer.write(light.ambientIntensity);
    if (mdx.version >= 1200) {
        writer.write(light.shadowIntensity);
    }

    writeTrackChunk(writer, KLAS_TAG, light.attenuationStartTracks);
    writeTrackChunk(writer, KLAE_TAG, light.attenuationEndTracks);
    writeTrackChunk(writer, KLAC_TAG, light.colorTracks);
    writeTrackChunk(writer, KLAI_TAG, light.intensityTracks);
    writeTrackChunk(writer, KLBI_TAG, light.ambientIntensityTracks);
    writeTrackChunk(writer, KLBC_TAG, light.ambientColorTracks);
    writeTrackChunk(writer, KLAV_TAG, light.visibilityTracks);
}

void Writer::Impl::writeHELP(BinaryWriter& writer, const Model& mdx) {
    SizeEnclosure sizeEnclosure(writer, HELP_TAG);

    for (const auto& helper : mdx.helpers) {
        writeHelper(writer, helper);
    }
}

void Writer::Impl::writeHelper(BinaryWriter& writer, const Helper& helper) {
    writeNode(writer, helper.node);
}

void Writer::Impl::writeATCH(BinaryWriter& writer, const Model& mdx) {
    SizeEnclosure sizeEnclosure(writer, ATCH_TAG);

    for (const auto& att : mdx.attachments) {
        writeAttachment(writer, att);
    }
}

void Writer::Impl::writeAttachment(BinaryWriter& writer, const Attachment& att) {
    SizeEnclosure sizeEnclosure(writer);

    writeNode(writer, att.node);
    writer.writeString(att.path, 260);
    writer.write(att.attachmentId);

    writeTrackChunk(writer, KATV_TAG, att.visibilityTracks);
}

void Writer::Impl::writePIVT(BinaryWriter& writer, const Model& mdx) {
    u32 size = mdx.pivotPoints.size() * 12;
    writeChunkHeader(writer, PIVT_TAG, size);
    writer.write(mdx.pivotPoints);
}

void Writer::Impl::writePREM(BinaryWriter& writer, const Model& mdx) {
    SizeEnclosure sizeEnclosure(writer, PREM_TAG);

    for (const auto& pem : mdx.particleEmitters) {
        writeParticleEmitter(writer, pem);
    }
}

void Writer::Impl::writeParticleEmitter(BinaryWriter& writer, const ParticleEmitter& pem) {
    SizeEnclosure sizeEnclosure(writer);

    writeNode(writer, pem.node);
    writer.write(pem.emissionRate);
    writer.write(pem.gravity);
    writer.write(pem.longitude);
    writer.write(pem.latitude);
    writer.writeString(pem.spawnModelFileName, 260);
    writer.write(pem.lifespan);
    writer.write(pem.initialVelocity);

    writeTrackChunk(writer, KPEE_TAG, pem.emissionRateTracks);
    writeTrackChunk(writer, KPEG_TAG, pem.gravityTracks);
    writeTrackChunk(writer, KPLN_TAG, pem.longitudeTracks);
    writeTrackChunk(writer, KPLT_TAG, pem.latitudeTracks);
    writeTrackChunk(writer, KPEL_TAG, pem.lifespanTracks);
    writeTrackChunk(writer, KPES_TAG, pem.speedTracks);
    writeTrackChunk(writer, KPEV_TAG, pem.visibilityTracks);
}

void Writer::Impl::writePRE2(BinaryWriter& writer, const Model& mdx) {
    SizeEnclosure sizeEnclosure(writer, PRE2_TAG);

    for (const auto& pem2 : mdx.particleEmitters2) {
        writeParticleEmitter2(writer, pem2);
    }
}

void Writer::Impl::writeParticleEmitter2(BinaryWriter& writer, const ParticleEmitter2& pem2) {
    SizeEnclosure sizeEnclosure(writer);

    writeNode(writer, pem2.node);
    writer.write(pem2.speed);
    writer.write(pem2.variation);
    writer.write(pem2.latitude);
    writer.write(pem2.gravity);
    writer.write(pem2.lifespan);
    writer.write(pem2.emissionRate);
    writer.write(pem2.length);
    writer.write(pem2.width);

    writer.write(pem2.filterMode);
    writer.write(pem2.rows);
    writer.write(pem2.columns);
    writer.write(pem2.headOrTail);

    writer.write(pem2.tailLength);
    writer.write(pem2.time);

    for (int i = 0; i < 3; i++) {
        writer.write(pem2.segmentColor[i]);
    }
    for (int i = 0; i < 3; i++) {
        writer.write(pem2.segmentAlpha[i]);
    }
    for (int i = 0; i < 3; i++) {
        writer.write(pem2.segmentScaling[i]);
    }

    for (int i = 0; i < 3; i++) {
        writer.write(pem2.headInterval[i]);
    }
    for (int i = 0; i < 3; i++) {
        writer.write(pem2.headDecayInterval[i]);
    }
    for (int i = 0; i < 3; i++) {
        writer.write(pem2.tailInterval[i]);
    }
    for (int i = 0; i < 3; i++) {
        writer.write(pem2.tailDecayInterval[i]);
    }

    writer.write(pem2.textureId);
    writer.write(pem2.squirt);
    writer.write(pem2.priorityPlane);
    writer.write(pem2.replaceableId);

    writeTrackChunk(writer, KP2S_TAG, pem2.speedTracks);
    writeTrackChunk(writer, KP2R_TAG, pem2.variationTracks);
    writeTrackChunk(writer, KP2L_TAG, pem2.latitudeTracks);
    writeTrackChunk(writer, KP2G_TAG, pem2.gravityTracks);
    writeTrackChunk(writer, KP2E_TAG, pem2.emissionRateTracks);
    writeTrackChunk(writer, KP2N_TAG, pem2.lengthTracks);
    writeTrackChunk(writer, KP2W_TAG, pem2.widthTracks);
    writeTrackChunk(writer, KP2V_TAG, pem2.visibilityTracks);
}

void Writer::Impl::writeRIBB(BinaryWriter& writer, const Model& mdx) {
    SizeEnclosure sizeEnclosure(writer, RIBB_TAG);

    for (const auto& ribb : mdx.ribbonEmitters) {
        writeRibbonEmitter(writer, ribb);
    }
}

void Writer::Impl::writeRibbonEmitter(BinaryWriter& writer, const RibbonEmitter& ribb) {
    SizeEnclosure sizeEnclosure(writer);

    writeNode(writer, ribb.node);
    writer.write(ribb.heightAbove);
    writer.write(ribb.heightBelow);
    writer.write(ribb.alpha);
    writer.write(ribb.color);
    writer.write(ribb.lifespan);
    writer.write(ribb.textureSlot);
    writer.write(ribb.emissionRate);
    writer.write(ribb.rows);
    writer.write(ribb.columns);
    writer.write(ribb.materialId);
    writer.write(ribb.gravity);

    writeTrackChunk(writer, KRHA_TAG, ribb.heightAboveTracks);
    writeTrackChunk(writer, KRHB_TAG, ribb.heightBelowTracks);
    writeTrackChunk(writer, KRAL_TAG, ribb.alphaTracks);
    writeTrackChunk(writer, KRCO_TAG, ribb.colorTracks);
    writeTrackChunk(writer, KRTX_TAG, ribb.textureSlotTracks);
    writeTrackChunk(writer, KRVS_TAG, ribb.visibilityTracks);
}

void Writer::Impl::writeEVTS(BinaryWriter& writer, const Model& mdx) {
    SizeEnclosure sizeEnclosure(writer, EVTS_TAG);

    for (const auto& evt : mdx.eventObjects) {
        writeEventObject(writer, evt);
    }
}

void Writer::Impl::writeEventObject(BinaryWriter& writer, const EventObject& evt) {
    writeNode(writer, evt.node);

    writer.write(KEVT_TAG);
    writer.write(static_cast<u32>(evt.eventTrackTimes.size()));
    writer.write(evt.eventTrackTimes);
    writer.write(evt.globalSequenceId);
}

void Writer::Impl::writeCAMS(BinaryWriter& writer, const Model& mdx) {
    SizeEnclosure sizeEnclosure(writer, CAMS_TAG);

    for (const auto& cam : mdx.cameras) {
        writeCamera(writer, cam);
    }
}

void Writer::Impl::writeCamera(BinaryWriter& writer, const Camera& cam) {
    SizeEnclosure sizeEnclosure(writer);

    writer.writeString(cam.name, 80);
    writer.write(cam.position);
    writer.write(cam.fieldOfView);
    writer.write(cam.farClippingPlane);
    writer.write(cam.nearClippingPlane);
    writer.write(cam.targetPosition);

    writeTrackChunk(writer, KCTR_TAG, cam.positionTracks);
    writeTrackChunk(writer, KCRL_TAG, cam.targetRotationTracks);
    writeTrackChunk(writer, KTTR_TAG, cam.targetPositionTracks);
}

void Writer::Impl::writeCLID(BinaryWriter& writer, const Model& mdx) {
    SizeEnclosure sizeEnclosure(writer, CLID_TAG);

    for (const auto& shape : mdx.collisionShapes) {
        writeCollisionShape(writer, shape);
    }
}

void Writer::Impl::writeCollisionShape(BinaryWriter& writer, const CollisionShape& shape) {
    writeNode(writer, shape.node);
    writer.write(static_cast<u32>(shape.type));

    writer.write(shape.vertices);

    if (shape.type == CollisionShape::ShapeType::Sphere ||
        shape.type == CollisionShape::ShapeType::Cylinder) {
        writer.write(shape.radius);
    }
}

void Writer::Impl::writeBPOS(BinaryWriter& writer, const Model& mdx) {
    u32 size = 4 + mdx.bindPoses.size() * 48; // count + matrices
    writeChunkHeader(writer, BPOS_TAG, size);

    writer.write(static_cast<u32>(mdx.bindPoses.size()));
    for (const auto& pose : mdx.bindPoses) {
        for (int i = 0; i < 12; i++) {
            writer.write(pose[i]);
        }
    }
}

void Writer::Impl::writeFFX(BinaryWriter& writer, const Model& mdx) {
    u32 size = mdx.faceEffects.size() * 340;
    writeChunkHeader(writer, FAFX_TAG, size);

    for (const auto& fx : mdx.faceEffects) {
        writer.writeString(fx.target, 80);
        writer.writeString(fx.path, 260);
    }
}

void Writer::Impl::writeCORN(BinaryWriter& writer, const Model& mdx) {
    SizeEnclosure sizeEnclosure(writer, CORN_TAG);

    for (const auto& corn : mdx.cornEmitters) {
        writeCornEmitter(writer, corn);
    }
}

void Writer::Impl::writeCornEmitter(BinaryWriter& writer, const CornEmitter& corn) {
    SizeEnclosure sizeEnclosure(writer);

    writeNode(writer, corn.node);
    writer.write(corn.lifeSpan);
    writer.write(corn.emissionRate);
    writer.write(corn.speed);
    writer.write(corn.color.x);
    writer.write(corn.color.y);
    writer.write(corn.color.z);
    writer.write(corn.color.w);
    writer.write(corn.replaceableId);
    writer.writeString(corn.path, 260);
    writer.writeString(corn.animVisibilityGuide, 260);

    writeTrackChunk(writer, KPPA_TAG, corn.lifeSpanTracks);
    writeTrackChunk(writer, KPPC_TAG, corn.colorTracks);
    writeTrackChunk(writer, KPPE_TAG, corn.emissionRateTracks);
    writeTrackChunk(writer, KPPL_TAG, corn.lifeSpanVariationTracks);
    writeTrackChunk(writer, KPPS_TAG, corn.speedTracks);
    writeTrackChunk(writer, KPPV_TAG, corn.visibilityTracks);
}

} // namespace mdx
} // namespace whiteout
