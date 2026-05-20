// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/m3/writer.h>
#include "../../common/binary_writer.h"
#include "../../common/streams.h"
#include "../../common/unicode_path.h"
#include "binary_writer_visitor.h"

#include <fstream>
#include <stdexcept>

namespace whiteout {
namespace m3 {

using common::BinaryWriter;

// ============================================================================
// WriterImpl - Implementation class using PImpl idiom
// ============================================================================

class Writer::Impl {
public:
    void writeToWriter(BinaryWriter& writer, const Model& model);
};

void Writer::Impl::writeToWriter(BinaryWriter& writer, const Model& model) {
    BinaryWriterVisitor visitor(writer);
    visitor.write(model);
}

// ============================================================================
// Writer Public Interface (using PImpl)
// ============================================================================

Writer::Writer() : pImpl(std::make_unique<Impl>()) {}

Writer::~Writer() = default;

void Writer::write(const std::string& filePath, const Model& model) {
    auto file = common::open_ofstream(filePath, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open M3 file for writing: " + filePath);
    }
    BinaryWriter writer(file);
    pImpl->writeToWriter(writer, model);
}

std::vector<u8> Writer::write(const Model& model) {
    // Estimate output size from header
    u32 const estimatedSize = 1 * 1024 * 1024; // Start with 2 MB
    std::vector<u8> buffer;
    buffer.reserve(estimatedSize);
    common::vector_streambuf streambuf(buffer);
    std::ostream out(&streambuf);
    BinaryWriter writer(out);
    pImpl->writeToWriter(writer, model);
    return buffer;
}

} // namespace m3
} // namespace whiteout
