// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include <cstdint>
#include <string>
#include "structures.h"

namespace whiteout {
namespace common {
class BinaryWriter;
}

namespace m2 {

using common::BinaryWriter;

class Writer {
public:
    explicit Writer();

    void write(const std::string& filePath, const FileSystem& model);

    std::vector<uint8_t> write(const BaseFile& model);
    std::vector<uint8_t> write(const SkinFile& model);

private:
    void writeBase(BinaryWriter& writer, const BaseFile& model);
    void writeSkin(BinaryWriter& writer, const SkinFile& model);

    void writeChunkedBase(BinaryWriter& writer, const BaseFile& model);
    void writeChunkedSkeleton(BinaryWriter& writer, const SkeletonFile& model);
    void writeChunkedBone(BinaryWriter& writer, const BoneFile& model);
    void writeChunkedAnim(BinaryWriter& writer, const AnimFile& model);
};

} // namespace m2
} // namespace whiteout
