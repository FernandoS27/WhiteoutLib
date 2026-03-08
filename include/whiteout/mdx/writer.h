// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file mdx_writer.h
 * @brief MDX file writer
 *
 * This file provides the Writer class for writing MDX model files.
 * The writer converts Model structures back to binary MDX format.
 *
 * @example Basic writing
 * @code
 * mdx::Model model;
 * // ... populate model data ...
 *
 * mdx::Writer writer;
 * writer.write("output.mdx", model);
 * @endcode
 */

#include <memory>
#include "../compatibility.h"
#include "structures.h"
#include "types.h"

namespace whiteout {
namespace common {
class BinaryWriter;
}

namespace mdx {

// Use BinaryWriter from Common namespace
using common::BinaryWriter;

// ============================================================================
// MDX Writer
// ============================================================================

/**
 * @brief Writer for MDX model files
 *
 * The Writer takes an Model structure and writes it to disk in binary
 * MDX format. It automatically handles chunk serialization and size calculation.
 */
class Writer {
public:
    Writer() = default;
    ~Writer() = default;

    /**
     * @brief Write an MDX file to disk
     * @param filePath Path where the MDX file should be written
     * @param mdx MDX file data to write
     * @throws std::runtime_error If file cannot be created or written
     */
    void write(const std::string& filePath, const Model& mdx);

private:
    struct ChunkHeader {
        u32 tag;  ///< Chunk identifier
        u32 size; ///< Chunk data size in bytes
    };

    void writeChunkHeader(BinaryWriter& writer, u32 tag, u32 size);

    void write(BinaryWriter& writer, const Model& mdx);

    // Chunk writers - each writes one MDX chunk type
    void writeVERS(BinaryWriter& writer, const Model& mdx); ///< Write version chunk
    void writeMODL(BinaryWriter& writer, const Model& mdx); ///< Write model info chunk
    void writeSEQS(BinaryWriter& writer, const Model& mdx); ///< Write sequences chunk
    void writeGLBS(BinaryWriter& writer, const Model& mdx); ///< Write global sequences
    void writeTEXS(BinaryWriter& writer, const Model& mdx); ///< Write textures chunk
    void writeSNDS(BinaryWriter& writer, const Model& mdx); ///< Write sounds chunk
    void writeSNEM(BinaryWriter& writer, const Model& mdx); ///< Write sound emitters chunk
    void writeMTLS(BinaryWriter& writer, const Model& mdx); ///< Write materials chunk
    void writeTXAN(BinaryWriter& writer, const Model& mdx); ///< Write texture animations
    void writeGEOS(BinaryWriter& writer, const Model& mdx); ///< Write geosets chunk
    void writeGEOA(BinaryWriter& writer, const Model& mdx); ///< Write geoset animations
    void writeBONE(BinaryWriter& writer, const Model& mdx); ///< Write bones chunk
    void writeLITE(BinaryWriter& writer, const Model& mdx); ///< Write lights chunk
    void writeHELP(BinaryWriter& writer, const Model& mdx); ///< Write helpers chunk
    void writeATCH(BinaryWriter& writer, const Model& mdx); ///< Write attachments chunk
    void writePIVT(BinaryWriter& writer, const Model& mdx); ///< Write pivot points
    void writePREM(BinaryWriter& writer, const Model& mdx); ///< Write particle emitters v1
    void writePRE2(BinaryWriter& writer, const Model& mdx); ///< Write particle emitters v2
    void writeRIBB(BinaryWriter& writer, const Model& mdx); ///< Write ribbon emitters
    void writeEVTS(BinaryWriter& writer, const Model& mdx); ///< Write event objects
    void writeCAMS(BinaryWriter& writer, const Model& mdx); ///< Write cameras chunk
    void writeCLID(BinaryWriter& writer, const Model& mdx); ///< Write collision shapes
    void writeBPOS(BinaryWriter& writer, const Model& mdx); ///< Write bind poses (Reforged)
    void writeFFX(BinaryWriter& writer, const Model& mdx);  ///< Write face effects (Reforged)
    void writeCORN(BinaryWriter& writer, const Model& mdx); ///< Write corn emitters (Reforged)

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

    void writeNodeTracks(BinaryWriter& writer, const Node& node); ///< Write all tracks for a node
};

} // namespace mdx
} // namespace whiteout
