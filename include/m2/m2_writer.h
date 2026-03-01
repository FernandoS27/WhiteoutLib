#pragma once

#include "structures.h"
#include <string>
#include <cstdint>

namespace whiteout {
namespace common {
    class BinaryWriter;
}

namespace m2 {

using common::BinaryWriter;

class M2Writer {
public:
    explicit M2Writer();
    
    void write(const std::string& filePath, const M2FileSystem& model);
    
    std::vector<uint8_t> write(const M2BaseFile& model);
    std::vector<uint8_t> write(const M2SkinFile& model);
    
private:

    void writeM2Base(BinaryWriter& writer, const M2BaseFile& model);
    void writeM2Skin(BinaryWriter& writer, const M2SkinFile& model);

    void writeChunkedM2Base(BinaryWriter& writer, const M2BaseFile& model);
    void writeChunkedM2Skeleton(BinaryWriter& writer, const M2SkeletonFile& model);
    void writeChunkedM2Bone(BinaryWriter& writer, const M2BoneFile& model);
    void writeChunkedM2Anim(BinaryWriter& writer, const M2AnimFile& model);
};

} // namespace m2
} // namespace whiteout
