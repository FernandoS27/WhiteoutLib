// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file mdx_parser.h
 * @brief MDX file parser
 *
 * This file provides the Parser class for reading and parsing MDX model files.
 * The parser can handle multiple MDX versions from Classic (800) to Reforged (1200).
 *
 * @example Basic parsing
 * @code
 * mdx::Parser parser(mdx::Parser::ParseMode::Lenient);
 * mdx::Model model = parser.parse("model.mdx");
 *
 * if (parser.hasIssues()) {
 *     for (const auto& issue : parser.getIssues()) {
 *         std::cout << "Warning: " << issue << std::endl;
 *     }
 * }
 * @endcode
 */

#include <memory>
#include <string>
#include <vector>
#include "../compatibility.h"
#include "structures.h"
#include "types.h"

namespace whiteout {
namespace common {
class BinaryReader;
}

namespace mdx {

// Use BinaryReader from Common namespace
using common::BinaryReader;

// ============================================================================
// MDX Parser
// ============================================================================

/**
 * @brief Parser for MDX model files
 *
 * The Parser reads binary MDX files and converts them into the Model
 * structure. It supports multiple parsing modes and can handle version differences.
 */
class Parser {
public:
    /**
     * @brief Parsing strictness mode
     */
    enum class ParseMode {
        Strict, ///< Throw exceptions on unknown chunks or invalid data
        Lenient ///< Skip unknown chunks and try to recover from errors (recommended)
    };

    /**
     * @brief Version handling mode
     */
    enum class UpgradeMode {
        UpgradeOldVersions, ///< Automatically upgrade older versions to latest format
        PreserveOriginal    ///< Keep original version and structure as-is
    };

    /**
     * @brief Construct a new Parser
     * @param parseMode Strictness mode for parsing
     * @param upgradeMode How to handle version differences
     */
    Parser(ParseMode parseMode = ParseMode::Lenient,
           UpgradeMode upgradeMode = UpgradeMode::UpgradeOldVersions)
        : parseMode(parseMode), upgradeMode(upgradeMode) {}
    ~Parser() = default;

    /**
     * @brief Parse an MDX file from disk
     * @param filePath Path to the MDX file
     * @return Parsed MDX file data
     * @throws std::runtime_error If file cannot be opened or parsing fails in strict mode
     */
    Model parse(const std::string& filePath);

    /**
     * @brief Parse an MDX file from memory buffer
     * @param buffer Memory buffer containing MDX data
     * @return Parsed MDX file data
     * @throws std::runtime_error If parsing fails in strict mode
     */
    Model parse(std::span<const u8> buffer);

    /**
     * @brief Check if parsing encountered any issues
     * @return True if there were warnings or recoverable errors
     */
    bool hasIssues() const {
        return !issues.empty();
    }

    /**
     * @brief Get list of issues encountered during parsing
     * @return Vector of issue description strings
     */
    const std::vector<std::string>& getIssues() const {
        return issues;
    }

private:
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
    void parseVERS(BinaryReader& reader, u32 size, Model& mdx); ///< Parse version chunk
    void parseMODL(BinaryReader& reader, u32 size, Model& mdx); ///< Parse model info chunk
    void parseSEQS(BinaryReader& reader, u32 size, Model& mdx); ///< Parse sequences chunk
    void parseGLBS(BinaryReader& reader, u32 size, Model& mdx); ///< Parse global sequences
    void parseTEXS(BinaryReader& reader, u32 size, Model& mdx); ///< Parse textures chunk
    void parseSNDS(BinaryReader& reader, u32 size, Model& mdx); ///< Parse sounds chunk
    void parseSNEM(BinaryReader& reader, u32 size, Model& mdx); ///< Parse sound emitters chunk
    void parseMTLS(BinaryReader& reader, u32 size, Model& mdx); ///< Parse materials chunk
    void parseTXAN(BinaryReader& reader, u32 size, Model& mdx); ///< Parse texture animations
    void parseGEOS(BinaryReader& reader, u32 size, Model& mdx); ///< Parse geosets chunk
    void parseGEOA(BinaryReader& reader, u32 size, Model& mdx); ///< Parse geoset animations
    void parseBONE(BinaryReader& reader, u32 size, Model& mdx); ///< Parse bones chunk
    void parseLITE(BinaryReader& reader, u32 size, Model& mdx); ///< Parse lights chunk
    void parseHELP(BinaryReader& reader, u32 size, Model& mdx); ///< Parse helpers chunk
    void parseATCH(BinaryReader& reader, u32 size, Model& mdx); ///< Parse attachments chunk
    void parsePIVT(BinaryReader& reader, u32 size, Model& mdx); ///< Parse pivot points
    void parsePREM(BinaryReader& reader, u32 size, Model& mdx); ///< Parse particle emitters v1
    void parsePRE2(BinaryReader& reader, u32 size, Model& mdx); ///< Parse particle emitters v2
    void parseRIBB(BinaryReader& reader, u32 size, Model& mdx); ///< Parse ribbon emitters
    void parseEVTS(BinaryReader& reader, u32 size, Model& mdx); ///< Parse event objects
    void parseCAMS(BinaryReader& reader, u32 size, Model& mdx); ///< Parse cameras chunk
    void parseCLID(BinaryReader& reader, u32 size, Model& mdx); ///< Parse collision shapes
    void parseBPOS(BinaryReader& reader, u32 size, Model& mdx); ///< Parse bind poses (Reforged)
    void parseFAFX(BinaryReader& reader, u32 size, Model& mdx); ///< Parse face effects (Reforged)
    void parseCORN(BinaryReader& reader, u32 size, Model& mdx); ///< Parse corn emitters (Reforged)

    // Structure parsers - parse individual structure types
    Node parseNode(BinaryReader& reader); ///< Parse a node (common to many types)
    void parseNodeTracks(BinaryReader& reader, Node& node,
                         u32 nodeSize); ///< Parse node animation tracks

    Material parseMaterial(BinaryReader& reader, u32 chunkSize,
                           Model& mdx);                 ///< Parse a single material
    Layer parseLayer(BinaryReader& reader, Model& mdx); ///< Parse a material layer

    Geoset parseGeoset(BinaryReader& reader, u32 maxSize, Model& mdx); ///< Parse a single geoset

    TextureAnimation parseTextureAnimation(BinaryReader& reader,
                                           u32 maxSize); ///< Parse texture animation

    Attachment parseAttachment(BinaryReader& reader, u32 maxSize); ///< Parse an attachment
    ParticleEmitter parseParticleEmitter(BinaryReader& reader,
                                         u32 maxSize); ///< Parse particle emitter v1
    ParticleEmitter2 parseParticleEmitter2(BinaryReader& reader,
                                           u32 maxSize); ///< Parse particle emitter v2
    RibbonEmitter parseRibbonEmitter(BinaryReader& reader, u32 maxSize); ///< Parse ribbon emitter
    Camera parseCamera(BinaryReader& reader, u32 maxSize);               ///< Parse a camera
    Light parseLight(BinaryReader& reader, u32 maxSize, Model& mdx);     ///< Parse a light
    CollisionShape parseCollisionShape(BinaryReader& reader);            ///< Parse collision shape
    SoundEmitter parseSoundEmitter(BinaryReader& reader, u32 maxSize);   ///< Parse sound emitter

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
};

} // namespace mdx
} // namespace whiteout
