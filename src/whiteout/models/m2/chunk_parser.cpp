
#include "../../common/binary_reader.h"
#include "../../common/streams.h"
#include "binary_parse_visitor.h"
#include "chunk_parser.h"
#include "wow_file_system.h"

#include <algorithm>
#include <cstring>

namespace whiteout {
namespace m2 {

using common::BinaryReader;

ChunkParser::ChunkParser() = default;

void ChunkParser::reportIssue(const std::string& message) {
    issues.push_back(message);
}

void ChunkParser::skipUnknownChunk(BinaryReader& reader, u32 tag, u32 size) {
    std::string const error = "Unknown chunk: " + std::string(reinterpret_cast<char*>(&tag), 4) +
                              " (size: " + std::to_string(size) + ")";
    reportIssue(error);
    reader.skip(size);
}

void ChunkParser::drainIssues(std::vector<std::string>& target) {
    target.insert(target.end(), std::make_move_iterator(issues.begin()),
                  std::make_move_iterator(issues.end()));
    issues.clear();
}

std::vector<ChunkParser::ChunkEntry> ChunkParser::collectChunks(BinaryReader& reader) {
    std::vector<ChunkEntry> chunks;
    while (reader.hasRemaining()) {
        u32 const tag = reader.read<u32>();
        u32 const size = reader.read<u32>();
        // A size past the end of the buffer means this is not a chunk header —
        // scanning on from garbage yields garbage entries until memory runs out.
        if (size > reader.getRemainingBytes()) {
            reportIssue("Malformed chunk " + std::string(reinterpret_cast<const char*>(&tag), 4) +
                        ": size " + std::to_string(size) + " exceeds remaining bytes");
            break;
        }
        u32 const dataOffset = reader.getPosition();
        chunks.push_back({tag, size, dataOffset});
        reader.skip(size);
    }
    return chunks;
}

namespace {

std::string tagName(u32 tag) {
    char chars[4];
    std::memcpy(chars, &tag, 4);
    return std::string(chars, 4);
}

} // namespace

// Same walk as collectChunks, except that a `.phys` payload can be followed by
// alignment padding — PFDC zero-fills to a 16-byte multiple — and a zero tag is
// that padding, not a chunk.
std::vector<ChunkParser::ChunkEntry> ChunkParser::collectPhysChunks(BinaryReader& reader) {
    std::vector<ChunkEntry> chunks;
    while (reader.getRemainingBytes() >= 8) {
        u32 const tag = reader.read<u32>();
        u32 const size = reader.read<u32>();
        if (tag == 0) {
            // Padding is not stored — it is regenerated from the payload length
            // — so anything non-zero back here would be dropped silently.
            reader.setPosition(reader.getPosition() - 8);
            const auto tail = reader.read<std::vector<u8>>(reader.getRemainingBytes());
            if (std::any_of(tail.begin(), tail.end(), [](u8 byte) { return byte != 0; })) {
                reportIssue("PHYS: " + std::to_string(tail.size()) +
                            " trailing bytes after the last chunk are not padding");
            }
            break;
        }
        if (size > reader.getRemainingBytes()) {
            reportIssue("Malformed PHYS chunk " + tagName(tag) + ": size " + std::to_string(size) +
                        " exceeds remaining bytes");
            break;
        }
        chunks.push_back({tag, size, reader.getPosition()});
        reader.skip(size);
    }
    return chunks;
}

void ChunkParser::parsePhysics(BinaryReader& reader, PhysicsData& physics) {
    auto chunks = collectPhysChunks(reader);

    // The version chunk leads the file and every other chunk's layout depends
    // on it, so it is read before anything is dispatched rather than in order.
    for (const auto& chunk : chunks) {
        if (chunk.tag != PHYS_TAG)
            continue;
        reader.setPosition(chunk.dataOffset);
        physics.version = reader.read<u16>();
        break;
    }

    // Only the shoulder joint needs this: it is the one record that grew
    // without its chunk being renamed. Every other layout is implied by the
    // chunk name alone.
    const auto shoulderLayout = physShoulderLayoutFor(physics.version);

    for (const auto& chunk : chunks) {
        reader.setPosition(chunk.dataOffset);
        BinaryParseVisitor parser(reader, nullptr, chunk.size);

        // Every record array is a flat run of fixed-size entries, so the count
        // is just the chunk size over the stride.
        const auto readArray = [&]<typename T, typename Read>(std::vector<T>& out, u32 stride,
                                                              Read read) {
            if (chunk.size % stride != 0) {
                reportIssue("PHYS chunk " + tagName(chunk.tag) + ": size " +
                            std::to_string(chunk.size) + " is not a multiple of the " +
                            std::to_string(stride) + "-byte entry");
            }
            out.resize(chunk.size / stride);
            for (auto& entry : out) {
                read(entry);
            }
        };

        switch (chunk.tag) {
        case PHYS_TAG:
            break; // already read above
        case PHYT_TAG:
            physics.phyt = reader.read<u32>();
            break;

        // Each body layout has a name of its own, so the name decides — which
        // is what lets a v0 file's BODY and a v6 file's BDY4 land in the same
        // struct with only the fields each one carries.
        case BODY_TAG:
        case BDY2_TAG:
        case BDY3_TAG:
        case BDY4_TAG: {
            const auto layout = chunk.tag == BODY_TAG   ? PhysBodyLayout::Body
                                : chunk.tag == BDY2_TAG ? PhysBodyLayout::Body2
                                : chunk.tag == BDY3_TAG ? PhysBodyLayout::Body3
                                                        : PhysBodyLayout::Body4;
            readArray(physics.bodies, physBodyStride(layout),
                      [&](PhysicsBody& body) { parser.read(body, layout); });
            break;
        }
        case SHAP_TAG:
        case SHP2_TAG: {
            const auto layout =
                chunk.tag == SHAP_TAG ? PhysShapeLayout::Shape : PhysShapeLayout::Shape2;
            readArray(physics.shapes, physShapeStride(layout),
                      [&](PhysicsShape& shape) { parser.read(shape, layout); });
            break;
        }
        case BOXS_TAG:
            readArray(physics.boxShapes, PHYS_BOX_STRIDE,
                      [&](BoxShape& shape) { parser.read(shape); });
            break;
        case CAPS_TAG:
            readArray(physics.capsuleShapes, PHYS_CAPSULE_STRIDE,
                      [&](CapsuleShape& shape) { parser.read(shape); });
            break;
        case SPHS_TAG:
            readArray(physics.sphereShapes, PHYS_SPHERE_STRIDE,
                      [&](SphereShape& shape) { parser.read(shape); });
            break;
        case PLYT_TAG:
            parser.read(physics.polytopeShapes);
            break;

        case JOIN_TAG:
            readArray(physics.joints, PHYS_JOIN_STRIDE,
                      [&](PhysicsJoint& joint) { parser.read(joint); });
            break;
        case WELJ_TAG:
        case WLJ2_TAG:
        case WLJ3_TAG: {
            const auto layout = chunk.tag == WELJ_TAG   ? PhysWeldLayout::Weld
                                : chunk.tag == WLJ2_TAG ? PhysWeldLayout::Weld2
                                                        : PhysWeldLayout::Weld3;
            readArray(physics.weldJoints, physWeldStride(layout),
                      [&](WeldJoint& joint) { parser.read(joint, layout); });
            break;
        }
        case SPHJ_TAG:
            readArray(physics.sphericalJoints, PHYS_SPHERICAL_STRIDE,
                      [&](SphericalJoint& joint) { parser.read(joint); });
            break;
        // SHOJ never got a version-2 name — the client just started assuming
        // the longer record — so this one really does need the file version.
        case SHOJ_TAG:
        case SHJ2_TAG: {
            const auto layout =
                chunk.tag == SHJ2_TAG ? PhysShoulderLayout::Shoulder2 : shoulderLayout;
            readArray(physics.shoulderJoints, physShoulderStride(layout),
                      [&](ShoulderJoint& joint) { parser.read(joint, layout); });
            break;
        }
        case PRSJ_TAG:
        case PRS2_TAG: {
            const auto layout =
                chunk.tag == PRS2_TAG ? PhysMotorLayout::Sprung : PhysMotorLayout::Base;
            readArray(physics.prismaticJoints, physPrismaticStride(layout),
                      [&](PrismaticJoint& joint) { parser.read(joint, layout); });
            break;
        }
        case REVJ_TAG:
        case REV2_TAG: {
            const auto layout =
                chunk.tag == REV2_TAG ? PhysMotorLayout::Sprung : PhysMotorLayout::Base;
            readArray(physics.revoluteJoints, physRevoluteStride(layout),
                      [&](RevoluteJoint& joint) { parser.read(joint, layout); });
            break;
        }
        case DSTJ_TAG:
            readArray(physics.distanceJoints, PHYS_DISTANCE_STRIDE,
                      [&](DistanceJoint& joint) { parser.read(joint); });
            break;
        case PHYV_TAG:
            readArray(physics.tuning, PHYS_TUNING_STRIDE,
                      [&](PhysicsTuning& tuning) { parser.read(tuning); });
            break;

        default: {
            reportIssue("Unknown PHYS chunk: " + tagName(chunk.tag) +
                        " (size: " + std::to_string(chunk.size) + ")");
            PhysicsUnknownChunk unknown;
            std::memcpy(unknown.tag.data(), &chunk.tag, 4);
            unknown.data = reader.read<std::vector<u8>>(chunk.size);
            physics.unknownChunks.push_back(std::move(unknown));
            break;
        }
        }
    }
}

void ChunkParser::parseChunkedBase(BinaryReader& reader, BaseFile& m2file, WoWFileSystem* wfs) {
    auto chunks = collectChunks(reader);

    std::vector<ChunkEntry> nextChunks;

    for (const auto& chunk : chunks) {
        reader.setPosition(chunk.dataOffset);

        switch (chunk.tag) {
        // Deferred until after MD21: their parses size themselves from the
        // model header (skin count, materials + emitters, emitter count).
        case MD21_TAG:
        case SFID_TAG:
        case TXAC_TAG:
        case EXP2_TAG: {
            nextChunks.push_back(chunk);
            break;
        }
        case PFID_TAG: {
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            m2file.pfid_chunk.emplace();
            parser.read(m2file.pfid_chunk.value(), m2file);
            break;
        }
        case AFID_TAG: {
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            m2file.afid_chunk.emplace();
            parser.read(m2file.afid_chunk.value());
            break;
        }
        case BFID_TAG: {
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            m2file.bfid_chunk.emplace();
            parser.read(m2file.bfid_chunk.value());
            if (wfs) {
                wfs->setBoneChunk(*m2file.bfid_chunk);
            }
            break;
        }
        case EXPT_TAG: {
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            m2file.expt_chunk.emplace();
            parser.read(m2file.expt_chunk.value());
            break;
        }
        case PABC_TAG: {
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            m2file.pabc_chunk.emplace();
            parser.read(m2file.pabc_chunk.value());
            break;
        }
        case PADC_TAG: {
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            m2file.padc_chunk.emplace();
            parser.read(m2file.padc_chunk.value());
            break;
        }
        case PSBC_TAG: {
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            m2file.psbc_chunk.emplace();
            parser.read(m2file.psbc_chunk.value());
            break;
        }
        case PEDC_TAG: {
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            m2file.pedc_chunk.emplace();
            parser.read(m2file.pedc_chunk.value());
            break;
        }
        case SKID_TAG: {
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            m2file.skid_chunk.emplace();
            parser.read(m2file.skid_chunk.value());
            break;
        }
        case TXID_TAG: {
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            m2file.txid_chunk.emplace();
            parser.read(m2file.txid_chunk.value());
            break;
        }
        case LDV1_TAG: {
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            m2file.ldv1_chunk.emplace();
            parser.read(m2file.ldv1_chunk.value());
            break;
        }
        case RPID_TAG: {
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            m2file.rpid_chunk.emplace();
            parser.read(m2file.rpid_chunk.value());
            break;
        }
        case GPID_TAG: {
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            m2file.gpid_chunk.emplace();
            parser.read(m2file.gpid_chunk.value());
            break;
        }
        case WFV1_TAG:
        case WFV2_TAG:
            break; // Obsolete markers — dropped on parse
        case PGD1_TAG: {
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            m2file.pgd1_chunk.emplace();
            parser.read(m2file.pgd1_chunk.value());
            break;
        }
        case WFV3_TAG: {
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            m2file.wfv3_chunk.emplace();
            parser.read(m2file.wfv3_chunk.value());
            break;
        }
        case PFDC_TAG: {
            // A nested chunked payload: give it a reader of its own so its
            // chunk walk and its trailing padding stay inside the PFDC bounds.
            auto payload = reader.read<std::vector<u8>>(chunk.size);
            common::span_streambuf sbuf(payload);
            BinaryReader physReader(sbuf);
            m2file.pfdc_chunk.emplace();
            parsePhysics(physReader, m2file.pfdc_chunk->physics);
            break;
        }
        case EDGF_TAG: {
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            m2file.edgf_chunk.emplace();
            parser.read(m2file.edgf_chunk.value());
            break;
        }
        case NERF_TAG: {
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            m2file.nerf_chunk.emplace();
            parser.read(m2file.nerf_chunk.value());
            break;
        }
        case DETL_TAG: {
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            m2file.detl_chunk.emplace();
            parser.read(m2file.detl_chunk.value());
            break;
        }
        case DBOC_TAG: {
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            m2file.dboc_chunk.emplace();
            parser.read(m2file.dboc_chunk.value());
            break;
        }
        case AFRA_TAG: {
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            m2file.afra_chunk.emplace();
            parser.read(m2file.afra_chunk.value());
            break;
        }
        case PCOL_TAG: {
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            m2file.pcol_chunk.emplace();
            parser.read(m2file.pcol_chunk.value());
            break;
        }
        case DPIV_TAG: {
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            m2file.dpiv_chunk.emplace();
            parser.read(m2file.dpiv_chunk.value());
            break;
        }
        case TEXL_TAG: {
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            m2file.texl_chunk.emplace();
            parser.read(m2file.texl_chunk.value());
            break;
        }
        default:
            skipUnknownChunk(reader, chunk.tag, chunk.size);
            break;
        }
    }

    if (m2file.afid_chunk) {
        wfs->setAnimChunk(*m2file.afid_chunk);
    }

    for (const auto& chunk : nextChunks) {
        if (chunk.tag == MD21_TAG) {
            reader.setPosition(chunk.dataOffset);
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            parser.read(m2file.header);
        } else if (chunk.tag == SFID_TAG) {
            reader.setPosition(chunk.dataOffset);
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            m2file.sfid_chunk.emplace();
            parser.read(m2file.sfid_chunk.value(), m2file);
        } else if (chunk.tag == TXAC_TAG) {
            reader.setPosition(chunk.dataOffset);
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            m2file.txac_chunk.emplace();
            parser.read(m2file.txac_chunk.value(), m2file);
        } else if (chunk.tag == EXP2_TAG) {
            reader.setPosition(chunk.dataOffset);
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            m2file.exp2_chunk.emplace();
            parser.read(m2file.exp2_chunk.value());
        }
    }
    if (m2file.pfid_chunk) {
        wfs->setPhysicsChunk(*m2file.pfid_chunk);
    }
    if (m2file.skid_chunk) {
        wfs->setSkeletonChunk(*m2file.skid_chunk);
    }
    if (m2file.sfid_chunk) {
        wfs->setSkinChunk(*m2file.sfid_chunk);
    }
}

void ChunkParser::parseChunkedSkeleton(BinaryReader& reader, SkeletonFile& skeletonFile,
                                       WoWFileSystem* wfs) {
    auto chunks = collectChunks(reader);

    for (const auto& chunk : chunks) {
        reader.setPosition(chunk.dataOffset);

        switch (chunk.tag) {
        case AFID_TAG: {
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            skeletonFile.afid_chunk.emplace();
            parser.read(skeletonFile.afid_chunk.value());
            break;
        }
        case BFID_TAG: {
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            skeletonFile.bfid_chunk.emplace();
            parser.read(skeletonFile.bfid_chunk.value());
            break;
        }
        case SKL1_TAG:
        case SKA1_TAG:
        case SKB1_TAG:
        case SKS1_TAG:
        case SKPD_TAG:
            break; // These will be parsed in the second pass to ensure we have AFID/BFID data
                   // available if needed
        default:
            skipUnknownChunk(reader, chunk.tag, chunk.size);
            break;
        }
    }

    if (skeletonFile.afid_chunk) {
        wfs->setAnimChunk(*skeletonFile.afid_chunk);
    }
    // A character model keeps its `.bone` ids on the skeleton, not the `.m2`.
    if (skeletonFile.bfid_chunk) {
        wfs->setBoneChunk(*skeletonFile.bfid_chunk);
    }

    // SKS1 supersedes whatever sequence list the `.m2` header carried, and a
    // parent-skeleton reference runs this whole function again — so the
    // per-sequence state starts over here, or SKB1's bone tracks would index a
    // list with two models' sequences concatenated.
    wfs->clearSequenceInFile();

    for (const auto& chunk : chunks) {
        reader.setPosition(chunk.dataOffset);

        switch (chunk.tag) {
        case SKL1_TAG: {
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            skeletonFile.skl1_chunk.emplace();
            parser.read(skeletonFile.skl1_chunk.value());
            break;
        }
        case SKA1_TAG: {
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            skeletonFile.ska1_chunk.emplace();
            parser.read(skeletonFile.ska1_chunk.value());
            break;
        }
        case SKB1_TAG: {
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            skeletonFile.skb1_chunk.emplace();
            parser.read(skeletonFile.skb1_chunk.value());
            break;
        }
        case SKS1_TAG: {
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            skeletonFile.sks1_chunk.emplace();
            parser.read(skeletonFile.sks1_chunk.value());
            break;
        }
        case SKPD_TAG: {
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            skeletonFile.skpd_chunk.emplace();
            parser.read(skeletonFile.skpd_chunk.value());
            break;
        }
        case AFID_TAG:
        case BFID_TAG:
        default:
            break;
        }
    }
}

// A `.bone` is a version word and two flat chunks: BIDA's bone ids and BOMT's
// matrices, one for one. They are read into separate vectors first because
// nothing in the framing guarantees BIDA comes first, or comes at all.
bool ChunkParser::parseBoneOverrides(BinaryReader& reader, BoneOverrideSet& overrides) {
    overrides.version = reader.read<u32>();

    std::vector<u16> boneIndices;
    std::vector<Matrix44f> matrices;
    bool sawIndices = false;
    bool sawMatrices = false;

    for (const auto& chunk : collectChunks(reader)) {
        reader.setPosition(chunk.dataOffset);

        switch (chunk.tag) {
        case BIDA_TAG:
            boneIndices = reader.read<std::vector<u16>>(chunk.size / 2);
            sawIndices = true;
            break;
        case BOMT_TAG:
            matrices.resize(chunk.size / 64);
            for (auto& matrix : matrices) {
                for (auto& row : matrix.data) {
                    row = reader.readArray<f32, 4>();
                }
            }
            sawMatrices = true;
            break;
        default:
            skipUnknownChunk(reader, chunk.tag, chunk.size);
            break;
        }
    }

    if (!sawIndices || !sawMatrices) {
        reportIssue("BONE: missing " + std::string(sawIndices ? "BOMT" : "BIDA") + " chunk");
        return false;
    }
    if (boneIndices.size() != matrices.size()) {
        reportIssue("BONE: BIDA holds " + std::to_string(boneIndices.size()) +
                    " ids but BOMT holds " + std::to_string(matrices.size()) + " matrices");
        return false;
    }

    overrides.overrides.resize(boneIndices.size());
    for (size_t i = 0; i < boneIndices.size(); ++i) {
        overrides.overrides[i].boneIndex = boneIndices[i];
        overrides.overrides[i].matrix = matrices[i];
    }
    return true;
}

void ChunkParser::parseChunkedAnim(BinaryReader& reader, AnimFile& animFile, WoWFileSystem* wfs,
                                   bool isChunked) {

    if (!isChunked) {

        u32 const fileSize = reader.getRemainingBytes();
        animFile.profile.afm2_chunk.emplace();
        animFile.profile.isChunked = true;
        animFile.profile.afm2_chunk->animationData = reader.read<std::vector<u8>>(fileSize);
        return;
    }

    animFile.profile.isChunked = true;

    auto chunks = collectChunks(reader);

    for (const auto& chunk : chunks) {
        reader.setPosition(chunk.dataOffset);

        switch (chunk.tag) {
        case AFM2_TAG: {
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            animFile.profile.afm2_chunk.emplace();
            parser.read(animFile.profile.afm2_chunk.value());
            break;
        }
        case AFSA_TAG: {
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            animFile.profile.afsa_chunk.emplace();
            parser.read(animFile.profile.afsa_chunk.value());
            break;
        }
        case AFSB_TAG: {
            BinaryParseVisitor parser(reader, wfs, chunk.size);
            animFile.profile.afsb_chunk.emplace();
            parser.read(animFile.profile.afsb_chunk.value());
            break;
        }
        default:
            skipUnknownChunk(reader, chunk.tag, chunk.size);
            break;
        }
    }
}

} // namespace m2
} // namespace whiteout
