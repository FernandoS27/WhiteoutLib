
#include <whiteout/models/m2/phys_file.h>
#include "../../common/binary_reader.h"
#include "../../common/binary_writer.h"
#include "../../common/streams.h"
#include "binary_writer_visitor.h"
#include "chunk_parser.h"

#include <cstring>

namespace whiteout {
namespace m2 {

using common::BinaryReader;
using common::BinaryWriter;

std::optional<PhysicsData> parsePhysics(std::span<const u8> data,
                                        std::vector<std::string>* issues) {
    if (data.size() < 8) {
        return std::nullopt;
    }
    u32 firstTag = 0;
    std::memcpy(&firstTag, data.data(), 4);
    if (firstTag != PHYS_TAG) {
        return std::nullopt;
    }

    common::span_streambuf sbuf(data);
    BinaryReader reader(sbuf);

    ChunkParser chunkParser;
    PhysicsData physics;
    chunkParser.parsePhysics(reader, physics);
    if (issues) {
        chunkParser.drainIssues(*issues);
    }
    return physics;
}

// Chunk order follows what the client writes: version, tuning, shapes, shape
// table, bodies, joints by kind, then the JOIN table that indexes them. Every
// `.phys` in the WoW corpus is reproduced byte for byte by this ordering.
// Boxes, spherical joints and distance joints appear nowhere in that corpus, so
// their slots here are the best guess their neighbours allow.
std::vector<u8> writePhysics(const PhysicsData& physics) {
    std::vector<u8> buffer;
    common::vector_streambuf streambuf(buffer);
    std::ostream out(&streambuf);
    BinaryWriter writer(out);

    const auto chunk = [&writer](u32 tag, auto&& body) {
        writer.write(tag);
        u32 const sizePos = writer.getPosition();
        writer.write<u32>(0);
        u32 const start = writer.getPosition();
        body();
        u32 const end = writer.getPosition();
        writer.setPosition(sizePos);
        writer.write(end - start);
        writer.setPosition(end);
    };

    BinaryWriterVisitor visitor(writer);
    const auto array = [&](u32 tag, const auto& entries, auto&&... layout) {
        if (entries.empty()) {
            return;
        }
        chunk(tag, [&]() {
            for (const auto& entry : entries) {
                visitor.write(entry, layout...);
            }
        });
    };

    chunk(PHYS_TAG, [&]() { writer.write(physics.version); });
    if (physics.phyt) {
        chunk(PHYT_TAG, [&]() { writer.write(*physics.phyt); });
    }

    array(BOXS_TAG, physics.boxShapes);
    array(SPHS_TAG, physics.sphereShapes);
    array(CAPS_TAG, physics.capsuleShapes);
    if (!physics.polytopeShapes.empty()) {
        chunk(PLYT_TAG, [&]() { visitor.write(physics.polytopeShapes); });
    }

    const auto shapeLayout = physShapeLayoutFor(physics.version);
    array(shapeLayout == PhysShapeLayout::Shape ? SHAP_TAG : SHP2_TAG, physics.shapes, shapeLayout);

    const auto bodyLayout = physBodyLayoutFor(physics.version);
    constexpr u32 bodyTags[] = {BODY_TAG, BDY2_TAG, BDY3_TAG, BDY4_TAG};
    array(bodyTags[static_cast<u32>(bodyLayout)], physics.bodies, bodyLayout);

    const auto shoulderLayout = physShoulderLayoutFor(physics.version);
    array(shoulderLayout == PhysShoulderLayout::Shoulder2 ? SHJ2_TAG : SHOJ_TAG,
          physics.shoulderJoints, shoulderLayout);

    const auto weldLayout = physWeldLayoutFor(physics.version);
    constexpr u32 weldTags[] = {WELJ_TAG, WLJ2_TAG, WLJ3_TAG};
    array(weldTags[static_cast<u32>(weldLayout)], physics.weldJoints, weldLayout);

    array(SPHJ_TAG, physics.sphericalJoints);

    const auto motorLayout = physMotorLayoutFor(physics.version);
    const bool sprung = motorLayout == PhysMotorLayout::Sprung;
    array(sprung ? PRS2_TAG : PRSJ_TAG, physics.prismaticJoints, motorLayout);
    array(sprung ? REV2_TAG : REVJ_TAG, physics.revoluteJoints, motorLayout);
    array(DSTJ_TAG, physics.distanceJoints);

    array(JOIN_TAG, physics.joints);
    array(PHYV_TAG, physics.tuning);

    for (const auto& unknown : physics.unknownChunks) {
        u32 tag = 0;
        std::memcpy(&tag, unknown.tag.data(), 4);
        chunk(tag, [&]() { writer.write(unknown.data); });
    }

    buffer.shrink_to_fit();
    return buffer;
}

} // namespace m2
} // namespace whiteout
