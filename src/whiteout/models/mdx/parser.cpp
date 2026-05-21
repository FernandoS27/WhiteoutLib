// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/mdx/parser.h>
#include "../../common/binary_reader.h"
#include "../../common/streams.h"
#include "../../common/unicode_path.h"
#include "mdl_converter.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <streambuf>

namespace whiteout {
namespace mdx {

using common::BinaryReader;

// ============================================================================
// ParserImpl - Implementation class using PImpl idiom
// ============================================================================

class Parser::Impl {
public:
    ParseMode parseMode = ParseMode::Lenient;
    UpgradeMode upgradeMode = UpgradeMode::UpgradeOldVersions;
    std::vector<std::string> issues;
    static constexpr u32 CurrentVersion = 1200; ///< Latest known MDX version (Reforged)

    // Internal helper methods for parsing
    void SkipUnknownChunk(BinaryReader& reader, u32 tag, u32 size);
    void SkipUnknownTrack(BinaryReader& reader, u32 tag, u32 trackCount, u32 interpolationType);

    struct ChunkHeader {
        u32 tag;  ///< Chunk identifier
        u32 size; ///< Chunk data size in bytes
    };

    ChunkHeader readChunkHeader(BinaryReader& reader);

    Model parse(BinaryReader& reader);

    // Chunk-specific parsers - each handles one MDX chunk type
    void parseVERS(BinaryReader& reader, u32 size, Model& mdx);
    void parseMODL(BinaryReader& reader, u32 size, Model& mdx);
    void parseSEQS(BinaryReader& reader, u32 size, Model& mdx);
    void parseGLBS(BinaryReader& reader, u32 size, Model& mdx);
    void parseTEXS(BinaryReader& reader, u32 size, Model& mdx);
    void parseSNDS(BinaryReader& reader, u32 size, Model& mdx);
    void parseSNEM(BinaryReader& reader, u32 size, Model& mdx);
    void parseMTLS(BinaryReader& reader, u32 size, Model& mdx);
    void parseTXAN(BinaryReader& reader, u32 size, Model& mdx);
    void parseGEOS(BinaryReader& reader, u32 size, Model& mdx);
    void parseGEOA(BinaryReader& reader, u32 size, Model& mdx);
    void parseBONE(BinaryReader& reader, u32 size, Model& mdx);
    void parseLITE(BinaryReader& reader, u32 size, Model& mdx);
    void parseHELP(BinaryReader& reader, u32 size, Model& mdx);
    void parseATCH(BinaryReader& reader, u32 size, Model& mdx);
    void parsePIVT(BinaryReader& reader, u32 size, Model& mdx);
    void parsePREM(BinaryReader& reader, u32 size, Model& mdx);
    void parsePRE2(BinaryReader& reader, u32 size, Model& mdx);
    void parseRIBB(BinaryReader& reader, u32 size, Model& mdx);
    void parseEVTS(BinaryReader& reader, u32 size, Model& mdx);
    void parseCAMS(BinaryReader& reader, u32 size, Model& mdx);
    void parseCLID(BinaryReader& reader, u32 size, Model& mdx);
    void parseBPOS(BinaryReader& reader, u32 size, Model& mdx);
    void parseFAFX(BinaryReader& reader, u32 size, Model& mdx);
    void parseCORN(BinaryReader& reader, u32 size, Model& mdx);

    // Structure parsers - parse individual structure types
    Node parseNode(BinaryReader& reader);
    void parseNodeTracks(BinaryReader& reader, Node& node, u32 nodeSize);

    Material parseMaterial(BinaryReader& reader, u32 chunkSize, Model& mdx);
    Layer parseLayer(BinaryReader& reader, Model& mdx);

    Geoset parseGeoset(BinaryReader& reader, u32 maxSize, Model& mdx);

    TextureAnimation parseTextureAnimation(BinaryReader& reader, u32 maxSize);

    Attachment parseAttachment(BinaryReader& reader, u32 maxSize);
    ParticleEmitter parseParticleEmitter(BinaryReader& reader, u32 maxSize);
    ParticleEmitter2 parseParticleEmitter2(BinaryReader& reader, u32 maxSize);
    RibbonEmitter parseRibbonEmitter(BinaryReader& reader, u32 maxSize);
    Camera parseCamera(BinaryReader& reader, u32 maxSize);
    Light parseLight(BinaryReader& reader, u32 maxSize, Model& mdx);
    CollisionShape parseCollisionShape(BinaryReader& reader);
    SoundEmitter parseSoundEmitter(BinaryReader& reader, u32 maxSize);

    void upgradeMaterials(Model& mdx);

    /**
     * @brief Parse animation tracks
     * @tparam T Track value type (f32, Vector3f, etc.)
     * @param reader Binary reader
     * @param tag Track chunk tag
     * @param interpolationType Interpolation mode
     * @param trackCount Number of tracks to parse
     * @return Vector of parsed tracks
     */
    template <typename T>
    std::vector<Track<T>> parseTracks(BinaryReader& reader, u32 tag, u32 interpolationType,
                                      u32 trackCount);

    template <typename T>
    Track<T> readTrackChunk(BinaryReader& reader, u32 trackCount, u32 interpolationType,
                            u32 globalSequenceId);

