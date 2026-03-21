// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include <whiteout/models/wem/structures.h>

#include "chunk_tags.h"
#include "../../common/binary_reader.h"

#include <vector>

namespace whiteout {
namespace models {
namespace wem {

/// Visitor that reads WEM binary data (index-table format) into the Model structure.
class BinaryParseVisitor {
public:
    explicit BinaryParseVisitor(common::BinaryReader& reader);

    /// Read the complete model from the current reader position.
    void read(Model& model);

    /// Issues encountered during parsing (non-fatal in Lenient mode).
    const std::vector<std::string>& getIssues() const { return issues; }

protected:
    void visit(Model& model);
    void visit(TextureRef& tex);
    void visit(Material& mat);
    void visit(TextureSlot& slot);
    void visit(CompositeSection& sec);
    void visit(Mesh& mesh);
    void visit(Submesh& sub);

    template <typename T>
    void visit(std::vector<T>& container);

    void visit(std::string& str);

    void visitFresnel(FresnelProperties& f);

    std::vector<IndexEntry> indexTable;
    common::BinaryReader& reader;
    std::vector<std::string> issues;
};

} // namespace wem
} // namespace models
} // namespace whiteout
