// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "binary_read_visitor.h"

#include <algorithm>

namespace whiteout {
namespace models {
namespace wem {

/**
 * @brief Captures every index entry this build cannot account for (§11.4).
 *
 * A chunk's length is not in its index entry -- `count` is elements, and the
 * element type is exactly what an unknown tag does not tell us -- so extent comes
 * from the layout instead: a chunk runs to the next chunk offset, and the last
 * one runs to the index table. That is sound because the writer lays chunks out
 * contiguously and 16-byte aligned, and it captures the alignment fill along with
 * the data, which is what makes re-emission byte-exact.
 */
void BinaryReadVisitor::collectUnknownChunks(u32 indexOffset) {
    unknown_.clear();

    std::vector<u32> boundaries;
    boundaries.reserve(index_.size() + 1);
    for (const IndexEntry& entry : index_) {
        if (entry.offset != 0) {
            boundaries.push_back(entry.offset);
        }
    }
    boundaries.push_back(indexOffset);
    std::sort(boundaries.begin(), boundaries.end());
    boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());

    for (std::size_t slot = 1; slot < index_.size(); ++slot) {
        const IndexEntry& entry = index_[slot];
        if (entry.tag == kHoleTag || entry.offset == 0) {
            continue;
        }
        if (IsKnownChunkTag(entry.tag) && !isSkipped(entry.tag)) {
            continue;
        }

        const auto next = std::upper_bound(boundaries.begin(), boundaries.end(), entry.offset);
        const u32 end = next == boundaries.end() ? indexOffset : *next;
        if (end <= entry.offset) {
            issues_.push_back("Unknown chunk at slot " + std::to_string(slot) +
                              " has no extent; not preserved");
            continue;
        }

        UnknownChunk chunk;
        chunk.tag = entry.tag;
        chunk.version = entry.version;
        chunk.count = entry.count;
        chunk.index = static_cast<u32>(slot);
        chunk.data.resize(end - entry.offset);
        reader_.setPosition(entry.offset);
        reader_.readBytes(reinterpret_cast<char*>(chunk.data.data()),
                          static_cast<u32>(chunk.data.size()));
        unknown_.push_back(std::move(chunk));
    }
}

void BinaryReadVisitor::readString(std::string& text) {
    const Reference ref = reader_.read<Reference>();
    if (ref.entries == 0) {
        text.clear();
        return;
    }

    if (ref.index >= index_.size()) {
        issues_.push_back("String reference index out of bounds: " + std::to_string(ref.index));
        text.clear();
        return;
    }

    const auto savedPos = reader_.getPosition();
    reader_.setPosition(index_[ref.index].offset);
    text = reader_.readString(ref.entries, false);
    reader_.setPosition(savedPos);
}

} // namespace wem
} // namespace models
} // namespace whiteout