    std::vector<u8> key_buffer; // Temporary buffer for reading track data
};

template <typename T>
Track<T> Parser::Impl::readTrackChunk(BinaryReader& reader, u32 trackCount, u32 interpolationType,
                        u32 globalSequenceId) {
    Track<T> track;
    track.isUsed = true;
    track.interpolationType = static_cast<InterpolationType>(interpolationType);
    track.globalSequenceId = globalSequenceId;
    track.keyCount = trackCount;

    size_t stride = sizeof(u32) + sizeof(T);
    size_t keyStride = sizeof(T);
    size_t key_components = 1;
    track.timestamps.resize(trackCount);
    if (isSmoothInterpolation(track.interpolationType)) {
        stride += 2 * sizeof(T); // Add tangents for Hermite/Bezier
        keyStride += 2 * sizeof(T);
        key_components = 3; // Value + inTan + outTan
    }
    track.keys_data.resize((trackCount * keyStride) / sizeof(T));
    key_buffer.resize(trackCount * stride);
    reader.readBytes(reinterpret_cast<char*>(key_buffer.data()),
                     static_cast<u32>(trackCount * stride));
    for (size_t i = 0; i < trackCount; ++i) {
        std::memcpy(&track.timestamps[i], key_buffer.data() + i * stride, sizeof(u32));
        std::memcpy(&track.keys_data[i * key_components],
                    key_buffer.data() + i * stride + sizeof(u32), keyStride);
    }
    return track;
}

void Parser::Impl::SkipUnknownChunk(BinaryReader& reader, u32 tag, u32 size) {
    std::string const error =
        "Unknown chunk: " + std::string((char*)&tag, 4) + " (size: " + std::to_string(size) + ")";
    if (parseMode == ParseMode::Strict) {
        throw std::runtime_error(error);
    }
    issues.push_back(error);
    reader.skip(size);
}

void Parser::Impl::SkipUnknownTrack(BinaryReader& reader, u32 tag, u32 trackCount,
                                    u32 interpolationType) {
    std::string const error = "Unknown track: " + std::string((char*)&tag, 4) +
                              " (count: " + std::to_string(trackCount) + ")";
    if (parseMode == ParseMode::Strict) {
        throw std::runtime_error(error);
    }
    issues.push_back(error);
    // Conservative per-keyframe size: timestamp (u32) + value(u32) [+ 2*u32 tangents].
    // We don't know the actual T for an unknown track, so this assumes a 4-byte
    // value -- matches the prior behavior; large-typed unknown tracks (Vector3f,
    // Quaternion, ...) would still mis-skip just as before.
    const bool smooth =
        isSmoothInterpolation(static_cast<InterpolationType>(interpolationType));
    size_t bytesPerKey = sizeof(u32) + sizeof(u32);
    if (smooth) bytesPerKey += 2 * sizeof(u32);
    reader.skip(static_cast<u32>(trackCount * bytesPerKey));
}

// ============================================================================
// Parser Public Interface (using PImpl)
// ============================================================================

Parser::Parser(ParseMode parseMode, UpgradeMode upgradeMode) : pImpl(std::make_unique<Impl>()) {
    pImpl->parseMode = parseMode;
    pImpl->upgradeMode = upgradeMode;
}

Parser::~Parser() = default;

Model Parser::parse(const std::string& filePath) {
    // Detect format from file extension
    auto dotPos = filePath.rfind('.');
    if (dotPos != std::string::npos) {
        std::string ext = filePath.substr(dotPos);
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext == ".mdl") {
            // Read entire file as text
            auto file = common::open_ifstream(filePath, std::ios::ate);
            if (!file.is_open()) {
                throw std::runtime_error("Failed to open file: " + filePath);
            }
            auto size = file.tellg();
            file.seekg(0);
            std::string source(static_cast<size_t>(size), '\0');
            file.read(source.data(), size);
            Model model = convertMdlToModel(source, pImpl->issues);
            pImpl->upgradeMaterials(model); // Upgrade materials if needed
            return model;
        }
    }

