// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/mdx/parser.h>
#include "../common/binary_reader.h"
#include "../common/streams.h"

#include <cstring>
#include <fstream>
#include <stdexcept>
#include <streambuf>

namespace whiteout {
namespace mdx {

using common::BinaryReader;

namespace {

template <typename T>
Track<T> readTrackChunk(BinaryReader& reader, u32 trackCount, u32 interpolationType,
                        u32 globalSequenceId) {
    Track<T> track;
    track.isUsed = true;
    track.interpolationType = static_cast<InterpolationType>(interpolationType);
    track.globalSequenceId = globalSequenceId;
    track.keyCount = trackCount;
    size_t keySize = (isSmoothInterpolation(track.interpolationType))
                         ? sizeof(typename Track<T>::TangentKey)
                         : sizeof(typename Track<T>::Key);
    track.keys_data = reader.read<std::vector<u8>>(trackCount * keySize);
    return track;
}

} // namespace

void Parser::SkipUnknownChunk(BinaryReader& reader, u32 tag, u32 size) {
    std::string error =
        "Unknown chunk: " + std::string((char*)&tag, 4) + " (size: " + std::to_string(size) + ")";
    if (parseMode == ParseMode::Strict) {
        throw std::runtime_error(error);
    }
    issues.push_back(error);
    reader.skip(size);
}

void Parser::SkipUnknownTrack(BinaryReader& reader, u32 tag, u32 trackCount,
                              u32 interpolationType) {
    std::string error = "Unknown track: " + std::string((char*)&tag, 4) +
                        " (count: " + std::to_string(trackCount) + ")";
    if (parseMode == ParseMode::Strict) {
        throw std::runtime_error(error);
    }
    issues.push_back(error);
    size_t keySize = (isSmoothInterpolation(static_cast<InterpolationType>(interpolationType)))
                         ? sizeof(typename Track<u32>::TangentKey)
                         : sizeof(typename Track<u32>::Key);
    reader.skip(trackCount * keySize);
}

Model Parser::parse(const std::string& filePath) {
    std::ifstream file;
    file.open(filePath, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open file: " + filePath);
    }
    BinaryReader reader(file);
    return parse(reader);
}

Model Parser::parse(std::span<const u8> buffer) {
    common::span_streambuf streambuf(buffer);
    std::istream in(&streambuf);
    BinaryReader reader(in);
    return parse(reader);
}

Model Parser::parse(BinaryReader& reader) {
    Model mdx;

    // Read magic number
    u32 magic = reader.read<u32>();
    if (magic != MDLX_TAG) {
        std::string error = "Invalid MDX file: expected magic 'MDLX', got '" +
                            std::string(reinterpret_cast<char*>(&magic), 4) + "'";
        if (parseMode == ParseMode::Strict) {
            throw std::runtime_error(error);
        }
        issues.push_back(error);
        return mdx; // Return empty MDX file on failure
    }

    // Parse chunks
    while (reader.hasRemaining()) {
        ChunkHeader header = readChunkHeader(reader);

        switch (header.tag) {
        case VERS_TAG:
            parseVERS(reader, header.size, mdx);
            if (mdx.version > CurrentVersion) {
                std::string error = "Unsupported MDX version: " + std::to_string(mdx.version);
                if (parseMode == ParseMode::Strict) {
                    throw std::runtime_error(error);
                }
                issues.push_back(error);
            }
            break;
        case MODL_TAG:
            parseMODL(reader, header.size, mdx);
            break;
        case SEQS_TAG:
            parseSEQS(reader, header.size, mdx);
            break;
        case GLBS_TAG:
            parseGLBS(reader, header.size, mdx);
            break;
        case TEXS_TAG:
            parseTEXS(reader, header.size, mdx);
            break;
        case SNDS_TAG:
            parseSNDS(reader, header.size, mdx);
            break;
        case MTLS_TAG:
            parseMTLS(reader, header.size, mdx);
            break;
        case TXAN_TAG:
            parseTXAN(reader, header.size, mdx);
            break;
        case GEOS_TAG:
            parseGEOS(reader, header.size, mdx);
            break;
        case GEOA_TAG:
            parseGEOA(reader, header.size, mdx);
            break;
        case BONE_TAG:
            parseBONE(reader, header.size, mdx);
            break;
        case LITE_TAG:
            parseLITE(reader, header.size, mdx);
            break;
        case HELP_TAG:
            parseHELP(reader, header.size, mdx);
            break;
        case ATCH_TAG:
            parseATCH(reader, header.size, mdx);
            break;
        case PIVT_TAG:
            parsePIVT(reader, header.size, mdx);
            break;
        case PREM_TAG:
            parsePREM(reader, header.size, mdx);
            break;
        case PRE2_TAG:
            parsePRE2(reader, header.size, mdx);
            break;
        case RIBB_TAG:
            parseRIBB(reader, header.size, mdx);
            break;
        case EVTS_TAG:
            parseEVTS(reader, header.size, mdx);
            break;
        case CAMS_TAG:
            parseCAMS(reader, header.size, mdx);
            break;
        case CLID_TAG:
            parseCLID(reader, header.size, mdx);
            break;
        case BPOS_TAG:
            parseBPOS(reader, header.size, mdx);
            break;
        case FAFX_TAG:
            parseFAFX(reader, header.size, mdx);
            break;
        case CORN_TAG:
            parseCORN(reader, header.size, mdx);
            break;
        default:
            SkipUnknownChunk(reader, header.tag, header.size);
            break;
        }
    }
    if (upgradeMode == UpgradeMode::UpgradeOldVersions && mdx.version < CurrentVersion) {
        mdx.version = CurrentVersion;
    }
    return mdx;
}

Parser::ChunkHeader Parser::readChunkHeader(BinaryReader& reader) {
    u32 tag = reader.read<u32>();
    u32 size = reader.read<u32>();
    return {tag, size};
}

void Parser::parseVERS(BinaryReader& reader, u32 size, Model& mdx) {
    mdx.version = reader.read<u32>();
}

void Parser::parseMODL(BinaryReader& reader, u32 size, Model& mdx) {
    mdx.modelName = reader.readString(80);
    mdx.animationFileName = reader.readString(260);
    mdx.modelExtent = reader.read<Extent>();
    mdx.blendTime = reader.read<u32>();
}

void Parser::parseSEQS(BinaryReader& reader, u32 size, Model& mdx) {
    u32 count = size / 132;
    mdx.sequences.resize(count);

    for (u32 i = 0; i < count; i++) {
        Sequence& seq = mdx.sequences[i];
        seq.name = reader.readString(80);
        seq.intervalStart = reader.read<u32>();
        seq.intervalEnd = reader.read<u32>();
        seq.moveSpeed = reader.read<f32>();
        seq.flags = reader.read<u32>();
        seq.rarity = reader.read<f32>();
        seq.syncPoint = reader.read<u32>();
        seq.extent = reader.read<Extent>();
    }
}

void Parser::parseGLBS(BinaryReader& reader, u32 size, Model& mdx) {
    u32 count = size / 4;
    mdx.globalSequences = reader.read<std::vector<u32>>(count);
}

void Parser::parseTEXS(BinaryReader& reader, u32 size, Model& mdx) {
    u32 count = size / 268;
    mdx.textures.resize(count);

    for (u32 i = 0; i < count; i++) {
        mdx.textures[i].replaceableId = reader.read<u32>();
        mdx.textures[i].fileName = reader.readString(260);
        mdx.textures[i].flags = reader.read<u32>();
    }
}

void Parser::parseSNDS(BinaryReader& reader, u32 size, Model& mdx) {
    u32 totalRead = 0;

    while (totalRead < size) {
        SoundTrack st;
        st.fileName = reader.readString(260);
        st.volume = reader.read<f32>();
        st.pitch = reader.read<f32>();
        st.flags = reader.read<u32>();

        mdx.soundTracks.push_back(st);
        totalRead += 272;
    }
}

void Parser::parseMTLS(BinaryReader& reader, u32 size, Model& mdx) {
    u32 totalRead = 0;

    while (totalRead < size) {
        Material mat = parseMaterial(reader, size - totalRead, mdx);
        mdx.materials.push_back(mat);
        totalRead += mat.inclusiveSize;
    }
}

Material Parser::parseMaterial(BinaryReader& reader, u32 chunkSize, Model& mdx) {
    Material mat;
    u32 startPos = reader.getPosition();
    bool is_hd = false;

    mat.inclusiveSize = reader.read<u32>();
    mat.priorityPlane = reader.read<u32>();
    mat.flags = reader.read<u32>();

    if (mdx.version > 800 && mdx.version < 1100) {
        mat.shader = reader.readString(80);
        is_hd = !mat.shader.empty();
    }

    // Read LAYS chunk
    u32 laysTag = reader.read<u32>();
    u32 layerCount = reader.read<u32>();

    mat.layers.resize(layerCount);
    for (u32 i = 0; i < layerCount; i++) {
        mat.layers[i] = parseLayer(reader, mdx);
    }

    // Upgrade older versions to newer format
    if (upgradeMode == UpgradeMode::UpgradeOldVersions && mdx.version < 1100) {
        if (is_hd) {
            // Convert to HD layer format
            std::vector<Layer> hdLayers;
            hdLayers.resize(1);
            hdLayers[0] = mat.layers[0]; // First layer is the HD layer

            hdLayers[0].textureId = 0;
            hdLayers[0].textureIdTracks = Track<u32>();
            for (auto& layer : mat.layers) {
                Layer::SubTexture subTex;
                subTex.textureId = layer.textureId;
                subTex.slot = 0;
                subTex.tracks = std::move(layer.textureIdTracks);
                hdLayers[0].subTextures.push_back(subTex);
            }
            hdLayers[0].is_hd = true;

            mat.layers = std::move(hdLayers);
        } else {
            // For non-HD materials, ensure textureId is set correctly
            for (auto& layer : mat.layers) {
                Layer::SubTexture subTex;
                subTex.textureId = layer.textureId;
                subTex.slot = 0;
                subTex.tracks = std::move(layer.textureIdTracks);
                layer.subTextures.push_back(subTex);
                layer.textureId = 0; // Clear textureId since it will be in subTextures
                layer.textureIdTracks = Track<u32>(); // Clear old tracks
            }
        }
    }

    return mat;
}

Layer Parser::parseLayer(BinaryReader& reader, Model& mdx) {
    Layer layer;
    u32 startPos = reader.getPosition();

    layer.inclusiveSize = reader.read<u32>();
    layer.filterMode = static_cast<Layer::FilterMode>(reader.read<u32>());
    layer.shadingFlags = static_cast<Layer::ShadingFlag>(reader.read<u32>());
    layer.textureId = reader.read<u32>();
    layer.textureAnimationId = reader.read<u32>();
    layer.coordId = reader.read<u32>();
    layer.alpha = reader.read<f32>();

    if (mdx.version > 800) {
        layer.emissiveGain = reader.read<f32>();
        layer.fresnelColor = reader.read<Vector3f>();
        layer.fresnelOpacity = reader.read<f32>();
        layer.fresnelTeamColor = reader.read<f32>();
    }

    if (mdx.version >= 1100) {
        layer.textureId = 0;
        const auto is_hd = reader.read<u32>();
        layer.is_hd = is_hd != 0;
        const auto num_textures = reader.read<u32>();
        for (u32 i = 0; i < num_textures; i++) {
            Layer::SubTexture subTex;
            subTex.textureId = reader.read<u32>();
            subTex.slot = reader.read<u32>();

            u32 startPos = reader.getPosition();
            u32 peek_tag = reader.read<u32>();
            if (peek_tag == KMTF_TAG) {
                u32 trackCount = reader.read<u32>();
                u32 interpolationType = reader.read<u32>();
                u32 globalSequenceId = reader.read<u32>();
                subTex.tracks =
                    readTrackChunk<u32>(reader, trackCount, interpolationType, globalSequenceId);
            } else {
                reader.setPosition(startPos);
            }
            layer.subTextures.push_back(subTex);
        }
    }

    const u32 endPos = startPos + layer.inclusiveSize;
    while (reader.getPosition() < endPos) {
        u32 currentPos = reader.getPosition();

        u32 trackTag = reader.read<u32>();
        u32 trackCount = reader.read<u32>();
        u32 interpolationType = reader.read<u32>();
        u32 globalSequenceId = reader.read<u32>();

        switch (trackTag) {
        case KMTF_TAG: // Texture ID (for older versions 800-1100)
            layer.textureIdTracks =
                readTrackChunk<u32>(reader, trackCount, interpolationType, globalSequenceId);
            break;

        case KMTA_TAG: // Alpha
            layer.alphaTracks =
                readTrackChunk<f32>(reader, trackCount, interpolationType, globalSequenceId);
            break;

        case KMTE_TAG: // Emissive gain
            layer.emissiveGainTracks =
                readTrackChunk<f32>(reader, trackCount, interpolationType, globalSequenceId);
            break;

        case KFC3_TAG: // Fresnel color
            layer.fresnelColorTracks =
                readTrackChunk<Vector3f>(reader, trackCount, interpolationType, globalSequenceId);
            break;

        case KFCA_TAG: // Fresnel alpha
            layer.fresnelAlphaTracks =
                readTrackChunk<f32>(reader, trackCount, interpolationType, globalSequenceId);
            break;

        case KFTC_TAG: // Fresnel team color
            layer.fresnelTeamColorTracks =
                readTrackChunk<f32>(reader, trackCount, interpolationType, globalSequenceId);
            break;

        default:
            SkipUnknownTrack(reader, trackTag, trackCount, interpolationType);
            break;
        }
    }

    return layer;
}

void Parser::parseTXAN(BinaryReader& reader, u32 size, Model& mdx) {
    u32 totalRead = 0;

    while (totalRead < size) {
        u32 startPos = reader.getPosition();
        TextureAnimation anim = parseTextureAnimation(reader, size - totalRead);
        mdx.textureAnimations.push_back(anim);
        totalRead += anim.inclusiveSize;
    }
}

TextureAnimation Parser::parseTextureAnimation(BinaryReader& reader, u32 maxSize) {
    TextureAnimation anim;
    u32 startPos = reader.getPosition();

    anim.inclusiveSize = reader.read<u32>();

    // Parse animation tracks (KTAT, KTAR, KTAS)
    u32 endPos = startPos + anim.inclusiveSize;
    while (reader.getPosition() < endPos) {
        u32 trackTag = reader.read<u32>();
        u32 trackCount = reader.read<u32>();
        u32 interpolationType = reader.read<u32>();
        u32 globalSequenceId = reader.read<u32>();

        switch (trackTag) {
        case KTAT_TAG: // KTAT - translation
            anim.translationTracks =
                readTrackChunk<Vector3f>(reader, trackCount, interpolationType, globalSequenceId);
            break;
        case KTAR_TAG: // KTAR - rotation
            anim.rotationTracks =
                readTrackChunk<f32>(reader, trackCount, interpolationType, globalSequenceId);
            break;
        case KTAS_TAG: // KTAS - scaling
            anim.scalingTracks =
                readTrackChunk<Vector3f>(reader, trackCount, interpolationType, globalSequenceId);
            break;
        default:
            SkipUnknownTrack(reader, trackTag, trackCount, interpolationType);
            break;
        }
    }
    return anim;
}

void Parser::parseGEOS(BinaryReader& reader, u32 size, Model& mdx) {
    u32 totalRead = 0;

    while (totalRead < size) {
        u32 startPos = reader.getPosition();
        Geoset geo = parseGeoset(reader, size - totalRead, mdx);

        // Ensure we're at the correct position
        reader.setPosition(startPos + geo.inclusiveSize);

        mdx.geosets.push_back(geo);
        totalRead += geo.inclusiveSize;
    }
}

Geoset Parser::parseGeoset(BinaryReader& reader, u32 maxSize, Model& mdx) {
    Geoset geoset;
    u32 startPos = reader.getPosition();

    geoset.inclusiveSize = reader.read<u32>();

    // Parse VRTX - vertex positions
    u32 vrtxTag = reader.read<u32>();
    u32 vertexCount = reader.read<u32>();
    geoset.vertexPositions = reader.read<std::vector<Vector3f>>(vertexCount);

    // Parse NRMS - normals
    u32 nrmsTag = reader.read<u32>();
    u32 normalCount = reader.read<u32>();
    geoset.vertexNormals = reader.read<std::vector<Vector3f>>(normalCount);

    // Parse PTYP - face type groups
    u32 ptypTag = reader.read<u32>();
    u32 faceTypeGroupCount = reader.read<u32>();
    geoset.faceTypeGroups = reader.read<std::vector<u32>>(faceTypeGroupCount);

    // Parse PCNT - face groups
    u32 pcntTag = reader.read<u32>();
    u32 faceGroupCount = reader.read<u32>();
    geoset.faceGroups = reader.read<std::vector<u32>>(faceGroupCount);

    // Parse PVTX - face indices
    u32 pvtxTag = reader.read<u32>();
    u32 faceCount = reader.read<u32>();
    geoset.faces = reader.read<std::vector<u16>>(faceCount);

    // Parse GNDX - vertex groups
    u32 gndxTag = reader.read<u32>();
    u32 vertexGroupCount = reader.read<u32>();
    geoset.vertexGroups = reader.read<std::vector<u8>>(vertexGroupCount);

    // Parse MTGC - matrix groups
    u32 mtgcTag = reader.read<u32>();
    u32 matrixGroupCount = reader.read<u32>();
    geoset.matrixGroups = reader.read<std::vector<u32>>(matrixGroupCount);

    // Parse MATS - matrix indices
    u32 matsTag = reader.read<u32>();
    u32 matrixIndexCount = reader.read<u32>();
    geoset.matrixIndices = reader.read<std::vector<u32>>(matrixIndexCount);

    // Material ID, selection group, selection flags
    geoset.materialId = reader.read<u32>();
    geoset.selectionGroup = reader.read<u32>();
    geoset.selectionFlags = reader.read<u32>();

    // LOD fields (Reforged only)
    if (mdx.version > 800) {
        geoset.lod = reader.read<u32>();
        geoset.lodName = reader.readString(80);
    } else {
        geoset.lod = 0;
        geoset.lodName = "";
    }

    // Extent
    geoset.extent = reader.read<Extent>();

    // Sequence extents
    u32 extentsCount = reader.read<u32>();
    geoset.sequenceExtents.resize(extentsCount);
    for (u32 i = 0; i < extentsCount; i++) {
        geoset.sequenceExtents[i] = reader.read<Extent>();
    }

    // Parse UVAS - texture coordinate sets
    u32 currentPos = reader.getPosition();
    u32 endPos = startPos + geoset.inclusiveSize;

    while (currentPos < endPos) {
        u32 peekTag = reader.read<u32>();
        switch (peekTag) {
        case UVAS_TAG: {
            u32 uvSetCount = reader.read<u32>();

            geoset.textureCoordinateSets.resize(uvSetCount);
            for (u32 i = 0; i < uvSetCount; i++) {
                u32 uvbsTag = reader.read<u32>();
                u32 uvCount = reader.read<u32>();
                geoset.textureCoordinateSets[i] = reader.read<std::vector<Vector2f>>(uvCount);
            }
            break;
        }
        case TANG_TAG: {
            u32 tangentCount = reader.read<u32>();
            geoset.tangents = reader.read<std::vector<Vector4f>>(tangentCount);
            break;
        }
        case SKIN_TAG: {
            u32 skinDataCount = reader.read<u32>();
            geoset.skinData = reader.read<std::vector<u8>>(skinDataCount);
            break;
        }
        default: {
            std::string error = "Unknown chunk in geoset: " + std::string((char*)&peekTag, 4);
            if (parseMode == ParseMode::Strict) {
                throw std::runtime_error(error);
            }
            issues.push_back(error);
            reader.skip(4); // Skip the size field of the unknown chunk
            u32 unknownSize = reader.read<u32>();
            reader.skip(unknownSize * 4);
            break;
        }
        }
        currentPos = reader.getPosition();
    }

    return geoset;
}

void Parser::parseGEOA(BinaryReader& reader, u32 size, Model& mdx) {
    u32 totalRead = 0;

    while (totalRead < size) {
        u32 startPos = reader.getPosition();

        GeosetAnimation anim;
        anim.inclusiveSize = reader.read<u32>();
        anim.alpha = reader.read<f32>();
        anim.flags = reader.read<u32>();
        anim.color = reader.read<Vector3f>();
        anim.geosetId = reader.read<u32>();

        // Parse animation tracks (KGAO, KGAC)
        u32 endPos = startPos + anim.inclusiveSize;
        while (reader.getPosition() < endPos) {
            u32 trackTag = reader.read<u32>();
            u32 trackCount = reader.read<u32>();
            u32 interpolationType = reader.read<u32>();
            u32 globalSequenceId = reader.read<u32>();

            switch (trackTag) {
            case KGAO_TAG: // KGAO - alpha
                anim.alphaTracks =
                    readTrackChunk<f32>(reader, trackCount, interpolationType, globalSequenceId);
                break;
            case KGAC_TAG: // KGAC - color
                anim.colorTracks = readTrackChunk<Vector3f>(reader, trackCount, interpolationType,
                                                            globalSequenceId);
                break;
            default:
                SkipUnknownTrack(reader, trackTag, trackCount, interpolationType);
                break;
            }
        }

        mdx.geosetAnimations.push_back(anim);
        totalRead += anim.inclusiveSize;
    }
}

void Parser::parseBONE(BinaryReader& reader, u32 size, Model& mdx) {
    u32 totalRead = 0;

    while (totalRead < size) {
        Bone bone;

        u32 startPos = reader.getPosition();
        bone.node = parseNode(reader);

        bone.geosetId = reader.read<u32>();
        bone.geosetAnimationId = reader.read<u32>();

        bone.node.type = Node::NodeType::Bone; // Set node type to Bone
        bone.node.nodeFamilyId =
            mdx.bones.size(); // Assign a unique family ID based on current bone count

        mdx.bones.push_back(bone);

        u32 endPos = reader.getPosition();
        totalRead += (endPos - startPos);
    }
}

Node Parser::parseNode(BinaryReader& reader) {
    Node node;
    u32 startPos = reader.getPosition();
    u32 nodeSize = reader.read<u32>();

    node.inclusiveSize = nodeSize;
    node.name = reader.readString(80);
    node.objectId = reader.read<u32>();
    node.parentId = reader.read<u32>();
    node.flags = static_cast<Node::NodeFlag>(reader.read<u32>());

    parseNodeTracks(reader, node, nodeSize);

    return node;
}

void Parser::parseNodeTracks(BinaryReader& reader, Node& node, u32 nodeSize) {
    u32 nodeDataSize = 4 + 80 + 4 + 4 + 4; // inclusiveSize + name + objectId + parentId + flags
    u32 tracksSize = nodeSize - nodeDataSize;

    u32 startPos = reader.getPosition();

    while (reader.getPosition() - startPos < tracksSize) {
        if (reader.getPosition() - startPos >= tracksSize)
            break;

        u32 trackTag = reader.read<u32>();
        u32 trackCount = reader.read<u32>();
        u32 interpolationType = reader.read<u32>();
        u32 globalSequenceId = reader.read<u32>();

        switch (trackTag) {
        case KGTR_TAG:
            node.translationTracks =
                readTrackChunk<Vector3f>(reader, trackCount, interpolationType, globalSequenceId);
            break;
        case KGRT_TAG:
            node.rotationTracks =
                readTrackChunk<Vector4f>(reader, trackCount, interpolationType, globalSequenceId);
            break;
        case KGSC_TAG:
            node.scalingTracks =
                readTrackChunk<Vector3f>(reader, trackCount, interpolationType, globalSequenceId);
            break;
        default:
            SkipUnknownTrack(reader, trackTag, trackCount, interpolationType);
            break;
        }
    }
}

void Parser::parseLITE(BinaryReader& reader, u32 size, Model& mdx) {
    u32 totalRead = 0;

    while (totalRead < size) {
        u32 startPos = reader.getPosition();
        Light light = parseLight(reader, size - totalRead, mdx);
        light.node.type = Node::NodeType::Light; // Set node type to Light
        light.node.nodeFamilyId =
            mdx.lights.size(); // Assign a unique family ID based on current light count
        mdx.lights.push_back(light);
        totalRead += light.inclusiveSize;
    }
}

Light Parser::parseLight(BinaryReader& reader, u32 maxSize, Model& mdx) {
    Light light;
    u32 startPos = reader.getPosition();

    light.inclusiveSize = reader.read<u32>();
    light.node = parseNode(reader);
    light.type = static_cast<Light::LightType>(reader.read<u32>());
    light.attenuationStart = reader.read<f32>();
    light.attenuationEnd = reader.read<f32>();
    light.color = reader.read<Vector3f>();
    light.intensity = reader.read<f32>();
    light.ambientColor = reader.read<Vector3f>();
    light.ambientIntensity = reader.read<f32>();
    if (mdx.version >= 1200) {
        light.shadowIntensity = reader.read<f32>();
    } else {
        light.shadowIntensity = 0.4f; // Default value for older versions
    }

    // Parse animation tracks (KLAS, KLAE, KLAC, KLAI, KLBI, KLBC, KLAV)
    u32 endPos = startPos + light.inclusiveSize;
    while (reader.getPosition() < endPos) {
        u32 trackTag = reader.read<u32>();
        u32 trackCount = reader.read<u32>();
        u32 interpolationType = reader.read<u32>();
        u32 globalSequenceId = reader.read<u32>();

        switch (trackTag) {
        case KLAS_TAG: // KLAS - attenuationStart
            light.attenuationStartTracks =
                readTrackChunk<f32>(reader, trackCount, interpolationType, globalSequenceId);
            break;
        case KLAE_TAG: // KLAE - attenuationEnd
            light.attenuationEndTracks =
                readTrackChunk<f32>(reader, trackCount, interpolationType, globalSequenceId);
            break;
        case KLAC_TAG: // KLAC - color
            light.colorTracks =
                readTrackChunk<Vector3f>(reader, trackCount, interpolationType, globalSequenceId);
            break;
        case KLAI_TAG: // KLAI - intensity
            light.intensityTracks =
                readTrackChunk<f32>(reader, trackCount, interpolationType, globalSequenceId);
            break;
        case KLBI_TAG: // KLBI - ambientIntensity
            light.ambientIntensityTracks =
                readTrackChunk<f32>(reader, trackCount, interpolationType, globalSequenceId);
            break;
        case KLBC_TAG: // KLBC - ambientColor
            light.ambientColorTracks =
                readTrackChunk<Vector3f>(reader, trackCount, interpolationType, globalSequenceId);
            break;
        case KLAV_TAG: // KLAV - visibility
            light.visibilityTracks =
                readTrackChunk<f32>(reader, trackCount, interpolationType, globalSequenceId);
            break;
        default:
            SkipUnknownTrack(reader, trackTag, trackCount, interpolationType);
            break;
        }
    }

    return light;
}

void Parser::parseHELP(BinaryReader& reader, u32 size, Model& mdx) {
    u32 totalRead = 0;

    while (totalRead < size) {
        u32 startPos = reader.getPosition();

        Helper helper;
        helper.node = parseNode(reader);

        helper.node.type = Node::NodeType::Helper; // Set node type to Helper
        helper.node.nodeFamilyId =
            mdx.helpers.size(); // Assign a unique family ID based on current helper count

        mdx.helpers.push_back(helper);

        u32 endPos = reader.getPosition();
        totalRead += (endPos - startPos);
    }
}

void Parser::parseATCH(BinaryReader& reader, u32 size, Model& mdx) {
    u32 totalRead = 0;

    while (totalRead < size) {
        Attachment att = parseAttachment(reader, size - totalRead);

        att.node.type = Node::NodeType::Attachment; // Set node type to Attachment
        att.node.nodeFamilyId =
            mdx.attachments.size(); // Assign a unique family ID based on current attachment count

        mdx.attachments.push_back(att);
        totalRead += att.inclusiveSize;
    }
}

Attachment Parser::parseAttachment(BinaryReader& reader, u32 maxSize) {
    Attachment att;
    u32 startPos = reader.getPosition();

    att.inclusiveSize = reader.read<u32>();
    att.node = parseNode(reader);
    att.path = reader.readString(260);
    att.attachmentId = reader.read<u32>();

    // Parse animation tracks (KATV)
    u32 endPos = startPos + att.inclusiveSize;
    while (reader.getPosition() < endPos) {
        u32 trackTag = reader.read<u32>();
        u32 trackCount = reader.read<u32>();
        u32 interpolationType = reader.read<u32>();
        u32 globalSequenceId = reader.read<u32>();

        switch (trackTag) {
        case KATV_TAG: // KATV - visibility
            att.visibilityTracks =
                readTrackChunk<f32>(reader, trackCount, interpolationType, globalSequenceId);
            break;
        default:
            SkipUnknownTrack(reader, trackTag, trackCount, interpolationType);
            break;
        }
    }

    return att;
}

void Parser::parsePIVT(BinaryReader& reader, u32 size, Model& mdx) {
    u32 count = size / 12;
    mdx.pivotPoints.resize(count);

    for (u32 i = 0; i < count; i++) {
        mdx.pivotPoints[i] = reader.read<Vector3f>();
    }
}

void Parser::parsePREM(BinaryReader& reader, u32 size, Model& mdx) {
    u32 totalRead = 0;

    while (totalRead < size) {
        ParticleEmitter pem = parseParticleEmitter(reader, size - totalRead);
        pem.node.type = Node::NodeType::ParticleEmitter; // Set node type to ParticleEmitter
        pem.node.nodeFamilyId =
            mdx.particleEmitters
                .size(); // Assign a unique family ID based on current particle emitter count

        mdx.particleEmitters.push_back(pem);
        totalRead += pem.inclusiveSize;
    }
}

ParticleEmitter Parser::parseParticleEmitter(BinaryReader& reader, u32 maxSize) {
    ParticleEmitter pem;
    u32 startPos = reader.getPosition();

    pem.inclusiveSize = reader.read<u32>();
    pem.node = parseNode(reader);
    pem.emissionRate = reader.read<f32>();
    pem.gravity = reader.read<f32>();
    pem.longitude = reader.read<f32>();
    pem.latitude = reader.read<f32>();
    pem.spawnModelFileName = reader.readString(260);
    pem.lifespan = reader.read<f32>();
    pem.initialVelocity = reader.read<f32>();

    // Parse animation tracks (KPEE, KPEG, KPLN, KPLT, KPEL, KPES, KPEV)
    u32 endPos = startPos + pem.inclusiveSize;
    while (reader.getPosition() < endPos) {
        u32 trackTag = reader.read<u32>();
        u32 trackCount = reader.read<u32>();
        u32 interpolationType = reader.read<u32>();
        u32 globalSequenceId = reader.read<u32>();

        switch (trackTag) {
        case KPEE_TAG: // KPEE - emissionRate
            pem.emissionRateTracks =
                readTrackChunk<f32>(reader, trackCount, interpolationType, globalSequenceId);
            break;
        case KPEG_TAG: // KPEG - gravity
            pem.gravityTracks =
                readTrackChunk<f32>(reader, trackCount, interpolationType, globalSequenceId);
            break;
        case KPLN_TAG: // KPLN - longitude
            pem.longitudeTracks =
                readTrackChunk<f32>(reader, trackCount, interpolationType, globalSequenceId);
            break;
        case KPLT_TAG: // KPLT - latitude
            pem.latitudeTracks =
                readTrackChunk<f32>(reader, trackCount, interpolationType, globalSequenceId);
            break;
        case KPEL_TAG: // KPEL - lifespan
            pem.lifespanTracks =
                readTrackChunk<f32>(reader, trackCount, interpolationType, globalSequenceId);
            break;
        case KPES_TAG: // KPES - speed
            pem.speedTracks =
                readTrackChunk<f32>(reader, trackCount, interpolationType, globalSequenceId);
            break;
        case KPEV_TAG: // KPEV - visibility
            pem.visibilityTracks =
                readTrackChunk<f32>(reader, trackCount, interpolationType, globalSequenceId);
            break;
        default:
            SkipUnknownTrack(reader, trackTag, trackCount, interpolationType);
            break;
        }
    }

    return pem;
}

void Parser::parsePRE2(BinaryReader& reader, u32 size, Model& mdx) {
    u32 totalRead = 0;

    while (totalRead < size) {
        ParticleEmitter2 pem2 = parseParticleEmitter2(reader, size - totalRead);
        pem2.node.type = Node::NodeType::ParticleEmitter2; // Set node type to ParticleEmitter2
        pem2.node.nodeFamilyId =
            mdx.particleEmitters2
                .size(); // Assign a unique family ID based on current particle emitter count

        mdx.particleEmitters2.push_back(pem2);
        totalRead += pem2.inclusiveSize;
    }
}

ParticleEmitter2 Parser::parseParticleEmitter2(BinaryReader& reader, u32 maxSize) {
    ParticleEmitter2 pem2;
    u32 startPos = reader.getPosition();

    pem2.inclusiveSize = reader.read<u32>();
    pem2.node = parseNode(reader);
    pem2.speed = reader.read<f32>();
    pem2.variation = reader.read<f32>();
    pem2.latitude = reader.read<f32>();
    pem2.gravity = reader.read<f32>();
    pem2.lifespan = reader.read<f32>();
    pem2.emissionRate = reader.read<f32>();
    pem2.length = reader.read<f32>();
    pem2.width = reader.read<f32>();

    pem2.filterMode = reader.read<u32>();
    pem2.rows = reader.read<u32>();
    pem2.columns = reader.read<u32>();
    pem2.headOrTail = reader.read<u32>();

    pem2.tailLength = reader.read<f32>();
    pem2.time = reader.read<f32>();

    for (int i = 0; i < 3; i++) {
        pem2.segmentColor[i] = reader.read<Vector3f>();
    }
    for (int i = 0; i < 3; i++) {
        pem2.segmentAlpha[i] = reader.read<u8>();
    }
    for (int i = 0; i < 3; i++) {
        pem2.segmentScaling[i] = reader.read<f32>();
    }

    for (int i = 0; i < 3; i++) {
        pem2.headInterval[i] = reader.read<u32>();
    }
    for (int i = 0; i < 3; i++) {
        pem2.headDecayInterval[i] = reader.read<u32>();
    }
    for (int i = 0; i < 3; i++) {
        pem2.tailInterval[i] = reader.read<u32>();
    }
    for (int i = 0; i < 3; i++) {
        pem2.tailDecayInterval[i] = reader.read<u32>();
    }

    pem2.textureId = reader.read<u32>();
    pem2.squirt = reader.read<u32>();
    pem2.priorityPlane = reader.read<u32>();
    pem2.replaceableId = reader.read<u32>();

    // Parse animation tracks (KP2S, KP2R, KP2L, KP2G, KP2E, KP2N, KP2W, KP2V)
    u32 endPos = startPos + pem2.inclusiveSize;
    while (reader.getPosition() < endPos) {
        u32 trackTag = reader.read<u32>();
        u32 trackCount = reader.read<u32>();
        u32 interpolationType = reader.read<u32>();
        u32 globalSequenceId = reader.read<u32>();

        switch (trackTag) {
        case KP2S_TAG: // KP2S - speed
            pem2.speedTracks =
                readTrackChunk<f32>(reader, trackCount, interpolationType, globalSequenceId);
            break;
        case KP2R_TAG: // KP2R - variation
            pem2.variationTracks =
                readTrackChunk<f32>(reader, trackCount, interpolationType, globalSequenceId);
            break;
        case KP2L_TAG: // KP2L - latitude
            pem2.latitudeTracks =
                readTrackChunk<f32>(reader, trackCount, interpolationType, globalSequenceId);
            break;
        case KP2G_TAG: // KP2G - gravity
            pem2.gravityTracks =
                readTrackChunk<f32>(reader, trackCount, interpolationType, globalSequenceId);
            break;
        case KP2E_TAG: // KP2E - emission rate
            pem2.emissionRateTracks =
                readTrackChunk<f32>(reader, trackCount, interpolationType, globalSequenceId);
            break;
        case KP2N_TAG: // KP2N - length
            pem2.lengthTracks =
                readTrackChunk<f32>(reader, trackCount, interpolationType, globalSequenceId);
            break;
        case KP2W_TAG: // KP2W - width
            pem2.widthTracks =
                readTrackChunk<f32>(reader, trackCount, interpolationType, globalSequenceId);
            break;
        case KP2V_TAG: // KP2V - visibility
            pem2.visibilityTracks =
                readTrackChunk<f32>(reader, trackCount, interpolationType, globalSequenceId);
            break;
        default:
            SkipUnknownTrack(reader, trackTag, trackCount, interpolationType);
            break;
        }
    }

    return pem2;
}

void Parser::parseRIBB(BinaryReader& reader, u32 size, Model& mdx) {
    u32 totalRead = 0;

    while (totalRead < size) {
        RibbonEmitter ribb = parseRibbonEmitter(reader, size - totalRead);
        ribb.node.type = Node::NodeType::RibbonEmitter; // Set node type to RibbonEmitter
        ribb.node.nodeFamilyId =
            mdx.ribbonEmitters
                .size(); // Assign a unique family ID based on current ribbon emitter count

        mdx.ribbonEmitters.push_back(ribb);
        totalRead += ribb.inclusiveSize;
    }
}

RibbonEmitter Parser::parseRibbonEmitter(BinaryReader& reader, u32 maxSize) {
    RibbonEmitter ribb;
    u32 startPos = reader.getPosition();

    ribb.inclusiveSize = reader.read<u32>();
    ribb.node = parseNode(reader);
    ribb.heightAbove = reader.read<f32>();
    ribb.heightBelow = reader.read<f32>();
    ribb.alpha = reader.read<f32>();
    ribb.color = reader.read<Vector3f>();
    ribb.lifespan = reader.read<f32>();
    ribb.textureSlot = reader.read<u32>();
    ribb.emissionRate = reader.read<u32>();
    ribb.rows = reader.read<u32>();
    ribb.columns = reader.read<u32>();
    ribb.materialId = reader.read<u32>();
    ribb.gravity = reader.read<f32>();

    // Parse animation tracks (KRHA, KRHB, KRAL, KRCO, KRTX, KRVS)
    u32 endPos = startPos + ribb.inclusiveSize;
    while (reader.getPosition() < endPos) {
        u32 trackTag = reader.read<u32>();
        u32 trackCount = reader.read<u32>();
        u32 interpolationType = reader.read<u32>();
        u32 globalSequenceId = reader.read<u32>();

        switch (trackTag) {
        case KRHA_TAG: // KRHA - heightAbove
            ribb.heightAboveTracks =
                readTrackChunk<f32>(reader, trackCount, interpolationType, globalSequenceId);
            break;
        case KRHB_TAG: // KRHB - heightBelow
            ribb.heightBelowTracks =
                readTrackChunk<f32>(reader, trackCount, interpolationType, globalSequenceId);
            break;
        case KRAL_TAG: // KRAL - alpha
            ribb.alphaTracks =
                readTrackChunk<f32>(reader, trackCount, interpolationType, globalSequenceId);
            break;
        case KRCO_TAG: // KRCO - color
            ribb.colorTracks =
                readTrackChunk<Vector3f>(reader, trackCount, interpolationType, globalSequenceId);
            break;
        case KRTX_TAG: // KRTX - textureSlot
            ribb.textureSlotTracks =
                readTrackChunk<u32>(reader, trackCount, interpolationType, globalSequenceId);
            break;
        case KRVS_TAG: // KRVS - visibility
            ribb.visibilityTracks =
                readTrackChunk<f32>(reader, trackCount, interpolationType, globalSequenceId);
            break;
        default:
            SkipUnknownTrack(reader, trackTag, trackCount, interpolationType);
            break;
        }
    }

    return ribb;
}

void Parser::parseEVTS(BinaryReader& reader, u32 size, Model& mdx) {
    u32 totalRead = 0;

    while (totalRead < size) {
        u32 startPos = reader.getPosition();

        EventObject evt;
        evt.node = parseNode(reader);
        evt.node.type = Node::NodeType::EventObject; // Set node type to EventObject
        evt.node.nodeFamilyId =
            mdx.eventObjects
                .size(); // Assign a unique family ID based on current event object count

        // Read KEVT chunk
        u32 kevtTag = reader.read<u32>();
        u32 trackCount = reader.read<u32>();

        for (u32 i = 0; i < trackCount; i++) {
            evt.eventTrackTimes.push_back(reader.read<u32>());
        }

        evt.globalSequenceId = reader.read<u32>();

        mdx.eventObjects.push_back(evt);

        u32 endPos = reader.getPosition();
        totalRead += (endPos - startPos);
    }
}

void Parser::parseCAMS(BinaryReader& reader, u32 size, Model& mdx) {
    u32 totalRead = 0;

    while (totalRead < size) {
        Camera cam = parseCamera(reader, size - totalRead);
        mdx.cameras.push_back(cam);
        totalRead += cam.inclusiveSize;
    }
}

Camera Parser::parseCamera(BinaryReader& reader, u32 maxSize) {
    Camera cam;
    u32 startPos = reader.getPosition();

    cam.inclusiveSize = reader.read<u32>();
    cam.name = reader.readString(80);
    cam.position = reader.read<Vector3f>();
    cam.fieldOfView = reader.read<f32>();
    cam.farClippingPlane = reader.read<f32>();
    cam.nearClippingPlane = reader.read<f32>();
    cam.targetPosition = reader.read<Vector3f>();

    // Parse animation tracks (KCTR, KCRL, KTTR)
    u32 endPos = startPos + cam.inclusiveSize;
    while (reader.getPosition() < endPos) {
        u32 trackTag = reader.read<u32>();
        u32 trackCount = reader.read<u32>();
        u32 interpolationType = reader.read<u32>();
        u32 globalSequenceId = reader.read<u32>();

        switch (trackTag) {
        case KCTR_TAG: // KCTR - position
            cam.positionTracks =
                readTrackChunk<Vector3f>(reader, trackCount, interpolationType, globalSequenceId);
            break;
        case KCRL_TAG: // KCRL - targetRotation (as quaternion/angle, read as float)
            cam.targetRotationTracks =
                readTrackChunk<f32>(reader, trackCount, interpolationType, globalSequenceId);
            break;
        case KTTR_TAG: // KTTR - targetPosition
            cam.targetPositionTracks =
                readTrackChunk<Vector3f>(reader, trackCount, interpolationType, globalSequenceId);
            break;
        default:
            SkipUnknownTrack(reader, trackTag, trackCount, interpolationType);
            break;
        }
    }

    return cam;
}

void Parser::parseCLID(BinaryReader& reader, u32 size, Model& mdx) {
    u32 totalRead = 0;

    while (totalRead < size) {
        u32 startPos = reader.getPosition();

        CollisionShape shape = parseCollisionShape(reader);
        shape.node.type = Node::NodeType::CollisionShape; // Set node type to CollisionShape
        shape.node.nodeFamilyId =
            mdx.collisionShapes
                .size(); // Assign a unique family ID based on current collision shape count
        mdx.collisionShapes.push_back(shape);

        u32 endPos = reader.getPosition();
        totalRead += (endPos - startPos);
    }
}

CollisionShape Parser::parseCollisionShape(BinaryReader& reader) {
    CollisionShape shape;
    shape.node = parseNode(reader);
    u32 type_index = reader.read<u32>();
    shape.type = static_cast<CollisionShape::ShapeType>(type_index);

    constexpr std::array<size_t, 4> shapeVertexCounts = {2, 2, 1, 2};

    u32 vertexCount = shapeVertexCounts[type_index];

    shape.vertices.resize(vertexCount);
    for (u32 i = 0; i < vertexCount; i++) {
        shape.vertices[i] = reader.read<Vector3f>();
    }

    if (shape.type == CollisionShape::ShapeType::Sphere ||
        shape.type == CollisionShape::ShapeType::Cylinder) {
        shape.radius = reader.read<f32>();
    }

    return shape;
}

void Parser::parseBPOS(BinaryReader& reader, u32 size, Model& mdx) {
    u32 count = reader.read<u32>();
    mdx.bindPoses.resize(count);

    for (u32 i = 0; i < count; i++) {
        for (int j = 0; j < 12; j++) {
            mdx.bindPoses[i][j] = reader.read<f32>();
        }
    }
}

void Parser::parseFAFX(BinaryReader& reader, u32 size, Model& mdx) {
    u32 count = size / 340;
    mdx.faceEffects.resize(count);

    for (u32 i = 0; i < count; i++) {
        mdx.faceEffects[i].target = reader.readString(80);
        mdx.faceEffects[i].path = reader.readString(260);
    }
}

void Parser::parseCORN(BinaryReader& reader, u32 size, Model& mdx) {
    u32 totalRead = 0;

    while (totalRead < size) {
        CornEmitter corn;
        u32 startPos = reader.getPosition();

        corn.inclusiveSize = reader.read<u32>();
        corn.node = parseNode(reader);
        corn.node.type = Node::NodeType::CornEmitter; // Set node type to CornEmitter
        corn.node.nodeFamilyId =
            mdx.cornEmitters
                .size(); // Assign a unique family ID based on current corn emitter count

        corn.lifeSpan = reader.read<f32>();
        corn.emissionRate = reader.read<f32>();
        corn.speed = reader.read<f32>();
        corn.color = reader.read<Vector4f>();
        corn.replaceableId = reader.read<u32>();
        corn.path = reader.readString(260);
        corn.animVisibilityGuide = reader.readString(260);

        // Parse animation tracks (KPPA, KPPC, KPPE, KPPL, KPPS, KPPV)
        u32 endPos = startPos + corn.inclusiveSize;
        while (reader.getPosition() < endPos) {
            u32 trackTag = reader.read<u32>();
            u32 trackCount = reader.read<u32>();
            u32 interpolationType = reader.read<u32>();
            u32 globalSequenceId = reader.read<u32>();

            switch (trackTag) {
            case KPPA_TAG: // KPPA - lifeSpan
                corn.lifeSpanTracks =
                    readTrackChunk<f32>(reader, trackCount, interpolationType, globalSequenceId);
                break;
            case KPPC_TAG: // KPPC - color
                corn.colorTracks = readTrackChunk<Vector4f>(reader, trackCount, interpolationType,
                                                            globalSequenceId);
                break;
            case KPPE_TAG: // KPPE - emissionRate
                corn.emissionRateTracks =
                    readTrackChunk<f32>(reader, trackCount, interpolationType, globalSequenceId);
                break;
            case KPPL_TAG: // KPPL - lifeSpanVariation (reading as float)
                corn.lifeSpanVariationTracks =
                    readTrackChunk<f32>(reader, trackCount, interpolationType, globalSequenceId);
                break;
            case KPPS_TAG: // KPPS - speed
                corn.speedTracks =
                    readTrackChunk<f32>(reader, trackCount, interpolationType, globalSequenceId);
                break;
            case KPPV_TAG: // KPPV - visibility
                corn.visibilityTracks =
                    readTrackChunk<f32>(reader, trackCount, interpolationType, globalSequenceId);
                break;
            default:
                SkipUnknownTrack(reader, trackTag, trackCount, interpolationType);
                break;
            }
        }

        mdx.cornEmitters.push_back(corn);
        totalRead += corn.inclusiveSize;
    }
}

} // namespace mdx
} // namespace whiteout
