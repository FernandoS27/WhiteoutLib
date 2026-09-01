// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "binary_write_visitor.h"

namespace whiteout {
namespace models {
namespace wem {

void BinaryWriteVisitor::transferLevel() {
    while (!currentLevel_.empty()) {
        deferred_.push_front(std::move(currentLevel_.back()));
        currentLevel_.pop_back();
    }
}

void BinaryWriteVisitor::writeString(const std::string& text) {
    Reference const nullRef{};
    if (text.empty()) {
        writer_.write(nullRef);
        return;
    }

    const auto refPos = writer_.getPosition();
    writer_.write(nullRef); // placeholder, backpatched below

    currentLevel_.push_back([this, refPos, &text]() {
        const auto entryIndex = static_cast<u32>(index_.size());
        const auto offset = writer_.getPosition();
        index_.emplace_back(IndexEntry{ChunkTagTraits<char>::value, offset,
                                       static_cast<u32>(text.size()),
                                       ChunkTagTraits<char>::max_version});

        writer_.setPosition(refPos);
        Reference ref{};
        ref.entries = static_cast<u32>(text.size());
        ref.index = entryIndex;
        writer_.write(ref);
        noteReference(entryIndex);
        writer_.setPosition(offset);

        // Not NUL-terminated: the `Reference` carries the length.
        writer_.writeString(text);
        writer_.AlignTo(16, 0xAA);
    });
}

} // namespace wem
} // namespace models
} // namespace whiteout
