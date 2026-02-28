#include "../include/mdx/mdx_writer.h"
#include "../include/common/binary_writer.h"
#include <iostream>
#include <fstream>
#include <cstring>
#include <stdexcept>

namespace mdx {

namespace {

struct SizeEnclosure {
    u32 startPos;
    u32 sizePos;
    BinaryWriter& writer;

    SizeEnclosure(BinaryWriter& writer, u32 tag = 0) : writer(writer) {
        
        if (tag != 0) {
            writer.writeUInt32(tag);
        }
        startPos = writer.getPosition();
        writer.writeUInt32(0); // Placeholder for size
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
        writer.writeUInt32(actualSize);
        writer.setPosition(endPos); // Move back to end
    }
};

}

template<typename T>
static void writeTrackChunk(BinaryWriter& writer, u32 tag, const Track<T>& track) {
    if (!track.isUsed) return; // Skip unused tracks
    
    // Write track header
    writer.writeUInt32(tag);
    writer.writeUInt32(track.keyCount);
    writer.writeUInt32(static_cast<u32>(track.interpolationType));
    writer.writeUInt32(track.globalSequenceId);
    
    // Write track data
    writer.writeUInt8Array(track.keys_data);
}

void MDXWriter::write(const std::string& filePath, const MDXFile& mdx) {
    std::ofstream file;
    file.open(filePath, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open file: " + filePath);
    }
    BinaryWriter writer(file);
    
    write(writer, mdx);
}

void MDXWriter::write(BinaryWriter& writer, const MDXFile& mdx) {
    // Write magic number
    writer.writeUInt32(MDLX_TAG);
    
    // Write chunks
    writeVERS(writer, mdx);
    writeMODL(writer, mdx);
    
    if (!mdx.sequences.empty()) writeSEQS(writer, mdx);
    if (!mdx.globalSequences.empty()) writeGLBS(writer, mdx);
    if (!mdx.textures.empty()) writeTEXS(writer, mdx);
    if (!mdx.soundTracks.empty()) writeSNDS(writer, mdx);
    if (!mdx.materials.empty()) writeMTLS(writer, mdx);
    if (!mdx.textureAnimations.empty()) writeTXAN(writer, mdx);
    if (!mdx.geosets.empty()) writeGEOS(writer, mdx);
    if (!mdx.geosetAnimations.empty()) writeGEOA(writer, mdx);
    if (!mdx.bones.empty()) writeBONE(writer, mdx);
    if (!mdx.lights.empty()) writeLITE(writer, mdx);
    if (!mdx.helpers.empty()) writeHELP(writer, mdx);
    if (!mdx.attachments.empty()) writeATCH(writer, mdx);
    if (!mdx.pivotPoints.empty()) writePIVT(writer, mdx);
    if (!mdx.particleEmitters.empty()) writePREM(writer, mdx);
    if (!mdx.particleEmitters2.empty()) writePRE2(writer, mdx);
    if (!mdx.ribbonEmitters.empty()) writeRIBB(writer, mdx);
    if (!mdx.eventObjects.empty()) writeEVTS(writer, mdx);
    if (!mdx.cameras.empty()) writeCAMS(writer, mdx);
    if (!mdx.collisionShapes.empty()) writeCLID(writer, mdx);
    if (!mdx.bindPoses.empty()) writeBPOS(writer, mdx);
    if (!mdx.faceEffects.empty()) writeFFX(writer, mdx);
    if (!mdx.cornEmitters.empty()) writeCORN(writer, mdx);
}

void MDXWriter::writeChunkHeader(BinaryWriter& writer, u32 tag, u32 size) {
    writer.writeUInt32(tag);
    writer.writeUInt32(size);
}

void MDXWriter::writeVERS(BinaryWriter& writer, const MDXFile& mdx) {
    writeChunkHeader(writer, VERS_TAG, 4);
    writer.writeUInt32(mdx.version);
}

void MDXWriter::writeMODL(BinaryWriter& writer, const MDXFile& mdx) {
    writeChunkHeader(writer, MODL_TAG, 372);
    writer.writeString(mdx.modelName, 80);
    writer.writeString(mdx.animationFileName, 260);
    writer.writeExtent(mdx.modelExtent);
    writer.writeUInt32(mdx.blendTime);
}

void MDXWriter::writeSEQS(BinaryWriter& writer, const MDXFile& mdx) {
    u32 size = mdx.sequences.size() * 132;
    writeChunkHeader(writer, SEQS_TAG, size);
    
    for (const auto& seq : mdx.sequences) {
        writer.writeString(seq.name, 80);
        writer.writeUInt32(seq.intervalStart);
        writer.writeUInt32(seq.intervalEnd);
        writer.writeFloat32(seq.moveSpeed);
        writer.writeUInt32(seq.flags);
        writer.writeFloat32(seq.rarity);
        writer.writeUInt32(seq.syncPoint);
        writer.writeExtent(seq.extent);
    }
}

void MDXWriter::writeGLBS(BinaryWriter& writer, const MDXFile& mdx) {
    u32 size = mdx.globalSequences.size() * 4;
    writeChunkHeader(writer, GLBS_TAG, size);
    writer.writeUInt32Array(mdx.globalSequences);
}

void MDXWriter::writeTEXS(BinaryWriter& writer, const MDXFile& mdx) {
    u32 size = mdx.textures.size() * 268;
    writeChunkHeader(writer, TEXS_TAG, size);
    
    for (const auto& tex : mdx.textures) {
        writer.writeUInt32(tex.replaceableId);
        writer.writeString(tex.fileName, 260);
        writer.writeUInt32(tex.flags);
    }
}

void MDXWriter::writeSNDS(BinaryWriter& writer, const MDXFile& mdx) {
    u32 size = mdx.soundTracks.size() * 272;
    writeChunkHeader(writer, SNDS_TAG, size);
    
    for (const auto& st : mdx.soundTracks) {
        writer.writeString(st.fileName, 260);
        writer.writeFloat32(st.volume);
        writer.writeFloat32(st.pitch);
        writer.writeUInt32(st.flags);
    }
}

void MDXWriter::writeMTLS(BinaryWriter& writer, const MDXFile& mdx) {
    SizeEnclosure sizeEnclosure(writer, MTLS_TAG);
    
    for (const auto& mat : mdx.materials) {
        writeMaterial(writer, mat, mdx);
    }
}

void MDXWriter::writeMaterial(BinaryWriter& writer, const Material& mat, const MDXFile& mdx) {
    SizeEnclosure sizeEnclosure(writer);
    
    writer.writeUInt32(mat.priorityPlane);
    writer.writeUInt32(mat.flags);
    
    if (mdx.version > 800 && mdx.version < 1100) {
        writer.writeString(mat.shader, 80);
    }
    
    // Write LAYS chunk
    writer.writeUInt32(LAYS_TAG);
    writer.writeUInt32(mat.layers.size());
    
    for (const auto& layer : mat.layers) {
        writeLayer(writer, layer, mdx);
    }
}

void MDXWriter::writeLayer(BinaryWriter& writer, const Layer& layer, const MDXFile& mdx) {
    SizeEnclosure sizeEnclosure(writer);
    
    writer.writeUInt32(static_cast<u32>(layer.filterMode));
    writer.writeUInt32(static_cast<u32>(layer.shadingFlags));
    writer.writeUInt32(layer.textureId);
    writer.writeUInt32(layer.textureAnimationId);
    writer.writeUInt32(layer.coordId);
    writer.writeFloat32(layer.alpha);

    if (mdx.version > 800) {
        writer.writeFloat32(layer.emissiveGain);
        writer.writeVector3f(layer.fresnelColor);
        writer.writeFloat32(layer.fresnelOpacity);
        writer.writeFloat32(layer.fresnelTeamColor);
    }

    if (mdx.version >= 1100) {
        writer.writeUInt32(layer.is_hd ? 1 : 0);
        writer.writeUInt32(layer.subTextures.size());
        
        for (const auto& subTex : layer.subTextures) {
            writer.writeUInt32(subTex.textureId);
            writer.writeUInt32(subTex.slot);
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

void MDXWriter::writeTXAN(BinaryWriter& writer, const MDXFile& mdx) {
    SizeEnclosure sizeEnclosure(writer, TXAN_TAG);
    
    for (const auto& anim : mdx.textureAnimations) {
        writeTextureAnimation(writer, anim);
    }
    
}

void MDXWriter::writeTextureAnimation(BinaryWriter& writer, const TextureAnimation& anim) {
    SizeEnclosure sizeEnclosure(writer);
    
    writeTrackChunk(writer, KTAT_TAG, anim.translationTracks);
    writeTrackChunk(writer, KTAR_TAG, anim.rotationTracks);
    writeTrackChunk(writer, KTAS_TAG, anim.scalingTracks);
}

void MDXWriter::writeGEOS(BinaryWriter& writer, const MDXFile& mdx) {
    SizeEnclosure sizeEnclosure(writer, GEOS_TAG);
    
    for (const auto& geo : mdx.geosets) {
        writeGeoset(writer, geo, mdx);
    }
}

void MDXWriter::writeGeoset(BinaryWriter& writer, const Geoset& geo, const MDXFile& mdx) {
    SizeEnclosure sizeEnclosure(writer);
    
    // Write VRTX
    writeChunkHeader(writer, VRTX_TAG, geo.vertexPositions.size());
    writer.writeVector3fArray(geo.vertexPositions);
    
    // Write NRMS
    writeChunkHeader(writer, NRMS_TAG, geo.vertexNormals.size());
    writer.writeVector3fArray(geo.vertexNormals);
    
    // Write PTYP
    writeChunkHeader(writer, PTYP_TAG, geo.faceTypeGroups.size());
    writer.writeUInt32Array(geo.faceTypeGroups);
    
    // Write PCNT
    writeChunkHeader(writer, PCNT_TAG, geo.faceGroups.size());
    writer.writeUInt32Array(geo.faceGroups);
    
    // Write PVTX
    writeChunkHeader(writer, PVTX_TAG, geo.faces.size());
    writer.writeUInt16Array(geo.faces);
    
    // Write GNDX
    writeChunkHeader(writer, GNDX_TAG, geo.vertexGroups.size());
    writer.writeUInt8Array(geo.vertexGroups);
    
    // Write MTGC
    writeChunkHeader(writer, MTGC_TAG, geo.matrixGroups.size());
    writer.writeUInt32Array(geo.matrixGroups);
    
    // Write MATS
    writeChunkHeader(writer, MATS_TAG, geo.matrixIndices.size());
    writer.writeUInt32Array(geo.matrixIndices);
    
    writer.writeUInt32(geo.materialId);
    writer.writeUInt32(geo.selectionGroup);
    writer.writeUInt32(geo.selectionFlags);
    
    if (mdx.version > 800) {
        writer.writeUInt32(geo.lod);
        writer.writeString(geo.lodName, 80);
    }
    
    writer.writeExtent(geo.extent);
    
    writer.writeUInt32(geo.sequenceExtents.size());
    for (const auto& ext : geo.sequenceExtents) {
        writer.writeExtent(ext);
    }
    
    if (mdx.version > 800) {
        // Write optional TANG
        if (!geo.tangents.empty()) {
            writeChunkHeader(writer, TANG_TAG, geo.tangents.size());
            writer.writeVector4fArray(geo.tangents);
        }
        
        // Write optional SKIN
        if (!geo.skinData.empty()) {
            writeChunkHeader(writer, SKIN_TAG, geo.skinData.size());
            writer.writeUInt8Array(geo.skinData);
        }
    }
    
    // Write optional UVAS
    if (!geo.textureCoordinateSets.empty()) {
        writeChunkHeader(writer, UVAS_TAG, geo.textureCoordinateSets.size());
        
        for (const auto& uvSet : geo.textureCoordinateSets) {
            writeChunkHeader(writer, UVBS_TAG, uvSet.size());
            writer.writeVector2fArray(uvSet);
        }
    }
}

void MDXWriter::writeGEOA(BinaryWriter& writer, const MDXFile& mdx) {
    SizeEnclosure sizeEnclosure(writer, GEOA_TAG);
    
    for (const auto& anim : mdx.geosetAnimations) {
        writeGeosetAnimation(writer, anim);
    }

}

void MDXWriter::writeGeosetAnimation(BinaryWriter& writer, const GeosetAnimation& anim) {
    SizeEnclosure sizeEnclosure(writer);
    
    writer.writeFloat32(anim.alpha);
    writer.writeUInt32(anim.flags);
    writer.writeVector3f(anim.color);
    writer.writeUInt32(anim.geosetId);
    
    writeTrackChunk(writer, KGAO_TAG, anim.alphaTracks);
    writeTrackChunk(writer, KGAC_TAG, anim.colorTracks);
}

void MDXWriter::writeBONE(BinaryWriter& writer, const MDXFile& mdx) {
    SizeEnclosure sizeEnclosure(writer, BONE_TAG);

    for (const auto& bone : mdx.bones) {
        writeBone(writer, bone);
    }
}

void MDXWriter::writeBone(BinaryWriter& writer, const Bone& bone) {
    writeNode(writer, bone.node);
    writer.writeUInt32(bone.geosetId);
    writer.writeUInt32(bone.geosetAnimationId);
}

void MDXWriter::writeNode(BinaryWriter& writer, const Node& node) {
    SizeEnclosure sizeEnclosure(writer);
    
    writer.writeString(node.name, 80);
    writer.writeUInt32(node.objectId);
    writer.writeUInt32(node.parentId);
    writer.writeUInt32(static_cast<u32>(node.flags));
    
    writeNodeTracks(writer, node);
}

void MDXWriter::writeNodeTracks(BinaryWriter& writer, const Node& node) {
    writeTrackChunk(writer, KGTR_TAG, node.translationTracks);
    writeTrackChunk(writer, KGRT_TAG, node.rotationTracks);
    writeTrackChunk(writer, KGSC_TAG, node.scalingTracks);
}

void MDXWriter::writeLITE(BinaryWriter& writer, const MDXFile& mdx) {
    SizeEnclosure sizeEnclosure(writer, LITE_TAG);
    
    for (const auto& light : mdx.lights) {
        writeLight(writer, light, mdx);
    }
}

void MDXWriter::writeLight(BinaryWriter& writer, const Light& light, const MDXFile& mdx) {
    SizeEnclosure sizeEnclosure(writer);
    
    writeNode(writer, light.node);
    writer.writeUInt32(static_cast<u32>(light.type));
    writer.writeFloat32(light.attenuationStart);
    writer.writeFloat32(light.attenuationEnd);
    writer.writeVector3f(light.color);
    writer.writeFloat32(light.intensity);
    writer.writeVector3f(light.ambientColor);
    writer.writeFloat32(light.ambientIntensity);
    if (mdx.version >= 1200) {
        writer.writeFloat32(light.shadowIntensity);
    }
    
    writeTrackChunk(writer, KLAS_TAG, light.attenuationStartTracks);
    writeTrackChunk(writer, KLAE_TAG, light.attenuationEndTracks);
    writeTrackChunk(writer, KLAC_TAG, light.colorTracks);
    writeTrackChunk(writer, KLAI_TAG, light.intensityTracks);
    writeTrackChunk(writer, KLBI_TAG, light.ambientIntensityTracks);
    writeTrackChunk(writer, KLBC_TAG, light.ambientColorTracks);
    writeTrackChunk(writer, KLAV_TAG, light.visibilityTracks);
}

void MDXWriter::writeHELP(BinaryWriter& writer, const MDXFile& mdx) {
    SizeEnclosure sizeEnclosure(writer, HELP_TAG);
    
    for (const auto& helper : mdx.helpers) {
        writeHelper(writer, helper);
    }
}

void MDXWriter::writeHelper(BinaryWriter& writer, const Helper& helper) {
    writeNode(writer, helper.node);
}

void MDXWriter::writeATCH(BinaryWriter& writer, const MDXFile& mdx) {
    SizeEnclosure sizeEnclosure(writer, ATCH_TAG);
    
    for (const auto& att : mdx.attachments) {
        writeAttachment(writer, att);
    }
}

void MDXWriter::writeAttachment(BinaryWriter& writer, const Attachment& att) {
    SizeEnclosure sizeEnclosure(writer);
    
    writeNode(writer, att.node);
    writer.writeString(att.path, 260);
    writer.writeUInt32(att.attachmentId);
    
    writeTrackChunk(writer, KATV_TAG, att.visibilityTracks);
}

void MDXWriter::writePIVT(BinaryWriter& writer, const MDXFile& mdx) {
    u32 size = mdx.pivotPoints.size() * 12;
    writeChunkHeader(writer, PIVT_TAG, size);
    writer.writeVector3fArray(mdx.pivotPoints);
}

void MDXWriter::writePREM(BinaryWriter& writer, const MDXFile& mdx) {
    SizeEnclosure sizeEnclosure(writer, PREM_TAG);
    
    for (const auto& pem : mdx.particleEmitters) {
        writeParticleEmitter(writer, pem);
    }
}

void MDXWriter::writeParticleEmitter(BinaryWriter& writer, const ParticleEmitter& pem) {
    SizeEnclosure sizeEnclosure(writer);
    
    writeNode(writer, pem.node);
    writer.writeFloat32(pem.emissionRate);
    writer.writeFloat32(pem.gravity);
    writer.writeFloat32(pem.longitude);
    writer.writeFloat32(pem.latitude);
    writer.writeString(pem.spawnModelFileName, 260);
    writer.writeFloat32(pem.lifespan);
    writer.writeFloat32(pem.initialVelocity);
    
    writeTrackChunk(writer, KPEE_TAG, pem.emissionRateTracks);
    writeTrackChunk(writer, KPEG_TAG, pem.gravityTracks);
    writeTrackChunk(writer, KPLN_TAG, pem.longitudeTracks);
    writeTrackChunk(writer, KPLT_TAG, pem.latitudeTracks);
    writeTrackChunk(writer, KPEL_TAG, pem.lifespanTracks);
    writeTrackChunk(writer, KPES_TAG, pem.speedTracks);
    writeTrackChunk(writer, KPEV_TAG, pem.visibilityTracks);
}

void MDXWriter::writePRE2(BinaryWriter& writer, const MDXFile& mdx) {
    SizeEnclosure sizeEnclosure(writer, PRE2_TAG);
    
    for (const auto& pem2 : mdx.particleEmitters2) {
        writeParticleEmitter2(writer, pem2);
    }
}

void MDXWriter::writeParticleEmitter2(BinaryWriter& writer, const ParticleEmitter2& pem2) {
    SizeEnclosure sizeEnclosure(writer);
    
    writeNode(writer, pem2.node);
    writer.writeFloat32(pem2.speed);
    writer.writeFloat32(pem2.variation);
    writer.writeFloat32(pem2.latitude);
    writer.writeFloat32(pem2.gravity);
    writer.writeFloat32(pem2.lifespan);
    writer.writeFloat32(pem2.emissionRate);
    writer.writeFloat32(pem2.length);
    writer.writeFloat32(pem2.width);
    
    writer.writeUInt32(pem2.filterMode);
    writer.writeUInt32(pem2.rows);
    writer.writeUInt32(pem2.columns);
    writer.writeUInt32(pem2.headOrTail);
    
    writer.writeFloat32(pem2.tailLength);
    writer.writeFloat32(pem2.time);
    
    for (int i = 0; i < 3; i++) {
        writer.writeVector3f(pem2.segmentColor[i]);
    }
    for (int i = 0; i < 3; i++) {
        writer.writeUInt8(pem2.segmentAlpha[i]);
    }
    for (int i = 0; i < 3; i++) {
        writer.writeFloat32(pem2.segmentScaling[i]);
    }
    
    for (int i = 0; i < 3; i++) {
        writer.writeUInt32(pem2.headInterval[i]);
    }
    for (int i = 0; i < 3; i++) {
        writer.writeUInt32(pem2.headDecayInterval[i]);
    }
    for (int i = 0; i < 3; i++) {
        writer.writeUInt32(pem2.tailInterval[i]);
    }
    for (int i = 0; i < 3; i++) {
        writer.writeUInt32(pem2.tailDecayInterval[i]);
    }
    
    writer.writeUInt32(pem2.textureId);
    writer.writeUInt32(pem2.squirt);
    writer.writeUInt32(pem2.priorityPlane);
    writer.writeUInt32(pem2.replaceableId);
    
    writeTrackChunk(writer, KP2S_TAG, pem2.speedTracks);
    writeTrackChunk(writer, KP2R_TAG, pem2.variationTracks);
    writeTrackChunk(writer, KP2L_TAG, pem2.latitudeTracks);
    writeTrackChunk(writer, KP2G_TAG, pem2.gravityTracks);
    writeTrackChunk(writer, KP2E_TAG, pem2.emissionRateTracks);
    writeTrackChunk(writer, KP2N_TAG, pem2.lengthTracks);
    writeTrackChunk(writer, KP2W_TAG, pem2.widthTracks);
    writeTrackChunk(writer, KP2V_TAG, pem2.visibilityTracks);
}

void MDXWriter::writeRIBB(BinaryWriter& writer, const MDXFile& mdx) {
    SizeEnclosure sizeEnclosure(writer, RIBB_TAG);
    
    for (const auto& ribb : mdx.ribbonEmitters) {
        writeRibbonEmitter(writer, ribb);
    }
}

void MDXWriter::writeRibbonEmitter(BinaryWriter& writer, const RibbonEmitter& ribb) {
    SizeEnclosure sizeEnclosure(writer);
    
    writeNode(writer, ribb.node);
    writer.writeFloat32(ribb.heightAbove);
    writer.writeFloat32(ribb.heightBelow);
    writer.writeFloat32(ribb.alpha);
    writer.writeVector3f(ribb.color);
    writer.writeFloat32(ribb.lifespan);
    writer.writeUInt32(ribb.textureSlot);
    writer.writeUInt32(ribb.emissionRate);
    writer.writeUInt32(ribb.rows);
    writer.writeUInt32(ribb.columns);
    writer.writeUInt32(ribb.materialId);
    writer.writeFloat32(ribb.gravity);
    
    writeTrackChunk(writer, KRHA_TAG, ribb.heightAboveTracks);
    writeTrackChunk(writer, KRHB_TAG, ribb.heightBelowTracks);
    writeTrackChunk(writer, KRAL_TAG, ribb.alphaTracks);
    writeTrackChunk(writer, KRCO_TAG, ribb.colorTracks);
    writeTrackChunk(writer, KRTX_TAG, ribb.textureSlotTracks);
    writeTrackChunk(writer, KRVS_TAG, ribb.visibilityTracks);
}

void MDXWriter::writeEVTS(BinaryWriter& writer, const MDXFile& mdx) {
    SizeEnclosure sizeEnclosure(writer, EVTS_TAG);
    
    for (const auto& evt : mdx.eventObjects) {
        writeEventObject(writer, evt);
    }
}

void MDXWriter::writeEventObject(BinaryWriter& writer, const EventObject& evt) {
    writeNode(writer, evt.node);
    
    writer.writeUInt32(KEVT_TAG);
    writer.writeUInt32(evt.eventTrackTimes.size());
    writer.writeUInt32Array(evt.eventTrackTimes);
    writer.writeUInt32(evt.globalSequenceId);
}

void MDXWriter::writeCAMS(BinaryWriter& writer, const MDXFile& mdx) {
    SizeEnclosure sizeEnclosure(writer, CAMS_TAG);
    
    for (const auto& cam : mdx.cameras) {
        writeCamera(writer, cam);
    }
}

void MDXWriter::writeCamera(BinaryWriter& writer, const Camera& cam) {
    SizeEnclosure sizeEnclosure(writer);
    
    writer.writeString(cam.name, 80);
    writer.writeVector3f(cam.position);
    writer.writeFloat32(cam.fieldOfView);
    writer.writeFloat32(cam.farClippingPlane);
    writer.writeFloat32(cam.nearClippingPlane);
    writer.writeVector3f(cam.targetPosition);
    
    writeTrackChunk(writer, KCTR_TAG, cam.positionTracks);
    writeTrackChunk(writer, KCRL_TAG, cam.targetRotationTracks);
    writeTrackChunk(writer, KTTR_TAG, cam.targetPositionTracks);
}

void MDXWriter::writeCLID(BinaryWriter& writer, const MDXFile& mdx) {
    SizeEnclosure sizeEnclosure(writer, CLID_TAG);
    
    for (const auto& shape : mdx.collisionShapes) {
        writeCollisionShape(writer, shape);
    }
}

void MDXWriter::writeCollisionShape(BinaryWriter& writer, const CollisionShape& shape) {
    writeNode(writer, shape.node);
    writer.writeUInt32(shape.type);
    
    writer.writeVector3fArray(shape.vertices);
    
    if (shape.type == 2 || shape.type == 3) {
        writer.writeFloat32(shape.radius);
    }
}

void MDXWriter::writeBPOS(BinaryWriter& writer, const MDXFile& mdx) {
    u32 size = 4 + mdx.bindPoses.size() * 48; // count + matrices
    writeChunkHeader(writer, BPOS_TAG, size);
    
    writer.writeUInt32(mdx.bindPoses.size());
    for (const auto& pose : mdx.bindPoses) {
        for (int i = 0; i < 12; i++) {
            writer.writeFloat32(pose[i]);
        }
    }
}

void MDXWriter::writeFFX(BinaryWriter& writer, const MDXFile& mdx) {
    u32 size = mdx.faceEffects.size() * 340;
    writeChunkHeader(writer, FAFX_TAG, size);
    
    for (const auto& fx : mdx.faceEffects) {
        writer.writeString(fx.target, 80);
        writer.writeString(fx.path, 260);
    }
}

void MDXWriter::writeCORN(BinaryWriter& writer, const MDXFile& mdx) {
    SizeEnclosure sizeEnclosure(writer, CORN_TAG);
    
    for (const auto& corn : mdx.cornEmitters) {
        writeCornEmitter(writer, corn);
    }
}

void MDXWriter::writeCornEmitter(BinaryWriter& writer, const CornEmitter& corn) {
    SizeEnclosure sizeEnclosure(writer);
    
    writeNode(writer, corn.node);
    writer.writeFloat32(corn.lifeSpan);
    writer.writeFloat32(corn.emissionRate);
    writer.writeFloat32(corn.speed);
    writer.writeFloat32(corn.color.x);
    writer.writeFloat32(corn.color.y);
    writer.writeFloat32(corn.color.z);
    writer.writeFloat32(corn.color.w);
    writer.writeUInt32(corn.replaceableId);
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
