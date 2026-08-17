
#include <whiteout/models/m2/bone_file.h>
#include "../../common/binary_reader.h"
#include "../../common/binary_writer.h"
#include "../../common/streams.h"
#include "chunk_parser.h"

namespace whiteout {
namespace m2 {

using common::BinaryReader;
using common::BinaryWriter;

std::optional<BoneOverrideSet> parseBoneOverrides(std::span<const u8> data,
                                                  std::vector<std::string>* issues) {
    if (data.size() < 12) {
        return std::nullopt;
    }

    common::span_streambuf sbuf(data);
    BinaryReader reader(sbuf);

    ChunkParser chunkParser;
    BoneOverrideSet result;
    const bool ok = chunkParser.parseBoneOverrides(reader, result);
    if (issues) {
        chunkParser.drainIssues(*issues);
    }
    if (!ok) {
        return std::nullopt;
    }
    return result;
}

std::vector<u8> writeBoneOverrides(const BoneOverrideSet& overrides) {
    std::vector<u8> buffer;
    common::vector_streambuf streambuf(buffer);
    std::ostream out(&streambuf);
    BinaryWriter writer(out);

    writer.write(overrides.version);

    const auto chunk = [&writer](u32 tag, u32 size, auto&& body) {
        writer.write(tag);
        writer.write(size);
        body();
    };

    const u32 count = static_cast<u32>(overrides.overrides.size());
    chunk(BIDA_TAG, count * 2, [&] {
        for (const auto& entry : overrides.overrides) {
            writer.write(entry.boneIndex);
        }
    });
    chunk(BOMT_TAG, count * 64, [&] {
        for (const auto& entry : overrides.overrides) {
            for (const auto& row : entry.matrix.data) {
                writer.write(row);
            }
        }
    });

    buffer.shrink_to_fit();
    return buffer;
}

} // namespace m2
} // namespace whiteout
