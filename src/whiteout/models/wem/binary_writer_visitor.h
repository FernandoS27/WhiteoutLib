// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include <whiteout/models/wem/structures.h>

#include "../../common/binary_writer.h"
#include "chunk_tags.h"

#include <deque>
#include <functional>
#include <vector>

namespace whiteout {
namespace models {
namespace wem {

/// Visitor that writes a WEM Model to binary index-table format.
/// Uses deferred writes with backpatching for Reference resolution.
class BinaryWriterVisitor {
public:
    explicit BinaryWriterVisitor(common::BinaryWriter& writer);

    /// Write the complete model.
    void write(const Model& model);

protected:
    void visit(const Model& model);
    void visit(const TextureRef& tex);
    void visit(const Material& mat);
    void visit(const TextureSlot& slot);
    void visit(const CompositeSection& sec);
    void visit(const Mesh& mesh);
    void visit(const Submesh& sub);

    template <typename T>
    void visit(const std::vector<T>& container);

    void visit(const std::string& str);

    void visitFresnel(const FresnelProperties& f);

    void transferDeferredWrites();

    std::deque<std::function<void()>> currentLevelWrites;
    std::deque<std::function<void()>> deferredWrites;
    std::vector<IndexEntry> indexTable;
    common::BinaryWriter& writer;
};

} // namespace wem
} // namespace models
} // namespace whiteout
