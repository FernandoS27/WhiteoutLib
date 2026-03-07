// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "structures.h"

namespace whiteout {
namespace common {
class BinaryWriter;
}

namespace m3 {

class Writer {
public:
    explicit Writer();

    /// Write an M3 model to a file on disk.
    void write(const std::string& filePath, const Model& model);

    /// Write an M3 model to a byte buffer.
    std::vector<u8> write(const Model& model);

private:
    void writeToWriter(common::BinaryWriter& writer, const Model& model);
};

} // namespace m3
} // namespace whiteout