    auto file = common::open_ifstream(filePath, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open file: " + filePath);
    }
    BinaryReader reader(file);
    return pImpl->parse(reader);
}

Model Parser::parse(std::span<const u8> buffer, MDLXFormat format) {
    if (format == MDLXFormat::MDL) {
        std::string_view const source(reinterpret_cast<const char*>(buffer.data()), buffer.size());
        Model model = convertMdlToModel(source, pImpl->issues);
        pImpl->upgradeMaterials(model); // Upgrade materials if needed
        return model;
    }

    common::span_streambuf streambuf(buffer);
    std::istream in(&streambuf);
    BinaryReader reader(in);
    return pImpl->parse(reader);
}

bool Parser::hasIssues() const {
    return !pImpl->issues.empty();
}

const std::vector<std::string>& Parser::getIssues() const {
    return pImpl->issues;
}

// ============================================================================
// ParserImpl Implementation - Moved all private methods here
// ============================================================================

Model Parser::Impl::parse(BinaryReader& reader) {
    issues.clear();
    Model mdx;

    // Read magic number
    u32 magic = reader.read<u32>();
    if (magic != MDLX_TAG) {
        std::string const error = "Invalid MDX file: expected magic 'MDLX', got '" +
                                  std::string(reinterpret_cast<char*>(&magic), 4) + "'";
        if (parseMode == ParseMode::Strict) {
            throw std::runtime_error(error);
        }
        issues.push_back(error);
        return mdx; // Return empty MDX file on failure
    }

    // Parse chunks
    while (reader.hasRemaining()) {
        ChunkHeader const header = readChunkHeader(reader);

        switch (header.tag) {
        case VERS_TAG:
            parseVERS(reader, header.size, mdx);
            if (mdx.version > CurrentVersion) {
                std::string const error = "Unsupported MDX version: " + std::to_string(mdx.version);
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
        case SNEM_TAG:
            parseSNEM(reader, header.size, mdx);
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
    upgradeMaterials(mdx); // Upgrade materials if needed
    if (upgradeMode == UpgradeMode::UpgradeOldVersions && mdx.version < CurrentVersion && mdx.version > 800) {
        mdx.version = CurrentVersion;
    }
    return mdx;
}

Parser::Impl::ChunkHeader Parser::Impl::readChunkHeader(BinaryReader& reader) {
    u32 const tag = reader.read<u32>();
    u32 const size = reader.read<u32>();
    return {tag, size};
}

void Parser::Impl::parseVERS(BinaryReader& reader, u32 /*size*/, Model& mdx) {
    mdx.version = reader.read<u32>();
}

void Parser::Impl::parseMODL(BinaryReader& reader, u32 /*size*/, Model& mdx) {
    mdx.modelName = reader.readString(80);
    mdx.animationFileName = reader.readString(260);
    mdx.modelExtent = reader.read<Extent>();
    mdx.blendTime = reader.read<u32>();
}

void Parser::Impl::parseSEQS(BinaryReader& reader, u32 size, Model& mdx) {
    u32 const count = size / 132;
    mdx.sequences.resize(count);

    for (u32 i = 0; i < count; i++) {
        Sequence& seq = mdx.sequences[i];
        seq.name = reader.readString(80);
        seq.intervalStart = reader.read<u32>();
        seq.intervalEnd = reader.read<u32>();
        seq.moveSpeed = reader.read<f32>();
        seq.flags = reader.read<Sequence::Flag>();
        seq.rarity = reader.read<f32>();
        seq.syncPoint = reader.read<u32>();
        seq.extent = reader.read<Extent>();
    }
}

void Parser::Impl::parseGLBS(BinaryReader& reader, u32 size, Model& mdx) {
    u32 const count = size / 4;
    mdx.globalSequences = reader.read<std::vector<u32>>(count);
}

void Parser::Impl::parseTEXS(BinaryReader& reader, u32 size, Model& mdx) {
    u32 const count = size / 268;
    mdx.textures.resize(count);

    for (u32 i = 0; i < count; i++) {
        mdx.textures[i].replaceableId = reader.read<u32>();
        mdx.textures[i].fileName = reader.readString(260);
        mdx.textures[i].flags = reader.read<Texture::Flag>();
    }
}

void Parser::Impl::parseSNDS(BinaryReader& reader, u32 size, Model& mdx) {
    u32 const count = size / 56;

    for (u32 i = 0; i < count; i++) {
        Sound snd;
        snd.soundFile = reader.readString(44);
        snd.maximumDistance = reader.read<f32>();
        snd.minimumDistance = reader.read<f32>();
        snd.soundChannel = reader.read<u32>();

        mdx.sounds.push_back(snd);
    }
}

void Parser::Impl::parseSNEM(BinaryReader& reader, u32 size, Model& mdx) {
    u32 totalRead = 0;

    while (totalRead < size) {
        u32 const posBefore = reader.getPosition();
        SoundEmitter snem = parseSoundEmitter(reader, size - totalRead);
        snem.node.nodeFamilyId = static_cast<u32>(mdx.soundEmitters.size());
        mdx.soundEmitters.push_back(snem);
        totalRead += reader.getPosition() - posBefore;
    }
}

SoundEmitter Parser::Impl::parseSoundEmitter(BinaryReader& reader, u32 /*maxSize*/) {
    SoundEmitter snem;
    u32 const startPos = reader.getPosition();

    u32 const inclusiveSize = reader.read<u32>();
    snem.node = parseNode(reader);

    // Parse animation tracks (KSEK)
    u32 const endPos = startPos + inclusiveSize;
    while (reader.getPosition() < endPos) {
        u32 const trackTag = reader.read<u32>();
        u32 const trackCount = reader.read<u32>();
        u32 const interpolationType = reader.read<u32>();
        u32 const globalSequenceId = reader.read<u32>();

        switch (trackTag) {
        case KSEK_TAG: // KSEK - sound track
            snem.soundTrack =
                readTrackChunk<u32>(reader, trackCount, interpolationType, globalSequenceId);
            break;
        default:
            SkipUnknownTrack(reader, trackTag, trackCount, interpolationType);
            break;
        }
    }

    return snem;
}

void Parser::Impl::parseMTLS(BinaryReader& reader, u32 size, Model& mdx) {
    u32 totalRead = 0;

    while (totalRead < size) {
        u32 const posBefore = reader.getPosition();
        Material const mat = parseMaterial(reader, size - totalRead, mdx);
        mdx.materials.push_back(mat);
        totalRead += reader.getPosition() - posBefore;
    }
}

Material Parser::Impl::parseMaterial(BinaryReader& reader, u32 /*chunkSize*/, Model& mdx) {
    Material mat;
    [[maybe_unused]] u32 const inclusiveSize = reader.read<u32>();
    mat.priorityPlane = reader.read<u32>();
    mat.flags = reader.read<Material::Flag>();

    if (mdx.version >= 900 && mdx.version < 1100) {
        mat.shader = reader.readString(80);
    }

    // Read LAYS chunk
    [[maybe_unused]] u32 const laysTag = reader.read<u32>();
    u32 const layerCount = reader.read<u32>();

    mat.layers.resize(layerCount);
    for (u32 i = 0; i < layerCount; i++) {
        mat.layers[i] = parseLayer(reader, mdx);
    }

    return mat;
}

Layer Parser::Impl::parseLayer(BinaryReader& reader, Model& mdx) {
    Layer layer;
    u32 const startPos = reader.getPosition();

    u32 const inclusiveSize = reader.read<u32>();
    layer.filterMode = static_cast<Layer::FilterMode>(reader.read<u32>());
    layer.shadingFlags = static_cast<Layer::ShadingFlag>(reader.read<u32>());
    layer.textureId = reader.read<u32>();
    layer.textureAnimationId = reader.read<u32>();
    layer.coordId = reader.read<u32>();
    layer.alpha = reader.read<f32>();

    // emissiveGain appeared in v900; the fresnel fields only in v1000. Gating
    // both on `> 800` made v900 layers consume 16 bytes of the following track
    // chunk as fresnel data — surfacing as NaN fresnel values and misaligned
    // track parsing. The fresnel *track* chunks (KFC3/KFCA/KFTC) are already
    // correctly gated on `> 900` further below.
    if (mdx.version > 800) {
        layer.emissiveGain = reader.read<f32>();
    }
    if (mdx.version > 900) {
        layer.fresnelColor = reader.read<Vector3f>();
        layer.fresnelOpacity = reader.read<f32>();
        layer.fresnelTeamColor = reader.read<f32>();
    }

    if (mdx.version >= 1100) {
        layer.textureId = 0;
        layer.shader = reader.read<Layer::ShaderType>();
        const auto num_textures = reader.read<u32>();
        for (u32 i = 0; i < num_textures; i++) {
            Layer::SubTexture subTex;
            subTex.textureId = reader.read<u32>();
            subTex.slot = static_cast<Layer::SlotType>(reader.read<u32>());

            u32 const subTexStart = reader.getPosition();
            u32 const peek_tag = reader.read<u32>();
            if (peek_tag == KMTF_TAG) {
                u32 const trackCount = reader.read<u32>();
                u32 const interpolationType = reader.read<u32>();
                u32 const globalSequenceId = reader.read<u32>();
                subTex.tracks =
                    readTrackChunk<u32>(reader, trackCount, interpolationType, globalSequenceId);
            } else {
                reader.setPosition(subTexStart);
            }
            layer.subTextures.push_back(subTex);
        }
    }

    const u32 endPos = startPos + inclusiveSize;
    while (reader.getPosition() < endPos) {
        u32 const trackTag = reader.read<u32>();
        u32 const trackCount = reader.read<u32>();
        u32 const interpolationType = reader.read<u32>();
        u32 const globalSequenceId = reader.read<u32>();

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

void Parser::Impl::parseTXAN(BinaryReader& reader, u32 size, Model& mdx) {
    u32 totalRead = 0;

    while (totalRead < size) {
        u32 const posBefore = reader.getPosition();
        TextureAnimation const anim = parseTextureAnimation(reader, size - totalRead);
        mdx.textureAnimations.push_back(anim);
        totalRead += reader.getPosition() - posBefore;
    }
}

TextureAnimation Parser::Impl::parseTextureAnimation(BinaryReader& reader, u32 /*maxSize*/) {
    TextureAnimation anim;
    u32 const startPos = reader.getPosition();

    u32 const inclusiveSize = reader.read<u32>();

    // Parse animation tracks (KTAT, KTAR, KTAS)
    u32 const endPos = startPos + inclusiveSize;
    while (reader.getPosition() < endPos) {
        u32 const trackTag = reader.read<u32>();
        u32 const trackCount = reader.read<u32>();
        u32 const interpolationType = reader.read<u32>();
        u32 const globalSequenceId = reader.read<u32>();

        switch (trackTag) {
        case KTAT_TAG: // KTAT - translation
            anim.translationTracks =
                readTrackChunk<Vector3f>(reader, trackCount, interpolationType, globalSequenceId);
            break;
        case KTAR_TAG: // KTAR - rotation (quaternion XYZW)
            anim.rotationTracks =
                readTrackChunk<Quaternion>(reader, trackCount, interpolationType, globalSequenceId);
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

void Parser::Impl::parseGEOS(BinaryReader& reader, u32 size, Model& mdx) {
    u32 totalRead = 0;

    while (totalRead < size) {
        u32 const posBefore = reader.getPosition();
        Geoset const geo = parseGeoset(reader, size - totalRead, mdx);

        mdx.geosets.push_back(geo);
        totalRead += reader.getPosition() - posBefore;
    }
}

Geoset Parser::Impl::parseGeoset(BinaryReader& reader, u32 /*maxSize*/, Model& mdx) {
    Geoset geoset;
    u32 const startPos = reader.getPosition();

    u32 const inclusiveSize = reader.read<u32>();

    // Parse VRTX - vertex positions
    [[maybe_unused]] u32 const vrtxTag = reader.read<u32>();
    u32 const vertexCount = reader.read<u32>();
    geoset.vertexPositions = reader.read<std::vector<Vector3f>>(vertexCount);

    // Parse NRMS - normals
    [[maybe_unused]] u32 const nrmsTag = reader.read<u32>();
    u32 const normalCount = reader.read<u32>();
    geoset.vertexNormals = reader.read<std::vector<Vector3f>>(normalCount);

    // Parse PTYP - face type groups
    [[maybe_unused]] u32 const ptypTag = reader.read<u32>();
    u32 const faceTypeGroupCount = reader.read<u32>();
    geoset.faceTypeGroups = reader.read<std::vector<u32>>(faceTypeGroupCount);

    // Parse PCNT - face groups
    [[maybe_unused]] u32 const pcntTag = reader.read<u32>();
    u32 const faceGroupCount = reader.read<u32>();
    geoset.faceGroups = reader.read<std::vector<u32>>(faceGroupCount);

    // Parse PVTX - face indices
    [[maybe_unused]] u32 const pvtxTag = reader.read<u32>();
    u32 const faceCount = reader.read<u32>();
    geoset.faces = reader.read<std::vector<u16>>(faceCount);

    // Parse GNDX - vertex groups
    [[maybe_unused]] u32 const gndxTag = reader.read<u32>();
    u32 const vertexGroupCount = reader.read<u32>();
    geoset.vertexGroups = reader.read<std::vector<u8>>(vertexGroupCount);

    // Parse MTGC - matrix groups
    [[maybe_unused]] u32 const mtgcTag = reader.read<u32>();
    u32 const matrixGroupCount = reader.read<u32>();
    geoset.matrixGroups = reader.read<std::vector<u32>>(matrixGroupCount);

    // Parse MATS - matrix indices
    [[maybe_unused]] u32 const matsTag = reader.read<u32>();
    u32 const matrixIndexCount = reader.read<u32>();
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
    u32 const extentsCount = reader.read<u32>();
    geoset.sequenceExtents.resize(extentsCount);
    for (u32 i = 0; i < extentsCount; i++) {
        geoset.sequenceExtents[i] = reader.read<Extent>();
    }

    // Parse UVAS - texture coordinate sets
    u32 currentPos = reader.getPosition();
    u32 const endPos = startPos + inclusiveSize;

    while (currentPos < endPos) {
        u32 peekTag = reader.read<u32>();
        switch (peekTag) {
        case UVAS_TAG: {
            u32 const uvSetCount = reader.read<u32>();

            geoset.textureCoordinateSets.resize(uvSetCount);
            for (u32 i = 0; i < uvSetCount; i++) {
                [[maybe_unused]] u32 const uvbsTag = reader.read<u32>();
                u32 const uvCount = reader.read<u32>();
                geoset.textureCoordinateSets[i] = reader.read<std::vector<Vector2f>>(uvCount);
            }
            break;
        }
        case TANG_TAG: {
            u32 const tangentCount = reader.read<u32>();
            geoset.tangents = reader.read<std::vector<Vector4f>>(tangentCount);
            break;
        }
        case SKIN_TAG: {
            u32 const skinDataCount = reader.read<u32>();
            geoset.skinData = reader.read<std::vector<u8>>(skinDataCount);
            break;
        }
        default: {
            std::string const error = "Unknown chunk in geoset: " + std::string((char*)&peekTag, 4);
            if (parseMode == ParseMode::Strict) {
                throw std::runtime_error(error);
            }
            issues.push_back(error);
            reader.skip(4); // Skip the size field of the unknown chunk
            u32 const unknownSize = reader.read<u32>();
            reader.skip(unknownSize * 4);
            break;
        }
        }
        currentPos = reader.getPosition();
    }

    // Ensure we're at the correct position
    reader.setPosition(startPos + inclusiveSize);

    return geoset;
}

void Parser::Impl::parseGEOA(BinaryReader& reader, u32 size, Model& mdx) {
    u32 totalRead = 0;

    while (totalRead < size) {
        u32 const startPos = reader.getPosition();

        GeosetAnimation anim;
        u32 const inclusiveSize = reader.read<u32>();
        anim.alpha = reader.read<f32>();
        anim.flags = reader.read<GeosetAnimation::Flag>();
        anim.color = reader.read<Vector3f>();
        anim.geosetId = reader.read<u32>();

        // Parse animation tracks (KGAO, KGAC)
        u32 const endPos = startPos + inclusiveSize;
        while (reader.getPosition() < endPos) {
            u32 const trackTag = reader.read<u32>();
            u32 const trackCount = reader.read<u32>();
            u32 const interpolationType = reader.read<u32>();
            u32 const globalSequenceId = reader.read<u32>();

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
        totalRead += reader.getPosition() - startPos;
    }
}

void Parser::Impl::parseBONE(BinaryReader& reader, u32 size, Model& mdx) {
    u32 totalRead = 0;

    while (totalRead < size) {
        Bone bone;

        u32 const startPos = reader.getPosition();
        bone.node = parseNode(reader);

        bone.geosetId = reader.read<u32>();
        bone.geosetAnimationId = reader.read<u32>();

        bone.node.type = Node::NodeType::Bone; // Set node type to Bone
        bone.node.nodeFamilyId = static_cast<u32>(
            mdx.bones.size()); // Assign a unique family ID based on current bone count

        mdx.bones.push_back(bone);

        u32 const endPos = reader.getPosition();
        totalRead += (endPos - startPos);
    }
}

Node Parser::Impl::parseNode(BinaryReader& reader) {
    Node node;
    [[maybe_unused]] u32 const startPos = reader.getPosition();
    [[maybe_unused]] u32 const nodeSize = reader.read<u32>();

    node.name = reader.readString(80);
    node.objectId = reader.read<u32>();
    node.parentId = reader.read<u32>();
    node.flags = static_cast<Node::NodeFlag>(reader.read<u32>());

    parseNodeTracks(reader, node, nodeSize);

    return node;
}

void Parser::Impl::parseNodeTracks(BinaryReader& reader, Node& node, u32 nodeSize) {
    u32 const nodeDataSize =
        4 + 80 + 4 + 4 + 4; // inclusiveSize + name + objectId + parentId + flags
    u32 const tracksSize = nodeSize - nodeDataSize;

    u32 const startPos = reader.getPosition();

    while (reader.getPosition() - startPos < tracksSize) {
        if (reader.getPosition() - startPos >= tracksSize)
            break;

        u32 const trackTag = reader.read<u32>();
        u32 const trackCount = reader.read<u32>();
        u32 const interpolationType = reader.read<u32>();
        u32 const globalSequenceId = reader.read<u32>();

        switch (trackTag) {
        case KGTR_TAG:
            node.translationTracks =
                readTrackChunk<Vector3f>(reader, trackCount, interpolationType, globalSequenceId);
            break;
        case KGRT_TAG:
            node.rotationTracks =
                readTrackChunk<Quaternion>(reader, trackCount, interpolationType, globalSequenceId);
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

void Parser::Impl::parseLITE(BinaryReader& reader, u32 size, Model& mdx) {
    u32 totalRead = 0;

    while (totalRead < size) {
        u32 const posBefore = reader.getPosition();
        Light light = parseLight(reader, size - totalRead, mdx);
        light.node.type = Node::NodeType::Light; // Set node type to Light
        light.node.nodeFamilyId = static_cast<u32>(
            mdx.lights.size()); // Assign a unique family ID based on current light count
        mdx.lights.push_back(light);
        totalRead += reader.getPosition() - posBefore;
    }
}

Light Parser::Impl::parseLight(BinaryReader& reader, u32 /*maxSize*/, Model& mdx) {
    Light light;
    u32 const startPos = reader.getPosition();

    u32 const inclusiveSize = reader.read<u32>();
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
    u32 const endPos = startPos + inclusiveSize;
    while (reader.getPosition() < endPos) {
        u32 const trackTag = reader.read<u32>();
        u32 const trackCount = reader.read<u32>();
        u32 const interpolationType = reader.read<u32>();
        u32 const globalSequenceId = reader.read<u32>();

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

void Parser::Impl::parseHELP(BinaryReader& reader, u32 size, Model& mdx) {
    u32 totalRead = 0;

    while (totalRead < size) {
        u32 const startPos = reader.getPosition();

        Helper helper;
        helper.node = parseNode(reader);

        helper.node.type = Node::NodeType::Helper; // Set node type to Helper
        helper.node.nodeFamilyId = static_cast<u32>(
            mdx.helpers.size()); // Assign a unique family ID based on current helper count

        mdx.helpers.push_back(helper);

        u32 const endPos = reader.getPosition();
        totalRead += (endPos - startPos);
    }
}

void Parser::Impl::parseATCH(BinaryReader& reader, u32 size, Model& mdx) {
    u32 totalRead = 0;

    while (totalRead < size) {
        u32 const posBefore = reader.getPosition();
        Attachment att = parseAttachment(reader, size - totalRead);

        att.node.type = Node::NodeType::Attachment; // Set node type to Attachment
        att.node.nodeFamilyId = static_cast<u32>(
            mdx.attachments.size()); // Assign a unique family ID based on current attachment count

        mdx.attachments.push_back(att);
        totalRead += reader.getPosition() - posBefore;
    }
}

Attachment Parser::Impl::parseAttachment(BinaryReader& reader, u32 /*maxSize*/) {
    Attachment att;
    u32 const startPos = reader.getPosition();

    u32 const inclusiveSize = reader.read<u32>();
    att.node = parseNode(reader);
    att.path = reader.readString(260);
    att.attachmentId = reader.read<u32>();

    // Parse animation tracks (KATV)
    u32 const endPos = startPos + inclusiveSize;
    while (reader.getPosition() < endPos) {
        u32 const trackTag = reader.read<u32>();
        u32 const trackCount = reader.read<u32>();
        u32 const interpolationType = reader.read<u32>();
        u32 const globalSequenceId = reader.read<u32>();

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

void Parser::Impl::parsePIVT(BinaryReader& reader, u32 size, Model& mdx) {
    u32 const count = size / 12;
    mdx.pivotPoints.resize(count);

    for (u32 i = 0; i < count; i++) {
        mdx.pivotPoints[i] = reader.read<Vector3f>();
    }
}

void Parser::Impl::parsePREM(BinaryReader& reader, u32 size, Model& mdx) {
    u32 totalRead = 0;

    while (totalRead < size) {
        u32 const posBefore = reader.getPosition();
        ParticleEmitter pem = parseParticleEmitter(reader, size - totalRead);
        pem.node.type = Node::NodeType::ParticleEmitter; // Set node type to ParticleEmitter
        pem.node.nodeFamilyId = static_cast<u32>(
            mdx.particleEmitters
                .size()); // Assign a unique family ID based on current particle emitter count

        mdx.particleEmitters.push_back(pem);
        totalRead += reader.getPosition() - posBefore;
    }
}

ParticleEmitter Parser::Impl::parseParticleEmitter(BinaryReader& reader, u32 /*maxSize*/) {
    ParticleEmitter pem;
    u32 const startPos = reader.getPosition();

    u32 const inclusiveSize = reader.read<u32>();
    pem.node = parseNode(reader);
    pem.emissionRate = reader.read<f32>();
    pem.gravity = reader.read<f32>();
    pem.longitude = reader.read<f32>();
    pem.latitude = reader.read<f32>();
    pem.spawnModelFileName = reader.readString(260);
    pem.lifespan = reader.read<f32>();
    pem.initialVelocity = reader.read<f32>();

    // Parse animation tracks (KPEE, KPEG, KPLN, KPLT, KPEL, KPES, KPEV)
    u32 const endPos = startPos + inclusiveSize;
    while (reader.getPosition() < endPos) {
        u32 const trackTag = reader.read<u32>();
        u32 const trackCount = reader.read<u32>();
        u32 const interpolationType = reader.read<u32>();
        u32 const globalSequenceId = reader.read<u32>();

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

void Parser::Impl::parsePRE2(BinaryReader& reader, u32 size, Model& mdx) {
    u32 totalRead = 0;

    while (totalRead < size) {
        u32 const posBefore = reader.getPosition();
        ParticleEmitter2 pem2 = parseParticleEmitter2(reader, size - totalRead);
        pem2.node.type = Node::NodeType::ParticleEmitter2; // Set node type to ParticleEmitter2
        pem2.node.nodeFamilyId = static_cast<u32>(
            mdx.particleEmitters2
                .size()); // Assign a unique family ID based on current particle emitter count

        mdx.particleEmitters2.push_back(pem2);
        totalRead += reader.getPosition() - posBefore;
    }
}

ParticleEmitter2 Parser::Impl::parseParticleEmitter2(BinaryReader& reader, u32 /*maxSize*/) {
    ParticleEmitter2 pem2;
    u32 const startPos = reader.getPosition();

    u32 const inclusiveSize = reader.read<u32>();
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
    u32 const endPos = startPos + inclusiveSize;
    while (reader.getPosition() < endPos) {
        u32 const trackTag = reader.read<u32>();
        u32 const trackCount = reader.read<u32>();
        u32 const interpolationType = reader.read<u32>();
        u32 const globalSequenceId = reader.read<u32>();

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

void Parser::Impl::parseRIBB(BinaryReader& reader, u32 size, Model& mdx) {
    u32 totalRead = 0;

    while (totalRead < size) {
        u32 const posBefore = reader.getPosition();
        RibbonEmitter ribb = parseRibbonEmitter(reader, size - totalRead);
        ribb.node.type = Node::NodeType::RibbonEmitter; // Set node type to RibbonEmitter
        ribb.node.nodeFamilyId = static_cast<u32>(
            mdx.ribbonEmitters
                .size()); // Assign a unique family ID based on current ribbon emitter count

        mdx.ribbonEmitters.push_back(ribb);
        totalRead += reader.getPosition() - posBefore;
    }
}

RibbonEmitter Parser::Impl::parseRibbonEmitter(BinaryReader& reader, u32 /*maxSize*/) {
    RibbonEmitter ribb;
    u32 const startPos = reader.getPosition();

    u32 const inclusiveSize = reader.read<u32>();
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
    u32 const endPos = startPos + inclusiveSize;
    while (reader.getPosition() < endPos) {
        u32 const trackTag = reader.read<u32>();
        u32 const trackCount = reader.read<u32>();
        u32 const interpolationType = reader.read<u32>();
        u32 const globalSequenceId = reader.read<u32>();

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

void Parser::Impl::parseEVTS(BinaryReader& reader, u32 size, Model& mdx) {
    u32 totalRead = 0;

    while (totalRead < size) {
        u32 const startPos = reader.getPosition();

        EventObject evt;
        evt.node = parseNode(reader);
        evt.node.type = Node::NodeType::EventObject; // Set node type to EventObject
        evt.node.nodeFamilyId = static_cast<u32>(
            mdx.eventObjects
                .size()); // Assign a unique family ID based on current event object count

        // Read KEVT chunk
        [[maybe_unused]] u32 const kevtTag = reader.read<u32>();
        u32 const trackCount = reader.read<u32>();
        evt.globalSequenceId = reader.read<u32>();

        for (u32 i = 0; i < trackCount; i++) {
            evt.eventTrackTimes.push_back(reader.read<u32>());
        }

        mdx.eventObjects.push_back(evt);

        u32 const endPos = reader.getPosition();
        totalRead += (endPos - startPos);
    }
}

void Parser::Impl::parseCAMS(BinaryReader& reader, u32 size, Model& mdx) {
    u32 totalRead = 0;

    while (totalRead < size) {
        u32 const posBefore = reader.getPosition();
        Camera const cam = parseCamera(reader, size - totalRead);
        mdx.cameras.push_back(cam);
        totalRead += reader.getPosition() - posBefore;
    }
}

Camera Parser::Impl::parseCamera(BinaryReader& reader, u32 /*maxSize*/) {
    Camera cam;
    u32 const startPos = reader.getPosition();

    u32 const inclusiveSize = reader.read<u32>();
    cam.name = reader.readString(80);
    cam.position = reader.read<Vector3f>();
    cam.fieldOfView = reader.read<f32>();
    cam.farClippingPlane = reader.read<f32>();
    cam.nearClippingPlane = reader.read<f32>();
    cam.targetPosition = reader.read<Vector3f>();

    // Parse animation tracks (KCTR, KCRL, KTTR)
    u32 const endPos = startPos + inclusiveSize;
    while (reader.getPosition() < endPos) {
        u32 const trackTag = reader.read<u32>();
        u32 const trackCount = reader.read<u32>();
        u32 const interpolationType = reader.read<u32>();
        u32 const globalSequenceId = reader.read<u32>();

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

void Parser::Impl::parseCLID(BinaryReader& reader, u32 size, Model& mdx) {
    u32 totalRead = 0;

    while (totalRead < size) {
        u32 const startPos = reader.getPosition();

        CollisionShape shape = parseCollisionShape(reader);
        shape.node.type = Node::NodeType::CollisionShape; // Set node type to CollisionShape
        shape.node.nodeFamilyId = static_cast<u32>(
            mdx.collisionShapes
                .size()); // Assign a unique family ID based on current collision shape count
        mdx.collisionShapes.push_back(shape);

        u32 const endPos = reader.getPosition();
        totalRead += (endPos - startPos);
    }
}

CollisionShape Parser::Impl::parseCollisionShape(BinaryReader& reader) {
    CollisionShape shape;
    shape.node = parseNode(reader);
    u32 const type_index = reader.read<u32>();
    shape.type = static_cast<CollisionShape::ShapeType>(type_index);

    constexpr std::array<size_t, 4> shapeVertexCounts = {2, 2, 1, 2};

    u32 const vertexCount = static_cast<u32>(shapeVertexCounts[type_index]);

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

void Parser::Impl::parseBPOS(BinaryReader& reader, u32 /*size*/, Model& mdx) {
    u32 const count = reader.read<u32>();
    mdx.bindPoses.resize(count);

    for (u32 i = 0; i < count; i++) {
        for (int j = 0; j < 12; j++) {
            mdx.bindPoses[i][j] = reader.read<f32>();
        }
    }
}

void Parser::Impl::parseFAFX(BinaryReader& reader, u32 size, Model& mdx) {
    u32 const count = size / 340;
    mdx.faceEffects.resize(count);

    for (u32 i = 0; i < count; i++) {
        mdx.faceEffects[i].name = reader.readString(80);
        mdx.faceEffects[i].path = reader.readString(260);
    }
}

void Parser::Impl::parseCORN(BinaryReader& reader, u32 size, Model& mdx) {
    u32 totalRead = 0;

    while (totalRead < size) {
        CornEmitter corn;
        u32 const startPos = reader.getPosition();

        u32 const inclusiveSize = reader.read<u32>();
        corn.node = parseNode(reader);
        corn.node.type = Node::NodeType::CornEmitter; // Set node type to CornEmitter
        corn.node.nodeFamilyId = static_cast<u32>(
            mdx.cornEmitters
                .size()); // Assign a unique family ID based on current corn emitter count

        corn.lifeSpan = reader.read<f32>();
        corn.emissionRate = reader.read<f32>();
        corn.speed = reader.read<f32>();
        corn.color = reader.read<Vector3f>();
        corn.alpha = reader.read<f32>();
        corn.replaceableId = reader.read<u32>();
        corn.path = reader.readString(260);
        corn.animVisibilityGuide = reader.readString(260);

        u32 const endPos = startPos + inclusiveSize;
        while (reader.getPosition() < endPos) {
            u32 const trackTag = reader.read<u32>();
            u32 const trackCount = reader.read<u32>();
            u32 const interpolationType = reader.read<u32>();
            u32 const globalSequenceId = reader.read<u32>();

            switch (trackTag) {
            case KPPL_TAG: // lifespan
                corn.lifeSpanTracks =
                    readTrackChunk<f32>(reader, trackCount, interpolationType, globalSequenceId);
                break;
            case KPPE_TAG: // emission rate
                corn.emissionRateTracks =
                    readTrackChunk<f32>(reader, trackCount, interpolationType, globalSequenceId);
                break;
            case KPPS_TAG: // speed
                corn.speedTracks =
                    readTrackChunk<f32>(reader, trackCount, interpolationType, globalSequenceId);
                break;
            case KPPC_TAG: // color
                corn.colorTracks = readTrackChunk<Vector3f>(reader, trackCount, interpolationType,
                                                            globalSequenceId);
                break;
            case KPPA_TAG: // alpha
                corn.alphaTracks =
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
        totalRead += reader.getPosition() - startPos;
    }
}

void Parser::Impl::upgradeMaterials(Model& mdx) {
    if (mdx.version <= 800 || mdx.version > 1000) {
        return;
    }

    if (upgradeMode != UpgradeMode::UpgradeOldVersions)
    {
        return;
    }

    // If the model is from Reforged and has no materials but has geosets, create default materials
    for (auto& mat : mdx.materials) {
        const bool is_hd = mat.shader == "Shader_HD_DefaultUnit" || mat.shader == "Shader_HD_Crystal";
        // Upgrade older versions to newer format.
        const bool engineHdMerge =
            is_hd && mdx.version >= 900 && mat.layers.size() == 6;

        if (engineHdMerge) {
            // For HD-collapsed layers, layer index is the slot value.
            static constexpr Layer::SlotType kHdSlotOrder[] = {
                Layer::SlotType::DiffuseMap,
                Layer::SlotType::NormalMap,
                Layer::SlotType::ORMMap,
                Layer::SlotType::EmissiveMap,
                Layer::SlotType::TeamColor,
                Layer::SlotType::EnvironmentMap,
            };

            // Convert to HD layer format
            std::vector<Layer> hdLayers;
            hdLayers.resize(1);
            hdLayers[0] = mat.layers[0]; // First layer is the HD layer

            hdLayers[0].textureId = 0;
            hdLayers[0].textureIdTracks = Track<u32>();
            for (size_t i = 0; i < mat.layers.size(); i++) {
                auto& layer = mat.layers[i];
                Layer::SubTexture subTex;
                subTex.textureId = layer.textureId;
                subTex.slot = kHdSlotOrder[i];
                subTex.tracks = std::move(layer.textureIdTracks);
                hdLayers[0].subTextures.push_back(subTex);
            }
            if (mat.shader == "Shader_SD_FixedFunction") {
                hdLayers[0].shader = Layer::ShaderType::SDOnHD;
            } else if (mat.shader == "Shader_HD_DefaultUnit") {
                hdLayers[0].shader = Layer::ShaderType::HD;
            } else if (mat.shader == "Shader_HD_Crystal") {
                hdLayers[0].shader = Layer::ShaderType::Crystal;
            } else {
                hdLayers[0].shader = Layer::ShaderType::SD; // Unknown shader, set to 0
            }

            // Material-level TwoSided propagates onto the merged layer.
            if (hasFlag(mat.flags, Material::Flag::TwoSided)) {
                hdLayers[0].shadingFlags |= Layer::ShadingFlag::TwoSided;
            }

            mat.layers = std::move(hdLayers);
        } else {
            // Non-HD path: seed subTextures[0] for each layer so the writer
            // can emit the unified v1100 layout consistently.
            for (auto& layer : mat.layers) {
                Layer::SubTexture subTex;
                subTex.textureId = layer.textureId;
                subTex.slot = Layer::SlotType::DiffuseMap;
                subTex.tracks = std::move(layer.textureIdTracks);
                layer.subTextures.push_back(subTex);
                layer.textureId = 0; // Clear textureId since it will be in subTextures
                layer.textureIdTracks = Track<u32>(); // Clear old tracks
            }
        }
    }
}

} // namespace mdx
} // namespace whiteout
