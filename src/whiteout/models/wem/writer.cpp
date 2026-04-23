// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/writer.h>

#include "../../common/binary_writer.h"
#include "../../common/streams.h"
#include "../../common/unicode_path.h"

#include "binary_writer_visitor.h"

#include <fstream>
#include <stdexcept>

namespace whiteout {
namespace models {
namespace wem {

using common::BinaryWriter;

// ============================================================================
// WriterImpl
// ============================================================================

class Writer::Impl {
public:
    void write(BinaryWriter& writer, const Model& model) {
        BinaryWriterVisitor visitor(writer);
        visitor.write(model);
    }
};

// ============================================================================
// Writer Public Interface
// ============================================================================

Writer::Writer() : pImpl(std::make_unique<Impl>()) {}

Writer::~Writer() = default;

bool Writer::write(const std::string& filePath, const Model& model) {
    auto file = common::open_ofstream(filePath, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open file: " + filePath);
    }
    BinaryWriter writer(file);
    pImpl->write(writer, model);
    return true;
}

std::vector<u8> Writer::write(const Model& model) {
    std::vector<u8> buffer;
    buffer.reserve(256 * 1024); // 256KB initial reserve
    common::vector_streambuf streambuf(buffer);
    std::ostream out(&streambuf);
    BinaryWriter writer(out);
    pImpl->write(writer, model);
    buffer.shrink_to_fit();
    return buffer;
}

} // namespace wem
} // namespace models
} // namespace whiteout
